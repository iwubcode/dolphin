// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <set>

namespace GraphicsModSystem::Runtime
{
/// "Already executed" set of copy-relative texture-op indices for ONE replay unit,
/// consulted by the executor's texture-cache-mutating cases only (Reinterpret /
/// Palette / PartialTextureUpdate — the ops that write texture-cache entries;
/// UnifiedReplayDesign §4, invariant I3). Draws/clears/SV are NOT tracked (re-running
/// them is harmless: the EFB is rewritten each tick); RenderTarget ops are not tracked
/// (VRAM completion is already idempotent, Phase 2 §2 audit). NOT a watermark:
/// scrubbing jumps, so the executed set is not a monotonic prefix — a plain set of op
/// indices (renamed from the design doc's "watermark", D11; renamed from
/// ExecutedXFBOperations, D15). The CALLER owns the partitioning and passes the right
/// set per Execute call: Phase 3 keeps one set per XFB copy in the handler session
/// (§3); Phase 11 moves the same pattern into RenderTargetPool sessions with one set
/// per phase. A new presented frame's frame_number, a resume, or
/// ClearPresentedFrames() resets the container (§7).
using ExecutedTextureOperations = std::set<std::size_t>;
}  // namespace GraphicsModSystem::Runtime
