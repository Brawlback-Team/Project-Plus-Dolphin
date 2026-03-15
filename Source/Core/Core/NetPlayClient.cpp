// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/NetPlayClient.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <tuple>
#include <type_traits>
#include <vector>
#include <latch>
#include <optional>
#include <ranges>
#include <chrono>
#include <map>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "Common/Assert.h"
#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/Crypto/SHA1.h"
#include "Common/ENet.h"
#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/MsgHandler.h"
#include "Common/NandPaths.h"
#include "Common/QoSSession.h"
#include "Common/SFMLHelper.h"
#include "Common/StringUtil.h"
#include "Common/Timer.h"
#include "Common/Version.h"

#include "Core/ActionReplay.h"
#include "Core/Boot/Boot.h"
#include "Core/Core.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Config/SessionSettings.h"
#include "Core/Config/WiimoteSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_DeviceIPL.h"
#ifdef HAS_LIBMGBA
#include "Core/HW/GBACore.h"
#endif
#include "Core/HW/GBAPad.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SI/SI_DeviceGCController.h"
#include "Core/HW/Sram.h"
#include "Core/HW/WiiSave.h"
#include "Core/HW/WiiSaveStructs.h"
#include "Core/HW/WiimoteEmu/DesiredWiimoteState.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "Core/HW/WiimoteReal/WiimoteReal.h"
#include "Core/IOS/FS/FileSystem.h"
#include "Core/IOS/FS/HostBackend/FS.h"
#include "Core/IOS/USB/Bluetooth/BTEmu.h"
#include "Core/IOS/Uids.h"
#include "Core/Movie.h"
#include "Core/NetPlayCommon.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/SyncIdentifier.h"
#include "Core/System.h"
#include "Core/PowerPC/CPUCoreBase.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/State.h"
#include "DiscIO/Blob.h"

#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/GCAdapter.h"
#include "InputCommon/InputConfig.h"
#include "UICommon/GameFile.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoConfig.h"
#include "Common/HookableEvent.h"
#include "VideoCommon/VideoEvents.h"
#include <incremental-rollback/incremental_rb.h>
#include "Brawlback/TimeSync.h"
#include "brawlback-common/BrawlbackPad.h"

