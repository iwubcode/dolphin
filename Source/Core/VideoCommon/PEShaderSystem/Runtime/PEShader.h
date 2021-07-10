// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include "VideoCommon/PEShaderSystem/Config/PEShaderConfig.h"
#include "VideoCommon/PEShaderSystem/Runtime/PEShaderApplyOptions.h"
#include "VideoCommon/PEShaderSystem/Runtime/PEShaderPass.h"

class AbstractShader;

namespace VideoCommon::PE
{
  class ShaderOption;
  class Shader
  {
  public:
	bool CreateOptions(const ShaderConfig& config);
	bool CreatePasses(const ShaderConfig& config);
	bool PrepareOutputTextures(u32 width, u32 height, u32 layers, AbstractTextureFormat format);
	void LinkPasses();
	void Apply(bool skip_final_copy, const ShaderApplyOptions& options);
  private:
    void UploadUniforms();
	std::unique_ptr<AbstractShader> CompileGeometryShader() const;
	void PrepareUniformHeader(fmt::memory_buffer& shader_source) const;
	std::shared_ptr<AbstractShader> CompileVertexShader(const ShaderConfig& config) const;
    std::vector<u8> m_uniform_staging_buffer;
    std::vector<ShaderPass> m_passes;
    std::vector<std::unique_ptr<ShaderOption>> m_options;
  };
}