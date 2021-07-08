// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <string>
#include <vector>

#include <picojson.h>

#include "VideoCommon/PEShaderSystem/Config/PEShaderConfigInput.h"

namespace VideoCommon::PE
{
struct ShaderConfigPass
{
  std::vector<ShaderConfigInput> m_inputs;
  std::string m_entry_point;
  std::string m_dependent_option;

  float m_output_scale = 1.0f;

  bool DeserializeFromConfig(const picojson::object& obj, std::size_t pass_index, std::size_t total_passes);
  void SerializeToProfile(picojson::object& obj) const;
  void DeserializeFromProfile(const picojson::object& obj);

  static ShaderConfigPass CreateDefaultPass();
};
}  // namespace VideoCommon::PE
