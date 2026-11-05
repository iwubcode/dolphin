// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/XFBDirector.h"

#include <algorithm>

#include "Common/Assert.h"

#include "VideoCommon/GraphicsModSystem/Runtime/PresentedFrameEvents.h"
#include "VideoCommon/VideoConfig.h"
#include "VideoCommon/VideoEvents.h"

namespace GraphicsModSystem::Runtime
{
XFBDirector::XFBDirector()
{
  m_frame_present_event = GetVideoEvents().after_present_event.Register(
      [this](const PresentInfo& present_info) { OnFramePresented(present_info); });
}

ReplayPassPlan
XFBDirector::BuildDrawPlan(const XFBCopyData::View& view, std::size_t op_count,
                           const std::optional<EmulatedGraphics::ScissorsAndViewportData>& sv_seed)
{
  // Byte-identical to the plan ExecuteXFBOperations built inline before Phase 2 (contract §5 C3):
  // identity op_indices, main EFB target, TextureCache, impacts=true, no interpolation.
  ReplayPassPlan plan;
  plan.op_indices.resize(op_count);
  for (std::size_t i = 0; i < op_count; ++i)
    plan.op_indices[i] = i;
  plan.target = g_framebuffer_manager.get();
  // ScissorPos/ScissorOffset are bitfield unions with deleted copy-assignment,
  // so optional<ScissorsAndViewportData> is non-assignable: reset + emplace.
  plan.sv_seed.reset();
  if (sv_seed.has_value())
    plan.sv_seed.emplace(*sv_seed);
  plan.data_target = DataTarget::TextureCache;
  plan.impacts_emulation_state = true;
  plan.interpolation_phase = std::nullopt;
  plan.variant = PipelineVariant::Emulated;
  return plan;
}

ReplayPassPlan
XFBDirector::BuildCopyPlan(const XFBCopy& copy,
                           const std::optional<EmulatedGraphics::ScissorsAndViewportData>& sv_seed,
                           std::size_t op_count)
{
  // P3-f: the prefix never exceeds the copy's op count
  ASSERT(op_count <= copy.GetView().operations.size());

  ReplayPassPlan plan;
  plan.op_indices.resize(op_count);
  for (std::size_t i = 0; i < op_count; ++i)
    plan.op_indices[i] = i;
  plan.target = g_framebuffer_manager.get();
  // ScissorPos/ScissorOffset are bitfield unions with deleted copy-assignment,
  // so optional<ScissorsAndViewportData> is non-assignable: reset + emplace.
  plan.sv_seed.reset();
  if (sv_seed.has_value())
    plan.sv_seed.emplace(*sv_seed);
  plan.data_target = DataTarget::TextureCache;
  plan.impacts_emulation_state = false;
  plan.interpolation_phase = std::nullopt;
  plan.variant = PipelineVariant::Emulated;

  return plan;
}

void XFBDirector::StoreXFBCopyOperations(XFBCopyData& recorder)
{
  // Bump the vertex-lifetime counter BEFORE stamping the token: the frame is
  // vertex-valid only while the counter is unchanged (invariant I6).
  BumpVertexLifetimeCounter();

  m_pending_copies.emplace_back();
  XFBCopy& copy = m_pending_copies.back();
  copy.vertex_lifetime_token = GetVertexLifetimeCounter();
  copy.created_present_count = m_last_present_count;

  // Compute the end-of-copy scissor/viewport over the full range BEFORE the move,
  // since the view spans the recorder's (about-to-be-moved) storage.
  const XFBCopyData::View view = recorder.GetView();
  ReplayExecutor::UpdateTrackedScissorViewport(view, 0, view.operations.size(), &copy.end_sv);

  copy.AddData(std::move(recorder));
}

void XFBDirector::AttachXfbCopy(EmulatedGraphics::FramebufferCopyResolvedData copy_data,
                                RcTcacheEntry entry)
{
  if (m_pending_copies.empty())
    return;
  XFBCopy& copy = m_pending_copies.back();
  copy.resolved_copy = std::move(copy_data);
  copy.copy_entry = std::move(entry);
}

void XFBDirector::OnFramePresented(const PresentInfo& present_info)
{
  m_last_present_count = present_info.present_count;

  // Bound retention: a copy never sampled this long is not coming back (e.g.
  // offscreen XFB dumps that are never presented).
  std::erase_if(m_pending_copies, [this](const XFBCopy& copy) {
    return m_last_present_count - copy.created_present_count > MAX_COPY_RETENTION_PRES;
  });

  // Partition by pointer identity: copies whose copy entry the presenter
  // actually sampled compose the presented frame; the rest are still waiting for
  // their present (late-flip double buffering presents a copy one frame after it
  // fires). Pointer identity is the exact per-buffer key — texture_info_name is
  // dimensions-only and collides across ping-pong buffers.
  std::vector<XFBCopy> presented;
  std::vector<XFBCopy> retained;
  for (auto& copy : m_pending_copies)
  {
    const TCacheEntry* entry = copy.copy_entry.get();
    const bool sampled = entry && std::ranges::contains(present_info.xfb_copy_entries, entry);
    if (sampled)
      presented.push_back(std::move(copy));
    else
      retained.push_back(std::move(copy));
  }
  m_pending_copies = std::move(retained);

  // Nothing of this present is copy-backed (static re-present, CPU-written XFB):
  // keep the previous frame (SceneDumper wait rule).
  if (presented.empty())
    return;

  PresentedFrame frame;
  frame.frame_number = ++m_frame_number;
  // Token of the LAST presented copy. With retention the frame can be one
  // present older than the newest capture; while paused no new captures happen,
  // so the vertices are still resident (see the relaxed I6 check in Execute).
  frame.vertex_lifetime_token = presented.back().vertex_lifetime_token;
  frame.copies = std::move(presented);

  m_ring.Push(std::move(frame));

  // The push always succeeds (ring capacity 1 overwrites), so the ring slot is valid.
  // Subscribers copy what they need in-callback; the pointer is the ring slot, valid
  // until the next push or ClearPresentedFrames() (contract §1).
  GetPresentedFrameEvents().presented_frame_published_event.Trigger(*m_ring.Latest());
}

void XFBDirector::ClearPresentedFrames()
{
  m_ring.Clear();
  m_pending_copies.clear();
}
}  // namespace GraphicsModSystem::Runtime
