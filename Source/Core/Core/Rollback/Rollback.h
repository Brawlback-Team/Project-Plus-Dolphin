// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Orca rollback core: exact whole-machine snapshots, saved and restored at the frame boundary.
//
// A snapshot is RAM (MEM1, MEM2, standalone GameCube ARAM, and the locked L1
// cache) plus everything else through Dolphin's DoState with tracked memory skipped. Guest memory
// is kept as undo logs over a dirty page bitmap (DirtyPages.h).
// Because
// saves and loads happen at the same instruction, CPU, DSP, timing and device state all come back
// exactly. Subsystems check
// InSnapshotDoState() to skip what a rollback must not touch: RAM inside DoState, JIT clears on
// load, and NAND file contents.

#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "Common/Buffer.h"
#include "Common/CommonTypes.h"
#include "Core/State.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace Rollback
{
struct SnapshotPhaseTimings
{
  double state_ms = 0.0;
  double dirty_pages_ms = 0.0;
  double l1_ms = 0.0;
  double jit_ms = 0.0;
  double journal_ms = 0.0;
  std::size_t state_bytes = 0;
  std::size_t changed_blocks = 0;
  std::size_t changed_cache_lines = 0;
  std::size_t jit_blocks_invalidated = 0;
};

struct ResimJitCompileStats
{
  u64 blocks = 0;
  u64 nanoseconds = 0;
};

struct ResimGuestHotspot
{
  u32 pc = 0;
  u64 nanoseconds = 0;
  u64 cycles = 0;
  u64 slices = 0;
};

struct ResimJitMemoryStats
{
  u64 read_calls = 0;
  u64 read_nanoseconds = 0;
  u64 write_calls = 0;
  u64 write_nanoseconds = 0;
};

enum class ResimEventCategory : u8
{
  GuestCpu,
  FrameSetup,
  Fifo,
  VideoInterface,
  DspAudio,
  IosIpc,
  Devices,
  Other,
  CoreTiming,
  Count,
};

struct ResimEventStats
{
  std::array<u64, static_cast<std::size_t>(ResimEventCategory::Count)> nanoseconds{};
  std::array<u64, static_cast<std::size_t>(ResimEventCategory::Count)> callbacks{};
};

struct ChangedMemoryRange
{
  u32 physical_address = 0;
  u32 length = 0;
};

// True while a rollback snapshot is being saved or loaded (CPU thread; single core only).
bool InSnapshotDoState();

// True while frames are re-run after a load: host rendering is skipped and their audio dropped.
bool IsResimulating();
void SetResimulating(bool resimulating);
void RecordResimJitCompile(u64 nanoseconds);
ResimJitCompileStats GetResimJitCompileStats();
void RecordResimGuestSlice(u32 pc, u64 nanoseconds, u64 cycles);
std::vector<ResimGuestHotspot> TakeResimGuestHotspots(std::size_t limit);
void RecordResimJitMemoryHelper(bool write, u64 nanoseconds);
ResimJitMemoryStats TakeResimJitMemoryStats();
void RecordResimEvent(ResimEventCategory category, u64 nanoseconds);
ResimEventStats GetResimEventStats();

// Holds InSnapshotDoState() true for its lifetime.
class SnapshotScope
{
public:
  SnapshotScope();
  ~SnapshotScope();
  SnapshotScope(const SnapshotScope&) = delete;
  SnapshotScope& operator=(const SnapshotScope&) = delete;
};

// A full machine image at a frame boundary: the keyframe a host sends to a player joining mid-game.
// NAND contents are sent separately.
struct MachineImage
{
  std::vector<u8> state;
  std::vector<u8> mem1;
  std::vector<u8> mem2;
  std::vector<u8> l1_cache;
};

// A ring of snapshots keyed by frame number. Saving overwrites the oldest slot.
class SnapshotRing
{
public:
  explicit SnapshotRing(std::size_t slots, bool tracked_bitmap_clear = false,
                        bool force_full_scan = false);
  ~SnapshotRing();
  SnapshotRing(const SnapshotRing&) = delete;
  SnapshotRing& operator=(const SnapshotRing&) = delete;

  // Returns true when the save had to allocate (a slot's first use, or a bigger state).
  bool Save(Core::System& system, s64 frame);
  // Restores the snapshot taken at `frame`; false if missing or the state or NAND fails to restore.
  // Only call from the frame-boundary hook: snapshots resume at the hook's pc, so loading elsewhere
  // would run the boundary twice.
  bool Load(Core::System& system, s64 frame);
  bool Has(s64 frame) const;
  // Serialized non-RAM state from the most recent Save, for determinism diagnostics.
  std::span<const u8> LastState() const;
  const State::RollbackStateLayout& LastStateLayout() const;
  std::span<const u8> StateForFrame(s64 frame) const;
  const SnapshotPhaseTimings& GetLastSaveTimings() const { return m_last_save_timings; }
  const SnapshotPhaseTimings& GetLastLoadTimings() const { return m_last_load_timings; }
  // Forgets every snapshot and stops the NAND journal. Call while emulation is still running.
  void Reset(Core::System& system);
  // Keyframes for mid-game joins. Capture writes what Save would into `image`; call it from the
  // frame-boundary hook. `stop_journal` stops the NAND journal the capture
  // started, for callers with no ring to undo to. LoadImage loads an image from another machine as
  // `frame`; the NAND must already match it.
  static bool Capture(Core::System& system, MachineImage* image, bool stop_journal);
  bool LoadImage(Core::System& system, MachineImage image, s64 frame);

private:
  // `redisplay` shows the snapshot's frame again: true for keyframes, false for rollbacks.
  bool LoadSlot(Core::System& system, s64 frame, bool redisplay);

  struct Slot
  {
    s64 frame = -1;
    std::vector<u8> mem1;
    std::vector<u8> mem2;
    std::vector<u8> l1_cache;
    Common::UniqueBuffer<u8> state;
    std::size_t state_size = 0;
    State::RollbackStateLayout state_layout;
    u64 nand_journal_mark = 0;  // NAND journal position at save time
    // Dirty-page snapshot id for MEM1/MEM2/GC ARAM, or 0 when main RAM is copied into mem1/mem2
    // and GC ARAM remains in state.
    u64 page_snapshot_id = 0;
  };

  Slot* Find(s64 frame);
  const Slot* Find(s64 frame) const;
  // Empties a slot; its dirty-page history merges into the previous snapshot.
  void Forget(Slot* slot);
  // Whether saves use dirty-page tracking; decided, and tracking armed, at the first save.
  bool UseDirtyPageTracking(Core::System& system, bool* armed_now);

  std::vector<Slot> m_slots;
  std::size_t m_next = 0;
  std::size_t m_last = 0;
  // Reused by loads to avoid allocating page vectors and unordered-set nodes every rollback.
  std::vector<u32> m_changed_blocks;
  std::vector<ChangedMemoryRange> m_changed_ranges;
  std::optional<bool> m_dirty_page_tracking;
  const bool m_tracked_bitmap_clear;
  const bool m_force_full_scan;
  SnapshotPhaseTimings m_last_save_timings;
  SnapshotPhaseTimings m_last_load_timings;
  u32 m_initial_trace_events = 0;
};

}  // namespace Rollback
