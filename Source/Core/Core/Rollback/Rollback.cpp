// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Rollback.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <span>
#include <tuple>
#include <unordered_set>

#include <xxh3.h>

#include "Common/FPURoundMode.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/IOS.h"
#include "Core/PowerPC/JitCommon/JitBase.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/DirtyBitmap.h"
#include "Core/Rollback/DirtyPages.h"
#include "Core/State.h"
#include "Core/System.h"
#include "VideoCommon/VideoState.h"

namespace Rollback
{
namespace
{
std::atomic<bool> s_in_snapshot{false};
std::atomic<bool> s_resimulating{false};
std::atomic<u64> s_boot_diagnostic_sequence{0};

constexpr std::size_t RESTORE_PAGE = 4096;
constexpr u32 MEM1_VIRTUAL = 0x80000000u;
constexpr u32 MEM2_VIRTUAL = 0x90000000u;
// Effective to physical for MEM1/MEM2 and their uncached mirrors (0x8/0xC -> 0x0, 0x9/0xD -> 0x1).
constexpr u32 PHYSICAL_MASK = 0x1FFFFFFFu;

// Copies saved RAM back one page at a time, skipping pages that already match. Only changed pages
// have their JIT blocks invalidated; clearing the whole JIT (as a normal state load does) would
// recompile the hot code after every rollback. Changed pages are collected by physical address.
void RestoreRam(Core::System& system, u8* live, const std::vector<u8>& saved, u32 virtual_base,
                std::unordered_set<u32>* changed_pages)
{
  auto& jit = system.GetJitInterface();
  const std::size_t size = saved.size();
  for (std::size_t offset = 0; offset < size; offset += RESTORE_PAGE)
  {
    const std::size_t length = std::min(RESTORE_PAGE, size - offset);
    if (std::memcmp(live + offset, saved.data() + offset, length) == 0)
      continue;
    std::memcpy(live + offset, saved.data() + offset, length);
    const u32 address = virtual_base + static_cast<u32>(offset);
    jit.InvalidateICache(address, static_cast<u32>(length), true);
    changed_pages->insert(address & PHYSICAL_MASK);
  }
}

// The JIT also remembers per-instruction facts (GPU FIFO writes, paired quantization, no
// speculative constants). A restored page may hold different instructions, so forget its entries.
// Done here in bulk because the JIT's own per-word invalidation is too slow for thousands of pages.
void ForgetLearnedJitAddresses(Core::System& system, const std::unordered_set<u32>& changed_pages)
{
  // JitInterface only holds a JitBase (null for the interpreter). static_cast because MSVC builds
  // have RTTI off.
  auto* jit = static_cast<JitBase*>(system.GetJitInterface().GetCore());
  if (!jit || changed_pages.empty())
    return;
  const auto in_changed_page = [&changed_pages](u32 address) {
    return changed_pages.contains(address & PHYSICAL_MASK & ~static_cast<u32>(RESTORE_PAGE - 1));
  };
  std::erase_if(jit->js.fifoWriteAddresses, in_changed_page);
  std::erase_if(jit->js.pairedQuantizeAddresses, in_changed_page);
  std::erase_if(jit->js.noSpeculativeConstantsAddresses, in_changed_page);
}

// The NAND file system, which is always the host backend. Its journal is part of Orca's
// authoritative whole-emulator rollback state.
IOS::HLE::FS::HostFileSystem* HostNand(Core::System& system)
{
  IOS::HLE::EmulationKernel* ios = system.GetIOS();
  return ios ? static_cast<IOS::HLE::FS::HostFileSystem*>(ios->GetFS().get()) : nullptr;
}
}  // namespace

u64 RamChecksum(std::span<const u8> mem1, std::span<const u8> mem2)
{
  XXH3_state_t* const state = XXH3_createState();
  XXH3_64bits_reset(state);
  XXH3_64bits_update(state, mem1.data(), mem1.size());
  XXH3_64bits_update(state, mem2.data(), mem2.size());
  const u64 hash = XXH3_64bits_digest(state);
  XXH3_freeState(state);
  return hash;
}

bool InSnapshotDoState()
{
  return s_in_snapshot.load(std::memory_order_relaxed);
}

bool IsResimulating()
{
  return s_resimulating.load(std::memory_order_relaxed);
}

void SetResimulating(bool resimulating)
{
  s_resimulating.store(resimulating, std::memory_order_relaxed);
  VideoCommon_SetSkipRender(resimulating);
}

void LogBootMemoryDigest(Core::System& system, std::string_view stage)
{
  if (stage == "memory_clear")
    s_boot_diagnostic_sequence.fetch_add(1, std::memory_order_relaxed);

  auto& memory = system.GetMemory();
  const std::span<const u8> mem1(memory.GetRAM(), memory.GetRamSize());
  const std::span<const u8> mem2(memory.GetEXRAM(),
                                 memory.GetEXRAM() ? memory.GetExRamSize() : 0);
  const std::span<const u8> l1(memory.GetL1Cache(), memory.GetL1CacheSize());
  NOTICE_LOG_FMT(CORE,
                 "Rollback boot_diag: boot={} stage={} mem1={:016x} mem2={:016x} l1={:016x}",
                 s_boot_diagnostic_sequence.load(std::memory_order_relaxed), stage,
                 XXH3_64bits(mem1.data(), mem1.size()), XXH3_64bits(mem2.data(), mem2.size()),
                 XXH3_64bits(l1.data(), l1.size()));
}

SnapshotScope::SnapshotScope()
{
  s_in_snapshot.store(true, std::memory_order_relaxed);
  // In a session, host GPU state (EFB, texture cache, bounding box) never reaches emulated RAM, so
  // snapshots skip it: no GPU readback on save, no texture-cache reload on load.
  VideoCommon_SetRollbackSnapshot(true);
}

SnapshotScope::~SnapshotScope()
{
  VideoCommon_SetRollbackSnapshot(false);
  s_in_snapshot.store(false, std::memory_order_relaxed);
}

SnapshotRing::SnapshotRing(std::size_t slots) : m_slots(std::max<std::size_t>(slots, 2))
{
}

SnapshotRing::~SnapshotRing()
{
  DirtyPages::Disarm(this);
}

void SnapshotRing::Forget(Slot* slot)
{
  if (slot->page_snapshot_id != 0)
    DirtyPages::Drop(slot->page_snapshot_id);
  slot->page_snapshot_id = 0;
  slot->frame = -1;
}

bool SnapshotRing::UseDirtyPageTracking(Core::System& system, bool* armed_now)
{
  *armed_now = false;
  // If tracking stopped under us (emulation stopped, or an unsupported RAM mapping), the
  // undo-log snapshots are gone.
  if (m_dirty_page_tracking.value_or(false) && !DirtyPages::IsArmedFor(this))
  {
    for (Slot& slot : m_slots)
    {
      if (slot.page_snapshot_id != 0)
      {
        slot.page_snapshot_id = 0;
        slot.frame = -1;
      }
    }
    m_dirty_page_tracking.reset();
  }
  if (!m_dirty_page_tracking)
  {
    m_dirty_page_tracking = DirtyPages::ArmForSystem(system, this);
    *armed_now = *m_dirty_page_tracking;
  }
  return *m_dirty_page_tracking;
}

SnapshotRing::Slot* SnapshotRing::Find(s64 frame)
{
  for (Slot& slot : m_slots)
  {
    if (slot.frame == frame)
      return &slot;
  }
  return nullptr;
}

const SnapshotRing::Slot* SnapshotRing::Find(s64 frame) const
{
  for (const Slot& slot : m_slots)
  {
    if (slot.frame == frame)
      return &slot;
  }
  return nullptr;
}

bool SnapshotRing::Has(s64 frame) const
{
  const Slot* slot = Find(frame);
  return slot && (slot->page_snapshot_id == 0 || DirtyPages::Has(slot->page_snapshot_id));
}

void SnapshotRing::Reset(Core::System& system)
{
  for (Slot& slot : m_slots)
  {
    slot.frame = -1;
    slot.page_snapshot_id = 0;
  }
  DirtyPages::Disarm(this);
  m_dirty_page_tracking.reset();
  m_next = 0;
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
    nand->JournalStop();
}

bool SnapshotRing::Save(Core::System& system, s64 frame)
{
  // Re-saving a frame after a rewind reuses its slot; otherwise take the next one.
  Slot* slot = Find(frame);
  if (!slot)
  {
    slot = &m_slots[m_next];
    m_next = (m_next + 1) % m_slots.size();
  }
  m_last = static_cast<std::size_t>(slot - m_slots.data());
  Forget(slot);
  bool armed_now;
  const bool dirty_page_tracking = UseDirtyPageTracking(system, &armed_now);
  const auto buffers = [slot] {
    return std::tuple{slot->state.data(), slot->mem1.capacity(), slot->mem2.capacity()};
  };
  const auto buffers_before = buffers();

  // Non-RAM state first, so anything DoState writes into RAM is in the RAM snapshot.
  {
    SnapshotScope scope;
    slot->state_size = State::SaveToBufferForRollback(system, slot->state);
  }

  auto& memory = system.GetMemory();
  // Dirty-page tracking: commit the RAM written since the last snapshot and open this snapshot's
  // undo log. A 0 id means tracking stopped, so fall back to a full copy.
  if (dirty_page_tracking)
    slot->page_snapshot_id = DirtyPages::Snapshot();
  if (slot->page_snapshot_id == 0)
  {
    slot->mem1.assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSize());
    if (memory.GetEXRAM())
      slot->mem2.assign(memory.GetEXRAM(), memory.GetEXRAM() + memory.GetExRamSize());
    else
      slot->mem2.clear();
  }
  slot->l1_cache.assign(memory.GetL1Cache(), memory.GetL1Cache() + memory.GetL1CacheSize());
  slot->frame = frame;

