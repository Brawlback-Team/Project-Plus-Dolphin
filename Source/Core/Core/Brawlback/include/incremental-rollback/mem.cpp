#include "mem.h"
// #include "profiler.h"
#include <cassert>
#include <cmath>
#include <set>
#include <vector>

#include <Common/Logging/Log.h>
#include <Common/MemoryUtil.h>
#include <Core/HW/Memmap.h>
#include <Core/System.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/types.h>
#if defined __APPLE__ || defined __FreeBSD__ || defined __OpenBSD__ || defined __NetBSD__
#include <sys/sysctl.h>
#elif defined __HAIKU__
#include <OS.h>
#else
#include <sys/sysinfo.h>
#endif
#include <brawlback-common/SavestateMemRegion.h>
#endif

std::vector<TrackedBuffer> TrackedMemList = {};
std::vector<ExcludeBuffer> ExcludeMemList = {};
std::vector<SavestateMemRegionInfo> LastFrameRegions = {};

static u8* GetFastmemAddressFromEmulatedAddress(Memory::MemoryManager& memory, u32 emu_addr)
{
  if (!memory.IsFastmemArenaInitialized())
    return nullptr;

  if (emu_addr == 0)
    return nullptr;

  u8* logical_base = memory.GetLogicalBase();
  if (logical_base)
  {
    u8* logical_ptr = logical_base + emu_addr;
    if (memory.IsAddressInFastmemArea(logical_ptr))
      return logical_ptr;
  }

  u8* physical_base = memory.GetPhysicalBase();
  if (physical_base)
  {
    const u32 physical_addr = emu_addr & 0x3FFFFFFF;
    u8* physical_ptr = physical_base + physical_addr;
    if (memory.IsAddressInFastmemArea(physical_ptr))
      return physical_ptr;
  }

  return nullptr;
}

void TrackAlloc(void* ptr, size_t size)
{
  if (!ptr || !size)
  {
    return;
  }

  TrackedBuffer tracked_buf = {};
  const u64 pageSize = Common::PageSize();
  const uintptr_t alloc_start = reinterpret_cast<uintptr_t>(ptr);
  const uintptr_t alloc_end_inclusive = alloc_start + size - 1;
  const uintptr_t alloc_start_page = reinterpret_cast<uintptr_t>(
      Common::GetPageAddress(reinterpret_cast<void*>(alloc_start), pageSize));
  const uintptr_t alloc_end_page = reinterpret_cast<uintptr_t>(
      Common::GetPageAddress(reinterpret_cast<void*>(alloc_end_inclusive), pageSize));
  const u64 PageCount = ((alloc_end_page - alloc_start_page) / pageSize) + 1;

  tracked_buf.buffer.size = size;
  tracked_buf.buffer.data = (char*)ptr;

  AddressArray res;
  res.Count = PageCount;
  res.Addresses = static_cast<void**>(malloc(PageCount * sizeof(void*)));
  if (!res.Addresses)
  {
    ERROR_LOG_FMT(BRAWLBACK, "TrackAlloc: Failed to allocate address array");
    return;
  }
  memset(res.Addresses, 0, PageCount * sizeof(void*));
  tracked_buf.changedPages = res;

  TrackedMemList.push_back(tracked_buf);

  const uintptr_t alloc_end = alloc_start + size;
  INFO_LOG_FMT(BRAWLBACK, "TrackAlloc: [{:016x}, {:016x}] ({} pages)", alloc_start, alloc_end,
               PageCount);
}

void IncludeMem(void* ptr)
{
  if (!ptr)
    return;

  ExcludeMemList.erase(
      std::remove_if(ExcludeMemList.begin(), ExcludeMemList.end(),
                     [ptr](const ExcludeBuffer& eb) { return eb.buffer.data == ptr; }),
      ExcludeMemList.end());
}

