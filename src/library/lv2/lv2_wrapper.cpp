#ifdef SUSHI_BUILD_WITH_LV2

#include <exception>
#include <math.h>
#include <iostream>

#include "lv2_wrapper.h"
#include "lv2_port.h"
#include "lv2_worker.h"
#include "lv2_state.h"

#include "logging.h"

namespace
{

static constexpr int LV2_STRING_BUFFER_SIZE = 256;

} // anonymous namespace

namespace sushi {
namespace lv2 {

MIND_GET_LOGGER_WITH_MODULE_NAME("lv2");

/** Return true iff Sushi supports the given feature. */
static bool feature_is_supported(LV2Model* model, const char* uri)
{
    if (!strcmp(uri, "http://lv2plug.in/ns/lv2core#isLive")) {
        return true;
    }

    for (const LV2_Feature*const* f = model->feature_list; *f; ++f) {
        if (!strcmp(uri, (*f)->URI)) {
            return true;
        }
    }
    return false;
}

ProcessorReturnCode Lv2Wrapper::init(float sample_rate)
{
// TODO - inherited from VST2 wrapper: sanity checks on sample_rate,
//       but these can probably better be handled on Processor::init()
    _sample_rate = sample_rate;

    auto library_handle = _loader.get_plugin_handle_from_URI(_plugin_path.c_str());
    if (library_handle == nullptr)
    {
        MIND_LOG_ERROR("Failed to load LV2 plugin - handle not recognized.");
        _cleanup();
        return ProcessorReturnCode::SHARED_LIBRARY_OPENING_ERROR;
    }

// TODO: Fix ownership of Model issue - once it is fully ported from Jalv.
    _model = _loader.getModel();
    _model->plugin = library_handle;

    _model->play_state = LV2_PAUSED;

// TODO: Move initialization to Model constructor, which throws if it fails.
    if(!_model->initialize_host_feature_list())
    {
        _cleanup();
        return ProcessorReturnCode::PLUGIN_INIT_ERROR;
    }

    if(!_check_for_required_features(_model->plugin))
    {
        _cleanup();
        return ProcessorReturnCode::PLUGIN_INIT_ERROR;
    }

    _loader.load_plugin(library_handle, _sample_rate, _model->feature_list);

    if (_model->instance == nullptr)
    {
        MIND_LOG_ERROR("Failed to load LV2 - Plugin entry point not found.");
        _cleanup();
        return ProcessorReturnCode::PLUGIN_ENTRY_POINT_NOT_FOUND;
    }

    _fetch_plugin_name_and_label();

// TODO: Re-introduce when program management is implemented for LV2.
    //_number_of_programs = _plugin_handle->numPrograms;

    _UI_IO.init(_model->plugin, _sample_rate, _model->midi_buf_size);

    _create_ports(library_handle);
    _create_controls(_model, true);
    _create_controls(_model, false);

    // Register internal parameters
    if (!_register_parameters())
    {
        MIND_LOG_ERROR("Failed to allocate LV2 feature list.");
        _cleanup();
        return ProcessorReturnCode::PARAMETER_ERROR;
    }

    auto state = lilv_state_new_from_world(_model->world, &_model->map, lilv_plugin_get_uri(library_handle));
    /* Apply loaded state to plugin instance if necessary */
    if (state)
    {
        apply_state(_model, state);
    }

    /* Activate plugin */
    lilv_instance_activate(_model->instance);

    _model->play_state = LV2_RUNNING;

    return ProcessorReturnCode::OK;
}

void Lv2Wrapper::_create_controls(LV2Model *model, bool writable)
{
    const LilvPlugin* plugin = model->plugin;
    LilvWorld* world = model->world;
    LilvNode* patch_writable = lilv_new_uri(world, LV2_PATCH__writable);
    LilvNode* patch_readable = lilv_new_uri(world, LV2_PATCH__readable);

    const LilvNode* uri_node = lilv_plugin_get_uri(plugin);
    const std::string uri_as_string = lilv_node_as_string(uri_node);

// TODO: Once Worker extension is implemented, test the eg-sampler plugin - it advertises parameters read here.
    LilvNodes* properties = lilv_world_find_nodes(
            world,
            uri_node,
            writable ? patch_writable : patch_readable,
            NULL);

    LILV_FOREACH(nodes, p, properties)
    {
        const LilvNode* property = lilv_nodes_get(properties, p);
        ControlID* record = NULL;

        if (!writable && lilv_world_ask(world,
                                        uri_node,
                                        patch_writable,
                                        property))
        {
            // Find existing writable control
            for (size_t i = 0; i < model->controls.n_controls; ++i)
            {
                if (lilv_node_equals(model->controls.controls[i]->node, property))
                {
                    record = model->controls.controls[i];
                    record->is_readable = true;
                    break;
                }
            }

            if (record)
            {
                continue;
            }
        }

        record = new_property_control(model, property);
        if (writable)
        {
            record->is_writable = true;
        }
        else
        {
            record->is_readable = true;
        }

        if (record->value_type)
        {
            add_control(&model->controls, record);
        }
        else
        {
            fprintf(stderr, "Parameter <%s> has unknown value type, ignored\n",
                    lilv_node_as_string(record->node));
            free(record);
        }
    }
    lilv_nodes_free(properties);

    lilv_node_free(patch_readable);
    lilv_node_free(patch_writable);
}

void Lv2Wrapper::_fetch_plugin_name_and_label()
{
    const LilvNode* uri_node = lilv_plugin_get_uri(_model->plugin);
    const std::string uri_as_string = lilv_node_as_string(uri_node);
    set_name(uri_as_string);

    LilvNode* label_node = lilv_plugin_get_name(_model->plugin);
    const std::string label_as_string = lilv_node_as_string(label_node);
    set_label(label_as_string);
    lilv_free(label_node); // Why do I free this but not uri_node? Remember...
}

bool Lv2Wrapper::_check_for_required_features(const LilvPlugin* plugin)
{
    /* Check that any required features are supported */
    LilvNodes* req_feats = lilv_plugin_get_required_features(plugin);
    LILV_FOREACH(nodes, f, req_feats)
    {
        const char* uri = lilv_node_as_uri(lilv_nodes_get(req_feats, f));
        if (!feature_is_supported(_model, uri))
        {
            MIND_LOG_ERROR("LV2 feature {} is not supported\n", uri);
            return false;
        }
    }
    lilv_nodes_free(req_feats);
    return true;
}

void Lv2Wrapper::_create_ports(const LilvPlugin *plugin)
{
    _max_input_channels = 0;
    _max_output_channels = 0;

    _model->num_ports = lilv_plugin_get_num_ports(plugin);

    float* default_values = (float*)calloc(lilv_plugin_get_num_ports(plugin), sizeof(float));

    lilv_plugin_get_port_ranges_float(plugin, NULL, NULL, default_values);

    for (int i = 0; i < _model->num_ports; ++i)
    {
        auto newPort = _create_port(plugin, i, default_values[i]);
        _model->ports.emplace_back(std::move(newPort));
    }

    const LilvPort* control_input = lilv_plugin_get_port_by_designation(
            plugin,
            _model->nodes.
            lv2_InputPort,
            _model->nodes.lv2_control);

    // The (optional) lv2:designation of this port is lv2:control,
    // which indicates that this is the "main" control port where the host should send events
    // it expects to configure the plugin, for example changing the MIDI program.
    // This is necessary since it is possible to have several MIDI input ports,
    // though typically it is best to have one.
    if (control_input)
    {
// TODO: test with MidiGate when UI code is activated.
// control_in in Jalv seems to only be used in UI code.
        _model->control_in = lilv_port_get_index(plugin, control_input);
    }

    free(default_values);

    // Channel setup derived from ports:
    _current_input_channels = _max_input_channels;
    _current_output_channels = _max_output_channels;
}

/**
   Create a port from data description. This is called before plugin
   and Jack instantiation. The remaining instance-specific setup
   (e.g. buffers) is done later in activate_port().
*/
std::unique_ptr<Port> Lv2Wrapper::_create_port(const LilvPlugin *plugin, int port_index, float default_value)
{
    std::unique_ptr<Port> to_return = nullptr;

    try
    {
        to_return = std::make_unique<Port>(plugin, port_index, default_value, _model);

        if(to_return->getType() == TYPE_AUDIO)
        {
            if (to_return->getFlow() == FLOW_INPUT)
                _max_input_channels++;
            else if (to_return->getFlow() == FLOW_OUTPUT)
                _max_output_channels++;
        }

        // TODO: This is always returned NULL for eg-amp output, should it be?
        LilvNode *min_size = lilv_port_get(plugin, to_return->get_lilv_port(), _model->nodes.rsz_minimumSize);

        if(min_size &&lilv_node_is_int(min_size))
        {
            int buf_size = lilv_node_as_int(min_size);
            _UI_IO.set_buffer_size(buf_size);
        }

        lilv_node_free(min_size);
    }
    catch(Port::FailedCreation& e)
    {
        _cleanup();
    }

    return to_return;
}

void Lv2Wrapper::configure(float sample_rate)
{
    _sample_rate = sample_rate;
    bool reset_enabled = enabled();

    if (reset_enabled)
    {
        set_enabled(false);
    }

    if (reset_enabled)
    {
        set_enabled(true);
    }

    return;
}

std::pair<ProcessorReturnCode, float> Lv2Wrapper::parameter_value(ObjectId parameter_id) const
{
    float value = 0.0;
    const int index = static_cast<int>(parameter_id);
    if (index < _model->num_ports)
    {
        Port* port = _model->ports[index].get();
        if (port)
        {
            value = port->control;
            return {ProcessorReturnCode::OK, value};
        }
    }
    return {ProcessorReturnCode::PARAMETER_NOT_FOUND, value};
}

std::pair<ProcessorReturnCode, float> Lv2Wrapper::parameter_value_normalised(ObjectId parameter_id) const
{
// TODO: Implement parameter normalization
    return this->parameter_value(parameter_id);
}

std::pair<ProcessorReturnCode, std::string> Lv2Wrapper::parameter_value_formatted(ObjectId parameter_id) const
{
// TODO: Populate parameter_value_formatted
    /*if (static_cast<int>(parameter_id) < _plugin_handle->numParams)
    {
        char buffer[kVstMaxParamStrLen] = "";
        _vst_dispatcher(effGetParamDisplay, 0, static_cast<VstInt32>(parameter_id), buffer, 0);
        return {ProcessorReturnCode::OK, buffer};
    }*/
    return {ProcessorReturnCode::PARAMETER_NOT_FOUND, ""};
}

bool Lv2Wrapper::supports_programs() const
{
    return _number_of_programs > 0;
}

int Lv2Wrapper::program_count() const
{
    return _number_of_programs;
}

int Lv2Wrapper::current_program() const
{
    /*if (this->supports_programs())
    {
        return _vst_dispatcher(effGetProgram, 0, 0, nullptr, 0);
    }*/
    return 0;
}

std::string Lv2Wrapper::current_program_name() const
{
    /*if (this->supports_programs())
    {
        char buffer[kVstMaxProgNameLen] = "";
        _vst_dispatcher(effGetProgramName, 0, 0, buffer, 0);
        return std::string(buffer);
    }
*/    return "";
}

std::pair<ProcessorReturnCode, std::string> Lv2Wrapper::program_name(int program) const
{
    /*if (this->supports_programs())
    {
        char buffer[kVstMaxProgNameLen] = "";
        auto success = _vst_dispatcher(effGetProgramNameIndexed, program, 0, buffer, 0);
        return {success ? ProcessorReturnCode::OK : ProcessorReturnCode::PARAMETER_NOT_FOUND, buffer};
    }*/
    return {ProcessorReturnCode::UNSUPPORTED_OPERATION, ""};
}

std::pair<ProcessorReturnCode, std::vector<std::string>> Lv2Wrapper::all_program_names() const
{
    if (!this->supports_programs())
    {
        return {ProcessorReturnCode::UNSUPPORTED_OPERATION, std::vector<std::string>()};
    }
    std::vector<std::string> programs;
// TODO: return all program names!
    /*for (int i = 0; i < _number_of_programs; ++i)
    {
        char buffer[kVstMaxProgNameLen] = "";
        _vst_dispatcher(effGetProgramNameIndexed, i, 0, buffer, 0);
        programs.push_back(buffer);
    }*/
    return {ProcessorReturnCode::OK, programs};
}

ProcessorReturnCode Lv2Wrapper::set_program(int program)
{
    if (this->supports_programs() && program < _number_of_programs)
    {
// TODO: Actually DO set program!
       /* _vst_dispatcher(effBeginSetProgram, 0, 0, nullptr, 0);
        *//* Vst2 lacks a mechanism for signaling that the program change was successful *//*
        _vst_dispatcher(effSetProgram, 0, program, nullptr, 0);
        _vst_dispatcher(effEndSetProgram, 0, 0, nullptr, 0);
        return ProcessorReturnCode::OK;*/
    }
    return ProcessorReturnCode::UNSUPPORTED_OPERATION;
}

void Lv2Wrapper::_cleanup()
{
    _model->ports.clear();

    symap_free(_model->symap);

    // Tell plugin to stop and shutdown
    set_enabled(false);

    _loader.close_plugin_instance();
}

bool Lv2Wrapper::_register_parameters()
{
    bool param_inserted_ok = true;

    for (int _pi = 0; _pi < _model->num_ports; ++_pi)
    {
        Port* currentPort = _model->ports[_pi].get();

        if (currentPort->getType() == TYPE_CONTROL)
        {
            // Here I need to get the name of the port.
            auto nameNode = lilv_port_get_name(_model->plugin, currentPort->get_lilv_port());

            std::string nameAsString = lilv_node_as_string(nameNode);

            param_inserted_ok = register_parameter(new FloatParameterDescriptor(nameAsString, // name
                    nameAsString, // label
                    currentPort->getMin(), // range min
                    currentPort->getMax(), // range max
                    nullptr), // ParameterPreProcessor
                    static_cast<ObjectId>(_pi)); // Registering the ObjectID as the index in LV2 plugin's ports list.

            if (param_inserted_ok)
            {
                MIND_LOG_DEBUG("Plugin: {}, registered param: {}", name(), nameAsString);
            }
            else
            {
                MIND_LOG_ERROR("Plugin: {}, Error while registering param: {}", name(), nameAsString);
            }

            lilv_node_free(nameNode);
        }
    }

    return param_inserted_ok;
}

void Lv2Wrapper::process_event(RtEvent event)
{
    if (event.type() == RtEventType::FLOAT_PARAMETER_CHANGE)
    {
        auto typed_event = event.parameter_change_event();
        auto id = typed_event->param_id();

        const int portIndex = static_cast<int>(id);
        assert(portIndex < _model->num_ports);

        Port* port = _model->ports[portIndex].get();

        port->control = typed_event->value();
    }
    else if (is_keyboard_event(event))
    {
        if (_incoming_event_queue.push(event) == false)
        {
            MIND_LOG_WARNING("Plugin: {}, MIDI queue Overflow!", name());
        }
    }
    else
    {
        MIND_LOG_INFO("Plugin: {}, received unhandled event", name());
    }
}

void Lv2Wrapper::process_audio(const ChunkSampleBuffer &in_buffer, ChunkSampleBuffer &out_buffer)
{
    if (_bypassed)
    {
         bypass_process(in_buffer, out_buffer);
        _flush_event_queue();
    }
    else
    {
        switch (_model->play_state)
        {
            case LV2_PAUSE_REQUESTED:
                _model->play_state = LV2_PAUSED;
                _model->paused.notify();
                break;
            case LV2_PAUSED:
                for (uint32_t p = 0; p < _model->num_ports; ++p)
                {
// TODO: Implement the below pause functionality:
//                    jack_port_t* jport = _model->ports[p].sys_port;
//                    if (jport && _model->ports[p].flow == FLOW_OUTPUT)
//                    {
//                        void* buf = jack_port_get_buffer(jport, nframes);
//                        if (_model->ports[p].type == TYPE_EVENT)
//                        {
//                            jack_midi_clear_buffer(buf);
//                        }
//                        else
//                        {
//                            memset(buf, '\0', nframes * sizeof(float));
//                        }
//                    }
                }
                return/* 0*/;
            default:
                break;
        }

        _map_audio_buffers(in_buffer, out_buffer);

        _deliver_inputs_to_plugin();

        _UI_IO.apply_ui_events(_model, AUDIO_CHUNK_SIZE);

        lilv_instance_run(_model->instance, AUDIO_CHUNK_SIZE);

        /* Process any worker replies. */
        lv2_worker_emit_responses(&_model->state_worker, _model->instance);
        lv2_worker_emit_responses(&_model->worker, _model->instance);

        /* Notify the plugin the run() cycle is finished */
        if (_model->worker.iface && _model->worker.iface->end_run)
        {
            _model->worker.iface->end_run(_model->instance->lv2_handle);
        }

// TODO: Reintroduce when implementing 'GUI'.
        /* Check if it's time to send updates to the UI */
//        _model->event_delta_t += nframes;
          bool send_ui_updates = false;
//        float update_frames   = _model->sample_rate / _model->ui_update_hz;
//        if (_model->has_ui && (_model->event_delta_t > update_frames)) {
//            send_ui_updates = true;
//            _model->event_delta_t = 0;
//        }

        _deliver_outputs_from_plugin(send_ui_updates);
    }
}

void Lv2Wrapper::_deliver_inputs_to_plugin()
{
    for (_p = 0, _i = 0, _o = 0; _p < _model->num_ports; ++_p)
    {
        _current_port = _model->ports[_p].get();

        switch(_current_port->getType())
        {
            case TYPE_CONTROL:
                lilv_instance_connect_port(_model->instance, _p, &_current_port->control);
                break;
            case TYPE_AUDIO:
                if (_current_port->getFlow() == FLOW_INPUT)
                    lilv_instance_connect_port(_model->instance, _p, _process_inputs[_i++]);
                else
                    lilv_instance_connect_port(_model->instance, _p, _process_outputs[_o++]);
                break;
            case TYPE_EVENT:
                if (_current_port->getFlow() == FLOW_INPUT)
                {
                    _current_port->resetInputBuffer();
                    _process_midi_input_for_current_port();

                }
                else if (_current_port->getFlow() == FLOW_OUTPUT)// Clear event output for plugin to write to.
                {
                    _current_port->resetOutputBuffer();
                }
                break;
// TODO: Implement also CV support.
            case TYPE_CV:
            case TYPE_UNKNOWN:
                assert(false);
                break;
            default:
                lilv_instance_connect_port(_model->instance, _p, NULL);
        }
    }
    _model->request_update = false;
}

void Lv2Wrapper::_deliver_outputs_from_plugin(bool send_ui_updates)
{
    for (_p = 0; _p < _model->num_ports; ++_p)
    {
        _current_port = _model->ports[_p].get();
        if(_current_port->getFlow() == FLOW_OUTPUT)
        {
            switch(_current_port->getType())
            {
                case TYPE_CONTROL:
                    if (lilv_port_has_property(_model->plugin,
                                               _current_port->get_lilv_port(),
                                               _model->nodes.lv2_reportsLatency))
                    {
                        if (_model->plugin_latency != _current_port->control)
                        {
                            _model->plugin_latency = _current_port->control;
// TODO: Introduce latency compensation reporting to Sushi
                        }
                    }
                    else if (send_ui_updates)
                    {
                        char buf[sizeof(ControlChange) + sizeof(float)];
                        ControlChange* ev = (ControlChange*)buf;
                        ev->index = _p;
                        ev->protocol = 0;
                        ev->size = sizeof(float);
                        *(float*)ev->body = _current_port->control;

                        // TODO Untested.
                        _UI_IO.write_ui_event(buf);
                    }
                    break;
                case TYPE_EVENT:
                    _process_midi_output_for_current_port();
                    break;
            }
        }
    }
}

void Lv2Wrapper::_process_midi_output_for_current_port()
{
    for (LV2_Evbuf_Iterator buf_i = lv2_evbuf_begin(_current_port->_evbuf);
         lv2_evbuf_is_valid(buf_i);
         buf_i = lv2_evbuf_next(buf_i))
    {
        // Get event from LV2 buffer

        lv2_evbuf_get(buf_i, &_midi_frames, &_midi_subframes, &_midi_type, &_midi_size, &_midi_body);

// TODO: This works, but WHY SHOULD IT BE NEEDED? to_midi_data_byte expects size to be 3 with an Assertion.
        _midi_size--;

        if (_midi_type == _model->urids.midi_MidiEvent)
        {
            _outgoing_midi_data = midi::to_midi_data_byte(_midi_body, _midi_size);
            _outgoing_midi_type = midi::decode_message_type(_outgoing_midi_data);

            switch (_outgoing_midi_type)
            {
                case midi::MessageType::CONTROL_CHANGE:
                {
                    _decoded_midi_cc_msg = midi::decode_control_change(_outgoing_midi_data);
                    output_event(RtEvent::make_parameter_change_event(this->id(),
                                                                      _decoded_midi_cc_msg.channel,
                                                                      _decoded_midi_cc_msg.controller,
                                                                      _decoded_midi_cc_msg.value));
                    break;
                }
                case midi::MessageType::NOTE_ON:
                {
                    _decoded_node_on_msg = midi::decode_note_on(_outgoing_midi_data);
                    output_event(RtEvent::make_note_on_event(this->id(),
                                                             0, // Sample offset 0?
                                                             _decoded_node_on_msg.channel,
                                                             _decoded_node_on_msg.note,
                                                             _decoded_node_on_msg.velocity));
                    break;
                }
                case midi::MessageType::NOTE_OFF:
                {
                    _decoded_node_off_msg = midi::decode_note_off(_outgoing_midi_data);
                    output_event(RtEvent::make_note_off_event(this->id(),
                                                              0, // Sample offset 0?
                                                              _decoded_node_off_msg.channel,
                                                              _decoded_node_off_msg.note,
                                                              _decoded_node_off_msg.velocity));
                    break;
                }
                case midi::MessageType::PITCH_BEND:
                {
                    _decoded_pitch_bend_msg = midi::decode_pitch_bend(_outgoing_midi_data);
                    output_event(RtEvent::make_pitch_bend_event(this->id(),
                                                                0, // Sample offset 0?
                                                                _decoded_pitch_bend_msg.channel,
                                                                _decoded_pitch_bend_msg.value));
                    break;
                }
                case midi::MessageType::POLY_KEY_PRESSURE:
                {
                    _decoded_poly_pressure_msg = midi::decode_poly_key_pressure(_outgoing_midi_data);
                    output_event(RtEvent::make_note_aftertouch_event(this->id(),
                                                                     0, // Sample offset 0?
                                                                     _decoded_poly_pressure_msg.channel,
                                                                     _decoded_poly_pressure_msg.note,
                                                                     _decoded_poly_pressure_msg.pressure));
                    break;
                }
                case midi::MessageType::CHANNEL_PRESSURE:
                {
                    _decoded_channel_pressure_msg = midi::decode_channel_pressure(_outgoing_midi_data);
                    output_event(RtEvent::make_aftertouch_event(this->id(),
                                                                0, // Sample offset 0?
                                                                _decoded_channel_pressure_msg.channel,
                                                                _decoded_channel_pressure_msg.pressure));
                    break;
                }
                default:
                    output_event(RtEvent::make_wrapped_midi_event(this->id(),
                                                                  0, // Sample offset 0?
                                                                  _outgoing_midi_data));
                    break;
            }
        }

        if (_model->has_ui)
        {
// TODO: Ugly - _p is changed outside this method but there's no communicating that.
            // Forward event to UI
//            _UI_IO.send_to_ui(_model, _p, _midi_type, _midi_size, _midi_body);
        }
    }
}

void Lv2Wrapper::_process_midi_input_for_current_port()
{
    _lv2_evbuf_iterator = lv2_evbuf_begin(_current_port->_evbuf);

// TODO: Re-introduce transport support.
    /* Write transport change event if applicable */
    /*
    if (xport_changed)
    {
        lv2_evbuf_write(&_lv2_evbuf_iterator, 0, 0,
                        lv2_pos->type, lv2_pos->size,
                        (const uint8_t*)LV2_ATOM_BODY(lv2_pos));
    }*/

// TODO: This will never run for now! will be used for having 'UI' support.
// with LV2Model example. Request_update is never set to true.
    if (_model->request_update)
    {
        // Plugin state has changed, request an update
        _get_ATOM = {
                {sizeof(LV2_Atom_Object_Body), _model->urids.atom_Object},
                {0,                            _model->urids.patch_Get}};

        lv2_evbuf_write(&_lv2_evbuf_iterator, 0, 0,
                        _get_ATOM.atom.type, _get_ATOM.atom.size,
                        (const uint8_t *) LV2_ATOM_BODY(&_get_ATOM));
    }

    // MIDI transfer, from incoming RT event queue into LV2 event buffers:
    while (!_incoming_event_queue.empty())
    {
        if (_incoming_event_queue.pop(_rt_event))
        {
            _convert_event_to_midi_buffer(_rt_event);

            lv2_evbuf_write(&_lv2_evbuf_iterator,
                            _rt_event.sample_offset(), // Is sample_offset the timestamp?
                            0, // Subframes
                            _model->urids.midi_MidiEvent,
                            _midi_data.size(),
                            _midi_data.data());
        }
    }
}

void Lv2Wrapper::_flush_event_queue()
{
    while (!_incoming_event_queue.empty())
    {
        _incoming_event_queue.pop(_rt_event);
    }
}

void Lv2Wrapper::_convert_event_to_midi_buffer(RtEvent& event)
{
    if (event.type() >= RtEventType::NOTE_ON && event.type() <= RtEventType::NOTE_AFTERTOUCH)
    {
        _keyboard_event_ptr = event.keyboard_event();

        switch (_keyboard_event_ptr->type())
        {
            case RtEventType::NOTE_ON:
            {
                _midi_data = midi::encode_note_on(_keyboard_event_ptr->channel(),
                                                  _keyboard_event_ptr->note(),
                                                  _keyboard_event_ptr->velocity());
                break;
            }
            case RtEventType::NOTE_OFF:
            {
                _midi_data = midi::encode_note_off(_keyboard_event_ptr->channel(),
                                                   _keyboard_event_ptr->note(),
                                                   _keyboard_event_ptr->velocity());
                break;
            }
            case RtEventType::NOTE_AFTERTOUCH:
            {
                _midi_data = midi::encode_poly_key_pressure(_keyboard_event_ptr->channel(),
                                                            _keyboard_event_ptr->note(),
                                                            _keyboard_event_ptr->velocity());
                break;
            }
        }
    }
    else if (event.type() >= RtEventType::PITCH_BEND && event.type() <= RtEventType::MODULATION)
    {
        _keyboard_common_event_ptr = event.keyboard_common_event();

        switch (_keyboard_common_event_ptr->type())
        {
            case RtEventType::AFTERTOUCH:
            {
                _midi_data = midi::encode_channel_pressure(_keyboard_common_event_ptr->channel(),
                                                           _keyboard_common_event_ptr->value());
                break;
            }
            case RtEventType::PITCH_BEND:
            {
                _midi_data = midi::encode_pitch_bend(_keyboard_common_event_ptr->channel(),
                                                     _keyboard_common_event_ptr->value());
                break;
            }
            case RtEventType::MODULATION:
            {
                _midi_data = midi::encode_control_change(_keyboard_common_event_ptr->channel(),
                                                         midi::MOD_WHEEL_CONTROLLER_NO,
                                                         _keyboard_common_event_ptr->value());
                break;
            }
        }
    }
    else if (event.type() == RtEventType::WRAPPED_MIDI_EVENT)
    {
        _wrapped_midi_event_ptr = event.wrapped_midi_event();
        _midi_data = _wrapped_midi_event_ptr->midi_data();
    }
    else
    {
        assert(false);
    }
}

void Lv2Wrapper::_map_audio_buffers(const ChunkSampleBuffer &in_buffer, ChunkSampleBuffer &out_buffer)
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

void Lv2Wrapper::_update_mono_mode(bool speaker_arr_status)
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

void Lv2Wrapper::_notify_parameter_change_rt(int parameter_index, float value)
{
    if (parameter_index > this->parameter_count())
    {
        return;
    }
    auto e = RtEvent::make_parameter_change_event(this->id(), 0, static_cast<ObjectId>(parameter_index), value);
    output_event(e);
}

void Lv2Wrapper::_notify_parameter_change(int parameter_index, float value)
{
    auto e = new ParameterChangeNotificationEvent(ParameterChangeNotificationEvent::Subtype::FLOAT_PARAMETER_CHANGE_NOT,
                                                  this->id(),
                                                  static_cast<ObjectId>(parameter_index),
                                                  value,
                                                  IMMEDIATE_PROCESS);
    _host_control.post_event(e);
}

/*VstTimeInfo* Lv2Wrapper::time_info()
{
    auto transport = _host_control.transport();
    auto ts = transport->current_time_signature();

    _time_info.samplePos          = transport->current_samples();
    _time_info.sampleRate         = _sample_rate;
    _time_info.nanoSeconds        = std::chrono::duration_cast<std::chrono::nanoseconds>(transport->current_process_time()).count();
    _time_info.ppqPos             = transport->current_beats();
    _time_info.tempo              = transport->current_tempo();
    _time_info.barStartPos        = transport->current_bar_start_beats();
    _time_info.timeSigNumerator   = ts.numerator;
    _time_info.timeSigDenominator = ts.denominator;
    _time_info.flags = SUSHI_HOST_TIME_CAPABILITIES | transport->playing()? kVstTransportPlaying : 0;
    return &_time_info;
}*/

} // namespace lv2
} // namespace sushi

#endif //SUSHI_BUILD_WITH_LV2
#ifndef SUSHI_BUILD_WITH_LV2
#include "lv2_wrapper.h"
#include "logging.h"
namespace sushi {
namespace lv2 {
MIND_GET_LOGGER;
ProcessorReturnCode Lv2Wrapper::init(float /*sample_rate*/)
{
    /* The log print needs to be in a cpp file for initialisation order reasons */
    MIND_LOG_ERROR("Sushi was not built with LV2 support!");
    return ProcessorReturnCode::ERROR;
}}}
#endif