// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

#include "VideoCommon/GraphicsModEditor/Controls/TimelineWidget.h"
#include "VideoCommon/GraphicsModSystem/Runtime/PresentedFrame.h"

namespace GraphicsModEditor::Panels
{
/// Subscribes to GetPresentedFrameEvents().presented_frame_published_event (copies
/// display metadata + a flattened TimelineEntry list inside the callback — never
/// stores the presented-frame pointer). Scrub controls enabled only while
/// Core::GetState() == Paused AND the active system's graphics mode is deferred.
/// On selection: computes the global op index and calls
/// EmulatedGraphicsManager::RequestScrubPrefix (contract §4). Does NOT call the
/// executor itself — the paused-present timer path executes the request.
class ScrubberPanel
{
public:
  ScrubberPanel();
  ~ScrubberPanel();

  void DrawImGui();

private:
  void OnPresentedFramePublished(const GraphicsModSystem::Runtime::PresentedFrame& frame);
  void ScrubTo(std::optional<u32> global_op);  // nullopt = full frame (un-scrub)

  TimelineWidget m_timeline;
  // Latest published frame's display metadata (copied in the event callback):
  u64 m_frame_number = 0;
  std::size_t m_total_ops = 0;
  std::vector<u32> m_copy_starts;  // global index of each copy start (boundary markers)
  std::optional<u32> m_selected;  // nullopt = full frame
  Common::EventHook m_presented_frame_hook;
  Common::EventHook m_entry_selected_hook;
  Common::EventHook m_range_selected_hook;
};
}  // namespace GraphicsModEditor::Panels
