/*
 * Copyright 2017-2019 Modern Ancient Instruments Networked AB, dba Elk
 *
 * SUSHI is free software: you can redistribute it and/or modify it under the terms of
 * the GNU Affero General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * SUSHI is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License along with
 * SUSHI.  If not, see http://www.gnu.org/licenses/
 */

/**
 * @Brief Wrapper for LV2 plugins - Wrapper for LV2 plugins.
 * @copyright 2017-2019 Modern Ancient Instruments Networked AB, dba Elk, Stockholm
 */

#include "lv2_wrapper.h"

#include <exception>
#include <cmath>

#include <twine/twine.h>

#include "logging.h"

#include "lv2_port.h"
#include "lv2_state.h"
#include "lv2_control.h"

#include "lv2_worker.h"

namespace
{

static constexpr int LV2_STRING_BUFFER_SIZE = 256;

} // anonymous namespace

namespace sushi {
namespace lv2 {

SUSHI_GET_LOGGER_WITH_MODULE_NAME("lv2");

LilvWorldWrapper::~LilvWorldWrapper()
{
    if (_world)
    {
        lilv_world_free(_world);
    }
}

bool LilvWorldWrapper::create_world()
{
    assert(_world == nullptr);

    _world = lilv_world_new();
    if (_world)
    {
        lilv_world_load_all(_world);
    }
    return _world;
}

LilvWorld* LilvWorldWrapper::world()
{
    return _world;
}

LV2_Wrapper::LV2_Wrapper(HostControl host_control,
                         const std::string& lv2_plugin_uri,
                         std::shared_ptr<LilvWorldWrapper> world):
                         Processor(host_control),
                         _plugin_path(lv2_plugin_uri),
                         _world(world)
{
    _max_input_channels = LV2_WRAPPER_MAX_N_CHANNELS;
    _max_output_channels = LV2_WRAPPER_MAX_N_CHANNELS;
}

ProcessorReturnCode LV2_Wrapper::init(float sample_rate)
{
    _model = std::make_unique<Model>(sample_rate, this, _world->world());

    _lv2_pos = reinterpret_cast<LV2_Atom*>(pos_buf);

    auto library_handle = _plugin_handle_from_URI(_plugin_path.c_str());

    if (library_handle == nullptr)
    {
        SUSHI_LOG_ERROR("Failed to load LV2 plugin - handle not recognized.");
        return ProcessorReturnCode::SHARED_LIBRARY_OPENING_ERROR;
    }

    auto loading_return_code = _model->load_plugin(library_handle, sample_rate);

    if (loading_return_code != ProcessorReturnCode::OK)
    {
        return loading_return_code;
    }

    // Channel setup derived from ports:
    _max_input_channels = _model->input_audio_channel_count();
    _max_output_channels = _model->output_audio_channel_count();
    _current_input_channels = _max_input_channels;
    _current_output_channels = _max_output_channels;

    _fetch_plugin_name_and_label();

    if (_register_parameters() == false) // Register internal parameters
    {
        SUSHI_LOG_ERROR("Failed to allocate LV2 feature list.");
        return ProcessorReturnCode::PARAMETER_ERROR;
    }

    _model->set_play_state(PlayState::RUNNING);

    return ProcessorReturnCode::OK;
}

void LV2_Wrapper::_fetch_plugin_name_and_label()
{
    const auto uri_node = lilv_plugin_get_uri(_model->plugin_class());
    const std::string uri_as_string = lilv_node_as_string(uri_node);
    set_name(uri_as_string);

    auto label_node = lilv_plugin_get_name(_model->plugin_class());
    const std::string label_as_string = lilv_node_as_string(label_node);
    set_label(label_as_string);
    lilv_free(label_node);
}

void LV2_Wrapper::configure(float /*sample_rate*/)
{
    SUSHI_LOG_WARNING("LV2 does not support altering the sample rate after initialization.");
}

const ParameterDescriptor* LV2_Wrapper::parameter_from_id(ObjectId id) const
{
    auto descriptor = _parameters_by_lv2_id.find(id);
    if (descriptor !=  _parameters_by_lv2_id.end())
    {
        return descriptor->second;
    }
    return nullptr;
}

std::pair<ProcessorReturnCode, float> LV2_Wrapper::parameter_value(ObjectId parameter_id) const
{
    auto parameter = parameter_from_id(parameter_id);

    if (parameter == nullptr)
    {
        return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};
    }

