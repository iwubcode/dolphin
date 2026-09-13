// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/IGraphicsHandler.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "Common/SmallVector.h"
#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/ExecutedTextureOperations.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModGraphicsHandler.h"
#include "VideoCommon/GraphicsModSystem/Runtime/NativeEFBSync.h"
#include "VideoCommon/GraphicsModSystem/Runtime/XFBCopyData.h"
#include "VideoCommon/GraphicsModSystem/Runtime/XFBDirector.h"
#include "VideoCommon/IFramebufferRenderer.h"
#include "VideoCommon/IMeshRenderer.h"
#include "VideoCommon/TextureCacheBase.h"

class DeferredGraphicsHandler final : public IGraphicsHandler,
                                      public IMeshRenderer,
                                      public IFramebufferRenderer
{
public:
  explicit DeferredGraphicsHandler(XFBCopyData* frame,
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

  /// Replays the ring's latest presented frame (paused FreeLook). No-op when the ring is empty.
  /// Consults the scrub request (m_scrub_global_op) to replay a flattened multi-copy prefix.
  void ReplayPresentedFrame();

  /// Scrub request (Phase 3). nullopt = full-frame replay (Phase 2 behavior).
  /// Clamped at request time (D12): asserts Core::GetState() == Paused when setting a
  /// value, and stores nullopt when global_op >= TotalOperations() of the ring's
  /// Latest() — the replay loop may assume a stored value is always < TotalOperations()
  /// of the current frame (safe: while paused the ring slot is static, and a new frame
  /// clears the position anyway). Also resets the executed-set session when the request
  /// transitions to nullopt from a set value (explicit un-scrub).
  void RequestScrubPrefix(std::optional<std::size_t> global_op);

  /// Clears the scrub session (position + executed sets). Called on resume
  /// (EmulatedGraphicsManager::SettingsChanged) and savestate load.
  void ResetScrubSession();

  /// Clears the presented-frame ring and pending copies (savestate load while paused).
  /// Present-time finalize is driven by the director's own after_present_event hook.
  /// Also resets the scrub session (invariant M5 extension).
  void ClearPresentedFrames()
  {
    m_director.ClearPresentedFrames();
    ResetScrubSession();
  }

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

  void ExecuteXFBOperations();

  XFBCopyData* m_xfb_copy_data;
  std::unique_ptr<NativeEFBSync> m_native_sync;

  // Last known viewport/scissor for the main (user-IR) target, persisted across replays.
  // Used to re-establish correct GPU state at the start of a replay when the game
  // did not issue a new ScissorViewport operation this frame.
  std::optional<EmulatedGraphics::ScissorsAndViewportData> m_main_scissor_viewport;

  GraphicsModGraphicsHandler m_graphics_mod_handler;

  GraphicsModSystem::Runtime::XFBDirector m_director;

  std::optional<EmulatedGraphics::ClearCopyData> m_last_clear_copy_data;

  // Scrub session (Phase 3). nullopt = full-frame replay (Phase 2 behavior).
  std::optional<std::size_t> m_scrub_global_op;
  // frame_number of the presented frame the executed sets were established for.
  u64 m_scrub_session_frame = 0;
  // One executed set per copy of the current presented frame; index == copy_index.
  // Invariant: size == frame->copies.size() whenever a session is live — established
  // at the session reset in ReplayPresentedFrame and in RequestScrubPrefix's un-scrub
  // reset. SmallVector (fixed capacity, zero heap) because copies per presented frame
  // are small; the capacity ASSERT at the resize site makes an over-capacity frame a
  // debug assert, not UB.
  Common::SmallVector<GraphicsModSystem::Runtime::ExecutedTextureOperations, 16>
      m_scrub_executed_texture_ops;
};
