// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>

#include "Common/CommonTypes.h"
#include "VideoCommon/GraphicsModSystem/Types.h"
#include "VideoCommon/VertexManagerBase.h"
#include "VideoCommon/VideoEvents.h"

class GraphicsModAction;
namespace GraphicsModSystem::Runtime
{
class GraphicsModBackend
{
public:
  virtual ~GraphicsModBackend() = default;

  virtual void OnDraw(const DrawData& draw_started, VertexManagerBase* vertex_manager) = 0;
  virtual void OnTextureLoad(const Texture& texture) = 0;
  virtual void OnTextureCreate(const Texture& texture) = 0;
  virtual void OnLight() = 0;
  virtual void OnFramePresented(const PresentInfo& present_info) = 0;

protected:
  void CustomDraw(const DrawData& draw_data, VertexManagerBase* vertex_manager,
                  std::span<GraphicsModAction*> actions);
};
}  // namespace GraphicsModSystem::Runtime
