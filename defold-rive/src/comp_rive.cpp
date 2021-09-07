// Copyright 2020 The Defold Foundation
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

#include <rive/animation/linear_animation_instance.hpp>
#include <rive/animation/state_machine_instance.hpp>
#include <rive/animation/state_machine_input.hpp>
#include <rive/animation/state_machine_input_instance.hpp>
#include <rive/animation/state_machine_trigger.hpp>
#include <rive/animation/state_machine_bool.hpp>
#include <rive/animation/state_machine_number.hpp>
#include <rive/animation/loop.hpp>
#include <rive/bones/bone.hpp>
#include <rive/file.hpp>
#include <rive/renderer.hpp>

#include <riverender/rive_render_api.h>

#include "comp_rive.h"
#include "res_rive_data.h"
#include "res_rive_scene.h"
#include "res_rive_model.h"

#include <common/bones.h>


#include <string.h> // memset

#include <dmsdk/gameobject/gameobject.h>
#include <dmsdk/gameobject/component.h>
#include <dmsdk/resource/resource.h>
#include <dmsdk/dlib/math.h>

#include <dmsdk/gamesys/resources/res_skeleton.h>
#include <dmsdk/gamesys/resources/res_rig_scene.h>
#include <dmsdk/gamesys/resources/res_meshset.h>
#include <dmsdk/gamesys/resources/res_animationset.h>
#include <dmsdk/gamesys/resources/res_textureset.h>

#include <dmsdk/dlib/log.h>
#include <dmsdk/gamesys/property.h>
#include <dmsdk/dlib/object_pool.h>

#include <gameobject/gameobject_ddf.h> // for creating bones where the rive scene bones are
#include <dmsdk/graphics/graphics.h>
#include <dmsdk/render/render.h>
#include <gameobject/gameobject_ddf.h>

namespace rive
{
    // JG: Hmmm should we do it like this? Rive needs these two functions that
    //     are externally linked, but our API is built around a context structure.
    static HContext g_Ctx = 0;
    RenderPath* makeRenderPath()
    {
        return createRenderPath(g_Ctx);
    }

    RenderPaint* makeRenderPaint()
    {
        return createRenderPaint(g_Ctx);
    }
}

namespace dmRive
{
    using namespace dmVMath;

    static const dmhash_t PROP_ANIMATION     = dmHashString64("animation");
    static const dmhash_t PROP_CURSOR        = dmHashString64("cursor");
    static const dmhash_t PROP_PLAYBACK_RATE = dmHashString64("playback_rate");
    static const dmhash_t PROP_MATERIAL      = dmHashString64("material");
    static const dmhash_t MATERIAL_EXT_HASH  = dmHashString64("materialc");
    static const dmhash_t UNIFORM_COLOR      = dmHashString64("color");
    static const dmhash_t UNIFORM_COVER      = dmHashString64("cover");

    static void ResourceReloadedCallback(const dmResource::ResourceReloadedParams& params);
    static void DestroyComponent(struct RiveWorld* world, uint32_t index);
    static void CompRiveAnimationReset(RiveComponent* component);
    static bool PlayAnimation(RiveComponent* component, dmRive::RiveSceneData* data, dmhash_t anim_id,
                    dmGameObject::Playback playback_mode, float offset, float playback_rate);
    static bool PlayStateMachine(RiveComponent* component, dmRive::RiveSceneData* data, dmhash_t anim_id, float playback_rate);
    static bool CreateBones(struct RiveWorld* world, RiveComponent* component, dmRive::RiveSceneData* data);
    static void DeleteBones(RiveComponent* component);
    static void UpdateBones(RiveComponent* component);

    static rive::HBuffer RiveRequestBufferCallback(rive::HBuffer buffer, rive::BufferType type, void* data, unsigned int dataSize, void* userData);
    static void          RiveDestroyBufferCallback(rive::HBuffer buffer, void* userData);

    struct RiveVertex
    {
        float x;
        float y;
    };

    struct RiveBuffer
    {
        void*        m_Data;
        unsigned int m_Size;
    };

    // JG: Do we need this or can we loop through events in render batch instead?
    struct RiveDrawEntry
    {
        rive::DrawBuffers  m_Buffers;
        rive::HRenderPaint m_Paint;
        Matrix4            m_WorldTransform;
    };

    // For the entire app's life cycle
    struct CompRiveContext
    {
        CompRiveContext()
        {
            memset(this, 0, sizeof(*this));
        }
        rive::HRenderer          m_RiveRenderer;
        rive::HContext           m_RiveContext;
        dmResource::HFactory     m_Factory;
        dmRender::HRenderContext m_RenderContext;
        dmGraphics::HContext     m_GraphicsContext;
        uint32_t                 m_MaxInstanceCount;
    };

    // One per collection
    struct RiveWorld
    {
        CompRiveContext*                    m_Ctx; // JG: Is this a bad idea?
        dmObjectPool<RiveComponent*>        m_Components;
        dmArray<dmRender::RenderObject>     m_RenderObjects;
        dmGraphics::HVertexDeclaration      m_VertexDeclaration;
        dmGraphics::HVertexBuffer           m_VertexBuffer;
        dmArray<RiveVertex>                 m_VertexBufferData;
        dmGraphics::HIndexBuffer            m_IndexBuffer;
        dmArray<int>                        m_IndexBufferData;
        dmArray<RiveDrawEntry>              m_DrawEntries;
    };

    static inline void Mat4ToMat2D(const Matrix4 m4, rive::Mat2D& m2)
    {
        m2[0] = m4[0][0];
        m2[1] = m4[0][1];

        m2[2] = m4[1][0];
        m2[3] = m4[1][1];

        m2[4] = m4[3][0];
        m2[5] = m4[3][1];
    }

    static inline void Mat2DToMat4(const rive::Mat2D m2, Matrix4& m4)
    {
        m4[0][0] = m2[0];
        m4[0][1] = m2[1];
        m4[0][2] = 0.0;
        m4[0][3] = 0.0;

        m4[1][0] = m2[2];
        m4[1][1] = m2[3];
        m4[1][2] = 0.0;
        m4[1][3] = 0.0;

        m4[2][0] = 0.0;
        m4[2][1] = 0.0;
        m4[2][2] = 1.0;
        m4[2][3] = 0.0;

        m4[3][0] = m2[4];
        m4[3][1] = m2[5];
        m4[3][2] = 0.0;
        m4[3][3] = 1.0;
    }

    dmGameObject::CreateResult CompRiveNewWorld(const dmGameObject::ComponentNewWorldParams& params)
    {
        CompRiveContext* context = (CompRiveContext*)params.m_Context;
        RiveWorld* world         = new RiveWorld();

        world->m_Ctx = context;
        world->m_Components.SetCapacity(context->m_MaxInstanceCount);
        world->m_RenderObjects.SetCapacity(context->m_MaxInstanceCount);
        world->m_DrawEntries.SetCapacity(context->m_MaxInstanceCount);

        dmGraphics::VertexElement ve[] =
        {
            {"position", 0, 2, dmGraphics::TYPE_FLOAT, false}
        };

        world->m_VertexDeclaration = dmGraphics::NewVertexDeclaration(context->m_GraphicsContext, ve, sizeof(ve) / sizeof(dmGraphics::VertexElement));
        world->m_VertexBuffer      = dmGraphics::NewVertexBuffer(context->m_GraphicsContext, 0, 0x0, dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);
        world->m_IndexBuffer       = dmGraphics::NewIndexBuffer(context->m_GraphicsContext, 0, 0x0, dmGraphics::BUFFER_USAGE_DYNAMIC_DRAW);

        // TODO: Make this count configurable and/or grow accordingly
        world->m_VertexBufferData.SetCapacity(context->m_MaxInstanceCount * 512);
        world->m_IndexBufferData.SetCapacity(context->m_MaxInstanceCount * 512);

        *params.m_World = world;

        dmResource::RegisterResourceReloadedCallback(context->m_Factory, ResourceReloadedCallback, world);

        return dmGameObject::CREATE_RESULT_OK;
    }

