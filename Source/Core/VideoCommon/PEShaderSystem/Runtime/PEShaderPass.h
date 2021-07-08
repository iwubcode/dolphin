// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "VideoCommon/PEShaderSystem/Runtime/PEBaseShaderPass.h"

#include <memory>

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/AbstractShader.h"

namespace VideoCommon::PE
{
struct ShaderPass final : public BaseShaderPass
{
  std::shared_ptr<AbstractShader> m_vertex_shader;
  std::unique_ptr<AbstractShader> m_pixel_shader;
  std::unique_ptr<AbstractPipeline> m_pipeline;
  std::unique_ptr<AbstractFramebuffer> m_output_framebuffer;
};
}  // namespace VideoCommon::PE
