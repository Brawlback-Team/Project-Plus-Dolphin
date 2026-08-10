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
    ppc_state.gpr[25] = 0x1;
    ppc_state.npc = BRAWL_UNCONDITIONAL_HOOK_ADDR + 4;
    return;
  }

  int current_iteration = NetPlay::GetCurrentIteration();

  if (current_iteration == 0)
  {
    NetPlay::OnFrameStart();
  }

  int total_iterations = NetPlay::GetFramesToAdvance();
  static constexpr u32 BRAWL_LOOP_END_ADDR = 0x80017508;

  if (total_iterations == 0)
  {
    NetPlay::PauseForLocalAdvantage();

    ppc_state.gpr[25] = 0x1;
    ppc_state.npc = BRAWL_LOOP_END_ADDR;
    return;
  }

  if (current_iteration >= total_iterations)
  {
    ppc_state.gpr[25] = 0x1;
    ppc_state.npc = BRAWL_LOOP_END_ADDR;
    return;
  }

  NetPlay::InjectPadsForIteration(current_iteration);
  NetPlay::PauseForLocalAdvantage();

  // Execute the original instruction: li r25, 0x1
  ppc_state.gpr[25] = 0x1;

  // Continue to next instruction
  ppc_state.npc = BRAWL_UNCONDITIONAL_HOOK_ADDR + 4;
}

void BrawlbackGekkoNetGameLoop(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  ppc_state.gpr[25] = 0;
  ppc_state.npc = BRAWL_GAME_LOOP_HOOK_ADDR + 4;
}

void BrawlbackGekkoNetGameProcCallsite(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  ppc_state.gpr[3] = ppc_state.gpr[23];
  ppc_state.npc = BRAWL_GAMEPROC_CALLSITE_NEXT_ADDR;
}

void BrawlbackGekkoNetFrameEnd(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  auto& ppc_state = system.GetPPCState();

  static constexpr u32 BRAWL_FRAME_END_HOOK_ADDR = 0x80017504;

  if (!NetPlay::IsNetPlayRunning() || !NetPlay::IsInRollbackMode())
  {
    const u32 addr = ppc_state.gpr[23] + 0x100;
    PowerPC::MMU::HostWrite<u32>(guard, ppc_state.gpr[0], addr);
    ppc_state.npc = BRAWL_FRAME_END_HOOK_ADDR + 4;
    return;
  }

  int current_iteration = NetPlay::GetCurrentIteration();

  if (NetPlay::ShouldSaveAfterIteration(current_iteration))
  {
    Rollback::RollbackManager::Get().SaveFrame(Core::System::GetInstance());
  }

  const u32 addr = ppc_state.gpr[23] + 0x100;
  PowerPC::MMU::HostWrite<u32>(guard, ppc_state.gpr[0], addr);
  ppc_state.npc = BRAWL_FRAME_END_HOOK_ADDR + 4;
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

  int current_iteration = NetPlay::GetCurrentIteration();
  int total_iterations = NetPlay::GetFramesToAdvance();

  if (total_iterations == 0)
  {
    ppc_state.npc = BRAWL_LOOP_START_ADDR;
    return;
  }

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
}  // namespace HLE_Misc
