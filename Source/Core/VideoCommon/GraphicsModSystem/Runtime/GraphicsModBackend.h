// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <span>
#include <string_view>

#include "Common/CommonTypes.h"
#include "VideoCommon/GraphicsModSystem/Runtime/CameraManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/FBInfo.h"
#include "VideoCommon/GraphicsModSystem/Runtime/FrameData.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModActionData.h"
#include "VideoCommon/GraphicsModSystem/Runtime/SkeletonManager.h"
#include "VideoCommon/GraphicsModSystem/Types.h"
#include "VideoCommon/IGraphicsHandler.h"
#include "VideoCommon/IMeshRenderer.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/ShaderGenCommon.h"
#include "VideoCommon/VideoEvents.h"

class GraphicsModAction;
namespace GraphicsModSystem::Runtime
{
class GraphicsModBackend
{
public:
  virtual ~GraphicsModBackend() = default;

  virtual void OnDraw(const DrawDataView& draw_started, IMeshRenderer* mesh_renderer) = 0;
  // The entry is valid for the duration of this callback only; it may be destroyed immediately
  // after.
  virtual void OnTextureUnload(const TCacheEntry* entry) = 0;
  virtual void OnTextureLoad(const TextureRef& texture) = 0;
  virtual void OnTextureCreate(const TextureRef& texture) = 0;
  virtual void OnLight() = 0;
  virtual void OnFramePresented(const PresentInfo& present_info);

  virtual void AddIndices(OpcodeDecoder::Primitive primitive, u32 num_vertices) = 0;
  virtual void ResetIndices() = 0;

  void SetHostConfig(const ShaderHostConfig& config);

  void ClearAdditionalCameras(const MathUtil::Rectangle<int>& rc, bool color_enable,
                              bool alpha_enable, bool z_enable, u32 color, u32 z);

  XFBFrame* GetFrameData();

  // Legacy behavior
  virtual void OnEFBStarted(const FBInfo& info, GraphicsModActionData::PreEFB* efb) = 0;
  virtual void OnEFBFinished(const FBInfo& info, GraphicsModActionData::PostEFB* efb) = 0;
  virtual void OnXFBStarted(const FBInfo& info) = 0;
  virtual void OnXFBFinished(const FBInfo& info) = 0;

protected:
  void CustomDraw(const DrawDataView& draw_data, IMeshRenderer* mesh_renderer,
                  std::span<GraphicsModAction*> actions, DrawCallID draw_call,
                  std::string_view group_name);

  // This will ensure gpu skinned draw calls are joined together
  DrawCallID GetSkinnedDrawCallID(DrawCallID draw_call_id, MaterialID material_id,
                                  const DrawDataView& draw_data);

  static bool IsDrawGPUSkinned(NativeVertexFormat* format, PrimitiveType primitive_type);

  ShaderHostConfig m_shader_host_config;

  VideoCommon::CameraManager m_camera_manager;
  XFBFrame m_xfb_frame;
  VideoCommon::SkeletonManager m_skeleton_manager;

private:
  bool m_last_draw_gpu_skinned = false;
  GraphicsModSystem::DrawCallID m_last_draw_call_id = GraphicsModSystem::DrawCallID::INVALID;
  GraphicsModSystem::MaterialID m_last_material_id = GraphicsModSystem::MaterialID::INVALID;
};
}  // namespace GraphicsModSystem::Runtime