void ExcludeMem(void* ptr, size_t size)
{
  if (!ptr || !size)
  {
    return;
  }
  ExcludeBuffer exl = {};
  Buffer buf = {};
  buf.data = (char*)ptr;
  buf.size = size;
  exl.buffer = buf;
  auto ptr_addr = reinterpret_cast<uintptr_t>(ptr);
  exl.excludeGap = boost::icl::construct<boost::icl::discrete_interval<uintptr_t>>(
      ptr_addr, ptr_addr + size - 1, boost::icl::interval_bounds::closed());
  ExcludeMemList.push_back(exl);

  INFO_LOG_FMT(BRAWLBACK, "ExcludeMem: [{:016x}, {:016x}]", ptr_addr, ptr_addr + size);
}

void UntrackAlloc(void* ptr)
{
  if (!ptr)
    return;
  for (int i = 0; i < TrackedMemList.size(); i++)
  {
    if (TrackedMemList[i].buffer.data == ptr)
    {
      free(TrackedMemList[i].changedPages.Addresses);
      TrackedMemList.erase(TrackedMemList.begin() + i);
      break;
    }
  }
}

void ResetAllocs(IncrementalRB::SavestateInfo& savestateInfo)
{
  for (int i = 0; i < TrackedMemList.size(); i++)
  {
    free(TrackedMemList[i].changedPages.Addresses);
  }
  TrackedMemList.clear();
  ExcludeMemList.clear();

  for (auto& savestate : savestateInfo.savestates)
  {
    savestate.changedPages.clear();
    savestate.afterCopies.clear();
    if (savestate.arena.backing_mem)
    {
      _mm_free(savestate.arena.backing_mem);
    }
  }
}

void PrintAddressArray(const TrackedBuffer& buf)
{
  const AddressArray& ChangedPages = buf.changedPages;
  u8* BaseAddress = (u8*)buf.buffer.data;
  u64 pageSize = Common::PageSize();
  for (u64 PageIndex = 0; PageIndex < ChangedPages.Count; ++PageIndex)
  {
    u64 changedOffset = ((u8*)ChangedPages.Addresses[PageIndex] - BaseAddress) / pageSize;
    printf("%llu : %llu\n", PageIndex, changedOffset);
  }
}

void PrintTrackedBuf(const TrackedBuffer& buf)
{
  printf("Tracked buffer [%p, %p]\n", buf.buffer.data, buf.buffer.data + buf.buffer.size);
  PrintAddressArray(buf);
}

