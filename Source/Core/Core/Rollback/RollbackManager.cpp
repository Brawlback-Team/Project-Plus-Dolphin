// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/RollbackManager.h"

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstring>
#include <fmt/format.h>
#include <vector>

#include <Core/State.h>
#include "Common/Hash.h"
#include "Common/Logging/Log.h"
#include "Common/Swap.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/Perf.h"
#include "Core/System.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoState.h"

namespace Rollback
{

// Brawl's GameFrame::frameCounter at 0x901812a4 (MEM2). This resets when the
// match scene starts and advances inside the real game loop.
static constexpr uint32_t BRAWL_GAME_FRAME_COUNTER_MEM2_OFFSET = 0x001812a4u;

uint32_t ReadBrawlMatchFrameCounter(const uint8_t* mem2_ptr, size_t mem2_size)
{
  if (!mem2_ptr || BRAWL_GAME_FRAME_COUNTER_MEM2_OFFSET + 4 > mem2_size)
    return 0;

  uint32_t raw;
  std::memcpy(&raw, mem2_ptr + BRAWL_GAME_FRAME_COUNTER_MEM2_OFFSET, sizeof(raw));
  return Common::swap32(raw);
}

void RollbackManager::CaptureFullRamSnapshot(RollbackSnapshot& snap)
{
  if (!snap.mem1)
    snap.mem1 = std::make_unique<uint8_t[]>(m_mem1_size);
  std::memcpy(snap.mem1.get(), m_mem1_ptr, m_mem1_size);

  if (m_mem2_ptr && m_mem2_size > 0)
  {
    if (!snap.mem2)
      snap.mem2 = std::make_unique<uint8_t[]>(m_mem2_size);
    std::memcpy(snap.mem2.get(), m_mem2_ptr, m_mem2_size);
  }

  snap.brawl_frame = ReadBrawlMatchFrameCounter(m_mem2_ptr, m_mem2_size);
  snap.valid = true;
}

#if ROLLBACK_VALIDATE

void RollbackManager::CompareValSnapshot(int target_slot, int frames_back) const
{
  ROLLBACK_ZONE();
  const RollbackSnapshot& snap = m_val_snapshots[target_slot];
  if (!snap.valid || !snap.mem1)
  {
    OSD::AddMessage(fmt::format("VALIDATE: no snapshot for slot {}", target_slot), 3000,
                    OSD::Color::YELLOW);
    return;
  }

  uint32_t mem1_mismatch_count = 0;
  constexpr int MAX_LOG_PAGES = 8;
  uint32_t mismatch_addrs[MAX_LOG_PAGES];  // Store physical addresses instead of page indices
  const uint32_t mem1_end = static_cast<uint32_t>(m_mem1_size);

  // Helper lambda to check if a specific byte address is excluded
  auto IsExcluded = [&](uint32_t addr) -> bool {
    for (const auto& r : m_exclude_regions)
    {
      if (addr >= r.phys_start && addr < r.phys_end)
        return true;
    }
    return false;
  };

  // Scan MEM1 byte-by-byte (optimized by segments)
  uint32_t cursor = 0;
  while (cursor < mem1_end)
  {
    // If cursor is inside an exclusion, jump to the end of it
    if (IsExcluded(cursor))
    {
      uint32_t jump_to = cursor + 1;
      for (const auto& r : m_exclude_regions)
      {
        if (cursor >= r.phys_start && cursor < r.phys_end)
        {
          jump_to = std::max(jump_to, r.phys_end);
          break;
        }
      }
      cursor = std::min(jump_to, mem1_end);
      continue;
    }

    // Find the next exclusion boundary to define the comparison segment
    uint32_t seg_end = mem1_end;
    for (const auto& r : m_exclude_regions)
    {
      if (r.phys_start > cursor && r.phys_start < seg_end)
        seg_end = r.phys_start;
    }
    // Also ensure we don't cross into an exclusion if we started before it
    // (Handled by the IsExcluded check at loop start, but seg_end ensures we stop before next one)

    const size_t copy_size = seg_end - cursor;
    if (copy_size > 0)
    {
      if (std::memcmp(m_mem1_ptr + cursor, snap.mem1.get() + cursor, copy_size) != 0)
      {
        // Mismatch found in this segment.
        // For logging, we record the first mismatching address found.
        // Note: memcmp doesn't tell us exactly where, so we log the segment start.
        if (mem1_mismatch_count < MAX_LOG_PAGES)
        {
          mismatch_addrs[mem1_mismatch_count] = cursor;
        }
        ++mem1_mismatch_count;

        // Optimization: If we already logged enough, we can just count the rest
        // without precise logging, or break if strict performance is needed.
        // Here we continue to get an accurate total count.
      }
    }
    cursor = seg_end;
  }

  // MEM2 Handling (Similar logic)
  uint32_t mem2_mismatch_count = 0;
  uint32_t mem2_mismatch_addrs[MAX_LOG_PAGES] = {};

  if (snap.mem2 && m_mem2_ptr && m_mem2_size > 0)
  {
    const uint32_t mem2_end = static_cast<uint32_t>(m_mem2_size);
    const uint32_t MEM2_PHYS_BASE = 0x10000000u;

    cursor = 0;
    while (cursor < mem2_end)
    {
      uint32_t phys_addr = MEM2_PHYS_BASE + cursor;

      // Check exclusion
      bool excluded = false;
      for (const auto& r : m_exclude_regions)
      {
        if (phys_addr >= r.phys_start && phys_addr < r.phys_end)
        {
          excluded = true;
          // Jump logic
          uint32_t jump_to = phys_addr + 1;
          if (phys_addr >= r.phys_start && phys_addr < r.phys_end)
            jump_to = std::max(jump_to, r.phys_end);
          cursor = std::min(jump_to - MEM2_PHYS_BASE, mem2_end);
          break;
        }
      }
      if (excluded)
        continue;

      // Find next boundary
      uint32_t seg_end_phys = MEM2_PHYS_BASE + mem2_end;
      for (const auto& r : m_exclude_regions)
      {
        if (r.phys_start > phys_addr && r.phys_start < seg_end_phys)
          seg_end_phys = r.phys_start;
      }

      const size_t seg_size = seg_end_phys - phys_addr;
      if (seg_size > 0)
      {
        if (std::memcmp(m_mem2_ptr + cursor, snap.mem2.get() + cursor, seg_size) != 0)
        {
          if (mem2_mismatch_count < MAX_LOG_PAGES)
            mem2_mismatch_addrs[mem2_mismatch_count] = phys_addr;
          ++mem2_mismatch_count;
        }
      }
      cursor = seg_end_phys - MEM2_PHYS_BASE;
    }
  }

  const uint32_t current_brawl_frame = ReadBrawlMatchFrameCounter(m_mem2_ptr, m_mem2_size);
  const bool frame_ok = (current_brawl_frame == snap.brawl_frame);
  const char* frame_tag = frame_ok ? "frame_ok" : "FRAME_MISMATCH";

  if (mem1_mismatch_count == 0 && mem2_mismatch_count == 0)
  {
    const MemoryRegion& stack_excl = m_exclude_regions.back();
    INFO_LOG_FMT(BRAWLBACK,
                 "[Rollback] VALIDATE OK  step={}  slot={}  brawl_frame={} (want {})  {}  "
                 "stack_excl=[0x{:08x},0x{:08x})",
                 frames_back, target_slot, current_brawl_frame, snap.brawl_frame, frame_tag,
                 stack_excl.phys_start, stack_excl.phys_end);
    return;
  }

  // Logging MEM1
  std::string mem1_list;
  const uint32_t mem1_logged = std::min(mem1_mismatch_count, static_cast<uint32_t>(MAX_LOG_PAGES));
  for (uint32_t i = 0; i < mem1_logged; ++i)
  {
    if (i)
      mem1_list += ", ";
    mem1_list += fmt::format("0x{:08x}", mismatch_addrs[i]);
  }
  if (mem1_mismatch_count > MAX_LOG_PAGES)
    mem1_list += fmt::format(" (+{} more)", mem1_mismatch_count - MAX_LOG_PAGES);

  // Logging MEM2
  std::string mem2_list;
  const uint32_t mem2_logged = std::min(mem2_mismatch_count, static_cast<uint32_t>(MAX_LOG_PAGES));
  for (uint32_t i = 0; i < mem2_logged; ++i)
  {
    if (i)
      mem2_list += ", ";
    mem2_list += fmt::format("0x{:08x}", mem2_mismatch_addrs[i]);
  }
  if (mem2_mismatch_count > MAX_LOG_PAGES)
    mem2_list += fmt::format(" (+{} more)", mem2_mismatch_count - MAX_LOG_PAGES);

  const MemoryRegion& stack_excl = m_exclude_regions.back();
  WARN_LOG_FMT(BRAWLBACK,
               "[Rollback] VALIDATE FAIL  step={}  slot={} - {} MEM1 + "
               "{} MEM2 region(s) wrong.  "
               "brawl_frame={} (want {})  {}  stack_excl=[0x{:08x},0x{:08x})\n"
               "  First MEM1 addrs: {}\n  First MEM2 addrs: {}",
               frames_back, target_slot, mem1_mismatch_count, mem2_mismatch_count,
               current_brawl_frame, snap.brawl_frame, frame_tag, stack_excl.phys_start,
               stack_excl.phys_end, mem1_list, mem2_list);
}

void RollbackManager::InvalidateValSnapshots()
{
  for (int i = 0; i < NUM_SAVE_SLOTS; ++i)
    m_val_snapshots[i].valid = false;
}

#endif  // ROLLBACK_VALIDATE

RollbackManager& RollbackManager::Get()
{
  static RollbackManager s_instance;
  return s_instance;
}

void RollbackManager::BeginDoState()
{
  m_skip_ram_in_dostate.store(true, std::memory_order_seq_cst);
  m_skip_jit_clear_in_dostate.store(true, std::memory_order_seq_cst);
  VideoCommon_SetSkipGPUReadbackForRollback(true);
  VideoCommon_SetSkipVertexFlushForRollback(true);
  m_skip_ios_in_dostate.store(true, std::memory_order_seq_cst);
  PowerPC_SetSkipDCacheFlushForRollback(true);
  PowerPC_SetSkipCPURegsForRollback(true);
}

void RollbackManager::EndDoState()
{
  PowerPC_SetSkipCPURegsForRollback(false);
  PowerPC_SetSkipDCacheFlushForRollback(false);
  m_skip_ios_in_dostate.store(false, std::memory_order_seq_cst);
  VideoCommon_SetSkipVertexFlushForRollback(false);
  VideoCommon_SetSkipGPUReadbackForRollback(false);
  m_skip_jit_clear_in_dostate.store(false, std::memory_order_seq_cst);
  m_skip_ram_in_dostate.store(false, std::memory_order_seq_cst);
}

void RollbackManager::AddExcludeRegion(uint32_t virt_addr, uint32_t size_bytes)
{
  INFO_LOG_FMT(BRAWLBACK, "Added exclude region {} - {}", virt_addr, virt_addr + size_bytes);
  m_exclude_regions.push_back(MemoryRegion::FromVirt(virt_addr, size_bytes));
}

static const std::vector<MemoryRegion> s_brawlback_hardcoded_exclude_regions = {
};

static const std::vector<MemoryRegionThroughPtrs> s_brawlback_hardcoded_desync_detection_regions = {
    // GAME_FRAME->persistentFrameCounter
    MemoryRegionThroughPtrs::FromVirt(0x901812a0u + 0x14u, 4),

    // Player damage/percent
    MemoryRegionThroughPtrs::FromVirt(0x80623324u, 4),  // P1
    MemoryRegionThroughPtrs::FromVirt(0x80623568u, 4),  // P2
    MemoryRegionThroughPtrs::FromVirt(0x806237ACu, 4),  // P3
    MemoryRegionThroughPtrs::FromVirt(0x806239F0u, 4),  // P4

    // Player stock count
    MemoryRegionThroughPtrs::FromVirt(0x80623318u, 4),  // P1
    MemoryRegionThroughPtrs::FromVirt(0x8062355Cu, 4),  // P2
    MemoryRegionThroughPtrs::FromVirt(0x806237A0u, 4),  // P3
    MemoryRegionThroughPtrs::FromVirt(0x806239E4u, 4),  // P4

    // Player positions
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x34u, 0x60u, 0xD8u, 0xCu, 0xCu}, 4),    // P1 X
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x34u, 0x60u, 0xD8u, 0xCu, 0x10u}, 4),   // P1 Y
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x278u, 0x60u, 0xD8u, 0xCu, 0xCu}, 4),   // P2 X
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x278u, 0x60u, 0xD8u, 0xCu, 0x10u}, 4),  // P2 Y
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x4BCu, 0x60u, 0xD8u, 0xCu, 0xCu}, 4),   // P3 X
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x4BCu, 0x60u, 0xD8u, 0xCu, 0x10u}, 4),  // P3 Y
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x700u, 0x60u, 0xD8u, 0xCu, 0xCu}, 4),   // P4 X
    MemoryRegionThroughPtrs::FromPtrs(0x80624780u, {0x700u, 0x60u, 0xD8u, 0xCu, 0x10u}, 4),  // P4 Y

    // Player velocities
    MemoryRegionThroughPtrs::FromVirt(0x80494F30u, 8),  // P1 Total Velocity (X, Y)
    MemoryRegionThroughPtrs::FromVirt(0x8049DEE4u, 8),  // P2 Total Velocity (X, Y)
    MemoryRegionThroughPtrs::FromVirt(0x80494F98u, 8),  // P3 Total Velocity (X, Y)
    MemoryRegionThroughPtrs::FromVirt(0x80495000u, 8),  // P4 Total Velocity (X, Y)
};

