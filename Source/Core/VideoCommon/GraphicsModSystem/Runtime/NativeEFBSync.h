// Copyright 2025 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "VideoCommon/FramebufferManager.h"
#include "VideoCommon/GraphicsModSystem/Runtime/XFBCopyData.h"
#include "VideoCommon/IGraphicsHandler.h"
#include "VideoCommon/TextureCacheBase.h"

/// Keeps the game-facing (native) EFB in sync with recorded operations.
///
/// The game reads from this surface for RAM copies and CPU peek; it must
/// reflect all draws/state up to a given point in the frame. This component
/// absorbs the native-EBF tracking, deferred copy batching, and replay logic
/// that was previously scattered across DeferredGraphicsHandler.
class NativeEFBSync
{
public:
  explicit NativeEFBSync(FramebufferManager* fbm);

  // Render all ops in [m_synced_up_to, target_op_index) into the native EFB.
  void SyncTo(const XFBCopyData::View& view, const std::vector<XFBCopyData::UniformState>& uniforms,
              std::size_t target_op_index);

  // Immediate state application (clears happen at record time).
  void OnClear(EmulatedGraphics::ClearCopyData data, EmulatedGraphics::ScissorsAndViewportData sv,
               bool is_xfb);

  // Register a deferred native EFB copy. The copy will be rendered in a batch
  // when FlushCopy is called for the matching cache entry.
  void DeferCopy(TCacheEntry* entry, std::size_t op_index);

  // Flush a deferred native EFB copy: render the ops up to the copy's op index,
  // then complete the RAM copy and flush the texture cache entry.
  void FlushCopy(const XFBCopyData::View& view,
                 const std::vector<XFBCopyData::UniformState>& uniforms,
                 const std::shared_ptr<TCacheEntry>& cache_entry);

  // Reset per-frame state (called at the end of ExecuteXFBOperations).
  void Reset();

  // Whether native EFB sync is active (i.e. the handler has a native framebuffer).
  bool IsActive() const { return m_fbm != nullptr; }

  // The native framebuffer manager (for record-time fbm selection).
  FramebufferManager* GetFbm() const { return m_fbm; }

private:
  FramebufferManager* m_fbm;
  std::size_t m_synced_up_to = 0;
  std::optional<EmulatedGraphics::ScissorsAndViewportData> m_last_sv;

  // Deferred copies: cache entry pointer -> op index at which the copy was recorded.
  std::unordered_map<TCacheEntry*, std::size_t> m_deferred_copies;
};