namespace NetPlay
{
using namespace WiimoteCommon;

static std::mutex crit_netplay_client;
NetPlayClient* netplay_client = nullptr;
static bool s_si_poll_batching = false;
static std::atomic<bool> is_rollingback;
static std::atomic<bool> is_stalled;
static std::atomic<bool> game_started;
static std::atomic<bool> is_predicting;
static json inputs_output = json{};

//static Common::EventHook s_after_frame_event = AfterFrameEvent::Register(
//    [](const Core::System& system) { OnFrameEnd(); }, "Netplay::OnFrameEnd");

/*
static Common::EventHook s_before_frame_event = BeforeFrameEvent::Register(
    [] { OnFrameStart(); }, "Netplay::OnFrameStart");
    */
// called from ---GUI--- thread
NetPlayClient::~NetPlayClient()
{
  // not perfect
  if (m_is_running.IsSet())
    StopGame();

  if (m_is_connected)
  {
    m_should_compute_game_digest = false;
    m_dialog->AbortGameDigest();
    if (m_game_digest_thread.joinable())
      m_game_digest_thread.join();
    m_do_loop.Clear();
    m_thread.join();

    m_chunked_data_receive_queue.clear();
    m_dialog->HideChunkedProgressDialog();
  }

  if (m_server)
  {
    Disconnect();
  }

  if (Common::g_MainNetHost.get() == m_client)
  {
    Common::g_MainNetHost.release();
  }
  if (m_client)
  {
    enet_host_destroy(m_client);
    m_client = nullptr;
  }

  if (m_traversal_client)
  {
    Common::ReleaseTraversalClient();
  }
}

// called from ---GUI--- thread
NetPlayClient::NetPlayClient(const std::string& address, const u16 port, NetPlayUI* dialog,
                             const std::string& name, const NetTraversalConfig& traversal_config)
    : m_dialog(dialog), m_player_name(name),
      m_last_desync_notification(std::chrono::steady_clock::now())
{
  ClearBuffers();

  if (!traversal_config.use_traversal)
  {
    // Direct Connection
    m_client = enet_host_create(nullptr, 1, CHANNEL_COUNT, 0, 0);

    if (m_client == nullptr)
    {
      m_dialog->OnConnectionError(_trans("Could not create client."));
      return;
    }

    m_client->mtu = std::min(m_client->mtu, NetPlay::MAX_ENET_MTU);

    ENetAddress addr;
    enet_address_set_host(&addr, address.c_str());
    addr.port = port;

    m_server = enet_host_connect(m_client, &addr, CHANNEL_COUNT, 0);

    if (m_server == nullptr)
    {
      m_dialog->OnConnectionError(_trans("Could not create peer."));
      return;
    }

    // Update time in milliseconds of no acknowledgment of
    // sent packets before a connection is deemed disconnected
    enet_peer_timeout(m_server, 0, PEER_TIMEOUT.count(), PEER_TIMEOUT.count());

    ENetEvent netEvent;
    int net = enet_host_service(m_client, &netEvent, 5000);
    if (net > 0 && netEvent.type == ENET_EVENT_TYPE_CONNECT)
    {
      if (Connect())
      {
        m_client->intercept = Common::ENet::InterceptCallback;
        m_thread = std::thread(&NetPlayClient::ThreadFunc, this);
      }
    }
    else
    {
      m_dialog->OnConnectionError(_trans("Could not communicate with host."));
    }
  }
  else
  {
    if (address.size() > Common::NETPLAY_CODE_SIZE)
    {
      m_dialog->OnConnectionError(
          _trans("The host code is too long.\nPlease recheck that you have the correct code."));
      return;
    }

    if (!Common::EnsureTraversalClient(traversal_config.traversal_host,
                                       traversal_config.traversal_port))
    {
      return;
    }
    m_client = Common::g_MainNetHost.get();

    m_traversal_client = Common::g_TraversalClient.get();

    // If we were disconnected in the background, reconnect.
    if (m_traversal_client->HasFailed())
      m_traversal_client->ReconnectToServer();
    m_traversal_client->m_Client = this;
    m_host_spec = address;
    m_connection_state = ConnectionState::WaitingForTraversalClientConnection;
    OnTraversalStateChanged();
    m_connecting = true;

    Common::Timer connect_timer;
    connect_timer.Start();

    while (m_connecting)
    {
      ENetEvent netEvent;
      if (m_traversal_client)
        m_traversal_client->HandleResends();

      while (enet_host_service(m_client, &netEvent, 4) > 0)
      {
        sf::Packet rpac;
        switch (netEvent.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
          m_server = netEvent.peer;

          // Update time in milliseconds of no acknowledgment of
          // sent packets before a connection is deemed disconnected
          enet_peer_timeout(m_server, 0, PEER_TIMEOUT.count(), PEER_TIMEOUT.count());

          if (Connect())
          {
            m_connection_state = ConnectionState::Connected;
            m_thread = std::thread(&NetPlayClient::ThreadFunc, this);
          }
          return;
        default:
          break;
        }
      }
      if (connect_timer.ElapsedMs() > 5000)
        break;
    }
    m_dialog->OnConnectionError(_trans("Could not communicate with host."));
  }
}

bool NetPlayClient::Connect()
{
  INFO_LOG_FMT(NETPLAY, "Connecting to server.");

  // send connect message
  sf::Packet packet;
  packet << Common::GetScmRevGitStr();
  packet << Common::GetNetplayDolphinVer();
  packet << m_player_name;
  Send(packet);
  enet_host_flush(m_client);
  sf::Packet rpac;
  // TODO: make this not hang
  ENetEvent netEvent;
  int net;
  while ((net = enet_host_service(m_client, &netEvent, 5000)) > 0 &&
         static_cast<int>(netEvent.type) == Common::ENet::SKIPPABLE_EVENT)
  {
    // ignore packets from traversal server
  }
  if (net > 0 && netEvent.type == ENET_EVENT_TYPE_RECEIVE)
  {
    rpac.append(netEvent.packet->data, netEvent.packet->dataLength);
    enet_packet_destroy(netEvent.packet);
  }
  else
  {
    return false;
  }

  ConnectionError error;
  rpac >> error;

  // got error message
  if (error != ConnectionError::NoError)
  {
    switch (error)
    {
    case ConnectionError::ServerFull:
      m_dialog->OnConnectionError(_trans("The server is full."));
      break;
    case ConnectionError::VersionMismatch:
      m_dialog->OnConnectionError(
          _trans("The server and client's NetPlay versions are incompatible."));
      break;
    case ConnectionError::GameRunning:
      m_dialog->OnConnectionError(_trans("The game is currently running."));
      break;
    case ConnectionError::NameTooLong:
      m_dialog->OnConnectionError(_trans("Nickname is too long."));
      break;
    default:
      m_dialog->OnConnectionError(_trans("The server sent an unknown error message."));
      break;
    }

    Disconnect();
    return false;
  }
  else
  {
    rpac >> m_pid;

    Player player;
    player.name = m_player_name;
    player.pid = m_pid;
    player.revision = Common::GetNetplayDolphinVer();
    player.buffer = 0 /* will be raised once we get the packet */;

    // add self to player list
    m_players[m_pid] = player;
    m_local_player = &m_players[m_pid];

    m_dialog->Update();

    m_is_connected = true;

    return true;
  }
}

// called from ---GUI--- and ---NETPLAY--- thread
void NetPlayClient::AdjustPlayerPadBufferSize(u32 buffer)
{
  std::lock_guard<std::recursive_mutex> lkp(m_crit.players);

  m_local_player->buffer = buffer;
  if (m_local_player->buffer < m_minimum_buffer_size)
    m_local_player->buffer = m_minimum_buffer_size;


	 // not needed on clients with host input authority
  if (!m_host_input_authority)
  {
    // tell clients to change buffer size
    sf::Packet spac;
    spac << MessageID::PadBufferPlayer;
    spac << m_local_player->buffer;

    SendAsync(std::move(spac));
  }
  
  m_dialog->OnPlayerPadBufferChanged(m_local_player->buffer);
}

void NetPlayClient::AdjustMinimumPadBufferSize(const unsigned int size)
{
  m_minimum_buffer_size = size;
  m_dialog->OnMinimumPadBufferChanged(size);
}

static void ReceiveSyncIdentifier(sf::Packet& spac, SyncIdentifier& sync_identifier)
{
  // We use a temporary variable here due to a potential long vs long long mismatch
  u64 dol_elf_size;
  spac >> dol_elf_size;
  sync_identifier.dol_elf_size = dol_elf_size;

  spac >> sync_identifier.game_id;
  spac >> sync_identifier.revision;
  spac >> sync_identifier.disc_number;
  spac >> sync_identifier.is_datel;

  for (u8& x : sync_identifier.sync_hash)
    spac >> x;
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnData(sf::Packet& packet)
{
  MessageID mid;
  packet >> mid;

  INFO_LOG_FMT(NETPLAY, "Got server message: {:x}", static_cast<u8>(mid));

  switch (mid)
  {
  case MessageID::PlayerJoin:
    OnPlayerJoin(packet);
    break;

  case MessageID::PlayerLeave:
    OnPlayerLeave(packet);
    break;

  case MessageID::ChatMessage:
    OnChatMessage(packet);
    break;

  case MessageID::ChunkedDataStart:
    OnChunkedDataStart(packet);
    break;

  case MessageID::ChunkedDataEnd:
    OnChunkedDataEnd(packet);
    break;

  case MessageID::ChunkedDataPayload:
    OnChunkedDataPayload(packet);
    break;

  case MessageID::ChunkedDataAbort:
    OnChunkedDataAbort(packet);
    break;

  case MessageID::PadMapping:
    OnPadMapping(packet);
    break;

  case MessageID::GBAConfig:
    OnGBAConfig(packet);
    break;

  case MessageID::WiimoteMapping:
    OnWiimoteMapping(packet);
    break;

  case MessageID::PadData:
    OnPadData(packet);
    break;

  case MessageID::PadHostData:
    OnPadHostData(packet);
    break;

  case MessageID::WiimoteData:
    OnWiimoteData(packet);
    break;

  case MessageID::PadBufferMinimum:
    OnPadBufferMinimum(packet);
    break;
    
  case MessageID::PadBufferPlayer:
    OnPadBufferPlayer(packet);
    break;

  case MessageID::HostInputAuthority:
    OnHostInputAuthority(packet);
    break;
  case MessageID::RollbackMode:
    OnRollbackMode(packet);
    break;  
  case MessageID::GolfSwitch:
    OnGolfSwitch(packet);
    break;

  case MessageID::GolfPrepare:
    OnGolfPrepare(packet);
    break;

  case MessageID::ChangeGame:
    OnChangeGame(packet);
    break;

  case MessageID::GameStatus:
    OnGameStatus(packet);
    break;

  case MessageID::StartGame:
    OnStartGame(packet);
    break;

  case MessageID::StopGame:
  case MessageID::DisableGame:
    OnStopGame(packet);
    break;

  case MessageID::PowerButton:
    OnPowerButton();
    break;

  case MessageID::Ping:
    OnPing(packet);
    break;

  case MessageID::PlayerPingData:
    OnPlayerPingData(packet);
    break;

  case MessageID::DesyncDetected:
    OnDesyncDetected(packet);
    break;

  case MessageID::SyncSaveData:
    OnSyncSaveData(packet);
    break;

  case MessageID::SyncCodes:
    OnSyncCodes(packet);
    break;

  case MessageID::ComputeGameDigest:
    OnComputeGameDigest(packet);
    break;

  case MessageID::GameDigestProgress:
    OnGameDigestProgress(packet);
    break;

  case MessageID::GameDigestResult:
    OnGameDigestResult(packet);
    break;

  case MessageID::GameDigestError:
    OnGameDigestError(packet);
    break;

  case MessageID::GameDigestAbort:
    OnGameDigestAbort();
    break;
  case MessageID::AckInputs:
    OnFrameAck(packet);
    break;
  default:
    PanicAlertFmtT("Unknown message received with id : {0}", static_cast<u8>(mid));
    break;
  }
}

void NetPlayClient::OnPlayerJoin(sf::Packet& packet)
{
  Player player{};
  packet >> player.pid;
  packet >> player.name;
  packet >> player.revision;

  INFO_LOG_FMT(NETPLAY, "Player {} ({}) using {} joined", player.name, player.pid, player.revision);

  {
    std::lock_guard lkp(m_crit.players);
    m_players[player.pid] = player;
  }

  m_dialog->OnPlayerConnect(player.name);

  m_dialog->Update();
}

void NetPlayClient::OnPlayerLeave(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard lkp(m_crit.players);
    const auto it = m_players.find(pid);
    if (it == m_players.end())
      return;

    const auto& player = it->second;
    INFO_LOG_FMT(NETPLAY, "Player {} ({}) left", player.name, pid);
    m_dialog->OnPlayerDisconnect(player.name);
    m_players.erase(it);
  }

  m_dialog->Update();
}

void NetPlayClient::OnChatMessage(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;
  std::string msg;
  packet >> msg;

  // don't need lock to read in this thread
  const Player& player = m_players[pid];

  INFO_LOG_FMT(NETPLAY, "Player {} ({}) wrote: {}", player.name, player.pid, msg);

  // add to gui
  m_dialog->AppendChat(fmt::format("{}[{}]: {}", player.name, pid, msg));
}

void NetPlayClient::OnChunkedDataStart(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;
  std::string title;
  packet >> title;
  const u64 data_size = Common::PacketReadU64(packet);

  INFO_LOG_FMT(NETPLAY, "Starting data chunk {}.", cid);

  m_chunked_data_receive_queue.emplace(cid, sf::Packet{});

  std::vector<int> players;
  players.push_back(m_local_player->pid);
  m_dialog->ShowChunkedProgressDialog(title, data_size, players);
}

void NetPlayClient::OnChunkedDataEnd(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;

  const auto data_packet_iter = m_chunked_data_receive_queue.find(cid);
  if (data_packet_iter == m_chunked_data_receive_queue.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", cid);
    return;
  }

  INFO_LOG_FMT(NETPLAY, "Ending data chunk {}.", cid);

  auto& data_packet = data_packet_iter->second;
  OnData(data_packet);
  m_chunked_data_receive_queue.erase(data_packet_iter);
  m_dialog->HideChunkedProgressDialog();

  sf::Packet complete_packet;
  complete_packet << MessageID::ChunkedDataComplete;
  complete_packet << cid;
  Send(complete_packet, CHUNKED_DATA_CHANNEL);
}

void NetPlayClient::OnChunkedDataPayload(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;

  const auto data_packet_iter = m_chunked_data_receive_queue.find(cid);
  if (data_packet_iter == m_chunked_data_receive_queue.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", cid);
    return;
  }

  auto& data_packet = data_packet_iter->second;
  while (!packet.endOfPacket())
  {
    u8 byte;
    packet >> byte;
    data_packet << byte;
  }

  INFO_LOG_FMT(NETPLAY, "Received {} bytes of data chunk {}.", data_packet.getDataSize(), cid);

  m_dialog->SetChunkedProgress(m_local_player->pid, data_packet.getDataSize());

  sf::Packet progress_packet;
  progress_packet << MessageID::ChunkedDataProgress;
  progress_packet << cid;
  progress_packet << u64{data_packet.getDataSize()};
  Send(progress_packet, CHUNKED_DATA_CHANNEL);
}

void NetPlayClient::OnChunkedDataAbort(sf::Packet& packet)
{
  u32 cid;
  packet >> cid;

  const auto iter = m_chunked_data_receive_queue.find(cid);
  if (iter == m_chunked_data_receive_queue.end())
  {
    INFO_LOG_FMT(NETPLAY, "Invalid data chunk ID {}.", cid);
    return;
  }

  INFO_LOG_FMT(NETPLAY, "Aborting data chunk {}.", cid);

  m_chunked_data_receive_queue.erase(iter);
  m_dialog->HideChunkedProgressDialog();
}

void NetPlayClient::OnPadMapping(sf::Packet& packet)
{
  for (PlayerId& mapping : m_pad_map)
    packet >> mapping;

  UpdateDevices();

  m_dialog->Update();
}

void NetPlayClient::OnWiimoteMapping(sf::Packet& packet)
{
  for (PlayerId& mapping : m_wiimote_map)
    packet >> mapping;

  m_dialog->Update();
}

void NetPlayClient::OnGBAConfig(sf::Packet& packet)
{
  for (size_t i = 0; i < m_gba_config.size(); ++i)
  {
    auto& config = m_gba_config[i];
    const auto old_config = config;

    packet >> config.enabled >> config.has_rom >> config.title;
    for (auto& data : config.hash)
      packet >> data;

    if (std::tie(config.has_rom, config.title, config.hash) !=
        std::tie(old_config.has_rom, old_config.title, old_config.hash))
    {
      m_dialog->OnMsgChangeGBARom(static_cast<int>(i), config);
      m_net_settings.gba_rom_paths[i] =
          config.has_rom ?
              m_dialog->FindGBARomPath(config.hash, config.title, static_cast<int>(i)) :
              "";
    }
  }

  SendGameStatus();
  UpdateDevices();

  m_dialog->Update();
}

void NetPlayClient::OnPadData(sf::Packet& packet)
{
  while (!packet.endOfPacket())
  {
    PadIndex map;
    packet >> map;

    if (m_net_settings.m_RollbackMode)
    {
      u8 sizeofFramedatas;
      u32 maxFrame;
      PlayerId playerIndex;
      int config;
      packet >> config;
      packet >> sizeofFramedatas;
      packet >> playerIndex;
      packet >> maxFrame;

      pad_config.at(playerIndex) = config;

      if (sizeofFramedatas > 0)
      {
        sf::Packet ackDataPacket = sf::Packet();
        auto cmdbyte = MessageID::AckInputs;
        ackDataPacket << cmdbyte;
        ackDataPacket << maxFrame;
        ackDataPacket << playerIndex;
        Send(ackDataPacket, DEFAULT_CHANNEL, ENET_PACKET_FLAG_UNSEQUENCED);
      }

      std::vector<Inputs> pads(sizeofFramedatas);
      for (u8 i = 0; i < sizeofFramedatas; i++)
      {
        Inputs pad_inputs;
        packet >> pad_inputs.game_pad.buttons >> pad_inputs.game_pad._buttons >>
            pad_inputs.game_pad.holdButtons >> pad_inputs.game_pad.rapidFireButtons >>
            pad_inputs.game_pad.newPressedButtons >> pad_inputs.game_pad.releasedButtons >>
            pad_inputs.game_pad.stickX >> pad_inputs.game_pad.stickY >>
            pad_inputs.game_pad.cStickX >> pad_inputs.game_pad.cStickY >>
            pad_inputs.game_pad.LAnalogue >> pad_inputs.game_pad.RAnalogue >>
            pad_inputs.game_pad.LTrigger >> pad_inputs.game_pad.RTrigger;

        packet >> pad_inputs.emu_pad.button >> pad_inputs.emu_pad.stickX >>
            pad_inputs.emu_pad.stickY >> pad_inputs.emu_pad.substickX >>
            pad_inputs.emu_pad.substickY >> pad_inputs.emu_pad.triggerLeft >>
            pad_inputs.emu_pad.triggerRight >> pad_inputs.emu_pad.analogA >>
            pad_inputs.emu_pad.analogB >> pad_inputs.emu_pad.isConnected;

        packet >> pad_inputs.frame;

        pads[i] = pad_inputs;
      }
      if (sizeofFramedatas > 0)
      {
        std::lock_guard<std::mutex> lock(crit_netplay_client);
        for (auto pad : pads)
        {
          if (!inputs.at(playerIndex).Empty())
          {
            // if the remote frame we're trying to process is not newer than the most recent frame,
            // we don't care about it
            if (pad.frame <= inputs.at(playerIndex).GetBack()->frame)
            {
              ERROR_LOG_FMT(BRAWLBACK, "Remote input is already accounted for! {} <= {}\n",
                            pad.frame, inputs.at(playerIndex).GetBack()->frame);
              continue;
            }
            // make sure the inputs we're adding are sequential
            if (pad.frame != inputs.at(playerIndex).GetBack()->frame + 1)
            {
              ERROR_LOG_FMT(BRAWLBACK, "Remote input is not sequential! {} != {}\n", pad.frame,
                            inputs.at(playerIndex).GetBack()->frame + 1);
              continue;
            }
          }
          // clamp size of remote player framedata queue
          inputs.at(playerIndex).Push(pad);

          auto current_inputs = json{};
          ToJson(current_inputs, pad);
          std::string port = fmt::format("{}_{}", "player_port", playerIndex);

          inputs_output[port].push_back(current_inputs);
        }
        int local_player_port = -1;
        for (int i = 0; i < m_pad_map.size(); i++)
        {
          if (m_pad_map.at(i) == m_local_player->pid)
            local_player_port = i;
        }
        time_sync->ReceivedRemoteFramedata(maxFrame, static_cast<u8>(local_player_port),
                                           start_inputs);
      }
    }
    else
    {
      GCPadStatus pad;
      packet >> pad.button;
      if (!m_gba_config.at(map).enabled)
      {
        packet >> pad.analogA >> pad.analogB >> pad.stickX >> pad.stickY >> pad.substickX >>
            pad.substickY >> pad.triggerLeft >> pad.triggerRight >> pad.isConnected;
      }
      // Trusting server for good map value (>=0 && <4)
      // add to pad buffer
      m_pad_buffer.at(map).Push(pad);
      m_gc_pad_event.Set();
    }
  }
}


void NetPlayClient::OnPadHostData(sf::Packet& packet)
{
  while (!packet.endOfPacket())
  {
    PadIndex map;
    packet >> map;

    GCPadStatus pad;
    packet >> pad.button;
    if (!m_gba_config.at(map).enabled)
    {
      packet >> pad.analogA >> pad.analogB >> pad.stickX >> pad.stickY >> pad.substickX >>
          pad.substickY >> pad.triggerLeft >> pad.triggerRight >> pad.isConnected;
    }

    // Trusting server for good map value (>=0 && <4)
    // write to last status
    m_last_pad_status[map] = pad;

    if (!m_first_pad_status_received[map])
    {
      m_first_pad_status_received[map] = true;
      m_first_pad_status_received_event.Set();
    }
  }
}

void NetPlayClient::SendRollbackVerification(s32 frame, u64 hash)
{
  std::lock_guard lk(crit_netplay_client);

  if (!IsInRollbackMode() || !IsStarted())
    return;

  sf::Packet packet;
  packet << MessageID::RollbackVerification;
  packet << frame;
  packet << hash;

  netplay_client->SendAsync(std::move(packet));
}

void NetPlayClient::OnWiimoteData(sf::Packet& packet)
{
  while (!packet.endOfPacket())
  {
    PadIndex map;
    packet >> map;

    WiimoteEmu::SerializedWiimoteState pad;
    packet >> pad.length;
    ASSERT(pad.length <= pad.data.size());
    if (pad.length <= pad.data.size())
    {
      for (size_t i = 0; i < pad.length; ++i)
        packet >> pad.data[i];
    }
    else
    {
      pad.length = 0;
    }

    // Trusting server for good map value (>=0 && <4)
    // add to pad buffer
    m_wiimote_buffer.at(map).Push(pad);
    m_wii_pad_event.Set();
  }
}

void NetPlayClient::OnPadBufferMinimum(sf::Packet& packet)
{
  u32 size = 0;
  packet >> size;
  
  m_minimum_buffer_size = size;
    m_dialog->OnMinimumPadBufferChanged(size);

    if (m_local_player->buffer < m_minimum_buffer_size)
      AdjustPlayerPadBufferSize(m_minimum_buffer_size);
}


void NetPlayClient::OnPadBufferPlayer(sf::Packet& packet)
{
    PlayerId pid;
    packet >> pid;

    {
      std::lock_guard<std::recursive_mutex> lkp(m_crit.players);
      packet >> m_players[pid].buffer;
    }
}

void NetPlayClient::OnHostInputAuthority(sf::Packet& packet)
{
  packet >> m_host_input_authority;
  m_dialog->OnHostInputAuthorityChanged(m_host_input_authority);
}

void NetPlayClient::OnRollbackMode(sf::Packet& packet)
{
  packet >> m_rollback_mode;
  m_dialog->OnRollbackModeChanged(m_rollback_mode);
}

void NetPlayClient::OnGolfSwitch(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  const PlayerId previous_golfer = m_current_golfer;
  m_current_golfer = pid;
  m_dialog->OnGolferChanged(m_local_player->pid == pid, pid != 0 ? m_players[pid].name : "");

  if (m_local_player->pid == previous_golfer)
  {
    sf::Packet spac;
    spac << MessageID::GolfRelease;
    Send(spac);
  }
  else if (m_local_player->pid == pid)
  {
    sf::Packet spac;
    spac << MessageID::GolfAcquire;
    Send(spac);

    // Pads are already calibrated so we can just ignore this
    m_first_pad_status_received.fill(true);

    m_wait_on_input = false;
    m_wait_on_input_event.Set();
  }
}

void NetPlayClient::OnGolfPrepare(sf::Packet& packet)
{
  m_wait_on_input_received = true;
  m_wait_on_input = true;
}

void NetPlayClient::OnChangeGame(sf::Packet& packet)
{
  std::string netplay_name;
  {
    std::lock_guard lkg(m_crit.game);
    ReceiveSyncIdentifier(packet, m_selected_game);
    packet >> netplay_name;
  }

  INFO_LOG_FMT(NETPLAY, "Game changed to {}", netplay_name);

  // update gui
  m_dialog->OnMsgChangeGame(m_selected_game, netplay_name);

  SendGameStatus();

  sf::Packet client_capabilities_packet;
  client_capabilities_packet << MessageID::ClientCapabilities;
  client_capabilities_packet << ExpansionInterface::CEXIIPL::HasIPLDump();
  client_capabilities_packet << Config::Get(Config::SESSION_USE_FMA);
  Send(client_capabilities_packet);
}

void NetPlayClient::OnGameStatus(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard lkp(m_crit.players);
    packet >> m_players[pid].game_status;
  }

  m_dialog->Update();
}

