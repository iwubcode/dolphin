// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/IGraphicsHandler.h"

class EmulatedGraphicsHandler final : public IGraphicsHandler
{
public:
  explicit EmulatedGraphicsHandler(FramebufferManager* frame_buffer_manager,
                                   bool attempt_copy_efb_to_ram);

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
  FramebufferManager* m_frame_buffer_manager;
  bool m_attempt_copy_efb_to_ram;
};
