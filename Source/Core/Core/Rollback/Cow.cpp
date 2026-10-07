// Copyright 2026 YouGame
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/Cow.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

#include <xxh3.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "Common/Logging/Log.h"
#include "Core/HW/Memmap.h"
#include "Core/MemTools.h"
#include "Core/Rollback/UndoLog.h"
#include "Core/System.h"

namespace Rollback::Cow
{
namespace
{
// Restore compares, copies and reports changed RAM in blocks of this size (the JIT is invalidated
// per changed block).
constexpr std::size_t RESTORE_BLOCK = 4096;
constexpr u32 MEM2_PHYSICAL = 0x10000000u;
constexpr std::size_t MAX_VIEWS = 64;
// The widest single host store (AVX-512, arm64 DC ZVA).
constexpr std::size_t MAX_ACCESS = 64;
// Spare page buffers kept ready at each snapshot so a fault rarely allocates.
constexpr std::size_t RESERVE_PAGES = 512;

// Hot pages. A fault costs far more than a page copy (~12 us vs under 1 us for 16 KB on Apple
// Silicon), and games write mostly the same pages every frame. So a recently written page stays
// writable and each snapshot copies it up front. A page's heat is how many more snapshots it stays
// hot: set on a fault, raised to the max while its bytes keep changing, and counting down while
// they don't. At zero it is protected again. YG_COW_FAULT_HEAT and YG_COW_HEAT (0-255) override
// the defaults for tuning.
int EnvHeat(const char* name, int fallback)
{
  const char* value = std::getenv(name);
  return value && *value ? std::clamp(std::atoi(value), 0, 255) : fallback;
}
const u8 HEAT_AFTER_FAULT = static_cast<u8>(EnvHeat("YG_COW_FAULT_HEAT", 64));
const u8 HEAT_WHILE_CHANGING = static_cast<u8>(EnvHeat("YG_COW_HEAT", 255));

struct ViewInfo
{
  u8* base;
  u8* end;
  std::size_t first_page;
};

struct Tracker
{
  std::mutex lock;
  const void* owner = nullptr;
  std::size_t page_size = 0;
  std::vector<Area> areas;
  std::vector<std::size_t> area_first_page;  // global index of each area's first page
  std::vector<ViewInfo> views;               // writeable views only
  std::unique_ptr<UndoLog> log;
  // Per page: bit v set when the page is writable in views[v]. Only pages in the newest log may be.
  std::vector<u64> open_in;
  std::vector<u32> open_pages;  // pages whose open_in is nonzero
  // Per page: remaining heat, and whether a fault (not a snapshot copy) saved it since the newest
  // snapshot.
  std::vector<u8> heat;
  std::vector<u8> fault_recorded;
  Counters counters;
  // Ids stay unique across arms, so a stale id never names another ring's snapshot.
  u64 next_id = 1;
};

// Never destroyed: a fault or a static destructor may still use it at exit.
Tracker& T()
{
  static Tracker* const tracker = new Tracker;
  return *tracker;
}

// HandleFault's lock-free early out.
std::atomic<bool> s_armed{false};

std::size_t HostPageSize()
{
#ifdef _WIN32
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwPageSize;
#else
  return static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
}

bool Protect(u8* address, std::size_t size, bool writable)
{
#ifdef _WIN32
  DWORD old_protect;
  if (VirtualProtect(address, size, writable ? PAGE_READWRITE : PAGE_READONLY, &old_protect))
    return true;
#else
  if (mprotect(address, size, writable ? (PROT_READ | PROT_WRITE) : PROT_READ) == 0)
    return true;
#endif
  ERROR_LOG_FMT(MEMMAP, "Rollback COW: could not make {} bytes at {} {}", size, fmt::ptr(address),
                writable ? "writable" : "read-only");
  return false;
}

u8* AliasOf(const Tracker& t, std::size_t page)
{
  const std::size_t area = static_cast<std::size_t>(
      std::upper_bound(t.area_first_page.begin(), t.area_first_page.end(), page) -
      t.area_first_page.begin() - 1);
  return t.areas[area].alias + (page - t.area_first_page[area]) * t.page_size;
}

u32 PhysicalOf(const Tracker& t, std::size_t page)
{
  const std::size_t area = static_cast<std::size_t>(
      std::upper_bound(t.area_first_page.begin(), t.area_first_page.end(), page) -
      t.area_first_page.begin() - 1);
  return t.areas[area].physical_address +
         static_cast<u32>((page - t.area_first_page[area]) * t.page_size);
}

// Clips the writeable views to the tracked areas, as page ranges. False if one is not
// page-aligned or there are too many.
bool BuildViews(const Tracker& t, const std::vector<View>& views, std::vector<ViewInfo>* out)
{
  out->clear();
  for (const View& view : views)
  {
    if (!view.writeable || !view.base)
      continue;
    for (std::size_t a = 0; a < t.areas.size(); ++a)
    {
      const Area& area = t.areas[a];
      const u64 start = std::max<u64>(view.physical_address, area.physical_address);
      const u64 end = std::min<u64>(u64{view.physical_address} + view.size,
                                    u64{area.physical_address} + area.size);
      if (start >= end)
        continue;
      u8* const base = view.base + (start - view.physical_address);
      if ((start - area.physical_address) % t.page_size != 0 ||
          (end - start) % t.page_size != 0 || reinterpret_cast<uintptr_t>(base) % t.page_size != 0)
      {
        ERROR_LOG_FMT(MEMMAP, "Rollback COW: a view of {:08x}..{:08x} is not page-aligned", start,
                      end);
        return false;
      }
      out->push_back(ViewInfo{base, base + (end - start),
                              t.area_first_page[a] + (start - area.physical_address) / t.page_size});
    }
  }
  if (out->size() > MAX_VIEWS)
  {
    ERROR_LOG_FMT(MEMMAP, "Rollback COW: {} views of guest RAM, at most {} supported", out->size(),
                  MAX_VIEWS);
    return false;
  }
  return true;
}

// Makes every page read-only in every view.
bool ProtectAll(Tracker& t)
{
  bool ok = true;
  for (const ViewInfo& view : t.views)
    ok &= Protect(view.base, static_cast<std::size_t>(view.end - view.base), false);
  std::fill(t.open_in.begin(), t.open_in.end(), 0);
  t.open_pages.clear();
  return ok;
}

// Re-protects `pages` (open, sorted) in each view where they are writable, one call per run of
// adjacent pages.
void Reprotect(Tracker& t, const std::vector<u32>& pages)
{
  for (std::size_t v = 0; v < t.views.size(); ++v)
  {
    const u64 bit = u64{1} << v;
    const ViewInfo& view = t.views[v];
    std::size_t i = 0;
    while (i < pages.size())
    {
      if (!(t.open_in[pages[i]] & bit))
      {
        ++i;
        continue;
      }
      const std::size_t first = pages[i];
      std::size_t last = first;
      ++i;
      while (i < pages.size() && pages[i] == last + 1 && (t.open_in[pages[i]] & bit))
      {
        last = pages[i];
        ++i;
      }
      Protect(view.base + (first - view.first_page) * t.page_size,
              (last - first + 1) * t.page_size, false);
    }
  }
  for (const u32 page : pages)
    t.open_in[page] = 0;
  t.counters.pages_reprotected += pages.size();
}

// At a snapshot or restore: decides which open pages stay hot and re-protects the rest.
// `measure` updates heat by comparing each page with its saved copy; a restore passes false because
// it has just discarded those copies.
void SettleOpenPages(Tracker& t, bool measure)
{
  std::vector<u32> hot, cold;
  for (const u32 page : t.open_pages)
  {
    // Pages saved by a fault keep the heat the fault gave them.
    if (measure && !t.fault_recorded[page])
    {
      const u8* const pre_image = t.log->NewestPreImage(page);
      const bool changed =
          pre_image && std::memcmp(pre_image, AliasOf(t, page), t.page_size) != 0;
      t.heat[page] =
          changed ? HEAT_WHILE_CHANGING : static_cast<u8>(t.heat[page] > 0 ? t.heat[page] - 1 : 0);
    }
    t.fault_recorded[page] = 0;
    (t.heat[page] > 0 ? hot : cold).push_back(page);
  }
  std::sort(cold.begin(), cold.end());
  Reprotect(t, cold);
  t.open_pages = std::move(hot);
}

// Saves the hot pages into the newest log now, since they stay writable and will not fault.
void CopyHotPages(Tracker& t)
{
  for (const u32 page : t.open_pages)
  {
    if (t.log->Record(page, AliasOf(t, page)))
      ++t.counters.pages_copied;
  }
}

// Saves `page` into the newest log (if not saved yet) and makes it writable in view `v`. False if
// it was already writable there, meaning the fault was not caused by our protection.
bool OpenPage(Tracker& t, std::size_t v, std::size_t page)
{
  const u64 bit = u64{1} << v;
  if (t.open_in[page] & bit)
    return false;
  if (t.log->Record(page, AliasOf(t, page)))
  {
    ++t.counters.pages_recorded;
    t.fault_recorded[page] = 1;
    t.heat[page] = std::max(t.heat[page], HEAT_AFTER_FAULT);
  }
  const ViewInfo& view = t.views[v];
  if (!Protect(view.base + (page - view.first_page) * t.page_size, t.page_size, true))
    return false;
  if (t.open_in[page] == 0)
    t.open_pages.push_back(static_cast<u32>(page));
  t.open_in[page] |= bit;
  return true;
}

void UnprotectAll(Tracker& t)
{
  for (const ViewInfo& view : t.views)
    Protect(view.base, static_cast<std::size_t>(view.end - view.base), true);
}

void ResetLocked(Tracker& t)
{
  s_armed.store(false, std::memory_order_release);
  if (t.log)
    t.next_id = t.log->NextId();
  t.owner = nullptr;
  t.views.clear();
  t.areas.clear();
  t.area_first_page.clear();
  t.open_in.clear();
  t.open_pages.clear();
  t.heat.clear();
  t.fault_recorded.clear();
  t.log.reset();
}
}  // namespace

std::size_t PageSize()
{
  static const std::size_t page_size = HostPageSize();
  return page_size;
}

bool Arm(const void* owner, const std::vector<Area>& areas, const std::vector<View>& views)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (t.owner)
    return t.owner == owner;
  if (!EMM::IsExceptionHandlerSupported() || areas.empty())
    return false;