    dmGameObject::CreateResult CompRiveDeleteWorld(const dmGameObject::ComponentDeleteWorldParams& params)
    {
        RiveWorld* world = (RiveWorld*)params.m_World;
        dmGraphics::DeleteVertexDeclaration(world->m_VertexDeclaration);
        dmGraphics::DeleteVertexBuffer(world->m_VertexBuffer);
        dmGraphics::DeleteIndexBuffer(world->m_IndexBuffer);

        dmResource::UnregisterResourceReloadedCallback(((CompRiveContext*)params.m_Context)->m_Factory, ResourceReloadedCallback, world);

        delete world;
        return dmGameObject::CREATE_RESULT_OK;
    }

    static inline dmRender::HMaterial GetMaterial(const RiveComponent* component, const RiveModelResource* resource) {
        return component->m_Material ? component->m_Material : resource->m_Material;
    }

    static void ReHash(RiveComponent* component)
    {
        // material, texture set, blend mode and render constants
        HashState32 state;
        bool reverse = false;
        RiveModelResource* resource = component->m_Resource;
        dmRiveDDF::RiveModelDesc* ddf = resource->m_DDF;
        dmRender::HMaterial material = GetMaterial(component, resource);
        dmHashInit32(&state, reverse);
        dmHashUpdateBuffer32(&state, &material, sizeof(material));
        dmHashUpdateBuffer32(&state, &ddf->m_BlendMode, sizeof(ddf->m_BlendMode));
        if (component->m_RenderConstants)
            dmGameSystem::HashRenderConstants(component->m_RenderConstants, &state);
        component->m_MixedHash = dmHashFinal32(&state);
        component->m_ReHash = 0;
    }

    static inline RiveComponent* GetComponentFromIndex(RiveWorld* world, int index)
    {
        return world->m_Components.Get(index);
    }

    void* CompRiveGetComponent(const dmGameObject::ComponentGetParams& params)
    {
        RiveWorld* world = (RiveWorld*)params.m_World;
        uint32_t index = (uint32_t)*params.m_UserData;
        return GetComponentFromIndex(world, index);
    }

    dmGameObject::CreateResult CompRiveCreate(const dmGameObject::ComponentCreateParams& params)
    {
        RiveWorld* world = (RiveWorld*)params.m_World;

        if (world->m_Components.Full())
        {
            dmLogError("Rive instance could not be created since the buffer is full (%d).", world->m_Components.Capacity());
            return dmGameObject::CREATE_RESULT_UNKNOWN_ERROR;
        }

        RiveComponent* component = new RiveComponent;
        memset(component, 0, sizeof(RiveComponent));

        uint32_t index = world->m_Components.Alloc();
        world->m_Components.Set(index, component);
        component->m_Instance = params.m_Instance;
        component->m_Transform = dmTransform::Transform(Vector3(params.m_Position), params.m_Rotation, 1.0f);
        component->m_Resource = (RiveModelResource*)params.m_Resource;

        CompRiveAnimationReset(component);
        dmMessage::ResetURL(&component->m_Listener);

        component->m_ComponentIndex = params.m_ComponentIndex;
        component->m_Enabled = 1;
        component->m_World = Matrix4::identity();
        component->m_DoRender = 0;
        component->m_RenderConstants = 0;

        dmRive::RiveSceneData* data = (dmRive::RiveSceneData*) component->m_Resource->m_Scene->m_Scene;

        CreateBones(world, component, data);

        dmhash_t empty_id = dmHashString64("");
        dmhash_t anim_id = dmHashString64(component->m_Resource->m_DDF->m_DefaultAnimation);
        dmhash_t state_machine_id = dmHashString64(component->m_Resource->m_DDF->m_DefaultStateMachine);

        if (empty_id != state_machine_id)
        {
            if (!PlayStateMachine(component, data, state_machine_id, 1.0f))
            {
                dmLogError("Couldn't play state machine named '%s'", dmHashReverseSafe64(state_machine_id));
            }
        }
        else if (empty_id != anim_id)
        {
            if (!PlayAnimation(component, data, anim_id, dmGameObject::PLAYBACK_LOOP_FORWARD, 0.0f, 1.0f))
            {
                dmLogError("Couldn't play animation named '%s'", dmHashReverseSafe64(anim_id));
            }
        }

        component->m_ReHash = 1;

        *params.m_UserData = (uintptr_t)index;
        return dmGameObject::CREATE_RESULT_OK;
    }

    static void DestroyComponent(RiveWorld* world, uint32_t index)
    {
        RiveComponent* component = GetComponentFromIndex(world, index);
        dmGameObject::DeleteBones(component->m_Instance);

        if (component->m_RenderConstants)
            dmGameSystem::DestroyRenderConstants(component->m_RenderConstants);

        if (component->m_AnimationInstance)
            delete component->m_AnimationInstance;

        delete component;
        world->m_Components.Free(index, true);
    }

    dmGameObject::CreateResult CompRiveDestroy(const dmGameObject::ComponentDestroyParams& params)
    {
        CompRiveContext* ctx = (CompRiveContext*)params.m_Context;
        RiveWorld* world = (RiveWorld*)params.m_World;
        uint32_t index = *params.m_UserData;
        RiveComponent* component = GetComponentFromIndex(world, index);
        if (component->m_Material) {
            dmResource::Release(ctx->m_Factory, (void*)component->m_Material);
        }
        DestroyComponent(world, index);
        return dmGameObject::CREATE_RESULT_OK;
    }

    static void SetBlendMode(dmRender::RenderObject& ro, dmRiveDDF::RiveModelDesc::BlendMode blend_mode)
    {
        switch (blend_mode)
        {
            case dmRiveDDF::RiveModelDesc::BLEND_MODE_ALPHA:
                ro.m_SourceBlendFactor = dmGraphics::BLEND_FACTOR_ONE;
                ro.m_DestinationBlendFactor = dmGraphics::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;

            case dmRiveDDF::RiveModelDesc::BLEND_MODE_ADD:
                ro.m_SourceBlendFactor = dmGraphics::BLEND_FACTOR_ONE;
                ro.m_DestinationBlendFactor = dmGraphics::BLEND_FACTOR_ONE;
            break;

            case dmRiveDDF::RiveModelDesc::BLEND_MODE_MULT:
                ro.m_SourceBlendFactor = dmGraphics::BLEND_FACTOR_DST_COLOR;
                ro.m_DestinationBlendFactor = dmGraphics::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;

            case dmRiveDDF::RiveModelDesc::BLEND_MODE_SCREEN:
                ro.m_SourceBlendFactor = dmGraphics::BLEND_FACTOR_ONE_MINUS_DST_COLOR;
                ro.m_DestinationBlendFactor = dmGraphics::BLEND_FACTOR_ONE;
            break;

            default:
                dmLogError("Unknown blend mode: %d\n", blend_mode);
                assert(0);
            break;
        }

        ro.m_SetBlendFactors = 1;
    }

    static void GetRiveDrawParams(rive::HContext ctx, rive::HRenderer renderer, uint32_t& vertex_count, uint32_t& index_count, uint32_t& render_object_count)
    {
        vertex_count        = 0;
        index_count         = 0;
        render_object_count = 0;

        for (int i = 0; i < rive::getDrawEventCount(renderer); ++i)
        {
            const rive::PathDrawEvent evt = rive::getDrawEvent(renderer, i);

            switch(evt.m_Type)
            {
                case rive::EVENT_DRAW_STENCIL:
                {
                    rive::DrawBuffers buffers = rive::getDrawBuffers(ctx, renderer, evt.m_Path);
                    RiveBuffer* vxBuffer      = (RiveBuffer*) buffers.m_VertexBuffer;

                    if (vxBuffer != 0)
                    {
                        uint32_t vx_count = vxBuffer->m_Size / sizeof(RiveVertex);
                        index_count += (vx_count - 5) * 3;
                        vertex_count += vx_count;
                        render_object_count++;
                    }
                } break;
                case rive::EVENT_DRAW_COVER:
                {
                    rive::DrawBuffers buffers = rive::getDrawBuffers(ctx, renderer, evt.m_Path);
                    RiveBuffer* vxBuffer      = (RiveBuffer*) buffers.m_VertexBuffer;

                    if (vxBuffer != 0)
                    {
                        index_count += 3 * 2;
                        vertex_count += vxBuffer->m_Size / sizeof(RiveVertex);
                        render_object_count++;
                    }
                } break;
                default:break;
            }
        }
    }

