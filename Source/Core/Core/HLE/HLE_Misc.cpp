// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE_Misc.h"

#include <chrono>
#include <thread>

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/HW/CPU.h"
#include "Core/HW/Memmap.h"
#include "Core/Host.h"
#include "Core/NetPlayClient.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/RollbackManager.h"
#include "Core/System.h"

namespace HLE_Misc
{
// If you just want to kill a function, one of the three following are usually appropriate.
// According to the PPC ABI, the return value is always in r3.
void UnimplementedFunction(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  ppc_state.npc = LR(ppc_state);
}

void HBReload(const Core::CPUThreadGuard& guard)
{
  // There isn't much we can do. Just stop cleanly.
  auto& system = guard.GetSystem();
  system.GetCPU().Break();
  Host_Message(HostMessageID::WMUserStop);
}

void GeckoCodeHandlerICacheFlush(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();
  auto& jit_interface = system.GetJitInterface();

  // Work around the codehandler not properly invalidating the icache, but
  // only the first few frames.
  // (Project M uses a conditional to only apply patches after something has
  // been read into memory, or such, so we do the first 5 frames.  More
  // robust alternative would be to actually detect memory writes, but that
  // would be even uglier.)
  u32 gch_gameid = PowerPC::MMU::HostRead<u32>(guard, Gecko::INSTALLER_BASE_ADDRESS);
  if (gch_gameid - Gecko::MAGIC_GAMEID == 5)
  {
    return;
  }
  else if (gch_gameid - Gecko::MAGIC_GAMEID > 5)
  {
    gch_gameid = Gecko::MAGIC_GAMEID;
  }
  PowerPC::MMU::HostWrite<u32>(guard, gch_gameid + 1, Gecko::INSTALLER_BASE_ADDRESS);

  ppc_state.iCache.Reset(jit_interface);
}

// Because Dolphin messes around with the CPU state instead of patching the game binary, we
// need a way to branch into the GCH from an arbitrary PC address. Branching is easy, returning
// back is the hard part. This HLE function acts as a trampoline that restores the original LR, SP,
// and PC before the magic, invisible BL instruction happened.
void GeckoReturnTrampoline(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  // Stack frame is built in GeckoCode.cpp, Gecko::RunCodeHandler.
  const u32 SP = ppc_state.gpr[1];
  ppc_state.gpr[1] = PowerPC::MMU::HostRead<u32>(guard, SP + 8);
  ppc_state.npc = PowerPC::MMU::HostRead<u32>(guard, SP + 12);
  LR(ppc_state) = PowerPC::MMU::HostRead<u32>(guard, SP + 16);
  ppc_state.cr.Set(PowerPC::MMU::HostRead<u32>(guard, SP + 20));
  for (int i = 0; i < 14; ++i)
  {
    ppc_state.ps[i].SetBoth(
        PowerPC::MMU::HostRead<u64>(guard, SP + 24 + 2 * i * sizeof(u64)),
        PowerPC::MMU::HostRead<u64>(guard, SP + 24 + (2 * i + 1) * sizeof(u64)));
  }
}

static constexpr u32 BRAWL_UNCONDITIONAL_HOOK_ADDR = 0x800171b4;
static constexpr u32 BRAWL_GAME_LOOP_HOOK_ADDR = 0x80017344;
static constexpr u32 BRAWL_GAME_LOOP_CONDITION_ADDR = 0x800173a4;
static constexpr u32 BRAWL_GAMEPROC_CALLSITE_ADDR = 0x80017350;
static constexpr u32 BRAWL_GAMEPROC_CALLSITE_NEXT_ADDR = 0x80017354;

void BrawlbackGekkoNetUnconditionalFrame(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  // Check if rollback mode is active
  const bool netplay_running = NetPlay::IsNetPlayRunning();
  const bool rollback_mode = NetPlay::IsInRollbackMode();

  if (!netplay_running || !rollback_mode)
  {
    // Not in rollback mode - execute original instruction and continue normally
    // Execute the original instruction: li r25, 0x1
    ppc_state.gpr[25] = (s16)0x1;
    ppc_state.npc = BRAWL_UNCONDITIONAL_HOOK_ADDR + 4;
    return;
  }

  static constexpr u32 BRAWL_LOOP_END_ADDR = 0x80017508;

  int current_iteration = NetPlay::GetCurrentIteration();

  if (current_iteration == 0)
  {
    NetPlay::OnFrameStart();
  }

  int total_iterations = NetPlay::GetFramesToAdvance();

  if (total_iterations == 0)
  {
    ppc_state.gpr[25] = (s16)0x1;
    ppc_state.npc = BRAWL_LOOP_END_ADDR;
    return;
  }

  if (current_iteration >= total_iterations)
  {
    // Execute the original instruction: li r25, 0x1
    ppc_state.gpr[25] = (s16)0x1;
    ppc_state.npc = BRAWL_LOOP_END_ADDR;
    return;
  }

  NetPlay::InjectPadsForIteration(current_iteration);

  // Execute the original instruction: li r25, 0x1
  ppc_state.gpr[25] = (s16)0x1;

  // Continue to next instruction
  ppc_state.npc = BRAWL_UNCONDITIONAL_HOOK_ADDR + 4;
}

void BrawlbackGekkoNetFrameEnd(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  static constexpr u32 BRAWL_FRAME_END_NEXT_ADDR = 0x80017508;

  if (!NetPlay::IsNetPlayRunning() || !NetPlay::IsInRollbackMode())
  {
    // Execute the original instruction: stw r0,0x100(r23)
    const u32 addr = ppc_state.gpr[23] + 0x100;
    PowerPC::MMU::HostWrite<u32>(guard, ppc_state.gpr[0], addr);
    ppc_state.npc = BRAWL_FRAME_END_NEXT_ADDR;
    return;
  }

  int current_iteration = NetPlay::GetCurrentIteration();

  if (NetPlay::ShouldSaveAfterIteration(current_iteration))
  {
    Rollback::RollbackManager::Get().SaveFrame(Core::System::GetInstance());
    NetPlay::WriteGekkoChecksumForIteration(
        current_iteration,
        Rollback::CalculateBrawlbackDesyncChecksum(Core::System::GetInstance()));
  }

  // Execute the original instruction: stw r0,0x100(r23)
  const u32 addr = ppc_state.gpr[23] + 0x100;
  PowerPC::MMU::HostWrite<u32>(guard, ppc_state.gpr[0], addr);
  ppc_state.npc = BRAWL_FRAME_END_NEXT_ADDR;
}

void BrawlbackGekkoNetLoopEnd(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  static constexpr u32 BRAWL_LOOP_START_ADDR = 0x800171b4;

  if (!NetPlay::IsNetPlayRunning() || !NetPlay::IsInRollbackMode())
  {
    ppc_state.npc = BRAWL_LOOP_START_ADDR;
    return;
  }

  if (NetPlay::GetShouldSleep())
  {
    NetPlay::PauseForLocalAdvantage();
  }

  int total_iterations = NetPlay::GetFramesToAdvance();

  if (total_iterations == 0)
  {
    ppc_state.npc = BRAWL_LOOP_START_ADDR;
    return;
  }

  int current_iteration = NetPlay::GetCurrentIteration();

  current_iteration++;

  if (current_iteration >= total_iterations)
  {
    NetPlay::SetCurrentIteration(0);
  }
  else
  {
    NetPlay::SetCurrentIteration(current_iteration);
  }

  ppc_state.npc = BRAWL_LOOP_START_ADDR;
}

// `bl OSSleepThread` inside DVDCancel's wait loop (dvd.o @ 0x801fb1a8). DVDCancel spins here,
// re-checking the DI command block's state field (at +0xc) until it reaches a terminal value
// (0, -1, 10, or a small set of command-specific codes), relying on the real DI completion
// interrupt to eventually call OSWakeupThread on this same queue. Across a rollback, the
// CoreTiming event driving that interrupt for the in-flight command can end up permanently
// lost/desynced from the resimulated command block, so the wakeup never arrives and the game
// hangs here forever.
static constexpr u32 BRAWL_DVDCANCEL_SLEEP_CALL_ADDR = 0x801fb1a8;
static constexpr u32 BRAWL_DVDCANCEL_LOOP_TOP_ADDR = 0x801fb134;
static constexpr u32 BRAWL_OSSLEEPTHREAD_ADDR = 0x801e1790;
static constexpr u32 BRAWL_DVD_CMD_BLOCK_STATE_OFFSET = 0xc;
static constexpr u32 BRAWL_DVD_STATE_DONE = 10;

void BrawlbackDVDCancelSleepHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode())
  {
    // Force the command block straight to "done" instead of actually sleeping, so DVDCancel's
    // loop condition (re-checked where we jump back to) is satisfied on its own rather than
    // waiting on a wakeup that may never come.
    const u32 command_block = ppc_state.gpr[30];
    PowerPC::MMU::HostWrite<u32>(guard, BRAWL_DVD_STATE_DONE,
                                 command_block + BRAWL_DVD_CMD_BLOCK_STATE_OFFSET);
    ppc_state.npc = BRAWL_DVDCANCEL_LOOP_TOP_ADDR;
    return;
  }

  // Not rollback netplay: replicate the original `bl OSSleepThread` we replaced.
  LR(ppc_state) = BRAWL_DVDCANCEL_SLEEP_CALL_ADDR + 4;
  ppc_state.npc = BRAWL_OSSLEEPTHREAD_ADDR;
}

