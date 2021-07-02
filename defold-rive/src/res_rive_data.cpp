// Copyright 2021 The Defold Foundation
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include "res_rive_data.h"
#include <dmsdk/dlib/log.h>
#include <dmsdk/resource/resource.h>

// Rive includes
// #include <artboard.hpp>
#include <file.hpp>

namespace dmRive
{
    static dmResource::Result ResourceType_RiveData_Create(const dmResource::ResourceCreateParams& params)
    {
        rive::File* file          = 0;
        rive::BinaryReader reader = rive::BinaryReader((uint8_t*) params.m_Buffer, params.m_BufferSize);
        rive::ImportResult result = rive::File::import(reader, &file);

        if (result != rive::ImportResult::success)
        {
            params.m_Resource->m_Resource = 0;
            return  dmResource::RESULT_INVALID_DATA;
        }

        params.m_Resource->m_Resource     = (void*) file;
        params.m_Resource->m_ResourceSize = 0;

        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceType_RiveData_Destroy(const dmResource::ResourceDestroyParams& params)
    {
        // TODO: Destroy rive data
        void* data = params.m_Resource->m_Resource;

        return dmResource::RESULT_OK;
    }

    static dmResource::Result ResourceType_RiveData_Recreate(const dmResource::ResourceRecreateParams& params)
    {
        if (params.m_Resource->m_Resource != 0)
        {
            delete (rive::File*) params.m_Resource->m_Resource;
            params.m_Resource->m_Resource = 0;
        }

        rive::File* file          = 0;
        rive::BinaryReader reader = rive::BinaryReader((uint8_t*) params.m_Buffer, params.m_BufferSize);
        rive::ImportResult result = rive::File::import(reader, &file);

        if (result != rive::ImportResult::success)
        {
            return  dmResource::RESULT_INVALID_DATA;
        }

        params.m_Resource->m_Resource     = (void*) file;
        params.m_Resource->m_ResourceSize = 0;

        return dmResource::RESULT_OK;
    }

    static dmResource::Result RegisterResourceType_RiveData(dmResource::ResourceTypeRegisterContext& ctx)
    {
        return dmResource::RegisterType(ctx.m_Factory,
                                           ctx.m_Name,
                                           0, // context
                                           0, // preload
                                           ResourceType_RiveData_Create,
                                           0, // post create
                                           ResourceType_RiveData_Destroy,
                                           ResourceType_RiveData_Recreate);

    }
}


DM_DECLARE_RESOURCE_TYPE(ResourceTypeRiveData, "rivc", dmRive::RegisterResourceType_RiveData, 0);