// Credit to FaultyPine for this logic: https://github.com/FaultyPine/incremental-rollback

#include "incremental_rb.h"
#include "util.h"
#include "mem.h"
#include "tiny_arena.h"
#include "job_system.h"
#include "brawlback-common/BrawlbackConstants.h"
#include <set>
#include <cassert>
#include <Common/Logging/Log.h>
#include <Core/HW/MemoryInterface.h>
#include <Core/HW/VideoInterface.h>
#include <Core/HW/SI/SI.h>
#include <Core/HW/DSP.h>
#include <Core/HW/DVD/DVDInterface.h>
#include <Core/HW/EXI/EXI.h>
#include <Core/HW/AudioInterface.h>
#include <Core/HW/HSP/HSP_Device.h>
#include <Core/HW/ProcessorInterface.h>
#include <Core/HW/GPFifo.h>
#include <Core/HW/HSP/HSP.h>
#include <Core/HW/WII_IPC.h>
#include <Core/IOS/IOS.h>
#include <Core/Movie.h>
#include <VideoCommon/VideoBackendBase.h>
#include <Core/GeckoCode.h>
#include <Core/PowerPC/PowerPC.h>
#include <Core/System.h>
#include <Core/Core.h>
#include <Common/MemoryUtil.h>
#include <fstream>
#include <Core/NetPlayClient.h>

#define ENABLE_LOGGING
//#define SPECIFIC_TRACKING
//#define HEAPS
constexpr u32 numWorkerThreads = 4;

// FUTURE: go faster than memcpy - https://squadrick.dev/journal/going-faster-than-memcpy.html
// we have very specific restrictions on the blocks of mem we move around
// always page-sized, and pages are always aligned... surely there's some wins there. Also very easy to parallelize


constexpr u64 MAX_NUM_CHANGED_PAGES = 60000;

namespace IncrementalRB
{
  std::vector<SavestateVerification> g_verification_history;
  std::vector<CriticalMemoryRegion> g_critical_regions;
  typedef boost::icl::interval_set<uintptr_t> TIntervalSet;

  struct Region
  {
    u32 startAddress;
    u32 endAddress;
  };

  SavestateInfo savestateInfo = {};
  static jobsystem::context jobctx;

  static IncrementalRBCallbacks cbs = {};

  static TIntervalSet excludeSet;
 
  inline u8* GetRAM()
  {
    if (cbs.getRAM)
    {
      return cbs.getRAM();
    }
    return nullptr;
  }
  inline u8* GetEXRAM()
  {
    if (cbs.getEXRAM)
    {
      return cbs.getEXRAM();
    }
    return nullptr;
  }
  inline u32 GetRAMSize()
  {
    if (cbs.getRAMSize)
    {
      return cbs.getRAMSize();
    }
    return 0;
  }
  inline u32 GetEXRAMSize()
  {
    if (cbs.getEXRAMSize)
    {
      return cbs.getEXRAMSize();
    }
    return 0;
  }
  inline u32 GetEXRAMMask()
  {
    if (cbs.getEXRAMMask)
    {
      return cbs.getEXRAMMask();
    }
    return 0;
  }
  inline u32 GetGamestateSize()
  {
    if (cbs.getEXRAMSize && cbs.getRAMSize)
    {
      return cbs.getEXRAMSize() + cbs.getRAMSize();
    }
    return 0;
  }
  inline u32* GetGameMemFrame()
  {
    if (cbs.getGameMemFrame)
    {
      return cbs.getGameMemFrame();
    }
    return nullptr;
  }
  inline u8* GetPointer(u32 addr)
  {
    if (cbs.getPointer)
    {
      return cbs.getPointer(addr);
    }
    return nullptr;
  }

  inline std::array<Memory::PhysicalMemoryRegion, 4> GetPhysicalRegions()
  {
    if (cbs.getPhysicalRegions)
    {
      return cbs.getPhysicalRegions();
    }
    return {};
  }

  static void ClearTrackingState()
  {
    for (auto& tracked : TrackedMemList)
    {
      free(tracked.changedPages.Addresses);
    }
    TrackedMemList.clear();
    ExcludeMemList.clear();
    excludeSet.clear();
  }

  static void BuildExcludeSet()
  {
    excludeSet.clear();

    for (u32 i = 0; i < ExcludeMemList.size(); i++)
    {
      excludeSet.insert(ExcludeMemList[i].excludeGap);
    }
    
    std::sort(ExcludeMemList.begin(), ExcludeMemList.end(),
              [](const ExcludeBuffer& a, const ExcludeBuffer& b) {
                return a.buffer.data < b.buffer.data;
              });
  }

