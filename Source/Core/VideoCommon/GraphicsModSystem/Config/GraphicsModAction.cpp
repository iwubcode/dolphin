// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Config/GraphicsModAction.h"

#include "Common/Logging/Log.h"

namespace GraphicsModSystem::Config
{
void GraphicsModAction::Serialize(picojson::object& json_obj) const
{
  json_obj.emplace("factory_name", m_factory_name);
  json_obj.emplace("data", m_data);
}

bool GraphicsModAction::Deserialize(const picojson::object& obj)
{
  if (auto factory_name_iter = obj.find("factory_name"); factory_name_iter != obj.end())
  {
    if (!factory_name_iter->second.is<std::string>())
    {
      ERROR_LOG_FMT(
          VIDEO,
          "Failed to load mod configuration file, specified action's factory_name is not a string");
      return false;
    }
    m_factory_name = factory_name_iter->second.get<std::string>();
  }

  if (auto data_iter = obj.find("data"); data_iter != obj.end())
  {
    m_data = data_iter->second;
  }

  return true;
}
}  // namespace GraphicsModSystem::Config