    static void GenerateVertexData(RiveWorld*   world,
        dmRender::HRenderContext                render_context,
        dmGameSystem::HComponentRenderConstants render_constants,
        dmRender::HMaterial                     material,
        dmRiveDDF::RiveModelDesc::BlendMode     blend_mode,
        rive::HContext                          rive_ctx,
        rive::HRenderer                         renderer,
        RiveVertex*&                            vx_ptr,
        int*&                                   ix_ptr,
        unsigned int                            ro_start)
    {
        dmGraphics::FaceWinding last_face_winding = dmGraphics::FACE_WINDING_CCW;
        rive::HRenderPaint paint                  = 0;
        uint32_t last_ix                          = 0;
        uint32_t vx_offset                        = 0;
        uint8_t clear_clipping_flag               = 0;
        bool is_applying_clipping                 = false;
        uint32_t ro_index                         = ro_start;

        rive::DrawBuffers buffers_renderer = rive::getDrawBuffers(rive_ctx, renderer, 0);
        RiveBuffer* ixBuffer               = (RiveBuffer*) buffers_renderer.m_IndexBuffer;
        int* ix_data_ptr                   = (int*) ixBuffer->m_Data;

        for (int i = 0; i < rive::getDrawEventCount(renderer); ++i)
        {
            const rive::PathDrawEvent evt = rive::getDrawEvent(renderer, i);

            switch(evt.m_Type)
            {
                case rive::EVENT_SET_PAINT:
                    paint = evt.m_Paint;
                    break;
                case rive::EVENT_DRAW_STENCIL:
                {
                    rive::DrawBuffers buffers = rive::getDrawBuffers(rive_ctx, renderer, evt.m_Path);
                    RiveBuffer* vxBuffer      = (RiveBuffer*) buffers.m_VertexBuffer;

                    if (vxBuffer != 0)
                    {
                        uint32_t vx_count = vxBuffer->m_Size / sizeof(RiveVertex);

                        int triangle_count = vx_count - 5;
                        if (vx_count < 5)
                        {
                            continue;
                        }

                        uint32_t ix_count = triangle_count * 3;

                        for (int j = 0; j < ix_count; ++j)
                        {
                            ix_ptr[j] = ix_data_ptr[j + 6] + vx_offset;
                        }

                        memcpy(vx_ptr, vxBuffer->m_Data, vxBuffer->m_Size);

                        dmRender::RenderObject& ro = world->m_RenderObjects[ro_index];
                        ro.Init();
                        ro.m_VertexDeclaration = world->m_VertexDeclaration;
                        ro.m_VertexBuffer      = world->m_VertexBuffer;
                        ro.m_PrimitiveType     = dmGraphics::PRIMITIVE_TRIANGLES;
                        ro.m_VertexStart       = last_ix; // byte offset
                        ro.m_VertexCount       = ix_count;
                        ro.m_Material          = material;
                        ro.m_IndexBuffer       = world->m_IndexBuffer;
                        ro.m_IndexType         = dmGraphics::TYPE_UNSIGNED_INT;
                        ro.m_SetStencilTest    = 1;
                        ro.m_SetFaceWinding    = 1;

                        dmRender::StencilTestParams& stencil_state = ro.m_StencilTestParams;

                        stencil_state.m_Front = {
                            .m_Func     = dmGraphics::COMPARE_FUNC_ALWAYS,
                            .m_OpSFail  = dmGraphics::STENCIL_OP_KEEP,
                            .m_OpDPFail = dmGraphics::STENCIL_OP_KEEP,
                            .m_OpDPPass = dmGraphics::STENCIL_OP_INCR_WRAP,
                        };

                        stencil_state.m_Back = {
                            .m_Func     = dmGraphics::COMPARE_FUNC_ALWAYS,
                            .m_OpSFail  = dmGraphics::STENCIL_OP_KEEP,
                            .m_OpDPFail = dmGraphics::STENCIL_OP_KEEP,
                            .m_OpDPPass = dmGraphics::STENCIL_OP_DECR_WRAP,
                        };

                        stencil_state.m_Ref                = 0x00;
                        stencil_state.m_RefMask            = 0xFF;
                        stencil_state.m_BufferMask         = 0xFF;
                        stencil_state.m_ColorBufferMask    = 0x00;
                        stencil_state.m_ClearBuffer        = 0;
                        stencil_state.m_SeparateFaceStates = 1;

                        if (evt.m_IsClipping)
                        {
                            stencil_state.m_Front.m_Func = dmGraphics::COMPARE_FUNC_EQUAL;
                            stencil_state.m_Back.m_Func  = dmGraphics::COMPARE_FUNC_EQUAL;
                            stencil_state.m_Ref          = 0x80;
                            stencil_state.m_RefMask      = 0x80;
                            stencil_state.m_BufferMask   = 0x7F;
                        }

                        if (clear_clipping_flag)
                        {
                            stencil_state.m_ClearBuffer = 1;
                            clear_clipping_flag = 0;
                        }

                        if (evt.m_IsEvenOdd && (evt.m_Idx % 2) != 0)
                        {
                            ro.m_FaceWinding = dmGraphics::FACE_WINDING_CW;
                        }
                        else
                        {
                            ro.m_FaceWinding = dmGraphics::FACE_WINDING_CCW;
                        }

                        dmGameObject::PropertyVar apply_clipping_var(Vectormath::Aos::Vector4(0.0f, 0.0f, 0.0f, 0.0f));
                        dmGameSystem::SetRenderConstant(render_constants, ro.m_Material, UNIFORM_COVER, 0, apply_clipping_var);
                        dmGameSystem::EnableRenderObjectConstants(&ro, render_constants);

                        Mat2DToMat4(evt.m_TransformWorld, ro.m_WorldTransform);
                        dmRender::AddToRender(render_context, &ro);

                        last_face_winding  = ro.m_FaceWinding;
                        vx_ptr            += vx_count;
                        ix_ptr            += ix_count;
                        last_ix           += ix_count * sizeof(int);
                        vx_offset         += vx_count;
                        ro_index++;
                    }
                } break;
                case rive::EVENT_DRAW_COVER:
                {
                    rive::DrawBuffers buffers = rive::getDrawBuffers(rive_ctx, renderer, evt.m_Path);
                    RiveBuffer* vxBuffer      = (RiveBuffer*) buffers.m_VertexBuffer;

                    if (vxBuffer != 0 && ixBuffer != 0)
                    {
                        int* ix_data_ptr  = (int*) ixBuffer->m_Data;
                        uint32_t vx_count = 4;
                        uint32_t ix_count = 2 * 3;

                        for (int j = 0; j < ix_count; ++j)
                        {
                            ix_ptr[j] = ix_data_ptr[j] + vx_offset;
                        }

                        memcpy(vx_ptr, vxBuffer->m_Data, vxBuffer->m_Size);

                        dmRender::RenderObject& ro = world->m_RenderObjects[ro_index];
                        ro.Init();
                        ro.m_VertexDeclaration = world->m_VertexDeclaration;
                        ro.m_VertexBuffer      = world->m_VertexBuffer;
                        ro.m_PrimitiveType     = dmGraphics::PRIMITIVE_TRIANGLES;
                        ro.m_VertexStart       = last_ix; // byte offset
                        ro.m_VertexCount       = ix_count;
                        ro.m_Material          = material;
                        ro.m_IndexBuffer       = world->m_IndexBuffer;
                        ro.m_IndexType         = dmGraphics::TYPE_UNSIGNED_INT;
                        ro.m_SetStencilTest    = 1;

                        dmRender::StencilTestParams& stencil_state = ro.m_StencilTestParams;
                        stencil_state.m_ClearBuffer = 0;

                        if (is_applying_clipping)
                        {
                            stencil_state.m_Front = {
                                .m_Func     = dmGraphics::COMPARE_FUNC_NOTEQUAL,
                                .m_OpSFail  = dmGraphics::STENCIL_OP_ZERO,
                                .m_OpDPFail = dmGraphics::STENCIL_OP_ZERO,
                                .m_OpDPPass = dmGraphics::STENCIL_OP_REPLACE,
                            };

                            stencil_state.m_Ref             = 0x80;
                            stencil_state.m_RefMask         = 0x7F;
                            stencil_state.m_BufferMask      = 0xFF;
                            stencil_state.m_ColorBufferMask = 0x00;
                        }
                        else
                        {
                            stencil_state.m_Front = {
                                .m_Func     = dmGraphics::COMPARE_FUNC_NOTEQUAL,
                                .m_OpSFail  = dmGraphics::STENCIL_OP_ZERO,
                                .m_OpDPFail = dmGraphics::STENCIL_OP_ZERO,
                                .m_OpDPPass = dmGraphics::STENCIL_OP_ZERO,
                            };

                            stencil_state.m_Ref             = 0x00;
                            stencil_state.m_RefMask         = 0xFF;
                            stencil_state.m_BufferMask      = 0xFF;
                            stencil_state.m_ColorBufferMask = 0x0F;

                            if (evt.m_IsClipping)
                            {
                                stencil_state.m_RefMask    = 0x7F;
                                stencil_state.m_BufferMask = 0x7F;
                            }

                            SetBlendMode(ro, blend_mode);

                            const rive::PaintData draw_entry_paint = rive::getPaintData(paint);
                            const float* color                     = &draw_entry_paint.m_Colors[0];
                            dmGameObject::PropertyVar colorVar(Vectormath::Aos::Vector4(color[0], color[1], color[2], color[3]));
                            dmGameSystem::SetRenderConstant(render_constants, ro.m_Material, UNIFORM_COLOR, 0, colorVar);
                        }

                        // If we are fullscreen-covering, we don't transform the vertices
                        float no_projection = (float) evt.m_IsClipping && is_applying_clipping;
                        dmGameObject::PropertyVar apply_clipping_var(Vectormath::Aos::Vector4(no_projection, 0.0f, 0.0f, 0.0f));
                        dmGameSystem::SetRenderConstant(render_constants, ro.m_Material, UNIFORM_COVER, 0, apply_clipping_var);
                        dmGameSystem::EnableRenderObjectConstants(&ro, render_constants);

                        if (last_face_winding != dmGraphics::FACE_WINDING_CCW)
                        {
                            ro.m_FaceWinding    = dmGraphics::FACE_WINDING_CCW;
                            ro.m_SetFaceWinding = 1;
                        }

                        Mat2DToMat4(evt.m_TransformWorld, ro.m_WorldTransform);
                        dmRender::AddToRender(render_context, &ro);

                        vx_ptr    += vx_count;
                        ix_ptr    += ix_count;
                        last_ix   += ix_count * sizeof(int);
                        vx_offset += vx_count;
                        ro_index++;
                    }
                } break;
                case rive::EVENT_CLIPPING_BEGIN:
                    is_applying_clipping = true;
                    clear_clipping_flag = 1;
                    break;
                case rive::EVENT_CLIPPING_END:
                    is_applying_clipping = false;
                    break;
                case rive::EVENT_CLIPPING_DISABLE:
                    is_applying_clipping = false;
                    break;
                default:break;
            }
        }
    }