  // NAND changes older than the oldest snapshot can never be undone, so trim them.
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
  {
    slot->nand_journal_mark = nand->LastJournalMark();
    u64 oldest = slot->nand_journal_mark;
    for (const Slot& other : m_slots)
    {
      if (other.frame >= 0)
        oldest = std::min(oldest, other.nand_journal_mark);
    }
    nand->JournalTrim(oldest);
  }

  // Arming allocates and initializes the RAM mirrors; that one-off cost counts as an allocation.
  return armed_now || buffers() != buffers_before;
}

bool SnapshotRing::Load(Core::System& system, s64 frame)
{
  return LoadSlot(system, frame, false);
}

bool SnapshotRing::LoadSlot(Core::System& system, s64 frame, bool redisplay)
{
  Slot* slot = Find(frame);
  if (!slot || slot->state_size == 0)
  {
    ERROR_LOG_FMT(CORE, "Rollback: no snapshot for frame {}", frame);
    return false;
  }

  // Restore RAM before device state: some devices (emulated Bluetooth) re-parse pending IOS
  // requests from RAM while loading, and live RAM may already hold a newer reply. The device-state
  // load writes no RAM in a session, since EFB copies stay on the GPU.
  auto& memory = system.GetMemory();
  std::unordered_set<u32> changed_pages;
  if (slot->page_snapshot_id != 0)
  {
    // The undo logs of this snapshot and every newer one list exactly the pages written since.
    std::vector<u32> changed_blocks;
    if (!DirtyPages::Restore(slot->page_snapshot_id, [&changed_blocks](u32 physical, u32 length) {
          for (u32 offset = 0; offset < length; offset += static_cast<u32>(RESTORE_PAGE))
            changed_blocks.push_back(physical + offset);
        }))
    {
      ERROR_LOG_FMT(CORE, "Rollback: the dirty-page snapshot for frame {} is gone", frame);
      return false;
    }
    // MEM1 is at 0x80000000 and MEM2 at 0x90000000: both are physical | 0x80000000.
    auto& jit = system.GetJitInterface();
    for (const u32 physical : changed_blocks)
    {
      jit.InvalidateICache(physical | MEM1_VIRTUAL, static_cast<u32>(RESTORE_PAGE), true);
      changed_pages.insert(physical);
    }
  }
  else
  {
    RestoreRam(system, memory.GetRAM(), slot->mem1, MEM1_VIRTUAL, &changed_pages);
    if (memory.GetEXRAM() && !slot->mem2.empty())
      RestoreRam(system, memory.GetEXRAM(), slot->mem2, MEM2_VIRTUAL, &changed_pages);
  }
  std::memcpy(memory.GetL1Cache(), slot->l1_cache.data(), slot->l1_cache.size());
  ForgetLearnedJitAddresses(system, changed_pages);

  bool ok;
  {
    SnapshotScope scope;
    VideoCommon_SetSnapshotRedisplays(redisplay);
    ok = State::LoadFromBufferForRollback(system,
                                          std::span<u8>(slot->state.data(), slot->state_size));
    VideoCommon_SetSnapshotRedisplays(false);
  }
  if (!ok)
  {
    ERROR_LOG_FMT(CORE, "Rollback: snapshot for frame {} did not load", frame);
    return false;
  }
  // The NAND journal must have undone every change since this snapshot.
  if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system); nand && nand->TakeJournalError())
  {
    ERROR_LOG_FMT(CORE, "Rollback: the NAND could not be restored to frame {}", frame);
    return false;
  }

  // Later frames are about to be re-run and re-saved, so drop their snapshots.
  for (Slot& other : m_slots)
  {
    if (other.frame > frame ||
        (other.page_snapshot_id != 0 && !DirtyPages::Has(other.page_snapshot_id)))
      Forget(&other);
  }
  m_next = (static_cast<std::size_t>(slot - m_slots.data()) + 1) % m_slots.size();
  return true;
}