#if BRAWLBACK_DESYNC_DETECTION
static const u8* GetRegionPointer(const MemoryRegion& region, const u8* mem1_ptr, size_t mem1_size,
                                  const u8* mem2_ptr, size_t mem2_size)
{
  if (region.phys_start >= region.phys_end)
    return nullptr;

  if (region.phys_start >= MEM2_BASE)
  {
    const u32 mem2_start = region.phys_start - MEM2_BASE;
    const u32 mem2_end = region.phys_end - MEM2_BASE;
    if (mem2_ptr && mem2_end <= mem2_size)
      return mem2_ptr + mem2_start;
    return nullptr;
  }

  if (mem1_ptr && region.phys_end <= mem1_size)
    return mem1_ptr + region.phys_start;
  return nullptr;
}
#endif

uint32_t CalculateBrawlbackDesyncChecksum(Core::System& system)
{
#if BRAWLBACK_DESYNC_DETECTION
  auto& memory = system.GetMemory();
  u8* const mem1 = memory.GetRAM();
  u8* const mem2 = memory.GetEXRAM();
  const size_t mem1_size = memory.GetRamSize();
  const size_t mem2_size = memory.GetExRamSize();

  u32 crc = Common::StartCRC32();
  for (const MemoryRegionThroughPtrs& source_region :
       s_brawlback_hardcoded_desync_detection_regions)
  {
    const bool is_pointer_region =
        !source_region.pointer_offsets.empty() || source_region.final_data_size != 0;
    const MemoryRegion region = is_pointer_region ?
                                    source_region.Resolve(mem1, mem1_size, mem2, mem2_size) :
                                    MemoryRegion{source_region.phys_start, source_region.phys_end};
    const u32 region_len = region.phys_end - region.phys_start;
    if (const u8* ptr = GetRegionPointer(region, mem1, mem1_size, mem2, mem2_size))
      crc = Common::UpdateCRC32(crc, ptr, region_len);
  }
  return crc;
#else
  return 0;
#endif
}