void NetPlayClient::ToJson(json& j, const BrawlbackPad& i)
{
  j["game_pad"] = json{{"_buttons", i._buttons},
           {"buttons", i.buttons},
           {"holdButtons", i.holdButtons},
           {"rapidFireButtons", i.rapidFireButtons},
           {"releasedButtons", i.releasedButtons},
           {"newPressedButtons", i.newPressedButtons},
           {"LAnalogue", i.LAnalogue},
           {"RAnalogue", i.RAnalogue},
           {"LTrigger", i.LTrigger},
           {"RTrigger", i.RTrigger},
           {"stickX", i.stickX},
           {"stickY", i.stickY},
           {"cStickX", i.cStickX},
           {"cStickY", i.cStickY}};
}
void NetPlayClient::ToJson(json& j, const GCPadStatus& i)
{
  j["emu_pad"] = {{"button", i.button},
           {"stickX", i.stickX},
           {"stickY", i.stickY},
           {"substickX", i.substickX},
           {"substickY", i.substickY},
           {"triggerLeft", i.triggerLeft},
           {"triggerRight", i.triggerRight},
           {"analogA", i.analogA},
           {"analogB", i.analogB}};
}
void NetPlayClient::ToJson(json& j, const Inputs& i)
{
  j["frame"] = i.frame;
  ToJson(j, i.game_pad);
  ToJson(j, i.emu_pad);
}
void NetPlayClient::FromJson(const json& j, BrawlbackPad& i)
{
  j.at("frame").get_to(i._buttons);
  j.at("buttons").get_to(i.buttons);
  j.at("holdButtons").get_to(i.holdButtons);
  j.at("rapidFireButtons").get_to(i.rapidFireButtons);
  j.at("releasedButtons").get_to(i.releasedButtons);
  j.at("newPressedButtons").get_to(i.newPressedButtons);
  j.at("LAnalogue").get_to(i.LAnalogue);
  j.at("RAnalogue").get_to(i.RAnalogue);
  j.at("stickX").get_to(i.stickX);
  j.at("stickY").get_to(i.stickY);
  j.at("cStickX").get_to(i.cStickX);
  j.at("cStickY").get_to(i.cStickY);
}
void NetPlayClient::FromJson(const json& j, GCPadStatus& i)
{
  j.at("button").get_to(i.button);
  j.at("stickX").get_to(i.stickX);
  j.at("stickY").get_to(i.stickY);
  j.at("substickX").get_to(i.substickX);
  j.at("substickY").get_to(i.substickY);
  j.at("triggerLeft").get_to(i.triggerLeft);
  j.at("triggerRight").get_to(i.triggerRight);
  j.at("analogA").get_to(i.analogA);
  j.at("analogB").get_to(i.analogB);
}
void NetPlayClient::FromJson(const json& j, Inputs& i)
{
  j.at("frame").get_to(i.frame);
  FromJson(j, i.game_pad);
  FromJson(j, i.emu_pad);
}
void NetPlayClient::OnStartGame(sf::Packet& packet)
{
  {
    std::lock_guard lkg(m_crit.game);

    INFO_LOG_FMT(NETPLAY, "Start of game {}", m_selected_game.game_id);

    packet >> m_current_game;
    packet >> m_net_settings.cpu_thread;
    packet >> m_net_settings.cpu_core;
    packet >> m_net_settings.enable_cheats;
    packet >> m_net_settings.enable_hardcore;
    packet >> m_net_settings.selected_language;
    packet >> m_net_settings.override_region_settings;
    packet >> m_net_settings.dsp_enable_jit;
    packet >> m_net_settings.dsp_hle;
    packet >> m_net_settings.ram_override_enable;
    packet >> m_net_settings.mem1_size;
    packet >> m_net_settings.mem2_size;
    packet >> m_net_settings.fallback_region;
    packet >> m_net_settings.allow_sd_writes;
    packet >> m_net_settings.oc_enable;
    packet >> m_net_settings.oc_factor;
    packet >> m_net_settings.vi_oc_enable;
    packet >> m_net_settings.vi_oc_factor;
    packet >> m_net_settings.spectator_mode;

    for (auto slot : ExpansionInterface::SLOTS)
      packet >> m_net_settings.exi_device[slot];

    packet >> m_net_settings.memcard_size_override;

    for (u32& value : m_net_settings.sysconf_settings)
      packet >> value;

    packet >> m_net_settings.efb_access_enable;
    packet >> m_net_settings.bbox_enable;
    packet >> m_net_settings.force_progressive;
    packet >> m_net_settings.efb_to_texture_enable;
    packet >> m_net_settings.xfb_to_texture_enable;
    packet >> m_net_settings.disable_copy_to_vram;
    packet >> m_net_settings.immediate_xfb_enable;
    packet >> m_net_settings.efb_emulate_format_changes;
    packet >> m_net_settings.safe_texture_cache_color_samples;
    packet >> m_net_settings.perf_queries_enable;
    packet >> m_net_settings.float_exceptions;
    packet >> m_net_settings.divide_by_zero_exceptions;
    packet >> m_net_settings.fprf;
    packet >> m_net_settings.accurate_nans;
    packet >> m_net_settings.disable_icache;
    packet >> m_net_settings.sync_on_skip_idle;
    packet >> m_net_settings.sync_gpu;
    packet >> m_net_settings.sync_gpu_max_distance;
    packet >> m_net_settings.sync_gpu_min_distance;
    packet >> m_net_settings.sync_gpu_overclock;
    packet >> m_net_settings.jit_follow_branch;
    packet >> m_net_settings.fast_disc_speed;
    packet >> m_net_settings.mmu;
    packet >> m_net_settings.fastmem;
    packet >> m_net_settings.skip_ipl;
    packet >> m_net_settings.load_ipl_dump;
    packet >> m_net_settings.vertex_rounding;
    packet >> m_net_settings.internal_resolution;
    packet >> m_net_settings.efb_scaled_copy;
    packet >> m_net_settings.fast_depth_calc;
    packet >> m_net_settings.enable_pixel_lighting;
    packet >> m_net_settings.widescreen_hack;
    packet >> m_net_settings.force_texture_filtering;
    packet >> m_net_settings.max_anisotropy;
    packet >> m_net_settings.force_true_color;
    packet >> m_net_settings.disable_copy_filter;
    packet >> m_net_settings.disable_fog;
    packet >> m_net_settings.arbitrary_mipmap_detection;
    packet >> m_net_settings.arbitrary_mipmap_detection_threshold;
    packet >> m_net_settings.enable_gpu_texture_decoding;
    packet >> m_net_settings.defer_efb_copies;
    packet >> m_net_settings.efb_access_tile_size;
    packet >> m_net_settings.efb_access_defer_invalidation;
    packet >> m_net_settings.savedata_load;
    packet >> m_net_settings.savedata_write;
    packet >> m_net_settings.savedata_sync_all_wii;
    if (!m_net_settings.savedata_load)
    {
      m_net_settings.savedata_write = false;
      m_net_settings.savedata_sync_all_wii = false;
    }
    packet >> m_net_settings.strict_settings_sync;

    m_initial_rtc = Common::PacketReadU64(packet);

    packet >> m_net_settings.save_data_region;
    packet >> m_net_settings.sync_codes;
    packet >> m_net_settings.m_RollbackMode;
    packet >> m_net_settings.golf_mode;
    packet >> m_net_settings.use_fma;
    packet >> m_net_settings.hide_remote_gbas;

    for (size_t i = 0; i < sizeof(m_net_settings.sram); ++i)
      packet >> m_net_settings.sram[i];

    m_net_settings.is_hosting = m_local_player->IsHost();
  }

  if (IsInRollbackMode())
  {
    inputs.clear();
    predicted_inputs.clear();
    pad_config.clear();

    for (auto player : m_players)
    {
      if (player.first == m_local_player->pid)
      {
        RingBufferInput empty_pads = {};
        auto config = Config::Get(Config::GetInfoForSIDevice(0));
        for (int i = 0; i < delay; i++)
        {
          Inputs empty_pad = Inputs{};
          empty_pad.emu_pad = GetDefaultPad(config);
          empty_pads.Push(empty_pad);
        }
        inputs.push_back(empty_pads);

        pad_config.push_back(config);
      }
      else
      {
        pad_config.push_back(12);
        inputs.push_back({});
      }
      std::string port = fmt::format("{}_{}", "player_port", player.first);
      inputs_output[port] = json::array();
      predicted_inputs.push_back(Inputs{});
    }
    current_frame = 0;
    if (!time_sync)
    {
      time_sync = std::make_unique<Brawlback::TimeSync>();
    }
    time_sync->startGame(static_cast<u8>(inputs.size()));
  }
  
  m_dialog->OnMsgStartGame();
}

void NetPlayClient::OnStopGame(sf::Packet& packet)
{
  INFO_LOG_FMT(NETPLAY, "Game stopped");

  StopGame();
  m_dialog->OnMsgStopGame();
}

void NetPlayClient::OnPowerButton()
{
  InvokeStop();
  m_dialog->OnMsgPowerButton();
}

void NetPlayClient::OnPing(sf::Packet& packet)
{
  u32 ping_key = 0;
  packet >> ping_key;

  sf::Packet response_packet;
  response_packet << MessageID::Pong;
  response_packet << ping_key;

  Send(response_packet);
}

void NetPlayClient::OnPlayerPingData(sf::Packet& packet)
{
  PlayerId pid;
  packet >> pid;

  {
    std::lock_guard lkp(m_crit.players);
    Player& player = m_players[pid];
    packet >> player.ping;
  }

  DisplayPlayersPing();
  m_dialog->Update();
}

void NetPlayClient::OnDesyncDetected(sf::Packet& packet)
{
  int player_index;
  s32 frame;
  packet >> player_index;
  packet >> frame;

  std::string player_name = GetPlayerName(player_index);

  INFO_LOG_FMT(NETPLAY, "Server reported desync: Player {} at frame {}", player_name, frame);

  if (m_net_settings.m_RollbackMode)
  {
    IncrementalRB::DumpMemoryState(frame, "network_desync");

    // In rollback mode, show warning but continue playing
    std::string message =
        fmt::format("Network desync detected with {}!\nAttempting to recover...", player_name);

    m_dialog->AppendChat(message);

    OSD::AddTypedMessage(OSD::MessageType::NetPlayBuffer, message, OSD::Duration::VERY_LONG,
                         OSD::Color::YELLOW);
  }
  else
  {
    // In delay-based mode, this is critical
    m_dialog->OnDesync(frame, player_name);

    OSD::AddTypedMessage(OSD::MessageType::NetPlayBuffer,
                         fmt::format("Critical desync with {}!", player_name),
                         OSD::Duration::VERY_LONG, OSD::Color::RED);
  }
}

void NetPlayClient::OnSyncSaveData(sf::Packet& packet)
{
  SyncSaveDataID sub_id;
  packet >> sub_id;

  if (m_local_player->IsHost())
    return;

  INFO_LOG_FMT(NETPLAY, "Processing OnSyncSaveData sub id: {}", static_cast<u8>(sub_id));

  switch (sub_id)
  {
  case SyncSaveDataID::Notify:
    OnSyncSaveDataNotify(packet);
    break;

  case SyncSaveDataID::RawData:
    OnSyncSaveDataRaw(packet);
    break;

  case SyncSaveDataID::GCIData:
    OnSyncSaveDataGCI(packet);
    break;

  case SyncSaveDataID::WiiData:
    OnSyncSaveDataWii(packet);
    break;

  case SyncSaveDataID::GBAData:
    OnSyncSaveDataGBA(packet);
    break;

  default:
    PanicAlertFmtT("Unknown SYNC_SAVE_DATA message received with id: {0}", static_cast<u8>(sub_id));
    break;
  }
}

bool NetPlayClient::LoadFromFrame(s32 origFrame, s32 frame)
{
  return IncrementalRB::Rollback(origFrame, frame);
        }