std::optional<u64> SnapshotRing::RamChecksum(s64 frame) const
{
  const Slot* slot = Find(frame);
  if (!slot)
    return std::nullopt;
  // Dirty-page snapshots keep no full copy: rebuild RAM from live RAM plus the undo logs.
  if (slot->page_snapshot_id != 0)
    return DirtyPages::Checksum(slot->page_snapshot_id);
  return Rollback::RamChecksum(slot->mem1, slot->mem2);
}

std::optional<SnapshotDigest> SnapshotRing::Digest(s64 frame) const
{
  const Slot* slot = Find(frame);
  if (!slot || slot->state_size == 0)
    return std::nullopt;

  const std::optional<u64> ram = RamChecksum(frame);
  if (!ram)
    return std::nullopt;

  SnapshotDigest digest;
  digest.ram = *ram;
  digest.state = XXH3_64bits(slot->state.data(), slot->state_size);
  digest.l1_cache = XXH3_64bits(slot->l1_cache.data(), slot->l1_cache.size());
  const std::array<u64, 4> components = {digest.ram, digest.state, digest.l1_cache,
                                         slot->nand_journal_mark};
  digest.combined = XXH3_64bits(components.data(), sizeof(components));
  return digest;
}

std::optional<std::vector<u64>> SnapshotRing::PageChecksums(s64 frame) const
{
  const Slot* slot = Find(frame);
  if (!slot)
    return std::nullopt;
  if (slot->page_snapshot_id != 0)
    return DirtyPages::PageChecksums(slot->page_snapshot_id);

  std::vector<u64> hashes;
  hashes.reserve((slot->mem1.size() + slot->mem2.size()) / DIRTY_PAGE_SIZE);
  const auto append = [&hashes](const std::vector<u8>& memory) {
    for (std::size_t offset = 0; offset < memory.size(); offset += DIRTY_PAGE_SIZE)
    {
      const std::size_t size = std::min(DIRTY_PAGE_SIZE, memory.size() - offset);
      hashes.push_back(XXH3_64bits(memory.data() + offset, size));
    }
  };
  append(slot->mem1);
  append(slot->mem2);
  return hashes;
}

