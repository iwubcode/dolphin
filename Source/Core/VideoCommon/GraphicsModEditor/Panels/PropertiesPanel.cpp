// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModEditor/Panels/PropertiesPanel.h"

#include <filesystem>

#include <fmt/format.h>
#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include "Common/EnumUtils.h"
#include "Common/VariantUtil.h"

#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/GraphicsModEditor/EditorEvents.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"
#include "VideoCommon/Present.h"
#include "VideoCommon/TextureUtils.h"

namespace GraphicsModEditor::Panels
{
PropertiesPanel::PropertiesPanel(EditorState& state)
    : m_state(state), m_material_control(m_state), m_shader_control(m_state),
      m_texture_control(m_state), m_mesh_control(m_state)
{
  m_selection_event = EditorEvents::ItemsSelectedEvent::Register(
      [this](const auto& selected_targets) { SelectionOccurred(selected_targets); },
      "EditorPropertiesPanel");
}

void PropertiesPanel::DrawImGui()
{
  const ImGuiViewport* main_viewport = ImGui::GetMainViewport();
  u32 default_window_height = g_presenter->GetTargetRectangle().GetHeight() -
                              ((float)g_presenter->GetTargetRectangle().GetHeight() * 0.1);
  u32 default_window_width = ((float)g_presenter->GetTargetRectangle().GetWidth() * 0.15);
  ImGui::SetNextWindowPos(ImVec2(main_viewport->WorkPos.x +
                                     g_presenter->GetTargetRectangle().GetWidth() -
                                     default_window_width * 1.25,
                                 main_viewport->WorkPos.y +
                                     ((float)g_presenter->GetTargetRectangle().GetHeight() * 0.05)),
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(default_window_width, default_window_height),
                           ImGuiCond_FirstUseEver);

  ImGui::Begin("Properties Panel");

  if (m_selected_targets.size() > 1)
  {
    ImGui::Text("Multiple objects not yet supported");
  }
  else if (m_selected_targets.size() == 1)
  {
    std::visit(
        overloaded{[&](const GraphicsModSystem::DrawCallID& drawcallid) {
                     DrawCallIDSelected(drawcallid);
                   },
                   [&](const GraphicsModSystem::TextureCacheID& tcache_id) {
                     TextureCacheIDSelected(tcache_id);
                   },
                   [&](const GraphicsModSystem::LightID& light_id) { LightSelected(light_id); },
                   [&](GraphicsModAction* action) { action->DrawImGui(); },
                   [&](EditorAsset* asset_data) { AssetDataSelected(asset_data); }},
        *m_selected_targets.begin());
  }
  ImGui::End();
}

void PropertiesPanel::DrawCallIDSelected(const GraphicsModSystem::DrawCallID& selected_object)
{
  const auto& data = m_state.m_runtime_data.m_draw_call_id_to_data[selected_object];
  auto& user_data = m_state.m_user_data.m_draw_call_id_to_user_data[selected_object];

  if (ImGui::BeginTable("DrawCallBasicForm", 2))
  {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("DisplayName");
    ImGui::TableNextColumn();
    ImGui::InputText("##FrameTargetDisplayName", &user_data.m_friendly_name);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("ID");
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", fmt::to_string(Common::ToUnderlying(selected_object)).c_str());

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Time Created");
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s",
                       fmt::format("{}", data.m_create_time.time_since_epoch().count()).c_str());

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Projection Type");
    ImGui::TableNextColumn();
    ImGui::Text("%s", fmt::format("{}", data.draw_data.projection_type).c_str());

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Cull Mode");
    ImGui::TableNextColumn();
    ImGui::Text("%s", fmt::format("{}", data.draw_data.rasterization_state.cullmode).c_str());

    ImGui::EndTable();
  }

