// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <picojson.h>

#include "VideoCommon/PEShaderSystem/Config/PEShaderConfigGroup.h"

namespace VideoCommon::PE
{
  enum class TriggerType
  {
    EFB,
    Draw2D,
    Draw3D,
    Post
  };

  struct TriggerPoint
  {
    ShaderConfigGroup group;
    TriggerType type;

    void SerializeToJSON(picojson::object& obj) const;
    void DeserializeFromJSON(picojson::object& obj);
  };
}