// `bl OSSleepThread` inside nw4r::snd::detail::TaskManager::CancelTask's wait loop
// (snd_TaskManager.o @ 0x801cff38). CancelTask spins here waiting for the "currently executing
// task" pointer (at TaskManager+0x24) to stop matching the task being cancelled, relying on that
// task's own completion callback to advance/clear the pointer and call OSWakeupThread on this
// same queue (TaskManager+0x34). Same class of bug as DVDCancel: rollback resimulation can lose
// whatever real completion would normally clear it, hanging the game here forever.
static constexpr u32 BRAWL_CANCELTASK_SLEEP_CALL_ADDR = 0x801cff38;
static constexpr u32 BRAWL_CANCELTASK_LOOP_TOP_ADDR = 0x801cff3c;
static constexpr u32 BRAWL_CANCELTASK_CURRENT_TASK_OFFSET = 0x24;

void BrawlbackCancelTaskSleepHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode())
  {
    // Clear the "currently executing task" pointer instead of sleeping, so the loop condition
    // (re-checked where we jump back to) no longer matches and exits on its own.
    const u32 task_manager = ppc_state.gpr[25];
    PowerPC::MMU::HostWrite<u32>(guard, 0, task_manager + BRAWL_CANCELTASK_CURRENT_TASK_OFFSET);
    ppc_state.npc = BRAWL_CANCELTASK_LOOP_TOP_ADDR;
    return;
  }

  // Not rollback netplay: replicate the original `bl OSSleepThread` we replaced.
  LR(ppc_state) = BRAWL_CANCELTASK_SLEEP_CALL_ADDR + 4;
  ppc_state.npc = BRAWL_OSSLEEPTHREAD_ADDR;
}

