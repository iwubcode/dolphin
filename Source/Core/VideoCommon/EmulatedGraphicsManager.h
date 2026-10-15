// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <optional>

#include "Common/CommonTypes.h"
#include "Common/HookableEvent.h"

#include "VideoCommon/FramebufferManager.h"

class DeferredGraphicsHandler;
class EFBInterfaceBase;
class IGraphicsHandler;

class EmulatedGraphicsManager
{
public:
  EmulatedGraphicsManager();
  ~EmulatedGraphicsManager();

  void Initialize();
  void Shutdown();

  FramebufferManager* GetPeekPokeFramebuffer() const;
  IGraphicsHandler* GetGraphicsHandler() const;

  /// True when the active graphics handler is a deferred handler (Phase 3 scrubber
  /// gating: scrub controls are enabled only while paused AND deferred).
  bool IsDeferredActive() const { return m_active_deferred != nullptr; }

  /// Replays the last presented frame (paused FreeLook). No-op unless a deferred
  /// handler is active (D1: typed pointer, no interface change).
  void ReplayPresentedFrame();

  /// Scrub request (Phase 3): replay the presented frame up to global op k
  /// (flattened multi-copy prefix). nullopt = full-frame replay (un-scrub).
  /// Forwards through the existing typed m_active_deferred (Phase 2 D1); null in
  /// normal modes → no-op.
  void RequestScrubPrefix(std::optional<std::size_t> global_op);

  /// Clears the presented-frame ring and pending copies (savestate load while paused).
  /// No-op unless deferred.
  void ClearPresentedFrames();

private:
  void SettingsChanged();

  std::unique_ptr<IGraphicsHandler> m_deferred_native_graphics_handler;
  std::unique_ptr<IGraphicsHandler> m_deferred_graphics_handler;
  std::unique_ptr<IGraphicsHandler> m_normal_graphics_handler;
  std::unique_ptr<IGraphicsHandler> m_normal_graphicsmod_graphics_handler;
  std::unique_ptr<FramebufferManager> m_native_framebuffer_manager;

  IGraphicsHandler* m_graphics_handler;
  // D1: typed pointer to the active deferred handler (null in normal modes).
  // Set alongside m_graphics_handler in SettingsChanged().
  DeferredGraphicsHandler* m_active_deferred = nullptr;
  FramebufferManager* m_peek_poke_framebuffer_manager;

  Common::EventHook m_config_changed;
};
