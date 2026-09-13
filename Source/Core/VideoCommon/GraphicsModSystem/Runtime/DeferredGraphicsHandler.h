// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/IGraphicsHandler.h"

#include <optional>
#include <span>
#include <unordered_map>
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

  RcTcacheEntry
  OnFramebufferCopy(IFramebufferRenderer* renderer, PixelShaderManager& pixel_shader_manager,
                    EmulatedGraphics::FramebufferCopyResolvedData resolved_data) override;

  void FlushFramebufferCopy(IFramebufferRenderer* renderer,
                            const std::shared_ptr<TCacheEntry>& cache_entry) override;

  void OnFramebufferClear(EmulatedGraphics::ClearCopyData clear_data,
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
  void OnPartialTextureUpdate(PartialTextureUpdateContext partial_texture_update_context) override;

private:
  std::shared_ptr<TCacheEntry>
  RenderFramebufferCopy(PixelShaderManager& pixel_shader_manager,
                        const EmulatedGraphics::FramebufferCopyResolvedData& copy_data) override;
  void FlushFramebufferCopy(const std::shared_ptr<TCacheEntry>& cache_entry) override;

  void UninitializeFramebufferMemory(
      const EmulatedGraphics::FramebufferCopyResolvedData& copy_data) override;

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
  void DrawNativeEFBCopy(std::size_t first_operation_index, std::size_t last_operation_index);

  XFBFrame* m_xfb_frame;
  FramebufferManager* m_native_efb_framebuffer_manager;

  // Last known viewport/scissor per rendering target, persisted across replays.
  // Used to re-establish correct GPU state at the start of a replay when the game
  // did not issue a new ScissorViewport operation this frame.
  std::optional<EmulatedGraphics::ScissorsAndViewportData> m_native_scissor_viewport;
  std::optional<EmulatedGraphics::ScissorsAndViewportData> m_main_scissor_viewport;

  // Finds the last ScissorViewport op in [first, last) and stores it into *out.
  void UpdateTrackedScissorViewport(const XFBFrame::FrameView& view, std::size_t first,
                                    std::size_t last,
                                    std::optional<EmulatedGraphics::ScissorsAndViewportData>* out);

  // A map of the native EFB to
  std::unordered_map<TCacheEntry*, std::size_t> m_deferred_native_efb_to_last_operation_index;
  std::size_t m_last_native_efb_operation_index = 0;

  GraphicsModGraphicsHandler m_graphics_mod_handler;
};