void NetPlayClient::SendInputs(sf::Packet& packet, int local_player_port, MessageID frame_data_cmd)
{
  if (inputs.at(local_player_port).Empty())
  {
    return;
  }
  int minAckFrame = time_sync->getMinAckFrame(static_cast<u8>(inputs.size()));
  minAckFrame = std::min(minAckFrame, (int)current_frame);

  // ---------- WRONG ---------------
  // 1 - 1 - (0 - 0) = 0, send no frames
  // 2 - 1 - (1 - 0) = 0, send frames 1
  // 3 - 1 - (2 - 1) = 1, send frames 2 to 3
  // 4 - 1 - (3 - 1) = 1, send frames 2 to 4
  // --------- GOAL -----------------
  // on frame 100, send frame 100
  // on frame 101, send frames 100 and 101
  // ack frame 100
  // on frame 101, send frames 101 and 102
  // ack frame 101
  // ack frame 102
  // on frame 103, send frame 103

  // min ack frame = -1
  // current frame = 0
  // send frame    = 0
  // -----------------
  // start index = 0
  // end index = 0
  //
  // min ack frame = 10
  // current frame = 13
  // send frame    = 11, 12, 13
  // -----------------
  // start index = 11
  // end index = 14
  int endIdx = 0, startIdx = 0;
  if (minAckFrame == -1)
  {
    startIdx = 0;
    endIdx = (int)inputs.at(local_player_port).Size();
  }
  else
  {
    endIdx = current_frame + 1;
    startIdx = minAckFrame + 1;
  }
  std::vector<Inputs> localFramedatas = {};

  for (int i = startIdx; i < endIdx; i++)
  {
    auto localFramedata = inputs.at(local_player_port).Get(i);

    if (localFramedata.has_value())
    {
      localFramedatas.push_back(localFramedata.value());
      INFO_LOG_FMT(BRAWLBACK, "INPUT TO SEND FRAME: {}\n", localFramedata->frame);
    }
  }

  packet << frame_data_cmd;
  // TODO: Make this work for all local pads.
  packet << static_cast<s8>(LocalPadToInGamePad(0));
  packet << static_cast<int>(Config::Get(Config::GetInfoForSIDevice(0)));
  // append number of framedatas that are in this packet
  u8 sizeofFramedatas = static_cast<u8>(localFramedatas.size());
  packet << sizeofFramedatas;
  s32 maxFrame = (localFramedatas.size() > 0) ? localFramedatas.back().frame : 0;
  packet << static_cast<u8>(local_player_port);
  packet << maxFrame;
  for (auto& data : localFramedatas)
  {
    auto& pad = data.game_pad;
    auto& emu_pad = data.emu_pad;
    packet << pad.buttons << pad._buttons << pad.holdButtons << pad.rapidFireButtons
           << pad.newPressedButtons << pad.releasedButtons << pad.stickX << pad.stickY
           << pad.cStickX << pad.cStickY << pad.LAnalogue << pad.RAnalogue << pad.LTrigger
           << pad.RTrigger;

    packet << emu_pad.button << emu_pad.stickX << emu_pad.stickY << emu_pad.substickX
           << emu_pad.substickY << emu_pad.triggerLeft << emu_pad.triggerRight << emu_pad.analogA
           << emu_pad.analogB << emu_pad.isConnected;

    packet << data.frame;
  }
  this->SendAsync(std::move(packet), DEFAULT_CHANNEL, ENET_PACKET_FLAG_UNSEQUENCED);

  time_sync->TimeSyncUpdate(maxFrame, static_cast<u8>(inputs.size()));
}
u32 NetPlayClient::GetLatestRemoteFrame(int local_player_port)
{
  u32 lowestFrame = 0;

  for (int i = 0; i < inputs.size(); i++)
  {
    if (i == local_player_port)
      continue;

    const Inputs* last = inputs.at(i).GetBack();
    if (!last)
      continue;

    u32 f = static_cast<u32>(last->frame);

    if (f < lowestFrame || lowestFrame == 0)
    {
      lowestFrame = f;
    }
  }

  return lowestFrame;
}
void NetPlayClient::StoreInputs(Inputs& pad, int local_player_port)
{
  const Inputs* last = inputs.at(local_player_port).GetBack();

  bool is_sequential_input = inputs.at(local_player_port).Empty() ||
                             inputs.at(local_player_port).GetBack()->frame == pad.frame - 1;

  if (is_sequential_input)
  {
    // Store local framedata using RingBufferInput
    inputs.at(local_player_port).Push(pad);

    auto current_inputs = json{};
    ToJson(current_inputs, pad);
    std::string port = fmt::format("{}_{}", "player_port", local_player_port);

    inputs_output[port].push_back(current_inputs);
  }
  else
  {
    ERROR_LOG_FMT(BRAWLBACK, "Non-sequential input: last={}, new={}", last ? last->frame : -1,
                  pad.frame);
  }
}
void NetPlayClient::PrintInputs(BrawlbackPad& pad)
{
  INFO_LOG_FMT(BRAWLBACK, "-----------------\n");
  INFO_LOG_FMT(BRAWLBACK, "STICK X: {}\n", pad.stickX);
  INFO_LOG_FMT(BRAWLBACK, "STICK Y: {}\n", pad.stickY);
  INFO_LOG_FMT(BRAWLBACK, "C STICK X: {}\n", pad.cStickX);
  INFO_LOG_FMT(BRAWLBACK, "C STICK Y: {}\n", pad.cStickY);
  INFO_LOG_FMT(BRAWLBACK, "BUTTONS: {}\n", pad.buttons);
  INFO_LOG_FMT(BRAWLBACK, "_BUTTONS: {}\n", pad._buttons);
  INFO_LOG_FMT(BRAWLBACK, "_BUTTONS: {}\n", pad.holdButtons);
  INFO_LOG_FMT(BRAWLBACK, "_BUTTONS: {}\n", pad.rapidFireButtons);
  INFO_LOG_FMT(BRAWLBACK, "_BUTTONS: {}\n", pad.releasedButtons);
  INFO_LOG_FMT(BRAWLBACK, "_BUTTONS: {}\n", pad.newPressedButtons);
  INFO_LOG_FMT(BRAWLBACK, "LEFT TRIGGER: {}\n", pad.LTrigger);
  INFO_LOG_FMT(BRAWLBACK, "RIGHT TRIGGER: {}\n", pad.RTrigger);
  INFO_LOG_FMT(BRAWLBACK, "ANALOG A: {}\n", pad.LAnalogue);
  INFO_LOG_FMT(BRAWLBACK, "ANALOG B: {}\n", pad.RAnalogue);
  INFO_LOG_FMT(BRAWLBACK, "-----------------\n");
}
void NetPlayClient::HandleInputs(Inputs pad)
{
  int local_player_port = -1;
  for (int i = 0; i < m_pad_map.size(); i++)
  {
    if (m_pad_map.at(i) == m_local_player->pid)
      local_player_port = i;
  }

  sf::Packet packet = sf::Packet();
  auto frame_data_cmd = MessageID::PadData;

  if (!is_stalled)
  {
    StoreInputs(pad, local_player_port);
    SendInputs(packet, local_player_port, frame_data_cmd);
    // this->SendCmdToGame(EXICommand::CMD_TIMESYNC);
  }
  else
  {
    INFO_LOG_FMT(BRAWLBACK, "Should time sync\n");
    SendInputs(packet, local_player_port, frame_data_cmd);
    advance_frames = 0;
  }
}
void NetPlayClient::OnFrameStart(BrawlbackPad& pad)
{
  // this function is only called in rollback mode, but the logic to skip it is in
  // OnFrameStart() (the one not in NetPlayClient::)

  advance_frames = 1;
  // Wait for inputs if others are behind us, continue if we're behind them
  int local_player_port = -1;
  for (int i = 0; i < m_pad_map.size(); i++)
  {
    if (m_pad_map.at(i) == m_local_player->pid)
      local_player_port = i;
  }

  auto isRollbackMode = [](std::vector<RingBufferInput>& inputs, bu32 locFrame, u8 playerIdx) {
    return ROLLBACK_IMPL &&                     // delay-based/rollback toggle
           locFrame > GAME_FULL_START_FRAME &&  // give the game a bit of time in delay-based
                                                // mode to sync up
           !inputs[playerIdx].Empty() &&        // some sanity checks
           !inputs[playerIdx].Empty() && inputs[playerIdx].Size() >= MAX_ROLLBACK_FRAMES;
  };

  // TODO: Make this work for all local pad ports
  GCPadStatus pad_status;
  if (pad_config.at(local_player_port) == SerialInterface::SIDEVICE_WIIU_ADAPTER)
  {
    pad_status = GCAdapter::Input(0);
  }
  else
  {
    pad_status = Pad::GetStatus(0);
  }

  is_stalled = time_sync->shouldStallFrame(current_frame, GetLatestRemoteFrame(local_player_port),
                                           static_cast<u8>(inputs.size()), MAX_ROLLBACK_FRAMES);
  HandleInputs({pad_status, pad, current_frame + delay});

  if (!is_stalled)
  {
    for (s32 i = 0; i < inputs.size(); i++)
    {
      if (i != local_player_port && isRollbackMode(inputs, current_frame, i))
      {
        s32 remoteFrame = inputs.at(i).GetBack()->frame;
        s32 finalFrame = std::min(remoteFrame, current_frame);

        bool isSynchronized = true;

        auto shouldRollback = [](s32 latest_confirmed_frame, s32 frame, s32 remote_frame) -> bool {
          return frame > latest_confirmed_frame && remote_frame > latest_confirmed_frame;
        };
        if (isRollbackMode(inputs, current_frame, i))
        {
          auto ret = FindRemoteInputs(i, current_frame);
          if (ret == std::nullopt)
          {
            auto predict = FindRemoteInputs(i, latest_confirmed_frame);
            if (predict != std::nullopt)
            {
              is_predicting = true;
              predicted_inputs.at(i) = predict.value();
            }
          }
          else
          {
            is_predicting = false;
          }
        }
        if (is_predicting && shouldRollback(latest_confirmed_frame, current_frame, remoteFrame) &&
            latest_confirmed_frame > 0)
        {
          auto playerPredictedInputs = predicted_inputs.at(i).game_pad;
          INFO_LOG_FMT(BRAWLBACK,
                       "IS PREDICTING! LATEST CONFIRMED: {}, CURRENT FRAME: {}, FINAL FRAME: {}",
                       latest_confirmed_frame, current_frame, finalFrame);
          for (int f = latest_confirmed_frame + 1; f <= finalFrame; f++)
          {
            auto remoteInputs = FindRemoteInputs(i, f);
            if (remoteInputs == std::nullopt)
            {
              ERROR_LOG_FMT(BRAWLBACK, "Couldn't find remote inputs for frame {}!\n", f);
            }
            else
            {
              PrintInputs(playerPredictedInputs);
              PrintInputs(remoteInputs.value().game_pad);
              if (!(playerPredictedInputs.buttons == remoteInputs.value().game_pad.buttons &&
                    playerPredictedInputs._buttons == remoteInputs.value().game_pad.buttons &&
                    playerPredictedInputs.holdButtons == remoteInputs.value().game_pad.buttons &&
                    playerPredictedInputs.rapidFireButtons ==
                        remoteInputs.value().game_pad.buttons &&
                    playerPredictedInputs.releasedButtons ==
                        remoteInputs.value().game_pad.buttons &&
                    playerPredictedInputs.newPressedButtons ==
                        remoteInputs.value().game_pad.buttons &&
                    playerPredictedInputs.stickX == remoteInputs.value().game_pad.stickX &&
                    playerPredictedInputs.stickY == remoteInputs.value().game_pad.stickY &&
                    playerPredictedInputs.cStickX == remoteInputs.value().game_pad.cStickX &&
                    playerPredictedInputs.cStickY == remoteInputs.value().game_pad.cStickY &&
                    playerPredictedInputs.LTrigger == remoteInputs.value().game_pad.LTrigger &&
                    playerPredictedInputs.RTrigger == remoteInputs.value().game_pad.RTrigger &&
                    playerPredictedInputs.LAnalogue == remoteInputs.value().game_pad.LAnalogue &&
                    playerPredictedInputs.RAnalogue == remoteInputs.value().game_pad.RAnalogue))
              {
                // remote inputs don't match predicted
                latest_confirmed_frame = f - 1;
                isSynchronized = false;
                INFO_LOG_FMT(BRAWLBACK, "Remote didn't match predicted inputs frame = {}\n", f);
                break;
              }
            }
          }
        }

        if (isSynchronized)
        {
          latest_confirmed_frame = finalFrame;
          // INFO_LOG_FMT(BRAWLBACK, "is synchronized!\n");
        }
        else
        {
          // not synchronized, rollback & resim
          INFO_LOG_FMT(BRAWLBACK, "Should rollback! frame = {} latestConfirmedFrame = {}\n",
                       current_frame, latest_confirmed_frame);
          if (LoadFromFrame(current_frame, latest_confirmed_frame))
          {
            // if on frame 10 we rollback to frame 7 we need to simulate frames 7,8,9, and 10 to get
            // to where we were before. 10 - 7 + 1 = 4
            advance_frames = current_frame - latest_confirmed_frame + 1;
            INFO_LOG_FMT(BRAWLBACK, "Num frames to simulate = {}\n", advance_frames);
            frame_to_stop_at = current_frame;
            current_frame = latest_confirmed_frame;
            is_rollingback = true;
          }
          else
          {
            is_rollingback = false;
            ERROR_LOG_FMT(BRAWLBACK, "Failed to roll back to frame {}!", latest_confirmed_frame);
          }
        }
      }
    }
  }
  else
  {
    advance_frames = 0;
  }
}
bool NetPlayClient::IsRollingBack()
{
  return is_rollingback.load();
}

bool NetPlayClient::IsStarted()
{
  return game_started.load();
}

bool NetPlayClient::IsInRollbackMode()
{
  return m_net_settings.m_RollbackMode;
}

size_t NetPlayClient::GetInputsSize()
{
  return inputs.size();
}

std::optional<BrawlbackPad> NetPlayClient::GetPredictedInputs(int playerIdx, s32 frame)
{
  std::optional<BrawlbackPad> pad =  std::nullopt;
  if (predicted_inputs.at(playerIdx).frame == frame)
  {
    pad = predicted_inputs.at(playerIdx).game_pad;
  }
  return pad;
}

std::optional<Inputs> NetPlayClient::FindRemoteInputs(int playerIdx, s32 frame)
{
  return inputs.at(playerIdx).Get(frame);
}

void NetPlayClient::OnSyncSaveDataNotify(sf::Packet& packet)
{
  packet >> m_sync_save_data_count;
  m_sync_save_data_success_count = 0;

  INFO_LOG_FMT(NETPLAY, "Initializing wait for {} savegame chunks.", m_sync_save_data_count);

  if (m_sync_save_data_count == 0)
    SyncSaveDataResponse(true);
  else
    m_dialog->AppendChat(Common::GetStringT("Synchronizing save data..."));
}

void NetPlayClient::OnSyncSaveDataRaw(sf::Packet& packet)
{
  bool is_slot_a;
  std::string region;
  int size_override;
  packet >> is_slot_a >> region >> size_override;

  INFO_LOG_FMT(NETPLAY, "Received raw memcard data for slot {}: region {}, size override {}.",
               is_slot_a ? 'A' : 'B', region, size_override);

  // This check is mainly intended to filter out characters which have special meanings in paths
  if (region != JAP_DIR && region != USA_DIR && region != EUR_DIR)
  {
    WARN_LOG_FMT(NETPLAY, "Received invalid raw memory card region.");
    SyncSaveDataResponse(false);
    return;
  }

  std::string size_suffix;
  if (size_override >= 0 && size_override <= 4)
  {
    size_suffix = fmt::format(
        ".{}", Memcard::MbitToFreeBlocks(Memcard::MBIT_SIZE_MEMORY_CARD_59 << size_override));
  }

  const std::string path = File::GetUserPath(D_GCUSER_IDX) + GC_MEMCARD_NETPLAY +
                           (is_slot_a ? "A." : "B.") + region + size_suffix + ".raw";
  if (File::Exists(path) && !File::Delete(path))
  {
    PanicAlertFmtT("Failed to delete NetPlay memory card. Verify your write permissions.");
    SyncSaveDataResponse(false);
    return;
  }

  const bool success = DecompressPacketIntoFile(packet, path);
  SyncSaveDataResponse(success);
}

void NetPlayClient::OnSyncSaveDataGCI(sf::Packet& packet)
{
  bool is_slot_a;
  u8 file_count;
  packet >> is_slot_a >> file_count;

  const std::string path = File::GetUserPath(D_GCUSER_IDX) + GC_MEMCARD_NETPLAY DIR_SEP +
                           fmt::format("Card {}", is_slot_a ? 'A' : 'B');

  INFO_LOG_FMT(NETPLAY, "Received GCI memcard data for slot {}: {}, {} files.",
               is_slot_a ? 'A' : 'B', path, file_count);

  if ((File::Exists(path) && !File::DeleteDirRecursively(path + DIR_SEP)) ||
      !File::CreateFullPath(path + DIR_SEP))
  {
    PanicAlertFmtT("Failed to reset NetPlay GCI folder. Verify your write permissions.");
    SyncSaveDataResponse(false);
    return;
  }

  for (u8 i = 0; i < file_count; i++)
  {
    std::string file_name;
    packet >> file_name;

    INFO_LOG_FMT(NETPLAY, "Received GCI: {}", file_name);

    if (!Common::IsFileNameSafe(file_name) ||
        !DecompressPacketIntoFile(packet, path + DIR_SEP + file_name))
    {
      WARN_LOG_FMT(NETPLAY, "Received invalid GCI.");
      SyncSaveDataResponse(false);
      return;
    }
  }

  SyncSaveDataResponse(true);
}

