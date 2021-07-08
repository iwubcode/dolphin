// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "VideoCommon/PEShaderSystem/Runtime/PEShaderPass.h"

#include <fmt/format.h>

#include "VideoCommon/VideoConfig.h"

namespace VideoCommon::PE
{
  std::string GetPixelShaderFooter(const ShaderPass& pass)
  {
    fmt::memory_buffer buffer;
    if (g_ActiveConfig.backend_info.api_type == APIType::D3D)
    {
      fmt::format_to(buffer, "#undef main\n");
      fmt::format_to(buffer, "void main(in float3 v_tex0_ : TEXCOORD0,\n");
                             "\tin float3 v_tex1_ : TEXCOORD1,\n"
                             "\tin float4 pos : SV_Position,\n"
                             "\tout float4 ocol0_ : SV_Target)\n");
      fmt::format_to(buffer, "{\n");
      fmt::format_to(buffer, "\tv_tex0 = v_tex0_;\n");
      fmt::format_to(buffer, "\tv_tex1 = v_tex1_;\n");
      fmt::format_to(buffer, "\tv_fragcoord = pos;\n");
      
      if (pass.entry_point == "main")
      {
        fmt::format_to(buffer, "\treal_main();\n");
      }
      else if (pass.entry_point.empty())
      {
        // No entry point should create a copy
        fmt::format_to(buffer, "\tocol0 = SampleInput(0);\n");
      }
      else
      {
        fmt::format_to(buffer, "\t{}(0);\n", pass.entry_point);
      }
      fmt::format_to(buffer, "\tocol0_ = ocol0;\n");
      fmt::format_to(buffer, "}\n");
    }
    else
    {
      if (pass.entry_point.empty())
      {
        // No entry point should create a copy
        fmt::format_to(buffer, "void main() { ocol0 = SampleInput(0);\n }\n");
      }
      else if (pass.entry_point != "main")
      {
        fmt::format_to(buffer, "void main()\n");
        fmt::format_to(buffer, "{\n");
        fmt::format_to(buffer, "\t{}();\n", pass.entry_point);
        fmt::format_to(buffer, "}\n");
      }
    }
  }
  return fmt::to_string(buffer);
}
