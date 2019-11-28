#include <fstream>

#include "gtest/gtest.h"

#define private public
#define protected public

#include "engine/audio_engine.h"
#include "engine/midi_dispatcher.h"
#include "engine/json_configurator.cpp"
#include "test_utils/test_utils.h"

constexpr unsigned int SAMPLE_RATE = 44000;
constexpr unsigned int ENGINE_CHANNELS = 8;

using namespace sushi;
using namespace sushi::engine;
using namespace sushi::jsonconfig;

class TestJsonConfigurator : public ::testing::Test
{
protected:
    TestJsonConfigurator() {}

    void SetUp()
    {
        _engine = new AudioEngine(SAMPLE_RATE);
        _engine->set_audio_input_channels(ENGINE_CHANNELS);
        _engine->set_audio_output_channels(ENGINE_CHANNELS);
        _midi_dispatcher = new MidiDispatcher(_engine);
        _path = test_utils::get_data_dir_path();
        _path.append("config.json");
        _module_under_test = new JsonConfigurator(_engine, _midi_dispatcher, _path);
    }

    void TearDown()
    {
        delete _module_under_test;
        delete _midi_dispatcher;
        delete _engine;
    }

    /* Helper functions */
    JsonConfigReturnStatus _make_track(const rapidjson::Value &track);

    AudioEngine* _engine;
    MidiDispatcher* _midi_dispatcher;
    JsonConfigurator* _module_under_test;
    std::string _path;
};

JsonConfigReturnStatus TestJsonConfigurator::_make_track(const rapidjson::Value &track)
{
    return _module_under_test->_make_track(track);
}

TEST_F(TestJsonConfigurator, TestInstantiation)
{
    EXPECT_TRUE(_engine);
    EXPECT_TRUE(_module_under_test);
}

TEST_F(TestJsonConfigurator, TestLoadHostConfig)
{
    auto status = _module_under_test->load_host_config();
    ASSERT_EQ(JsonConfigReturnStatus::OK, status);
    ASSERT_FLOAT_EQ(48000.0f, _engine->sample_rate());
}

TEST_F(TestJsonConfigurator, TestLoadTracks)
{
    auto status = _module_under_test->load_tracks();
    ASSERT_EQ(JsonConfigReturnStatus::OK, status);
    ASSERT_EQ(2, _engine->_audio_graph[0]->input_channels());
    ASSERT_EQ(2, _engine->_audio_graph[0]->output_channels());
    ASSERT_EQ(1, _engine->_audio_graph[1]->input_channels());
    ASSERT_EQ(1, _engine->_audio_graph[1]->output_channels());
    ASSERT_EQ(4, _engine->_audio_graph[2]->input_channels());
    ASSERT_EQ(4, _engine->_audio_graph[2]->output_channels());
    auto track_l = &_engine->_audio_graph[0]->_processors;
    auto track_r = &_engine->_audio_graph[1]->_processors;
    ASSERT_EQ(3u, track_l->size());
    ASSERT_EQ(3u, track_r->size());
    ASSERT_EQ(1, _engine->_audio_graph[1]->input_channels());

    /* TODO - Is this casting a good idea */
    ASSERT_EQ("passthrough_0_l", static_cast<InternalPlugin*>(track_l->at(0))->name());
    ASSERT_EQ("gain_0_l", static_cast<InternalPlugin*>(track_l->at(1))->name());
    ASSERT_EQ("equalizer_0_l", static_cast<InternalPlugin*>(track_l->at(2))->name());

    ASSERT_EQ("gain_0_r", static_cast<InternalPlugin*>(track_r->at(0))->name());
    ASSERT_EQ("passthrough_0_r", static_cast<InternalPlugin*>(track_r->at(1))->name());
    ASSERT_EQ("gain_1_r", static_cast<InternalPlugin*>(track_r->at(2))->name());
}

TEST_F(TestJsonConfigurator, TestLoadMidi)
{
    auto status = _module_under_test->load_tracks();
    ASSERT_EQ(JsonConfigReturnStatus::OK, status);
    _midi_dispatcher->set_midi_inputs(1);

    status = _module_under_test->load_midi();
    ASSERT_EQ(JsonConfigReturnStatus::OK, status);
    ASSERT_EQ(1u, _midi_dispatcher->_kb_routes_in.size());
    ASSERT_EQ(1u, _midi_dispatcher->_cc_routes.size());
    ASSERT_EQ(1u, _midi_dispatcher->_raw_routes_in.size());
    ASSERT_EQ(1u, _midi_dispatcher->_pc_routes.size());
}