int GetWrittenPages(char* base, u64 baseSize, std::vector<uintptr_t>& changedPageAddresses,
                    u64& pageCount)
{
  struct DirtyPageRange
  {
    uintptr_t start;
    uintptr_t end;
  };

  size_t writtenToPagesIndex = 0;
  const size_t pageSize = Common::PageSize();
  const auto base_pte = reinterpret_cast<uintptr_t>(Common::GetPageAddress(base, pageSize));
  const auto end_pte =
      reinterpret_cast<uintptr_t>(Common::GetPageAddress(base + baseSize - 1, pageSize));
  const u64 max_page_count = static_cast<u64>(((end_pte - base_pte) / pageSize) + 1);

  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();

  std::vector<DirtyPageRange> dirty_ranges;
  size_t dirty_page_count = 0;

  for (uintptr_t collect_pte = base_pte; collect_pte <= end_pte; collect_pte += pageSize)
  {
    if (!memory.IsPageDirty(collect_pte))
      continue;

    dirty_page_count++;
    if (dirty_ranges.empty() || collect_pte != dirty_ranges.back().end + pageSize)
    {
      dirty_ranges.push_back({collect_pte, collect_pte});
    }
    else
    {
      dirty_ranges.back().end = collect_pte;
    }
  }

  if (dirty_page_count == 0)
  {
    pageCount = 0;
    return 0;
  }

  INFO_LOG_FMT(BRAWLBACK, "GetWrittenPages: Found {} dirty pages", dirty_page_count);

  for (const DirtyPageRange& range : dirty_ranges)
  {
    const size_t protect_size = (range.end - range.start) + pageSize;
    u8* protect_ptr = reinterpret_cast<u8*>(range.start);

    if (!memory.HandleChangeProtection(protect_ptr, protect_size, PAGE_READONLY))
    {
      ERROR_LOG_FMT(BRAWLBACK, "Failed to protect memory range [{:016x}, {:016x}] size={}",
                    range.start, range.end, protect_size);
      return 2;
    }

    u32 emu_addr_start = memory.GetEmulatedAddress(reinterpret_cast<u8*>(range.start));
    u8* check_pte_bytes = GetFastmemAddressFromEmulatedAddress(memory, emu_addr_start);
    if (!check_pte_bytes ||
        !memory.HandleChangeProtection(check_pte_bytes, protect_size, PAGE_READONLY))
    {
      ERROR_LOG_FMT(BRAWLBACK, "Failed to protect fastmem range [{:016x}, {:016x}] size={}",
                    reinterpret_cast<uintptr_t>(check_pte_bytes),
                    reinterpret_cast<uintptr_t>(check_pte_bytes) + protect_size, protect_size);
      return 2;
    }

    for (uintptr_t check_pte = range.start; check_pte <= range.end; check_pte += pageSize)
    {
      if (writtenToPagesIndex >= max_page_count)
      {
        return 1;
      }

      auto& dirty_pages_map = memory.GetDirtyPages();
      auto it = dirty_pages_map.find(check_pte);
      if (it == dirty_pages_map.end())
      {
        WARN_LOG_FMT(BRAWLBACK, "Page {:016x} marked dirty but not in map", check_pte);
        continue;
      }

      uintptr_t cur_addr = it->second.fastmem_address;
      uintptr_t actual_page_addr = check_pte;

      check_pte_bytes = reinterpret_cast<u8*>(check_pte);
      if (memory.IsAddressInFastmemArea(check_pte_bytes))
      {
        u32 emu_addr = memory.GetEmulatedAddress(check_pte_bytes);
        auto span = memory.GetSpanForAddress(emu_addr);
        if (!span.empty() && span.data())
        {
          actual_page_addr = reinterpret_cast<uintptr_t>(span.data());
        }
        else
        {
          WARN_LOG_FMT(BRAWLBACK, "Failed to get span for emulated address {:08x}", emu_addr);
          continue;
        }
      }

      uintptr_t actual_page_start = reinterpret_cast<uintptr_t>(
          Common::GetPageAddress(reinterpret_cast<void*>(actual_page_addr), pageSize));

      if (std::find(changedPageAddresses.begin(), changedPageAddresses.end(), actual_page_start) ==
          changedPageAddresses.end() && memory.IsAddressInEmulatedMemory(actual_page_start))
      {
        changedPageAddresses.push_back(actual_page_start);
        writtenToPagesIndex++;
      }

      memory.SetPageDirtyBit(check_pte, false, cur_addr, false);
    }
  }

  pageCount = writtenToPagesIndex;
  return 0;
}

std::vector<SavestateMemRegionInfo> GetWrittenAddresses(std::vector<SavestateMemRegionInfo> thisFrameRegions,
                        std::vector<SavestateMemRegionInfo> previousFrameRegions)
{
  std::vector<SavestateMemRegionInfo> diff;

  std::sort(thisFrameRegions.begin(), thisFrameRegions.end(), [](const SavestateMemRegionInfo& a, const SavestateMemRegionInfo& b) {
    return a.address < b.address;
  });
  std::sort(previousFrameRegions.begin(), previousFrameRegions.end(), [](const SavestateMemRegionInfo& a, const SavestateMemRegionInfo& b) {
    return a.address < b.address;
  });

  std::set_difference(thisFrameRegions.begin(), thisFrameRegions.end(),
                      previousFrameRegions.begin(), previousFrameRegions.end(),
                      std::inserter(diff, diff.begin()), [](const SavestateMemRegionInfo& a, const SavestateMemRegionInfo& b) {
                        return a.address < b.address;
                      }
  );

  return diff;
}