std::span<const u8> SnapshotRing::LastState() const
{
  if (m_last >= m_slots.size())
    return {};
  const Slot& slot = m_slots[m_last];
  return {slot.state.data(), slot.state_size};
}

bool SnapshotRing::Capture(Core::System& system, MachineImage* image, bool stop_journal)
{
  Common::UniqueBuffer<u8> state;
  std::size_t state_size;
  {
    SnapshotScope scope;
    state_size = State::SaveToBufferForRollback(system, state);
  }
  if (state_size == 0)
    return false;
  image->state.assign(state.data(), state.data() + state_size);
  auto& memory = system.GetMemory();
  image->mem1.assign(memory.GetRAM(), memory.GetRAM() + memory.GetRamSize());
  if (memory.GetEXRAM())
    image->mem2.assign(memory.GetEXRAM(), memory.GetEXRAM() + memory.GetExRamSize());
  else
    image->mem2.clear();
  image->l1_cache.assign(memory.GetL1Cache(), memory.GetL1Cache() + memory.GetL1CacheSize());
  if (stop_journal)
  {
    if (IOS::HLE::FS::HostFileSystem* nand = HostNand(system))
      nand->JournalStop();
  }
  return true;
}

bool SnapshotRing::LoadImage(Core::System& system, MachineImage image, s64 frame)
{
  auto& memory = system.GetMemory();
  if (image.state.empty() || image.mem1.size() != memory.GetRamSize() ||
      image.mem2.size() != (memory.GetEXRAM() ? memory.GetExRamSize() : 0) ||
      image.l1_cache.size() != memory.GetL1CacheSize())
  {
    ERROR_LOG_FMT(CORE, "Rollback: keyframe for frame {} doesn't fit this machine", frame);
    return false;
  }
  Slot* slot = Find(frame);
  if (!slot)
  {
    slot = &m_slots[m_next];
    m_next = (m_next + 1) % m_slots.size();
  }
  // Store as a full-copy slot (page_snapshot_id 0), so LoadSlot restores mem1/mem2 directly.
  Forget(slot);
  slot->state.reset(image.state.size());
  std::memcpy(slot->state.data(), image.state.data(), image.state.size());
  slot->state_size = image.state.size();
  slot->mem1 = std::move(image.mem1);
  slot->mem2 = std::move(image.mem2);
  slot->l1_cache = std::move(image.l1_cache);
  slot->frame = frame;
  slot->nand_journal_mark = 0;
  // A joining machine has nothing on screen yet, so show the keyframe's frame.
  return LoadSlot(system, frame, true);
}

}  // namespace Rollback