  static void SetupTrackedRegions()
  {
#ifdef SPECIFIC_TRACKING
    std::vector<Region> staticRegions = {
        {0x806414a0, 0x806414a0 + 0x60},   {0x8062f440, 0x8062f440 + 0x500},
        {0x8063ff60, 0x8063ff60 + 0x1520}, {0x8063dcc0, 0x8063dcc0 + 0x2280},
        {0x8063da80, 0x8063da80 + 0x220},  {0x80624780, 0x80624780 + 0x50},
        {0x806232e0, 0x806232e0 + 0x1480}, {0x806273c0, 0x806273c0 + 0x540},
        {0x80624800, 0x80624800 + 0x2ba0}, {0x80622d20, 0x80622d20 + 0x388},
        {0x80621260, 0x80621260 + 0x1aa0}, {0x8061f920, 0x8061f920 + 0x1920},
        {0x8061f460, 0x8061f460 + 0x4a0},  {0x80615620, 0x80615620 + 0x6f20},
        {0x8061c560, 0x8061c560 + 0x2EE0}, {0x8063d9a0, 0x8063d9a0 + 0xc0},
        {0x8063d8e0, 0x8063d8e0 + 0xa0},   {0x806312e0, 0x806312e0 + 0xbba0},
        {0x8062fb40, 0x8062fb40 + 0x1780}, {0x8062f9e0, 0x8062f9e0 + 0x140},
        {0x8062f960, 0x8062f960 + 0x60},   {0x80663300, 0x80663300 + 0x140},
        {0x80663280, 0x80663280 + 0x60},   {0x80663060, 0x80663060 + 0x200},
        {0x8062b360, 0x8062b360 + 0x14c0}, {0x80613e00, 0x80613e00 + 0x28},
        {0x80641520, 0x80641520 + 0x20},   {0x80629a00, 0x80629a00 + 0x160},
        {0x80623180, 0x80623180 + 0x20},   {0x806230e0, 0x806230e0 + 0x80},
        {0x80627920, 0x80627920 + 0x10c0}, {0x8062f3e0, 0x8062f3e0 + 0x40},
        {0x8062f360, 0x8062f360 + 0x60},   {0x8062c840, 0x8062c840 + 0x1e00},
        {0x80629980, 0x80629980 + 0x60},   {0x80628a00, 0x80628a00 + 0xAA0},
        {0x8049edd8, 0x8049edd8 + 0x5064}, {0x805b62a0, 0x805b62a0 + 0x100},
        {0x80662b40, 0x80662b40 + 0x500},  {0x80662620, 0x80662620 + 0x500},
        {0x80672f40, 0x80672f40 + 0x520},  {0x8066b5e0, 0x8066b5e0 + 0x4220},
        {0x806673a0, 0x806673a0 + 0x4220}, {0x80663fe0, 0x80663fe0 + 0x33a0},
        {0x80672920, 0x80672920 + 0x600},  {0x80672300, 0x80672300 + 0x600},
        {0x80671ce0, 0x80671ce0 + 0x600},  {0x806716c0, 0x806716c0 + 0x600},
        {0x806710a0, 0x806710a0 + 0x600},  {0x80670a80, 0x80670a80 + 0x600},
        {0x80670460, 0x80670460 + 0x600},  {0x8066fe40, 0x8066fe40 + 0x600},
        {0x8066f820, 0x8066f820 + 0x600},  {0x8049e57c, 0x8049e57c + 0xC},
        {0x805ba480, 0x805baca0}};
    if (GetRAM())
    {
      auto system = GetPointer(0x80611f60);
      auto systemFW = GetPointer(0x805b5160);
      auto effect = GetPointer(0x80b8db60);
      auto infoResource = GetPointer(0x80c23a60);
      auto commonResource = GetPointer(0x80da3a60);
      auto tmp = GetPointer(0x81049e60);
      auto infoExtraResource = GetPointer(0x815cdf60);
      auto infoInstance = GetPointer(0x81601960);
      auto stageInstance = GetPointer(0x814ce460);
      auto menuInstance = GetPointer(0x81734d60);
      auto itemInstance = GetPointer(0x81382b60);
      auto overlayFighter1 = GetPointer(0x81061060);
      auto overlayFighter2 = GetPointer(0x810a9560);
      auto overlayStage = GetPointer(0x810f1a60);
      auto fighter1Instance = GetPointer(0x8123ab60);
      auto fighter2Instance = GetPointer(0x8128cb60);
      auto physics = GetPointer(0x8154e560);
      auto overlayCommon = GetPointer(0x80673460);

      TrackAlloc(system, 0x61500);
      TrackAlloc(systemFW, 0x15100);
      TrackAlloc(effect, 0x95f00);
      TrackAlloc(infoResource, 0x180000);
      TrackAlloc(commonResource, 0x232800);
      TrackAlloc(tmp, 0x00017200);
      TrackAlloc(infoExtraResource, 0x1cce00);
      TrackAlloc(infoInstance, 1258496);
      TrackAlloc(stageInstance, 524544);
      TrackAlloc(menuInstance, 629504);
      TrackAlloc(itemInstance, 1358080);
      TrackAlloc(overlayFighter1, 296192);
      TrackAlloc(overlayFighter2, 296192);
      TrackAlloc(fighter1Instance, 335872);
      TrackAlloc(fighter2Instance, 335872);
      TrackAlloc(physics, 734208);
      TrackAlloc(overlayCommon, 0x51a700);
      TrackAlloc(overlayStage, 0x70b00);
    }
    if (GetEXRAM())
    {
      auto wiiPad = GetPointer(0x90e61400);
      auto iteamResource = GetPointer(0x91018b00);
      auto fighter1Resoruce = GetPointer(0x9151fa00);
      auto fighter2Resoruce = GetPointer(0x91b04c80);
      auto fighter1Resoruce2 = GetPointer(0x91a72e00);
      auto fighter2Resoruce2 = GetPointer(0x92058080);
      auto fighterTechqniq = GetPointer(0x92cb4400);
      auto gameGlobal = GetPointer(0x90167400);
      auto fighterKirbyResource1 = GetPointer(0x914d2900);
      auto globalMode = GetPointer(0x90fddc00);
      auto itemExtraResource = GetPointer(0x9359ae00);
      auto fighterKirbyResource2 = GetPointer(0x914ec400);
      auto fighterKirbyResource3 = GetPointer(0x91505f00);
      auto fighterEffect = GetPointer(0x91478e00);

      TrackAlloc(wiiPad, 90368);
      TrackAlloc(iteamResource, 3051520);
      TrackAlloc(fighter1Resoruce, 5583872);
      TrackAlloc(fighter2Resoruce, 5583872);
      TrackAlloc(fighter1Resoruce2, 597632);
      TrackAlloc(fighter2Resoruce2, 597632);
      TrackAlloc(fighterTechqniq, 1153792);
      TrackAlloc(gameGlobal, 205824);
      TrackAlloc(fighterKirbyResource1, 105216);
      TrackAlloc(globalMode, 241408);
      TrackAlloc(itemExtraResource, 209920);
      TrackAlloc(fighterKirbyResource2, 105216);
      TrackAlloc(fighterKirbyResource3, 105216);
      TrackAlloc(fighterEffect, 0x59b00);
    }
#elif defined HEAPS
    TrackAlloc(GetPointer(0x8049edd8), 0x804a3e3c - 0x8049edd8);
    TrackAlloc(GetPointer(0x804c1d08), 0x804c1d34 - 0x804c1d08);
    TrackAlloc(GetPointer(0x804f67e0), 0x804f680c - 0x804f67e0);
    TrackAlloc(GetPointer(0x805297a0), 0x805297cc - 0x805297a0);
    TrackAlloc(GetPointer(0x805a0128), 0x805a0134 - 0x805a0128);
    TrackAlloc(GetPointer(0x805a06c0), 0x805a06c6 - 0x805a06c0);
    TrackAlloc(GetPointer(0x805a06cc), 0x805a06d4 - 0x805a06cc);
    TrackAlloc(GetPointer(0x805a084c), 0x805a0850 - 0x805a084c);
    TrackAlloc(GetPointer(0x805a2d2c), 0x805a2d54 - 0x805a2d2c);
    TrackAlloc(GetPointer(0x805b5160), 0x805b5187 - 0x805b5160);
    TrackAlloc(GetPointer(0x805b51a1), 0x805b765f - 0x805b51a1);
    TrackAlloc(GetPointer(0x805b8661), 0x805b867f - 0x805b8661);
    TrackAlloc(GetPointer(0x805b89e1), 0x805b8edf - 0x805b89e1);
    TrackAlloc(GetPointer(0x805ba0e1), 0x805ba0ff - 0x805ba0e1);
    TrackAlloc(GetPointer(0x805ba461), 0x805bc4bf - 0x805ba461);
    TrackAlloc(GetPointer(0x805bf4c1), 0x805bf4e7 - 0x805bf4c1);
    TrackAlloc(GetPointer(0x805bf841), 0x805bfa9f - 0x805bf841);
    TrackAlloc(GetPointer(0x805bfab9), 0x805bfd1f - 0x805bfab9);
    TrackAlloc(GetPointer(0x805bfd39), 0x805c4fff - 0x805bfd39);
    TrackAlloc(GetPointer(0x805c5019), 0x805ca1bf - 0x805c5019);
    TrackAlloc(GetPointer(0x80611f60), 0x80611f87 - 0x80611f60);
    TrackAlloc(GetPointer(0x80611fa1), 0x80673487 - 0x80611fa1);
    TrackAlloc(GetPointer(0x806734a1), 0x8067406b - 0x806734a1);
    TrackAlloc(GetPointer(0x8067b76d), 0x80680ceb - 0x8067b76d);
    TrackAlloc(GetPointer(0x80680db1), 0x806828c3 - 0x80680db1);
    TrackAlloc(GetPointer(0x806a07e1), 0x806a3b23 - 0x806a07e1);
    TrackAlloc(GetPointer(0x806ad1e9), 0x806b0983 - 0x806ad1e9);
    TrackAlloc(GetPointer(0x806b8ff5), 0x806bb553 - 0x806b8ff5);
    TrackAlloc(GetPointer(0x806f8ad5), 0x8070aa13 - 0x806f8ad5);
    TrackAlloc(GetPointer(0x80ad67f9), 0x80b8db87 - 0x80ad67f9);
    TrackAlloc(GetPointer(0x80b8dba1), 0x80c23a87 - 0x80b8dba1);
    TrackAlloc(GetPointer(0x80c23aa1), 0x80fd6287 - 0x80c23aa1);
    TrackAlloc(GetPointer(0x80fd62a1), 0x81049e87 - 0x80fd62a1);
    TrackAlloc(GetPointer(0x81049ea1), 0x81061087 - 0x81049ea1);
    TrackAlloc(GetPointer(0x810610a1), 0x810a9587 - 0x810610a1);
    TrackAlloc(GetPointer(0x810a95a1), 0x810f1a87 - 0x810a95a1);
    TrackAlloc(GetPointer(0x810f1aa1), 0x81162587 - 0x810f1aa1);
    TrackAlloc(GetPointer(0x811625a1), 0x8116384b - 0x811625a1);
    TrackAlloc(GetPointer(0x811a86c1), 0x811aa187 - 0x811a86c1);
    TrackAlloc(GetPointer(0x811aa1a1), 0x811f2687 - 0x811aa1a1);
    TrackAlloc(GetPointer(0x811f26a1), 0x8123ab87 - 0x811f26a1);
    TrackAlloc(GetPointer(0x8123aba1), 0x815edf87 - 0x8123aba1);
    TrackAlloc(GetPointer(0x815edfa1), 0x8168599f - 0x815edfa1);
    TrackAlloc(GetPointer(0x81685ce1), 0x817ae860 - 0x81685ce1);
    TrackAlloc(GetPointer(0x90167400), 0x90167427 - 0x90167400);
    TrackAlloc(GetPointer(0x90167441), 0x90199800 - 0x90167441);
    TrackAlloc(GetPointer(0x90e61400), 0x90e61427 - 0x90e61400);
    TrackAlloc(GetPointer(0x90e61441), 0x90e77527 - 0x90e61441);
    TrackAlloc(GetPointer(0x90e77541), 0x90fd90ff - 0x90e77541);
    TrackAlloc(GetPointer(0x90fd9459), 0x90fd9897 - 0x90fd9459);
    TrackAlloc(GetPointer(0x90fdc899), 0x90fddc27 - 0x90fdc899);
    TrackAlloc(GetPointer(0x90fddc41), 0x91018b27 - 0x90fddc41);
    TrackAlloc(GetPointer(0x91018b41), 0x91301b27 - 0x91018b41);
    TrackAlloc(GetPointer(0x91301b41), 0x9134cc00 - 0x91301b41);
    TrackAlloc(GetPointer(0x91478e00), 0x91478e27 - 0x91478e00);
    TrackAlloc(GetPointer(0x91478e41), 0x914d2927 - 0x91478e41);
    TrackAlloc(GetPointer(0x914d2941), 0x914ec427 - 0x914d2941);
    TrackAlloc(GetPointer(0x914ec441), 0x91505f27 - 0x914ec441);
    TrackAlloc(GetPointer(0x91505f41), 0x9151fa27 - 0x91505f41);
    TrackAlloc(GetPointer(0x9151fa41), 0x919b6627 - 0x9151fa41);
    TrackAlloc(GetPointer(0x919b6641), 0x91a72e27 - 0x919b6641);
    TrackAlloc(GetPointer(0x91a72e41), 0x91b04ca7 - 0x91a72e41);
    TrackAlloc(GetPointer(0x91b04cc1), 0x920580a7 - 0x91b04cc1);
    TrackAlloc(GetPointer(0x920580c1), 0x920e9f27 - 0x920580c1);
    TrackAlloc(GetPointer(0x920e9f41), 0x92112927 - 0x920e9f41);
    TrackAlloc(GetPointer(0x92112941), 0x9263d327 - 0x92112941);
    TrackAlloc(GetPointer(0x9263d341), 0x92650127 - 0x9263d341);
    TrackAlloc(GetPointer(0x92650141), 0x926cf1a7 - 0x92650141);
    TrackAlloc(GetPointer(0x926cf1c1), 0x92c225a7 - 0x926cf1c1);
    TrackAlloc(GetPointer(0x92c225c1), 0x92cb4427 - 0x92c225c1);
    TrackAlloc(GetPointer(0x92cb4441), 0x92dcdf27 - 0x92cb4441);
    TrackAlloc(GetPointer(0x92dcdf41), 0x92e90127 - 0x92dcdf41);
    TrackAlloc(GetPointer(0x92e90141), 0x935ce200 - 0x92e90141);
#else
    std::array<Memory::PhysicalMemoryRegion, 4> physical_entries = GetPhysicalRegions();
    for (const auto& region : physical_entries)
    {
      if (!region.active || !region.out_pointer || !*region.out_pointer || region.size == 0)
      {
        continue;
      }

      TrackAlloc(*region.out_pointer, region.size);
      INFO_LOG_FMT(BRAWLBACK, "Tracking physical region: base={:016x} size={}",
                   reinterpret_cast<uintptr_t>(*region.out_pointer), region.size);
    }

    // Threading Stuff
    // ExcludeMem(GetPointer(0x805a5154), 0x805b5158 - 0x805a5154);  // Main Stack

    ExcludeMem(GetPointer(0x80009760), 0x805b5158 - 0x80009760);  // Data Sections, BSS, Main Stack

    // ExcludeMem(GetPointer(0x805bf420), 0x28);                    // ??? OSAlarm
    // ExcludeMem(GetPointer(0x805bacc0), 0x28);                    // PAD OSAlarm
    // ExcludeMem(GetPointer(0x805b85e0), 0x28);                    // OSALarmSleep OSAlarm
    //  Heaps
    ExcludeMem(GetPointer(0x817ba5a0), 0x817ca5a0 - 0x817ba5a0);  // Syringe
    ExcludeMem(GetPointer(0x93604000), 0x4000);                   // EXI Transfer
    // ExcludeMem(GetPointer(0x92163a00), 0x00de6700);              //  InfoExtraResource
    // ExcludeMem(GetPointer(0x914c9f00), 0x00c99b00);               //  MenuResource
    ExcludeMem(GetPointer(0x805d1e60), 0x00040100);  // RenderFifo
    ExcludeMem(GetPointer(0x9134cc00), 0x0012c200);  // CopyFB
    ExcludeMem(GetPointer(0x805ca260), 0x00007c00);  // Thread
    ExcludeMem(GetPointer(0x90199800), 0x00cc7c00);  // Sound
    // ExcludeMem(GetPointer(0x80b8db60), 0x80c23a60 - 0x80b8db60); // Effect*/

    BuildExcludeSet();
#endif
  }