    static void RenderBatch(RiveWorld* world, dmRender::HRenderContext render_context, dmRender::RenderListEntry *buf, uint32_t* begin, uint32_t* end)
    {
        RiveComponent* first        = (RiveComponent*) buf[*begin].m_UserData;
        RiveModelResource* resource = first->m_Resource;
        rive::HContext ctx          = world->m_Ctx->m_RiveContext;
        rive::HRenderer renderer    = world->m_Ctx->m_RiveRenderer;

        uint32_t ro_count         = 0;
        uint32_t vertex_count     = 0;
        uint32_t index_count      = 0;
        GetRiveDrawParams(ctx, renderer, vertex_count, index_count, ro_count);
        uint32_t ro_size = world->m_RenderObjects.Size() + ro_count;

        if (world->m_RenderObjects.Remaining() < ro_size)
        {
            world->m_RenderObjects.OffsetCapacity(ro_size - world->m_RenderObjects.Remaining());
        }

        uint32_t ro_index = world->m_RenderObjects.Size();
        world->m_RenderObjects.SetSize(ro_index + ro_count);

        dmArray<RiveVertex> &vertex_buffer = world->m_VertexBufferData;
        if (vertex_buffer.Remaining() < vertex_count)
        {
            vertex_buffer.OffsetCapacity(vertex_count - vertex_buffer.Remaining());
        }

        dmArray<int> &index_buffer = world->m_IndexBufferData;
        if (index_buffer.Remaining() < index_count)
        {
            index_buffer.OffsetCapacity(index_count - index_buffer.Remaining());
        }

        RiveVertex* vb_begin = vertex_buffer.End();
        RiveVertex* vb_end = vb_begin;

        int* ix_begin = index_buffer.End();
        int* ix_end   = ix_begin;

        if (!first->m_RenderConstants)
        {
            first->m_RenderConstants = dmGameSystem::CreateRenderConstants();
        }

        GenerateVertexData(world, render_context, first->m_RenderConstants,
            GetMaterial(first, resource), resource->m_DDF->m_BlendMode,
            ctx, renderer, vb_end, ix_end, ro_index);

        vertex_buffer.SetSize(vb_end - vertex_buffer.Begin());
        index_buffer.SetSize(ix_end - index_buffer.Begin());
    }

    void UpdateTransforms(RiveWorld* world)
    {
        //DM_PROFILE(SpineModel, "UpdateTransforms");

        dmArray<RiveComponent*>& components = world->m_Components.m_Objects;
        uint32_t n = components.Size();
        for (uint32_t i = 0; i < n; ++i)
        {
            RiveComponent* c = components[i];

            if (!c->m_Enabled || !c->m_AddedToUpdate)
                continue;

            const Matrix4& go_world = dmGameObject::GetWorldMatrix(c->m_Instance);
            const Matrix4 local = dmTransform::ToMatrix4(c->m_Transform);
            // if (dmGameObject::ScaleAlongZ(c->m_Instance))
            // {
            //     c->m_World = go_world * local;
            // }
            // else
            {
                c->m_World = dmTransform::MulNoScaleZ(go_world, local);
            }
        }
    }

    dmGameObject::CreateResult CompRiveAddToUpdate(const dmGameObject::ComponentAddToUpdateParams& params)
    {
        RiveWorld* world = (RiveWorld*)params.m_World;
        uint32_t index = (uint32_t)*params.m_UserData;
        RiveComponent* component = world->m_Components.Get(index);
        component->m_AddedToUpdate = true;
        return dmGameObject::CREATE_RESULT_OK;
    }

    static bool GetSender(RiveComponent* component, dmMessage::URL* out_sender)
    {
        dmMessage::URL sender;
        sender.m_Socket = dmGameObject::GetMessageSocket(dmGameObject::GetCollection(component->m_Instance));
        if (dmMessage::IsSocketValid(sender.m_Socket))
        {
            dmGameObject::Result go_result = dmGameObject::GetComponentId(component->m_Instance, component->m_ComponentIndex, &sender.m_Fragment);
            if (go_result == dmGameObject::RESULT_OK)
            {
                sender.m_Path = dmGameObject::GetIdentifier(component->m_Instance);
                *out_sender = sender;
                return true;
            }
        }
        return false;
    }

    static void CompRiveAnimationReset(RiveComponent* component)
    {
        component->m_AnimationIndex        = 0xff;
        component->m_AnimationCallbackRef  = 0;
        component->m_AnimationPlaybackRate = 1.0f;
        component->m_AnimationPlayback     = dmGameObject::PLAYBACK_NONE;

        if (component->m_AnimationInstance)
            delete component->m_AnimationInstance;
        component->m_AnimationInstance = 0;

        if (component->m_StateMachineInstance)
            delete component->m_StateMachineInstance;
        component->m_StateMachineInstance = 0;
        component->m_StateMachineInputs.SetSize(0);
    }

    static void CompRiveAnimationDoneCallback(RiveComponent& component)
    {
        assert(component.m_AnimationInstance);
        dmMessage::URL sender;
        dmMessage::URL receiver  = component.m_Listener;

        if (!GetSender(&component, &sender))
        {
            dmLogError("Could not send animation_done to listener because of incomplete component.");
            return;
        }

        dmRive::RiveSceneData* data = (dmRive::RiveSceneData*) component.m_Resource->m_Scene->m_Scene;
        dmhash_t message_id         = dmRiveDDF::RiveAnimationDone::m_DDFDescriptor->m_NameHash;

        dmRiveDDF::RiveAnimationDone message;
        message.m_AnimationId = data->m_LinearAnimations[component.m_AnimationIndex];
        message.m_Playback    = component.m_AnimationPlayback;

        uintptr_t descriptor = (uintptr_t)dmRiveDDF::RiveAnimationDone::m_DDFDescriptor;
        uint32_t data_size   = sizeof(dmRiveDDF::RiveAnimationDone);

        dmMessage::Result result = dmMessage::Post(&sender, &receiver, message_id, 0, component.m_AnimationCallbackRef, descriptor, &message, data_size, 0);
        dmMessage::ResetURL(&component.m_Listener);
        if (result != dmMessage::RESULT_OK)
        {
            dmLogError("Could not send animation_done to listener.");
        }
    }

