// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "Common/HookableEvent.h"

#include "VideoCommon/FramebufferManager.h"

class EFBInterfaceBase;
class IGraphicsHandler;

class EmulatedGraphicsManager
{
public:
  EmulatedGraphicsManager();
  ~EmulatedGraphicsManager();

  void Initialize();
  void Shutdown();

  EFBInterfaceBase* GetEFBInterface() const;
  IGraphicsHandler* GetGraphicsHandler() const;

private:
  void SettingsChanged();

  std::unique_ptr<IGraphicsHandler> m_deferred_native_graphics_handler;
  std::unique_ptr<IGraphicsHandler> m_deferred_graphics_handler;
  std::unique_ptr<IGraphicsHandler> m_normal_graphics_handler;
  std::unique_ptr<IGraphicsHandler> m_normal_graphicsmod_graphics_handler;
  std::unique_ptr<FramebufferManager> m_native_framebuffer_manager;

  IGraphicsHandler* m_graphics_handler;

  Common::EventHook m_config_changed;
};