  if (ImGui::CollapsingHeader("Blending", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (ImGui::BeginTable("DrawBlendingForm", 2))
    {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Blend enabled?");
      ImGui::TableNextColumn();
      if (data.draw_data.blending_state.blendenable)
        ImGui::Text("Yes");
      else
        ImGui::Text("No");

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Color update enabled?");
      ImGui::TableNextColumn();
      if (data.draw_data.blending_state.colorupdate)
        ImGui::Text("Yes");
      else
        ImGui::Text("No");

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Logicop update enabled?");
      ImGui::TableNextColumn();
      if (data.draw_data.blending_state.logicopenable)
        ImGui::Text("Yes");
      else
        ImGui::Text("No");

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Destination factor");
      ImGui::TableNextColumn();
      ImGui::Text("%s", fmt::to_string(data.draw_data.blending_state.dstfactor).c_str());

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Source factor");
      ImGui::TableNextColumn();
      ImGui::Text("%s", fmt::to_string(data.draw_data.blending_state.srcfactor).c_str());

      ImGui::EndTable();
    }
  }

  if (ImGui::CollapsingHeader("Textures", ImGuiTreeNodeFlags_DefaultOpen))
  {
    if (ImGui::BeginTable("DrawTexturesForm", 2))
    {
      for (const auto& texture : data.draw_data.textures)
      {
        const auto& texture_info =
            m_state.m_runtime_data.m_texture_cache_id_to_data[texture.hash_name];
        const auto& texture_view = texture_info.texture;
        if (texture_view.texture_data)
        {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::Text("Texture (%i)", texture_view.unit);
          ImGui::TableNextColumn();

          const float column_width = ImGui::GetContentRegionAvail().x;
          float image_width = texture_view.texture_data->GetWidth();
          float image_height = texture_view.texture_data->GetHeight();
          const float image_aspect_ratio = image_width / image_height;

          const float final_width = std::min(image_width * 4, column_width);
          image_width = final_width;
          image_height = final_width / image_aspect_ratio;

          if (texture_info.m_active)
          {
            ImGui::ImageButton(texture_view.hash_name.data(), texture_view.texture_data,
                               ImVec2{image_width, image_height});
            if (ImGui::BeginPopupContextItem())
            {
              if (ImGui::Selectable("Dump"))
              {
                VideoCommon::TextureUtils::DumpTexture(*texture_view.texture_data,
                                                       texture.hash_name, 0, false);
              }
              if (ImGui::Selectable("Copy hash"))
              {
                ImGui::SetClipboardText(texture_view.hash_name.data());
              }
              ImGui::EndPopup();
            }
            ImGui::Text("%dx%d", texture_view.texture_data->GetWidth(),
                        texture_view.texture_data->GetHeight());
          }
          else
          {
            ImGui::Text("<Texture unloaded>");
          }
        }
      }
      ImGui::EndTable();
    }
  }
}

void PropertiesPanel::TextureCacheIDSelected(
    const GraphicsModSystem::TextureCacheID& selected_object)
{
  const auto& data = m_state.m_runtime_data.m_texture_cache_id_to_data[selected_object];
  auto& user_data = m_state.m_user_data.m_texture_cache_id_to_user_data[selected_object];

  if (ImGui::BeginTable("TextureCacheTargetForm", 2))
  {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("DisplayName");
    ImGui::TableNextColumn();
    ImGui::InputText("##TextureCacheTargetDisplayName", &user_data.m_friendly_name);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("ID");
    ImGui::TableNextColumn();
    ImGui::Text("%s", data.m_id.c_str());

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Time Created");
    ImGui::TableNextColumn();
    ImGui::Text("%s", fmt::format("{}", data.m_create_time.time_since_epoch().count()).c_str());

    if (data.texture.texture_data)
    {
      const float column_width = ImGui::GetContentRegionAvail().x;
      float image_width = data.texture.texture_data->GetWidth();
      float image_height = data.texture.texture_data->GetHeight();
      const float image_aspect_ratio = image_width / image_height;

      image_width = column_width;
      image_height = column_width * image_aspect_ratio;

      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::Text("Texture");
      ImGui::TableNextColumn();
      ImGui::Image(data.texture.texture_data, ImVec2{image_width, image_height});
    }

    ImGui::EndTable();
  }
}

void PropertiesPanel::LightSelected(const GraphicsModSystem::LightID& selected_object)
{
  auto& data = m_state.m_runtime_data.m_light_id_to_data[selected_object];
  auto& user_data = m_state.m_user_data.m_light_id_to_user_data[selected_object];

  if (ImGui::BeginTable("LightTargetForm", 2))
  {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("DisplayName");
    ImGui::TableNextColumn();
    ImGui::InputText("##LightTargetDisplayName", &user_data.m_friendly_name);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("ID");
    ImGui::TableNextColumn();
    ImGui::Text("%s", fmt::to_string(Common::ToUnderlying(selected_object)).c_str());

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Time Created");
    ImGui::TableNextColumn();
    ImGui::Text("%s", fmt::format("{}", data.m_create_time.time_since_epoch().count()).c_str());

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Color");
    ImGui::TableNextColumn();
    ImGui::InputInt4("##LightColor", data.m_color.data(), ImGuiInputTextFlags_ReadOnly);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Position");
    ImGui::TableNextColumn();
    ImGui::InputFloat4("##LightPosition", data.m_pos.data(), "%.3f", ImGuiInputTextFlags_ReadOnly);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Direction");
    ImGui::TableNextColumn();
    ImGui::InputFloat4("##LightDirection", data.m_dir.data(), "%.3f", ImGuiInputTextFlags_ReadOnly);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Distance Attenutation");
    ImGui::TableNextColumn();
    ImGui::InputFloat4("##LightDistAtt", data.m_distatt.data(), "%.3f",
                       ImGuiInputTextFlags_ReadOnly);

    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::Text("Cosine Attenutation");
    ImGui::TableNextColumn();
    ImGui::InputFloat4("##LightCosAtt", data.m_cosatt.data(), "%.3f", ImGuiInputTextFlags_ReadOnly);

    ImGui::EndTable();
  }
}

void PropertiesPanel::AssetDataSelected(EditorAsset* selected_object)
{
  std::visit(
      overloaded{[&](const std::unique_ptr<VideoCommon::MaterialData>& material_data) {
                   m_material_control.DrawImGui(selected_object->m_asset_id, material_data.get(),
                                                &selected_object->m_last_data_write,
                                                &selected_object->m_valid);
                 },
                 [&](const std::unique_ptr<VideoCommon::PixelShaderData>& pixel_shader_data) {
                   m_shader_control.DrawImGui(selected_object->m_asset_id, pixel_shader_data.get(),
                                              &selected_object->m_last_data_write);
                 },
                 [&](const std::unique_ptr<VideoCommon::TextureData>& texture_data) {
                   auto asset_preview = m_state.m_user_data.m_asset_library->GetAssetPreview(
                       selected_object->m_asset_id);
                   m_texture_control.DrawImGui(selected_object->m_asset_id, texture_data.get(),
                                               selected_object->m_asset_path,
                                               &selected_object->m_last_data_write, asset_preview);
                 },
                 [&](const std::unique_ptr<VideoCommon::MeshData>& mesh_data) {
                   auto asset_preview = m_state.m_user_data.m_asset_library->GetAssetPreview(
                       selected_object->m_asset_id);
                   m_mesh_control.DrawImGui(selected_object->m_asset_id, mesh_data.get(),
                                            selected_object->m_asset_path,
                                            &selected_object->m_last_data_write, asset_preview);
                 }},
      selected_object->m_data);
}

void PropertiesPanel::SelectionOccurred(const std::set<SelectableType>& selection)
{
  m_selected_targets = selection;
}
}  // namespace GraphicsModEditor::Panels