void RollbackManager::Init(Core::System& system)
{
  if (m_initialized)
    Shutdown();

  // Initialize WSQ job system once; workers persist across save/load cycles.
  if (!m_dispatch_thread)
  {
    m_job_ctx.activate();
    // Worker 0 is "owned" by the rollback thread — used for job creation/dispatch.
    // All initialize_worker calls must be sequential (no thread safety in ctx setup).
    m_dispatch_thread = m_job_ctx.initialize_worker(0, nullptr);
    for (int i = 1; i <= ROLLBACK_NUM_HELPER_THREADS; ++i)
    {
      job::JobTaskThread* thr =
          m_job_ctx.initialize_worker(static_cast<int64_t>(i) * 0x9e3779b97f4a7c15LL, nullptr);
      m_worker_threads.emplace_back([thr]() {
        ROLLBACK_THREAD_NAME("Rollback Job Pool");
        thr->wait_for_termination();
      });
    }
  }

  m_exclude_regions = s_brawlback_hardcoded_exclude_regions;
  PerfInit();

  auto& memory = system.GetMemory();
  m_mem1_ptr = memory.GetRAM();
  m_mem1_size = memory.GetRamSize();
  m_mem2_ptr = memory.GetEXRAM();
  m_mem2_size = memory.GetExRamSize();
  m_l1_cache_ptr = memory.GetL1Cache();
  m_l1_cache_size = memory.GetL1CacheSize();

  for (int i = 0; i < NUM_SAVE_SLOTS; ++i)
    m_slots[i].Init(m_mem1_ptr, m_mem1_size, m_mem2_ptr, m_mem2_size, m_l1_cache_ptr,
                    m_l1_cache_size);

  m_needs_source_mem1.assign(m_mem1_size / PAGE_SIZE, 0);
  m_needs_source_mem2.assign(m_mem2_size / PAGE_SIZE, 0);

  JITDirtyBitmap::Get().Clear();

  m_ring_next = 0;
  m_ring_count = 0;

  m_skip_ram_in_dostate.store(false, std::memory_order_relaxed);
  m_skip_ios_in_dostate.store(false, std::memory_order_relaxed);
  m_skip_jit_clear_in_dostate.store(false, std::memory_order_relaxed);
  m_frame_save_pending.store(false, std::memory_order_relaxed);
  m_initialized = true;

#if ROLLBACK_VALIDATE
  InvalidateValSnapshots();
#endif
}

