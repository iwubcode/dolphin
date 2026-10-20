// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/ExecutedTextureOperations.h"
#include "VideoCommon/GraphicsModSystem/Runtime/XFBCopyData.h"
#include "VideoCommon/IGraphicsHandler.h"

using GraphicsModSystem::Runtime::ExecutedTextureOperations;

class EmulatedGraphicsHandler;

/// Where a replay pass writes data ops and whether it may touch CPU-visible state.
/// Independent axes — see the gating table in the Phase 1 contract §3.
enum class DataTarget : u8
{
  TextureCache,  // data ops write real cache entries (live replay, scrubber)
  Isolated,      // data ops write RenderTargetPool textures (interpolation; unused in P1)
  Skip,          // data ops not executed (native sync; unused paths in P1)
};

enum class PipelineVariant : u8
{
  Emulated,     // the only reachable value in Phase 1
  GBuffer,      // reserved (Phase 7)
  DepthOnly,    // reserved (Phase 7)
  ForwardPlus,  // reserved (Phase 8)
};

/// The entire feature surface of a replay pass. Pure policy data: no pointers to
/// executors, handlers, or features. Seven fields; none names a feature.
struct ReplayPassPlan
{
  std::vector<std::size_t> op_indices;  // indices into XFBCopyData::View::operations, exec order
  FramebufferManager* target;           // replay target (its GetEFBFramebuffer() is bound)
  std::optional<EmulatedGraphics::ScissorsAndViewportData> sv_seed;
  DataTarget data_target = DataTarget::TextureCache;
  bool impacts_emulation_state = true;       // false => pixels only (no PE ops, no CPU effects)
  std::optional<float> interpolation_phase;  // nullopt in Phase 1 (all callers)
  PipelineVariant variant = PipelineVariant::Emulated;
};

/// Executes one plan as a pure transaction (Phase 1 contract §2). Feature-agnostic
/// (invariant I2): reads only plan fields + frame_view + uniform_state; pass_name is
/// telemetry-only. Stateless: the handler and target arrive per call (contract Note A).
class ReplayExecutor
{
public:
  // The executed_texture_ops parameter is the executed set for the unit the caller is
  // replaying (Phase 3: the current XFB copy). The executor knows op_i from its dispatch
  // loop and needs nothing else — it never learns what the unit is. Choosing the right
  // set is caller-side normalization, so the copy concept stays out of the executor
  // (invariant I2, D13). nullptr = no gating (all live paths).
  void Execute(EmulatedGraphicsHandler& emulated_graphics_handler, const ReplayPassPlan& plan,
               const XFBCopyData::View& frame_view,
               std::span<const XFBCopyData::UniformState> uniform_state, const char* pass_name,
               std::optional<u64> vertex_lifetime_token = std::nullopt,
               ExecutedTextureOperations* executed_texture_ops = nullptr);

  /// Finds the last ScissorViewport op in [first, last) and stores it into *out.
  /// (Moved verbatim from DeferredReplay.h; used by callers to update their sv_seed.)
  static void
  UpdateTrackedScissorViewport(const XFBCopyData::View& view, std::size_t first, std::size_t last,
                               std::optional<EmulatedGraphics::ScissorsAndViewportData>* out);
};