    dmGameObject::UpdateResult CompRiveUpdate(const dmGameObject::ComponentsUpdateParams& params, dmGameObject::ComponentsUpdateResult& update_result)
    {
        RiveWorld* world         = (RiveWorld*)params.m_World;
        rive::HContext ctx       = world->m_Ctx->m_RiveContext;
        rive::HRenderer renderer = world->m_Ctx->m_RiveRenderer;

        rive::newFrame(renderer);
        rive::Renderer* rive_renderer = (rive::Renderer*) renderer;
        float dt = params.m_UpdateContext->m_DT;

        dmArray<RiveComponent*>& components = world->m_Components.m_Objects;
        const uint32_t count = components.Size();

        world->m_DrawEntries.SetSize(0);

        //const rive::RenderMode render_mode = rive::getRenderMode(ctx);

        for (uint32_t i = 0; i < count; ++i)
        {
            RiveComponent& component = *components[i];
            component.m_DoRender = 0;

            if (!component.m_Enabled || !component.m_AddedToUpdate)
            {
                continue;
            }

            // RIVE UPDATE
            dmRive::RiveSceneData* data = (dmRive::RiveSceneData*) component.m_Resource->m_Scene->m_Scene;
            rive::File* f               = data->m_File;
            rive::Artboard* artboard    = f->artboard();

            if (!artboard)
            {
                component.m_Enabled = false;
                continue;
            }
            rive::AABB artboard_bounds  = artboard->bounds();

            if (component.m_StateMachineInstance)
            {
                component.m_StateMachineInstance->advance(artboard, dt * component.m_AnimationPlaybackRate);
            }
            else if (component.m_AnimationInstance)
            {
                component.m_AnimationInstance->advance(dt * component.m_AnimationPlaybackRate);
                component.m_AnimationInstance->apply(artboard, 1.0f);

                if (component.m_AnimationInstance->didLoop())
                {
                    bool did_finish = false;
                    switch(component.m_AnimationPlayback)
                    {
                        case dmGameObject::PLAYBACK_ONCE_FORWARD:
                            did_finish = true;
                            break;
                        case dmGameObject::PLAYBACK_ONCE_BACKWARD:
                            did_finish = true;
                            break;
                        case dmGameObject::PLAYBACK_ONCE_PINGPONG:
                            did_finish = component.m_AnimationInstance->direction() > 0;
                            break;
                        default:break;
                    }

                    if (did_finish)
                    {
                        CompRiveAnimationDoneCallback(component);
                        CompRiveAnimationReset(&component);
                    }
                }
            }

            rive::Mat2D transform;
            Mat4ToMat2D(component.m_World, transform);

            // JG: Rive is using a different coordinate system that defold,
            //     in their examples they flip the projection but that isn't
            //     really compatible with our setup I don't think?
            rive::Vec2D yflip(1.0f,-1.0f);
            rive::Mat2D::scale(transform, transform, yflip);
            rive::setTransform(renderer, transform);
            rive::resetClipping(renderer);

            rive_renderer->align(rive::Fit::none,
                rive::Alignment::center,
                rive::AABB(-artboard_bounds.width(), -artboard_bounds.height(),
                artboard_bounds.width(), artboard_bounds.height()),
                artboard_bounds);

            rive_renderer->save();
            artboard->advance(dt);
            artboard->draw(rive_renderer);
            rive_renderer->restore();

            UpdateBones(&component); // after the artboard->advance();

            if (component.m_ReHash || (component.m_RenderConstants && dmGameSystem::AreRenderConstantsUpdated(component.m_RenderConstants)))
            {
                ReHash(&component);
            }

            component.m_DoRender = 1;
        }

        // If the child bones have been updated, we need to return true
        update_result.m_TransformsUpdated = false;

        return dmGameObject::UPDATE_RESULT_OK;
    }

    static void RenderListDispatch(dmRender::RenderListDispatchParams const &params)
    {
        RiveWorld *world            = (RiveWorld *) params.m_UserData;
        rive::RenderMode renderMode = rive::getRenderMode(world->m_Ctx->m_RiveContext);

        switch (params.m_Operation)
        {
            case dmRender::RENDER_LIST_OPERATION_BEGIN:
            {
                dmGraphics::SetVertexBufferData(world->m_VertexBuffer, 0, 0, dmGraphics::BUFFER_USAGE_STATIC_DRAW);
                dmGraphics::SetIndexBufferData(world->m_IndexBuffer, 0, 0, dmGraphics::BUFFER_USAGE_STATIC_DRAW);

                world->m_RenderObjects.SetSize(0);
                dmArray<RiveVertex>& vertex_buffer = world->m_VertexBufferData;
                vertex_buffer.SetSize(0);

                dmArray<int>& index_buffer = world->m_IndexBufferData;
                index_buffer.SetSize(0);
                break;
            }
            case dmRender::RENDER_LIST_OPERATION_BATCH:
            {
                RenderBatch(world, params.m_Context, params.m_Buf, params.m_Begin, params.m_End);
                break;
            }
            case dmRender::RENDER_LIST_OPERATION_END:
            {
                dmGraphics::SetVertexBufferData(world->m_VertexBuffer, sizeof(RiveVertex) * world->m_VertexBufferData.Size(),
                                                world->m_VertexBufferData.Begin(), dmGraphics::BUFFER_USAGE_STATIC_DRAW);
                dmGraphics::SetIndexBufferData(world->m_IndexBuffer, sizeof(int) * world->m_IndexBufferData.Size(),
                                                world->m_IndexBufferData.Begin(), dmGraphics::BUFFER_USAGE_STATIC_DRAW);
                break;
            }
            default:
                assert(false);
                break;
        }
    }

    dmGameObject::UpdateResult CompRiveRender(const dmGameObject::ComponentsRenderParams& params)
    {
        CompRiveContext* context = (CompRiveContext*)params.m_Context;
        dmRender::HRenderContext render_context = context->m_RenderContext;
        RiveWorld* world = (RiveWorld*)params.m_World;

        UpdateTransforms(world);

        dmArray<RiveComponent*>& components = world->m_Components.m_Objects;
        const uint32_t count = components.Size();

        // Prepare list submit
        dmRender::RenderListEntry* render_list = dmRender::RenderListAlloc(render_context, count);
        dmRender::HRenderListDispatch dispatch = dmRender::RenderListMakeDispatch(render_context, &RenderListDispatch, world);
        dmRender::RenderListEntry* write_ptr   = render_list;

        for (uint32_t i = 0; i < count; ++i)
        {
            RiveComponent& component = *components[i];
            if (!component.m_DoRender || !component.m_Enabled)
            {
                continue;
            }
            const Vector4 trans        = component.m_World.getCol(3);
            write_ptr->m_WorldPosition = Point3(trans.getX(), trans.getY(), trans.getZ());
            write_ptr->m_UserData      = (uintptr_t) &component;
            write_ptr->m_BatchKey      = component.m_MixedHash;
            write_ptr->m_TagListKey    = dmRender::GetMaterialTagListKey(GetMaterial(&component, component.m_Resource));
            write_ptr->m_Dispatch      = dispatch;
            write_ptr->m_MinorOrder    = 0;
            write_ptr->m_MajorOrder    = dmRender::RENDER_ORDER_WORLD;
            ++write_ptr;
        }

        dmRender::RenderListSubmit(render_context, render_list, write_ptr);
        return dmGameObject::UPDATE_RESULT_OK;
    }

    static bool CompRiveGetConstantCallback(void* user_data, dmhash_t name_hash, dmRender::Constant** out_constant)
    {
        RiveComponent* component = (RiveComponent*)user_data;
        return component->m_RenderConstants && dmGameSystem::GetRenderConstant(component->m_RenderConstants, name_hash, out_constant);
    }

