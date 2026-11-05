// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/HookableEvent.h"

#include "VideoCommon/GraphicsModSystem/Runtime/PresentedFrame.h"

namespace GraphicsModSystem::Runtime
{
/// Global event holder, GetEditorEvents()/GetVideoEvents() convention — a plain struct
/// plus accessor, no publisher class. The director fires it directly; the panel reaches
/// it without traversing the manager/handler graph (which swaps on settings changes).
/// presented_frame_published_event fires on the GPU emulation thread from
/// XFBDirector::OnFramePresented, immediately after m_ring.Push — the presented-frame
/// pointer is the ring slot, valid until the next push overwrites it or
/// ClearPresentedFrames() runs (same lifetime rules as Ring::Latest()). v1 keeps ring
/// depth 1: subscribers copy what they need (frame_number, op counts, per-op display
/// metadata) inside the event callback and never store the pointer.
struct PresentedFrameEvents
{
  Common::HookableEvent<const PresentedFrame&> presented_frame_published_event;
};

PresentedFrameEvents& GetPresentedFrameEvents();
}  // namespace GraphicsModSystem::Runtime
