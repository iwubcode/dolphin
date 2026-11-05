// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/PresentedFrameEvents.h"

namespace GraphicsModSystem::Runtime
{
PresentedFrameEvents& GetPresentedFrameEvents()
{
  static PresentedFrameEvents s_events;
  return s_events;
}
}  // namespace GraphicsModSystem::Runtime
