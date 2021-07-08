// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <vector>

#include <picojson.h>

#include "Common/CommonTypes.h"
#include "VideoCommon/PEShaderSystem/Config/PEShaderConfig.h"

namespace VideoCommon::PE
{
  struct ShaderConfigGroup
  {
    std::vector<ShaderConfig> m_shaders;
    std::vector<u32> m_shader_order;
    u32 m_changes;

    void SerializeToJSON(picojson::object& obj) const;
    void DeserializeFromJSON(picojson::object& obj);
  };
}