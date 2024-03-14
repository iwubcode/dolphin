// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"

#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"
#include "VideoCommon/GraphicsModSystem/Config/GraphicsModHashPolicy.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModAction.h"
#include "VideoCommon/GraphicsModSystem/Runtime/GraphicsModBackend.h"
#include "VideoCommon/GraphicsModSystem/Types.h"

namespace GraphicsModSystem::Runtime
{
class GraphicsModRuntimeBackend final : public GraphicsModBackend
{
public:
  explicit GraphicsModRuntimeBackend(const Config::GraphicsModGroup& config_data);
  void OnDraw(const DrawData& draw_data, VertexManagerBase* vertex_manager) override;
  void OnTextureLoad(const Texture& texture) override;
  void OnTextureCreate(const Texture& texture) override;
  void OnLight() override;
  void OnFramePresented(const PresentInfo& present_info) override;

private:
  struct GraphicsMod
  {
    Config::HashPolicy m_hash_policy;
    std::unordered_map<DrawCallID, std::vector<GraphicsModAction*>> m_draw_id_to_actions;

    struct string_hash
    {
      using is_transparent = void;
      [[nodiscard]] size_t operator()(const char* txt) const
      {
        return std::hash<std::string_view>{}(txt);
      }
      [[nodiscard]] size_t operator()(std::string_view txt) const
      {
        return std::hash<std::string_view>{}(txt);
      }
      [[nodiscard]] size_t operator()(const std::string& txt) const
      {
        return std::hash<std::string>{}(txt);
      }
    };

    std::unordered_map<std::string, std::vector<GraphicsModAction*>, string_hash, std::equal_to<>>
        m_str_id_to_actions;
    std::vector<std::unique_ptr<GraphicsModAction>> m_actions;
  };
  std::vector<GraphicsMod> m_mods;
};
}  // namespace GraphicsModSystem::Runtime