void NetPlayClient::OnSyncSaveDataWii(sf::Packet& packet)
{
  const std::string path = File::GetUserPath(D_USER_IDX) + "Wii" GC_MEMCARD_NETPLAY DIR_SEP;
  std::string redirect_path = File::GetUserPath(D_USER_IDX) + "Redirect" GC_MEMCARD_NETPLAY DIR_SEP;

  if (File::Exists(path) && !File::DeleteDirRecursively(path))
  {
    PanicAlertFmtT("Failed to reset NetPlay NAND folder. Verify your write permissions.");
    SyncSaveDataResponse(false);
    return;
  }
  if (File::Exists(redirect_path) && !File::DeleteDirRecursively(redirect_path))
  {
    PanicAlertFmtT("Failed to reset NetPlay redirect folder. Verify your write permissions.");
    SyncSaveDataResponse(false);
    return;
  }

  auto temp_fs = std::make_unique<IOS::HLE::FS::HostFileSystem>(path);
  std::vector<u64> titles;

  constexpr IOS::HLE::FS::Modes fs_modes{
      IOS::HLE::FS::Mode::ReadWrite,
      IOS::HLE::FS::Mode::ReadWrite,
      IOS::HLE::FS::Mode::ReadWrite,
  };

  // Read the Mii data
  bool mii_data;
  packet >> mii_data;
  if (mii_data)
  {
    INFO_LOG_FMT(NETPLAY, "Received Mii data.");

    auto buffer = DecompressPacketIntoBuffer(packet);

    temp_fs->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL, "/shared2/menu/FaceLib/", 0,
                            fs_modes);
    auto file = temp_fs->CreateAndOpenFile(IOS::PID_KERNEL, IOS::PID_KERNEL,
                                           Common::GetMiiDatabasePath(), fs_modes);

    if (!buffer || !file || !file->Write(buffer->data(), buffer->size()))
    {
      PanicAlertFmtT("Failed to write Mii data.");
      SyncSaveDataResponse(false);
      return;
    }
  }

  // Read the saves
  u32 save_count;
  packet >> save_count;
  INFO_LOG_FMT(NETPLAY, "Received data for {} Wii saves.", save_count);
  for (u32 n = 0; n < save_count; n++)
  {
    u64 title_id = Common::PacketReadU64(packet);
    titles.push_back(title_id);
    temp_fs->CreateFullPath(IOS::PID_KERNEL, IOS::PID_KERNEL,
                            Common::GetTitleDataPath(title_id) + '/', 0, fs_modes);
    auto save = WiiSave::MakeNandStorage(temp_fs.get(), title_id);

    bool exists;
    packet >> exists;
    if (!exists)
    {
      INFO_LOG_FMT(NETPLAY, "No data for Wii save of title {:016x}.", title_id);
      continue;
    }

    INFO_LOG_FMT(NETPLAY, "Received Wii save of title {:016x}.", title_id);

    // Header
    WiiSave::Header header;
    packet >> header.tid;
    packet >> header.banner_size;
    packet >> header.permissions;
    packet >> header.unk1;
    for (u8& byte : header.md5)
      packet >> byte;
    packet >> header.unk2;
    for (size_t i = 0; i < header.banner_size; i++)
      packet >> header.banner[i];

    // BkHeader
    WiiSave::BkHeader bk_header;
    packet >> bk_header.size;
    packet >> bk_header.magic;
    packet >> bk_header.ngid;
    packet >> bk_header.number_of_files;
    packet >> bk_header.size_of_files;
    packet >> bk_header.unk1;
    packet >> bk_header.unk2;
    packet >> bk_header.total_size;
    for (u8& byte : bk_header.unk3)
      packet >> byte;
    packet >> bk_header.tid;
    for (u8& byte : bk_header.mac_address)
      packet >> byte;

    // Files
    std::vector<WiiSave::Storage::SaveFile> files;
    for (u32 i = 0; i < bk_header.number_of_files; i++)
    {
      WiiSave::Storage::SaveFile file;
      packet >> file.mode >> file.attributes;
      packet >> file.type;
      packet >> file.path;

      INFO_LOG_FMT(NETPLAY, "Received Wii save data of type {} at {}", static_cast<u8>(file.type),
                   file.path);

      if (file.type == WiiSave::Storage::SaveFile::Type::File)
      {
        auto buffer = DecompressPacketIntoBuffer(packet);
        if (!buffer)
        {
          SyncSaveDataResponse(false);
          return;
        }

        file.data = std::move(*buffer);
      }

      files.push_back(std::move(file));
    }

    if (!save->WriteHeader(header) || !save->WriteBkHeader(bk_header) || !save->WriteFiles(files))
    {
      PanicAlertFmtT("Failed to write Wii save.");
      SyncSaveDataResponse(false);
      return;
    }
  }

  bool has_redirected_save;
  packet >> has_redirected_save;
  if (has_redirected_save)
  {
    INFO_LOG_FMT(NETPLAY, "Received redirected save.");
    if (!DecompressPacketIntoFolder(packet, redirect_path))
    {
      PanicAlertFmtT("Failed to write redirected save.");
      SyncSaveDataResponse(false);
      return;
    }
  }

  SetWiiSyncData(std::move(temp_fs), std::move(titles), std::move(redirect_path));
  SyncSaveDataResponse(true);
}

void NetPlayClient::OnSyncSaveDataGBA(sf::Packet& packet)
{
  u8 slot;
  packet >> slot;

  INFO_LOG_FMT(NETPLAY, "Received GBA save for slot {}.", slot);

  const std::string path =
      fmt::format("{}{}{}.sav", File::GetUserPath(D_GBAUSER_IDX), GBA_SAVE_NETPLAY, slot + 1);
  if (File::Exists(path) && !File::Delete(path))
  {
    PanicAlertFmtT("Failed to delete NetPlay GBA{0} save file. Verify your write permissions.",
                   slot + 1);
    SyncSaveDataResponse(false);
    return;
  }

  const bool success = DecompressPacketIntoFile(packet, path);
  SyncSaveDataResponse(success);
}

void NetPlayClient::OnSyncCodes(sf::Packet& packet)
{
  // Receive Data Packet
  SyncCodeID sub_id;
  packet >> sub_id;

  INFO_LOG_FMT(NETPLAY, "Processing OnSyncCodes sub id: {}", static_cast<u8>(sub_id));

  // Check Which Operation to Perform with This Packet
  switch (sub_id)
  {
  case SyncCodeID::Notify:
    OnSyncCodesNotify();
    break;

  case SyncCodeID::NotifyGecko:
    OnSyncCodesNotifyGecko(packet);
    break;

  case SyncCodeID::GeckoData:
    OnSyncCodesDataGecko(packet);
    break;

  case SyncCodeID::NotifyAR:
    OnSyncCodesNotifyAR(packet);
    break;

  case SyncCodeID::ARData:
    OnSyncCodesDataAR(packet);
    break;

  default:
    PanicAlertFmtT("Unknown SYNC_CODES message received with id: {0}", static_cast<u8>(sub_id));
    break;
  }
}

void NetPlayClient::OnSyncCodesNotify()
{
  // Set both codes as unsynced
  m_sync_gecko_codes_complete = false;
  m_sync_ar_codes_complete = false;
}

void NetPlayClient::OnSyncCodesNotifyGecko(sf::Packet& packet)
{
  // Return if this is the host
  if (m_local_player->IsHost())
    return;

  // Receive Number of Codelines to Receive
  packet >> m_sync_gecko_codes_count;

  m_sync_gecko_codes_success_count = 0;

  INFO_LOG_FMT(NETPLAY, "Receiving {} Gecko codelines", m_sync_gecko_codes_count);

  // Check if no codes to sync, if so return as finished
  if (m_sync_gecko_codes_count == 0)
  {
    m_sync_gecko_codes_complete = true;
    SyncCodeResponse(true);
  }
  else
  {
    m_dialog->AppendChat(Common::GetStringT("Synchronizing Gecko codes..."));
  }
}

void NetPlayClient::OnSyncCodesDataGecko(sf::Packet& packet)
{
  // Return if this is the host
  if (m_local_player->IsHost())
    return;

  std::vector<Gecko::GeckoCode> synced_codes;
  synced_codes.reserve(m_sync_gecko_codes_count);

  Gecko::GeckoCode gcode{};
  gcode.name = "Synced Codes";
  gcode.enabled = true;

  // Receive code contents from packet
  for (u32 i = 0; i < m_sync_gecko_codes_count; i++)
  {
    Gecko::GeckoCode::Code new_code;
    packet >> new_code.address;
    packet >> new_code.data;

    INFO_LOG_FMT(NETPLAY, "Received {:08x} {:08x}", new_code.address, new_code.data);

    gcode.codes.push_back(std::move(new_code));

    if (++m_sync_gecko_codes_success_count >= m_sync_gecko_codes_count)
    {
      m_sync_gecko_codes_complete = true;
      SyncCodeResponse(true);
    }
  }

  // Add gcode containing all codes to Gecko Code vector
  synced_codes.push_back(std::move(gcode));

  // Clear Vector if received 0 codes (match host's end when using no codes)
  if (m_sync_gecko_codes_count == 0)
    synced_codes.clear();

  // Copy this to the vector located in GeckoCode.cpp
  Gecko::UpdateSyncedCodes(synced_codes);
}

void NetPlayClient::OnSyncCodesNotifyAR(sf::Packet& packet)
{
  // Return if this is the host
  if (m_local_player->IsHost())
    return;

  // Receive Number of Codelines to Receive
  packet >> m_sync_ar_codes_count;

  m_sync_ar_codes_success_count = 0;

  INFO_LOG_FMT(NETPLAY, "Receiving {} AR codelines", m_sync_ar_codes_count);

  // Check if no codes to sync, if so return as finished
  if (m_sync_ar_codes_count == 0)
  {
    m_sync_ar_codes_complete = true;
    SyncCodeResponse(true);
  }
  else
  {
    m_dialog->AppendChat(Common::GetStringT("Synchronizing AR codes..."));
  }
}

void NetPlayClient::OnSyncCodesDataAR(sf::Packet& packet)
{
  // Return if this is the host
  if (m_local_player->IsHost())
    return;

  std::vector<ActionReplay::ARCode> synced_codes;
  synced_codes.reserve(m_sync_ar_codes_count);

  ActionReplay::ARCode arcode{};
  arcode.name = "Synced Codes";
  arcode.enabled = true;

  // Receive code contents from packet
  for (u32 i = 0; i < m_sync_ar_codes_count; i++)
  {
    ActionReplay::AREntry new_code;
    packet >> new_code.cmd_addr;
    packet >> new_code.value;

    INFO_LOG_FMT(NETPLAY, "Received {:08x} {:08x}", new_code.cmd_addr, new_code.value);
    arcode.ops.push_back(new_code);

    if (++m_sync_ar_codes_success_count >= m_sync_ar_codes_count)
    {
      m_sync_ar_codes_complete = true;
      SyncCodeResponse(true);
    }
  }

  // Add arcode containing all codes to AR Code vector
  synced_codes.push_back(std::move(arcode));

  // Clear Vector if received 0 codes (match host's end when using no codes)
  if (m_sync_ar_codes_count == 0)
    synced_codes.clear();

  // Copy this to the vector located in ActionReplay.cpp
  ActionReplay::UpdateSyncedCodes(synced_codes);
}

void NetPlayClient::OnComputeGameDigest(sf::Packet& packet)
{
  SyncIdentifier sync_identifier;
  ReceiveSyncIdentifier(packet, sync_identifier);

  ComputeGameDigest(sync_identifier);
}

void NetPlayClient::OnGameDigestProgress(sf::Packet& packet)
{
  PlayerId pid;
  int progress;
  packet >> pid;
  packet >> progress;

  m_dialog->SetGameDigestProgress(pid, progress);
}

void NetPlayClient::OnGameDigestResult(sf::Packet& packet)
{
  PlayerId pid;
  std::string result;
  packet >> pid;
  packet >> result;

  m_dialog->SetGameDigestResult(pid, result);
}

void NetPlayClient::OnGameDigestError(sf::Packet& packet)
{
  PlayerId pid;
  std::string error;
  packet >> pid;
  packet >> error;

  m_dialog->SetGameDigestResult(pid, error);
}

void NetPlayClient::OnGameDigestAbort()
{
  m_should_compute_game_digest = false;
  m_dialog->AbortGameDigest();
}

void NetPlayClient::OnFrameAck(sf::Packet& packet)
{
  int frame;
  u8 playerIdx;

  packet >> frame;
  packet >> playerIdx;

  Brawlback::FrameAck frameAck;
  frameAck.frame = frame;
  frameAck.playerIdx = playerIdx;

  int local_player_port = -1;
  for (int i = 0; i < m_pad_map.size(); i++)
  {
    if (m_pad_map.at(i) == m_local_player->pid)
      local_player_port = i;
  }

  if (playerIdx != local_player_port)  // should be local player
    ERROR_LOG_FMT(BRAWLBACK, "FrameAck playeridx is not local player idx! (This is wrong...)\n");
  else
    time_sync->ProcessFrameAck(&frameAck);
}

void NetPlayClient::Send(const sf::Packet& packet, const u8 channel_id, _ENetPacketFlag flag)
{
  Common::ENet::SendPacket(m_server, packet, channel_id, flag);
}

void NetPlayClient::DisplayPlayersPing()
{
  if (!Config::Get(Config::GFX_SHOW_NETPLAY_PING))
    return;

  OSD::AddTypedMessage(OSD::MessageType::NetPlayPing, fmt::format("Ping: {}", GetPlayersMaxPing()),
                       OSD::Duration::SHORT, OSD::Color::CYAN);
}

u32 NetPlayClient::GetPlayersMaxPing() const
{
  return std::ranges::max_element(m_players, {}, [](const auto& kv) { return kv.second.ping; })
      ->second.ping;
}

u32 NetPlayClient::CalculatePingVariance()
{
  std::lock_guard lkp(m_crit.players);

  std::vector<u32> pings;
  for (const auto& [pid, player] : m_players)
  {
    if (pid != m_local_player->pid)
      pings.push_back(player.ping);
  }

  if (pings.empty())
    return 0;

  u32 mean = std::accumulate(pings.begin(), pings.end(), 0u) / (u32)pings.size();
  u32 variance = 0;

  for (u32 ping : pings)
  {
    variance += (ping > mean) ? (ping - mean) : (mean - ping);
  }

  return variance / (u32)pings.size();
}

void NetPlayClient::Disconnect()
{
  ENetEvent netEvent;
  m_connecting = false;
  m_connection_state = ConnectionState::Failure;
  if (m_server)
    enet_peer_disconnect(m_server, 0);
  else
    return;

  while (enet_host_service(m_client, &netEvent, 3000) > 0)
  {
    switch (netEvent.type)
    {
    case ENET_EVENT_TYPE_RECEIVE:
      enet_packet_destroy(netEvent.packet);
      break;
    case ENET_EVENT_TYPE_DISCONNECT:
      m_server = nullptr;
      return;
    default:
      break;
    }
  }
  // didn't disconnect gracefully force disconnect
  enet_peer_reset(m_server);
  m_server = nullptr;
}

void NetPlayClient::SendAsync(sf::Packet&& packet, const u8 channel_id, _ENetPacketFlag flag)
{
  {
    std::lock_guard lkq(m_crit.async_queue_write);
    m_async_queue.Push(AsyncQueueEntry{std::move(packet), channel_id, flag});
  }
  Common::ENet::WakeupThread(m_client);
}

