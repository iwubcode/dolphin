// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>

#include "VideoCommon/AbstractShader.h"
#include "VideoCommon/PEShaderSystem/Runtime/PEBaseShaderPass.h"
#include "VideoCommon/PEShaderSystem/Runtime/PEShaderInput.h"

namespace VideoCommon::PE
{
struct ComputeShaderPass final : public BaseShaderPass
{
  std::unique_ptr<AbstractShader> m_compute_shader;
};
}  // namespace VideoCommon::PE