bool GetAndResetWrittenPages(std::vector<uintptr_t>& changedPageAddresses, u64 maxEntries, std::vector<SavestateMemRegionInfo> region)
{
  if (region.size() > 0)
  {
    auto changedAddresses = GetWrittenAddresses(region, LastFrameRegions);
    LastFrameRegions = region;
    auto& system = Core::System::GetInstance();
    auto& memory = system.GetMemory();

    for (const auto& addr : changedAddresses)
    {
      changedPageAddresses.push_back(reinterpret_cast<uintptr_t>(Common::GetPageAddress(reinterpret_cast<u8*>(memory.GetSpanForAddress(addr.address).data()), Common::PageSize())));
    }
    std::sort(changedPageAddresses.begin(), changedPageAddresses.end());
    auto last = std::unique(changedPageAddresses.begin(), changedPageAddresses.end());
    changedPageAddresses.erase(last, changedPageAddresses.end());

    return true;
  }
  else
  {
    for (size_t buf_idx = 0; buf_idx < TrackedMemList.size(); buf_idx++)
    {
      TrackedBuffer& buf = TrackedMemList[buf_idx];
      u64 pageCount = buf.changedPages.Count;
      int result;

      result = GetWrittenPages(buf.buffer.data, buf.buffer.size, changedPageAddresses, pageCount);

      if (result != 0)
      {
        ERROR_LOG_FMT(BRAWLBACK, "GetWrittenPages failed with result code: {}", result);
        if (result == 2 || result == 3)
        {
          DWORD dw = GetLastError();
          ERROR_LOG_FMT(BRAWLBACK, "Failed to write protect memory. Windows error: {} (0x{:08x})",
                        dw, dw);
        }
        return false;
      }

      if (pageCount > maxEntries || changedPageAddresses.size() > maxEntries)
      {
        ERROR_LOG_FMT(BRAWLBACK, "Too many changed pages: {} pages > {} max entries",
                      changedPageAddresses.size(), maxEntries);
        return false;
      }

      INFO_LOG_FMT(BRAWLBACK, "Buffer {}: {} dirty pages tracked", buf_idx, pageCount);
    }

    std::sort(changedPageAddresses.begin(), changedPageAddresses.end());
    changedPageAddresses.erase(
        std::unique(changedPageAddresses.begin(), changedPageAddresses.end()),
        changedPageAddresses.end());

    if (changedPageAddresses.size() > maxEntries)
    {
      ERROR_LOG_FMT(BRAWLBACK, "Too many changed pages: {} pages > {} max entries",
                    changedPageAddresses.size(), maxEntries);
      return false;
    }

    INFO_LOG_FMT(BRAWLBACK, "=== GetAndResetWrittenPages END: {} total changed pages ===",
                 changedPageAddresses.size());
    return true;
  }
}

#include <cstdint>
#include <immintrin.h>

void fastMemcpy(void* pvDest, void* pvSrc, size_t nBytes)
{
  assert(IS_ALIGNED(pvDest, 32));
  assert(IS_ALIGNED(pvSrc, 32));
  assert(nBytes % 32 == 0);
  const __m256i* pSrc = reinterpret_cast<const __m256i*>(pvSrc);
  __m256i* pDest = reinterpret_cast<__m256i*>(pvDest);
  int64_t nVects = nBytes / sizeof(*pSrc);
  for (; nVects > 0; nVects--, pSrc++, pDest++)
  {
    const __m256i loaded = _mm256_stream_load_si256(pSrc);
    _mm256_stream_si256(pDest, loaded);
  }
  _mm_sfence();
}
