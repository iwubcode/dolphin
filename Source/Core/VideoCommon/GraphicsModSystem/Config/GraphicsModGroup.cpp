// Copyright 2022 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Config/GraphicsModGroup.h"

#include <map>
#include <sstream>
#include <string>

#include <picojson.h>
#include <xxhash.h>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/StringUtil.h"
#include "Core/ConfigManager.h"

#include "VideoCommon/GraphicsModSystem/Constants.h"
#include "VideoCommon/HiresTextures.h"

namespace GraphicsModSystem::Config
{
GraphicsModGroup::GraphicsModGroup(std::string game_id) : m_game_id(std::move(game_id))
{
}

void GraphicsModGroup::Load()
{
  const auto try_add_mod = [&](const std::string& dir) {
    auto file = dir + DIR_SEP + "metadata.json";
    UnifyPathSeparators(file);

    if (auto mod = GraphicsMod::Create(file))
    {
      std::string file_data;
      if (!File::ReadFileToString(file, file_data))
        return;
      GraphicsModGroup::GraphicsModWithMetadata mod_with_metadata;
      mod_with_metadata.m_path = dir;
      mod_with_metadata.m_mod = std::move(*mod);
      mod_with_metadata.m_id = XXH3_64bits(file_data.data(), file_data.size());
      m_graphics_mods.push_back(std::move(mod_with_metadata));
    }
  };

  const std::set<std::string> graphics_mod_user_directories =
      GetTextureDirectoriesWithGameId(File::GetUserPath(D_GRAPHICSMOD_IDX), m_game_id);

  for (const auto& graphics_mod_directory : graphics_mod_user_directories)
  {
    try_add_mod(graphics_mod_directory);
  }

  const std::set<std::string> graphics_mod_system_directories = GetTextureDirectoriesWithGameId(
      File::GetSysDirectory() + DOLPHIN_SYSTEM_GRAPHICS_MOD_DIR, m_game_id);

  for (const auto& graphics_mod_directory : graphics_mod_system_directories)
  {
    try_add_mod(graphics_mod_directory);
  }

  for (auto& mod : m_graphics_mods)
  {
    m_id_to_graphics_mod[mod.m_id] = &mod;
  }

  const auto gameid_metadata = GetPath();
  if (File::Exists(gameid_metadata))
  {
    std::string json_data;
    if (!File::ReadFileToString(gameid_metadata, json_data))
    {
      ERROR_LOG_FMT(VIDEO, "Failed to load graphics mod group json file '{}'", gameid_metadata);
      return;
    }

    picojson::value root;
    const auto error = picojson::parse(root, json_data);

    if (!error.empty())
    {
      ERROR_LOG_FMT(VIDEO,
                    "Failed to load graphics mod group json file '{}' due to parse error: {}",
                    gameid_metadata, error);
      return;
    }
    if (!root.is<picojson::object>())
    {
      ERROR_LOG_FMT(
          VIDEO,
          "Failed to load graphics mod group json file '{}' due to root not being an object!",
          gameid_metadata);
      return;
    }

    const auto& mods = root.get("mods");
    if (mods.is<picojson::array>())
    {
      for (const auto& mod_json : mods.get<picojson::array>())
      {
        if (mod_json.is<picojson::object>())
        {
          const auto& mod_json_obj = mod_json.get<picojson::object>();
          u64 id = ReadNumericOrDefault<u64>(mod_json_obj, "id");
          if (const auto iter = m_id_to_graphics_mod.find(id); iter != m_id_to_graphics_mod.end())
          {
            iter->second->m_weight = ReadNumericOrDefault<u16>(mod_json_obj, "weight");
            iter->second->m_enabled = ReadBoolOrDefault(mod_json_obj, "enabled");
          }
        }
      }
    }
  }

  std::sort(m_graphics_mods.begin(), m_graphics_mods.end(),
            [](const auto& lhs, const auto& rhs) { return lhs.m_weight < rhs.m_weight; });

  m_change_count++;
}

void GraphicsModGroup::Save() const
{
  const std::string file_path = GetPath();
  std::ofstream json_stream;
  File::OpenFStream(json_stream, file_path, std::ios_base::out);
  if (!json_stream.is_open())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to open graphics mod group json file '{}' for writing", file_path);
    return;
  }

  picojson::object serialized_root;
  picojson::array serialized_mods;
  for (const auto& [id, mod_ptr] : m_id_to_graphics_mod)
  {
    picojson::object serialized_mod;
    serialized_mod.emplace("id", static_cast<double>(id));
    serialized_mod.emplace("enabled", mod_ptr->m_enabled);
    serialized_mod.emplace("weight", static_cast<double>(mod_ptr->m_weight));
    serialized_mods.emplace_back(std::move(serialized_mod));
  }
  serialized_root.emplace("mods", std::move(serialized_mods));

  const auto output = picojson::value{serialized_root}.serialize(true);
  json_stream << output;
}

void GraphicsModGroup::SetChangeCount(u32 change_count)
{
  m_change_count = change_count;
}

u32 GraphicsModGroup::GetChangeCount() const
{
  return m_change_count;
}

const std::vector<GraphicsModGroup::GraphicsModWithMetadata>& GraphicsModGroup::GetMods() const
{
  return m_graphics_mods;
}

std::vector<GraphicsModGroup::GraphicsModWithMetadata>& GraphicsModGroup::GetMods()
{
  return m_graphics_mods;
}

GraphicsModGroup::GraphicsModWithMetadata* GraphicsModGroup::GetMod(u64 id) const
{
  if (const auto iter = m_id_to_graphics_mod.find(id); iter != m_id_to_graphics_mod.end())
  {
    return iter->second;
  }

  return nullptr;
}

const std::string& GraphicsModGroup::GetGameID() const
{
  return m_game_id;
}

std::string GraphicsModGroup::GetPath() const
{
  const std::string game_mod_root = File::GetUserPath(D_CONFIG_IDX) + GRAPHICSMOD_CONFIG_DIR;
  return fmt::format("{}/{}.json", game_mod_root, m_game_id);
}
}  // namespace GraphicsModSystem::Config
