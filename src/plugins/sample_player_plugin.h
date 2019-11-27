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
 * @brief Sampler plugin example to test event and sample handling
 * @copyright 2017-2019 Modern Ancient Instruments Networked AB, dba Elk, Stockholm
 */

#ifndef SUSHI_SAMPLER_PLUGIN_H
#define SUSHI_SAMPLER_PLUGIN_H

#include <array>

#include "library/internal_plugin.h"
#include "plugins/sample_player_voice.h"

namespace sushi {
namespace sample_player_plugin {

constexpr size_t TOTAL_POLYPHONY = 8;

static const std::string DEFAULT_NAME = "sushi.testing.sampleplayer";
static const std::string DEFAULT_LABEL = "Sample player";

namespace SampleChangeStatus {
enum SampleChange : int
{
    SUCCESS = 0,
    FAILURE
};}

class SamplePlayerPlugin : public InternalPlugin
{
public:
    SamplePlayerPlugin(HostControl host_control);

    ~SamplePlayerPlugin();

    virtual ProcessorReturnCode init(float sample_rate) override;

    void configure(float sample_rate) override;

    void set_bypassed(bool bypassed) override;

    void process_event(RtEvent event) override ;

    void process_audio(const ChunkSampleBuffer &in_buffer, ChunkSampleBuffer &out_buffer) override;

    static int non_rt_callback(void* data, EventId id)
    {
        return reinterpret_cast<SamplePlayerPlugin*>(data)->_non_rt_callback(id);
    }

private:
    BlobData load_sample_file(const std::string &file_name);
    int _non_rt_callback(EventId id);

    float*  _sample_buffer{nullptr};
    float   _dummy_sample{0.0f};
    dsp::Sample _sample;

    SampleBuffer<AUDIO_CHUNK_SIZE> _buffer{1};
    FloatParameterValue* _volume_parameter;
    FloatParameterValue* _attack_parameter;
    FloatParameterValue* _decay_parameter;
    FloatParameterValue* _sustain_parameter;
    FloatParameterValue* _release_parameter;

    std::string*         _sample_file_property{nullptr};
    EventId              _pending_event_id{0};
    BlobData             _pending_sample{0, 0};

    std::array<sample_player_voice::Voice, TOTAL_POLYPHONY> _voices;
};


}// namespace sample_player_plugin
}// namespace sushi

#endif //SUSHI_SAMPLER_PLUGIN_H
