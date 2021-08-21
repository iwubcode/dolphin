// Copyright 2021 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <map>
#include <string>

#include "Common/FileUtil.h"
#include "VideoCommon/GraphicsTrigger.h"

class GraphicsTriggerManager
{
public:
  const GraphicsTrigger* GetTrigger(const std::string& name) const;

  inline static const std::string directory_path = "Profiles/GraphicsTrigger";
  
  bool Load();
private:
  std::map<std::string, GraphicsTrigger> m_user_triggers;
  std::map<std::string, GraphicsTrigger> m_internal_triggers;
};