void RollbackManager::Shutdown()
{
  if (!m_initialized)
    return;

  if (m_eviction_job)
  {
    job::DrainJobsUntilComplete(m_dispatch_thread, m_eviction_job);
    m_eviction_job = nullptr;
  }

  m_base_snapshot.valid = false;
  m_base_snapshot.mem1.reset();
  m_base_snapshot.mem2.reset();

  JITDirtyBitmap::Get().Clear();

  for (int i = 0; i < NUM_SAVE_SLOTS; ++i)
    m_slots[i].Reset();

  m_ring_next = 0;
  m_ring_count = 0;

  m_skip_ram_in_dostate.store(false, std::memory_order_relaxed);
  m_skip_ios_in_dostate.store(false, std::memory_order_relaxed);
  m_skip_jit_clear_in_dostate.store(false, std::memory_order_relaxed);
  m_frame_save_pending.store(false, std::memory_order_relaxed);
  m_frame_save_enabled.store(false, std::memory_order_relaxed);
  VideoCommon_SetSkipGPUReadbackForRollback(false);

  // Signal WSQ workers to exit and wait for them.
  // Workers are persistent — only tear them down on full shutdown.
  if (m_dispatch_thread)
  {
    m_job_ctx.deactivate();
    for (auto& t : m_worker_threads)
      if (t.joinable())
        t.join();
    m_worker_threads.clear();
    m_dispatch_thread = nullptr;
  }

  m_initialized = false;

#if ROLLBACK_VALIDATE
  InvalidateValSnapshots();
#endif
}

