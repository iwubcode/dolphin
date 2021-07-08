// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/PEShaderSystem/Config/PEShaderConfigGroup.h"
#include "VideoCommon/PEShaderSystem/Runtime/PEShader.h"
#include "VideoCommon/PEShaderSystem/Runtime/PEShaderApplyOptions.h"

namespace VideoCommon::PE
{
  class ShaderGroup
  {
  public:
    void Apply(const PEShaderConfigGroup& group, const ShaderApplyOptions& options);
  private:
    void CreateShaders(const PEShaderConfigGroup& group);
    u32 m_last_change_count = 0;
	std::vector<Shader> m_shaders;
  };
}