// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/ScopedRenderState.h"

#include "VideoCommon/BPFunctions.h"

ScopedRenderState::ScopedRenderState(
    FramebufferManager* fbm,
    const std::optional<EmulatedGraphics::ScissorsAndViewportData>& initial)
    : m_saved(g_gfx->GetStoredViewportAndScissor()),
      m_saved_framebuffer(g_gfx->GetCurrentFramebuffer())
{
  if (initial.has_value())
  {
    BPFunctions::SetScissorAndViewport(fbm, initial->scissor_top_left,
                                       initial->scissor_bottom_right, initial->scissor_offset,
                                       initial->viewport);
  }
}

ScopedRenderState::~ScopedRenderState()
{
  // Restore the stored bookmark so EndUtilityDrawing restores a correct value.
  g_gfx->StoreViewportAndScissor(m_saved);

  // Restore the live GPU state (framebuffer binding, viewport, scissor) so that no
  // target-specific values leak into subsequent work outside this replay scope.
  if (m_saved_framebuffer)
    g_gfx->SetFramebuffer(m_saved_framebuffer);
  g_gfx->SetViewport(m_saved.viewport_x, m_saved.viewport_y, m_saved.viewport_width,
                     m_saved.viewport_height, m_saved.viewport_near_depth,
                     m_saved.viewport_far_depth);
  g_gfx->SetScissorRect(m_saved.scissor_rect);
}