    static void CompRiveSetConstantCallback(void* user_data, dmhash_t name_hash, uint32_t* element_index, const dmGameObject::PropertyVar& var)
    {
        RiveComponent* component = (RiveComponent*)user_data;
        if (!component->m_RenderConstants)
            component->m_RenderConstants = dmGameSystem::CreateRenderConstants();
        dmGameSystem::SetRenderConstant(component->m_RenderConstants, GetMaterial(component, component->m_Resource), name_hash, element_index, var);
        component->m_ReHash = 1;
    }

    static int FindAnimationIndex(dmhash_t* entries, uint32_t num_entries, dmhash_t anim_id)
    {
        for (int i = 0; i < num_entries; ++i)
        {
            if (entries[i] == anim_id)
            {
                return i;
            }
        }
        return -1;
    }

    static rive::LinearAnimation* FindAnimation(dmRive::RiveSceneData* data, int* animation_index, dmhash_t anim_id)
    {
        rive::Artboard* artboard = data->m_File->artboard();
        int index = FindAnimationIndex(data->m_LinearAnimations.Begin(), data->m_LinearAnimations.Size(), anim_id);
        if (index == -1) {
            return 0;
        }
        *animation_index = index;
        return artboard->animation(index);
    }

    static rive::StateMachine* FindStateMachine(dmRive::RiveSceneData* data, int* state_machine_index, dmhash_t anim_id)
    {
        rive::Artboard* artboard = data->m_File->artboard();
        int index = FindAnimationIndex(data->m_StateMachines.Begin(), data->m_StateMachines.Size(), anim_id);
        if (index == -1) {
            return 0;
        }
        *state_machine_index = index;
        return artboard->stateMachine(index);
    }

    static bool PlayAnimation(RiveComponent* component, dmRive::RiveSceneData* data, dmhash_t anim_id,
                                dmGameObject::Playback playback_mode, float offset, float playback_rate)
    {
        int animation_index;
        rive::LinearAnimation* animation = FindAnimation(data, &animation_index, anim_id);

        if (!animation) {
            return false;
        }

        CompRiveAnimationReset(component);

        rive::Loop loop_value = rive::Loop::oneShot;
        int play_direction    = 1;
        float play_time       = animation->startSeconds();
        float offset_value    = animation->durationSeconds() * offset;

        switch(playback_mode)
        {
            case dmGameObject::PLAYBACK_ONCE_FORWARD:
                loop_value     = rive::Loop::oneShot;
                break;
            case dmGameObject::PLAYBACK_ONCE_BACKWARD:
                loop_value     = rive::Loop::oneShot;
                play_direction = -1;
                play_time      = animation->endSeconds();
                offset_value   = -offset_value;
                break;
            case dmGameObject::PLAYBACK_ONCE_PINGPONG:
                loop_value     = rive::Loop::pingPong;
                break;
            case dmGameObject::PLAYBACK_LOOP_FORWARD:
                loop_value     = rive::Loop::loop;
                break;
            case dmGameObject::PLAYBACK_LOOP_BACKWARD:
                loop_value     = rive::Loop::loop;
                play_direction = -1;
                play_time      = animation->endSeconds();
                offset_value   = -offset_value;
                break;
            case dmGameObject::PLAYBACK_LOOP_PINGPONG:
                loop_value     = rive::Loop::pingPong;
                break;
            default:break;
        }

        component->m_AnimationIndex        = animation_index;
        component->m_AnimationPlaybackRate = playback_rate;
        component->m_AnimationPlayback     = playback_mode;
        component->m_AnimationInstance     = new rive::LinearAnimationInstance(animation);
        component->m_AnimationInstance->time(play_time + offset_value);
        component->m_AnimationInstance->loopValue((int)loop_value);
        component->m_AnimationInstance->direction(play_direction);
        return true;
    }

    static bool PlayStateMachine(RiveComponent* component, dmRive::RiveSceneData* data, dmhash_t anim_id, float playback_rate)
    {
        int state_machine_index;
        rive::StateMachine* state_machine = FindStateMachine(data, &state_machine_index, anim_id);

        if (!state_machine) {
            return false;
        }

        CompRiveAnimationReset(component);

        component->m_StateMachineInstance = new rive::StateMachineInstance(state_machine);
        component->m_AnimationPlaybackRate = playback_rate;

        // update the list of current state machine inputs
        uint32_t count = state_machine->inputCount();
        if (count > component->m_StateMachineInputs.Capacity())
        {
            component->m_StateMachineInputs.SetCapacity(count);
        }
        component->m_StateMachineInputs.SetSize(count);

        for (uint32_t i = 0; i < count; ++i)
        {
            const rive::StateMachineInput* input = state_machine->input(i);
            component->m_StateMachineInputs[i] = dmHashString64(input->name().c_str());
        }
        return true;
    }

    dmGameObject::UpdateResult CompRiveOnMessage(const dmGameObject::ComponentOnMessageParams& params)
    {
        RiveWorld* world = (RiveWorld*)params.m_World;
        RiveComponent* component = world->m_Components.Get(*params.m_UserData);
        if (params.m_Message->m_Id == dmGameObjectDDF::Enable::m_DDFDescriptor->m_NameHash)
        {
            component->m_Enabled = 1;
        }
        else if (params.m_Message->m_Id == dmGameObjectDDF::Disable::m_DDFDescriptor->m_NameHash)
        {
            component->m_Enabled = 0;
        }
        else if (params.m_Message->m_Descriptor != 0x0)
        {
            if (params.m_Message->m_Id == dmRiveDDF::RivePlayAnimation::m_DDFDescriptor->m_NameHash)
            {
                dmRiveDDF::RivePlayAnimation* ddf = (dmRiveDDF::RivePlayAnimation*)params.m_Message->m_Data;
                dmRive::RiveSceneData* data       = (dmRive::RiveSceneData*) component->m_Resource->m_Scene->m_Scene;

                dmhash_t anim_id = ddf->m_AnimationId;

                if (ddf->m_IsStateMachine)
                {
                    bool result = PlayStateMachine(component, data, anim_id, ddf->m_PlaybackRate);
                    if (result) {
                        //component->m_AnimationCallbackRef  = params.m_Message->m_UserData2;
                        //component->m_Listener              = params.m_Message->m_Sender;
                    } else {
                        dmLogError("Couldn't play state machine named '%s'", dmHashReverseSafe64(anim_id));
                    }

                } else {
                    bool result = PlayAnimation(component, data, anim_id, (dmGameObject::Playback)ddf->m_Playback, ddf->m_Offset, ddf->m_PlaybackRate);
                    if (result) {
                        component->m_AnimationCallbackRef  = params.m_Message->m_UserData2;
                        component->m_Listener              = params.m_Message->m_Sender;
                    } else {
                        dmLogError("Couldn't play animation named '%s'", dmHashReverseSafe64(anim_id));
                    }
                }
            }
            else if (params.m_Message->m_Id == dmRiveDDF::RiveCancelAnimation::m_DDFDescriptor->m_NameHash)
            {
                CompRiveAnimationReset(component);
            }
        }

        return dmGameObject::UPDATE_RESULT_OK;
    }

    static bool OnResourceReloaded(RiveWorld* world, RiveComponent* component, int index)
    {
        // Make it regenerate the batch key
        component->m_ReHash = 1;
        return true;
    }

    void CompRiveOnReload(const dmGameObject::ComponentOnReloadParams& params)
    {
        RiveWorld* world = (RiveWorld*)params.m_World;
        int index = *params.m_UserData;
        RiveComponent* component = GetComponentFromIndex(world, index);
        component->m_Resource = (RiveModelResource*)params.m_Resource;
        (void)OnResourceReloaded(world, component, index);
    }

    static int FindStateMachineInputIndex(RiveComponent* component, dmhash_t property_name)
    {
        uint32_t count = component->m_StateMachineInputs.Size();
        for (uint32_t i = 0; i < count; ++i)
        {
            if (component->m_StateMachineInputs[i] == property_name)
            {
                return (int)i;
            }
        }
        return -1;
    }