void RollbackManager::ToggleFrameSave()
{
  const bool enabled = !m_frame_save_enabled.load(std::memory_order_relaxed);
  m_frame_save_enabled.store(enabled, std::memory_order_relaxed);

  if (enabled)
  {
    m_ring_next = 0;
    m_ring_count = 0;

    if (m_eviction_job)
    {
      job::DrainJobsUntilComplete(m_dispatch_thread, m_eviction_job);
      m_eviction_job = nullptr;
    }
    m_base_snapshot.valid = false;

    JITDirtyBitmap::Get().Clear();

#if ROLLBACK_VALIDATE
    InvalidateValSnapshots();
#endif
    OSD::AddMessage(fmt::format("Rollback: frame-save ON  ({} slots)", NUM_SAVE_SLOTS), 3000,
                    OSD::Color::GREEN);
  }
  else
  {
    OSD::AddMessage("Rollback: frame-save OFF", 3000, OSD::Color::YELLOW);
  }
}

s32 Wrap(s32 x, s32 wrap)
{
  if (x < 0)
    x = (wrap + x);
  ASSERT(x >= 0);
  return x % wrap;
}

void RollbackManager::SaveFrame(Core::System& system)
{
  ROLLBACK_ZONE();
  if (!m_initialized)
    return;

  // Lazy init on the first frame, so we can be sure the game is fully booted when taking the base
  // snapshot
  if (!m_base_snapshot.valid)
  {
    ROLLBACK_ZONE_N("BaseSnapshot::Init");
    std::unique_lock lk(m_base_snapshot.mutex);
    CaptureFullRamSnapshot(m_base_snapshot);
    INFO_LOG_FMT(BRAWLBACK, "Captured base snapshot at brawl frame {}",
                 m_base_snapshot.brawl_frame);
  }

  const int slot = m_ring_next;

  // Evict the oldest slot, async apply its deltas to the base snapshot
  if (m_ring_count >= NUM_SAVE_SLOTS)
  {
    ROLLBACK_ZONE_N("Prep eviction");
    // Wait for any in-flight eviction — typically completes within the same frame.
    if (m_eviction_job)
    {
      job::DrainJobsUntilComplete(m_dispatch_thread, m_eviction_job);
      m_eviction_job = nullptr;
    }
    auto evicted = std::make_shared<Rollback::EvictedDelta>(m_slots[slot].ExtractDeltas());
    m_eviction_job = job::KickRootJob(
        m_dispatch_thread, [this, evicted](job::JobTaskThread&, job::Job&) mutable {
          ROLLBACK_ZONE_N("BaseSnapshot::Evict");
          std::unique_lock lk(m_base_snapshot.mutex);

          const uint8_t* src = evicted->mem1.page_data.data();
          for (uint32_t i = 0; i < evicted->mem1.page_count; ++i)
          {
            const size_t dst_off = static_cast<size_t>(evicted->mem1.page_indices[i]) * PAGE_SIZE;
            std::memcpy(m_base_snapshot.mem1.get() + dst_off, src + i * PAGE_SIZE, PAGE_SIZE);
          }

          src = evicted->mem2.page_data.data();
          for (uint32_t i = 0; i < evicted->mem2.page_count; ++i)
          {
            const size_t dst_off = static_cast<size_t>(evicted->mem2.page_indices[i]) * PAGE_SIZE;
            std::memcpy(m_base_snapshot.mem2.get() + dst_off, src + i * PAGE_SIZE, PAGE_SIZE);
          }
        });
  }

  m_ring_next = Wrap(m_ring_next + 1, NUM_SAVE_SLOTS);
  m_ring_count = std::clamp(m_ring_count + 1, 0, NUM_SAVE_SLOTS);

  {
    m_slots[slot].Save(system);
  }

#if ROLLBACK_VALIDATE
  RollbackSnapshot& snap = m_val_snapshots[slot];
  CaptureFullRamSnapshot(snap);
#endif
}