  t.page_size = PageSize();
  std::size_t pages = 0;
  for (const Area& area : areas)
  {
    if (!area.alias || area.size % t.page_size != 0 || area.physical_address % t.page_size != 0)
    {
      ResetLocked(t);
      return false;
    }
    t.area_first_page.push_back(pages);
    pages += area.size / t.page_size;
  }
  t.areas = areas;
  if (!BuildViews(t, views, &t.views))
  {
    ResetLocked(t);
    return false;
  }
  t.log = std::make_unique<UndoLog>(t.page_size, pages, t.next_id);
  t.log->Reserve(RESERVE_PAGES);
  t.open_in.assign(pages, 0);
  t.open_pages.reserve(pages);
  t.heat.assign(pages, 0);
  t.fault_recorded.assign(pages, 0);
  t.owner = owner;
  // The handler must be installed and s_armed set before any page is protected.
  EMM::InstallCowFallbackHandler();
  s_armed.store(true, std::memory_order_release);
  if (!ProtectAll(t))
  {
    UnprotectAll(t);
    ResetLocked(t);
    return false;
  }
  NOTICE_LOG_FMT(CORE, "Rollback: copy-on-write snapshots over {} KB pages, {} views",
                 t.page_size / 1024, t.views.size());
  return true;
}

bool ArmForSystem(Core::System& system, const void* owner)
{
  auto& memory = system.GetMemory();
  std::vector<Area> areas;
  if (u8* alias = memory.GetRollbackAlias(false))
    areas.push_back(Area{alias, 0, memory.GetRamSize()});
  else
    return false;
  // Page-table mappings (MMU emulation) would mean far too many views, each change re-protecting
  // all of RAM.
  if (memory.HasPageTableMappings())
    return false;
  if (memory.GetEXRAM())
  {
    u8* alias = memory.GetRollbackAlias(true);
    if (!alias)
      return false;
    areas.push_back(Area{alias, MEM2_PHYSICAL, memory.GetExRamSize()});
  }
  std::vector<View> views;
  for (const auto& view : memory.GetGuestRamViews())
    views.push_back(View{view.base, view.physical_address, view.size, view.writeable});
  return Arm(owner, areas, views);
}

