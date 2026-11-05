// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModSystem/Runtime/PresentedFrame.h"

#include "Common/Assert.h"

namespace GraphicsModSystem::Runtime
{
namespace
{
u64 s_vertex_lifetime_counter = 0;
}  // namespace

u64 GetVertexLifetimeCounter()
{
  return s_vertex_lifetime_counter;
}

void BumpVertexLifetimeCounter()
{
  ++s_vertex_lifetime_counter;
}

std::size_t PresentedFrame::TotalOperations() const
{
  std::size_t total = 0;
  for (const XFBCopy& copy : copies)
    total += copy.GetView().operations.size();
  return total;
}

PresentedFrame::OperationIndex PresentedFrame::FindOperation(std::size_t global_operation_index) const
{
  // P3-f: global index must be within the flattened op space
  ASSERT(global_operation_index < TotalOperations());

  std::size_t running = 0;
  for (std::size_t copy_index = 0; copy_index < copies.size(); ++copy_index)
  {
    const std::size_t op_count = copies[copy_index].GetView().operations.size();
    if (global_operation_index < running + op_count)
      return {copy_index, global_operation_index - running + 1};
    running += op_count;
  }
  // Unreachable: the assert above guarantees a containing copy exists.
  return {0, 0};
}
}  // namespace GraphicsModSystem::Runtime
