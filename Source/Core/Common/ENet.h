// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
#pragma once

#include <cstddef>
#include <memory>
#include <vector>

#include <SFML/Network/Packet.hpp>
#include <enet/enet.h>

#include "Common/CommonTypes.h"

namespace Common::ENet
{
struct ENetHostDeleter
{
  void operator()(ENetHost* host) const noexcept { enet_host_destroy(host); }
};
using ENetHostPtr = std::unique_ptr<ENetHost, ENetHostDeleter>;

struct RollbackDatagram
{
  ENetAddress address{};
  std::vector<u8> payload;
};

struct RollbackDatagramStats
{
  u64 sent = 0;
  u64 received = 0;
  u64 rejected = 0;
};

void WakeupThread(ENetHost* host);
int ENET_CALLBACK InterceptCallback(ENetHost* host, ENetEvent* event);
bool SendPacket(ENetPeer* socket, const sf::Packet& packet, u8 channel_id);

// GekkoNet shares NetPlay's UDP socket so it also uses the NAT mapping opened by traversal.
// A server socket takes precedence over the host's loopback NetPlayClient socket.
void RegisterRollbackSocket(ENetHost* host, bool server_socket);
void UnregisterRollbackSocket(ENetHost* host);
bool StartRollbackDatagrams(u32 session_id);
void StopRollbackDatagrams();
bool SendRollbackDatagram(const ENetAddress& address, const void* payload, size_t size);
std::vector<RollbackDatagram> DrainRollbackDatagrams();
RollbackDatagramStats GetRollbackDatagramStats();
bool InterceptRollbackDatagram(ENetHost* host, ENetEvent* event);

// used for traversal packets and wake-up packets
constexpr int SKIPPABLE_EVENT = 42;
}  // namespace Common::ENet