void Disarm(const void* owner)
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.owner || t.owner != owner)
    return;
  UnprotectAll(t);
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
  t.log->Reserve(t.open_pages.size() + RESERVE_PAGES);
  SettleOpenPages(t, true);
  const u64 id = t.log->Open();
  CopyHotPages(t);
  return id;
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
  // Writes go through the private mapping, so protection does not matter here.
  t.log->ForEachPreImage(id, [&](std::size_t page, const u8* pre_image) {
    u8* const live = AliasOf(t, page);
    const u32 physical = PhysicalOf(t, page);
    for (std::size_t offset = 0; offset < t.page_size; offset += RESTORE_BLOCK)
    {
      if (std::memcmp(live + offset, pre_image + offset, RESTORE_BLOCK) == 0)
        continue;
      std::memcpy(live + offset, pre_image + offset, RESTORE_BLOCK);
      changed(physical + static_cast<u32>(offset), static_cast<u32>(RESTORE_BLOCK));
    }
  });
  t.log->RewindTo(id);
  // `id`'s log is now empty: save the hot pages into it and re-protect the rest.
  SettleOpenPages(t, false);
  CopyHotPages(t);
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
  std::vector<const u8*> pre_images(t.log->PageCount(), nullptr);
  t.log->ForEachPreImage(id, [&](std::size_t page, const u8* data) { pre_images[page] = data; });

  // Must hash the same bytes in the same order as Rollback::RamChecksum over MEM1 and MEM2.
  XXH3_state_t* const state = XXH3_createState();
  XXH3_64bits_reset(state);
  for (std::size_t a = 0; a < t.areas.size(); ++a)
  {
    const std::size_t first = t.area_first_page[a];
    const std::size_t count = t.areas[a].size / t.page_size;
    std::size_t run = 0;  // live pages not yet hashed
    for (std::size_t i = 0; i <= count; ++i)
    {
      const u8* const pre_image = i < count ? pre_images[first + i] : nullptr;
      if (i < count && !pre_image)
      {
        ++run;
        continue;
      }
      if (run > 0)
      {
        XXH3_64bits_update(state, t.areas[a].alias + (i - run) * t.page_size, run * t.page_size);
        run = 0;
      }
      if (pre_image)
        XXH3_64bits_update(state, pre_image, t.page_size);
    }
  }
  const u64 hash = XXH3_64bits_digest(state);
  XXH3_freeState(state);
  return hash;
}

