// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

namespace GraphicsModEditor
{
/// One strip cell. Pure display data; the panel builds entries from a presented frame.
struct TimelineEntry
{
  std::string label;     // "Draw 9a1b2c3d", "Clear", "SV", "RT→0x08000000", "PE", ...
  std::string category;  // "draw" | "clear" | "sv" | "copy" | "pe" | "data" | "other" (drives color)
  u32 index;             // caller-defined index echoed back on selection (here: global op index)
  std::string tooltip;   // pre-formatted detail (optional)
  bool is_boundary;      // render a copy-boundary marker before this entry
};

/// Horizontal op-sequence strip with click/drag selection and keyboard stepping.
/// Knows nothing about XFBCopyData — entries in, selection out. Reusable by the future
/// FMV panel and Frame Signature Triggers (FrameScrubberDesign Part 4).
class TimelineWidget
{
public:
  void SetEntries(std::vector<TimelineEntry> entries);
  void SetSelected(std::optional<u32> index);  // panel-driven (keyboard scrub, reset)

  Common::HookableEvent<u32> entry_selected_event;             // single op selected
  Common::HookableEvent<std::pair<u32, u32>> range_selected_event;  // drag range (inclusive)

  void DrawImGui();  // call every frame while the panel is open

private:
  static ImU32 CategoryColor(const std::string& category);
  static const char* CategoryGlyph(const std::string& category);

  std::vector<TimelineEntry> m_entries;
  std::optional<u32> m_selected;
  int m_hover_index = -1;
  bool m_dragging = false;
  std::optional<u32> m_drag_start;
  std::optional<u32> m_drag_current;
  std::optional<u32> m_last_scrolled_to;  // selection the strip was last centered on
};
}  // namespace GraphicsModEditor
