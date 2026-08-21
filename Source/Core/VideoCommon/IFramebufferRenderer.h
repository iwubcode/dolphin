// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>

#include "VideoCommon/PixelShaderManager.h"

namespace EmulatedGraphics
{
struct FrameBufferCopyData;
struct ClearCopyData;
struct ScissorsAndViewportData;
}  // namespace EmulatedGraphics

struct TCacheEntry;

class IFramebufferRenderer
{
public:
  virtual ~IFramebufferRenderer() = default;

  // Main entry point for framebuffer copy. Creates and returns a new cache entry
  // representing the framebuffer
  virtual std::shared_ptr<TCacheEntry>
  RenderFramebufferCopy(PixelShaderManager& pixel_shader_manager,
                        EmulatedGraphics::FrameBufferCopyData copy_data) = 0;

  // Uninitialize memory when a framebuffer copy is skipped.
  virtual void
  UninitializeFramebufferMemory(const EmulatedGraphics::FrameBufferCopyData& copy_data) = 0;
};