  void InitState(IncrementalRBCallbacks cb)
  {
    // PROFILE_FUNCTION();

    cbs = cb;

    ResetAllocs(savestateInfo);
    excludeSet.clear();
    SetupTrackedRegions();
    jobsystem::Initialize(
        numWorkerThreads -
        1);  // -1 because when we do our async and join stuff, main thread also becomes a worker
    assert(IS_ALIGNED(GetRAM(), 32));    // for simd memcpy, need to be 32 byte aligned
    assert(IS_ALIGNED(GetEXRAM(), 32));  // for simd memcpy, need to be 32 byte aligned

    // allocate mem for savestates
    u64 savestateMemSize = MAX_NUM_CHANGED_PAGES * Common::PageSize();
    for (Savestate& savestate : savestateInfo.savestates)
    {
      void* backingMem = _mm_malloc(savestateMemSize, 32);
      assert(IS_ALIGNED(backingMem, 32));
      savestate.arena = arena_init(backingMem, savestateMemSize);
    }

    InitializeCriticalRegions();

    g_verification_history.clear();
  }

  void RefreshTrackedRegions()
  {
    INFO_LOG_FMT(BRAWLBACK, "Refreshing tracked regions after remap");
    ClearTrackingState();
    SetupTrackedRegions();
    InitializeCriticalRegions();
  }

