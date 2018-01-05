#include <fstream>

#include "rapidjson/error/en.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/schema.h"

#include "logging.h"
#include "json_configurator.h"

namespace sushi {
namespace jsonconfig {

using namespace engine;
using namespace midi_dispatcher;

MIND_GET_LOGGER;

JsonConfigReturnStatus JsonConfigurator::load_host_config(const std::string& path_to_file)
{
    rapidjson::Document config;
    auto status = _parse_file(path_to_file, config, JsonSection::HOST_CONFIG);
    if(status != JsonConfigReturnStatus::OK)
    {
        return status;
    }

    float sample_rate = config["host_config"]["samplerate"].GetFloat();
    MIND_LOG_INFO("Setting engine sample rate to {}", sample_rate);
    _engine->set_sample_rate(sample_rate);
    return JsonConfigReturnStatus::OK;
}

JsonConfigReturnStatus JsonConfigurator::load_tracks(const std::string &path_to_file)
{
    rapidjson::Document config;
    auto status = _parse_file(path_to_file, config, JsonSection::TRACKS);
    if(status != JsonConfigReturnStatus::OK)
    {
        return status;
    }

    for (auto& track : config["tracks"].GetArray())
    {
        status = _make_track(track);
        if (status != JsonConfigReturnStatus::OK)
        {
            return status;
        }
    }
    MIND_LOG_INFO("Successfully configured engine with tracks in JSON config file \"{}\"", path_to_file);
    return JsonConfigReturnStatus::OK;
}

JsonConfigReturnStatus JsonConfigurator::load_midi(const std::string& path_to_file)
{
    rapidjson::Document config;
    auto status = _parse_file(path_to_file, config, JsonSection::MIDI);
    if(status != JsonConfigReturnStatus::OK)
    {
        return status;
    }

    const rapidjson::Value& midi = config["midi"];
    if(midi.HasMember("track_connections"))
    {
        for (const auto& con : midi["track_connections"].GetArray())
        {
            bool raw_midi = con["raw_midi"].GetBool();
            MidiDispatcherStatus res;
            if (raw_midi)
            {
                res = _midi_dispatcher->connect_raw_midi_to_track(con["port"].GetInt(),
                                                                  con["track"].GetString(),
                                                                  _get_midi_channel(con["channel"]));

            }
            else
            {
                res = _midi_dispatcher->connect_kb_to_track(con["port"].GetInt(),
                                                            con["track"].GetString(),
                                                            _get_midi_channel(con["channel"]));
            }
            if (res != MidiDispatcherStatus::OK)
            {
                if(res == MidiDispatcherStatus::INVALID_MIDI_INPUT)
                {
                    MIND_LOG_ERROR("Invalid port \"{}\" specified specified for midi "
                                           "channel connections in Json Config file.", con["port"].GetInt());
                    return JsonConfigReturnStatus::INVALID_MIDI_PORT;
                }
                MIND_LOG_ERROR("Invalid plugin track \"{}\" for midi "
                                       "track connection in Json config file.", con["track"].GetString());
                return JsonConfigReturnStatus::INVALID_TRACK_NAME;
            }
        }
    }

    if(midi.HasMember("track_out_connections"))
    {
        for (const auto& con : midi["track_out_connections"].GetArray())
        {
            auto res = _midi_dispatcher->connect_track_to_output(con["port"].GetInt(),
                                                                 con["track"].GetString(),
                                                                 _get_midi_channel(con["channel"]));
            if (res != MidiDispatcherStatus::OK)
            {
                if(res == MidiDispatcherStatus::INVALID_MIDI_OUTPUT)
                {
                    MIND_LOG_ERROR("Invalid port \"{}\" specified specified for midi "
                                           "channel connections in Json Config file.", con["port"].GetInt());
                    return JsonConfigReturnStatus::INVALID_MIDI_PORT;
                }
                MIND_LOG_ERROR("Invalid plugin track \"{}\" for midi "
                                       "track connection in Json config file.", con["track"].GetString());
                return JsonConfigReturnStatus::INVALID_TRACK_NAME;
            }
        }
    }

    if(config["midi"].HasMember("cc_mappings"))
    {
        for (const auto& cc_map : midi["cc_mappings"].GetArray())
        {
            auto res = _midi_dispatcher->connect_cc_to_parameter(cc_map["port"].GetInt(),
                                                                 cc_map["plugin_name"].GetString(),
                                                                 cc_map["parameter_name"].GetString(),
                                                                 cc_map["cc_number"].GetInt(),
                                                                 cc_map["min_range"].GetFloat(),
                                                                 cc_map["max_range"].GetFloat(),
                                                                 _get_midi_channel(cc_map["channel"]));
            if (res != MidiDispatcherStatus::OK)
            {
                if(res == MidiDispatcherStatus::INVALID_MIDI_INPUT)
                {
                    MIND_LOG_ERROR("Invalid port \"{}\" specified "
                                           "for midi cc mappings in Json Config file.", cc_map["port"].GetInt());
                    return JsonConfigReturnStatus::INVALID_MIDI_PORT;
                }
                if(res == MidiDispatcherStatus::INVALID_PROCESSOR)
                {
                    MIND_LOG_ERROR("Invalid plugin name \"{}\" specified "
                                           "for midi cc mappings in Json Config file.", cc_map["plugin_name"].GetString());
                    return JsonConfigReturnStatus::INVALID_TRACK_NAME;
                }
                MIND_LOG_ERROR("Invalid parameter name \"{}\" specified for plugin \"{}\" for midi cc mappings.",
                                                                            cc_map["parameter_name"].GetString(),
                                                                            cc_map["plugin_name"].GetString());
                return JsonConfigReturnStatus::INVALID_PARAMETER;
            }
        }
    }

    return JsonConfigReturnStatus::OK;
}

JsonConfigReturnStatus JsonConfigurator::parse_events_from_file(const std::string& path_to_file,
                                                                rapidjson::Document& config)
{
    auto status = _parse_file(path_to_file, config, JsonSection::EVENTS);
    if(status != JsonConfigReturnStatus::OK)
    {
        return status;
    }
    return JsonConfigReturnStatus::OK;
}
JsonConfigReturnStatus JsonConfigurator::_parse_file(const std::string& path_to_file,
                                                     rapidjson::Document& config,
                                                     JsonSection section)
{
    std::ifstream config_file(path_to_file);
    if(!config_file.good())
    {
        MIND_LOG_ERROR("Invalid file passed to JsonConfigurator {}", path_to_file);
        return JsonConfigReturnStatus::INVALID_FILE;
    }
    //iterate through every char in file and store in the string
    std::string config_file_contents((std::istreambuf_iterator<char>(config_file)), std::istreambuf_iterator<char>());
    config.Parse(config_file_contents.c_str());
    if(config.HasParseError())
    {
        MIND_LOG_ERROR("Error parsing JSON config file: {}",  rapidjson::GetParseError_En(config.GetParseError()));
        return JsonConfigReturnStatus::INVALID_FILE;
    }

    switch(section)
    {
        case JsonSection::MIDI:
            if(!config.HasMember("midi"))
            {
                MIND_LOG_DEBUG("Config file does not have MIDI definitions");
                return JsonConfigReturnStatus::NO_MIDI_DEFINITIONS;
            }
            break;

        case JsonSection::EVENTS:
            if(!config.HasMember("events"))
            {
                MIND_LOG_DEBUG("Config file does not have events definitions");
                return JsonConfigReturnStatus::NO_EVENTS_DEFINITIONS;
            }
            break;

        default:
            break;
    }

    if(!_validate_against_schema(config, section))
    {
        MIND_LOG_ERROR("Config file {} does not follow schema", path_to_file);
        return JsonConfigReturnStatus::INVALID_CONFIGURATION;
    }

    MIND_LOG_INFO("Successfully parsed JSON config file {}", path_to_file);
    return JsonConfigReturnStatus::OK;
}

JsonConfigReturnStatus JsonConfigurator::_make_track(const rapidjson::Value &track_def)
{
    int num_channels = 0;
    if (track_def["mode"] == "mono")
    {
        num_channels = 1;
    }
    else
    {
        num_channels = 2;
    }

    auto name = track_def["name"].GetString();
    auto status = _engine->create_track(name, num_channels);
    if(status != EngineReturnStatus::OK)
    {
        MIND_LOG_ERROR("Plugin Chain Name {} in JSON config file already exists in engine", name);
        return JsonConfigReturnStatus::INVALID_TRACK_NAME;
    }
    MIND_LOG_DEBUG("Successfully added track \"{}\" to the engine", name);

    for(const auto& con : track_def["inputs"].GetArray())
    {
        if (con.HasMember("engine_bus"))
        {
            status = _engine->connect_audio_input_bus(con["engine_bus"].GetInt(), con["track_bus"].GetInt(), name);
        }
        else
        {
            status = _engine->connect_audio_input_channel(con["engine_channel"].GetInt(), con["track_channel"].GetInt(), name);
        }
        if(status != EngineReturnStatus::OK)
        {
            MIND_LOG_ERROR("Error connection input bus to track \"{}\", error {}", name, static_cast<int>(status));
            return JsonConfigReturnStatus::INVALID_CONFIGURATION;
        }
    }

    for(const auto& con : track_def["outputs"].GetArray())
    {
        if (con.HasMember("engine_bus"))
        {
            status = _engine->connect_audio_output_bus(con["engine_bus"].GetInt(), con["track_bus"].GetInt(), name);
        }
        else
        {
            status = _engine->connect_audio_output_channel(con["engine_channel"].GetInt(), con["track_channel"].GetInt(), name);

        }
        if(status != EngineReturnStatus::OK)
        {
            MIND_LOG_ERROR("Error connection track \"{}\" to output bus, error {}", name, static_cast<int>(status));
            return JsonConfigReturnStatus::INVALID_CONFIGURATION;
        }
    }


    for(const auto& def : track_def["plugins"].GetArray())
    {
        std::string plugin_uid;
        std::string plugin_path;
        std::string plugin_name = def["name"].GetString();
        PluginType plugin_type;
        std::string type = def["type"].GetString();
        if(type == "internal")
        {
            plugin_type = PluginType::INTERNAL;
            plugin_uid = def["uid"].GetString();
        }
        else if(type == "vst2x")
        {
            plugin_type = PluginType::VST2X;
            plugin_path = def["path"].GetString();
        }
        else
        {
            plugin_uid = def["uid"].GetString();
            plugin_path = def["path"].GetString();
            plugin_type = PluginType::VST3X;
        }

        status = _engine->add_plugin_to_track(name, plugin_uid, plugin_name, plugin_path, plugin_type);
        if(status != EngineReturnStatus::OK)
        {
            if(status == EngineReturnStatus::INVALID_PLUGIN_UID)
            {
                MIND_LOG_ERROR("Invalid plugin uid {} in JSON config file", plugin_uid);
                return JsonConfigReturnStatus::INVALID_PLUGIN_PATH;
            }
            MIND_LOG_ERROR("Plugin Name {} in JSON config file already exists in engine", plugin_name);
            return JsonConfigReturnStatus::INVALID_PLUGIN_NAME;
        }
        MIND_LOG_DEBUG("Successfully added Plugin \"{}\" to"
                               " Chain \"{}\"", plugin_name, name);
    }

    MIND_LOG_DEBUG("Successfully added Track {} to the engine", name);
    return JsonConfigReturnStatus::OK;
}

int JsonConfigurator::_get_midi_channel(const rapidjson::Value& channels)
{
    if (channels.IsString())
    {
        return midi::MidiChannel::OMNI;
    }
    return channels.GetInt();
}

bool JsonConfigurator::_validate_against_schema(rapidjson::Document& config, JsonSection section)
{
    const char* schema_char_array = nullptr;
    switch(section)
    {
        case JsonSection::HOST_CONFIG:
            schema_char_array =
                #include "json_schemas/host_config_schema.json"
                                                              ;
            break;

        case JsonSection::TRACKS:
            schema_char_array =
                #include "json_schemas/tracks_schema.json"
                                                                ;
            break;

        case JsonSection::MIDI:
            schema_char_array =
                #include "json_schemas/midi_schema.json"
                                                       ;
            break;

        case JsonSection::EVENTS:
            schema_char_array =
                #include "json_schemas/events_schema.json"
                                                        ;
    }

    rapidjson::Document schema;
    schema.Parse(schema_char_array);
    rapidjson::SchemaDocument schema_document(schema);
    rapidjson::SchemaValidator schema_validator(schema_document);

    // Validate Schema
    if (!config.Accept(schema_validator))
    {
        rapidjson::Pointer invalid_config_pointer = schema_validator.GetInvalidDocumentPointer();
        rapidjson::StringBuffer string_buffer;
        invalid_config_pointer.Stringify(string_buffer);
        std::string error_node = string_buffer.GetString();
        if(error_node == "")
        {
            MIND_LOG_ERROR("Invalid Json Config File: missing definitions in the root of the document");
        }
        else
        {
            MIND_LOG_ERROR("Invalid Json Config File: Incorrect definition at {}", error_node);
        }
        return false;
    }
    return true;
}

} // namespace jsonconfig
} // namespace sushi