// called from ---NETPLAY--- thread
void NetPlayClient::ThreadFunc()
{
  INFO_LOG_FMT(NETPLAY, "NetPlayClient starting.");

  Common::QoSSession qos_session;
  if (Config::Get(Config::NETPLAY_ENABLE_QOS))
  {
    qos_session = Common::QoSSession(m_server);

    if (qos_session.Successful())
    {
      m_dialog->AppendChat(
          Common::GetStringT("Quality of Service (QoS) was successfully enabled.\nBuffer should be set to your ping divided by 16, at a minimum of 2."));
    }
    else
    {
      m_dialog->AppendChat(Common::GetStringT("Quality of Service (QoS) couldn't be enabled."));
    }
  }

  while (m_do_loop.IsSet())
  {
    ENetEvent netEvent;
    int net;
    if (m_traversal_client)
      m_traversal_client->HandleResends();
    net = enet_host_service(m_client, &netEvent, 250);
    while (!m_async_queue.Empty())
    {
      INFO_LOG_FMT(NETPLAY, "Processing async queue event.");
      {
        auto& e = m_async_queue.Front();
        Send(e.packet, e.channel_id, e.flag);
      }
      INFO_LOG_FMT(NETPLAY, "Processing async queue event done.");
      m_async_queue.Pop();
    }
    if (net > 0)
    {
      sf::Packet rpac;
      switch (netEvent.type)
      {
      case ENET_EVENT_TYPE_CONNECT:
        INFO_LOG_FMT(NETPLAY, "enet_host_service: connect event");
        break;
      case ENET_EVENT_TYPE_RECEIVE:
        INFO_LOG_FMT(NETPLAY, "enet_host_service: receive event");

        rpac.append(netEvent.packet->data, netEvent.packet->dataLength);
        OnData(rpac);

        enet_packet_destroy(netEvent.packet);
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        INFO_LOG_FMT(NETPLAY, "enet_host_service: disconnect event");

        m_dialog->OnConnectionLost();

        if (m_is_running.IsSet())
          StopGame();

        break;
      default:
        // not a valid switch case due to not technically being part of the enum
        if (static_cast<int>(netEvent.type) == Common::ENet::SKIPPABLE_EVENT)
          INFO_LOG_FMT(NETPLAY, "enet_host_service: skippable packet event");
        else
          ERROR_LOG_FMT(NETPLAY, "enet_host_service: unknown event type: {}", int(netEvent.type));
        break;
      }
    }
    else if (net == 0)
    {
      INFO_LOG_FMT(NETPLAY, "enet_host_service: no event occurred");
    }
    else
    {
      ERROR_LOG_FMT(NETPLAY, "enet_host_service error: {}", net);
    }
  }

  INFO_LOG_FMT(NETPLAY, "NetPlayClient shutting down.");

  Disconnect();
  return;
}

// called from ---GUI--- thread
std::vector<const Player*> NetPlayClient::GetPlayers()
{
  std::lock_guard lkp(m_crit.players);
  std::vector<const Player*> players;

  for (const auto& pair : m_players)
    players.push_back(&pair.second);

  return players;
}

const NetSettings& NetPlayClient::GetNetSettings() const
{
  return m_net_settings;
}

// called from ---GUI--- thread
void NetPlayClient::SendChatMessage(const std::string& msg)
{
  sf::Packet packet;
  packet << MessageID::ChatMessage;
  packet << msg;

  SendAsync(std::move(packet));
}

// called from ---CPU--- thread
void NetPlayClient::AddPadStateToPacket(const int in_game_pad, const GCPadStatus& pad,
                                        sf::Packet& packet)
{
  packet << static_cast<PadIndex>(in_game_pad);
  packet << pad.button;
  if (!m_gba_config[in_game_pad].enabled)
  {
    packet << pad.analogA << pad.analogB << pad.stickX << pad.stickY << pad.substickX
           << pad.substickY << pad.triggerLeft << pad.triggerRight << pad.isConnected;
  }
}

// called from ---CPU--- thread
void NetPlayClient::AddWiimoteStateToPacket(int in_game_pad,
                                            const WiimoteEmu::SerializedWiimoteState& state,
                                            sf::Packet& packet)
{
  packet << static_cast<PadIndex>(in_game_pad);
  packet << state.length;
  for (size_t i = 0; i < state.length; ++i)
    packet << state.data[i];
}

// called from ---GUI--- thread
void NetPlayClient::SendStartGamePacket()
{
  sf::Packet packet;
  packet << MessageID::StartGame;
  packet << m_current_game;

  SendAsync(std::move(packet));
}

// called from ---GUI--- thread
void NetPlayClient::SendStopGamePacket()
{
  sf::Packet packet;
  packet << MessageID::StopGame;

  SendAsync(std::move(packet));
}

// called from ---GUI--- thread
bool NetPlayClient::StartGame(const std::string& path)
{
  std::lock_guard lkg(m_crit.game);
  SendStartGamePacket();

  if (m_is_running.IsSet())
  {
    PanicAlertFmtT("Game is already running!");
    return false;
  }

  m_timebase_frame = 0;
  m_current_golfer = 1;
  m_wait_on_input = false;

  m_is_running.Set();
  NetPlay_Enable(this);

  ClearBuffers();

  m_first_pad_status_received.fill(false);

  if (m_dialog->IsRecording())
  {
    auto& movie = Core::System::GetInstance().GetMovie();
    if (movie.IsReadOnly())
      movie.SetReadOnly(false);

    Movie::ControllerTypeArray controllers{};
    Movie::WiimoteEnabledArray wiimotes{};
    for (unsigned int i = 0; i < 4; ++i)
    {
      if (m_pad_map[i] > 0 && m_gba_config[i].enabled)
        controllers[i] = Movie::ControllerType::GBA;
      else if (m_pad_map[i] > 0)
        controllers[i] = Movie::ControllerType::GC;
      else
        controllers[i] = Movie::ControllerType::None;
      wiimotes[i] = m_wiimote_map[i] > 0;
    }
    movie.BeginRecordingInput(controllers, wiimotes);
  }

  for (unsigned int i = 0; i < 4; ++i)
  {
    Config::SetCurrent(Config::GetInfoForWiimoteSource(i),
                       m_wiimote_map[i] > 0 ? WiimoteSource::Emulated : WiimoteSource::None);
  }

  // boot game
  auto boot_session_data = std::make_unique<BootSessionData>();

  INFO_LOG_FMT(NETPLAY,
               "Setting Wii sync data: has FS {}, sync_titles = {:016x}, redirect folder = {}",
               !!m_wii_sync_fs, fmt::join(m_wii_sync_titles, ", "), m_wii_sync_redirect_folder);

  boot_session_data->SetWiiSyncData(std::move(m_wii_sync_fs), std::move(m_wii_sync_titles),
                                    std::move(m_wii_sync_redirect_folder), [] {
                                      // on emulation end clean up the Wii save sync directory --
                                      // see OnSyncSaveDataWii()
                                      const std::string wii_path = File::GetUserPath(D_USER_IDX) +
                                                                   "Wii" GC_MEMCARD_NETPLAY DIR_SEP;
                                      if (File::Exists(wii_path))
                                        File::DeleteDirRecursively(wii_path);
                                      const std::string redirect_path =
                                          File::GetUserPath(D_USER_IDX) +
                                          "Redirect" GC_MEMCARD_NETPLAY DIR_SEP;
                                      if (File::Exists(redirect_path))
                                        File::DeleteDirRecursively(redirect_path);
                                    });
  boot_session_data->SetNetplaySettings(std::make_unique<NetPlay::NetSettings>(m_net_settings));

  m_dialog->BootGame(path, std::move(boot_session_data));

  UpdateDevices();

  return true;
}

void NetPlayClient::SyncSaveDataResponse(const bool success)
{
  m_dialog->AppendChat(success ? Common::GetStringT("Data received!") :
                                 Common::GetStringT("Error processing data."));

  if (success)
  {
    if (++m_sync_save_data_success_count >= m_sync_save_data_count)
    {
      sf::Packet response_packet;
      response_packet << MessageID::SyncSaveData;
      response_packet << SyncSaveDataID::Success;

      Send(response_packet);
    }
  }
  else
  {
    sf::Packet response_packet;
    response_packet << MessageID::SyncSaveData;
    response_packet << SyncSaveDataID::Failure;

    Send(response_packet);
  }
}

void NetPlayClient::SyncCodeResponse(const bool success)
{
  // If something failed, immediately report back that code sync failed
  if (!success)
  {
    m_dialog->AppendChat(Common::GetStringT("Error processing codes."));

    sf::Packet response_packet;
    response_packet << MessageID::SyncCodes;
    response_packet << SyncCodeID::Failure;

    Send(response_packet);
    return;
  }

  // If both gecko and AR codes have completely finished transferring, report back as successful
  if (m_sync_gecko_codes_complete && m_sync_ar_codes_complete)
  {
    m_dialog->AppendChat(Common::GetStringT("Codes received!"));

    sf::Packet response_packet;
    response_packet << MessageID::SyncCodes;
    response_packet << SyncCodeID::Success;

    Send(response_packet);
  }
}

// called from ---GUI--- thread
bool NetPlayClient::ChangeGame(const std::string&)
{
  return true;
}

// called from ---NETPLAY--- thread
void NetPlayClient::UpdateDevices()
{
  u8 local_pad = 0;
  u8 pad = 0;

  auto& si = Core::System::GetInstance().GetSerialInterface();
  for (auto player_id : m_pad_map)
  {
    if (m_gba_config[pad].enabled && player_id > 0)
    {
      si.ChangeDevice(SerialInterface::SIDEVICE_GC_GBA_EMULATED, pad);
    }
    else if (player_id == m_local_player->pid)
    {
      // Use local controller types for local controllers if they are compatible
      const SerialInterface::SIDevices si_device =
          Config::Get(Config::GetInfoForSIDevice(local_pad));
      if (SerialInterface::SIDevice_IsGCController(si_device))
      {
        si.ChangeDevice(si_device, pad);

        if (si_device == SerialInterface::SIDEVICE_WIIU_ADAPTER)
        {
          GCAdapter::ResetDeviceType(local_pad);
        }
      }
      else
      {
        si.ChangeDevice(SerialInterface::SIDEVICE_GC_CONTROLLER, pad);
      }
      local_pad++;
    }
    else if (player_id > 0)
    {
      si.ChangeDevice(SerialInterface::SIDEVICE_GC_CONTROLLER, pad);
    }
    else
    {
      si.ChangeDevice(SerialInterface::SIDEVICE_NONE, pad);
    }
    pad++;
  }
}

