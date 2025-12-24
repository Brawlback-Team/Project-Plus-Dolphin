#include "mem.h"
//#include "profiler.h"
#include <vector>
#include <cassert>
#include <set>
#include <cmath>

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
#endif

// the [i].AddressArray.Count here represents the total number of pages in this allocation
std::vector<TrackedBuffer> TrackedMemList = {};
std::vector<ExcludeBuffer> ExcludeMemList = {};
// called on a buffer allocated with VirtualAlloc and MEM_WRITE_WATCH flag
void TrackAlloc(void* ptr, size_t size)
{
    if (!ptr || !size)
    {
      return;
    }
    TrackedBuffer tracked_buf = {};
    u64 pageSize = Common::PageSize();
    // make sure we have space for the maximum number of changed pages - that being total pages in the
    // allocated block
    u64 PageCount = static_cast<u64>(std::floor((size + pageSize - 1) / pageSize));

    tracked_buf.buffer.size = size;
    tracked_buf.buffer.data = (char*)ptr;

    AddressArray res;
    // the Count here represents the total number of pages in this allocation
    res.Count = PageCount;
    res.Addresses = (void**)malloc(PageCount * sizeof(void**));
    memset(res.Addresses, 0, PageCount * sizeof(void**));
    tracked_buf.changedPages = res;

    TrackedMemList.push_back(tracked_buf);
}
void IncludeMem(void* ptr)
{
  ExcludeMemList.erase(std::remove_if(ExcludeMemList.begin(), ExcludeMemList.end(),
              [ptr](const ExcludeBuffer& eb) { return eb.buffer.data == ptr; }), ExcludeMemList.end());
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
  exl.excludeGap = boost::icl::construct<boost::icl::discrete_interval<uintptr_t>>(ptr_addr, ptr_addr + size, boost::icl::interval_bounds::closed());
  ExcludeMemList.push_back(exl);
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
        // offset from base buffer pointer that got changed
        u64 changedOffset = ((u8*)ChangedPages.Addresses[PageIndex] - BaseAddress) / pageSize;
        printf("%llu : %llu\n", PageIndex, changedOffset);
    }
}

void PrintTrackedBuf(const TrackedBuffer& buf)
{
    printf("Tracked buffer [%p, %p]\n", 
        buf.buffer.data, buf.buffer.data + buf.buffer.size);
    PrintAddressArray(buf);
}

int GetWrittenPages(char* base, u64 baseSize, std::vector<uintptr_t>& changedPageAddresses,
                    u64& pageCount)
{
  size_t writtenToPagesIndex = 0;
  size_t pageSize = Common::PageSize();
  auto base_pte = reinterpret_cast<uintptr_t>(Common::GetPageAddress(base, Common::PageSize()));
  auto end_pte = reinterpret_cast<uintptr_t>(Common::GetPageAddress(base + baseSize, Common::PageSize()));

  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();

  while (base_pte <= end_pte)
  {
    if (memory.IsPageDirty(base_pte))
    {
      if (writtenToPagesIndex > pageCount)
      {
        return 1;
      }
      u8* base_pte_bytes = reinterpret_cast<u8*>(base_pte);
      if(!memory.HandleChangeProtection(base_pte_bytes, 0x1, PAGE_READONLY))
      {
        return 2;
      }
      auto addr = memory.GetDirtyPages()[base_pte].address;
      if (memory.IsAddressInLogicalMemory(reinterpret_cast<u8*>(addr)) != std::nullopt &&
          !memory.HandleChangeProtection(reinterpret_cast<void*>(addr), 0x1, PAGE_READONLY))
      {
        return 3;
      }
      if (std::find(changedPageAddresses.begin(), changedPageAddresses.end(), base_pte) ==
          changedPageAddresses.end())
      {
        changedPageAddresses.push_back(base_pte);
        writtenToPagesIndex++;
      }

      memory.SetPageDirtyBit(base_pte, false, addr);
    }
    base_pte += pageSize;
  }
  std::sort(changedPageAddresses.begin(), changedPageAddresses.end());
  pageCount = writtenToPagesIndex;
  return 0;
}

bool GetAndResetWrittenPages(std::vector<uintptr_t>& changedPageAddresses, u64 maxEntries)
{
    //PROFILE_FUNCTION();
    for (TrackedBuffer& buf : TrackedMemList)
    {
        // on input to GetWriteWatch this is the maximum number of possible changed pages in this allocation
        // on output, this is the number of page addresses that have been changed
        u64 pageCount = buf.changedPages.Count;
        int result;
        // move forward by the number of changed pages.
        {
          //PROFILE_SCOPE("GetWriteWatch");
            // get changed pages for specified buffer (buffer.data, buffer.size)
            // NOTE: addresses returned here are sorted (ascending)
            result = GetWrittenPages(buf.buffer.data, buf.buffer.size, changedPageAddresses, pageCount);
        }
        if (result != 0 || pageCount > maxEntries || changedPageAddresses.size() > maxEntries)
        {
            ERROR_LOG_FMT(BRAWLBACK, "WRITTEN PAGE WRITE FAILED! RESULT CODE: {}\n", result);
            if (result == 2 || result == 3)
            {
              DWORD dw = GetLastError();
              ERROR_LOG_FMT(BRAWLBACK, "WRITTEN PAGE WRITE FAILED DUE TO A FAILURE ({}) TO WRITE PROTECT. THE REASON IS: {}\n", result, dw);
            }
            return false;
        }
    }
    return true;
}



#include <immintrin.h>
#include <cstdint>
// num bytes copied must be multiple of 32
// dest and src buffers must be 32 byte aligned
// since our code generally always works with actual system pages of memory,
// nearly (if not all) of our memcpys are on power-of-two aligned blocks of 4096kb
void fastMemcpy(void *pvDest, void *pvSrc, size_t nBytes) 
{
    assert(IS_ALIGNED(pvDest, 32));
    assert(IS_ALIGNED(pvSrc, 32));
    assert(nBytes % 32 == 0);
    const __m256i *pSrc = reinterpret_cast<const __m256i*>(pvSrc);
    __m256i *pDest = reinterpret_cast<__m256i*>(pvDest);
    int64_t nVects = nBytes / sizeof(*pSrc);
    for (; nVects > 0; nVects--, pSrc++, pDest++) 
    {
        const __m256i loaded = _mm256_stream_load_si256(pSrc);
        _mm256_stream_si256(pDest, loaded);
    }
    _mm_sfence();
}