// `bl OSSleepThread` inside OSLockMutex's contention loop (OSMutex.o @ 0x801dec5c). checkFileSD
// and the rest of gfFileIO (readDVDFile, readSDFile, etc.) all serialize on g_gfFileIO_Mutex
// (0x80494910), so if the thread holding it was doing real disc/file I/O whose completion got
// lost across a rollback the same way DVDCancel's did, every other thread calling OSLockMutex on
// it spins here forever waiting for an owner field that will never clear.
static constexpr u32 BRAWL_OSLOCKMUTEX_SLEEP_CALL_ADDR = 0x801dec5c;
static constexpr u32 BRAWL_OSLOCKMUTEX_LOOP_TOP_ADDR = 0x801debe8;
static constexpr u32 BRAWL_MUTEX_OWNER_OFFSET = 0x8;
static constexpr u32 BRAWL_GFFILEIO_MUTEX_ADDR = 0x80494910;

void BrawlbackFileIOMutexSleepHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  const u32 mutex_ptr = ppc_state.gpr[28];
  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode() &&
      mutex_ptr == BRAWL_GFFILEIO_MUTEX_ADDR)
  {
    // Clear the owner field instead of sleeping, so the normal acquire path (re-checked where
    // we jump back to) grabs the mutex on its own rather than waiting on a real completion that
    // may never come.
    PowerPC::MMU::HostWrite<u32>(guard, 0, mutex_ptr + BRAWL_MUTEX_OWNER_OFFSET);
    ppc_state.npc = BRAWL_OSLOCKMUTEX_LOOP_TOP_ADDR;
    return;
  }

  // Not our special-cased mutex (or not rollback netplay): replicate the original
  // `bl OSSleepThread` we replaced.
  LR(ppc_state) = BRAWL_OSLOCKMUTEX_SLEEP_CALL_ADDR + 4;
  ppc_state.npc = BRAWL_OSSLEEPTHREAD_ADDR;
}

