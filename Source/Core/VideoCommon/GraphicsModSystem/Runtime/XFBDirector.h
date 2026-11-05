// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

#include "VideoCommon/GraphicsModSystem/Runtime/PresentedFrame.h"
#include "VideoCommon/GraphicsModSystem/Runtime/PresentedFrameRing.h"
#include "VideoCommon/GraphicsModSystem/Runtime/ReplayExecutor.h"
#include "VideoCommon/IGraphicsHandler.h"
#include "VideoCommon/TextureCacheBase.h"

namespace GraphicsModSystem::Runtime
{
/// The only place replay policy is decided (UnifiedReplayDesign §2). Policy object —
/// no GPU calls. Copy time: copies accumulate in m_pending_copies. Present time:
/// OnFramePresented moves the copies whose copy entry the presenter actually
/// sampled (matched via PresentInfo::xfb_copy_entries pointer identity) into the
/// ring as one PresentedFrame. Copies not yet presented are RETAINED: late-flip double
/// buffering presents a copy one frame after it fires, so the presented frame's
/// copies are usually from the previous batch (SceneDumper's m_xfb_hash_to_scene
/// retention, keyed by entry pointer instead of the dimensions-only hash name).
/// Thread affinity (design §2/M1): every method here runs on the GPU emulation
/// thread (OnFramebufferCopy, the after_present_event hook, and the paused-replay
/// timer all execute there); no synchronization primitives are needed.
class XFBDirector
{
public:
  XFBDirector();

  // Live path (per XFB boundary): the single full-frame plan, exactly as
  // ExecuteXFBOperations builds it today. pass_name stays "main" (telemetry gate).
  ReplayPassPlan
  BuildDrawPlan(const XFBCopyData::View& view, std::size_t op_count,
                const std::optional<EmulatedGraphics::ScissorsAndViewportData>& sv_seed);

  // Paused path: prefix plan over one copy of the latest PresentedFrame. Fills:
  // op_indices = [0, op_count), target = g_framebuffer_manager, sv_seed = seed,
  // data_target = TextureCache, impacts_emulation_state = false,
  // interpolation_phase = nullopt, variant = Emulated.
  // Full copy: pass copy.GetView().operations.size(). Scrub prefix: pass the prefix
  // length. ASSERT(op_count <= copy.GetView().operations.size()).
  ReplayPassPlan
  BuildCopyPlan(const XFBCopy& copy,
                const std::optional<EmulatedGraphics::ScissorsAndViewportData>& sv_seed,
                std::size_t op_count);

  // XFB-copy-time capture: bumps the vertex-lifetime counter, moves the recorder's
  // contents into a pending copy (via XFBCopy::AddData), computes the copy's end_sv.
  // No ring push at copy time.
  void StoreXFBCopyOperations(XFBCopyData& recorder);

  // Attach the flushing XFB copy to the just-captured copy (§5 note C2).
  void AttachXfbCopy(EmulatedGraphics::FramebufferCopyResolvedData copy_data, RcTcacheEntry entry);

  // Present time (after_present_event): finalize the copies the presenter
  // actually sampled (pointer match against PresentInfo::xfb_copy_entries) into
  // one ring frame; retain the rest for their future present. No matches ->
  // static re-present / CPU-written XFB: keep the previous frame (SceneDumper
  // wait rule).
  void OnFramePresented(const PresentInfo&);

  // Savestate load: drop the ring AND any copies accumulated for the in-flight
  // frame (invariant M5).
  void ClearPresentedFrames();

  PresentedFrameRing& GetRing() { return m_ring; }

  // Copies accumulated since the last finalize (debug observability only).
  const std::vector<XFBCopy>& PendingCopies() const { return m_pending_copies; }

private:
  // Drop retained copies older than this many presents (a copy never displayed
  // this long is not coming back; bounds memory for games that copy without
  // presenting, e.g. offscreen XFB dumps).
  static constexpr u64 MAX_COPY_RETENTION_PRES = 4;

  PresentedFrameRing m_ring;  // capacity 1 in Phase 2
  std::vector<XFBCopy> m_pending_copies;
  u64 m_frame_number = 0;
  u64 m_last_present_count = 0;  // stamp for copy retention ages

  Common::EventHook m_frame_present_event;
};
}  // namespace GraphicsModSystem::Runtime
