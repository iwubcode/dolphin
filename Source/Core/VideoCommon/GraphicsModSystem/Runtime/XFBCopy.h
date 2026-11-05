// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>

#include "Common/CommonTypes.h"

#include "VideoCommon/GraphicsModSystem/Runtime/XFBCopyData.h"
#include "VideoCommon/IGraphicsHandler.h"
#include "VideoCommon/TextureCacheBase.h"

namespace GraphicsModSystem::Runtime
{
/// One XFB copy's worth of recorded operations, moved out of the live recorder
/// at the copy boundary. Self-contained: op indices are copy-relative (invariant M3).
/// Owns the operation storage so replay runs while the live handler records the
/// next frame — or, in this phase, while the emulator is paused.
struct XFBCopy
{
  // GetVertexLifetimeCounter() at this copy's capture. The PresentedFrame adopts
  // its last copy's token (invariant I6).
  u64 vertex_lifetime_token = 0;

  // XFBDirector present counter at capture time. Copies are retained until the
  // presenter samples their copy entry (late-flip double buffering presents a
  // copy one frame after it fires); this stamp bounds the retention.
  u64 created_present_count = 0;

  // The recorded operations for this XFB copy.
  XFBCopyData m_data;

  // End-of-copy scissor/viewport (last SV op), computed at capture via
  // ReplayExecutor::UpdateTrackedScissorViewport over the full range. Paused-replay
  // seed for the next copy (mirrors the live path's m_main_scissor_viewport carry).
  std::optional<EmulatedGraphics::ScissorsAndViewportData> end_sv;

  // The XFB copy that flushed this batch — attached AFTER capture (see §5 note C2).
  // Re-running its VRAM completion refreshes the presented image after a replay.
  std::optional<EmulatedGraphics::FramebufferCopyResolvedData> resolved_copy;
  RcTcacheEntry copy_entry;

  /// Spans over the owned XFBCopyData vectors.
  XFBCopyData::View GetView() const { return m_data.GetView(); }

  /// Move all recorded storage from src into this copy.
  void AddData(XFBCopyData&& src) { m_data = std::move(src); }
};
}  // namespace GraphicsModSystem::Runtime