    static dmGameObject::PropertyResult SetStateMachineInput(RiveComponent* component, int index, const dmGameObject::ComponentSetPropertyParams& params)
    {
        const rive::StateMachine* state_machine = component->m_StateMachineInstance->stateMachine();
        const rive::StateMachineInput* input = state_machine->input(index);
        rive::SMIInput* input_instance = component->m_StateMachineInstance->input(index);

        if (input->is<rive::StateMachineTrigger>())
        {
            if (params.m_Value.m_Type != dmGameObject::PROPERTY_TYPE_BOOLEAN)
            {
                dmLogError("Found property %s of type trigger, but didn't receive a boolean", input->name().c_str());
                return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;
            }

            // The trigger can only respond to the value "true"
            if (!params.m_Value.m_Bool)
            {
                dmLogError("Found property %s of type trigger, but didn't receive a boolean of true", input->name().c_str());
                return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;
            }

            rive::SMITrigger* trigger = (rive::SMITrigger*)input_instance;
            trigger->fire();
        }
        else if (input->is<rive::StateMachineBool>())
        {
            if (params.m_Value.m_Type != dmGameObject::PROPERTY_TYPE_BOOLEAN)
            {
                dmLogError("Found property %s of type bool, but didn't receive a boolean", input->name().c_str());
                return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;
            }

            rive::SMIBool* v = (rive::SMIBool*)input_instance;
            v->value(params.m_Value.m_Bool);
        }
        else if (input->is<rive::StateMachineNumber>())
        {
            if (params.m_Value.m_Type != dmGameObject::PROPERTY_TYPE_NUMBER)
            {
                dmLogError("Found property %s of type number, but didn't receive a number", input->name().c_str());
                return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;
            }

            rive::SMINumber* v = (rive::SMINumber*)input_instance;
            v->value(params.m_Value.m_Number);
        }

        return dmGameObject::PROPERTY_RESULT_OK;
    }

    static dmGameObject::PropertyResult GetStateMachineInput(RiveComponent* component, int index,
            const dmGameObject::ComponentGetPropertyParams& params, dmGameObject::PropertyDesc& out_value)
    {
        const rive::StateMachine* state_machine = component->m_StateMachineInstance->stateMachine();
        const rive::StateMachineInput* input = state_machine->input(index);
        rive::SMIInput* input_instance = component->m_StateMachineInstance->input(index);

        if (input->is<rive::StateMachineTrigger>())
        {
            dmLogError("Cannot get value of input type trigger ( %s )", input->name().c_str());
            return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;
        }
        else if (input->is<rive::StateMachineBool>())
        {
            rive::SMIBool* v = (rive::SMIBool*)input_instance;
            out_value.m_Variant = dmGameObject::PropertyVar(v->value());
        }
        else if (input->is<rive::StateMachineNumber>())
        {
            rive::SMINumber* v = (rive::SMINumber*)input_instance;
            out_value.m_Variant = dmGameObject::PropertyVar(v->value());
        }

        return dmGameObject::PROPERTY_RESULT_OK;
    }

    dmGameObject::PropertyResult CompRiveGetProperty(const dmGameObject::ComponentGetPropertyParams& params, dmGameObject::PropertyDesc& out_value)
    {
        CompRiveContext* context = (CompRiveContext*)params.m_Context;
        RiveWorld* world = (RiveWorld*)params.m_World;
        RiveComponent* component = GetComponentFromIndex(world, *params.m_UserData);
        dmRive::RiveSceneData* data = (dmRive::RiveSceneData*) component->m_Resource->m_Scene->m_Scene;

        if (params.m_PropertyId == PROP_ANIMATION)
        {
            if (component->m_AnimationInstance && component->m_AnimationIndex < data->m_LinearAnimations.Size())
            {
                out_value.m_Variant = dmGameObject::PropertyVar(data->m_LinearAnimations[component->m_AnimationIndex]);
            }
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_CURSOR)
        {
            if (component->m_AnimationInstance)
            {
                const rive::LinearAnimation* animation = component->m_AnimationInstance->animation();
                float cursor_value                     = (component->m_AnimationInstance->time() - animation->startSeconds()) / animation->durationSeconds();
                out_value.m_Variant                    = dmGameObject::PropertyVar(cursor_value);
            }
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_PLAYBACK_RATE)
        {
            out_value.m_Variant = dmGameObject::PropertyVar(component->m_AnimationPlaybackRate);
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_MATERIAL)
        {
            dmRender::HMaterial material = GetMaterial(component, component->m_Resource);
            return dmGameSystem::GetResourceProperty(context->m_Factory, material, out_value);
        } else {
            if (component->m_StateMachineInstance)
            {
                int index = FindStateMachineInputIndex(component, params.m_PropertyId);
                if (index >= 0)
                {
                    return GetStateMachineInput(component, index, params, out_value);
                }
            }
        }
        return dmGameSystem::GetMaterialConstant(GetMaterial(component, component->m_Resource), params.m_PropertyId, out_value, true, CompRiveGetConstantCallback, component);
    }

    dmGameObject::PropertyResult CompRiveSetProperty(const dmGameObject::ComponentSetPropertyParams& params)
    {
        RiveWorld* world = (RiveWorld*)params.m_World;
        RiveComponent* component = world->m_Components.Get(*params.m_UserData);
        if (params.m_PropertyId == PROP_CURSOR)
        {
            if (params.m_Value.m_Type != dmGameObject::PROPERTY_TYPE_NUMBER)
                return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;

            if (component->m_AnimationInstance)
            {
                const rive::LinearAnimation* animation = component->m_AnimationInstance->animation();
                float cursor = params.m_Value.m_Number * animation->durationSeconds() + animation->startSeconds();
                component->m_AnimationInstance->time(cursor);
            }

            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_PLAYBACK_RATE)
        {
            if (params.m_Value.m_Type != dmGameObject::PROPERTY_TYPE_NUMBER)
                return dmGameObject::PROPERTY_RESULT_TYPE_MISMATCH;

            component->m_AnimationPlaybackRate = params.m_Value.m_Number;
            return dmGameObject::PROPERTY_RESULT_OK;
        }
        else if (params.m_PropertyId == PROP_MATERIAL)
        {
            CompRiveContext* context = (CompRiveContext*)params.m_Context;
            dmGameObject::PropertyResult res = dmGameSystem::SetResourceProperty(context->m_Factory, params.m_Value, MATERIAL_EXT_HASH, (void**)&component->m_Material);
            component->m_ReHash |= res == dmGameObject::PROPERTY_RESULT_OK;
            return res;
        } else {
            if (component->m_StateMachineInstance)
            {
                int index = FindStateMachineInputIndex(component, params.m_PropertyId);
                if (index >= 0)
                {
                    return SetStateMachineInput(component, index, params);
                }
            }
        }
        return dmGameSystem::SetMaterialConstant(GetMaterial(component, component->m_Resource), params.m_PropertyId, params.m_Value, CompRiveSetConstantCallback, component);
    }

    static void ResourceReloadedCallback(const dmResource::ResourceReloadedParams& params)
    {
        RiveWorld* world = (RiveWorld*) params.m_UserData;
        dmArray<RiveComponent*>& components = world->m_Components.m_Objects;
        uint32_t n = components.Size();
        for (uint32_t i = 0; i < n; ++i)
        {
            RiveComponent* component = components[i];
            RiveModelResource* resource = component->m_Resource;
            if (!component->m_Enabled || !resource)
                continue;

            if (resource == params.m_Resource->m_Resource ||
                resource->m_Scene == params.m_Resource->m_Resource ||
                resource->m_Scene->m_Scene == params.m_Resource->m_Resource)
            {
                OnResourceReloaded(world, component, i);
            }
        }
    }

