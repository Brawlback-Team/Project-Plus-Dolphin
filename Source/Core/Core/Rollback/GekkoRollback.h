// Copyright 2026 Project+ Rollback Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "InputCommon/GCPadStatus.h"

namespace Core
{
class CPUThreadGuard;
class System;
}  // namespace Core

namespace Rollback
{
enum class FrameBoundary : u8
{
  BrawlHook = 0,
  VIBeginField = 1,
  VIEndField = 2,
  VINewField = 3,
};

// Wire format for GameCube controller input over GekkoNet (8 bytes)
// [Buttons Hi, Buttons Lo, MainStick X, MainStick Y, CStick X, CStick Y, Trigger L, Trigger R]
struct WirePad
{
  u8 data[8];
};

WirePad EncodePad(const GCPadStatus& status);
GCPadStatus DecodePad(const WirePad& pad);

// Rollback input query from SI controller polling
std::optional<GCPadStatus> GetRollbackPad(int port);

// Boundary callback called by the Brawl HLE hook at 0x80017504.
void OnFrameBoundary(const Core::CPUThreadGuard& guard);

// VI callbacks latch a candidate boundary. SystemTimers runs it after the next VI event has been
// scheduled, so snapshots never capture a half-completed VICallback.
void SignalVIBoundary(FrameBoundary boundary);
void RunPendingVIBoundary(Core::System& system);

// Session management. Dolphin negotiates endpoints in the lobby; GekkoNet owns gameplay traffic.
bool StartGekkoSession(const std::string& game_name, u32 session_id, int players, int local_player,
                       const std::vector<std::string>& player_endpoints,
                       int local_delay, int prediction_window, bool debug_p2_cstick,
                       bool simulate_remote_p2, bool stress_test, bool compare_confirmed_ram,
                       FrameBoundary frame_boundary);
void StopGekkoSession();
bool IsGekkoSessionActive();
void SetGekkoLocalDelay(int local_delay);

}  // namespace Rollback
