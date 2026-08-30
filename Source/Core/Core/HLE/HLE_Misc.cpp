// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/HLE/HLE_Misc.h"

#include <chrono>
#include <thread>

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/HW/CPU.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
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
static constexpr u32 BRAWL_APP_SCENE_MANAGER_OFFSET = 0xd4;
static constexpr u32 BRAWL_SCENE_MANAGER_CURRENT_SCENE_OFFSET = 0x4;
static constexpr u32 BRAWL_SCENE_NAME_OFFSET = 0;

bool IsBootScene(const Core::CPUThreadGuard& guard, u32 app_ptr)
{
  static constexpr char boot_scene[] = "scBoot";
  const u32 scene_manager =
      PowerPC::MMU::HostRead<u32>(guard, app_ptr + BRAWL_APP_SCENE_MANAGER_OFFSET);
  if (scene_manager == 0)
    return false;

  const u32 current_scene = PowerPC::MMU::HostRead<u32>(
      guard, scene_manager + BRAWL_SCENE_MANAGER_CURRENT_SCENE_OFFSET);
  if (current_scene == 0)
    return false;

  const u32 scene_name = PowerPC::MMU::HostRead<u32>(guard, current_scene + BRAWL_SCENE_NAME_OFFSET);
  if (scene_name == 0)
    return false;

  for (u32 index = 0; boot_scene[index] != '\0'; ++index)
  {
    if (PowerPC::MMU::HostRead<u8>(guard, scene_name + index) != boot_scene[index])
      return false;
  }
  return true;
}

void BrawlbackGekkoNetUnconditionalFrame(const Core::CPUThreadGuard& guard)
{
  static bool gekko_sync_activated = false;
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  // Check if rollback mode is active
  const bool netplay_running = NetPlay::IsNetPlayRunning();
  const bool rollback_mode = NetPlay::IsInRollbackMode();

  if (!netplay_running || !rollback_mode)
  {
    gekko_sync_activated = false;
    NetPlay::SetGekkoCpuStalled(false);
    // Not in rollback mode - execute original instruction and continue normally
    // Execute the original instruction: li r25, 0x1
    ppc_state.gpr[25] = (s16)0x1;
    ppc_state.npc = BRAWL_UNCONDITIONAL_HOOK_ADDR + 4;
    return;
  }

  static constexpr u32 BRAWL_LOOP_END_ADDR = 0x80017508;

  // Start synchronized frame advancement once Brawl reaches scBoot. Keep it active after the
  // boot scene transitions so GekkoNet continues exchanging inputs in later scenes.
  if (!gekko_sync_activated && !IsBootScene(guard, ppc_state.gpr[23]))
  {
    NetPlay::SetGekkoCpuStalled(false);
    ppc_state.gpr[25] = 1;
    ppc_state.npc = BRAWL_UNCONDITIONAL_HOOK_ADDR + 4;
    return;
  }
  gekko_sync_activated = true;

  int current_iteration = NetPlay::GetCurrentIteration();

  if (current_iteration == 0)
  {
    // Must run every spin even while stalled below - this is what actually polls GekkoNet and
    // lets its handshake with the peer (and thus IsTimeSynced()) ever progress.
    NetPlay::OnFrameStart();
  }

  if (!NetPlay::IsTimeSynced())
  {
    NetPlay::SetGekkoCpuStalled(true);
    // GekkoNet's handshake with the peer hasn't finished yet - spin here without advancing any
    // game logic instead of letting the local game run ahead into the match on its own, which
    // is what let host and guest load in at noticeably different real times.
    ppc_state.gpr[25] = (s16)0x1;
    ppc_state.npc = BRAWL_LOOP_END_ADDR;
    return;
  }

  int total_iterations = NetPlay::GetFramesToAdvance();

  if (total_iterations == 0)
  {
    NetPlay::SetGekkoCpuStalled(true);
    ppc_state.gpr[25] = (s16)0x1;
    ppc_state.npc = BRAWL_LOOP_END_ADDR;
    return;
  }

  if (current_iteration >= total_iterations)
  {
    NetPlay::SetGekkoCpuStalled(true);
    // Execute the original instruction: li r25, 0x1
    ppc_state.gpr[25] = (s16)0x1;
    ppc_state.npc = BRAWL_LOOP_END_ADDR;
    return;
  }

  NetPlay::SetGekkoCpuStalled(false);
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

  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode() && IsResimulationPass())
  {
    // Force the command block straight to "done" instead of actually sleeping, so DVDCancel's
    // loop condition (re-checked where we jump back to) is satisfied on its own rather than
    // waiting on a wakeup that may never come.
    const u32 command_block = ppc_state.gpr[30];
    if (command_block != 0)
    {
      PowerPC::MMU::HostWrite<u32>(guard, BRAWL_DVD_STATE_DONE,
                                   command_block + BRAWL_DVD_CMD_BLOCK_STATE_OFFSET);
    }
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

  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode() && IsResimulationPass())
  {
    // Clear the "currently executing task" pointer instead of sleeping, so the loop condition
    // (re-checked where we jump back to) no longer matches and exits on its own.
    const u32 task_manager = ppc_state.gpr[25];
    if (task_manager != 0)
    {
      PowerPC::MMU::HostWrite<u32>(guard, 0, task_manager + BRAWL_CANCELTASK_CURRENT_TASK_OFFSET);
    }
    ppc_state.npc = BRAWL_CANCELTASK_LOOP_TOP_ADDR;
    return;
  }

  // Not rollback netplay: replicate the original `bl OSSleepThread` we replaced.
  LR(ppc_state) = BRAWL_CANCELTASK_SLEEP_CALL_ADDR + 4;
  ppc_state.npc = BRAWL_OSSLEEPTHREAD_ADDR;
}