bool RollbackManager::LoadFrame(Core::System& system, int frames_back)
{
  ROLLBACK_ZONE();
  if (!m_initialized)
    return false;

  ASSERT(frames_back >= 1 && m_ring_count >= 2 && frames_back < m_ring_count &&
         frames_back <= Rollback::NUM_SAVE_SLOTS - 1);

  {
    ROLLBACK_ZONE_N("Preserve stack");
    // Preserve the live call stack in RAM so execution continues normally
    // after this load returns.  On PPC, r1 is the stack pointer and active
    // frames live at addresses >= r1 (callers are above the current frame).
    // We exclude those physical pages from the RAM restore so the function
    // call chain that issued CMD_LOAD_SAVESTATE stays intact.
    //
    // Use the OS thread struct to find the exact stack top rather than
    // assuming a fixed exclusion size (which could under- or over-cover).
    // DAT_800000e4 (physical 0xe4) = current OSThread pointer (virtual).
    // OSThread+0x304 = initialStackAddr (the HIGH end of the stack buffer).
    const uint32_t r1_virt = system.GetPowerPC().GetPPCState().gpr[1];
    const uint32_t r1_phys = r1_virt & 0x1FFF'FFFFu;
    const uint32_t stack_page = r1_phys & ~(static_cast<uint32_t>(PAGE_SIZE) - 1u);

    uint32_t stack_exclude_end = 0;
    ASSERT(m_mem1_size > 0xe4 + 4);
    uint32_t thread_virt;
    std::memcpy(&thread_virt, m_mem1_ptr + 0xe4, 4);
    thread_virt = Common::swap32(thread_virt);
    const uint32_t thread_phys = thread_virt & 0x1FFF'FFFFu;
    if (thread_phys + 0x308 <= m_mem1_size)
    {
      uint32_t stack_top_virt;
      std::memcpy(&stack_top_virt, m_mem1_ptr + thread_phys + 0x304, 4);
      stack_top_virt = Common::swap32(stack_top_virt);
      const uint32_t stack_top_phys = stack_top_virt & 0x1FFF'FFFFu;
      // Round up to next page boundary so the top page is fully covered.
      const uint32_t stack_top_page = (stack_top_phys + static_cast<uint32_t>(PAGE_SIZE) - 1u) &
                                      ~(static_cast<uint32_t>(PAGE_SIZE) - 1u);
      if (stack_top_page > stack_page && stack_top_page <= static_cast<uint32_t>(m_mem1_size))
        stack_exclude_end = stack_top_page;
    }
    ASSERT(stack_exclude_end != 0);

    /*INFO_LOG_FMT(BRAWLBACK,
                 "[Rollback] stack exclude: r1=0x{:08x} phys=[0x{:08x}, 0x{:08x}) ({} KB)", r1_virt,
                 stack_page, stack_exclude_end, (stack_exclude_end - stack_page) / 1024);*/
    m_exclude_regions.push_back(MemoryRegion{stack_page, stack_exclude_end});
  }

  const int most_recent = Wrap(m_ring_next - 1, NUM_SAVE_SLOTS);

  const int target_slot = Wrap(most_recent - frames_back, NUM_SAVE_SLOTS);

  constexpr u32 BASE_SNAPSHOT_SENTINEL = UINT32_MAX;

  struct SourceEntry
  {
    // source slot index, or BASE_SNAPSHOT_SENTINEL
    u32 slot;
    // position within delta.page_indices/page_data
    u32 local_idx;
  };

  // oldest slot currently alive in the ring
  const int oldest_ring_slot = Wrap(m_ring_next - m_ring_count, NUM_SAVE_SLOTS);

  static std::unordered_map<u32, SourceEntry> sourceDataToRestore;

  DeltaSaveSlot& deltaSave = m_slots[target_slot];

  // indexing + RAM restore happens on a worker thread so they overlap with DoState on the main
  // thread
  job::Job* ram_job = job::KickRootJob(m_dispatch_thread, [&](job::JobTaskThread& w, job::Job& j) {
    u32 remaining = 0;
    {
      ROLLBACK_ZONE_N("ram page indexing - forward");
      std::memset(m_needs_source_mem1.data(), 0, m_needs_source_mem1.size());
      std::memset(m_needs_source_mem2.data(), 0, m_needs_source_mem2.size());

      for (int n = 0; n <= frames_back; n++)
      {
        const int slot = Wrap(target_slot + n, NUM_SAVE_SLOTS);
        const RegionDelta& d1 = m_slots[slot].m_mem1_delta;
        for (u32 i = 0; i < d1.page_count; i++)
        {
          const u16 idx = d1.page_indices[i];
          if (!m_needs_source_mem1[idx])
          {
            m_needs_source_mem1[idx] = 1;
            remaining++;
          }
        }
        const RegionDelta& d2 = m_slots[slot].m_mem2_delta;
        for (u32 i = 0; i < d2.page_count; i++)
        {
          const u16 idx = d2.page_indices[i];
          if (!m_needs_source_mem2[idx])
          {
            m_needs_source_mem2[idx] = 1;
            remaining++;
          }
        }
      }
    }

    // Walk from target_slot toward oldest. For each slot, satisfy any still-needed
    // pages found there
    {
      ROLLBACK_ZONE_N("ram page indexing - backward");
      sourceDataToRestore.clear();
      sourceDataToRestore.reserve(remaining);

      for (int slot = target_slot;; slot = Wrap(slot - 1, NUM_SAVE_SLOTS))
      {
        const RegionDelta& d1 = m_slots[slot].m_mem1_delta;
        for (u32 i = 0; i < d1.page_count; i++)
        {
          const u16 idx = d1.page_indices[i];
          if (m_needs_source_mem1[idx])
          {
            m_needs_source_mem1[idx] = 0;
            sourceDataToRestore[idx] = {static_cast<u32>(slot), i};
            remaining--;
          }
        }
        const RegionDelta& d2 = m_slots[slot].m_mem2_delta;
        for (u32 i = 0; i < d2.page_count; i++)
        {
          const u16 idx = d2.page_indices[i];
          if (m_needs_source_mem2[idx])
          {
            m_needs_source_mem2[idx] = 0;
            sourceDataToRestore[MEM2_FIRST_PAGE + idx] = {static_cast<u32>(slot), i};
            remaining--;
          }
        }
        if (remaining == 0 || slot == oldest_ring_slot)
          break;
      }

      // Any pages still marked have no delta anywhere in the ring — use the base snapshot
      for (u32 i = 0; i < static_cast<u32>(m_needs_source_mem1.size()); i++)
      {
        if (m_needs_source_mem1[i])
          sourceDataToRestore[i] = {BASE_SNAPSHOT_SENTINEL, 0};
      }
      for (u32 i = 0; i < static_cast<u32>(m_needs_source_mem2.size()); i++)
      {
        if (m_needs_source_mem2[i])
          sourceDataToRestore[MEM2_FIRST_PAGE + i] = {BASE_SNAPSHOT_SENTINEL, 0};
      }
    }

    {
      ROLLBACK_ZONE_N("ram page restore");
#if defined(ROLLBACK_PROFILE_TRACY)
      auto x = StringFromFormat("Restored %u pages", sourceDataToRestore.size());
      ZoneText(x.c_str(), x.size());
#endif

      // Create jobs for parallel RAM page restoration using the work-stealing job system.
      // Each iteration writes a distinct page so this is safe to run concurrently.
      std::vector<job::Job*> page_jobs;
      page_jobs.reserve(sourceDataToRestore.size());

      for (auto const& kv : sourceDataToRestore)
      {
        page_jobs.push_back(w.create_job_as_child(
            j, [this, page_key = kv.first, source_entry = kv.second](job::JobTaskThread&, job::Job&) {
              const bool isMem2 = (page_key >= MEM2_FIRST_PAGE);
              const u32 local_page = isMem2 ? (page_key - MEM2_FIRST_PAGE) : page_key;
              uint8_t* const dst =
                  (isMem2 ? m_mem2_ptr : m_mem1_ptr) + static_cast<size_t>(local_page) * PAGE_SIZE;
              const uint32_t dst_phys =
                  (isMem2 ? MEM2_BASE : 0u) + local_page * static_cast<uint32_t>(PAGE_SIZE);

              const uint8_t* src;
              if (source_entry.slot == BASE_SNAPSHOT_SENTINEL)
              {
                const uint8_t* const snap_base =
                    isMem2 ? m_base_snapshot.mem2.get() : m_base_snapshot.mem1.get();
                src = snap_base + static_cast<size_t>(local_page) * PAGE_SIZE;
              }
              else
              {
                const Rollback::RegionDelta& src_delta =
                    isMem2 ? m_slots[source_entry.slot].m_mem2_delta : m_slots[source_entry.slot].m_mem1_delta;
                src = src_delta.page_data.data() + static_cast<size_t>(source_entry.local_idx) * PAGE_SIZE;
              }

              savestateMemcpy(dst, src, PAGE_SIZE, dst_phys, m_exclude_regions);
            }));
      }

      // Kick all page restore jobs
      if (!page_jobs.empty())
        w.do_work_and_kick_jobs(page_jobs.data(), static_cast<uint16_t>(page_jobs.size()));
    }
  });

  bool ok = false;
  {
    ROLLBACK_ZONE_N("DoState restore");
    BeginDoState();
    ok = State::LoadFromBuffer(
        system, std::span<uint8_t>(deltaSave.m_save_buffer.data(), deltaSave.m_save_buffer.size()));
    EndDoState();
  }

  {
    ROLLBACK_ZONE_N("L1 cache restore");
    if (m_l1_cache_ptr && m_l1_cache_size > 0 && deltaSave.m_l1_cache_snapshot.data())
      std::memcpy(m_l1_cache_ptr, deltaSave.m_l1_cache_snapshot.data(), m_l1_cache_size);
  }

  job::DrainJobsUntilComplete(m_dispatch_thread, ram_job);

#if ROLLBACK_VALIDATE
  CompareValSnapshot(target_slot, frames_back);
#endif

  // Remove the temporary live-stack exclusion (always the last element pushed).
  m_exclude_regions.pop_back();

  {
    ROLLBACK_ZONE_N("Clear JIT dirty bitmap");
    auto& bitmap = JITDirtyBitmap::Get();
    bitmap.ClearRange(0, static_cast<uint32_t>(m_mem1_size / PAGE_SIZE));
    if (m_mem2_ptr && m_mem2_size > 0)
      bitmap.ClearRange(MEM2_FIRST_PAGE, static_cast<uint32_t>(m_mem2_size / PAGE_SIZE));
  }

  // After loading, the target slot becomes the new "oldest" slot,
  // so the next save will overwrite the next slot.
  m_ring_next = Wrap(target_slot + 1, NUM_SAVE_SLOTS);
  m_ring_count = m_ring_count - frames_back;

  if (ok)
  {
    ROLLBACK_ZONE_N("log");
    const u32 loaded_frame = m_slots[target_slot].brawl_frame;
    INFO_LOG_FMT(BRAWLBACK, "Rolled back {} frame(s) - loaded slot {} (frame {})", 
                 frames_back, target_slot, loaded_frame);
  }
  else
  {
    ERROR_LOG_FMT(BRAWLBACK, "Rollback failed");
    OSD::AddMessage("Rollback state load failed", 3000, OSD::Color::RED);
  }
  return ok;
}

}  // namespace Rollback
