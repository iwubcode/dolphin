// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <vector>

#include "Common/CommonTypes.h"

#include "VideoCommon/GraphicsModSystem/Runtime/XFBCopy.h"

namespace GraphicsModSystem::Runtime
{
/// Debug-only vertex-lifetime token (UnifiedReplayDesign §3, invariant I6).
/// Monotonic counter incremented at every XFB-boundary capture. A presented frame is
/// vertex-valid only while the counter is unchanged (no frame committed since capture).
u64 GetVertexLifetimeCounter();
void BumpVertexLifetimeCounter();

/// A presented frame: every XFB copy recorded between the previous finalize and this
/// present (invariant M2). Finalized at present time; replay loops the copies in
/// order against the persistent main EFB, exactly like the live path composes a
/// multi-copy frame.
struct PresentedFrame
{
  u64 frame_number = 0;  // director's present-finalize counter
  // Adopted from the last copy's capture token (invariant I6): the frame is
  // vertex-valid only while the counter is unchanged since the frame's last capture.
  u64 vertex_lifetime_token = 0;

  std::vector<XFBCopy> copies;

  /// Sum of copy op counts — the size of the flattened global op-index space.
  std::size_t TotalOperations() const;

  /// Position of a global op index within `copies`. Global index g counts ops across
  /// copies in order: g ∈ [0, TotalOperations()). Returns the copy containing g and
  /// the local prefix length within it. ASSERT g < TotalOperations().
  struct OperationIndex
  {
    std::size_t copy_index;
    std::size_t local_op_count;  // replay [0, local_op_count) of that copy
  };
  OperationIndex FindOperation(std::size_t global_operation_index) const;
};

/// One copy prefix in a replay range (Phase 3 scrubber, D12).
struct XFBCopyRange
{
  std::size_t copy_index;
  std::size_t op_count;  // replay [0, op_count) of this copy
};

/// Per-copy prefix list for one replay of a presented frame. Pure loop scaffolding,
/// NOT session state: built locally at the top of ReplayPresentedFrame from the stored
/// scrub position each tick and destroyed at the end of it, so there is no stored
/// derived state that could go stale against a new presented frame. Every entry is a
/// copy prefix; all entries except possibly the last are full.
struct PresentedFrameCopyRange
{
  std::vector<XFBCopyRange> copies;  // in copy order, contiguous from copy 0
};
}  // namespace GraphicsModSystem::Runtime