TEST_F(TestJsonConfigurator, TestMakeChain)
{
    /* Create plugin track without processors */
    rapidjson::Document test_cfg;
    rapidjson::Value track(rapidjson::kObjectType);
    rapidjson::Value mode("mono");
    rapidjson::Value name("track_without_plugins");
    rapidjson::Value inputs(rapidjson::kArrayType);
    rapidjson::Value outputs(rapidjson::kArrayType);
    rapidjson::Value plugins(rapidjson::kArrayType);
    track.AddMember("mode", mode, test_cfg.GetAllocator());
    track.AddMember("name", name, test_cfg.GetAllocator());
    track.AddMember("inputs", inputs, test_cfg.GetAllocator());
    track.AddMember("outputs", outputs, test_cfg.GetAllocator());
    track.AddMember("plugins", plugins, test_cfg.GetAllocator());
    ASSERT_EQ(_make_track(track), JsonConfigReturnStatus::OK);

    /* Similar Plugin track but with same track id */
    track["mode"] = "stereo";
    ASSERT_EQ(_make_track(track), JsonConfigReturnStatus::INVALID_TRACK_NAME);

    /* Create valid plugin track with valid plugin */
    track["name"] = "tracks_with_internal_plugin";
    rapidjson::Value test_plugin(rapidjson::kObjectType);
    rapidjson::Value uid("sushi.testing.gain");
    rapidjson::Value path("empty_path");
    rapidjson::Value type("internal");
    rapidjson::Value plugin_name("internal_plugin");
    test_plugin.AddMember("uid", uid, test_cfg.GetAllocator());
    test_plugin.AddMember("path", path, test_cfg.GetAllocator());
    test_plugin.AddMember("type", type, test_cfg.GetAllocator());
    test_plugin.AddMember("name", plugin_name, test_cfg.GetAllocator());
    track["plugins"].PushBack(test_plugin, test_cfg.GetAllocator());
    ASSERT_EQ(_make_track(track), JsonConfigReturnStatus::OK);

    rapidjson::Value& plugin = track["plugins"][0];
    track["name"] = "track_invalid_internal";
    plugin["name"] = "invalid_internal_plugin";
    plugin["uid"] = "wrong_uid";
    plugin["type"] = "internal";
    ASSERT_EQ(_make_track(track), JsonConfigReturnStatus::INVALID_PLUGIN_PATH);

    track["name"] = "track_invalid_name";
    plugin["name"] = "internal_plugin";
    plugin["uid"] = "sushi.testing.gain";
    plugin["type"] = "internal";
    ASSERT_EQ(_make_track(track), JsonConfigReturnStatus::INVALID_PLUGIN_NAME);
}

TEST_F(TestJsonConfigurator, TestValidJsonSchema)
{
    std::ifstream config_file(_path);
    std::string config_file_contents((std::istreambuf_iterator<char>(config_file)), std::istreambuf_iterator<char>());
    rapidjson::Document test_cfg;
    test_cfg.Parse(config_file_contents.c_str());
    ASSERT_TRUE(_module_under_test->_validate_against_schema(test_cfg,JsonSection::HOST_CONFIG));
    ASSERT_TRUE(_module_under_test->_validate_against_schema(test_cfg,JsonSection::TRACKS));
    ASSERT_TRUE(_module_under_test->_validate_against_schema(test_cfg,JsonSection::MIDI));
    ASSERT_TRUE(_module_under_test->_validate_against_schema(test_cfg,JsonSection::EVENTS));
}

TEST_F(TestJsonConfigurator, TestHostConfigSchema)
{
    rapidjson::Document test_cfg;
    test_cfg.SetObject();
    /* No definition of host_config */
    ASSERT_FALSE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::HOST_CONFIG));

    /* no definition of samplerate */
    rapidjson::Value host_config(rapidjson::kObjectType);
    test_cfg.AddMember("host_config", host_config, test_cfg.GetAllocator());
    ASSERT_FALSE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::HOST_CONFIG));

    /* invalid type */
    rapidjson::Value samplerate(rapidjson::kObjectType);
    test_cfg["host_config"].AddMember("samplerate", samplerate, test_cfg.GetAllocator());
    test_cfg["host_config"]["samplerate"] = "44100";
    ASSERT_FALSE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::HOST_CONFIG));
}

TEST_F(TestJsonConfigurator, TestPluginChainSchema)
{
    rapidjson::Document test_cfg;
    test_cfg.SetObject();

    rapidjson::Value tracks(rapidjson::kArrayType);
    test_cfg.AddMember("tracks", tracks, test_cfg.GetAllocator());

    /* Plugin track without plugin list defined is not ok, empty list defined is ok */
    rapidjson::Value example_track(rapidjson::kObjectType);
    rapidjson::Value mode("mono");
    rapidjson::Value name("track_name");
    rapidjson::Value plugins(rapidjson::kArrayType);
    example_track.AddMember("mode", mode, test_cfg.GetAllocator());
    example_track.AddMember("name", name, test_cfg.GetAllocator());
    test_cfg["tracks"].PushBack(example_track, test_cfg.GetAllocator());
    ASSERT_FALSE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));
    test_cfg["tracks"][0].AddMember("plugins", plugins, test_cfg.GetAllocator());
    ASSERT_TRUE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));

    /* incorrect mode */
    test_cfg["tracks"][0]["mode"] = "invalid_mode";
    ASSERT_FALSE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));
    test_cfg["tracks"][0]["mode"] = "stereo";
    ASSERT_TRUE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));
}