  s32 Wrap(s32 x, s32 wrap)
  {
    if (x < 0)
      x = (wrap + x);
    return abs(x) % wrap;
  }

  static void RollbackSavestate(const Savestate& savestate)
  {
    u64 pageSize = Common::PageSize();

#ifndef MULTITHREAD
    if (excludeSet.size() > 0)
    {
      std::unordered_map<uintptr_t, size_t> pageToIndexMap;
      for (size_t i = 0; i < savestate.changedPages.size(); i++)
      {
        pageToIndexMap[savestate.changedPages[i]] = i;
      }

      TIntervalSet changedSet;
      for (u32 i = 0; i < savestate.changedPages.size(); i++)
      {
        changedSet += boost::icl::discrete_interval<uintptr_t>::closed(
            savestate.changedPages[i], savestate.changedPages[i] + pageSize - 1);
      }

      auto difference = changedSet - excludeSet;

#ifdef ENABLE_LOGGING
      INFO_LOG_FMT(BRAWLBACK, "Restoring {} intervals after exclude filtering (from {} pages)",
                   boost::icl::iterative_size(difference), savestate.changedPages.size());
#endif

      for (auto it = difference.begin(); it != difference.end(); ++it)
      {
        uintptr_t interval_start = it->lower();
        uintptr_t interval_end = it->upper();
        size_t interval_size = (interval_end - interval_start) + 1;

        uintptr_t current_addr = interval_start;
        size_t bytes_copied = 0;

        while (bytes_copied < interval_size)
        {
          uintptr_t page_start =
              reinterpret_cast<uintptr_t>(Common::GetPageAddress((void*)current_addr, pageSize));

          auto map_it = pageToIndexMap.find(page_start);
          if (map_it == pageToIndexMap.end())
          {
            ERROR_LOG_FMT(
                BRAWLBACK,
                "Could not find page {:016x} for address {:016x} in interval [{:016x}, {:016x}]",
                page_start, current_addr, interval_start, interval_end);
            break;
          }

          size_t page_index = map_it->second;
          u8* page_data = (u8*)savestate.afterCopies[page_index];

          uintptr_t offset_in_page = current_addr - page_start;
          size_t bytes_left_in_page = pageSize - offset_in_page;
          size_t bytes_left_in_interval = interval_size - bytes_copied;
          size_t bytes_to_copy = std::min(bytes_left_in_page, bytes_left_in_interval);

          u8* src = page_data + offset_in_page;
          u8* dst = (u8*)current_addr;

          bool can_use_fast_memcpy =
              (bytes_to_copy % 32 == 0) && (IS_ALIGNED(src, 32)) && (IS_ALIGNED(dst, 32));

          if (can_use_fast_memcpy)
          {
            rbMemcpy(dst, src, bytes_to_copy);
          }
          else
          {
            memcpy(dst, src, bytes_to_copy);
          }

          bytes_copied += bytes_to_copy;
          current_addr += bytes_to_copy;
        }
      }
    }
    else
    {
#ifdef ENABLE_LOGGING
      INFO_LOG_FMT(BRAWLBACK, "Restoring all {} pages (no excludes)",
                   savestate.changedPages.size());
#endif

      for (u32 i = 0; i < savestate.changedPages.size(); i++)
      {
        rbMemcpy((void*)savestate.changedPages[i], (void*)savestate.afterCopies[i], pageSize);
      }
    }
#endif
  }
  void DumpMemoryState(s32 frame, const std::string& reason)
  {
    std::string filename = fmt::format("memory_dump_frame_{}_{}.txt", frame, reason);
    std::ofstream dump_file(filename);

    if (!dump_file.is_open())
    {
      ERROR_LOG_FMT(BRAWLBACK, "Failed to create memory dump file: {}", filename);
      return;
    }

    dump_file << fmt::format("=== MEMORY DUMP: Frame {} ===\n", frame);
    dump_file << fmt::format("Reason: {}\n\n", reason);

    u64 total_hash = CalculateMemoryHash(g_critical_regions);
    dump_file << fmt::format("Total Hash: {:016x}\n\n", total_hash);

    for (const auto& region : g_critical_regions)
    {
      if (!region.ptr)
      {
        dump_file << fmt::format("Region '{}': NULL\n", region.name);
        continue;
      }

      // Calculate region hash
      u64 region_hash = 0xCBF29CE484222325ULL;
      const u64 prime = 0x100000001B3ULL;

      const u32* data = reinterpret_cast<const u32*>(region.ptr);
      size_t dwords = region.size / 4;

      for (size_t i = 0; i < dwords; ++i)
      {
        region_hash ^= static_cast<u64>(data[i]);
        region_hash *= prime;
      }

      dump_file << fmt::format("Region '{}': size={}, hash={:016x}\n", region.name, region.size,
                               region_hash);

      // Dump region
      dump_file << "  First 64 bytes: ";
      for (size_t i = 0; i < region.size; ++i)
      {
        dump_file << fmt::format("{:02x} ", region.ptr[i]);
        if ((i + 1) % 16 == 0)
          dump_file << "\n                  ";
      }
      dump_file << "\n\n";
    }

    dump_file.close();
    INFO_LOG_FMT(BRAWLBACK, "Memory dump written to: {}", filename);
  }
  bool Rollback(s32 currentFrame, s32 rollbackFrame)
  {
    // PROFILE_FUNCTION();
    if (currentFrame < MAX_SAVESTATES)
      return false;

    // Calculate how many frames we need to roll back
    s32 savestateOffset = currentFrame - rollbackFrame;

    if (rollbackFrame >= currentFrame || savestateOffset >= MAX_ROLLBACK_FRAMES ||
        savestateOffset <= 0)
    {
      ERROR_LOG_FMT(BRAWLBACK, "Invalid rollback: current={}, target={}, offset={}", currentFrame,
                    rollbackFrame, savestateOffset);
      return false;
    }

    // Savestates are captured at the END of each frame
    // To rollback to frame N, we need the savestate from frame N-1
    s32 currentSavestateIdx = Wrap(currentFrame - 1, MAX_SAVESTATES);

    // The target savestate is the one captured at the end of (rollbackFrame - 1)
    // which gets us to the start of rollbackFrame
    s32 targetSavestateIdx = Wrap(rollbackFrame - 1, MAX_SAVESTATES);

#ifdef ENABLE_LOGGING
    INFO_LOG_FMT(BRAWLBACK, "Starting at game mem frame {}\n", currentFrame);
    INFO_LOG_FMT(BRAWLBACK, "Rolling back {} frames from idx {} -> {} | frame {} -> {}\n",
                 savestateOffset, currentSavestateIdx, targetSavestateIdx, currentFrame,
                 rollbackFrame);
    INFO_LOG_FMT(BRAWLBACK, "Savestate frames stored:\n");
    for (u32 i = 0; i < MAX_SAVESTATES; i++)
    {
      INFO_LOG_FMT(BRAWLBACK, "| idx {} = frame {} |\t", i, savestateInfo.savestates[i].frame);
    }
    INFO_LOG_FMT(BRAWLBACK, "\n");
#endif

    if (targetSavestateIdx >= MAX_SAVESTATES)
    {
      ERROR_LOG_FMT(BRAWLBACK, "Invalid rollback indices: current={}, target={}",
                    currentSavestateIdx, targetSavestateIdx);
      return false;
    }

    // **FIX: Look for verification hash at the TARGET frame (not N-1)**
    // The hash recorded at frame N-1 represents the state at the START of frame N
    u64 expected_hash = 0;
    SavestateVerification* verification_point = nullptr;

    // Look for the hash recorded at the end of rollbackFrame-1
    // which represents the start of rollbackFrame
    auto verification_it = std::find_if(
        g_verification_history.begin(), g_verification_history.end(),
        [rollbackFrame](const SavestateVerification& v) { return v.frame == rollbackFrame - 1; });

    if (verification_it != g_verification_history.end())
    {
      expected_hash = verification_it->memory_hash;
      verification_point = &*verification_it;
      INFO_LOG_FMT(BRAWLBACK, "Found verification point: frame {} hash={:016x}", rollbackFrame - 1,
                   expected_hash);
    }

    // Apply savestates in reverse order to roll back
    s32 steps = 0;
    while (currentSavestateIdx != targetSavestateIdx)
    {
      Savestate& savestate = savestateInfo.savestates[currentSavestateIdx];

      if (!savestate.valid)
      {
        ERROR_LOG_FMT(BRAWLBACK, "Invalid savestate at idx {} (frame {})", currentSavestateIdx,
                      savestate.frame);
        return false;
      }

      INFO_LOG_FMT(BRAWLBACK, "Applying savestate idx {} (frame {})", currentSavestateIdx,
                   savestate.frame);
      RollbackSavestate(savestate);

      currentSavestateIdx = Wrap(currentSavestateIdx - 1, MAX_SAVESTATES);
      steps++;

      // Safety check to prevent infinite loops
      if (steps > MAX_ROLLBACK_FRAMES)
      {
        ERROR_LOG_FMT(BRAWLBACK, "Rollback exceeded maximum steps!");
        return false;
      }
    }

    // Apply the final target savestate to get to the start of rollbackFrame
    Savestate& finalState = savestateInfo.savestates[targetSavestateIdx];
    if (!finalState.valid)
    {
      ERROR_LOG_FMT(BRAWLBACK, "Invalid final savestate at idx {} (frame {})", targetSavestateIdx,
                    finalState.frame);
      return false;
    }

    INFO_LOG_FMT(BRAWLBACK, "Applying final savestate idx {} (frame {})", targetSavestateIdx,
                 finalState.frame);
    RollbackSavestate(finalState);

    if (verification_point)
    {
      const u64 current_hash = CalculateMemoryHash(g_critical_regions);
      if (current_hash != expected_hash)
      {
        ERROR_LOG_FMT(BRAWLBACK,
                      "VERIFICATION FAILED at frame {}: expected hash {:016x}, got {:016x}",
                      verification_point->frame, expected_hash, current_hash);
        DumpMemoryState(rollbackFrame, "rollback_verification_failed");
        return false;
      }

      verification_point->verified = true;
      INFO_LOG_FMT(BRAWLBACK, "Verification PASSED for frame {}: hash={:016x}",
                   verification_point->frame, current_hash);
    }

#ifdef ENABLE_LOGGING
    INFO_LOG_FMT(BRAWLBACK, "Rollback complete: applied {} savestates", steps + 1);
#endif

    return true;
  }

