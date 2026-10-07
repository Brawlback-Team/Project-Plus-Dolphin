// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/DirtyPages.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include <xxh3.h>

#include "Common/Buffer.h"
#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/Rollback/DirtyBitmap.h"
#include "Core/Rollback/UndoLog.h"
#include "Core/System.h"

namespace Rollback::DirtyPages
{
namespace
{
constexpr std::size_t PAGE = DIRTY_PAGE_SIZE;
constexpr u32 MEM2_PHYSICAL = 0x10000000u;
// Spare page buffers kept ready at each snapshot.
constexpr std::size_t RESERVE_PAGES = 512;

// The JIT's inline bitmap stores exist only in the x86-64 backend. Other hosts keep full copies.
#if defined(_M_X86_64) || defined(__x86_64__)
constexpr bool JIT_STORES_TRACKED = true;
#else
constexpr bool JIT_STORES_TRACKED = false;
#endif

struct Tracker
{
  std::mutex lock;
  const void* owner = nullptr;
  std::vector<Area> areas;
  std::vector<std::size_t> area_first_page;  // global index of each area's first page
  std::size_t page_count = 0;
  // RAM as it was at the newest snapshot, per area. Pages the bitmap marks are compared with it.
  std::vector<Common::UniqueBuffer<u8>> mirrors;
  std::unique_ptr<UndoLog> log;
  Counters counters;
  // Ids stay unique across arms, so a stale id never names another ring's snapshot.
  u64 next_id = 1;
};

// Never destroyed: a static destructor may still use it at exit.
Tracker& T()
{
  static Tracker* const tracker = new Tracker;
  return *tracker;
}

// The area that holds global page `page`.
std::size_t AreaOf(const Tracker& t, std::size_t page)
{
  return static_cast<std::size_t>(
      std::upper_bound(t.area_first_page.begin(), t.area_first_page.end(), page) -
      t.area_first_page.begin() - 1);
}

u32 PhysicalOf(const Area& area, std::size_t page_in_area)
{
  return area.physical_address + static_cast<u32>(page_in_area * PAGE);
}

// Copies `contents` over the live page at `live` if they differ, reporting the change.
void ReplacePage(u8* live, const u8* contents, u32 physical,
                 const std::function<void(u32, u32)>& changed)
{
  if (std::memcmp(live, contents, PAGE) == 0)
    return;
  std::memcpy(live, contents, PAGE);
  changed(physical, static_cast<u32>(PAGE));
}

// Settles the pages written since the newest snapshot. A page whose bytes changed keeps its old
// bytes (the mirror's) in the newest log, and then the mirror takes the live bytes. With no
// snapshot there is no log to keep them in, so only the mirror moves.
void CommitDirtyPages(Tracker& t)
{
  auto& bitmap = JITDirtyBitmap::Get();
  for (std::size_t a = 0; a < t.areas.size(); ++a)
  {
    const Area& area = t.areas[a];
    const std::size_t pages = area.size / PAGE;
    const std::size_t first_bitmap_page = area.physical_address / PAGE;
    for (std::size_t i = 0; i < pages; ++i)
    {
      if (!bitmap.Consume(first_bitmap_page + i))
        continue;
      ++t.counters.dirty_pages;
      u8* const live = area.alias + i * PAGE;
      u8* const mirror = t.mirrors[a].data() + i * PAGE;
      if (std::memcmp(live, mirror, PAGE) == 0)
        continue;
      if (t.log->Record(t.area_first_page[a] + i, mirror))
        ++t.counters.pages_recorded;
      std::memcpy(mirror, live, PAGE);
    }
  }
}

void ResetLocked(Tracker& t)
{
  JITDirtyBitmap::Get().SetEnabled(false);
  if (t.log)
    t.next_id = t.log->NextId();
  t.owner = nullptr;
  t.areas.clear();
  t.area_first_page.clear();
  t.page_count = 0;
  t.mirrors.clear();
  t.log.reset();
}
}  // namespace

bool Arm(const void* owner, const std::vector<Area>& areas)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (t.owner)
    return t.owner == owner;
  if (!JIT_STORES_TRACKED || areas.empty())
    return false;

  t.areas = areas;
  t.area_first_page.clear();
  t.page_count = 0;
  for (const Area& area : areas)
  {
    if (!area.alias || area.size % PAGE != 0 || area.physical_address % PAGE != 0)
    {
      ResetLocked(t);
      return false;
    }
    t.area_first_page.push_back(t.page_count);
    t.page_count += area.size / PAGE;
  }
  t.mirrors.clear();
  for (const Area& area : areas)
  {
    t.mirrors.emplace_back();
    t.mirrors.back().reset(area.size);
    std::memcpy(t.mirrors.back().data(), area.alias, area.size);
  }
  t.log = std::make_unique<UndoLog>(PAGE, t.page_count, t.next_id);
  t.log->Reserve(RESERVE_PAGES);
  t.owner = owner;
  // Everything written before this point is already in the mirror.
  JITDirtyBitmap::Get().Clear();
  JITDirtyBitmap::Get().SetEnabled(true);
  NOTICE_LOG_FMT(CORE, "Rollback: dirty-page snapshots over {} KB of guest RAM",
                 t.page_count * PAGE / 1024);
  return true;
}

