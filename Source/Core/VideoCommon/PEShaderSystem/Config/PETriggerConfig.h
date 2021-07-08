// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <map>
#include <string>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/PEShaderSystem/Config/PEShaderConfigGroup.h"
#include "VideoCommon/PEShaderSystem/Constants.h"

namespace VideoCommon::PE
{
struct TriggerConfig
{
  std::map<std::string, ShaderConfigGroup> m_trigger_name_to_shader_groups;
  std::string m_chosen_trigger_point = Constants::default_trigger;
  u64 m_changes = 0;

  void LoadFromProfile(const std::string& file_path);
  void SaveToProfile(const std::string& file_path) const;
};
}  // namespace VideoCommon::PE