  void AddFrameCounterLocation(bu32 frameCounterPtr)
  {
    auto frameCounterInt = boost::icl::construct<boost::icl::discrete_interval<uintptr_t>>(
        frameCounterPtr, frameCounterPtr + 0x4, boost::icl::interval_bounds::closed());

    excludeSet -= frameCounterInt;
  }

  void EvictSavestate(Savestate& savestate)
  {
    //PROFILE_FUNCTION();
    // free up all the page snapshots tied to it
    arena_clear(&savestate.arena);
    savestate.afterCopies.clear();
    savestate.changedPages.clear();
    savestate.valid = false;
  }

  void OnPagesWritten(Savestate& savestate)
  {
    //PROFILE_FUNCTION();
    u64 pageSize = Common::PageSize();
    // for parallelization, need to do the allocation stuff first since this needs to be serial.
    // Arenas are not threadsafe
    for (u32 i = 0; i < savestate.changedPages.size(); i++)
    {
      savestate.afterCopies.push_back(reinterpret_cast<uintptr_t>(arena_alloc(&savestate.arena, pageSize)));
    }
  #ifdef MULTITHREAD
    u32 pagesPerThread = savestate.numChangedPages / numWorkerThreads;
    for (u32 i = 0; i < numWorkerThreads; i++)
    {
      u32 pageOffset = i * pagesPerThread;
      jobsystem::Execute(jobctx,
                         [pageOffset, pagesPerThread, pageSize, &savestate](jobsystem::JobArgs args) {
                           //PROFILE_FUNCTION();
                           // if numWorkerThreads is 3, we do pages in chunks like this: [0,333),
                           // [333, 666), [666, 999)
                           u32 endPageIdx = pageOffset + pagesPerThread;
                           for (u32 pageIdx = pageOffset; pageIdx < endPageIdx; pageIdx++)
                           {
                             u8* changedGameMemPage = (u8*)savestate.changedPages[pageIdx];
                             assert((changedGameMemPage >= GetRAM() &&
                                    changedGameMemPage < GetRAM() + GetRAMSize()) || (changedGameMemPage >= GetEXRAM() &&
                                    changedGameMemPage < GetEXRAM() + GetEXRAMSize()));
                             rbMemcpy(savestate.afterCopies[pageIdx], changedGameMemPage, pageSize);
                           }
                         });
    }
    if (numWorkerThreads % 2 != 0)
    {
      // odd number of worker threads means we can't evenly split up the work, so do the last bit here
      u32 pageIdx = savestate.numChangedPages - 1;
      char* changedGameMemPage = (char*)savestate.changedPages[pageIdx];
      rbMemcpy(savestate.afterCopies[pageIdx], changedGameMemPage, pageSize);
    }
    jobsystem::Wait(jobctx);
    
    
  #else
    for (u32 i = 0; i < savestate.changedPages.size(); i++)
    {
      //PROFILE_SCOPE("save page");
      //assert((changedGameMemPage >= GetRAM() && changedGameMemPage < GetRAM() + GetRAMSize()) ||
          //(changedGameMemPage >= GetEXRAM() && changedGameMemPage < GetEXRAM() + GetEXRAMSize()));
      rbMemcpy((u8*)savestate.afterCopies[i], (u8*)savestate.changedPages[i], pageSize);
    }
  #endif
  }

