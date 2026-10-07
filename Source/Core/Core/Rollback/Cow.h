// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Copy-on-write rollback snapshots of guest RAM (MEM1 and MEM2).
//
// A snapshot write-protects guest RAM in every host mapping of it (host views and the JIT's fastmem
// views). The first write to a page afterwards faults; the fault handler saves the page's old
// contents in the newest snapshot's undo log, makes the page writable, and the write is retried.
// So a snapshot costs only the pages that change after it, not all of RAM.
//
// Invariant: every page is either read-only in every view or already saved in the newest log.
// Pages that change every frame stay writable and are copied up front at each snapshot ("hot
// pages"), because a fault costs far more than a page copy.
//
// Saves and restores go through a private, never-protected mapping of RAM. Pages are host pages
// (4 KB on x86-64, 16 KB on Apple Silicon).
//
// Kernel writes into guest RAM (e.g. read() into an IOS buffer) fail instead of faulting, so such
// code must call PrepareHostWrite first.
//
// Threading: the CPU thread drives this, but faults can arrive on any thread (the Mach exception
// thread on macOS), so all state is behind a lock. One snapshot ring owns the tracker at a time.

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

namespace Rollback::Cow
{
// A tracked region of guest physical memory and its private, never-protected mapping.
struct Area
{
  u8* alias;
  u32 physical_address;
  u32 size;
};

// One host mapping of guest physical memory. Non-writeable views are ignored: Dolphin's own fault
// handling owns writes through them.
struct View
{
  u8* base;
  u32 physical_address;
  u32 size;
  bool writeable;
};

// The host page size, which is the granularity of protection and of the undo logs.
std::size_t PageSize();

// Starts tracking `areas` through `views` for `owner` (a snapshot ring) and write-protects them.
// False if another owner is tracking, the platform has no fault handler, or the layout is unaligned
// or has more than 64 views.
bool Arm(const void* owner, const std::vector<Area>& areas, const std::vector<View>& views);
// Arm over MEM1 and MEM2 of the running machine.
bool ArmForSystem(Core::System& system, const void* owner);
// Stops tracking if `owner` holds it: RAM is writable again and every log is dropped.
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
// RamChecksum (Rollback.h) of RAM as it was at snapshot `id`, rebuilt from live RAM and the logs.
std::optional<u64> Checksum(u64 id);

// Called first by the fault handlers. True if `address` is a protected page of tracked RAM; the
// page is then saved and made writable so the faulting write can be retried.
bool HandleFault(uintptr_t address);
// Call before a system call writes [ptr, ptr + size) of guest RAM (the kernel fails on a protected
// page instead of faulting). Saves and unprotects those pages.
void PrepareHostWrite(const void* ptr, std::size_t size);

// Memmap calls this when the host mappings of guest RAM change.
void OnMappingsChanged(Core::System& system);
// Stops tracking regardless of owner. Must run before the mappings are removed and before the CPU
// thread (and its fault handler) exits.
void StopTracking();

struct Counters
{
  u64 faults = 0;
  u64 pages_recorded = 0;  // saved by faults
  u64 pages_copied = 0;    // hot pages copied up front
  u64 pages_reprotected = 0;
  u64 remaps = 0;  // mapping changes while armed
};
Counters GetCounters();
}  // namespace Rollback::Cow
