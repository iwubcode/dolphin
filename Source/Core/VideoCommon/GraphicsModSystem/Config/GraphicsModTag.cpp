// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Config/GraphicsModTag.h"

#include "Common/Logging/Log.h"

namespace GraphicsModSystem::Config
{
void GraphicsModTag::Serialize(picojson::object& json_obj) const
{
  json_obj.emplace("name", m_name);
  json_obj.emplace("description", m_description);
}

bool GraphicsModTag::Deserialize(const picojson::object& obj)
{
  auto name_iter = obj.find("name");
  if (name_iter == obj.end())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, specified tag has no name");
    return false;
  }
  if (!name_iter->second.is<std::string>())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, specified tag has a name "
                         "that is not a string");
    return false;
  }
  m_name = name_iter->second.to_str();

  auto description_iter = obj.find("description");
  if (description_iter == obj.end())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, specified tag has no description");
    return false;
  }
  if (!description_iter->second.is<std::string>())
  {
    ERROR_LOG_FMT(VIDEO, "Failed to load mod configuration file, specified tag has a description "
                         "that is not a string");
    return false;
  }
  m_description = description_iter->second.to_str();

  return true;
}
}  // namespace GraphicsModSystem::Config
