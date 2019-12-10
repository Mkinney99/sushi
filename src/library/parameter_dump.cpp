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
 * @brief Utility functions for writing parameter names to a file.
 * @copyright 2017-2019 Modern Ancient Instruments Networked AB, dba Elk, Stockholm
 */

#include <iostream>
#include <cstdio>

#include "rapidjson/filewritestream.h"
#include "rapidjson/writer.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/document.h"
#include "library/parameter_dump.h"

namespace sushi {

int dump_engine_processor_parameters(const ext::SushiControl* engine_controller, const std::string& file_path)
{
    rapidjson::Document document;
    document.SetObject();
    rapidjson::Document::AllocatorType& allocator = document.GetAllocator();
    rapidjson::Value processor_list(rapidjson::kObjectType);
    rapidjson::Value processors(rapidjson::kArrayType);

    for(auto& track : engine_controller->get_tracks())
    {
        for(auto& processor : engine_controller->get_track_processors(track.id).second)
        {
            rapidjson::Value processor_obj(rapidjson::kObjectType);
            processor_obj.AddMember(rapidjson::Value("name", allocator).Move(), 
                                    rapidjson::Value(processor.name.c_str(), allocator).Move(), allocator);

            processor_obj.AddMember(rapidjson::Value("label", allocator).Move(), 
                                    rapidjson::Value(processor.label.c_str(), allocator).Move(), allocator);

            processor_obj.AddMember(rapidjson::Value("processor_id", allocator).Move(), 
                                    rapidjson::Value(processor.id).Move(), allocator);

            processor_obj.AddMember(rapidjson::Value("parent_track_id", allocator).Move(),
                                    rapidjson::Value(track.id).Move(), allocator);

            rapidjson::Value parameters(rapidjson::kArrayType);
            for(auto& parameter : engine_controller->get_processor_parameters(processor.id).second)
            {
                rapidjson::Value parameter_obj(rapidjson::kObjectType);
                parameter_obj.AddMember(rapidjson::Value("name", allocator).Move(),
                                        rapidjson::Value(parameter.name.c_str(), allocator).Move(), allocator);

                parameter_obj.AddMember(rapidjson::Value("label", allocator).Move(),
                                        rapidjson::Value(parameter.label.c_str(), allocator).Move(), allocator);

                parameter_obj.AddMember(rapidjson::Value("id", allocator).Move(),
                                        rapidjson::Value(parameter.id).Move(), allocator);

                parameters.PushBack(parameter_obj.Move(),allocator);
            }
            processor_obj.AddMember(rapidjson::Value("parameters", allocator).Move(), parameters.Move(), allocator);
            processors.PushBack(processor_obj.Move(), allocator);
        }
    }

    document.AddMember(rapidjson::Value("plugins", allocator),processors.Move(), allocator);

    FILE* fp = fopen(file_path.c_str(), "w");
    
    if (fp == nullptr)
    {
        return 1;
    }
    
    char writeBuffer[65536];
    rapidjson::FileWriteStream os(fp, writeBuffer, sizeof(writeBuffer));

    rapidjson::PrettyWriter<rapidjson::FileWriteStream> writer(os);
    document.Accept(writer);
    
    fclose(fp);

    return 0;
}

} // end namespace sushi