  void SaveWrittenPages(u32 frame, bool resim)
  {
    //PROFILE_FUNCTION();
    u32 savestateHead = frame % MAX_SAVESTATES;
    Savestate& savestate = savestateInfo.savestates[savestateHead];
    if (savestate.valid && !resim)
    {
      #ifdef ENABLE_LOGGING
        INFO_LOG_FMT(BRAWLBACK, "EVICTING SAVESTATE!\n");
      #endif
      EvictSavestate(savestate);
    }
    savestate.frame = frame;
    savestate.valid = GetAndResetWrittenPages(savestate.changedPages,
                                              MAX_NUM_CHANGED_PAGES);
    
    assert(savestate.valid);
    assert(savestate.changedPages.size() <= MAX_NUM_CHANGED_PAGES);
    OnPagesWritten(savestate);

  #ifdef ENABLE_LOGGING
    u64 numChangedBytes = savestate.changedPages.size() * Common::PageSize();
    float changedMB = numChangedBytes / 1024.0 / 1024.0;
    INFO_LOG_FMT(BRAWLBACK, "Frame {}, head = {}\tNum changed pages = {}\tChanged MB = {}\n",
                 frame, savestateHead, savestate.changedPages.size(), changedMB);
  #endif
  }

  void OnFrameEnd(s32 frame, bool resim)
  {
    SaveWrittenPages(frame, resim);
    bool should_verify = frame > 120;

    if (should_verify && !resim)
    {
      u64 hash = CalculateMemoryHash(g_critical_regions);
      RecordVerificationPoint(frame, hash);
      if (NetPlay::netplay_client)
      {
        NetPlay::netplay_client->SendRollbackVerification(frame, hash);
      }
    }
  }

