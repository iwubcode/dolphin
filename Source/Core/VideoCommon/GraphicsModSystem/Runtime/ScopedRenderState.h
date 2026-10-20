// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/IGraphicsHandler.h"

/// RAII guard that makes a replay pass a self-contained transaction on GPU state.
///
/// On construction:
///   - Saves g_gfx's stored viewport/scissor bookmark (the value EndUtilityDrawing
///     restores to), the currently bound framebuffer, and the live viewport/scissor,
///     so none of it can be corrupted by the replay.
///   - Applies `initial` (this target's last-known viewport/scissor) to the GPU,
///     establishing a correct starting point even if utility drawing reset the GPU
///     state since the previous replay. This call does mutate the global store and
///     live state, but both are undone when the guard is destroyed.
///
/// On destruction:
///   - Restores the saved bookmark, framebuffer binding, and viewport/scissor, so any
///     subsequent GPU work (main-pass draws, texture uploads, utility drawing) sees
///     exactly the state that existed before the replay began.
class ScopedRenderState
{
public:
  ScopedRenderState(FramebufferManager* fbm,
                    const std::optional<EmulatedGraphics::ScissorsAndViewportData>& initial);
  ~ScopedRenderState();

  ScopedRenderState(const ScopedRenderState&) = delete;
  ScopedRenderState& operator=(const ScopedRenderState&) = delete;

private:
  AbstractGfx::ViewportAndScissor m_saved;
  AbstractFramebuffer* m_saved_framebuffer;
};
