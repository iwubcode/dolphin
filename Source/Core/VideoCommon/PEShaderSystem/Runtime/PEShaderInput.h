// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>

#include "Common/CommonTypes.h"

class AbstractTexture;

namespace VideoCommon::PE
{
  struct ShaderInput
  {
    InputType type;
    u32 texture_unit;
    SamplerState sampler_state;
    const AbstractTexture* texture_ptr;
    std::unique_ptr<AbstractTexture> owned_texture_ptr;
    u32 source_pass_index;
  };
}