    static dmGameObject::Result CompRiveRegister(const dmGameObject::ComponentTypeCreateCtx* ctx, dmGameObject::ComponentType* type)
    {
        CompRiveContext* rivectx    = new CompRiveContext;
        rivectx->m_Factory          = ctx->m_Factory;
        rivectx->m_GraphicsContext  = *(dmGraphics::HContext*)ctx->m_Contexts.Get(dmHashString64("graphics"));
        rivectx->m_RenderContext    = *(dmRender::HRenderContext*)ctx->m_Contexts.Get(dmHashString64("render"));
        rivectx->m_MaxInstanceCount = dmConfigFile::GetInt(ctx->m_Config, "rive.max_instance_count", 128);
        rivectx->m_RiveContext      = rive::createContext();

        rive::g_Ctx = rivectx->m_RiveContext;
        rive::setRenderMode(rivectx->m_RiveContext, rive::MODE_STENCIL_TO_COVER);
        rive::setBufferCallbacks(rivectx->m_RiveContext, RiveRequestBufferCallback, RiveDestroyBufferCallback, 0x0);

        rivectx->m_RiveRenderer = rive::createRenderer(rivectx->m_RiveContext);
        rive::setContourQuality(rivectx->m_RiveRenderer, 0.8888888888888889f);
        rive::setClippingSupport(rivectx->m_RiveRenderer, true);

        // after script/anim/gui, before collisionobject
        // the idea is to let the scripts/animations update the game object instance,
        // and then let the component update its bones, allowing them to influence collision objects in the same frame
        ComponentTypeSetPrio(type, 350);
        ComponentTypeSetContext(type, rivectx);
        ComponentTypeSetHasUserData(type, true);
        ComponentTypeSetReadsTransforms(type, false);

        ComponentTypeSetNewWorldFn(type, CompRiveNewWorld);
        ComponentTypeSetDeleteWorldFn(type, CompRiveDeleteWorld);
        ComponentTypeSetCreateFn(type, CompRiveCreate);
        ComponentTypeSetDestroyFn(type, CompRiveDestroy);
        ComponentTypeSetAddToUpdateFn(type, CompRiveAddToUpdate);
        ComponentTypeSetUpdateFn(type, CompRiveUpdate);
        ComponentTypeSetRenderFn(type, CompRiveRender);
        ComponentTypeSetOnMessageFn(type, CompRiveOnMessage);
            // ComponentTypeSetOnInputFn(type, CompRiveOnInput);
        ComponentTypeSetOnReloadFn(type, CompRiveOnReload);
        ComponentTypeSetGetPropertyFn(type, CompRiveGetProperty);
        ComponentTypeSetSetPropertyFn(type, CompRiveSetProperty);
            // ComponentTypeSetPropertyIteratorFn(type, CompRiveIterProperties); // for debugging/testing e.g. via extension-poco
        ComponentTypeSetGetFn(type, CompRiveGetComponent);

        return dmGameObject::RESULT_OK;
    }


    static void DeleteBones(RiveComponent* component)
    {
        dmGameObject::HInstance rive_instance = component->m_Instance;
        dmGameObject::HCollection collection = dmGameObject::GetCollection(rive_instance);

        uint32_t num_bones = component->m_BoneGOs.Size();
        for (uint32_t i = 0; i < num_bones; ++i)
        {
            dmGameObject::HInstance bone_instance = component->m_BoneGOs[i];
            if (bone_instance)
            {
                dmGameObject::Delete(collection, bone_instance, false);
            }
        }
        component->m_BoneGOs.SetSize(0);
    }

    static void UpdateBones(RiveComponent* component)
    {
        dmRive::RiveSceneData* data = (dmRive::RiveSceneData*) component->m_Resource->m_Scene->m_Scene;

        rive::Artboard* artboard    = data->m_File->artboard();

        rive::AABB bounds = artboard->bounds();
        float cx = (bounds.maxX - bounds.minX) * 0.5f;
        float cy = (bounds.maxY - bounds.minY) * 0.5f;

        uint32_t num_bones = component->m_BoneGOs.Size();

        dmVMath::Point3 go_pos = dmGameObject::GetPosition(component->m_Instance);

        for (uint32_t i = 0; i < num_bones; ++i)
        {
            dmGameObject::HInstance bone_instance = component->m_BoneGOs[i];
            dmRive::RiveBone* bone = data->m_Bones[i];

            const rive::Mat2D& rt = bone->m_Bone->worldTransform();

            dmVMath::Vector4 x_axis(rt.xx(), rt.xy(), 0, 0);
            dmVMath::Vector4 y_axis(rt.yx(), rt.yy(), 0, 0);

            float scale_x = length(x_axis);
            float scale_y = length(y_axis);
            dmVMath::Vector3 scale(scale_x, scale_y, 1);

            float angle = atan2f(x_axis.getY(), x_axis.getX());
            Quat rotation = Quat::rotationZ(-angle);

            // Since the Rive space is different, we need to flip the y axis
            dmVMath::Vector3 pos(rt.tx() - cx, -rt.ty() + cy, 0);

            dmVMath::Matrix4 world_transform(rotation, pos);

            dmTransform::Transform transform = dmTransform::ToTransform(world_transform);

            dmGameObject::SetPosition(bone_instance, Point3(transform.GetTranslation()));
            dmGameObject::SetRotation(bone_instance, transform.GetRotation());
            dmGameObject::SetScale(bone_instance, transform.GetScale());
        }
    }

    static bool CreateBones(RiveWorld* world, RiveComponent* component, dmRive::RiveSceneData* data)
    {
        dmGameObject::HInstance rive_instance = component->m_Instance;
        dmGameObject::HCollection collection = dmGameObject::GetCollection(rive_instance);

        uint32_t num_bones = data->m_Bones.Size();

        component->m_BoneGOs.SetCapacity(num_bones);
        component->m_BoneGOs.SetSize(num_bones);

        for (uint32_t i = 0; i < num_bones; ++i)
        {
            dmRive::RiveBone* bone = data->m_Bones[i];

            dmGameObject::HInstance bone_instance = dmGameObject::New(collection, 0x0);
            if (bone_instance == 0x0) {
                DeleteBones(component);
                return false;
            }

            component->m_BoneGOs[i] = bone_instance;

            uint32_t index = dmGameObject::AcquireInstanceIndex(collection);
            if (index == dmGameObject::INVALID_INSTANCE_POOL_INDEX)
            {
                DeleteBones(component);
                return false;
            }

            dmhash_t id = dmGameObject::ConstructInstanceId(index);
            dmGameObject::AssignInstanceIndex(index, bone_instance);

            dmGameObject::Result result = dmGameObject::SetIdentifier(collection, bone_instance, id);
            if (dmGameObject::RESULT_OK != result)
            {
                DeleteBones(component);
                return false;
            }

            dmGameObject::SetBone(bone_instance, true);

            // Since we're given the "world" coordinates from the rive bones,
            // we don't really need a full hierarchy. So we use the actual game object as parent
            dmGameObject::SetParent(bone_instance, rive_instance);
        }

        rive::Artboard* artboard = data->m_File->artboard();
        if (artboard) {
            artboard->advance(0.0f);
        }

        // Set the properties
        UpdateBones(component);

        return true;
    }

    static rive::HBuffer RiveRequestBufferCallback(rive::HBuffer buffer, rive::BufferType type, void* data, unsigned int dataSize, void* userData)
    {
        RiveBuffer* buf = (RiveBuffer*) buffer;
        if (dataSize == 0)
        {
            return 0;
        }

        if (buf == 0)
        {
            buf = new RiveBuffer();
        }

        buf->m_Data = realloc(buf->m_Data, dataSize);
        buf->m_Size = dataSize;
        memcpy(buf->m_Data, data, dataSize);

        return (rive::HBuffer) buf;
    }

    static void RiveDestroyBufferCallback(rive::HBuffer buffer, void* userData)
    {
        RiveBuffer* buf = (RiveBuffer*) buffer;
        if (buf != 0)
        {
            if (buf->m_Data != 0)
            {
                free(buf->m_Data);
            }

            delete buf;
        }
    }

    // ******************************************************************************
    // SCRIPTING HELPER FUNCTIONS
    // ******************************************************************************

    bool CompRiveGetBoneID(RiveComponent* component, dmhash_t bone_name, dmhash_t* id)
    {
        dmRive::RiveSceneData* data = (dmRive::RiveSceneData*) component->m_Resource->m_Scene->m_Scene;
        uint32_t num_bones = data->m_Bones.Size();

        // We need the arrays to be matching 1:1 (for lookup using the same indices)
        if (num_bones == 0 || component->m_BoneGOs.Size() != num_bones) {
            return false;
        }

        for (uint32_t i = 0; i < num_bones; ++i)
        {
            dmRive::RiveBone* bone = data->m_Bones[i];
            dmGameObject::HInstance bone_instance = component->m_BoneGOs[i];

            if (bone_name == bone->m_NameHash)
            {
                *id = dmGameObject::GetIdentifier(bone_instance);
                return true;
            }
        }

        return false;
    }
}

DM_DECLARE_COMPONENT_TYPE(ComponentTypeRive, "rivemodelc", dmRive::CompRiveRegister);