    // All parameters registered in the wrapper were of type FloatParameterDescriptor
    if (parameter->type() != ParameterType::FLOAT)
    {
        return {ProcessorReturnCode::PARAMETER_ERROR, 0.0f};
    }

    const int index = static_cast<int>(parameter_id);

    if (index < _model->port_count())
    {
        auto port = _model->get_port(index);

        if (port != nullptr)
        {
            float value = port->control_value();
            float min = parameter->min_domain_value();
            float max = parameter->max_domain_value();

            float value_normalized = _to_normalized(value, min, max);

            return {ProcessorReturnCode::OK, value_normalized};
        }
    }

    return {ProcessorReturnCode::PARAMETER_NOT_FOUND, 0.0f};
}

std::pair<ProcessorReturnCode, float> LV2_Wrapper::parameter_value_in_domain(ObjectId parameter_id) const
{
    float value = 0.0;
    const int index = static_cast<int>(parameter_id);

    if (index < _model->port_count())
    {
        auto port = _model->get_port(index);

        if (port != nullptr)
        {
            value = port->control_value();
            return {ProcessorReturnCode::OK, value};
        }
    }

    return {ProcessorReturnCode::PARAMETER_NOT_FOUND, value};
}

std::pair<ProcessorReturnCode, std::string> LV2_Wrapper::parameter_value_formatted(ObjectId parameter_id) const
{
    auto valueTuple = parameter_value_in_domain(parameter_id);

    if(valueTuple.first == ProcessorReturnCode::OK)
    {
        std::string parsedValue = std::to_string(valueTuple.second);
        return {ProcessorReturnCode::OK, parsedValue};
    }

    return {ProcessorReturnCode::PARAMETER_NOT_FOUND, ""};
}

bool LV2_Wrapper::supports_programs() const
{
    return _model->state()->number_of_programs() > 0;
}

int LV2_Wrapper::program_count() const
{
    return _model->state()->number_of_programs();
}

int LV2_Wrapper::current_program() const
{
    if (this->supports_programs())
    {
        return _model->state()->current_program_index();
    }

    return 0;
}

std::string LV2_Wrapper::current_program_name() const
{
   return _model->state()->current_program_name();
}

std::pair<ProcessorReturnCode, std::string> LV2_Wrapper::program_name(int program) const
{
    if (this->supports_programs())
    {
        if (program < _model->state()->number_of_programs())
        {
            std::string name = _model->state()->program_name(program);
            return {ProcessorReturnCode::OK, name};
        }
    }

    return {ProcessorReturnCode::ERROR, ""};
}

std::pair<ProcessorReturnCode, std::vector<std::string>> LV2_Wrapper::all_program_names() const
{
    if (this->supports_programs() == false)
    {
        return {ProcessorReturnCode::UNSUPPORTED_OPERATION, std::vector<std::string>()};
    }

    std::vector<std::string> programs(_model->state()->program_names().begin(),
                                      _model->state()->program_names().end());

    return {ProcessorReturnCode::OK, programs};
}

ProcessorReturnCode LV2_Wrapper::set_program(int program)
{
    if (this->supports_programs() && program < _model->state()->number_of_programs())
    {
        bool succeeded = _model->state()->apply_program(program);

        if (succeeded)
        {
            return ProcessorReturnCode::OK;
        }
        else
        {
            return ProcessorReturnCode::ERROR;
        }
    }

    return ProcessorReturnCode::UNSUPPORTED_OPERATION;
}

bool LV2_Wrapper::_register_parameters()
{
    bool param_inserted_ok = true;

    for (int _pi = 0; _pi < _model->port_count(); ++_pi)
    {
        auto currentPort = _model->get_port(_pi);

        if (currentPort->type() == PortType::TYPE_CONTROL)
        {
            // Here I need to get the name of the port.
            auto nameNode = lilv_port_get_name(_model->plugin_class(), currentPort->lilv_port());
            int portIndex = lilv_port_get_index(_model->plugin_class(), currentPort->lilv_port());

            assert(portIndex == _pi); // This should only fail is the plugin's .ttl file is incorrect.

            const std::string name_as_string = lilv_node_as_string(nameNode);
            const std::string param_unit = "";

            param_inserted_ok = register_parameter(new FloatParameterDescriptor(name_as_string, // name
                    name_as_string, // label
                    param_unit, // PARAMETER UNIT
                    currentPort->min(), // range min
                    currentPort->max(), // range max
                    nullptr), // ParameterPreProcessor
                    static_cast<ObjectId>(portIndex)); // Registering the ObjectID as the LV2 Port index.

            if (param_inserted_ok)
            {
                SUSHI_LOG_DEBUG("Plugin: {}, registered param: {}", name(), name_as_string);
            }
            else
            {
                SUSHI_LOG_ERROR("Plugin: {}, Error while registering param: {}", name(), name_as_string);
            }

            lilv_node_free(nameNode);
        }
    }

    /* Create a "backwards map" from LV2 parameter ids to parameter indices.
     * LV2 parameter ports have an integer ID, assigned in the ttl file.
     * While often it starts from 0 and goes up to n-1 parameters, there is no
     * guarantee. Very often this is not true, when in the ttl, the parameter ports,
     * are preceded by other types of ports in the list (i.e. audio/midi i/o).
     */
    for (auto param : this->all_parameters())
    {
        _parameters_by_lv2_id[param->id()] = param;
    }

    return param_inserted_ok;
}

void LV2_Wrapper::process_event(const RtEvent& event)
{
    if (event.type() == RtEventType::FLOAT_PARAMETER_CHANGE)
    {
        auto typed_event = event.parameter_change_event();
        auto parameter_id = typed_event->param_id();

        auto parameter = parameter_from_id(parameter_id);

        const int portIndex = static_cast<int>(parameter_id);
        assert(portIndex < _model->port_count());

        auto port = _model->get_port(portIndex);

        auto value = typed_event->value();

        float min = parameter->min_domain_value();
        float max = parameter->max_domain_value();

        auto value_in_domain = _to_domain(value, min, max);
        port->set_control_value(value_in_domain);
    }
    else if (is_keyboard_event(event))
    {
        if (_incoming_event_queue.push(event) == false)
        {
            SUSHI_LOG_DEBUG("Plugin: {}, MIDI queue Overflow!", name());
        }
    }
    else if(event.type() == RtEventType::SET_BYPASS)
    {
        bool bypassed = static_cast<bool>(event.processor_command_event()->value());
        _bypass_manager.set_bypass(bypassed, _model->sample_rate());
    }
}

void LV2_Wrapper::_update_transport()
{
    auto transport = _host_control.transport();

    const bool rolling = transport->playing();
    const float beats_per_minute = transport->current_tempo();
    const auto ts = transport->time_signature();
    const int beats_per_bar = ts.numerator;
    const int beat_type = ts.denominator;
    const double current_bar_beats = transport->current_bar_beats();
    const int32_t bar = transport->current_bar_start_beats() / current_bar_beats;
    const uint32_t frame = transport->current_samples() / AUDIO_CHUNK_SIZE;

    // If transport state is not as expected, then something has changed.
    _xport_changed = (rolling != _model->rolling() ||
                      frame != _model->position() ||
                      beats_per_minute != _model->bpm());

    if (_xport_changed)
    {
        // Build an LV2 position object to report change to plugin.
        lv2_atom_forge_set_buffer(&_model->forge(), pos_buf, sizeof(pos_buf));
        LV2_Atom_Forge* forge = &_model->forge();

        LV2_Atom_Forge_Frame frame_atom;
        lv2_atom_forge_object(forge, &frame_atom, 0, _model->urids().time_Position);
        lv2_atom_forge_key(forge, _model->urids().time_frame);
        lv2_atom_forge_long(forge, frame);
        lv2_atom_forge_key(forge, _model->urids().time_speed);
        lv2_atom_forge_float(forge, rolling ? 1.0 : 0.0);

        lv2_atom_forge_key(forge, _model->urids().time_barBeat);
        lv2_atom_forge_float(forge, current_bar_beats);

        lv2_atom_forge_key(forge, _model->urids().time_bar);
        lv2_atom_forge_long(forge, bar - 1);

        lv2_atom_forge_key(forge, _model->urids().time_beatUnit);
        lv2_atom_forge_int(forge, beat_type);

        lv2_atom_forge_key(forge, _model->urids().time_beatsPerBar);
        lv2_atom_forge_float(forge, beats_per_bar);

        lv2_atom_forge_key(forge, _model->urids().time_beatsPerMinute);
        lv2_atom_forge_float(forge, beats_per_minute);
    }

    // Update model transport state to expected values for next cycle.
    _model->set_position(rolling ? frame + AUDIO_CHUNK_SIZE : frame);
    _model->set_bpm(beats_per_minute);
    _model->set_rolling(rolling);
}

void LV2_Wrapper::process_audio(const ChunkSampleBuffer &in_buffer, ChunkSampleBuffer &out_buffer)
{
    if (_bypass_manager.should_process() == false)
    {
         bypass_process(in_buffer, out_buffer);
        _flush_event_queue();
    }
    else
    {
        switch (_model->play_state())
        {
            case PlayState::PAUSE_REQUESTED:
            {
                _model->set_play_state(PlayState::PAUSED);

                request_non_rt_task(&restore_state_callback);
                break;
            }
            case PlayState::PAUSED:
            {
                _flush_event_queue();
                return;
            }
            default:
                break;
        }

        _update_transport();

        _map_audio_buffers(in_buffer, out_buffer);

        _deliver_inputs_to_plugin();

        lilv_instance_run(_model->plugin_instance(), AUDIO_CHUNK_SIZE);

        /* Process any worker replies. */
        if(_model->state_worker() != nullptr)
        {
            _model->state_worker()->emit_responses(_model->plugin_instance());
        }

        _model->worker()->emit_responses(_model->plugin_instance());

        _deliver_outputs_from_plugin(false);

        if (_bypass_manager.should_ramp())
        {
            _bypass_manager.crossfade_output(in_buffer, out_buffer, _current_input_channels, _current_output_channels);
        }
    }
}

void LV2_Wrapper::_restore_state_callback(EventId)
{
    /* Note that this doesn't handle multiple requests at once.
     * Currently for the Pause functionality it is fine,
     * but if extended to support other use it may note be. */
    if(_model->state_to_set() != nullptr)
    {
        auto feature_list = _model->host_feature_list();

        lilv_state_restore(_model->state_to_set(),
                           _model->plugin_instance(),
                           set_port_value,
                           _model.get(),
                           0,
                           feature_list->data());

        _model->set_state_to_set(nullptr);

        _model->request_update();
        _model->set_play_state(PlayState::RUNNING);
    }
}

void LV2_Wrapper::_worker_callback(EventId)
{
    _model->worker()->worker_func();
}

void LV2_Wrapper::set_enabled(bool enabled)
{
    Processor::set_enabled(enabled);
    if (enabled)
    {
        lilv_instance_activate(_model->plugin_instance());
    }
    else
    {
        lilv_instance_deactivate(_model->plugin_instance());
    }
}

void LV2_Wrapper::set_bypassed(bool bypassed)
{
    assert(twine::is_current_thread_realtime() == false);
    _host_control.post_event(new SetProcessorBypassEvent(this->id(),
                                                                bypassed,
                                                                IMMEDIATE_PROCESS));
}

bool LV2_Wrapper::bypassed() const
{
    return _bypass_manager.bypassed();
}

void LV2_Wrapper::request_worker_callback(AsyncWorkCallback callback)
{
    request_non_rt_task(callback);
}

void LV2_Wrapper::_deliver_inputs_to_plugin()
{
    auto instance = _model->plugin_instance();

    for (int p = 0, i = 0, o = 0; p < _model->port_count(); ++p)
    {
        auto current_port = _model->get_port(p);

        switch(current_port->type())
        {
            case PortType::TYPE_CONTROL:
                lilv_instance_connect_port(instance, p, current_port->control_pointer());
                break;
            case PortType::TYPE_AUDIO:
                if (current_port->flow() == PortFlow::FLOW_INPUT)
                    lilv_instance_connect_port(instance, p, _process_inputs[i++]);
                else
                    lilv_instance_connect_port(instance, p, _process_outputs[o++]);
                break;
            case PortType::TYPE_EVENT:
                if (current_port->flow() == PortFlow::FLOW_INPUT)
                {
                    current_port->reset_input_buffer();
                    _process_midi_input(current_port);

                }
                else if (current_port->flow() == PortFlow::FLOW_OUTPUT) // Clear event output for plugin to write to.
                {
                    current_port->reset_output_buffer();
                }
                break;
            case PortType::TYPE_CV: // CV Support not yet implemented.
            case PortType::TYPE_UNKNOWN:
                assert(false);
                break;
            default:
                lilv_instance_connect_port(instance, p, nullptr);
        }
    }

    _model->clear_update_request();
}

void LV2_Wrapper::_deliver_outputs_from_plugin(bool /*send_ui_updates*/)
{
    for (int p = 0; p < _model->port_count(); ++p)
    {
        auto current_port = _model->get_port(p);

        if(current_port->flow() == PortFlow::FLOW_OUTPUT)
        {
            switch(current_port->type())
            {
                case PortType::TYPE_CONTROL:
                    if (lilv_port_has_property(_model->plugin_class(),
                                               current_port->lilv_port(),
                                               _model->nodes()->lv2_reportsLatency))
                    {
                        if (_model->plugin_latency() != current_port->control_value())
                        {
                            _model->set_plugin_latency(current_port->control_value());
                            // TODO: Introduce latency compensation reporting to Sushi
                        }
                    }
                    break;
                case PortType::TYPE_EVENT:
                    _process_midi_output(current_port);
                    break;
                case PortType::TYPE_UNKNOWN:
                case PortType::TYPE_AUDIO:
                case PortType::TYPE_CV:
                    break;
            }
        }
    }
}

void LV2_Wrapper::_process_midi_output(Port* port)
{
    for (auto buf_i = lv2_evbuf_begin(port->evbuf()); lv2_evbuf_is_valid(buf_i); buf_i = lv2_evbuf_next(buf_i))
    {
        uint32_t midi_frames, midi_subframes, midi_type, midi_size;
        uint8_t* midi_body;

        // Get event from LV2 buffer.
        lv2_evbuf_get(buf_i, &midi_frames, &midi_subframes, &midi_type, &midi_size, &midi_body);

        midi_size--;

        if (midi_type == _model->urids().midi_MidiEvent)
        {
            auto outgoing_midi_data = midi::to_midi_data_byte(midi_body, midi_size);
            auto outgoing_midi_type = midi::decode_message_type(outgoing_midi_data);

            switch (outgoing_midi_type)
            {
                case midi::MessageType::CONTROL_CHANGE:
                {
                    auto decoded_message = midi::decode_control_change(outgoing_midi_data);
                    output_event(RtEvent::make_parameter_change_event(this->id(),
                                                                      decoded_message.channel,
                                                                      decoded_message.controller,
                                                                      decoded_message.value));
                    break;
                }
                case midi::MessageType::NOTE_ON:
                {
                    auto decoded_message = midi::decode_note_on(outgoing_midi_data);
                    output_event(RtEvent::make_note_on_event(this->id(),
                                                             0, // Sample offset 0?
                                                             decoded_message.channel,
                                                             decoded_message.note,
                                                             decoded_message.velocity));
                    break;
                }
                case midi::MessageType::NOTE_OFF:
                {
                    auto decoded_message = midi::decode_note_off(outgoing_midi_data);
                    output_event(RtEvent::make_note_off_event(this->id(),
                                                              0, // Sample offset 0?
                                                              decoded_message.channel,
                                                              decoded_message.note,
                                                              decoded_message.velocity));
                    break;
                }
                case midi::MessageType::PITCH_BEND:
                {
                    auto decoded_message = midi::decode_pitch_bend(outgoing_midi_data);
                    output_event(RtEvent::make_pitch_bend_event(this->id(),
                                                                0, // Sample offset 0?
                                                                decoded_message.channel,
                                                                decoded_message.value));
                    break;
                }
                case midi::MessageType::POLY_KEY_PRESSURE:
                {
                    auto decoded_message = midi::decode_poly_key_pressure(outgoing_midi_data);
                    output_event(RtEvent::make_note_aftertouch_event(this->id(),
                                                                     0, // Sample offset 0?
                                                                     decoded_message.channel,
                                                                     decoded_message.note,
                                                                     decoded_message.pressure));
                    break;
                }
                case midi::MessageType::CHANNEL_PRESSURE:
                {
                    auto decoded_message = midi::decode_channel_pressure(outgoing_midi_data);
                    output_event(RtEvent::make_aftertouch_event(this->id(),
                                                                0, // Sample offset 0?
                                                                decoded_message.channel,
                                                                decoded_message.pressure));
                    break;
                }
                default:
                    output_event(RtEvent::make_wrapped_midi_event(this->id(),
                                                                  0, // Sample offset 0?
                                                                  outgoing_midi_data));
                    break;
            }
        }
    }
}

void LV2_Wrapper::_process_midi_input(Port* port)
{
    auto lv2_evbuf_iterator = lv2_evbuf_begin(port->evbuf());

    // Write transport change event if applicable:
    if (_xport_changed)
    {
        lv2_evbuf_write(&lv2_evbuf_iterator,
                        0, 0, _lv2_pos->type,
                        _lv2_pos->size,
                        (const uint8_t *) LV2_ATOM_BODY(_lv2_pos));
    }

    auto urids = _model->urids();

    if (_model->update_requested())
    {
        // Plugin state has changed, request an update
        LV2_Atom_Object atom = {
                {sizeof(LV2_Atom_Object_Body), urids.atom_Object},
                {0,urids.patch_Get}};

        lv2_evbuf_write(&lv2_evbuf_iterator, 0, 0,
                        atom.atom.type, atom.atom.size,
                        (const uint8_t *) LV2_ATOM_BODY(&atom));
    }

    // MIDI transfer, from incoming RT event queue into LV2 event buffers:
    RtEvent rt_event;
    while (_incoming_event_queue.empty() == false)
    {
        if (_incoming_event_queue.pop(rt_event))
        {
            MidiDataByte midi_data = _convert_event_to_midi_buffer(rt_event);

            lv2_evbuf_write(&lv2_evbuf_iterator,
                            rt_event.sample_offset(), // Assuming sample_offset is the timestamp
                            0, // Subframes
                            urids.midi_MidiEvent,
                            midi_data.size(),
                            midi_data.data());
        }
    }
}

void LV2_Wrapper::_flush_event_queue()
{
    RtEvent rt_event;
    while (_incoming_event_queue.empty() == false)
    {
        _incoming_event_queue.pop(rt_event);
    }
}

MidiDataByte LV2_Wrapper::_convert_event_to_midi_buffer(RtEvent& event)
{
    if (event.type() >= RtEventType::NOTE_ON && event.type() <= RtEventType::NOTE_AFTERTOUCH)
    {
        auto keyboard_event_ptr = event.keyboard_event();

        switch (keyboard_event_ptr->type())
        {
            case RtEventType::NOTE_ON:
            {
                return midi::encode_note_on(keyboard_event_ptr->channel(),
                                                  keyboard_event_ptr->note(),
                                                  keyboard_event_ptr->velocity());
            }
            case RtEventType::NOTE_OFF:
            {
                return midi::encode_note_off(keyboard_event_ptr->channel(),
                                                   keyboard_event_ptr->note(),
                                                   keyboard_event_ptr->velocity());
            }
            case RtEventType::NOTE_AFTERTOUCH:
            {
                return midi::encode_poly_key_pressure(keyboard_event_ptr->channel(),
                                                            keyboard_event_ptr->note(),
                                                            keyboard_event_ptr->velocity());
            }
            default:
                return MidiDataByte();
        }
    }
    else if (event.type() >= RtEventType::PITCH_BEND && event.type() <= RtEventType::MODULATION)
    {
        auto keyboard_common_event_ptr = event.keyboard_common_event();

        switch (keyboard_common_event_ptr->type())
        {
            case RtEventType::AFTERTOUCH:
            {
                return midi::encode_channel_pressure(keyboard_common_event_ptr->channel(),
                                                           keyboard_common_event_ptr->value());
            }
            case RtEventType::PITCH_BEND:
            {
                return midi::encode_pitch_bend(keyboard_common_event_ptr->channel(),
                                                     keyboard_common_event_ptr->value());
            }
            case RtEventType::MODULATION:
            {
                return midi::encode_control_change(keyboard_common_event_ptr->channel(),
                                                         midi::MOD_WHEEL_CONTROLLER_NO,
                                                         keyboard_common_event_ptr->value());
            }
            default:
                return MidiDataByte();
        }
    }
    else if (event.type() == RtEventType::WRAPPED_MIDI_EVENT)
    {
        auto wrapped_midi_event_ptr = event.wrapped_midi_event();
        return wrapped_midi_event_ptr->midi_data();
    }

    assert(false); // All cases should have been catered for.
    return MidiDataByte();
}

void LV2_Wrapper::_map_audio_buffers(const ChunkSampleBuffer &in_buffer, ChunkSampleBuffer &out_buffer)
{
    int i;

    if (_double_mono_input)
    {
        _process_inputs[0] = const_cast<float*>(in_buffer.channel(0));
        _process_inputs[1] = const_cast<float*>(in_buffer.channel(0));
    }
    else
    {
        for (i = 0; i < _current_input_channels; ++i)
        {
            _process_inputs[i] = const_cast<float*>(in_buffer.channel(i));
        }

        for (; i <= _max_input_channels; ++i)
        {
            _process_inputs[i] = (_dummy_input.channel(0));
        }
    }

    for (i = 0; i < _current_output_channels; i++)
    {
        _process_outputs[i] = out_buffer.channel(i);
    }

    for (; i <= _max_output_channels; ++i)
    {
        _process_outputs[i] = _dummy_output.channel(0);
    }
}

void LV2_Wrapper::_update_mono_mode(bool speaker_arr_status)
{
    _double_mono_input = false;

    if (speaker_arr_status)
    {
        return;
    }

    if (_current_input_channels == 1 && _max_input_channels == 2)
    {
        _double_mono_input = true;
    }
}

void LV2_Wrapper::_pause_audio_processing()
{
    _previous_play_state = _model->play_state();

    if(_previous_play_state != PlayState::PAUSED)
    {
        _model->set_play_state(PlayState::PAUSED);
    }
}

void LV2_Wrapper::_resume_audio_processing()
{
    _model->set_play_state(_previous_play_state);
}

const LilvPlugin* LV2_Wrapper::_plugin_handle_from_URI(const std::string& plugin_URI_string)
{
    if (plugin_URI_string.empty())
    {
        SUSHI_LOG_ERROR("Empty library path");
        return nullptr; // Calling dlopen with an empty string returns a handle to the calling
        // program, which can cause an infinite loop.
    }

    auto plugins = lilv_world_get_all_plugins(_model->lilv_world());
    auto plugin_uri = lilv_new_uri(_model->lilv_world(), plugin_URI_string.c_str());

    if (plugin_uri == nullptr)
    {
        SUSHI_LOG_ERROR("Missing plugin URI, try lv2ls to list plugins.");
        return nullptr;
    }

    /* Find plugin */
    SUSHI_LOG_INFO("Plugin: {}", lilv_node_as_string(plugin_uri));
    const auto plugin  = lilv_plugins_get_by_uri(plugins, plugin_uri);
    lilv_node_free(plugin_uri);

    if (plugin == nullptr)
    {
        SUSHI_LOG_ERROR("Failed to find LV2 plugin.");
        return nullptr;
    }

    return plugin;
}

} // namespace lv2
} // namespace sushi
