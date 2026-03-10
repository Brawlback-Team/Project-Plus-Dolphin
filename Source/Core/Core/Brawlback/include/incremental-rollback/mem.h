#pragma once
#include "util.h"
#include <utility>
#include <boost/icl/interval_set.hpp>
#include <brawlback-common/SavestateMemRegion.h>

#define rbMemcpy(dst, src, size) fastMemcpy(dst, src, size)

struct Buffer
{
    char* data = nullptr;
    u64 size = 0;
    bool operator==(const Buffer& buf) const { return data == buf.data && size == buf.size; }
};

struct ExcludeBuffer
{
  Buffer buffer;
  boost::icl::discrete_interval<uintptr_t> excludeGap;
};

struct AddressArray
{
    void** Addresses;
    u64 Count;
};

struct TrackedBuffer
{
    Buffer buffer; // the actual buffer this is tracking
    AddressArray changedPages; // the pages that have been written to
};

extern std::vector<TrackedBuffer> TrackedMemList;
extern std::vector<ExcludeBuffer> ExcludeMemList;
extern std::vector<SavestateMemRegionInfo> LastFrameRegions;
    // passed in memory block MUST be allocated (at least on windows...) with VirtualAlloc and the MEM_WRITE_WATCH flag
void TrackAlloc(void* ptr, size_t size);
void ExcludeMem(void* ptr, size_t size);
void IncludeMem(void* ptr);
void UntrackAlloc(void* ptr);
void ResetAllocs(IncrementalRB::SavestateInfo& savestateInfo);
void PrintTrackedBuf(const TrackedBuffer& buf);
int GetWrittenPages(char* base, u64 baseSize, std::vector<uintptr_t>& changedPageAddresses,
                    u64& pageCount);
std::vector<SavestateMemRegionInfo> GetWrittenAddresses(std::vector<SavestateMemRegionInfo> thisFrameRegions, std::vector<SavestateMemRegionInfo> previousFrameRegions);
bool GetAndResetWrittenPages(std::vector<uintptr_t>& changedPageAddresses, u64 maxEntries,
                             std::vector<SavestateMemRegionInfo> region = {});
void fastMemcpy(void* pvDest, void* pvSrc, size_t nBytes);