// `bl OSSleepThread` inside GXDrawDone's wait loop (GXMisc.o @ 0x801f0ac0). GXDrawDone pushes a
// GX_DRAWDONE token and waits for the GPU's real PE-finish interrupt to set the done flag
// (r13-0x3b60). mainLoopSub calls this unconditionally at the top of every iteration to wait for
// the *previous* iteration's draw, but BrawlbackSkipResimRenderHook skips issuing any draw at all
// for throwaway resimulation iterations, so that interrupt can arrive late (or never) relative to
// a compressed resim burst, hanging here forever.
static constexpr u32 BRAWL_GXDRAWDONE_SLEEP_CALL_ADDR = 0x801f0ac0;
static constexpr u32 BRAWL_GXDRAWDONE_RECHECK_ADDR = 0x801f0ac4;
static constexpr u32 BRAWL_GXDRAWDONE_FLAG_R13_OFFSET = 0xffffc4a0u;  // -0x3b60

void BrawlbackGXDrawDoneSleepHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode() && IsResimulationPass())
  {
    // Force the done flag instead of sleeping, so the loop condition (re-checked where we jump
    // back to) is satisfied on its own rather than waiting on an interrupt for a draw we skipped.
    PowerPC::MMU::HostWrite<u8>(guard, 1, ppc_state.gpr[13] + BRAWL_GXDRAWDONE_FLAG_R13_OFFSET);
    ppc_state.npc = BRAWL_GXDRAWDONE_RECHECK_ADDR;
    return;
  }

  // Not rollback netplay: replicate the original `bl OSSleepThread` we replaced.
  LR(ppc_state) = BRAWL_GXDRAWDONE_RECHECK_ADDR;
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

// `subi r3, r13, 0x3bf8` inside VIWaitForRetrace's wait loop (vi.o @ 0x801e894c), the setup for
// `bl OSSleepThread`. VIWaitForRetrace spins here waiting for __VIRetraceCount (r13-0x3bd4) to
// increment, relying on __VIRetraceHandler - driven by Dolphin's real VI/present timing on its
// own thread, not our compressed per-iteration CPU loop - to eventually call OSWakeupThread. Only
// bypass during a resimulation pass: those iterations never present, so nothing would ever wake
// this up. The final iteration of every update DOES present, so let it use the real wait - that's
// also what paces the match to 60fps; bypassing it unconditionally left rollback netplay running
// as fast as the CPU could go. Advance the count ourselves and let the existing reload-and-compare
// take its own normal exit path, so OSSleepThread/the thread queue are never touched mid-resim.
//
// Do NOT try to replace the real wait with a guest-side busy-wait (re-enabling MSR[EE] and looping
// on the reload-and-compare): that re-dispatches this HLE hook every spin, which tanked the frame
// rate and desynced.
static constexpr u32 BRAWL_VIWAITFORRETRACE_SLEEP_CALL_ADDR = 0x801e894c;
static constexpr u32 BRAWL_VIWAITFORRETRACE_RECHECK_ADDR = 0x801e8954;
static constexpr u32 BRAWL_VIWAITFORRETRACE_COUNT_R13_OFFSET = 0xffffc42cu;  // -0x3bd4

void BrawlbackVIWaitForRetraceSleepHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  if (IsResimulationPass())
  {
    const u32 count_addr = ppc_state.gpr[13] + BRAWL_VIWAITFORRETRACE_COUNT_R13_OFFSET;
    const u32 count = PowerPC::MMU::HostRead<u32>(guard, count_addr) + 1;
    PowerPC::MMU::HostWrite<u32>(guard, count, count_addr);
    ppc_state.npc = BRAWL_VIWAITFORRETRACE_RECHECK_ADDR;
    return;
  }

  // subi	r3, r13, 15352
  ppc_state.gpr[3] = ppc_state.gpr[13] - 15352;
  ppc_state.npc = BRAWL_VIWAITFORRETRACE_SLEEP_CALL_ADDR + 4;
}

// `bl VIWaitForRetrace` inside gfFrameBuffer::sync's busy-wait loop (gf_framebuffer.o @
// 0x80023b1c). sync() scans a small ring of framebuffer slots for one whose busy flag
// (slot+0xc) is clear, and if none are free, calls VIWaitForRetrace and rescans in a do-while
// loop. That flag is only ever cleared by drawDoneCallback, a real GX draw-done completion
// callback - which resimulation iterations never trigger since they skip rendering entirely. The
// final iteration of every update does render, so let its wait behave normally there (it also
// provides the real-time pacing that keeps rollback netplay at 60fps). Clear every slot's busy
// flag ourselves only during a resim pass, so the rescan that follows finds one free immediately.
static constexpr u32 BRAWL_FRAMEBUFFER_SYNC_VIWAIT_CALL_ADDR = 0x80023b1c;
static constexpr u32 BRAWL_FRAMEBUFFER_SYNC_VIWAIT_RETURN_ADDR = 0x80023b20;
static constexpr u32 BRAWL_FRAMEBUFFER_SLOT_COUNT_OFFSET = 0x4;
static constexpr u32 BRAWL_FRAMEBUFFER_SLOT_STRIDE = 0x8;
static constexpr u32 BRAWL_FRAMEBUFFER_SLOT_BUSY_OFFSET = 0xc;

void BrawlbackFrameBufferSyncWaitHook(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  if (IsResimulationPass())
  {
    const u32 this_ptr = ppc_state.gpr[31];
    const u8 slot_count = PowerPC::MMU::HostRead<u8>(guard, this_ptr + BRAWL_FRAMEBUFFER_SLOT_COUNT_OFFSET);
    for (u8 i = 0; i < slot_count; ++i)
    {
      const u32 slot_addr = this_ptr + static_cast<u32>(i) * BRAWL_FRAMEBUFFER_SLOT_STRIDE;
      PowerPC::MMU::HostWrite<u32>(guard, 0, slot_addr + BRAWL_FRAMEBUFFER_SLOT_BUSY_OFFSET);
    }
    ppc_state.npc = BRAWL_FRAMEBUFFER_SYNC_VIWAIT_RETURN_ADDR;
    return;
  }

  // Not rollback netplay: replicate the original `bl VIWaitForRetrace` we replaced.
  LR(ppc_state) = BRAWL_FRAMEBUFFER_SYNC_VIWAIT_RETURN_ADDR;
  ppc_state.npc = 0x801e892c;
}

bool IsResimulationPass()
{
  if (!NetPlay::IsNetPlayRunning() || !NetPlay::IsInRollbackMode())
    return false;

  const int frames_to_advance = NetPlay::GetFramesToAdvance();
  const int current_iteration = NetPlay::GetCurrentIteration();
  return frames_to_advance > 1 && current_iteration != frames_to_advance - 1;
}

