// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

// Orca rollback core: exact whole-machine snapshots, saved and restored at the frame boundary.
//
// A snapshot is RAM (MEM1, MEM2, the locked L1 cache) plus everything else through Dolphin's
// DoState with RAM skipped. RAM is kept as undo logs over a dirty page bitmap (DirtyPages.h).
// Because
// saves and loads happen at the same instruction, CPU, DSP, timing and device state all come back
// exactly. Subsystems check
// InSnapshotDoState() to skip what a rollback must not touch: RAM inside DoState, JIT clears on
// load, and NAND file contents.

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "Common/Buffer.h"
#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace Rollback
{
// True while a rollback snapshot is being saved or loaded (CPU thread; single core only).
bool InSnapshotDoState();

// True while frames are re-run after a load: host rendering is skipped and their audio dropped.
bool IsResimulating();
void SetResimulating(bool resimulating);

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
    u64 nand_journal_mark = 0;  // NAND journal position at save time
    // Dirty-page snapshot id for MEM1/MEM2, or 0 when they are copied into mem1/mem2.
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
  std::optional<bool> m_dirty_page_tracking;
  const bool m_tracked_bitmap_clear;
  const bool m_force_full_scan;
};

}  // namespace Rollback
