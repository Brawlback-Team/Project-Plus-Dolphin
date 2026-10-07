// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Rollback snapshots of guest RAM (MEM1 and MEM2) that copy only what changed.
//
// Writes are tracked by the dirty page bitmap (DirtyBitmap.h). The tracker keeps a mirror of RAM as
// it was at the newest snapshot. A snapshot copies the pages the bitmap marks, and only those whose
// bytes really differ from the mirror, into the newest undo log before the new snapshot opens.
// Restores and checksums are rebuilt from the mirror and the logs.
//
// Invariant: a page that is not marked dirty has the same bytes live as in the mirror. Every guest
// RAM write must therefore set the bitmap: JIT stores do it inline, and the other writers go
// through MemoryManager pointer accessors (GetPointerForRange) or call MarkPhysicalRangeDirty.
//
// Threading: the CPU thread drives this, and the bitmap can be set from other threads, so all
// tracker state is behind a lock. One snapshot ring owns the tracker at a time.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "Common/CommonTypes.h"

namespace Core
{
class System;
}

namespace Rollback::DirtyPages
{
// A tracked region of guest physical memory and a host mapping of it that the tracker can read and
// write freely.
struct Area
{
  u8* alias;
  u32 physical_address;
  u32 size;
};

// Starts tracking `areas` for `owner` (a snapshot ring). False if another owner is tracking, the
// areas are unaligned, or the host's JIT does not set the bitmap.
bool Arm(const void* owner, const std::vector<Area>& areas);
// Arm over MEM1 and MEM2 of the running machine.
bool ArmForSystem(Core::System& system, const void* owner);
// Stops tracking if `owner` holds it and drops every log.
void Disarm(const void* owner);
bool IsArmedFor(const void* owner);

// Takes a snapshot and opens an empty undo log for it. Returns its id (never 0).
u64 Snapshot();
bool Has(u64 id);
// Puts RAM back as it was at snapshot `id`, calling changed() for each 4 KB block that differed.
// Newer snapshots are dropped and `id` becomes the newest, with an empty log. False if `id` is
// not held.
bool Restore(u64 id, const std::function<void(u32 physical_address, u32 length)>& changed);
// Forgets snapshot `id`; its saved pages merge into the next older snapshot's log.
void Drop(u64 id);
// RamChecksum (Rollback.h) of RAM as it was at snapshot `id`, rebuilt from the mirror and the logs.
std::optional<u64> Checksum(u64 id);
// XXH3 for each 4 KB page at snapshot `id`, ordered as MEM1 pages then MEM2 pages.
std::optional<std::vector<u64>> PageChecksums(u64 id);

// Memmap calls this when the host mappings of guest RAM change. Tracking stops if RAM is now
// reachable through page tables, which the bitmap cannot see.
void OnMappingsChanged(Core::System& system);
// Stops tracking regardless of owner. Must run before the RAM mappings are removed.
void StopTracking();

struct Counters
{
  u64 dirty_pages = 0;     // pages found dirty at snapshots and restores
  u64 pages_recorded = 0;  // changed pages saved into undo logs
};
Counters GetCounters();
}  // namespace Rollback::DirtyPages
