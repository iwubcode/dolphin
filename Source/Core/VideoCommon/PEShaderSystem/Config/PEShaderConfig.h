// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <picojson.h>

#include "VideoCommon/PEShaderSystem/Config/PEShaderConfigOption.h"
#include "VideoCommon/PEShaderSystem/Config/PEShaderConfigPass.h"

namespace VideoCommon::PE
{
  // Note: this class can be accessed by both
  // the video thread and the UI
  // thread
  class RuntimeInfo
  {
  public:
    bool HasError() const;
    void SetError(bool error);

  private:
    mutable std::mutex m_error_mutex;
    bool m_has_error = false;
  };

  struct ShaderConfig
  {
    std::string m_name;
    std::string m_author;
    std::string m_source_path;
    std::vector<ShaderConfigOption> m_options;
    std::vector<ShaderConfigPass> m_passes;
    bool m_enabled = true;
    bool m_requires_depth_buffer = false;
    std::shared_ptr<RuntimeInfo> m_runtime_info;
  
    bool IsValid() const;
    void Reset();
    void Reload();

    void SerializeToJSON(picojson::object& obj) const;
    void DeserializeFromJSON(picojson::object& obj);
  };
}
