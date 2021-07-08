// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/PEShaderSystem/Runtime/PEShaderInput.h"

class AbstractFramebuffer;
class AbstractPipeline;
class AbstractShader;
class AbstractTexture;

namespace VideoCommon::PE
{
  struct ShaderPass
  {
    std::shared_ptr<AbstractShader> vertex_shader;
    std::unique_ptr<AbstractShader> pixel_shader;
    std::unique_ptr<AbstractPipeline> pipeline;
    std::vector<ShaderInput> inputs;

    std::unique_ptr<AbstractTexture> output_texture;
    std::unique_ptr<AbstractFramebuffer> output_framebuffer;
    float output_scale;

    u32 shader_index;
    u32 shader_pass_index;

    void CreatePipeline();
  };
  
  std::string GetPixelShaderFooter(const ShaderPass& pass);
}