// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "VideoCommon/AbstractTexture.h"
#include "VideoCommon/PEShaderSystem/Runtime/PEShaderInput.h"

class ShaderCode;

namespace VideoCommon::PE
{
struct BaseShaderPass
{
  std::vector<std::unique_ptr<ShaderInput>> m_inputs;

  std::unique_ptr<AbstractTexture> m_output_texture;
  float m_output_scale;

  void WriteShaderIndices(ShaderCode& shader_source) const;
};
}  // namespace VideoCommon::PE
