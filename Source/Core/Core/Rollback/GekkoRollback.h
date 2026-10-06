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

// Boundary callback called at Brawl's frame boundary (0x80017504)
void OnFrameBoundary(const Core::CPUThreadGuard& guard);

// Session management. Dolphin negotiates endpoints in the lobby; GekkoNet owns gameplay traffic.
bool StartGekkoSession(const std::string& game_name, u32 session_id, int players, int local_player,
                       const std::vector<std::string>& player_endpoints,
                       int local_delay, int prediction_window, bool debug_p2_cstick,
                       bool simulate_remote_p2);
void StopGekkoSession();
bool IsGekkoSessionActive();

}  // namespace Rollback