  void Shutdown()
  {
    for (auto savestate : savestateInfo.savestates)
    {
      _mm_free(savestate.arena.backing_mem);
    }
    jobsystem::ShutDown();
  }

  void InitializeCriticalRegions()
  {
    g_critical_regions.clear();

    INFO_LOG_FMT(BRAWLBACK, "=== Initializing Critical Memory Regions ===");

    if (GetRAM())
    {
      u8* system_ptr = GetPointer(0x80611f60);
      if (system_ptr)
      {
        g_critical_regions.push_back({system_ptr, 0x61500, "System"});
        INFO_LOG_FMT(BRAWLBACK, "Added System: addr={:016x}, size=0x61500",
                     reinterpret_cast<uintptr_t>(system_ptr));
      }

      u8* fighter1_ptr = GetPointer(0x8123ab60);
      if (fighter1_ptr)
      {
        g_critical_regions.push_back({fighter1_ptr, 335872, "Fighter1"});
        INFO_LOG_FMT(BRAWLBACK, "Added Fighter1: addr={:016x}, size=335872",
                     reinterpret_cast<uintptr_t>(fighter1_ptr));
      }

      u8* fighter2_ptr = GetPointer(0x8128cb60);
      if (fighter2_ptr)
      {
        g_critical_regions.push_back({fighter2_ptr, 335872, "Fighter2"});
        INFO_LOG_FMT(BRAWLBACK, "Added Fighter2: addr={:016x}, size=335872",
                     reinterpret_cast<uintptr_t>(fighter2_ptr));
      }

      u8* physics_ptr = GetPointer(0x8154e560);
      if (physics_ptr)
      {
        g_critical_regions.push_back({physics_ptr, 734208, "Physics"});
        INFO_LOG_FMT(BRAWLBACK, "Added Physics: addr={:016x}, size=734208",
                     reinterpret_cast<uintptr_t>(physics_ptr));
      }

      u8* stage_ptr = GetPointer(0x814ce460);
      if (stage_ptr)
      {
        g_critical_regions.push_back({stage_ptr, 524544, "Stage"});
        INFO_LOG_FMT(BRAWLBACK, "Added Stage: addr={:016x}, size=524544",
                     reinterpret_cast<uintptr_t>(stage_ptr));
      }
    }

    if (GetEXRAM())
    {
      u8* game_global_ptr = GetPointer(0x90167400);
      if (game_global_ptr)
      {
        g_critical_regions.push_back({game_global_ptr, 205824, "GameGlobal"});
        INFO_LOG_FMT(BRAWLBACK, "Added GameGlobal: addr={:016x}, size=205824",
                     reinterpret_cast<uintptr_t>(game_global_ptr));
      }

      u8* fighter_res1_ptr = GetPointer(0x9151fa00);
      if (fighter_res1_ptr)
      {
        g_critical_regions.push_back({fighter_res1_ptr, 5583872, "FighterResource1"});
        INFO_LOG_FMT(BRAWLBACK, "Added FighterResource1: addr={:016x}, size=5583872",
                     reinterpret_cast<uintptr_t>(fighter_res1_ptr));
      }

      u8* fighter_res2_ptr = GetPointer(0x91b04c80);
      if (fighter_res2_ptr)
      {
        g_critical_regions.push_back({fighter_res2_ptr, 5583872, "FighterResource2"});
        INFO_LOG_FMT(BRAWLBACK, "Added FighterResource2: addr={:016x}, size=5583872",
                     reinterpret_cast<uintptr_t>(fighter_res2_ptr));
      }
    }

    INFO_LOG_FMT(BRAWLBACK,
                 "=== Initialized {} critical memory regions ===", g_critical_regions.size());
  }