// `bl updateLowGC` inside updateLow (gf_pad.o @ 0x80029464), called both by gfPadReadThread's
// independently alarm-scheduled loop and (indirectly) by our own resimulation loop. updateLowGC
// calls the real PADRead() to refill gfPadSystem+0x40 - the exact same raw pad buffer
// NetPlayClient::InjectPads writes into - so letting it run races the two writers. updateLow
// itself must still run past this call: it also pushes gfPadSystem+0x40 onto gfPadStatusQueue,
// which updateGame can read from instead of the raw buffer directly, so skip only the real
// hardware re-poll and leave our injected values in place for that push to pick up.
static constexpr u32 BRAWL_PAD_UPDATELOWGC_CALL_ADDR = 0x80029464;
static constexpr u32 BRAWL_PAD_UPDATELOWGC_ADDR = 0x80029578;
static constexpr u32 BRAWL_PAD_UPDATELOWGC_RETURN_ADDR = 0x80029468;

void BrawlbackSkipPadThreadReadHook(const Core::CPUThreadGuard& guard)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();

  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode())
  {
    ppc_state.npc = BRAWL_PAD_UPDATELOWGC_RETURN_ADDR;
    return;
  }

  // Not rollback netplay: replicate the original `bl updateLowGC` we replaced.
  LR(ppc_state) = BRAWL_PAD_UPDATELOWGC_RETURN_ADDR;
  ppc_state.npc = BRAWL_PAD_UPDATELOWGC_ADDR;
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

  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode())
  {
    const int frames_to_advance = NetPlay::GetFramesToAdvance();
    const int current_iteration = NetPlay::GetCurrentIteration();
    // GekkoNet only hands us more than one frame to advance in a pass when we need to
    // resimulate to catch up; whichever iteration is last is always the real, newly-synced
    // frame, so it's the only one that should ever actually render.
    const bool is_resimulation_pass =
        frames_to_advance > 1 && current_iteration != frames_to_advance - 1;
    if (is_resimulation_pass)
    {
      // Not the newest frame this pass - skip straight past both render paths.
      ppc_state.npc = BRAWL_RENDER_DISPATCH_CONVERGE_ADDR;
      return;
    }
  }

  // Replicate the original `lbz r0,0xed(r23); rlwinm. r0,r0,0x1c,0x1f,0x1f; beq ...` we replaced.
  const u32 app_ptr = ppc_state.gpr[23];
  const u8 flags = PowerPC::MMU::HostRead<u8>(guard, app_ptr + BRAWL_APP_FLAGS_OFFSET);
  ppc_state.npc = (flags & BRAWL_APP_ALT_RENDER_FLAG) ? BRAWL_RENDER_ALT_BRANCH_ADDR :
                                                        BRAWL_RENDER_NORMAL_BRANCH_ADDR;
}

// The two `bl gfTask::process` call sites in gfTaskScheduler::process. Used to skip ecMgr,
// EffectManager and soEffectScreenManager's effect-only task callback on throwaway resimulation
// iterations, but effect tasks can reach soAnimCmdInterpreter::systemCmdFuncWaitRnadi (a "wait a
// random number of frames" anim command), which draws from the same global mt_prng stream as
// everything else - including Luigi's Green Missile misfire roll
// (ftLuigiStatusUniqProcessSpecialSRam::execFixPos). Skipping the call meant a peer that had to
// resimulate consumed fewer mt_prng draws than one that didn't, desyncing every later random
// roll between clients, so these hooks are no longer patched in HLE.cpp - kept only because
// os_patches references them by name. Both are now plain passthroughs to the original call.
static constexpr u32 BRAWL_TASK_PROCESS_ADDR = 0x8002dc74;
static constexpr u32 BRAWL_TASK_PROCESS_FIRST_CALL_ADDR = 0x8002e614;
static constexpr u32 BRAWL_TASK_PROCESS_SECOND_CALL_ADDR = 0x8002e63c;

void BrawlbackSkipResimTaskProcess(const Core::CPUThreadGuard& guard, u32 call_addr)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();
  LR(ppc_state) = call_addr + 4;
  ppc_state.npc = BRAWL_TASK_PROCESS_ADDR;
}

void BrawlbackSkipResimTaskProcessFirstHook(const Core::CPUThreadGuard& guard)
{
  BrawlbackSkipResimTaskProcess(guard, BRAWL_TASK_PROCESS_FIRST_CALL_ADDR);
}

