// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/IGraphicsHandler.h"

#include <optional>
#include <span>
#include <vector>

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/FrameData.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModGraphicsHandler.h"
#include "VideoCommon/IFramebufferRenderer.h"
#include "VideoCommon/IMeshRenderer.h"
#include "VideoCommon/TextureCacheBase.h"

class DeferredGraphicsHandler final : public IGraphicsHandler,
                                      public IMeshRenderer,
                                      public IFramebufferRenderer
{
public:
  explicit DeferredGraphicsHandler(XFBFrame* frame,
                                   FramebufferManager* native_efb_framebuffer_manager);

  RcTcacheEntry OnFrameBufferCopy(IFramebufferRenderer* renderer,
                                  PixelShaderManager& pixel_shader_manager,
                                  EmulatedGraphics::FrameBufferCopyData copy_data) override;

  void OnFrameBufferClear(EmulatedGraphics::ClearCopyData clear_data,
                          EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data,
                          bool is_xfb) override;

  void SetScissorAndViewport(
      EmulatedGraphics::ScissorsAndViewportData scissors_and_viewport_data) override;

  void PixelEngineOperation(EmulatedGraphics::PixelEngineData pixel_engine_data, bool notify_cpu,
                            bool notify_gpu) override;
  void PixelFormatChange(EmulatedGraphics::PixelFormatChangeData pixel_format_change_data) override;
  void OnDraw(GraphicsModSystem::DrawDataView draw_data, IMeshRenderer* mesh_renderer) override;

  void
  OnReinterpretTexture(EmulatedGraphics::ReinterpretTextureData reinterpret_texture_data) override;
  void OnPaletteTexture(EmulatedGraphics::PaletteTextureData palette_texture_data) override;
  void OnScaleTexture(EmulatedGraphics::ScaleTextureData scale_texture_data) override;

private:
  std::shared_ptr<TCacheEntry>
  RenderFramebufferCopy(PixelShaderManager& pixel_shader_manager,
                        EmulatedGraphics::FrameBufferCopyData copy_data) override;

  void
  UninitializeFramebufferMemory(const EmulatedGraphics::FrameBufferCopyData& copy_data) override;

  void DrawEmulatedMesh(
      const GraphicsModSystem::DrawDataView& draw_data,
      const Common::Matrix44& custom_transform = Common::Matrix44::Identity()) override;
  void DrawEmulatedMesh(
      const GraphicsModSystem::DrawDataView& draw_data,
      const VideoCommon::MaterialResource::Data& material_data,
      const Common::Matrix44& custom_transform = Common::Matrix44::Identity()) override;
  void DrawCustomMesh(const GraphicsModSystem::DrawDataView& draw_data,
                      GraphicsModSystem::DrawCallID draw_call,
                      const VideoCommon::MeshResource::Data& mesh_data,
                      const Common::Matrix44& custom_transform, bool ignore_mesh_transform,
                      VideoCommon::SkeletonInstance* skeleton_instance) override;

  void DrawXFBFrame();
  void DrawEFBCopy();

  XFBFrame* m_xfb_frame;
  std::optional<AbstractGfx::ViewportAndScissor> m_previous_frame_scissors_viewport;
  FramebufferManager* m_native_efb_framebuffer_manager;

  // Reference our xfb frame operations, for a *potential* EFB
  // that could be flushed, this avoids the cost of double drawing
  // when we don't need it
  std::vector<std::size_t> m_native_efb_operations;

  GraphicsModGraphicsHandler m_graphics_mod_handler;
};
