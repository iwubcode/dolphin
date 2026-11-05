// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <vector>

#include "VideoCommon/GraphicsModSystem/Runtime/PresentedFrame.h"

namespace GraphicsModSystem::Runtime
{
/// Fixed-capacity ring of presented frames (UnifiedReplayDesign §3). Phase 2 capacity = 1.
/// Push overwrites the oldest slot. The only retention type past the frame boundary.
class PresentedFrameRing
{
public:
  explicit PresentedFrameRing(std::size_t capacity = 1);

  void Push(PresentedFrame&& frame);     // move-in, overwrite oldest
  const PresentedFrame* Latest() const;  // nullptr when empty
  void Clear();                          // savestate load (see §7)
  std::size_t Size() const;

private:
  std::vector<PresentedFrame> m_slots;  // sized to capacity at construction
  std::size_t m_next = 0;
  std::size_t m_size = 0;
};
}  // namespace GraphicsModSystem::Runtime
