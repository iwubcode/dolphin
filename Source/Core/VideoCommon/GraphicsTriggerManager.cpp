// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/GraphicsTriggerManager.h"

#include "Common/FileSearch.h"
#include "Common/FileUtil.h"

const GraphicsTrigger* GraphicsTriggerManager::GetTrigger(const std::string& name) const
{
  if (auto iter = m_user_triggers.find(name); iter != m_user_triggers.end())
  {
    return &iter->second;
  }

  if (auto iter = m_internal_triggers.find(name); iter != m_internal_triggers.end())
  {
    return &iter->second;
  }

  return nullptr;
}

bool GraphicsTriggerManager::Load()
{
  m_user_triggers.clear();
  m_internal_triggers.clear();

  const std::string profiles_path = File::GetUserPath(D_CONFIG_IDX) + directory_path;
  for (const auto& filename : Common::DoFileSearch({profiles_path}, {".ini"}))
  {
    GraphicsTrigger trigger;
    if (!LoadFromFile(trigger, filename))
      return false;

    std::string basename;
    SplitPath(filename, nullptr, &basename, nullptr);
    m_user_triggers.insert_or_assign(basename, trigger);
  }

  const std::string builtin_profiles_path = File::GetSysDirectory() + directory_path;
  for (const auto& filename : Common::DoFileSearch({builtin_profiles_path}, {".ini"}))
  {
    GraphicsTrigger trigger;
    if (!LoadFromFile(trigger, filename))
      return false;

    std::string basename;
    SplitPath(filename, nullptr, &basename, nullptr);
    m_internal_triggers.insert_or_assign(basename, trigger);
  }

  return true;
}
