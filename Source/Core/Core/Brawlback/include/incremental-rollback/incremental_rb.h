#pragma once

#include "util.h"
#include <Common/ChunkFile.h>
#include <Core/HW/Memmap.h>

#ifdef DEBUG
#define ENABLE_LOGGING
#endif

namespace IncrementalRB
{
    // size of gamestate mem block
    typedef u32(*GetRAMSizeCb)();
    typedef u32 (*GetEXRAMSizeCb)();
    // gamestate mem block
    typedef u8* (*GetRAMCb)();
    typedef u8* (*GetEXRAMCb)();
    // [optional - for debugging] internal game frame derived from game memory, not our tracked frame 
    // we use this for asserts and stuff to make sure the internal game frame/data is on the frame we expect it to be
    // this location in memory should be part of the tracked allocation and should be written to every frame
    typedef u32*(*GetGameMemFrameCb)();

    typedef u8* (*GetPointerCb)(u32);

    typedef std::array<Memory::PhysicalMemoryRegion, 4> (*GetPhysicalRegionsCb)();

    struct IncrementalRBCallbacks
    {
        GetRAMSizeCb getRAMSize = nullptr;
        GetEXRAMSizeCb getEXRAMSize = nullptr;
        GetEXRAMSizeCb getEXRAMMask = nullptr;
        GetRAMCb getRAM = nullptr;
        GetEXRAMCb getEXRAM = nullptr;
        GetGameMemFrameCb getGameMemFrame = nullptr;
        GetPointerCb getPointer = nullptr;
        GetPhysicalRegionsCb getPhysicalRegions = nullptr;
    };
    // NOTE: gamestate pointer returned by the GetGameStateCb
    // must have been allocated with VirtualAlloc with the MEM_WRITE_WATCH flag
    // this sets our callbacks and tracks the memory block returned by GetGameStateCb and GetGamestateMemSizeCb
    void InitState(IncrementalRBCallbacks cb);
    // should be called at the END of every game simulation frame. Right now, this just saves the game state
    void SaveWrittenPages(u32 frame, bool resim);
    void OnFrameEnd(s32 frame, bool isResim);
    void Shutdown();

    bool Rollback(s32 currentFrame, s32 rollbackFrame);

    void AddFrameCounterLocation(bu32 frameCounterPtr);
    }