// called from ---NETPLAY--- thread
void NetPlayClient::ClearBuffers()
{
  // clear pad buffers, Clear method isn't thread safe
  for (unsigned int i = 0; i < 4; ++i)
  {
    while (m_pad_buffer[i].Size())
      m_pad_buffer[i].Pop();

    while (m_wiimote_buffer[i].Size())
      m_wiimote_buffer[i].Pop();
  }
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnTraversalStateChanged()
{
  const Common::TraversalClient::State state = m_traversal_client->GetState();

  if (m_connection_state == ConnectionState::WaitingForTraversalClientConnection &&
      state == Common::TraversalClient::State::Connected)
  {
    m_connection_state = ConnectionState::WaitingForTraversalClientConnectReady;
    m_traversal_client->ConnectToClient(m_host_spec);
  }
  else if (m_connection_state != ConnectionState::Failure &&
           state == Common::TraversalClient::State::Failure)
  {
    Disconnect();
    m_dialog->OnTraversalError(m_traversal_client->GetFailureReason());
  }
  m_dialog->OnTraversalStateChanged(state);
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnConnectReady(ENetAddress addr)
{
  if (m_connection_state == ConnectionState::WaitingForTraversalClientConnectReady)
  {
    m_connection_state = ConnectionState::Connecting;
    enet_host_connect(m_client, &addr, CHANNEL_COUNT, 0);
  }
}

// called from ---NETPLAY--- thread
void NetPlayClient::OnConnectFailed(Common::TraversalConnectFailedReason reason)
{
  m_connecting = false;
  m_connection_state = ConnectionState::Failure;
  switch (reason)
  {
  case Common::TraversalConnectFailedReason::ClientDidntRespond:
    PanicAlertFmtT("Traversal server timed out connecting to the host");
    break;
  case Common::TraversalConnectFailedReason::ClientFailure:
    PanicAlertFmtT("Server rejected traversal attempt");
    break;
  case Common::TraversalConnectFailedReason::NoSuchClient:
    PanicAlertFmtT("Invalid host");
    break;
  default:
    PanicAlertFmtT("Unknown error {0:x}", static_cast<int>(reason));
    break;
  }
}

u64 NetPlayClient::GetInitialRTCValue() const
{
  return m_initial_rtc;
}

bool NetPlayClient::WiimoteUpdate(const std::span<WiimoteDataBatchEntry>& entries)
{
  for (const WiimoteDataBatchEntry& entry : entries)
  {
    const int local_wiimote = InGameWiimoteToLocalWiimote(entry.wiimote);
    DEBUG_LOG_FMT(NETPLAY,
                  "Entering WiimoteUpdate() with wiimote {}, local_wiimote {}, state [{:02x}]",
                  entry.wiimote, local_wiimote,
                  fmt::join(std::span(entry.state->data.data(), entry.state->length), ", "));
    if (local_wiimote < 4)
    {
      sf::Packet packet;
      packet << MessageID::WiimoteData;
      if (AddLocalWiimoteToBuffer(local_wiimote, *entry.state, packet))
        SendAsync(std::move(packet));
    }

    // Now, we either use the data pushed earlier, or wait for the
    // other clients to send it to us
    while (m_wiimote_buffer[entry.wiimote].Size() == 0)
    {
      if (!m_is_running.IsSet())
      {
        return false;
      }

      m_wii_pad_event.Wait();
    }

    m_wiimote_buffer[entry.wiimote].Pop(*entry.state);

    DEBUG_LOG_FMT(NETPLAY, "Exiting WiimoteUpdate() with wiimote {}, state [{:02x}]", entry.wiimote,
                  fmt::join(std::span(entry.state->data.data(), entry.state->length), ", "));
  }

  return true;
}

GCPadStatus NetPlayClient::GetDefaultPad(int config)
{
  GCPadStatus pad = GCPadStatus{};

  pad.stickX = pad.MAIN_STICK_CENTER_X;
  pad.stickY = pad.MAIN_STICK_CENTER_Y;
  pad.substickX = pad.C_STICK_CENTER_X;
  pad.substickY = pad.C_STICK_CENTER_Y;

  return pad;
}
    // called from ---CPU--- thread
bool NetPlayClient::GetNetPads(const int pad_nb, const bool batching, GCPadStatus* pad_status)
{
  if (m_net_settings.m_RollbackMode)
  {
    int local_player_port = -1;
    for (int i = 0; i < m_pad_map.size(); i++)
    {
      if (m_pad_map.at(i) == m_local_player->pid)
        local_player_port = i;
    }
    if (pad_nb != local_player_port)
    {
      auto isRollbackMode = [](std::vector<RingBufferInput>& inputs, bu32 locFrame, u8 playerIdx) {
        return ROLLBACK_IMPL &&                     // delay-based/rollback toggle
               locFrame > GAME_FULL_START_FRAME &&  // give the game a bit of time in delay-based
                                                    // mode to sync up
               !inputs[playerIdx].Empty() &&        // some sanity checks
               !inputs[playerIdx].Empty() && inputs[playerIdx].Size() >= MAX_ROLLBACK_FRAMES;
      };
      if (advance_frames == 0)
      {
        *pad_status = GetDefaultPad(pad_config.at(pad_nb));
      }
      else if (isRollbackMode(inputs, current_frame, pad_nb))
      {
        auto ret = FindRemoteInputs(pad_nb, current_frame);
        if (ret != std::nullopt)
        {
          *pad_status = ret.value().emu_pad;
        }
        else
        {
          *pad_status = predicted_inputs.at(pad_nb).emu_pad;
        }
      }
      else
      {
        auto ret = FindRemoteInputs(pad_nb, current_frame);
        // no rollbacks, use delay-based
        if (ret == std::nullopt)
        {
          *pad_status = GetDefaultPad(pad_config.at(pad_nb));
        }
        else
        {
          *pad_status = ret.value().emu_pad;
        }
      }
    }
    else
    {
      auto ret = FindRemoteInputs(pad_nb, current_frame);
      if (ret != std::nullopt)
      {
        *pad_status = ret.value().emu_pad;
      }
      else
      {
        *pad_status = GetDefaultPad(pad_config.at(pad_nb));
      }
    }
  }
  else
  {
    // The interface for this is extremely silly.
    //
    // Imagine a physical device that links three GameCubes together
    // and emulates NetPlay that way. Which GameCube controls which
    // in-game controllers can be configured on the device (m_pad_map)
    // but which sockets on each individual GameCube should be used
    // to control which players? The solution that Dolphin uses is
    // that we hardcode the knowledge that they go in order, so if
    // you have a 3P game with three GameCubes, then every single
    // controller should be plugged into slot 1.
    //
    // If you have a 4P game, then one of the GameCubes will have
    // a controller plugged into slot 1, and another in slot 2.
    //
    // The slot number is the "local" pad number, and what player
    // it actually means is the "in-game" pad number.

    // When the 1st in-game pad is polled and batching is set, the
    // others will be polled as well. To reduce latency, we poll all
    // local controllers at once and then send the status to the other
    // clients.
    //
    // Batching is enabled when polled from VI. If batching is not
    // enabled, the poll is probably from MMIO, which can poll any
    // specific pad arbitrarily. In this case, we poll just that pad
    // and send it.

    // When here when told to so we don't deadlock in certain situations
    while (m_wait_on_input)
    {
      if (!m_is_running.IsSet())
      {
        return false;
      }

      if (m_wait_on_input_received)
      {
        // Tell the server we've acknowledged the message
        sf::Packet spac;
        spac << MessageID::GolfPrepare;
        Send(spac);

        m_wait_on_input_received = false;
      }

      m_wait_on_input_event.Wait();
    }

    if (IsFirstInGamePad(pad_nb) && batching)
    {
      sf::Packet packet;
      packet << MessageID::PadData;

      bool send_packet = false;
      const int num_local_pads = NumLocalPads();
      for (int local_pad = 0; local_pad < num_local_pads; local_pad++)
      {
        send_packet = PollLocalPad(local_pad, packet) || send_packet;
      }

      if (send_packet)
        SendAsync(std::move(packet));

      if (m_host_input_authority)
        SendPadHostPoll(-1);
    }

    if (!batching)
    {
      const int local_pad = InGamePadToLocalPad(pad_nb);
      if (local_pad < 4)
      {
        sf::Packet packet;
        packet << MessageID::PadData;
        if (PollLocalPad(local_pad, packet))
          SendAsync(std::move(packet));
      }

      if (m_host_input_authority)
        SendPadHostPoll(pad_nb);
    }

    if (m_host_input_authority)
    {
      if (m_local_player->pid != m_current_golfer)
      {
        // CoreTiming acts funny and causes what looks like frame skip if
        // we toggle the emulation speed too quickly, so to prevent this
        // we wait until the buffer has been over for at least 1 second.

        const bool buffer_over_target = m_pad_buffer[pad_nb].Size() > m_minimum_buffer_size + 1;
        if (!buffer_over_target)
          m_buffer_under_target_last = std::chrono::steady_clock::now();

        std::chrono::duration<double> time_diff =
            std::chrono::steady_clock::now() - m_buffer_under_target_last;
        if (time_diff.count() >= 1.0 || !buffer_over_target)
        {
          // run fast if the buffer is overfilled, otherwise run normal speed
          Config::SetCurrent(Config::MAIN_EMULATION_SPEED, buffer_over_target ? 0.0f : 1.0f);
        }
      }
      else
      {
        // Set normal speed when we're the host, otherwise it can get stuck at unlimited
        Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 1.0f);
      }
    }

    // Now, we either use the data pushed earlier, or wait for the
    // other clients to send it to us
    while (m_pad_buffer[pad_nb].Size() == 0)
    {
      if (!m_is_running.IsSet())
      {
        return false;
      }

      m_gc_pad_event.Wait();
    }

    m_pad_buffer[pad_nb].Pop(*pad_status);
  }
  auto& movie = Core::System::GetInstance().GetMovie();
  if (movie.IsRecordingInput())
  {
    movie.RecordInput(pad_status, pad_nb);
    movie.InputUpdate();
  }
  else
  {
    movie.CheckPadStatus(pad_status, pad_nb);
  }

  return true;
}

bool NetPlayClient::AddLocalWiimoteToBuffer(const int local_wiimote,
                                            const WiimoteEmu::SerializedWiimoteState& state,
                                            sf::Packet& packet)
{
  const int ingame_pad = LocalWiimoteToInGameWiimote(local_wiimote);
  bool data_added = false;

  // adjust the buffer either up or down
  // inserting multiple padstates or dropping states
  while (m_wiimote_buffer[ingame_pad].Size() <= m_minimum_buffer_size)
  {
    // add to buffer
    m_wiimote_buffer[ingame_pad].Push(state);

    // add to packet
    AddWiimoteStateToPacket(ingame_pad, state, packet);
    data_added = true;
  }

  return data_added;
}

bool NetPlayClient::PollLocalPad(const int local_pad, sf::Packet& packet)
{
  const int ingame_pad = LocalPadToInGamePad(local_pad);
  bool data_added = false;
  GCPadStatus pad_status;

  if (m_gba_config[ingame_pad].enabled)
  {
    pad_status = Pad::GetGBAStatus(local_pad);
  }
  else if (Config::Get(Config::GetInfoForSIDevice(local_pad)) ==
           SerialInterface::SIDEVICE_WIIU_ADAPTER)
  {
    pad_status = GCAdapter::Input(local_pad);
  }
  else
  {
    pad_status = Pad::GetStatus(local_pad);
  }

  if (!m_net_settings.m_RollbackMode)
  {
    if (m_host_input_authority)
    {
      if (m_local_player->pid != m_current_golfer)
      {
        // add to packet
        AddPadStateToPacket(ingame_pad, pad_status, packet);
        data_added = true;
      }
      else
      {
        // set locally
        m_last_pad_status[ingame_pad] = pad_status;
        m_first_pad_status_received[ingame_pad] = true;
      }
    }
    else
    {
      // adjust the buffer either up or down
      // inserting multiple padstates or dropping states
      while (m_pad_buffer[ingame_pad].Size() <= m_minimum_buffer_size)
      {
        // add to buffer
        m_pad_buffer[ingame_pad].Push(pad_status);

        // add to packet
        AddPadStateToPacket(ingame_pad, pad_status, packet);
        data_added = true;
      }
    }
  }

  return data_added;
}

void NetPlayClient::SendPadHostPoll(const PadIndex pad_num)
{
  // Here we handle polling for the Host Input Authority and Golf modes. Pad data is "polled" from
  // the most recent data received for the given pad. Passing pad_num < 0 will poll all assigned
  // pads (used for batched polls), while 0..3 will poll the respective pad (used for MMIO polls).
  // See GetNetPads for more details.
  //
  // If the local buffer is non-empty, we skip actually buffering and sending new pad data, this way
  // don't end up with permanent local latency. It does create a period of time where no inputs are
  // accepted, but under typical circumstances this is not noticeable.
  //
  // Additionally, we wait until some actual pad data has been received before buffering and sending
  // it, otherwise controllers get calibrated wrongly with the default values of GCPadStatus.

  if (m_local_player->pid != m_current_golfer)
    return;

  sf::Packet packet;
  packet << MessageID::PadHostData;

  if (pad_num < 0)
  {
    for (size_t i = 0; i < m_pad_map.size(); i++)
    {
      if (m_pad_map[i] <= 0)
        continue;

      while (!m_first_pad_status_received[i])
      {
        if (!m_is_running.IsSet())
          return;

        m_first_pad_status_received_event.Wait();
      }
    }

    for (size_t i = 0; i < m_pad_map.size(); i++)
    {
      if (m_pad_map[i] == 0 || m_pad_buffer[i].Size() > 0)
        continue;

      const GCPadStatus& pad_status = m_last_pad_status[i];
      m_pad_buffer[i].Push(pad_status);
      AddPadStateToPacket(static_cast<int>(i), pad_status, packet);
    }
  }
  else if (m_pad_map[pad_num] != 0)
  {
    while (!m_first_pad_status_received[pad_num])
    {
      if (!m_is_running.IsSet())
        return;

      m_first_pad_status_received_event.Wait();
    }

    if (m_pad_buffer[pad_num].Size() == 0)
    {
      const GCPadStatus& pad_status = m_last_pad_status[pad_num];
      m_pad_buffer[pad_num].Push(pad_status);
      AddPadStateToPacket(pad_num, pad_status, packet);
    }
  }

  SendAsync(std::move(packet));
}

void NetPlayClient::InvokeStop()
{
  m_is_running.Clear();

  // stop waiting for input
  m_gc_pad_event.Set();
  m_wii_pad_event.Set();
  m_first_pad_status_received_event.Set();
  m_wait_on_input_event.Set();

  std::ofstream json_stream;
  auto t = std::time(nullptr);
  auto tm = *std::localtime(&t);
  std::ostringstream oss;
  oss << std::put_time(&tm, "%d-%m-%Y_%H-%M-%S");
  auto timestamp = oss.str();
  std::string cur_directory = File::GetExeDirectory();
  File::OpenFStream(json_stream, cur_directory + DIR_SEP "replay_" + timestamp + ".json",
                    std::ios_base::out);
  if (json_stream.is_open())
  {
    json_stream << inputs_output;
    json_stream.close();
  }
}

// called from ---GUI--- thread and ---NETPLAY--- thread (client side)
bool NetPlayClient::StopGame()
{
  InvokeStop();

  NetPlay_Disable();

  // stop game
  m_dialog->StopGame();

  return true;
}

// called from ---GUI--- thread
void NetPlayClient::Stop()
{
  
  if (!m_is_running.IsSet())
    return;

  InvokeStop();

  // Tell the server to stop if we have a pad mapped in game.
  if (LocalPlayerHasControllerMapped())
    SendStopGamePacket();
  else
    StopGame();
}

void NetPlayClient::RequestStopGame()
{
  // Tell the server to stop if we have a pad mapped in game.
  if (LocalPlayerHasControllerMapped())
    SendStopGamePacket();
}

void NetPlayClient::SendPowerButtonEvent()
{
  sf::Packet packet;
  packet << MessageID::PowerButton;
  SendAsync(std::move(packet));
}

void NetPlayClient::RequestGolfControl(const PlayerId pid)
{
  if (!m_host_input_authority || !m_net_settings.golf_mode)
    return;

  sf::Packet packet;
  packet << MessageID::GolfRequest;
  packet << pid;
  SendAsync(std::move(packet));
}

void NetPlayClient::RequestGolfControl()
{
  RequestGolfControl(m_local_player->pid);
}

// called from ---GUI--- thread
std::string NetPlayClient::GetCurrentGolfer()
{
  std::lock_guard lkp(m_crit.players);
  if (const auto it = m_players.find(m_current_golfer); it != m_players.end())
    return it->second.name;
  return "";
}

// called from ---GUI--- thread
bool NetPlayClient::LocalPlayerHasControllerMapped() const
{
  return PlayerHasControllerMapped(m_local_player->pid);
}

bool NetPlayClient::IsFirstInGamePad(int ingame_pad) const
{
  return std::none_of(m_pad_map.begin(), m_pad_map.begin() + ingame_pad,
                      [](auto mapping) { return mapping > 0; });
}

int NetPlayClient::NumLocalPads() const
{
  return std::ranges::count(m_pad_map, m_local_player->pid);
}

int NetPlayClient::NumLocalWiimotes() const
{
  return std::ranges::count(m_wiimote_map, m_local_player->pid);
}

static int InGameToLocal(int ingame_pad, const PadMappingArray& pad_map, PlayerId local_player_pid)
{
  // not our pad
  if (pad_map[ingame_pad] != local_player_pid)
    return 4;

  int local_pad = 0;
  int pad = 0;

  for (; pad < ingame_pad; ++pad)
  {
    if (pad_map[pad] == local_player_pid)
      local_pad++;
  }

  return local_pad;
}

static int LocalToInGame(int local_pad, const PadMappingArray& pad_map, PlayerId local_player_pid)
{
  // Figure out which in-game pad maps to which local pad.
  // The logic we have here is that the local slots always
  // go in order.
  int local_pad_count = -1;
  int ingame_pad = 0;
  for (; ingame_pad < 4; ingame_pad++)
  {
    if (pad_map[ingame_pad] == local_player_pid)
      local_pad_count++;

    if (local_pad_count == local_pad)
      break;
  }

  return ingame_pad;
}

int NetPlayClient::InGamePadToLocalPad(int ingame_pad) const
{
  return InGameToLocal(ingame_pad, m_pad_map, m_local_player->pid);
}

int NetPlayClient::LocalPadToInGamePad(int local_pad) const
{
  return LocalToInGame(local_pad, m_pad_map, m_local_player->pid);
}

int NetPlayClient::InGameWiimoteToLocalWiimote(int ingame_wiimote) const
{
  return InGameToLocal(ingame_wiimote, m_wiimote_map, m_local_player->pid);
}

int NetPlayClient::LocalWiimoteToInGameWiimote(int local_wiimote) const
{
  return LocalToInGame(local_wiimote, m_wiimote_map, m_local_player->pid);
}

bool NetPlayClient::PlayerHasControllerMapped(const PlayerId pid) const
{
  const auto mapping_matches_player_id = [pid](const PlayerId& mapping) { return mapping == pid; };

  return std::ranges::any_of(m_pad_map, mapping_matches_player_id) ||
         std::ranges::any_of(m_wiimote_map, mapping_matches_player_id);
}

bool NetPlayClient::IsLocalPlayer(const PlayerId pid) const
{
  return pid == m_local_player->pid;
}

const PlayerId& NetPlayClient::GetLocalPlayerId() const
{
  return m_local_player->pid;
}

void NetPlayClient::SendGameStatus()
{
  sf::Packet packet;
  packet << MessageID::GameStatus;

  SyncIdentifierComparison result;
  m_dialog->FindGameFile(m_selected_game, &result);
  for (size_t i = 0; i < 4; ++i)
  {
    if (m_gba_config[i].enabled && m_gba_config[i].has_rom &&
        m_net_settings.gba_rom_paths[i].empty())
    {
      result = SyncIdentifierComparison::DifferentGame;
    }
  }

  packet << static_cast<u32>(result);
  Send(packet);
}

void NetPlayClient::SendTimeBase()
{
  std::lock_guard lk(crit_netplay_client);

  if (netplay_client->m_timebase_frame % 60 == 0)
  {
    const u64 timebase = Core::System::GetInstance().GetSystemTimers().GetFakeTimeBase();

    sf::Packet packet;
    packet << MessageID::TimeBase;
    packet << timebase;
    packet << netplay_client->m_timebase_frame;

    netplay_client->SendAsync(std::move(packet));
  }

  netplay_client->m_timebase_frame++;
}

bool NetPlayClient::DoAllPlayersHaveGame()
{
  std::lock_guard lkp(m_crit.players);

  return std::ranges::all_of(m_players, [](const auto& entry) {
    return entry.second.game_status == SyncIdentifierComparison::SameGame;
  });
}

static std::string SHA1Sum(const std::string& file_path, std::function<bool(int)> report_progress)
{
  std::vector<u8> data(8 * 1024 * 1024);
  u64 read_offset = 0;

  std::unique_ptr<DiscIO::BlobReader> file(DiscIO::CreateBlobReader(file_path));
  u64 game_size = file->GetDataSize();

  auto ctx = Common::SHA1::CreateContext();

  while (read_offset < game_size)
  {
    size_t read_size = std::min(static_cast<u64>(data.size()), game_size - read_offset);
    if (!file->Read(read_offset, read_size, data.data()))
      return "";

    ctx->Update(data.data(), read_size);
    read_offset += read_size;

    int progress =
        static_cast<int>(static_cast<float>(read_offset) / static_cast<float>(game_size) * 100);
    if (!report_progress(progress))
      return "";
  }

  // Convert to hex
  return fmt::format("{:02x}", fmt::join(ctx->Finish(), ""));
}

void NetPlayClient::ComputeGameDigest(const SyncIdentifier& sync_identifier)
{
  if (m_should_compute_game_digest)
    return;

  m_dialog->ShowGameDigestDialog(sync_identifier.game_id);
  m_should_compute_game_digest = true;

  std::string file;
  if (sync_identifier == GetSDCardIdentifier())
    file = File::GetUserPath(F_WIISDCARDIMAGE_IDX);
  else if (auto game = m_dialog->FindGameFile(sync_identifier))
    file = game->GetFilePath();
  else if (sync_identifier == GetBrawlFileIdentifier())
    file = File::GetSysDirectory() + "Wii" + DIR_SEP + "title" + DIR_SEP + "00010000" + DIR_SEP + "52534245" + DIR_SEP + "data" + DIR_SEP + BRAWL_SAVE_FILE;

  if (file.empty() || !File::Exists(file))
  {
    sf::Packet packet;
    packet << MessageID::GameDigestError;
    packet << "file not found";
    Send(packet);
    return;
  }

  if (m_game_digest_thread.joinable())
    m_game_digest_thread.join();
  m_game_digest_thread = std::thread([this, file] {
    std::string sum = SHA1Sum(file, [&](int progress) {
      sf::Packet packet;
      packet << MessageID::GameDigestProgress;
      packet << progress;
      SendAsync(std::move(packet));

      return m_should_compute_game_digest;
    });

    sf::Packet packet;
    packet << MessageID::GameDigestResult;
    packet << sum;
    SendAsync(std::move(packet));
  });
}

void NetPlayClient::NotifyDesync(int player_index, s32 frame)
{
  // Check if we've notified about this player recently
  auto now = std::chrono::steady_clock::now();
  auto time_since_last =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_desync_notification)
          .count();

  if (time_since_last < DESYNC_NOTIFICATION_COOLDOWN_MS)
  {
    return;  // Don't spam notifications
  }

  // Check if this is the same desync we already reported
  if (m_last_desync_frame.count(player_index) &&
      std::abs(m_last_desync_frame[player_index] - frame) < 60)  // Within 1 second
  {
    return;
  }

  m_last_desync_frame[player_index] = frame;
  m_last_desync_notification = now;

  // Send desync notification to server
  sf::Packet packet;
  packet << MessageID::DesyncDetected;
  packet << static_cast<int>(player_index);
  packet << frame;

  SendAsync(std::move(packet));

  // Show local notification
  std::string player_name = GetPlayerName(player_index);
  std::string message =
      fmt::format("Desync detected with player {} at frame {}", player_name, frame);

  INFO_LOG_FMT(NETPLAY, "{}", message);

  // Show on-screen display
  OSD::AddTypedMessage(OSD::MessageType::NetPlayBuffer, message, OSD::Duration::VERY_LONG,
                       OSD::Color::RED);
}

