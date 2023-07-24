// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <set>
#include <vector>

#include "Common/HookableEvent.h"
#include "VideoCommon/GraphicsModEditor/EditorState.h"
#include "VideoCommon/GraphicsModEditor/EditorTypes.h"
#include "VideoCommon/GraphicsModSystem/Types.h"

namespace GraphicsModEditor::Panels
{
class ActiveTargetsPanel
{
public:
  explicit ActiveTargetsPanel(EditorState& state);

  // Renders ImGui windows to the currently-bound framebuffer.
  void DrawImGui();

private:
  void DrawCallPanel(const std::vector<GraphicsModEditor::RuntimeState::XFBData*>& xfbs);
  void EFBPanel(const std::vector<GraphicsModEditor::RuntimeState::XFBData*>& xfbs);
  void LightPanel(const std::vector<GraphicsModEditor::RuntimeState::XFBData*>& xfbs);
  Common::EventHook m_selection_event;

  EditorState& m_state;

  // Track open nodes
  std::set<GraphicsModSystem::DrawCallID> m_open_draw_call_nodes;
  std::set<GraphicsModSystem::TextureCacheID, std::less<>> m_open_texture_call_nodes;
  std::set<GraphicsModSystem::LightID> m_open_light_nodes;

  // Selected nodes
  std::set<SelectableType> m_selected_nodes;
  bool m_selection_list_changed;

  // Mesh extraction window details
  SceneDumper::RecordingRequest m_last_mesh_dump_request;
  bool m_open_mesh_dump_export_window = false;

  // Freeze details
  bool m_freeze_scene_details = false;
  std::map<std::string, RuntimeState::XFBData, std::less<>> m_frozen_xfb_to_data;
  std::vector<std::string> m_frozen_xfbs_presented;
};
}  // namespace GraphicsModEditor::Panels