bool HandleFault(uintptr_t address)
{
  if (!s_armed.load(std::memory_order_acquire))
    return false;
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  // Tracking stopped while we waited for the lock and every page is writable again, so retry the
  // write. A fault that wasn't ours will come back and find tracking off.
  if (!t.log)
    return true;
  u8* const at = reinterpret_cast<u8*>(address);
  for (std::size_t v = 0; v < t.views.size(); ++v)
  {
    const ViewInfo& view = t.views[v];
    if (at < view.base || at >= view.end)
      continue;
    const std::size_t offset = static_cast<std::size_t>(at - view.base);
    const std::size_t index = offset / t.page_size;
    const std::size_t in_page = offset % t.page_size;
    const std::size_t last = static_cast<std::size_t>(view.end - view.base) / t.page_size - 1;
    // A store straddling two pages may report an address in either one (arm64 allows that), so if
    // this page is already writable, try a protected neighbour within one access.
    if (OpenPage(t, v, view.first_page + index) ||
        (in_page >= t.page_size - MAX_ACCESS && index < last &&
         OpenPage(t, v, view.first_page + index + 1)) ||
        (in_page < MAX_ACCESS && index > 0 && OpenPage(t, v, view.first_page + index - 1)))
    {
      ++t.counters.faults;
      return true;
    }
    // Already writable with no protected neighbour: another thread opened it while we waited, so
    // retry. Not near the view's ends, though, where the fault may be fastmem's.
    return (t.open_in[view.first_page + index] & (u64{1} << v)) &&
           offset >= MAX_ACCESS &&
           offset + MAX_ACCESS <= static_cast<std::size_t>(view.end - view.base);
  }
  return false;
}

void PrepareHostWrite(const void* ptr, std::size_t size)
{
  if (!s_armed.load(std::memory_order_acquire) || size == 0)
    return;
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log)
    return;
  const u8* const start = static_cast<const u8*>(ptr);
  for (std::size_t v = 0; v < t.views.size(); ++v)
  {
    const ViewInfo& view = t.views[v];
    if (start < view.base || start >= view.end)
      continue;
    const u8* const end = std::min<const u8*>(start + size, view.end);
    const std::size_t first = static_cast<std::size_t>(start - view.base) / t.page_size;
    const std::size_t last = static_cast<std::size_t>(end - 1 - view.base) / t.page_size;
    for (std::size_t page = first; page <= last; ++page)
      OpenPage(t, v, view.first_page + page);
    return;
  }
}

void OnMappingsChanged(Core::System& system)
{
  if (!s_armed.load(std::memory_order_acquire))
    return;
  std::vector<View> views;
  for (const auto& view : system.GetMemory().GetGuestRamViews())
    views.push_back(View{view.base, view.physical_address, view.size, view.writeable});
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  if (!t.log)
    return;
  std::vector<ViewInfo> rebuilt;
  const bool built = BuildViews(t, views, &rebuilt);
  const auto same = [](const ViewInfo& a, const ViewInfo& b) {
    return a.base == b.base && a.end == b.end && a.first_page == b.first_page;
  };
  if (built && std::equal(rebuilt.begin(), rebuilt.end(), t.views.begin(), t.views.end(), same))
    return;
  ++t.counters.remaps;
  // New views start writable, so protect everything again. Pages already in the newest log just
  // fault once more to reopen.
  t.views = std::move(rebuilt);
  if (!built || !ProtectAll(t))
  {
    // Untracked writes could follow, so no snapshot can be trusted.
    ERROR_LOG_FMT(CORE, "Rollback: guest RAM mappings changed beyond what copy-on-write "
                            "snapshots track; every snapshot is dropped");
    for (const View& view : views)
    {
      if (view.writeable && view.base)
        Protect(view.base, view.size, true);
    }
    ResetLocked(t);
  }
}

void StopTracking()
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  UnprotectAll(t);
  ResetLocked(t);
}

Counters GetCounters()
{
  Tracker& t = T();
  std::lock_guard lock(t.lock);
  return t.counters;
}
}  // namespace Rollback::Cow