void BrawlbackSkipResimTaskProcessSecondHook(const Core::CPUThreadGuard& guard)
{
  BrawlbackSkipResimTaskProcess(guard, BRAWL_TASK_PROCESS_SECOND_CALL_ADDR);
}

// The three `bl detail_AllocXXXSound` call sites inside
// SoundArchivePlayer::detail_SetupSound (snd_SoundArchivePlayer.o). Each hands out a wave/seq/strm
// sound object from a small fixed-size pool that is only restored by the next rollback's
// RollbackManager::LoadFrame, not per resimulation iteration. Letting throwaway resim iterations
// actually consume pool slots exhausts/corrupts the pool within a few frames, after which
// PrepareWaveSoundImpl (and its seq/strm equivalents) runs a virtual call through a stale or
// never-constructed object's vtable and crashes (e.g. the bctrl at 0x801ca1f4). Skip the alloc
// call during resim passes and fake the same "no channel available" (null) return the pool
// already produces when genuinely exhausted - every caller already handles that safely.
static constexpr u32 BRAWL_WAVESOUND_ALLOC_CALL_ADDR = 0x801c9bfc;
static constexpr u32 BRAWL_ALLOC_WAVE_SOUND_ADDR = 0x801cb8ac;
static constexpr u32 BRAWL_SEQSOUND_ALLOC_CALL_ADDR = 0x801c9aac;
static constexpr u32 BRAWL_ALLOC_SEQ_SOUND_ADDR = 0x801cb13c;
static constexpr u32 BRAWL_STRMSOUND_ALLOC_CALL_ADDR = 0x801c9b54;
static constexpr u32 BRAWL_ALLOC_STRM_SOUND_ADDR = 0x801cb4f4;

void BrawlbackSkipResimSoundAlloc(const Core::CPUThreadGuard& guard, u32 call_addr,
                                  u32 target_addr)
{
  auto& ppc_state = guard.GetSystem().GetPPCState();

  if (IsResimulationPass())
  {
    ppc_state.gpr[3] = 0;
    ppc_state.npc = call_addr + 4;
    return;
  }

  // Not a resim pass: replicate the original `bl` we replaced.
  LR(ppc_state) = call_addr + 4;
  ppc_state.npc = target_addr;
}

void BrawlbackSkipResimWaveSoundAllocHook(const Core::CPUThreadGuard& guard)
{
  BrawlbackSkipResimSoundAlloc(guard, BRAWL_WAVESOUND_ALLOC_CALL_ADDR, BRAWL_ALLOC_WAVE_SOUND_ADDR);
}

void BrawlbackSkipResimSeqSoundAllocHook(const Core::CPUThreadGuard& guard)
{
  BrawlbackSkipResimSoundAlloc(guard, BRAWL_SEQSOUND_ALLOC_CALL_ADDR, BRAWL_ALLOC_SEQ_SOUND_ADDR);
}

void BrawlbackSkipResimStrmSoundAllocHook(const Core::CPUThreadGuard& guard)
{
  BrawlbackSkipResimSoundAlloc(guard, BRAWL_STRMSOUND_ALLOC_CALL_ADDR, BRAWL_ALLOC_STRM_SOUND_ADDR);
}

// Synchronize global PRNG seeds (g_mtRand and g_randSeed) across netplay peers.
// The original game seeds PRNGs at match start (sqMelee::start) and screen transitions using OSGetTick(),
// which returns local hardware CPU tick counts that differ between clients.
// By overriding r3 on calls to srand (0x803f8c5c) and srandi (0x8003fb4c) with the session RTC seed,
// both clients enter matches and random selections with identical PRNG state.
void BrawlbackSyncCharSelectRandomSeedHook(const Core::CPUThreadGuard& guard)
{
  if (!NetPlay::IsNetPlayRunning() || !NetPlay::IsInRollbackMode())
    return;

  const u64 session_rtc = NetPlay::GetInitialRTCValue();
  auto& ppc_state = guard.GetSystem().GetPPCState();
  // r3 contains the seed value passed to srand/srandi; override it with our deterministic session RTC seed.
  ppc_state.gpr[3] = static_cast<u32>(session_rtc);
}
}  // namespace HLE_Misc
