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
#include <Core/HW/Memmap.h>
#include <Core/IOS/IOS.h>
#include <Core/Movie.h>
#include <VideoCommon/VideoBackendBase.h>
#include <Core/GeckoCode.h>
#include <Core/PowerPC/PowerPC.h>
#include <Core/System.h>
#include <Core/Core.h>
#include <Common/MemoryUtil.h>

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

  void InitState(IncrementalRBCallbacks cb)
  {
    //PROFILE_FUNCTION();
    
    cbs = cb;
    ResetAllocs(savestateInfo);
    #ifdef SPECIFIC_TRACKING
    std::vector<Region> staticRegions = {
        {0x806414a0, 0x806414a0 + 0x60},
        {0x8062f440, 0x8062f440 + 0x500},
        {0x8063ff60, 0x8063ff60 + 0x1520},
        {0x8063dcc0, 0x8063dcc0 + 0x2280},
        {0x8063da80, 0x8063da80 + 0x220},
        {0x80624780, 0x80624780 + 0x50},
        {0x806232e0, 0x806232e0 + 0x1480},
        {0x806273c0, 0x806273c0 + 0x540},
        {0x80624800, 0x80624800 + 0x2ba0},
        {0x80622d20, 0x80622d20 + 0x388},
        {0x80621260, 0x80621260 + 0x1aa0},
        {0x8061f920, 0x8061f920 + 0x1920},
        {0x8061f460, 0x8061f460 + 0x4a0},
        {0x80615620, 0x80615620 + 0x6f20},
        {0x8061c560, 0x8061c560 + 0x2EE0},
        {0x8063d9a0, 0x8063d9a0 + 0xc0},
        {0x8063d8e0, 0x8063d8e0 + 0xa0},
        {0x806312e0, 0x806312e0 + 0xbba0},
        {0x8062fb40, 0x8062fb40 + 0x1780},
        {0x8062f9e0, 0x8062f9e0 + 0x140},
        {0x8062f960, 0x8062f960 + 0x60},
        {0x80663300, 0x80663300 + 0x140},  // this is a gfTask "EffectManager", if there's issues are a good place to start
        {0x80663280, 0x80663280 + 0x60},
        {0x80663060, 0x80663060 + 0x200},
        {0x8062b360, 0x8062b360 + 0x14c0},
        {0x80613e00, 0x80613e00 + 0x28},
        {0x80641520, 0x80641520 + 0x20},
        {0x80629a00, 0x80629a00 + 0x160},
        {0x80623180, 0x80623180 + 0x20},
        {0x806230e0, 0x806230e0 + 0x80},
        {0x80627920, 0x80627920 + 0x10c0},
        {0x8062f3e0, 0x8062f3e0 + 0x40},
        {0x8062f360, 0x8062f360 + 0x60},
        {0x8062c840, 0x8062c840 + 0x1e00},
        {0x80629980, 0x80629980 + 0x60},
        {0x80628a00, 0x80628a00 + 0xAA0},
        {0x8049edd8, 0x8049edd8 + 0x5064},
        {0x805b62a0, 0x805b62a0 + 0x100},
        {0x80662b40, 0x80662b40 + 0x500},
        {0x80662620, 0x80662620 + 0x500},
        {0x80672f40, 0x80672f40 + 0x520},
        {0x8066b5e0, 0x8066b5e0 + 0x4220},
        {0x806673a0, 0x806673a0 + 0x4220},
        {0x80663fe0, 0x80663fe0 + 0x33a0},
        {0x80672920, 0x80672920 + 0x600},
        {0x80672300, 0x80672300 + 0x600},
        {0x80671ce0, 0x80671ce0 + 0x600},
        {0x806716c0, 0x806716c0 + 0x600},
        {0x806710a0, 0x806710a0 + 0x600},
        {0x80670a80, 0x80670a80 + 0x600},
        {0x80670460, 0x80670460 + 0x600},
        {0x8066fe40, 0x8066fe40 + 0x600},
        {0x8066f820, 0x8066f820 + 0x600},
        {0x8049e57c, 0x8049e57c + 0xC},
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

      /*
      for (Region& staticRegion : staticRegions)
      {
        TrackAlloc(GetPointer(staticRegion.startAddress), staticRegion.endAddress - staticRegion.startAddress + 1);
      }*/
      
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
    for (int i = 0; i < physical_entries.size(); i++)
    {
      if (!physical_entries[i].active)
      {
        continue;
      }

      TrackAlloc(*physical_entries[i].out_pointer, physical_entries[i].size);
    }
    // Threading Stuff
    //ExcludeMem(GetPointer(0x80009760), 0x805b5158 - 0x80009760);  // Data Sections, BSS, Main Stack
    ExcludeMem(GetPointer(0x805a5154), 0x805b5158 - 0x805a5154);  // Main Stack
    //ExcludeMem(GetPointer(0x805bf420), 0x28);                    // ??? OSAlarm
    //ExcludeMem(GetPointer(0x805bacc0), 0x28);                    // PAD OSAlarm
    //ExcludeMem(GetPointer(0x805b85e0), 0x28);                    // OSALarmSleep OSAlarm
    // Heaps
    ExcludeMem(GetPointer(0x817ba5a0), 0x817ca5a0 - 0x817ba5a0); // Syringe
    ExcludeMem(GetPointer(0x94000000), 0x3FBFFFF);                // SavestateHeap
    //ExcludeMem(GetPointer(0x92163a00), 0x00de6700);              //  InfoExtraResource
    //ExcludeMem(GetPointer(0x914c9f00), 0x00c99b00);               //  MenuResource
    ExcludeMem(GetPointer(0x805d1e60), 0x00040100);              // RenderFifo
    ExcludeMem(GetPointer(0x9134cc00), 0x0012c200);              // CopyFB
    ExcludeMem(GetPointer(0x805ca260), 0x00007c00);              // Thread
    ExcludeMem(GetPointer(0x90199800), 0x00cc7c00);              // Sound
    //ExcludeMem(GetPointer(0x80b8db60), 0x80c23a60 - 0x80b8db60); // Effect*/

    // VI Related Addresses
    ExcludeMem(GetPointer(0x805A084C), 0x4);
    ExcludeMem(GetPointer(0x805A07BC), 0x4);
    ExcludeMem(GetPointer(0x805A07C0), 0x4);
    ExcludeMem(GetPointer(0x805A07B0), 0x4);
    ExcludeMem(GetPointer(0x805A07B4), 0x4);
    ExcludeMem(GetPointer(0x805A07B8), 0x4);

    // OSAlarm Stuff
    ExcludeMem(GetPointer(0x804DE240), 0x30);
    ExcludeMem(GetPointer(0x804DE0A0), 0x30);
    ExcludeMem(GetPointer(0x804E42C0), 0x30);
    ExcludeMem(GetPointer(0x804E4290), 0x30);
    ExcludeMem(GetPointer(0x804E4360), 0x30);
    ExcludeMem(GetPointer(0x804F67E0), 0x30);
    ExcludeMem(GetPointer(0x804FBBB0), 0x30);
    ExcludeMem(GetPointer(0x805297A0), 0x30);
    ExcludeMem(GetPointer(0x80495418), 0x30);
    ExcludeMem(GetPointer(0x80495448), 0x30);
    ExcludeMem(GetPointer(0x80495478), 0x30);
    ExcludeMem(GetPointer(0x804954A8), 0x30);
    ExcludeMem(GetPointer(0x80532188), 0x30);
    ExcludeMem(GetPointer(0x805321B8), 0x30);
    ExcludeMem(GetPointer(0x805321E8), 0x30);
    ExcludeMem(GetPointer(0x80532218), 0x30);
    ExcludeMem(GetPointer(0x80532248), 0x30);
    ExcludeMem(GetPointer(0x80532278), 0x30);
    ExcludeMem(GetPointer(0x805322A8), 0x30);
    ExcludeMem(GetPointer(0x805322D8), 0x30);
    ExcludeMem(GetPointer(0x804ECEE0), 0x30);
    ExcludeMem(GetPointer(0x804ECFF8), 0x30);
    ExcludeMem(GetPointer(0x804DE3F0), 0x30);
    ExcludeMem(GetPointer(0x804DE420), 0x30);
    ExcludeMem(GetPointer(0x804DE450), 0x30);
    ExcludeMem(GetPointer(0x804DE480), 0x30);

    // OSThread Stuff
    ExcludeMem(GetPointer(0x804DD558), 0x318);
    ExcludeMem(GetPointer((u32)0x805A01E0 + 0x8), 0x318);
    ExcludeMem(GetPointer((u32)0x805A0200 + 0xB0), 0x318);
    ExcludeMem(GetPointer((u32)0x805A0208 + 0x8), 0x318);
    ExcludeMem(GetPointer((u32)0x805A0FE8 + 0x51C0), 0x318);

    ExcludeMem(GetPointer(0x804976F0), 0x8);
    ExcludeMem(GetPointer(0x80497718), 0x8);
    ExcludeMem(GetPointer(0x80497740), 0x8);
    ExcludeMem(GetPointer(0x80497768), 0x8);
    ExcludeMem(GetPointer(0x8049EB50), 0x8);
    ExcludeMem(GetPointer(0x8049EC30), 0x8);
    ExcludeMem(GetPointer(0x8049EC48), 0x8);
    ExcludeMem(GetPointer(0x8049EC60), 0x8);
    ExcludeMem(GetPointer(0x8049EC78), 0x8);
    ExcludeMem(GetPointer(0x804D8FD8), 0x8);
    ExcludeMem(GetPointer(0x804D8FE0), 0x8);
    ExcludeMem(GetPointer(0x804D8FE8), 0x8);
    ExcludeMem(GetPointer(0x804DD080), 0x8);
    ExcludeMem(GetPointer(0x804DD0D0), 0x8);
    ExcludeMem(GetPointer(0x804DD0D8), 0x8);
    ExcludeMem(GetPointer(0x804DD840), 0x8);
    ExcludeMem(GetPointer(0x804ECE8C), 0x8);
    ExcludeMem(GetPointer(0x804ECFA4), 0x8);
    ExcludeMem(GetPointer(0x804ED380), 0x8);
    ExcludeMem(GetPointer(0x804F65CC), 0x8);
    ExcludeMem(GetPointer(0x804F8124), 0x8);
    ExcludeMem(GetPointer(0x804F8AC4), 0x8);
    ExcludeMem(GetPointer(0x804F9464), 0x8);
    ExcludeMem(GetPointer(0x804F9E04), 0x8);
    ExcludeMem(GetPointer(0x805344B0), 0x8);
    ExcludeMem(GetPointer(0x805344D8), 0x8);
    ExcludeMem(GetPointer(0x80534580), 0x8);
    ExcludeMem(GetPointer(0x805365E4), 0x8);
    ExcludeMem(GetPointer(0x805365FC), 0x8);
    ExcludeMem(GetPointer(0x805A03C8), 0x8);
    ExcludeMem(GetPointer(0x805A0498), 0x8);
    ExcludeMem(GetPointer(0x805A0828), 0x8);
    ExcludeMem(GetPointer(0x805A08B8), 0x8);
    ExcludeMem(GetPointer(0x805A08D8), 0x8);
    ExcludeMem(GetPointer(0x805A0AB0), 0x8);
    ExcludeMem(GetPointer(0x804DD870), 0x100);


    std::sort(ExcludeMemList.begin(), ExcludeMemList.end(), [](const ExcludeBuffer& a, const ExcludeBuffer& b){ return a.buffer.data < b.buffer.data; });

    for (u32 i = 0; i < ExcludeMemList.size(); i++)
    {
      excludeSet.insert(ExcludeMemList[i].excludeGap);
    }
    
    #endif
    jobsystem::Initialize(
        numWorkerThreads -
        1);  // -1 because when we do our async and join stuff, main thread also becomes a worker
    assert(IS_ALIGNED(GetRAM(), 32));  // for simd memcpy, need to be 32 byte aligned
    assert(IS_ALIGNED(GetEXRAM(), 32));  // for simd memcpy, need to be 32 byte aligned

    // allocate mem for savestates
    u64 savestateMemSize = MAX_NUM_CHANGED_PAGES * Common::PageSize();
    for (Savestate& savestate : savestateInfo.savestates)
    {
      void* backingMem = _mm_malloc(savestateMemSize, 32);
      assert(IS_ALIGNED(backingMem, 32));
      savestate.arena = arena_init(backingMem, savestateMemSize);
    }
  }

  s32 Wrap(s32 x, s32 wrap)
  {
    if (x < 0)
      x = (wrap + x);
    return abs(x) % wrap;
  }

  static void RollbackSavestate(const Savestate& savestate)
  {
    //PROFILE_FUNCTION();
    u64 pageSize = Common::PageSize();
  #ifdef MULTITHREAD
    u32 pagesPerThread = savestate.numChangedPages / numWorkerThreads;
    for (u32 i = 0; i < numWorkerThreads; i++)
    {
      u32 pageOffset = i * pagesPerThread;
      jobsystem::Execute(jobctx, [pageOffset, pagesPerThread, &savestate, pageSize,
                                  i](jobsystem::JobArgs args) {
        //PROFILE_FUNCTION();
        u32 endPageIdx = pageOffset + pagesPerThread;
        for (u32 pageIdx = pageOffset; pageIdx < endPageIdx; pageIdx++)
        {
          void* orig = savestate.changedPages[pageIdx];
          void* ssData = savestate.afterCopies[pageIdx];
          #ifdef ENABLE_LOGGING
          assert((orig >= GetRAM() && orig < GetRAM() + GetRAMSize()) ||
                 (orig >= GetEXRAM() &&
                  orig < GetEXRAM() + GetEXRAMSize()));
          // first 4 bytes of game mem contains current frame
          if (orig == GetGameMemFrame())
          {
            u32 nowFrame = *GetGameMemFrame();
            INFO_LOG_FMT(BRAWLBACK, "rolling back {} -> {}\n", nowFrame, ((u32*)ssData)[0]);
          }
          #endif
          rbMemcpy(orig, ssData, pageSize);
        }
      });
    }
    if (numWorkerThreads % 2 != 0)
    {
      // odd number of worker threads means we can't evenly split up the work, so do the last bit here
      u32 pageIdx = savestate.numChangedPages - 1;
      void* orig = savestate.changedPages[pageIdx];
      void* ssData = savestate.afterCopies[pageIdx];
      rbMemcpy(orig, ssData, pageSize);
    }
    jobsystem::Wait(jobctx);
  #else

    if (excludeSet.size() > 0)
    {
      // Build interval set from changed pages - each page becomes a closed interval [page, page+pageSize]
      TIntervalSet changedSet;
      for (u32 i = 0; i < savestate.changedPages.size(); i++)
      {
        changedSet += boost::icl::discrete_interval<uintptr_t>::closed(
            savestate.changedPages[i], savestate.changedPages[i] + pageSize - 1);
      }

      // Compute which memory ranges to actually copy (changed pages minus excluded regions)
      auto difference = changedSet - excludeSet;

      // For each resulting interval, copy from afterCopies to changedPages addresses
      for (auto it = difference.begin(); it != difference.end(); ++it)
      {
        uintptr_t copyStartAddr = it->lower();
        uintptr_t copyEndAddr = it->upper();
        size_t totalCopySize = (copyEndAddr - copyStartAddr) + 1; // +1 because interval is closed

        // Handle intervals that may span multiple pages
        uintptr_t currentAddr = copyStartAddr;
        size_t remainingBytes = totalCopySize;

        while (remainingBytes > 0)
        {
          // Find which page this address belongs to
          uintptr_t pageAddr = reinterpret_cast<uintptr_t>(Common::GetPageAddress((void*)currentAddr, pageSize));

          // Binary search for the page in changedPages
          auto pageIt = std::lower_bound(savestate.changedPages.begin(), savestate.changedPages.end(), pageAddr);
          if (pageIt == savestate.changedPages.end() || *pageIt != pageAddr)
          {
            // Page not found - skip to next page boundary
            size_t bytesToNextPage = pageSize - (currentAddr - pageAddr);
            currentAddr += bytesToNextPage;
            remainingBytes = (remainingBytes > bytesToNextPage) ? (remainingBytes - bytesToNextPage) : 0;
            continue;
          }

          size_t pageIndex = std::distance(savestate.changedPages.begin(), pageIt);

          // Calculate offset within the page where we start copying
          size_t offsetInPage = currentAddr - pageAddr;

          // Calculate how many bytes to copy from this page
          size_t bytesInThisPage = std::min(pageSize - offsetInPage, remainingBytes);

          // Source: afterCopies data at the found page index, offset by how far into the page we are
          u8* srcData = reinterpret_cast<u8*>(savestate.afterCopies[pageIndex]) + offsetInPage;

          // Destination: the actual address where we want to restore data
          u8* destData = reinterpret_cast<u8*>(currentAddr);

          // Copy the data for this page chunk
          if (bytesInThisPage == pageSize && offsetInPage == 0)
          {
            rbMemcpy(destData, srcData, bytesInThisPage);
          }
          else
          {
            memcpy(destData, srcData, bytesInThisPage);
          }

          currentAddr += bytesInThisPage;
          remainingBytes -= bytesInThisPage;
        }
      }
    }
    else
    {
      // No exclusions - simple case: copy all pages from afterCopies to changedPages addresses
      for (u32 i = 0; i < savestate.changedPages.size(); i++)
      {
        void* dest = (void*)savestate.changedPages[i];
        void* src = (void*)savestate.afterCopies[i];
        rbMemcpy(dest, src, pageSize);
      }
    }
    
  #endif
  }

  bool Rollback(s32 currentFrame, s32 rollbackFrame)
  {
    // PROFILE_FUNCTION();
    if (currentFrame < MAX_SAVESTATES)
    {
#ifdef ENABLE_LOGGING
      INFO_LOG_FMT(BRAWLBACK,
                   "Rollback rejected: currentFrame {} < MAX_SAVESTATES {}",
                   currentFrame, MAX_SAVESTATES);
#endif
      return false;
    }

   // -1 because all savestates are taken after a frame's simulation
    // this means if you want to rollback to frame 5, you'd actually need to restore the data
    // captured on frame 4
    s32 savestateOffset = currentFrame - rollbackFrame - 1; 
    if (rollbackFrame >= currentFrame || savestateOffset >= MAX_SAVESTATES)
    {
#ifdef ENABLE_LOGGING
      INFO_LOG_FMT(BRAWLBACK,
                   "Rollback rejected: invalid frame window (currentFrame={}, rollbackFrame={}, "
                   "savestateOffset={}, MAX_SAVESTATES={})",
                   currentFrame, rollbackFrame, savestateOffset, MAX_SAVESTATES);
#endif
      return false;
    }
    // -1 because we want to start rolling back on the index before the current frame
    // another -1 because our savestates are for the end of the frame, so need to go back another
    s32 currentSavestateIdx = Wrap(currentFrame - 1 - 1, MAX_SAVESTATES);
    s32 endingSavestateIdx = Wrap(currentSavestateIdx - savestateOffset, MAX_SAVESTATES);

    //assert(endingSavestateIdx < MAX_SAVESTATES && endingSavestateIdx != currentSavestateIdx);
#ifdef ENABLE_LOGGING
    INFO_LOG_FMT(BRAWLBACK, "Starting at game mem frame {}\n", currentFrame);
    INFO_LOG_FMT(BRAWLBACK, "Rolling back {} frames from idx {} -> {} | frame {} -> {}\n",
                 savestateOffset, currentSavestateIdx, endingSavestateIdx, *GetGameMemFrame(),
                 rollbackFrame);
    INFO_LOG_FMT(BRAWLBACK, "Savestate frames stored:\n");
    for (u32 i = 0; i < MAX_SAVESTATES; i++)
    {
      INFO_LOG_FMT(BRAWLBACK, "| idx {} = frame {} |\t", i, savestateInfo.savestates[i].frame);
    }
    INFO_LOG_FMT(BRAWLBACK, "\n");
#endif
    // given an index into our savestates list, rollback to that.
    // go from the current index, walking backward applying each savestate's "before" snapshots
    // one at a time

    if (endingSavestateIdx >= MAX_SAVESTATES)
    {
#ifdef ENABLE_LOGGING
      INFO_LOG_FMT(BRAWLBACK,
                   "Rollback rejected: invalid savestate index (endingSavestateIdx={}, "
                   "MAX_SAVESTATES={})",
                   endingSavestateIdx, MAX_SAVESTATES);
#endif
      return false;
    }
    while (currentSavestateIdx != endingSavestateIdx)
    {
      Savestate& savestate = savestateInfo.savestates[currentSavestateIdx];
      RollbackSavestate(savestate);
      currentSavestateIdx = Wrap(currentSavestateIdx - 1, MAX_SAVESTATES);
    }
    // summary -
    // we are on frame 15, trying to rollback to frame 10 since we didn't get inputs on frame 10,
    // have been predicting, and just got past inputs from 10
    // ---
    // we start at frame 14 because all snapshots are for the end of the frame, so since we're at
    // the beginning of frame 15, we're technically at the end of 14 so we go from 14->13 by
    // applying frame 13's changed pages, then 13->12, .... then 11->10. We stop here, but applying
    // 11->10 actually gets us to the end of frame 10/beginning of 11. We want to be at the
    // beginning of 10/end of 9 since that's where we need to reapply the new inputs and start
    // resimulating so we do one more at the end of this loop
    INFO_LOG_FMT(BRAWLBACK, "ASSERT CHECK 1: {} == {}?\n", savestateInfo.savestates[currentSavestateIdx].frame, rollbackFrame - 1);
    //assert(savestateInfo.savestates[currentSavestateIdx].frame == (u32)(rollbackFrame) - 1);
    RollbackSavestate(savestateInfo.savestates[currentSavestateIdx]);
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
    if (savestate.valid)
    {
      #ifdef ENABLE_LOGGING
        INFO_LOG_FMT(BRAWLBACK, "EVICTING SAVESTATE! (resim={})\n", resim);
      #endif
      EvictSavestate(savestate);
    }
    savestate.frame = frame;

  #ifdef ENABLE_LOGGING
    // Check if frame counter page is dirty BEFORE reset (observation only, no forcing)
    bool frameCounterDirty = Memory::isFramePointerDirty();
  #endif

    savestate.valid = GetAndResetWrittenPages(savestate.changedPages,
                                              MAX_NUM_CHANGED_PAGES);

    assert(savestate.valid);
    assert(savestate.changedPages.size() <= MAX_NUM_CHANGED_PAGES);
    OnPagesWritten(savestate);

  #ifdef ENABLE_LOGGING
    u64 numChangedBytes = savestate.changedPages.size() * Common::PageSize();
    float changedMB = numChangedBytes / 1024.0 / 1024.0;
    INFO_LOG_FMT(BRAWLBACK, "Frame {}, head = {}\tNum changed pages = {}\tChanged MB = {}\tFrame counter page dirty = {}\n",
                 frame, savestateHead, savestate.changedPages.size(), changedMB, frameCounterDirty);
  #endif
  }

  void OnFrameEnd(s32 frame, bool resim)
  {
    SaveWrittenPages(frame, resim);

  }

  void Shutdown()
  {
    for (auto savestate : savestateInfo.savestates)
    {
      _mm_free(savestate.arena.backing_mem);
    }
    jobsystem::ShutDown();
  }
}
