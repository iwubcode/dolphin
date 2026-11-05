// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/PresentedFrameRing.h"

#include "Common/Assert.h"

namespace GraphicsModSystem::Runtime
{
PresentedFrameRing::PresentedFrameRing(std::size_t capacity) : m_slots(capacity)
{
  ASSERT(capacity > 0);
}

void PresentedFrameRing::Push(PresentedFrame&& frame)
{
  // PresentedFrame is non-assignable (end_sv's ScissorPos union has deleted
  // copy-assignment), so replace the slot in place.
  m_slots[m_next].~PresentedFrame();
  new (&m_slots[m_next]) PresentedFrame(std::move(frame));
  m_next = (m_next + 1) % m_slots.size();
  if (m_size < m_slots.size())
    ++m_size;
}

const PresentedFrame* PresentedFrameRing::Latest() const
{
  if (m_size == 0)
    return nullptr;
  const std::size_t index = (m_next + m_slots.size() - 1) % m_slots.size();
  return &m_slots[index];
}

void PresentedFrameRing::Clear()
{
  m_next = 0;
  m_size = 0;
}

std::size_t PresentedFrameRing::Size() const
{
  return m_size;
}
}  // namespace GraphicsModSystem::Runtime
