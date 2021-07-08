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
    std::vector<ShaderConfigInput> inputs;
    std::string entry_point;
    std::string dependent_option;
  
    float output_scale = 1.0f;

    void SerializeToJSON(picojson::object& obj) const;
    void DeserializeFromJSON(picojson::object& obj);
  
    static PEShaderConfigPass create_default_pass(InitialSource source);
  };
}