bool ArmForSystem(Core::System& system, const void* owner)
{
  if (!JIT_STORES_TRACKED)
    return false;
  auto& memory = system.GetMemory();
  // Page-table mappings reach RAM at addresses the bitmap does not index by physical page.
  if (memory.HasNonCanonicalMappingsForRollback())
  {
    NOTICE_LOG_FMT(CORE, "Rollback: dirty-page tracking off (noncanonical RAM mapping); using "
                         "full-copy snapshots");
    return false;
  }
  std::vector<Area> areas;
  u8* const alias = memory.GetRollbackAlias(false);
  if (!alias)
  {
    NOTICE_LOG_FMT(CORE, "Rollback: dirty-page tracking off (no RAM alias); using full copies");
    return false;
  }
  areas.push_back(Area{alias, 0, memory.GetRamSize()});
  if (memory.GetEXRAM())
  {
    u8* const exram_alias = memory.GetRollbackAlias(true);
    if (!exram_alias)
    {
      NOTICE_LOG_FMT(CORE, "Rollback: dirty-page tracking off (no MEM2 alias); using full copies");
      return false;
    }
    areas.push_back(Area{exram_alias, MEM2_PHYSICAL, memory.GetExRamSize()});
  }
  return Arm(owner, areas);
}

void Disarm(const void* owner)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.owner || t.owner != owner)
    return;
  ResetLocked(t);
}

bool IsArmedFor(const void* owner)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  return t.owner && t.owner == owner;
}

u64 Snapshot()
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log)
    return 0;
  t.log->Reserve(RESERVE_PAGES);
  // The pages written since the previous snapshot belong to that snapshot's log, so commit them
  // before the new log opens.
  CommitDirtyPages(t);
  return t.log->Open();
}

bool Has(u64 id)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  return t.log && t.log->Has(id);
}

bool Restore(u64 id, const std::function<void(u32 physical_address, u32 length)>& changed)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log || !t.log->Has(id))
    return false;
  // Pages with a pre-image in the logs from `id` on hold their bytes at `id` there.
  std::vector<u8> logged(t.page_count, 0);
  t.log->ForEachPreImage(id, [&](std::size_t page, const u8* pre_image) {
    logged[page] = 1;
    const std::size_t a = AreaOf(t, page);
    const Area& area = t.areas[a];
    const std::size_t i = page - t.area_first_page[a];
    ReplacePage(area.alias + i * PAGE, pre_image, PhysicalOf(area, i), changed);
    std::memcpy(t.mirrors[a].data() + i * PAGE, pre_image, PAGE);
  });
  // A page without a pre-image has not changed since the newest snapshot, so the mirror holds its
  // bytes at `id`. That includes pages written since then, which are still only in the bitmap.
  auto& bitmap = JITDirtyBitmap::Get();
  for (std::size_t a = 0; a < t.areas.size(); ++a)
  {
    const Area& area = t.areas[a];
    const std::size_t pages = area.size / PAGE;
    const std::size_t first_bitmap_page = area.physical_address / PAGE;
    for (std::size_t i = 0; i < pages; ++i)
    {
      if (!bitmap.Load(first_bitmap_page + i) || logged[t.area_first_page[a] + i])
        continue;
      ReplacePage(area.alias + i * PAGE, t.mirrors[a].data() + i * PAGE, PhysicalOf(area, i),
                  changed);
    }
  }
  // Live RAM now matches the mirror again, so nothing is dirty.
  bitmap.Clear();
  t.log->RewindTo(id);
  return true;
}

void Drop(u64 id)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (t.log)
    t.log->Drop(id);
}

std::optional<u64> Checksum(u64 id)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log || !t.log->Has(id))
    return std::nullopt;
  std::vector<const u8*> pre_images(t.page_count, nullptr);
  t.log->ForEachPreImage(id, [&](std::size_t page, const u8* data) { pre_images[page] = data; });

  // Must hash the same bytes in the same order as Rollback::RamChecksum over MEM1 and MEM2.
  XXH3_state_t* const state = XXH3_createState();
  XXH3_64bits_reset(state);
  for (std::size_t a = 0; a < t.areas.size(); ++a)
  {
    const std::size_t first = t.area_first_page[a];
    const std::size_t count = t.areas[a].size / PAGE;
    for (std::size_t i = 0; i < count; ++i)
    {
      const u8* const pre_image = pre_images[first + i];
      const u8* const data = pre_image ? pre_image : t.mirrors[a].data() + i * PAGE;
      XXH3_64bits_update(state, data, PAGE);
    }
  }
  const u64 hash = XXH3_64bits_digest(state);
  XXH3_freeState(state);
  return hash;
}

std::optional<std::vector<u64>> PageChecksums(u64 id)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log || !t.log->Has(id))
    return std::nullopt;

  std::vector<const u8*> pre_images(t.page_count, nullptr);
  t.log->ForEachPreImage(id, [&](std::size_t page, const u8* data) { pre_images[page] = data; });

  std::vector<u64> hashes;
  hashes.reserve(t.page_count);
  for (std::size_t a = 0; a < t.areas.size(); ++a)
  {
    const std::size_t first = t.area_first_page[a];
    const std::size_t count = t.areas[a].size / PAGE;
    for (std::size_t i = 0; i < count; ++i)
    {
      const u8* const pre_image = pre_images[first + i];
      const u8* const data = pre_image ? pre_image : t.mirrors[a].data() + i * PAGE;
      hashes.push_back(XXH3_64bits(data, PAGE));
    }
  }
  return hashes;
}

void OnMappingsChanged(Core::System& system)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log)
    return;
  if (system.GetMemory().HasNonCanonicalMappingsForRollback())
  {
    // The JIT bitmap indexes a masked effective address. Arbitrary BAT or page-table mappings do
    // not preserve that relationship, so no tracked snapshot can still be trusted.
    ERROR_LOG_FMT(CORE, "Rollback: guest RAM gained a noncanonical mapping; every dirty-page "
                        "snapshot is dropped");
    ResetLocked(t);
  }
}

void StopTracking()
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  ResetLocked(t);
}

Counters GetCounters()
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  return t.counters;
}
}  // namespace Rollback::DirtyPages