// `bl OSSleepThread` inside DVDReadPrio's wait loop (dvdfs.o @ 0x801f68ec). Waits on the same DI
// queue as DVDCancel for its own read command's cb.state field to reach a terminal value. Forcing
// that state here (as we do for DVDCancel) is unsafe for a real data read: the actual RAM copy and
// cb.state update only happen once DVDThread::FinishRead's CoreTiming event runs, so forcing
// completion early can hand the game uninitialized memory it believes is valid file data. The
// real fix is in DVDInterface::ScheduleReads, which now schedules read completion at a negligible
// delay during rollback netplay so that event - and the real DI interrupt/OSWakeupThread it
// triggers - always arrives promptly instead of racing rollback resimulation. This hook just
// replicates the original `bl OSSleepThread` unconditionally.
static constexpr u32 BRAWL_DVDREADPRIO_SLEEP_CALL_ADDR = 0x801f68ec;

void BrawlbackDVDReadPrioSleepHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  LR(ppc_state) = BRAWL_DVDREADPRIO_SLEEP_CALL_ADDR + 4;
  ppc_state.npc = BRAWL_OSSLEEPTHREAD_ADDR;
}

// Render dispatch branch in gfApplication::mainLoopSub (@ 0x80017404), run once per pass through
// the Brawlback-hooked frame loop. Bit 0x10 of the app object's flags byte (+0xed) picks between
// the normal 3D scene render (renderNormal, 0x8001789c) and an overlay/menu render path - both
// converge at 0x8001746c. During rollback resimulation only the newest iteration's frame is ever
// actually shown, so skip straight past both paths for every earlier, throwaway iteration instead
// of wasting GPU time (and briefly flashing stale frames) drawing them.
static constexpr u32 BRAWL_RENDER_DISPATCH_ADDR = 0x80017404;
static constexpr u32 BRAWL_RENDER_NORMAL_BRANCH_ADDR = 0x80017464;
static constexpr u32 BRAWL_RENDER_ALT_BRANCH_ADDR = 0x80017410;
static constexpr u32 BRAWL_RENDER_DISPATCH_CONVERGE_ADDR = 0x8001746c;
static constexpr u32 BRAWL_APP_FLAGS_OFFSET = 0xed;
static constexpr u8 BRAWL_APP_ALT_RENDER_FLAG = 0x10;

void BrawlbackSkipResimRenderHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode() &&
      NetPlay::GetCurrentIteration() + 1 < NetPlay::GetFramesToAdvance())
  {
    // Not the newest frame this pass - skip straight past both render paths.
    ppc_state.npc = BRAWL_RENDER_DISPATCH_CONVERGE_ADDR;
    return;
  }

  // Replicate the original `lbz r0,0xed(r23); rlwinm. r0,r0,0x1c,0x1f,0x1f; beq ...` we replaced.
  const u32 app_ptr = ppc_state.gpr[23];
  const u8 flags = PowerPC::MMU::HostRead<u8>(guard, app_ptr + BRAWL_APP_FLAGS_OFFSET);
  ppc_state.npc = (flags & BRAWL_APP_ALT_RENDER_FLAG) ? BRAWL_RENDER_ALT_BRANCH_ADDR :
                                                        BRAWL_RENDER_NORMAL_BRANCH_ADDR;
}
}  // namespace HLE_Misc