  u64 CalculateMemoryHash(const std::vector<CriticalMemoryRegion>& regions)
  {
    // FNV-1a hash
    u64 hash = 0xCBF29CE484222325ULL;
    const u64 prime = 0x100000001B3ULL;

    for (const auto& region : regions)
    {
      if (!region.ptr)
        continue;

      // Hash the region name first for better distribution
      for (char c : region.name)
      {
        hash ^= static_cast<u64>(c);
        hash *= prime;
      }

      // Hash memory content in 4-byte chunks for efficiency
      const u32* data = reinterpret_cast<const u32*>(region.ptr);
      size_t dwords = region.size / 4;

      for (size_t i = 0; i < dwords; ++i)
      {
        u32 value = data[i];
        hash ^= static_cast<u64>(value);
        hash *= prime;
      }

      // Handle remaining bytes
      size_t remaining = region.size % 4;
      if (remaining > 0)
      {
        const u8* tail = reinterpret_cast<const u8*>(&data[dwords]);
        for (size_t i = 0; i < remaining; ++i)
        {
          hash ^= static_cast<u64>(tail[i]);
          hash *= prime;
        }
      }
    }

    return hash;
  }

  void RecordVerificationPoint(s32 frame, u64 hash)
  {
    SavestateVerification verification;
    verification.frame = frame;
    verification.memory_hash = hash;
    verification.verified = false;
    verification.timestamp = std::chrono::steady_clock::now();

    g_verification_history.push_back(verification);

    // Keep only last 120 frames (2 seconds at 60fps)
    if (g_verification_history.size() > 120)
    {
      g_verification_history.erase(g_verification_history.begin());
    }

    INFO_LOG_FMT(BRAWLBACK, "Verification point recorded: frame={}, hash={:016x}", frame, hash);
  }

  bool VerifyRollbackState(s32 frame)
  {
    // Find the verification point for this frame
    auto it = std::find_if(g_verification_history.begin(), g_verification_history.end(),
                           [frame](const SavestateVerification& v) { return v.frame == frame; });

    if (it == g_verification_history.end())
    {
      WARN_LOG_FMT(BRAWLBACK, "No verification point found for frame {}", frame);
      return true;  // Can't verify, assume OK
    }

    // Calculate current hash
    u64 current_hash = CalculateMemoryHash(g_critical_regions);

    if (current_hash != it->memory_hash)
    {
      ERROR_LOG_FMT(BRAWLBACK,
                    "VERIFICATION FAILED at frame {}: expected hash {:016x}, got {:016x}", frame,
                    it->memory_hash, current_hash);

      // Log which regions differ
      for (const auto& region : g_critical_regions)
      {
        if (!region.ptr)
          continue;

        u64 region_hash = 0xCBF29CE484222325ULL;
        const u64 prime = 0x100000001B3ULL;

        const u32* data = reinterpret_cast<const u32*>(region.ptr);
        size_t dwords = region.size / 4;

        for (size_t i = 0; i < dwords; ++i)
        {
          region_hash ^= static_cast<u64>(data[i]);
          region_hash *= prime;
        }

        INFO_LOG_FMT(BRAWLBACK, "  Region '{}': hash={:016x}", region.name, region_hash);
      }

      return false;
    }

    it->verified = true;
    INFO_LOG_FMT(BRAWLBACK, "Verification PASSED for frame {}: hash={:016x}", frame, current_hash);

    return true;
  }
  }
