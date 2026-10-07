// Copyright 2015 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Common/ENet.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <mutex>

#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"

namespace Common::ENet
{
namespace
{
constexpr std::array<u8, 4> ROLLBACK_MAGIC{'G', 'K', 'R', 'B'};
constexpr u8 ROLLBACK_VERSION = 1;
constexpr size_t ROLLBACK_HEADER_SIZE = 12;

std::mutex s_rollback_mutex;
ENetHost* s_rollback_host = nullptr;
bool s_rollback_host_is_server = false;
bool s_rollback_active = false;
u32 s_rollback_session_id = 0;
std::deque<RollbackDatagram> s_rollback_datagrams;
RollbackDatagramStats s_rollback_stats;

void WriteSessionId(u8* header, u32 session_id)
{
  header[8] = static_cast<u8>(session_id >> 24);
  header[9] = static_cast<u8>(session_id >> 16);
  header[10] = static_cast<u8>(session_id >> 8);
  header[11] = static_cast<u8>(session_id);
}

u32 ReadSessionId(const u8* header)
{
  return static_cast<u32>(header[8]) << 24 | static_cast<u32>(header[9]) << 16 |
         static_cast<u32>(header[10]) << 8 | static_cast<u32>(header[11]);
}
}  // namespace

void WakeupThread(ENetHost* host)
{
  // Send ourselves a spurious message.  This is hackier than it should be.
  // comex reported this as https://github.com/lsalzman/enet/issues/23, so
  // hopefully there will be a better way to do it in the future.
  ENetAddress address;
  if (host->address.port != 0)
    address.port = host->address.port;
  else
    enet_socket_get_address(host->socket, &address);
  address.host = 0x0100007f;  // localhost
  u8 byte = 0;
  ENetBuffer buf;
  buf.data = &byte;
  buf.dataLength = 1;
  enet_socket_send(host->socket, &address, &buf, 1);
}

int ENET_CALLBACK InterceptCallback(ENetHost* host, ENetEvent* event)
{
  if (InterceptRollbackDatagram(host, event))
    return 1;

  // wakeup packet received
  if (host->receivedDataLength == 1 && host->receivedData[0] == 0)
  {
    event->type = static_cast<ENetEventType>(SKIPPABLE_EVENT);
    return 1;
  }
  return 0;
}

void RegisterRollbackSocket(ENetHost* host, bool server_socket)
{
  if (!host)
    return;

  std::lock_guard lk(s_rollback_mutex);
  if (!s_rollback_host || server_socket || !s_rollback_host_is_server)
  {
    s_rollback_host = host;
    s_rollback_host_is_server = server_socket;
  }
}

void UnregisterRollbackSocket(ENetHost* host)
{
  std::lock_guard lk(s_rollback_mutex);
  if (s_rollback_host == host)
  {
    s_rollback_host = nullptr;
    s_rollback_host_is_server = false;
    s_rollback_active = false;
    s_rollback_datagrams.clear();
  }
}

bool StartRollbackDatagrams(u32 session_id)
{
  std::lock_guard lk(s_rollback_mutex);
  if (!s_rollback_host)
    return false;

  s_rollback_session_id = session_id;
  s_rollback_datagrams.clear();
  s_rollback_stats = {};
  s_rollback_active = true;
  ENetAddress local_address{};
  enet_socket_get_address(s_rollback_host->socket, &local_address);
  INFO_LOG_FMT(NETPLAY, "GekkoNet shared UDP transport active on local port {} (session {})",
               local_address.port, session_id);
  return true;
}

void StopRollbackDatagrams()
{
  std::lock_guard lk(s_rollback_mutex);
  s_rollback_active = false;
  s_rollback_datagrams.clear();
}

bool SendRollbackDatagram(const ENetAddress& address, const void* payload, size_t size)
{
  std::array<u8, ROLLBACK_HEADER_SIZE> header{};
  ENetSocket socket = ENET_SOCKET_NULL;
  {
    std::lock_guard lk(s_rollback_mutex);
    if (!s_rollback_active || !s_rollback_host)
      return false;
    socket = s_rollback_host->socket;
    std::copy(ROLLBACK_MAGIC.begin(), ROLLBACK_MAGIC.end(), header.begin());
    header[4] = ROLLBACK_VERSION;
    WriteSessionId(header.data(), s_rollback_session_id);
  }

  ENetBuffer buffers[2]{};
  buffers[0].data = header.data();
  buffers[0].dataLength = header.size();
  buffers[1].data = const_cast<void*>(payload);
  buffers[1].dataLength = size;
  const bool sent = enet_socket_send(socket, &address, buffers, 2) >= 0;
  if (sent)
  {
    std::lock_guard lk(s_rollback_mutex);
    ++s_rollback_stats.sent;
  }
  return sent;
}

std::vector<RollbackDatagram> DrainRollbackDatagrams()
{
  std::lock_guard lk(s_rollback_mutex);
  std::vector<RollbackDatagram> result;
  result.reserve(s_rollback_datagrams.size());
  while (!s_rollback_datagrams.empty())
  {
    result.emplace_back(std::move(s_rollback_datagrams.front()));
    s_rollback_datagrams.pop_front();
  }
  return result;
}

RollbackDatagramStats GetRollbackDatagramStats()
{
  std::lock_guard lk(s_rollback_mutex);
  return s_rollback_stats;
}

bool InterceptRollbackDatagram(ENetHost* host, ENetEvent* event)
{
  if (host->receivedDataLength < ROLLBACK_HEADER_SIZE ||
      std::memcmp(host->receivedData, ROLLBACK_MAGIC.data(), ROLLBACK_MAGIC.size()) != 0)
  {
    return false;
  }

  const auto* data = static_cast<const u8*>(host->receivedData);
  {
    std::lock_guard lk(s_rollback_mutex);
    if (host == s_rollback_host && s_rollback_active && data[4] == ROLLBACK_VERSION &&
        ReadSessionId(data) == s_rollback_session_id)
    {
      RollbackDatagram datagram;
      datagram.address = host->receivedAddress;
      datagram.payload.assign(data + ROLLBACK_HEADER_SIZE, data + host->receivedDataLength);
      s_rollback_datagrams.emplace_back(std::move(datagram));
      ++s_rollback_stats.received;
    }
    else
      ++s_rollback_stats.rejected;
  }

  // Always consume our envelope, including stale sessions, so ENet never parses it as ENet data.
  event->type = static_cast<ENetEventType>(SKIPPABLE_EVENT);
  return true;
}

bool SendPacket(ENetPeer* socket, const sf::Packet& packet, u8 channel_id)
{
  if (!socket)
  {
    ERROR_LOG_FMT(NETPLAY, "Target socket is null.");
    return false;
  }

  ENetPacket* epac =
      enet_packet_create(packet.getData(), packet.getDataSize(), ENET_PACKET_FLAG_RELIABLE);
  if (!epac)
  {
    ERROR_LOG_FMT(NETPLAY, "Failed to create ENetPacket ({} bytes).", packet.getDataSize());
    return false;
  }

  const int result = enet_peer_send(socket, channel_id, epac);
  if (result != 0)
  {
    ERROR_LOG_FMT(NETPLAY, "Failed to send ENetPacket (error code {}).", result);
    return false;
  }

  return true;
}
}  // namespace Common::ENet