TEST_F(TestJsonConfigurator, TestPluginSchema)
{
    rapidjson::Document test_cfg;
    test_cfg.SetObject();
    rapidjson::Value tracks(rapidjson::kArrayType);
    test_cfg.AddMember("tracks", tracks, test_cfg.GetAllocator());

    rapidjson::Value example_track(rapidjson::kObjectType);
    rapidjson::Value track_name("track_name");
    rapidjson::Value mode("mono");
    rapidjson::Value inputs(rapidjson::kArrayType);
    rapidjson::Value outputs(rapidjson::kArrayType);
    rapidjson::Value plugins(rapidjson::kArrayType);
    example_track.AddMember("name", track_name, test_cfg.GetAllocator());
    example_track.AddMember("mode", mode, test_cfg.GetAllocator());
    example_track.AddMember("inputs", inputs, test_cfg.GetAllocator());
    example_track.AddMember("outputs", outputs, test_cfg.GetAllocator());
    example_track.AddMember("plugins", plugins, test_cfg.GetAllocator());
    test_cfg["tracks"].PushBack(example_track, test_cfg.GetAllocator());

    rapidjson::Value example_plugin(rapidjson::kObjectType);
    rapidjson::Value plugin_name("plugin_name");
    rapidjson::Value path("plugin_path");
    rapidjson::Value uid("plugin_name");
    rapidjson::Value type("internal");
    example_plugin.AddMember("name", plugin_name, test_cfg.GetAllocator());
    example_plugin.AddMember("type", type, test_cfg.GetAllocator());
    test_cfg["tracks"][0]["plugins"].PushBack(example_plugin, test_cfg.GetAllocator());
    rapidjson::Value& plugin = test_cfg["tracks"][0]["plugins"][0];

    /* type = internal; requires uid */
    ASSERT_FALSE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));
    plugin.AddMember("uid", uid, test_cfg.GetAllocator());
    ASSERT_TRUE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));
    plugin["type"] = "vst3x";
    ASSERT_FALSE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));

    /* type = vst2x; requires path */
    plugin["type"] = "vst2x";
    ASSERT_FALSE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));
    plugin.AddMember("path", path, test_cfg.GetAllocator());
    plugin.RemoveMember("uid");
    ASSERT_TRUE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));
    plugin["type"] = "vst3x";
    ASSERT_FALSE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));

    /* type = vst3x; requires uid & path */
    rapidjson::Value vst3_uid("vst3_uid");
    plugin.AddMember("uid", vst3_uid, test_cfg.GetAllocator());
    ASSERT_TRUE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));

    /* type = LV2; requires name & uri */
    plugin["type"] = "lv2";
    ASSERT_FALSE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));
    plugin.AddMember("uri", path, test_cfg.GetAllocator());
    plugin.RemoveMember("uid");
    ASSERT_FALSE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));
    plugin["type"] = "vst3x";
    ASSERT_FALSE(_module_under_test->_validate_against_schema(test_cfg, JsonSection::TRACKS));
}

TEST_F(TestJsonConfigurator, TestMidiSchema)
{
    auto [status, midi_cfg] =_module_under_test->_parse_section(JsonSection::MIDI);
    ASSERT_EQ(JsonConfigReturnStatus::OK, status);

    rapidjson::Document mutable_cfg;
    mutable_cfg.SetObject();
    rapidjson::Value val(rapidjson::kObjectType);
    mutable_cfg.AddMember("midi", val, mutable_cfg.GetAllocator());
    mutable_cfg["midi"].CopyFrom(midi_cfg, mutable_cfg.GetAllocator());

    rapidjson::Value& track_connections = mutable_cfg["midi"]["track_connections"][0];
    ASSERT_TRUE(_module_under_test->_validate_against_schema(mutable_cfg,JsonSection::MIDI));
    track_connections["channel"] = "invalid";
    ASSERT_FALSE(_module_under_test->_validate_against_schema(mutable_cfg,JsonSection::MIDI));
    track_connections["channel"] = 16;
    ASSERT_FALSE(_module_under_test->_validate_against_schema(mutable_cfg,JsonSection::MIDI));
}

TEST_F(TestJsonConfigurator, TestLoadEventList)
{
    // Load the tracks first so we can find the processors
    ASSERT_EQ(JsonConfigReturnStatus::OK, _module_under_test->load_tracks());

    auto [status, events] = _module_under_test->load_event_list();
    ASSERT_EQ(JsonConfigReturnStatus::OK, status);
    ASSERT_EQ(4u, events.size());
}