std::string NetPlayClient::GetPlayerName(int player_index)
{
  std::lock_guard lkp(m_crit.players);

  // Find the player ID for this port
  if (player_index >= 0 && player_index < m_pad_map.size())
  {
    PlayerId pid = m_pad_map[player_index];
    auto it = m_players.find(pid);
    if (it != m_players.end())
    {
      return it->second.name;
    }
  }

  return fmt::format("Player {}", player_index);
}

const PadMappingArray& NetPlayClient::GetPadMapping() const
{
  return m_pad_map;
}

const GBAConfigArray& NetPlayClient::GetGBAConfig() const
{
  return m_gba_config;
}

const PadMappingArray& NetPlayClient::GetWiimoteMapping() const
{
  return m_wiimote_map;
}

void NetPlayClient::SetWiiSyncData(std::unique_ptr<IOS::HLE::FS::FileSystem> fs,
                                   std::vector<u64> titles, std::string redirect_folder)
{
  m_wii_sync_fs = std::move(fs);
  m_wii_sync_titles = std::move(titles);
  m_wii_sync_redirect_folder = std::move(redirect_folder);
}

SyncIdentifier NetPlayClient::GetSDCardIdentifier()
{
  return SyncIdentifier{{}, "sd", {}, {}, {}, {}};
}

SyncIdentifier NetPlayClient::GetBrawlFileIdentifier()
{
  return SyncIdentifier{{}, "Save", {}, {}, {}, {}};
}

std::string GetPlayerMappingString(PlayerId pid, const PadMappingArray& pad_map,
                                   const GBAConfigArray& gba_config,
                                   const PadMappingArray& wiimote_map)
{
  std::vector<size_t> gc_slots, gba_slots, wiimote_slots;
  for (size_t i = 0; i < pad_map.size(); ++i)
  {
    if (pad_map[i] == pid && !gba_config[i].enabled)
      gc_slots.push_back(i + 1);
    if (pad_map[i] == pid && gba_config[i].enabled)
      gba_slots.push_back(i + 1);
    if (wiimote_map[i] == pid)
      wiimote_slots.push_back(i + 1);
  }
  std::vector<std::string> groups;
  std::array<std::pair<std::string, std::vector<size_t>*>, 3> slot_groups = {
      {{"GC", &gc_slots}, {"GBA", &gba_slots}, {"Wii", &wiimote_slots}}};

  for (const auto& [group_name, slots] : slot_groups)
  {
    if (!slots->empty())
      groups.emplace_back(fmt::format("{}{}", group_name, fmt::join(*slots, ",")));
  }
  std::string res = fmt::format("{}", fmt::join(groups, "|"));
  return res.empty() ? "None" : res;
}

bool IsNetPlayRunning()
{
  return netplay_client != nullptr;
}

void SetSIPollBatching(bool state)
{
  s_si_poll_batching = state;
}

void SendPowerButtonEvent()
{
  ASSERT(IsNetPlayRunning());
  netplay_client->SendPowerButtonEvent();
}

std::string GetGBASavePath(int pad_num)
{
  std::lock_guard lk(crit_netplay_client);

  if (!netplay_client || netplay_client->GetNetSettings().is_hosting)
  {
#ifdef HAS_LIBMGBA
    std::string rom_path = Config::Get(Config::MAIN_GBA_ROM_PATHS[pad_num]);
    return HW::GBA::Core::GetSavePath(rom_path, pad_num);
#else
    return {};
#endif
  }

  if (!netplay_client->GetNetSettings().savedata_load)
    return {};

  return fmt::format("{}{}{}.sav", File::GetUserPath(D_GBAUSER_IDX), GBA_SAVE_NETPLAY, pad_num + 1);
}

PadDetails GetPadDetails(int pad_num)
{
  std::lock_guard lk(crit_netplay_client);

  PadDetails res{};
  res.local_pad = 4;
  if (!netplay_client)
    return res;

  auto pad_map = netplay_client->GetPadMapping();
  if (pad_map[pad_num] <= 0)
    return res;

  for (auto player : netplay_client->GetPlayers())
  {
    if (player->pid == pad_map[pad_num])
      res.player_name = player->name;
  }

  int local_pad = 0;
  int non_local_pad = 0;
  for (int i = 0; i < pad_num; ++i)
  {
    if (netplay_client->IsLocalPlayer(pad_map[i]))
      ++local_pad;
    else
      ++non_local_pad;
  }
  res.is_local = netplay_client->IsLocalPlayer(pad_map[pad_num]);
  res.local_pad = res.is_local ? local_pad : netplay_client->NumLocalPads() + non_local_pad;
  res.hide_gba = !res.is_local && netplay_client->GetNetSettings().hide_remote_gbas &&
                 netplay_client->LocalPlayerHasControllerMapped();
  return res;
}

int NumLocalWiimotes()
{
  std::lock_guard lk(crit_netplay_client);
  if (netplay_client)
    return netplay_client->NumLocalWiimotes();
  return 0;
}

void NetPlay_Enable(NetPlayClient* const np)
{
  std::lock_guard lk(crit_netplay_client);
  netplay_client = np;
}

void NetPlay_Disable()
{
  std::lock_guard lk(crit_netplay_client);
  netplay_client = nullptr;
}

void OnFrameEnd()
{
  INFO_LOG_FMT(BRAWLBACK, "-- End of Frame {} --",
               netplay_client ? netplay_client->current_frame : -1);
  if (IsNetPlayRunning() && netplay_client)
  {
    if (netplay_client->IsInRollbackMode() && game_started)
    {
      if (is_rollingback)
      {
        is_rollingback = false;
        INFO_LOG_FMT(BRAWLBACK, "Rollback complete at frame {}", netplay_client->current_frame);
      }

      if (!is_stalled)
      {
        netplay_client->current_frame++;
        INFO_LOG_FMT(BRAWLBACK, "Advanced to frame {}", netplay_client->current_frame);
      }
      else
      {
        INFO_LOG_FMT(BRAWLBACK, "Frame not advanced (stalled at {})",
                     netplay_client->current_frame);
      }
    }
  }
}

void OnFrameStart(BrawlbackPad& pad)
{
  INFO_LOG_FMT(BRAWLBACK, "-- Start of Frame --");
  if (IsNetPlayRunning() && netplay_client)
  {
    if (netplay_client->IsInRollbackMode())
    {
      netplay_client->OnFrameStart(pad);
      game_started = true;
    }
  }
}
bool IsStarted()
{
  if (netplay_client)
  {
    return netplay_client->IsStarted();
  }
  else
  {
    return false;
  }
}
bool IsRollingBack()
{
  if (netplay_client)
  {
    return netplay_client->IsRollingBack();
  }
  else
  {
    return false;
  }
}

bool IsInRollbackMode()
{
  if (netplay_client)
  {
    return netplay_client->IsInRollbackMode();
  }
  else
  {
    return false;
  }
}

s32 CurrentFrame()
{
  if (netplay_client)
  {
    return netplay_client->current_frame;
  }
  else
  {
    return 0;
  }
}

s32 StopFrame()
{
  return netplay_client->frame_to_stop_at;
}

s32 AdvanceFrames()
{
  if (netplay_client)
  {
    return netplay_client->advance_frames;
  }
  else
  {
    return 1;
  }
}

void IncrementCurrentFrame()
{
  netplay_client->current_frame++;
}

size_t FramesSize()
{
  return netplay_client->GetInputsSize();
}

}  // namespace NetPlay

// stuff hacked into dolphin

// called from ---CPU--- thread
// Actual Core function which is called on every frame
bool SerialInterface::CSIDevice_GCController::NetPlay_GetInput(int pad_num, GCPadStatus* status)
{
  std::lock_guard lk(NetPlay::crit_netplay_client);

  if (NetPlay::netplay_client)
    return NetPlay::netplay_client->GetNetPads(pad_num, NetPlay::s_si_poll_batching, status);

  return false;
}

bool NetPlay::NetPlay_GetWiimoteData(const std::span<NetPlayClient::WiimoteDataBatchEntry>& entries)
{
  std::lock_guard lk(crit_netplay_client);

  if (netplay_client)
    return netplay_client->WiimoteUpdate(entries);

  return false;
}

unsigned int NetPlay::NetPlay_GetLocalWiimoteForSlot(unsigned int slot)
{
  if (slot >= std::tuple_size_v<PadMappingArray>)
    return slot;

  std::lock_guard lk(crit_netplay_client);

  if (!netplay_client)
    return slot;

  const auto& mapping = netplay_client->GetWiimoteMapping();
  const auto& local_player_id = netplay_client->GetLocalPlayerId();

  std::array<unsigned int, std::tuple_size_v<std::decay_t<decltype(mapping)>>> slot_map;
  size_t player_count = 0;
  for (size_t i = 0; i < mapping.size(); ++i)
  {
    if (mapping[i] == local_player_id)
    {
      slot_map[i] = static_cast<unsigned int>(player_count);
      ++player_count;
    }
  }
  for (size_t i = 0; i < mapping.size(); ++i)
  {
    if (mapping[i] != local_player_id)
    {
      slot_map[i] = static_cast<unsigned int>(player_count);
      ++player_count;
    }
  }

  INFO_LOG_FMT(NETPLAY, "Wiimote slot map: [{}]", fmt::join(slot_map, ", "));

  return slot_map[slot];
}

// called from ---CPU--- thread
// so all players' games get the same time
//
// also called from ---GUI--- thread when starting input recording
u64 ExpansionInterface::CEXIIPL::NetPlay_GetEmulatedTime()
{
  std::lock_guard lk(NetPlay::crit_netplay_client);

  if (NetPlay::netplay_client)
    return NetPlay::netplay_client->GetInitialRTCValue();

  return 0;
}

// called from ---CPU--- thread
// return the local pad num that should rumble given a ingame pad num
int SerialInterface::CSIDevice_GCController::NetPlay_InGamePadToLocalPad(int numPAD)
{
  std::lock_guard lk(NetPlay::crit_netplay_client);

  if (NetPlay::netplay_client)
    return NetPlay::netplay_client->InGamePadToLocalPad(numPAD);

  return numPAD;
}

