// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModEditor/EditorBackend.h"

#include <chrono>

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/GraphicsModEditor/EditorEvents.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModHash.h"
#include "VideoCommon/GraphicsModSystem/Types.h"
#include "VideoCommon/VertexLoaderManager.h"
#include "VideoCommon/XFMemory.h"

namespace GraphicsModEditor
{
namespace
{
bool IsDrawGPUSkinned(NativeVertexFormat* format, PrimitiveType primitive_type)
{
  if (primitive_type != PrimitiveType::Triangles && primitive_type != PrimitiveType::TriangleStrip)
  {
    return false;
  }

  const PortableVertexDeclaration vert_decl = format->GetVertexDeclaration();
  return vert_decl.posmtx.enable;
}
}  // namespace
EditorBackend::EditorBackend(EditorState& state) : m_state(state)
{
  m_selection_event = EditorEvents::ItemsSelectedEvent::Register(
      [this](const auto& selected_targets) { SelectionOccurred(selected_targets); },
      "EditorBackendSelect");
}

void EditorBackend::OnDraw(const GraphicsModSystem::DrawData& draw_data,
                           VertexManagerBase* vertex_manager)
{
  const auto hash_output =
      GraphicsModSystem::Runtime::GetDrawDataHash(m_state.m_user_data.m_hash_policy, draw_data);

  GraphicsModSystem::DrawCallID draw_call_id = hash_output.draw_call_id;
  const bool is_draw_gpu_skinned =
      IsDrawGPUSkinned(draw_data.vertex_format, draw_data.rasterization_state.primitive);
  if (is_draw_gpu_skinned && m_last_draw_gpu_skinned &&
      m_last_material_id == hash_output.material_id)
  {
    draw_call_id = m_last_draw_call_id;
  }
  m_last_draw_call_id = draw_call_id;
  m_last_material_id = hash_output.material_id;
  m_last_draw_gpu_skinned = is_draw_gpu_skinned;

  if (m_state.m_scene_dumper.IsRecording())
  {
    if (m_state.m_scene_dumper.IsDrawCallInRecording(draw_call_id))
    {
      GraphicsModEditor::SceneDumper::AdditionalDrawData additional_draw_data;
      if (!draw_data.vertex_format->GetVertexDeclaration().posmtx.enable)
      {
        float* pos = &xfmem.posMatrices[g_main_cp_state.matrix_index_a.PosNormalMtxIdx * 4];
        additional_draw_data.transform = {pos, 12};
      }
      m_state.m_scene_dumper.AddDataToRecording(hash_output.draw_call_id, draw_data,
                                                std::move(additional_draw_data));
    }
  }

  if (draw_call_id == hash_output.draw_call_id)
  {
    const auto now = std::chrono::steady_clock::now();
    if (auto iter = m_state.m_runtime_data.m_draw_call_id_to_data.find(hash_output.draw_call_id);
        iter != m_state.m_runtime_data.m_draw_call_id_to_data.end())
    {
      iter->second.draw_data = draw_data;

      // Clear out mesh details as spans aren't valid after this call
      iter->second.draw_data.vertex_data = {};
      iter->second.draw_data.index_data = {};
      iter->second.draw_data.gpu_skinning_position_transform = {};
      iter->second.draw_data.gpu_skinning_normal_transform = {};

      m_state.m_runtime_data.m_current_xfb.m_draw_call_ids.emplace(hash_output.draw_call_id,
                                                                   iter->second.m_create_time);
      iter->second.m_last_update_time = now;
    }
    else
    {
      GraphicsModEditor::DrawCallData data;
      data.m_id = hash_output.draw_call_id;
      data.draw_data = draw_data;

      // Clear out mesh details as spans aren't valid after this call
      data.draw_data.vertex_data = {};
      data.draw_data.index_data = {};

      data.m_create_time = now;
      data.m_last_update_time = data.m_create_time;
      m_state.m_runtime_data.m_draw_call_id_to_data.try_emplace(hash_output.draw_call_id,
                                                                std::move(data));
      m_state.m_runtime_data.m_current_xfb.m_draw_call_ids.emplace(hash_output.draw_call_id, now);
    }
  }

  if (m_selected_objects.contains(draw_call_id))
  {
    std::array<GraphicsModAction*, 1> actions{m_state.m_editor_data.m_highlight_action.get()};
    CustomDraw(draw_data, vertex_manager, actions);
  }
  else
  {
    auto& actions = m_state.m_user_data.m_draw_call_id_to_actions[draw_call_id];
    CustomDraw(draw_data, vertex_manager, actions);
  }
}

void EditorBackend::OnTextureLoad(const GraphicsModSystem::Texture& texture)
{
  if (auto iter = m_state.m_runtime_data.m_texture_cache_id_to_data.find(texture.hash_name);
      iter != m_state.m_runtime_data.m_texture_cache_id_to_data.end())
  {
    iter->second.texture = texture;
    iter->second.m_last_load_time = std::chrono::steady_clock::now();
  }
}

void EditorBackend::OnTextureCreate(const GraphicsModSystem::Texture& texture)
{
  if (texture.texture_type == GraphicsModSystem::Texture::TextureType::XFB)
  {
    // Skip XFBs that have no draw calls
    if (m_state.m_runtime_data.m_current_xfb.m_draw_call_ids.empty())
      return;

    // TODO: use string_view directly in C++26 (or use a different container)
    const std::string xfb_hash_name{texture.hash_name};
    m_state.m_runtime_data.m_xfb_to_data[xfb_hash_name] =
        std::move(m_state.m_runtime_data.m_current_xfb);
    m_state.m_runtime_data.m_current_xfb = {};

    m_state.m_scene_dumper.OnXFBCreated(xfb_hash_name);
    return;
  }

  std::string texture_cache_id{texture.hash_name};
  m_state.m_runtime_data.m_current_xfb.m_texture_cache_ids.insert(texture_cache_id);

  GraphicsModEditor::TextureCacheData data;
  data.m_id = texture_cache_id;
  data.texture = texture;
  data.m_create_time = std::chrono::steady_clock::now();
  m_state.m_runtime_data.m_texture_cache_id_to_data.insert_or_assign(std::move(texture_cache_id),
                                                                     std::move(data));
}

void EditorBackend::OnLight()
{
}

void EditorBackend::OnFramePresented(const PresentInfo& present_info)
{
  for (const auto& action : m_state.m_user_data.m_actions)
  {
    action->OnFrameEnd();
  }

  m_state.m_editor_data.m_highlight_action->OnFrameEnd();

  // Clear out the old xfbs presented
  for (const auto& xfb_presented : m_state.m_runtime_data.m_xfbs_presented)
  {
    m_state.m_runtime_data.m_xfb_to_data.erase(xfb_presented);
  }

  // Set the new ones
  m_state.m_runtime_data.m_xfbs_presented.clear();
  for (const auto& xfb_presented : present_info.xfb_copy_hashes)
  {
    m_state.m_runtime_data.m_xfbs_presented.push_back(std::string{xfb_presented});
  }

  m_state.m_scene_dumper.OnFramePresented(m_state.m_runtime_data.m_xfbs_presented);
}

void EditorBackend::SelectionOccurred(const std::set<SelectableType>& selection)
{
  m_selected_objects = selection;
}
}  // namespace GraphicsModEditor
