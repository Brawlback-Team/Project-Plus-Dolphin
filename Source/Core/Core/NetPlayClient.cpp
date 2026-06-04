// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#define GEKKONET_STATIC
#include "Core/NetPlayClient.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
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
#include "Core/HW/Memmap.h"
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
#include "Brawlback/TimeSync.h"
#include "brawlback-common/BrawlbackPad.h"
#include "Core/Brawlback/include/GekkoNet/GekkoLib/include/gekkonet.h"

namespace NetPlay
{
using namespace WiimoteCommon;

// Forward declarations for GekkoNet integration
void QueueGekkoPacket(const u8* data, size_t length, PlayerId from_player);

struct CoreRollbackRunFrameStats
{
  long long totalUs = 0;
  long long r4300Us = 0;
  long long viUs = 0;
  long long newFrameUs = 0;
  long long cheatsUs = 0;
  long long pacingUs = 0;
  long long inputUs = 0;
  long long pauseUs = 0;
  long long netplayUs = 0;
  int dynarecRecompileCount = 0;
  long long dynarecRecompileUs = 0;
  long long dynarecInvalidateUs = 0;
  int dynarecFullInvalidateCount = 0;
  int dynarecRangeInvalidateCount = 0;
  int dynarecBlockInvalidateCount = 0;
  int dynarecVerifyDirtyCount = 0;
  long long dynarecVerifyDirtyUs = 0;
  int dynarecGetAddrCount = 0;
  long long dynarecGetAddrUs = 0;
  int dynarecGetAddrHtCount = 0;
  int dynarecGetAddr32Count = 0;
  int dynarecDynamicLinkerCount = 0;
  long long dynarecDynamicLinkerUs = 0;
  int dynarecDynamicLinkerDsCount = 0;
  long long dynarecDynamicLinkerDsUs = 0;
  int cachedCodeFullInvalidateCount = 0;
  int cachedCodeRangeInvalidateCount = 0;
  int interruptCount = 0;
  long long interruptUs = 0;
  long long interruptMaxUs = 0;
  int interruptMaxType = 0;
  int interruptViCount = 0;
  long long interruptViUs = 0;
  int interruptCompareCount = 0;
  long long interruptCompareUs = 0;
  int interruptCheckCount = 0;
  long long interruptCheckUs = 0;
  int interruptSiCount = 0;
  long long interruptSiUs = 0;
  int interruptPiCount = 0;
  long long interruptPiUs = 0;
  int interruptAiCount = 0;
  long long interruptAiUs = 0;
  int interruptSpCount = 0;
  long long interruptSpUs = 0;
  int interruptDpCount = 0;
  long long interruptDpUs = 0;
  int interruptRspDmaCount = 0;
  long long interruptRspDmaUs = 0;
  int interruptRspTaskCount = 0;
  long long interruptRspTaskUs = 0;
  int aiSetFrequencyCount = 0;
  long long aiSetFrequencyUs = 0;
  int aiPushSamplesCount = 0;
  long long aiPushSamplesUs = 0;
  int aiFifoPopCount = 0;
  long long aiFifoPopUs = 0;
  int aiRaiseInterruptCount = 0;
  long long aiRaiseInterruptUs = 0;
  int emumode = 0;
  u32 cp0CountBefore = 0;
  u32 cp0CountAfter = 0;
  u32 nextInterruptBefore = 0;
  u32 nextInterruptAfter = 0;
  u32 pcBefore = 0;
  u32 pcAfter = 0;
  u32 dynarecPcaddrBefore = 0;
  u32 dynarecPcaddrAfter = 0;
  u32 cp0LastAddrBefore = 0;
  u32 cp0LastAddrAfter = 0;
  int dynarecCycleCountBefore = 0;
  int dynarecCycleCountAfter = 0;
  int dynarecPendingExceptionBefore = 0;
  int dynarecPendingExceptionAfter = 0;
  int dynarecStopBefore = 0;
  int dynarecStopAfter = 0;
  int delaySlotBefore = 0;
  int delaySlotAfter = 0;
  int currentFrameBefore = 0;
  int currentFrameAfter = 0;
  int outputFlags = 0;
};


static std::mutex crit_netplay_client;
NetPlayClient* netplay_client = nullptr;
constexpr unsigned int kGekkoStateCapacity = 24u * 1024u * 1024u;
constexpr int kGekkoMaxLoggedFrames = 600;
constexpr int kGekkoWaitSleepUs = 100;
constexpr float kGekkoTimesyncDeadzone = 0.5f;
constexpr double kGekkoTimesyncStrength = 0.002;
constexpr double kGekkoTimesyncMinScale = 0.99;
constexpr double kGekkoTimesyncMaxScale = 1.01;
constexpr double kGekkoTimesyncLerp = 0.15;
// Sample gekko_frames_ahead() this often when recomputing the target
// emulation speed. Mirrors Slippi's SLIPPI_ONLINE_LOCKSTEP_INTERVAL (30
// frames @ 60Hz = once per ~500ms) — single-frame jitter spikes no
// longer kick the speed scale around; the lerp keeps speed_scale
// converging toward the cached target on every frame in between.
constexpr int kGekkoTimesyncIntervalFrames = 30;
constexpr size_t kGekkoClientReplayFrames = 600;
GekkoSession* g_GekkoSession = nullptr;
GekkoNetAdapter* g_GekkoNetAdapter = nullptr;
std::vector<GekkoNetResult*> g_GekkoNetResults;
int g_GekkoPlayers = 0;
int g_GekkoInputSize = 0;
int g_GekkoLocalPlayer = 0;
int g_GekkoLocalHandle = -1;
int g_GekkoRemoteHandle = -1;
std::vector<int> g_GekkoPlayerHandles;
std::vector<int> g_GekkoLocalHandles;
std::vector<unsigned char> g_GekkoLatchedInput;
bool g_GekkoHasLatchedInput = false;
// Per-frame input buffer for rollback-aware krec recording. Buffered until
// each frame ages past the rollback window so that rolling-back re-sims can
// overwrite the initial speculative entry with the corrected input before
// it gets committed to the .krec file.
std::map<int, std::vector<unsigned char>> g_GekkoFrameInputBuffer;
int g_GekkoMaxObservedFrame = -1;
constexpr int kGekkoRecordingRollbackHorizon = 32;
std::atomic_bool g_GekkoExecuting{false};
std::atomic_bool g_GekkoStopRequested{false};
std::vector<PendingGekkoSave> g_GekkoPendingSaves;
std::mutex g_GekkoLogMutex;
std::filesystem::path g_GekkoLogDirectory;
std::string g_GekkoLogPrefix;
int g_GekkoLogFrames = 0;
Inputs g_GekkoLastSubmittedInput = Inputs{};
long long g_GekkoLastLoadStateUs = 0;
long long g_GekkoLastSaveStateUs = 0;
long long g_GekkoLastRunFrameUs = 0;
long long g_GekkoLastPendingSaveUs = 0;
void* g_GekkoDebugUserData = nullptr;
int g_GekkoDebugFrameOutput = -1;
std::vector<unsigned char> g_GekkoLastLatchedInput;
int g_GekkoWaitingLoops = 0;
int g_GekkoLocalInputLogRepeats = 0;
int g_GekkoPacingLogFrames = 0;
double g_GekkoSpeedScale = 1.0;
// Ring buffer: g_Inputs[player_port][frame % g_GekkoInputRingSize] = GCPadStatus
// g_BrawlbackInputs[player_port][frame % g_GekkoInputRingSize] = BrawlbackPad
// Size is set to (input_prediction_window + 1) at session start.
std::vector<std::vector<Inputs>> g_Inputs;
int g_GekkoInputRingSize = 0;
// Timesync sample state. Counter wraps every kGekkoTimesyncIntervalFrames
// to trigger a fresh frames_ahead sample. TargetScale is what g_GekkoSpeedScale
// lerps toward between samples.
int g_GekkoTimesyncSampleCounter = 0;
double g_GekkoTimesyncTargetScale = 1.0;
bool g_GekkoLogEnabled = true;
bool g_GekkoPreserveLogOnNextReset = false;
std::mutex g_GekkoClientReplayMutex;
std::vector<uint32_t> g_GekkoClientReplayInputs;
size_t g_GekkoClientReplayIndex = 0;

// Custom GekkoNetAdapter implementation that integrates with ENet
struct ENetPacketData
{
  GekkoNetAddress addr;
  std::vector<u8> data;
};

static std::mutex g_GekkoPacketMutex;
static std::vector<ENetPacketData> g_GekkoIncomingPackets;
static std::vector<GekkoNetResult*> g_GekkoPacketResults;

// GekkoNetAdapter callbacks
static void gekko_enet_send_data(GekkoNetAddress* addr, const char* data, int length)
{
  if (!netplay_client)
    return;

  sf::Packet packet;
  packet << MessageID::GekkoNetData;

  // Append the raw gekko data
  packet.append(data, length);

  // Send via ENet on the game channel
  netplay_client->SendAsync(std::move(packet), DEFAULT_CHANNEL, ENET_PACKET_FLAG_UNSEQUENCED);
}

static GekkoNetResult** gekko_enet_receive_data(int* length)
{
  std::lock_guard<std::mutex> lock(g_GekkoPacketMutex);

  // Clear previous results
  for (auto* result : g_GekkoNetResults)
  {
    if (result)
    {
      // Free the address data if it was allocated
      if (result->addr.data)
      {
        delete static_cast<PlayerId*>(result->addr.data);
        result->addr.data = nullptr;
      }
      // Free the packet data if it was allocated
      if (result->data)
      {
        delete[] result->data;
        result->data = nullptr;
      }
      // Free the result structure itself
      delete result;
    }
  }
  g_GekkoPacketResults.clear();

  // Convert incoming packets to GekkoNetResult array
  for (const auto& packet_data : g_GekkoIncomingPackets)
  {
    auto* result = new GekkoNetResult();

    // Deep copy the addr.data to avoid sharing pointers
    if (packet_data.addr.data && packet_data.addr.size == sizeof(PlayerId))
    {
      auto* player_id_copy = new PlayerId(*static_cast<PlayerId*>(packet_data.addr.data));
      result->addr.data = player_id_copy;
      result->addr.size = sizeof(PlayerId);
    }
    else
    {
      result->addr.data = nullptr;
      result->addr.size = 0;
    }

    result->data_len = static_cast<unsigned int>(packet_data.data.size());

    // Allocate and copy data
    auto* data_copy = new u8[packet_data.data.size()];
    std::memcpy(data_copy, packet_data.data.data(), packet_data.data.size());
    result->data = data_copy;

    g_GekkoPacketResults.push_back(result);
  }

  g_GekkoIncomingPackets.clear();

  *length = static_cast<int>(g_GekkoPacketResults.size());
  return g_GekkoPacketResults.empty() ? nullptr : g_GekkoPacketResults.data();
}

static void gekko_enet_free_data(void* data_ptr)
{
  if (data_ptr)
    delete[] static_cast<u8*>(data_ptr);
}

// Function to queue incoming gekko packets from ENet
void QueueGekkoPacket(const u8* data, size_t length, PlayerId from_player)
{
  std::lock_guard<std::mutex> lock(g_GekkoPacketMutex);

  ENetPacketData packet_data;

  // Create address from player ID - allocate persistent memory for the address
  auto* player_id_ptr = new PlayerId(from_player);
  packet_data.addr.data = player_id_ptr;
  packet_data.addr.size = sizeof(PlayerId);

  // Copy packet data
  packet_data.data.assign(data, data + length);

  g_GekkoIncomingPackets.push_back(std::move(packet_data));
}

// Initialize the custom ENet adapter
static GekkoNetAdapter g_ENetGekkoAdapter = {
  .send_data = gekko_enet_send_data,
  .receive_data = gekko_enet_receive_data,
  .free_data = gekko_enet_free_data
};

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

// Helper function to format input as hexadecimal string
static std::string hex_input(uint32_t value)
{
  std::ostringstream stream;
  stream << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
  return stream.str();
}

// called from ---GUI--- thread
NetPlayClient::~NetPlayClient()
{
  netplay_client = nullptr;  // Clear global pointer

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
  netplay_client = this;  // Set global pointer for GekkoNet adapter
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

bool NetPlayClient::load_gekko_state(GekkoGameEvent* event)
{
  const auto beginTime = std::chrono::steady_clock::now();
  CoreRollbackState state;
  state.buffer = event->data.load.state;
  state.len = static_cast<int>(event->data.load.state_len);
  state.frame = event->data.load.frame;
  state.checksum = 0;  // Checksum will be computed during load for verification

  if (!RollbackLoadGameState(state))
  {
    g_GekkoLastLoadStateUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - beginTime)
                                 .count();
    std::ostringstream stream;
    stream << "load_state result=fail elapsed_us=" << g_GekkoLastLoadStateUs;
    write_gekko_log(stream.str());
    return false;
  }
  g_GekkoLastLoadStateUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - beginTime)
                               .count();

  if (g_GekkoLogEnabled &&
      (g_GekkoLogFrames < kGekkoMaxLoggedFrames || g_GekkoLastLoadStateUs >= 2000))
  {
    std::ostringstream stream;
    stream << "load_state result=ok frame=" << event->data.load.frame
           << " len=" << event->data.load.state_len << " elapsed_us=" << g_GekkoLastLoadStateUs;
    write_gekko_log(stream.str());
  }
  current_frame = event->data.load.frame;
  return true;
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
  case MessageID::GekkoNetData:
    OnGekkoNetData(packet);
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

void NetPlayClient::OnGekkoNetData(sf::Packet& packet)
{
  // Extract raw GekkoNet data from packet
  const void* packet_data = static_cast<const u8*>(packet.getData()) + sizeof(MessageID);
  size_t packet_size = packet.getDataSize() - sizeof(MessageID);

  if (packet_data && packet_size > 0)
  {
    // Identify the remote sender: in a 2-player session it is the only player
    // whose PID differs from ours. Fall back to 1 if we can't determine it.
    PlayerId sender_pid = 1;
    {
      std::lock_guard lkp(m_crit.players);
      for (const auto& [pid, player] : m_players)
      {
        if (pid != m_local_player->pid)
        {
          sender_pid = pid;
          break;
        }
      }
    }
    QueueGekkoPacket(static_cast<const u8*>(packet_data), packet_size, sender_pid);
  }
}

void NetPlayClient::OnPadData(sf::Packet& packet)
{
  while (!packet.endOfPacket())
  {
    PadIndex map;
    packet >> map;

    if (!m_net_settings.m_RollbackMode)
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
  {
    INFO_LOG_FMT(BRAWLBACK, 
                 "SendRollbackVerification skipped - RollbackMode: {} | Started: {}",
                 IsInRollbackMode(), IsStarted());
    return;
  }

  auto verify_start = std::chrono::high_resolution_clock::now();

  sf::Packet packet;
  packet << MessageID::RollbackVerification;
  packet << frame;
  packet << hash;

  netplay_client->SendAsync(std::move(packet));

  auto verify_duration = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::high_resolution_clock::now() - verify_start);

  INFO_LOG_FMT(BRAWLBACK, 
               "ROLLBACK VERIFICATION SENT - Frame: {} | Hash: {:#018x} | SendTime: {}µs",
               frame, hash, verify_duration.count());
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
  FromJson(j, i.game_pad);
  FromJson(j, i.emu_pad);
}
void NetPlayClient::CloseSession()
{
  g_GekkoStopRequested.store(false, std::memory_order_relaxed);
  g_GekkoExecuting.store(false, std::memory_order_relaxed);

  if (g_GekkoSession != nullptr)
  {
    gekko_destroy(&g_GekkoSession);
    gekko_default_adapter_destroy();
  }

  {
    std::lock_guard<std::mutex> lock(g_GekkoPacketMutex);
    // Free g_GekkoPacketResults (allocated in gekko_enet_receive_data)
    for (auto* result : g_GekkoPacketResults)
    {
      if (result)
      {
        if (result->addr.data)
          delete static_cast<PlayerId*>(result->addr.data);
        if (result->data)
          delete[] result->data;
        delete result;
      }
    }
    g_GekkoPacketResults.clear();
    g_GekkoNetResults.clear();
    // Free address data still held in pending incoming packets
    for (auto& pkt : g_GekkoIncomingPackets)
    {
      if (pkt.addr.data)
        delete static_cast<PlayerId*>(pkt.addr.data);
    }
    g_GekkoIncomingPackets.clear();
  }

  g_GekkoSession = nullptr;
  g_GekkoNetAdapter = nullptr;
  g_GekkoPlayers = 0;
  g_GekkoInputSize = 0;
  g_GekkoLocalPlayer = 0;
  g_GekkoLocalHandle = -1;
  g_GekkoRemoteHandle = -1;
  g_GekkoPlayerHandles.clear();
  g_GekkoLocalHandles.clear();
  g_GekkoInputRingSize = 0;
  g_Inputs.clear();
  g_GekkoLatchedInput.clear();
  g_GekkoLastLatchedInput.clear();
  g_GekkoHasLatchedInput = false;
  g_GekkoPendingSaves.clear();
  g_GekkoFrameInputBuffer.clear();
  g_GekkoMaxObservedFrame = -1;
  g_GekkoLastSubmittedInput = Inputs{};
  g_GekkoWaitingLoops = 0;
  g_GekkoLocalInputLogRepeats = 0;
  g_GekkoPacingLogFrames = 0;
  g_GekkoLogFrames = 0;
  g_GekkoSpeedScale = 1.0;
  g_GekkoTimesyncTargetScale = 1.0;
  g_GekkoTimesyncSampleCounter = 0;
  g_GekkoClientReplayInputs.clear();
  g_GekkoClientReplayIndex = 0;
  g_GekkoLastLoadStateUs = 0;
  g_GekkoLastSaveStateUs = 0;
  g_GekkoLastRunFrameUs = 0;
  g_GekkoLastPendingSaveUs = 0;
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
    // Ensure any previous session is fully torn down before creating a new one.
    if (g_GekkoSession != nullptr)
      CloseSession();

    gekko_create(&g_GekkoSession, GekkoGameSession);
    GekkoConfig config = {};
    const int clampedLocalDelay = std::clamp(delay, 0, 10);
    const int clampedPredictionWindow = std::clamp(7, 1, 10);
    g_GekkoLocalPlayer = static_cast<int>(m_local_player->pid) - 1;
    config.num_players = static_cast<unsigned char>(2);
    config.max_spectators = 0;
    config.input_prediction_window = static_cast<unsigned char>(clampedPredictionWindow);
    config.input_size = static_cast<unsigned int>(sizeof(Inputs));
    config.state_size = kGekkoStateCapacity;
    config.limited_saving = false;
    config.desync_detection = true;
    config.check_distance = 10;
    gekko_start(g_GekkoSession, &config);

    // Use our custom ENet-based adapter
    g_GekkoNetAdapter = &g_ENetGekkoAdapter;
    gekko_net_adapter_set(g_GekkoSession, g_GekkoNetAdapter);

    gekko_set_runahead(g_GekkoSession, 0);
    g_GekkoPlayers = 2;
    g_GekkoInputSize = sizeof(Inputs);
    g_GekkoLocalHandle = -1;
    g_GekkoRemoteHandle = -1;
    g_GekkoPlayerHandles.assign(static_cast<size_t>(2), -1);
    g_GekkoLocalHandles.assign(static_cast<size_t>(2), -1);
    g_GekkoInputRingSize = clampedPredictionWindow + 1;
    g_Inputs.assign(static_cast<size_t>(2),
                    std::vector<Inputs>(static_cast<size_t>(g_GekkoInputRingSize)));
    g_GekkoHasLatchedInput = false;
    g_GekkoFrameInputBuffer.clear();
    g_GekkoMaxObservedFrame = -1;
    g_GekkoPendingSaves.clear();
    pad_config.clear();

    for (auto player : m_players)
    {
      if (player.first == m_local_player->pid)
      {
        const int handle = gekko_add_actor(g_GekkoSession, GekkoLocalPlayer, nullptr);
        if (handle < 0)
        {
          write_gekko_log("gekko_add_actor result=fail type=local");
          return;
        }
        g_GekkoLocalHandle = handle;
        g_GekkoPlayerHandles[static_cast<size_t>(player.first - 1)] = handle;
        g_GekkoLocalHandles[static_cast<size_t>(player.first - 1)] = handle;
        gekko_set_local_delay(g_GekkoSession, handle,
                              static_cast<unsigned char>(clampedLocalDelay));
        if (g_GekkoLogEnabled)
        {
          std::ostringstream stream;
          stream << "gekko_add_actor result=ok player=" << player.first
                 << " type=local handle=" << handle;
          write_gekko_log(stream.str());
        }
      }
      else
      {
        pad_config.push_back(12);

        auto* remote_player_id = new PlayerId(player.first);
        GekkoNetAddress remote_address = {};
        remote_address.data = remote_player_id;
        remote_address.size = sizeof(PlayerId);

        const int handle = gekko_add_actor(g_GekkoSession, GekkoRemotePlayer, &remote_address);
        if (handle < 0)
        {
          delete remote_player_id;
          write_gekko_log("gekko_add_actor result=fail type=remote");
          return;
        }
        if (g_GekkoRemoteHandle < 0)
        {
          g_GekkoRemoteHandle = handle;
        }
        g_GekkoPlayerHandles[static_cast<size_t>(player.first - 1)] = handle;
        if (g_GekkoLogEnabled)
        {
          std::ostringstream stream;
          stream << "gekko_add_actor result=ok player=" << player.first
                 << " type=remote handle=" << handle << " player_id=" << player.first;
          write_gekko_log(stream.str());
        }
      }
      std::string port = fmt::format("{}_{}", "player_port", player.first);
      inputs_output[port] = json::array();
    }
    current_frame = 0;
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
void NetPlayClient::apply_gekko_frame_pacing()
{
  // Read frames_ahead every call (cheap, just a member access in
  // GekkoSession) but only recompute the target scale once per
  // sampling interval. The per-frame lerp below carries the speed scale
  // toward the cached target between samples.
  const float framesAhead = gekko_frames_ahead(g_GekkoSession);
  const bool isSampleFrame = (g_GekkoTimesyncSampleCounter % kGekkoTimesyncIntervalFrames) == 0;
  if (isSampleFrame)
  {
    double newTarget = 1.0;
    if (framesAhead >= kGekkoTimesyncDeadzone || framesAhead <= -kGekkoTimesyncDeadzone)
    {
      newTarget = 1.0 - (static_cast<double>(framesAhead) * kGekkoTimesyncStrength);
      newTarget = std::clamp(newTarget, kGekkoTimesyncMinScale, kGekkoTimesyncMaxScale);
    }
    g_GekkoTimesyncTargetScale = newTarget;
  }
  g_GekkoTimesyncSampleCounter++;

  g_GekkoSpeedScale += (g_GekkoTimesyncTargetScale - g_GekkoSpeedScale) * kGekkoTimesyncLerp;
  Config::SetCurrent(Config::MAIN_EMULATION_SPEED, g_GekkoSpeedScale);

  g_GekkoPacingLogFrames++;
  if (g_GekkoLogEnabled && (g_GekkoTimesyncTargetScale != 1.0 || g_GekkoPacingLogFrames <= 10 ||
                            (g_GekkoPacingLogFrames % 60) == 0))
  {
    std::ostringstream stream;
    stream << "pacing frames_ahead=" << std::fixed << std::setprecision(2) << framesAhead
           << " sample=" << (isSampleFrame ? 1 : 0) << " target_scale=" << std::setprecision(4)
           << g_GekkoTimesyncTargetScale << " speed_scale=" << g_GekkoSpeedScale;

    if (g_GekkoRemoteHandle >= 0)
    {
      GekkoNetworkStats stats = {};
      gekko_network_stats(g_GekkoSession, g_GekkoRemoteHandle, &stats);
      stream << " remote_handle=" << g_GekkoRemoteHandle << " ping_ms=" << stats.last_ping
             << " avg_ping_ms=" << std::setprecision(1) << stats.avg_ping
             << " jitter_ms=" << stats.jitter << " kb_sent=" << stats.kb_sent
             << " kb_recv=" << stats.kb_received;
    }

    INFO_LOG_FMT(BRAWLBACK, "{}", stream.str());
  }
}

bool NetPlayClient::submit_local_input(Inputs* input, size_t playerIndex)
{
  bool submitted = false;
  const int handle =
      playerIndex < g_GekkoLocalHandles.size() ? g_GekkoLocalHandles[playerIndex] : -1;
  if (handle < 0)
  {
    return false;
  }

  if (input == nullptr)
  {
    return false;
  }

  gekko_add_local_input(g_GekkoSession, handle, std::move((void*)input));
  submitted = true;

  const bool changed =
      nullptr && memcmp(input, &g_GekkoLastSubmittedInput, g_GekkoInputSize) != 0;
  if (changed)
  {
    g_GekkoLocalInputLogRepeats = 0;
  }
  else
  {
    g_GekkoLocalInputLogRepeats++;
  }

  if (g_GekkoLogEnabled &&
      (changed || g_GekkoLocalInputLogRepeats <= 20 || (g_GekkoLocalInputLogRepeats % 60) == 0))
  {
    std::ostringstream stream;
    stream << "add_local_input local_player=" << playerIndex << " handle=" << handle << " physical_p"
            << 1 << "=" << hex_input(1)
            << " repeat=" << g_GekkoLocalInputLogRepeats;
    INFO_LOG_FMT(BRAWLBACK, "{}", stream.str());
  }
  g_GekkoLastSubmittedInput = *input;
  return submitted;
}
void NetPlayClient::set_environment_value(const char* name, const std::string& value)
{
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  setenv(name, value.c_str(), 1);
#endif
}
std::string NetPlayClient::make_rollback_log_prefix()
{
  static unsigned int sessionCounter = 0;

  const auto now = std::chrono::system_clock::now();
  const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
  const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) %
                     std::chrono::seconds(1);
  std::tm localTime = {};

#ifdef _WIN32
  localtime_s(&localTime, &nowTime);
#else
  localtime_r(&nowTime, &localTime);
#endif

  std::ostringstream stream;
  stream << "rollback_" << std::put_time(&localTime, "%Y%m%d_%H%M%S") << "_" << std::setw(3)
         << std::setfill('0') << nowMs.count() << "_" << ++sessionCounter;
  return stream.str();
}
std::filesystem::path NetPlayClient::create_rollback_log_directory()
{
  std::error_code errorCode;
  std::filesystem::path directory = File::GetUserPath(D_DUMP_IDX) + "Rollback" + DIR_SEP;

  if (std::filesystem::is_directory(directory, errorCode) ||
      std::filesystem::create_directories(directory, errorCode))
  {
    return directory.make_preferred();
  }

  errorCode.clear();
  directory = "Logs";
  if (std::filesystem::is_directory(directory, errorCode) ||
      std::filesystem::create_directories(directory, errorCode))
  {
    return directory.make_preferred();
  }

  return std::filesystem::path();
}
void NetPlayClient::reset_rollback_log_session()
{
  g_GekkoLogDirectory = create_rollback_log_directory();
  g_GekkoLogPrefix = make_rollback_log_prefix();

  set_environment_value("RMGK_ROLLBACK_LOG_DIR", g_GekkoLogDirectory.string());
  set_environment_value("RMGK_ROLLBACK_LOG_PREFIX", g_GekkoLogPrefix);
}
std::filesystem::path NetPlayClient::get_gekko_log_path()
{
  if (g_GekkoLogPrefix.empty())
  {
    reset_rollback_log_session();
  }

  std::string filename = g_GekkoLogPrefix;
  filename += g_GekkoLocalPlayer == 2 ? "_gekko_client.log" : "_gekko_host.log";

  if (!g_GekkoLogDirectory.empty())
  {
    return g_GekkoLogDirectory / filename;
  }

  return std::filesystem::path(filename);
}
void NetPlayClient::write_gekko_log(const std::string& message)
{
  if (!g_GekkoLogEnabled)
  {
    return;
  }

  std::lock_guard<std::mutex> lock(g_GekkoLogMutex);
  const std::filesystem::path path = get_gekko_log_path();
  std::ofstream file(path, std::ios::out | std::ios::app);
  file << "core_frame=" << current_frame << " " << message << "\n";
  INFO_LOG_FMT(BRAWLBACK, "{} -- {}\n", current_frame, message);
}
const char* NetPlayClient::gekko_session_event_name(GekkoSessionEventType type)
{
  switch (type)
  {
  case GekkoPlayerSyncing:
    return "player_syncing";
  case GekkoPlayerConnected:
    return "player_connected";
  case GekkoPlayerDisconnected:
    return "player_disconnected";
  case GekkoSessionStarted:
    return "session_started";
  case GekkoSpectatorPaused:
    return "spectator_paused";
  case GekkoSpectatorUnpaused:
    return "spectator_unpaused";
  case GekkoDesyncDetected:
    return "desync_detected";
  default:
    return "unknown";
  }
}
void NetPlayClient::log_session_events()
{
  if (!g_GekkoLogEnabled)
  {
    return;
  }

  int count = 0;
  GekkoSessionEvent** events = gekko_session_events(g_GekkoSession, &count);
  for (int i = 0; i < count; i++)
  {
    GekkoSessionEvent* event = events[i];
    if (event == nullptr)
    {
      continue;
    }

    std::ostringstream stream;
    stream << "event name=" << gekko_session_event_name(event->type);
    switch (event->type)
    {
    case GekkoPlayerSyncing:
      stream << " handle=" << event->data.syncing.handle
             << " count=" << static_cast<int>(event->data.syncing.current)
             << " total=" << static_cast<int>(event->data.syncing.max);
      break;
    case GekkoPlayerConnected:
      stream << " handle=" << event->data.connected.handle;
      break;
    case GekkoPlayerDisconnected:
      stream << " handle=" << event->data.disconnected.handle;
      break;
    case GekkoDesyncDetected:
      stream << " frame=" << event->data.desynced.frame
             << " remote_handle=" << event->data.desynced.remote_handle
             << " local_checksum=" << event->data.desynced.local_checksum
             << " remote_checksum=" << event->data.desynced.remote_checksum;
      break;
    default:
      break;
    }
    write_gekko_log(stream.str());
  }
}
const char* NetPlayClient::gekko_game_event_name(GekkoGameEventType type)
{
  switch (type)
  {
  case GekkoAdvanceEvent:
    return "advance";
  case GekkoSaveEvent:
    return "save";
  case GekkoLoadEvent:
    return "load";
  default:
    return "unknown";
  }
}
bool NetPlayClient::save_gekko_state(const PendingGekkoSave& save)
{
  const auto beginTime = std::chrono::steady_clock::now();
  CoreRollbackState state;
  const int coreFrame = std::max(0, save.frame);
  if (g_GekkoLogEnabled)
  {
    std::ostringstream stream;
    stream << "save_state begin frame=" << save.frame << " core_frame=" << coreFrame
           << " state_ptr=" << static_cast<void*>(save.state)
           << " state_len_ptr=" << static_cast<void*>(save.stateLen)
           << " checksum_ptr=" << static_cast<void*>(save.checksum);
    write_gekko_log(stream.str());
  }

  if (save.state == nullptr || save.stateLen == nullptr)
  {
    write_gekko_log("save_state result=fail reason=null_event_buffer");
    return false;
  }

  if (save.frame < 0)
  {
    *save.stateLen = 0;
    if (save.checksum != nullptr)
    {
      *save.checksum = 0;
    }
    write_gekko_log("save_state result=skipped reason=pre_frame_baseline");
    return true;
  }

  if (!RollbackSaveGameStateInto(state, save.state, static_cast<int>(kGekkoStateCapacity),
                                     coreFrame))
  {
    g_GekkoLastSaveStateUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - beginTime)
                                 .count();
    std::ostringstream stream;
    stream << "save_state result=fail elapsed_us=" << g_GekkoLastSaveStateUs;
    write_gekko_log(stream.str());
    return false;
  }

  if (state.len < 1 || static_cast<unsigned int>(state.len) > kGekkoStateCapacity)
  {
    std::ostringstream stream;
    stream << "save_state result=fail reason=state_too_large len=" << state.len
           << " capacity=" << kGekkoStateCapacity;
    write_gekko_log(stream.str());
    return false;
  }

  if (state.buffer != save.state)
  {
    write_gekko_log("save_state result=fail reason=state_not_written_in_place");
    return false;
  }

  if (save.stateLen != nullptr)
  {
    *save.stateLen = static_cast<unsigned int>(state.len);
  }
  if (save.checksum != nullptr)
  {
    *save.checksum = static_cast<unsigned int>(state.checksum);
  }

  g_GekkoLastSaveStateUs = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - beginTime)
                               .count();

  if (g_GekkoLogEnabled &&
      (g_GekkoLogFrames < kGekkoMaxLoggedFrames || g_GekkoLastSaveStateUs >= 2000))
  {
    std::ostringstream stream;
    stream << "save_state result=ok frame=" << save.frame << " len=" << state.len
           << " checksum=" << static_cast<unsigned int>(state.checksum)
           << " elapsed_us=" << g_GekkoLastSaveStateUs;
    write_gekko_log(stream.str());
  }

  return true;
}

bool NetPlayClient::RollbackSaveGameStateInto(CoreRollbackState& state, unsigned char* buffer,
                                               int buffer_capacity, int frame)
{
  if (buffer == nullptr || buffer_capacity <= 0)
  {
    ERROR_LOG_FMT(BRAWLBACK, "RollbackSaveGameStateInto: invalid buffer parameters");
    return false;
  }

  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();

  // Define fixed regions that should always be saved
  struct FixedRegion
  {
    u32 address;
    u32 size;
  };

  const FixedRegion fixedRegions[] = {
      {0x800064E0, 0x6380},    // 0x800064E0 - 0x8000C860
      {0x804064E0, 0x8E360},   // 0x804064E0 - 0x80494840
      {0x80494880, 0x1108D4},  // 0x80494880 - 0x805A5154
  };

  // Define regions to exclude from saving (volatile/non-deterministic memory)
  struct ExcludedRegion
  {
    u32 address;
    u32 size;
  };

  const ExcludedRegion excludedRegions[] = {
      {0x8059FFF8, 0x00000004},  // Exclude frame counter or volatile data
      {0x8059FFFC, 0x00000004},  // Exclude volatile region
      {0x805b5030, 0x00000078},  // Exclude extended volatile region
  };

  // Helper function to check if an address range overlaps with excluded regions
  auto isExcluded = [&excludedRegions](u32 address, u32 size) -> bool {
    u32 range_end = address + size;
    for (const ExcludedRegion& excluded : excludedRegions)
    {
      u32 excluded_end = excluded.address + excluded.size;
      // Check for overlap
      if (!(range_end <= excluded.address || address >= excluded_end))
      {
        return true;
      }
    }
    return false;
  };

  // Calculate total size needed
  u32 total_size = 0;
  for (const FixedRegion& region : fixedRegions)
  {
    if (!isExcluded(region.address, region.size))
    {
      total_size += region.size;
    }
  }

  if (total_size > static_cast<u32>(buffer_capacity))
  {
    ERROR_LOG_FMT(BRAWLBACK,
                  "RollbackSaveGameStateInto: buffer too small (need {} bytes, have {} bytes)",
                  total_size, buffer_capacity);
    return false;
  }

  // Save all regions into the buffer
  u32 offset = 0;
  u64 checksum = 0;

  for (const FixedRegion& region : fixedRegions)
  {
    if (isExcluded(region.address, region.size))
    {
      continue;
    }

    u8* source_data = memory.GetSpanForAddress(region.address).data();
    if (source_data == nullptr)
    {
      ERROR_LOG_FMT(BRAWLBACK, "RollbackSaveGameStateInto: invalid source address 0x{:08X}",
                    region.address);
      return false;
    }

    // Copy the memory region
    memcpy(buffer + offset, source_data, region.size);

    // Update checksum
    for (u32 i = 0; i < region.size; ++i)
    {
      checksum = (checksum * 31) + source_data[i];
    }

    offset += region.size;
  }

  // Fill out the state structure
  state.buffer = buffer;
  state.len = static_cast<int>(offset);
  state.checksum = static_cast<int>(checksum & 0xFFFFFFFF);  // Use lower 32 bits
  state.frame = frame;

  return true;
}

bool NetPlayClient::RollbackLoadGameState(const CoreRollbackState& state)
{
  if (state.buffer == nullptr || state.len <= 0)
  {
    ERROR_LOG_FMT(BRAWLBACK, "RollbackLoadGameState: invalid buffer parameters (buffer={}, len={})",
                  static_cast<void*>(state.buffer), state.len);
    return false;
  }

  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();

  // Define the same fixed regions as in RollbackSaveGameStateInto
  struct FixedRegion
  {
    u32 address;
    u32 size;
  };

  const FixedRegion fixedRegions[] = {
      {0x800064E0, 0x6380},    // 0x800064E0 - 0x8000C860
      {0x804064E0, 0x8E360},   // 0x804064E0 - 0x80494840
      {0x80494880, 0x1108D4},  // 0x80494880 - 0x805A5154
  };

  // Define the same excluded regions
  struct ExcludedRegion
  {
    u32 address;
    u32 size;
  };

  const ExcludedRegion excludedRegions[] = {
      {0x8059FFF8, 0x00000004},  // Exclude frame counter or volatile data
      {0x8059FFFC, 0x00000004},  // Exclude volatile region
      {0x805b5030, 0x00000078},  // Exclude extended volatile region
  };

  // Helper function to check if an address range overlaps with excluded regions
  auto isExcluded = [&excludedRegions](u32 address, u32 size) -> bool {
    u32 range_end = address + size;
    for (const ExcludedRegion& excluded : excludedRegions)
    {
      u32 excluded_end = excluded.address + excluded.size;
      // Check for overlap
      if (!(range_end <= excluded.address || address >= excluded_end))
      {
        return true;
      }
    }
    return false;
  };

  // Calculate expected size
  u32 expected_size = 0;
  for (const FixedRegion& region : fixedRegions)
  {
    if (!isExcluded(region.address, region.size))
    {
      expected_size += region.size;
    }
  }

  if (static_cast<u32>(state.len) != expected_size)
  {
    ERROR_LOG_FMT(BRAWLBACK,
                  "RollbackLoadGameState: buffer size mismatch (expected {} bytes, got {} bytes)",
                  expected_size, state.len);
    return false;
  }

  // Restore all regions from the buffer
  u32 offset = 0;

  for (const FixedRegion& region : fixedRegions)
  {
    if (isExcluded(region.address, region.size))
    {
      continue;
    }

    u8* dest_data = memory.GetSpanForAddress(region.address).data();
    if (dest_data == nullptr)
    {
      ERROR_LOG_FMT(BRAWLBACK, "RollbackLoadGameState: invalid destination address 0x{:08X}",
                    region.address);
      return false;
    }

    // Copy the memory region from buffer back to emulated memory
    memcpy(dest_data, state.buffer + offset, region.size);

    offset += region.size;
  }

  INFO_LOG_FMT(BRAWLBACK, "RollbackLoadGameState: successfully loaded state for frame {} ({} bytes)",
               state.frame, state.len);

  return true;
}

void NetPlayClient::OnFrameStart(BrawlbackPad& pad)
{
  const auto beginTime = std::chrono::steady_clock::now();
  int summaryEventCount = 0;
  int summarySaveCount = 0;
  int summaryLoadCount = 0;
  int summaryRollbackAdvanceCount = 0;
  int summaryRunaheadAdvanceCount = 0;
  int summaryWaitLoops = 0;
  long long summaryNetworkPollUs = 0;
  long long summaryPacingUs = 0;
  long long summarySubmitInputUs = 0;
  long long summaryUpdateSessionUs = 0;
  long long summaryLatchInputUs = 0;
  long long summarySaveUs = 0;
  long long summaryLoadUs = 0;
  long long summaryResimUs = 0;
  long long summaryMaxResimUs = 0;
  long long summaryDebugBeginUs = 0;

  if (g_GekkoSession == nullptr)
  {
    return;
  }
  if (g_GekkoStopRequested.load(std::memory_order_relaxed))
  {
    write_gekko_log("begin_frame result=stop_requested");
    return;
  }
  gekko_network_poll(g_GekkoSession);

  // Store the local player's input into the inputs buffer so it can be sent to the remote player.
  {
    int local_player_port = m_local_player->pid - 1;
    if (local_player_port >= 0 && local_player_port < (int)g_GekkoPlayers)
    {
      // Also write into the BrawlbackPad ring buffer for EXI reads
      if (local_player_port < (int)g_Inputs.size() && g_GekkoInputRingSize > 0)
        g_Inputs[local_player_port][current_frame % g_GekkoInputRingSize].game_pad = pad;
    }
  }

  Inputs* localInputPtr =
      (g_GekkoLocalPlayer < (int)g_Inputs.size() && g_GekkoInputRingSize > 0)
          ? &g_Inputs[g_GekkoLocalPlayer][current_frame % g_GekkoInputRingSize]
          : nullptr;
  submit_local_input(localInputPtr, g_GekkoLocalPlayer);
  g_GekkoHasLatchedInput = false;
  g_GekkoPendingSaves.clear();
  {
    if (g_GekkoStopRequested.load(std::memory_order_relaxed))
    {
      write_gekko_log("begin_frame result=stop_requested");
      return;
    }

    int count = 0;
    const auto updateSessionTime = std::chrono::steady_clock::now();
    GekkoGameEvent** events = gekko_update_session(g_GekkoSession, &count);
    summaryUpdateSessionUs += std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - updateSessionTime)
                                  .count();
    log_session_events();

    if (count == 0)
    {
      g_GekkoWaitingLoops++;
      summaryWaitLoops++;
      if (g_GekkoLogEnabled && (g_GekkoWaitingLoops <= 20 || (g_GekkoWaitingLoops % 60) == 0))
      {
        std::ostringstream stream;
        stream << "update_session result=waiting loop=" << g_GekkoWaitingLoops << " events=0";
        write_gekko_log(stream.str());
      }
    }
    else if (g_GekkoLogEnabled)
    {
      std::ostringstream stream;
      stream << "update_session result=events count=" << count;
      for (int i = 0; i < count; i++)
      {
        if (events[i] != nullptr)
        {
          stream << " event" << i << "=" << gekko_game_event_name(events[i]->type);
          if (events[i]->type == GekkoAdvanceEvent)
          {
            stream << "(frame=" << events[i]->data.adv.frame
                   << ",rollback=" << (events[i]->data.adv.rolling_back ? "true" : "false")
                   << ",runahead=" << (events[i]->data.adv.running_ahead ? "true" : "false") << ")";
          }
          else if (events[i]->type == GekkoSaveEvent)
          {
            stream << "(frame=" << events[i]->data.save.frame << ")";
          }
          else if (events[i]->type == GekkoLoadEvent)
          {
            stream << "(frame=" << events[i]->data.load.frame
                   << ",len=" << events[i]->data.load.state_len << ")";
          }
        }
      }
      write_gekko_log(stream.str());
      g_GekkoWaitingLoops = 0;
    }
    else
    {
      g_GekkoWaitingLoops = 0;
    }

    bool deferSavesUntilFrameEnd = false;
    bool hasRealAdvance = false;
    summaryEventCount += count;
    advance_frames = 0;
    for (int i = 0; i < count; i++)
    {
      GekkoGameEvent* event = events[i];
      if (event == nullptr)
      {
        continue;
      }

      switch (event->type)
      {
      case GekkoSaveEvent:
      {
        summarySaveCount++;
        PendingGekkoSave save;
        save.frame = event->data.save.frame;
        save.checksum = event->data.save.checksum;
        save.stateLen = event->data.save.state_len;
        save.state = event->data.save.state;
        if (deferSavesUntilFrameEnd)
        {
          write_gekko_log("save_state result=deferred");
          g_GekkoPendingSaves.push_back(save);
        }
        else if (!save_gekko_state(save))
        {
          return;
        }
        else
        {
          summarySaveUs += g_GekkoLastSaveStateUs;
        }
        break;
      }
      case GekkoLoadEvent:
        summaryLoadCount++;
        write_gekko_log("load_state begin");
        if (!load_gekko_state(event))
        {
          return;
        }
        summaryLoadUs += g_GekkoLastLoadStateUs;
        break;
      case GekkoAdvanceEvent:
        write_gekko_log("advance_frame begin");
        advance_frames++;
        {
          const auto latchInputTime = std::chrono::steady_clock::now();

          // Extract inputs from the GekkoNet event and store them in g_Inputs
          // event->data.adv.inputs is indexed by GekkoNet handle, not player port.
          // Use g_GekkoPlayerHandles to map player port -> handle -> correct input slot.
          for (int playerPort = 0; playerPort < g_GekkoPlayers; playerPort++)
          {
            const int handle = (playerPort < (int)g_GekkoPlayerHandles.size())
                                   ? g_GekkoPlayerHandles[playerPort]
                                   : -1;
            if (handle >= 0 && handle < g_GekkoPlayers &&
                playerPort < (int)g_Inputs.size() && g_GekkoInputRingSize > 0)
            {
              g_Inputs[playerPort][event->data.adv.frame % g_GekkoInputRingSize] =
                  ((Inputs*)(event->data.adv.inputs))[handle];
            }
          }

          summaryLatchInputUs += std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now() - latchInputTime)
                                     .count();

          if (event->data.adv.rolling_back || event->data.adv.running_ahead)
          {
            if (event->data.adv.rolling_back)
            {
              summaryRollbackAdvanceCount++;
            }
            if (event->data.adv.running_ahead)
            {
              summaryRunaheadAdvanceCount++;
            }
            const auto runFrameBeginTime = std::chrono::steady_clock::now();
            // Send advance frames to EXI here
            g_GekkoLastRunFrameUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - runFrameBeginTime)
                                        .count();
            summaryResimUs += g_GekkoLastRunFrameUs;
            summaryMaxResimUs = std::max(summaryMaxResimUs, g_GekkoLastRunFrameUs);
            g_GekkoHasLatchedInput = false;
          }
          else
          {
            write_gekko_log("advance_frame result=real_frame_ready");
            hasRealAdvance = true;
            deferSavesUntilFrameEnd = true;
          }
          break;
        }
      default:
        break;
      }
    }

    if (hasRealAdvance)
    {
      if (g_GekkoLogEnabled)
      {
        const auto endTime = std::chrono::steady_clock::now();
        const auto elapsedUs =
            std::chrono::duration_cast<std::chrono::microseconds>(endTime - beginTime).count();
        if (elapsedUs >= 2000 || summaryRollbackAdvanceCount > 0 || summaryLoadCount > 0 ||
            summaryWaitLoops > 0)
        {
          std::ostringstream stream;
          stream << "frame_summary elapsed_us=" << elapsedUs << " events=" << summaryEventCount
                 << " saves=" << summarySaveCount << " loads=" << summaryLoadCount
                 << " rollback_advances=" << summaryRollbackAdvanceCount
                 << " runahead_advances=" << summaryRunaheadAdvanceCount
                 << " wait_loops=" << summaryWaitLoops << " debug_begin_us=" << summaryDebugBeginUs
                 << " network_poll_us=" << summaryNetworkPollUs << " pacing_us=" << summaryPacingUs
                 << " submit_input_us=" << summarySubmitInputUs
                 << " update_session_us=" << summaryUpdateSessionUs
                 << " latch_input_us=" << summaryLatchInputUs << " save_total_us=" << summarySaveUs
                 << " load_total_us=" << summaryLoadUs << " resim_total_us=" << summaryResimUs
                 << " resim_max_us=" << summaryMaxResimUs
                 << " last_load_us=" << g_GekkoLastLoadStateUs
                 << " last_save_us=" << g_GekkoLastSaveStateUs
                 << " last_run_frame_us=" << g_GekkoLastRunFrameUs
                 << " pending_save_us=" << g_GekkoLastPendingSaveUs
                 << " frames_ahead=" << std::fixed << std::setprecision(2)
                 << gekko_frames_ahead(g_GekkoSession);
          write_gekko_log(stream.str());
        }
      }
      write_gekko_log("begin_frame result=real_frame");
    }
  }
}
bool NetPlayClient::IsRollingBack()
{
  return is_rollingback.load();
}

GCPadStatus NetPlayClient::GetInputForFrame(int player_port, s32 frame) const
{
  if (player_port < 0 || player_port >= (int)g_Inputs.size() || g_GekkoInputRingSize <= 0)
    return GCPadStatus{};
  return g_Inputs[player_port][frame % g_GekkoInputRingSize].emu_pad;
}

BrawlbackPad NetPlayClient::GetBrawlbackInputForFrame(int player_port, s32 frame) const
{
  if (player_port < 0 || player_port >= (int)g_Inputs.size() || g_GekkoInputRingSize <= 0)
    return BrawlbackPad{};
  return g_Inputs[player_port][frame % g_GekkoInputRingSize].game_pad;
}

bool NetPlayClient::IsStarted()
{
  return game_started.load();
}

bool NetPlayClient::IsInRollbackMode()
{
  return m_net_settings.m_RollbackMode;
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
// called from ---CPU--- thread
bool NetPlayClient::GetNetPads(const int pad_nb, const bool batching, GCPadStatus* pad_status)
{
  if (m_net_settings.m_RollbackMode)
  {
    *pad_status = GetDefaultPad(12);
    if (IsFirstInGamePad(pad_nb) && batching)
    {
      const int num_local_pads = NumLocalPads();
      for (int local_pad = 0; local_pad < num_local_pads; local_pad++)
      {
        GCPadStatus local_pad_status;
        if (Config::Get(Config::GetInfoForSIDevice(local_pad)) ==
            SerialInterface::SIDEVICE_WIIU_ADAPTER)
        {
          local_pad_status = GCAdapter::Input(local_pad);
        }
        else
        {
          local_pad_status = Pad::GetStatus(local_pad);
        }
        const int ingame_pad = LocalPadToInGamePad(local_pad);
        if (ingame_pad < (int)g_Inputs.size() && g_GekkoInputRingSize > 0)
          g_Inputs[ingame_pad][current_frame % g_GekkoInputRingSize].emu_pad = local_pad_status;
      }
    }

    if (batching)
    {
      if (pad_nb < (int)g_Inputs.size() && g_GekkoInputRingSize > 0)
        *pad_status = g_Inputs[pad_nb][current_frame % g_GekkoInputRingSize].emu_pad;
    }

    if (!batching)
    {
      const int local_pad = InGamePadToLocalPad(pad_nb);
      if (local_pad < 4)
      {
        if (Config::Get(Config::GetInfoForSIDevice(local_pad)) ==
            SerialInterface::SIDEVICE_WIIU_ADAPTER)
        {
          *pad_status = GCAdapter::Input(local_pad);
        }
        else
        {
          *pad_status = Pad::GetStatus(local_pad);
        }
      }
      if (pad_nb < (int)g_Inputs.size() && g_GekkoInputRingSize > 0)
        g_Inputs[pad_nb][current_frame % g_GekkoInputRingSize].emu_pad = *pad_status;
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

// called from ---GUI--- thread
bool NetPlayClient::StopGame()
{
  InvokeStop();

  if (IsInRollbackMode())
  {
    CloseSession();
    game_started.store(false);
    g_GekkoSpeedScale = 1.0;
    g_GekkoTimesyncTargetScale = 1.0;
    g_GekkoTimesyncSampleCounter = 0;
    current_frame = 0;
    advance_frames = 0;
    Config::SetCurrent(Config::MAIN_EMULATION_SPEED, 1.0);
  }

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
  auto desync_start_time = std::chrono::high_resolution_clock::now();

  // Check if we've notified about this player recently
  auto now = std::chrono::steady_clock::now();
  auto time_since_last =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_desync_notification)
          .count();

  INFO_LOG_FMT(BRAWLBACK, 
               "DESYNC DETECTED - PlayerIdx: {} | Frame: {} | TimeSinceLastNotif: {}ms",
               player_index, frame, time_since_last);

  if (time_since_last < DESYNC_NOTIFICATION_COOLDOWN_MS)
  {
    INFO_LOG_FMT(BRAWLBACK, "Desync notification suppressed (cooldown active - {}ms remaining)",
                 DESYNC_NOTIFICATION_COOLDOWN_MS - time_since_last);
    return;  // Don't spam notifications
  }

  // Check if this is the same desync we already reported
  if (m_last_desync_frame.count(player_index) &&
      std::abs(m_last_desync_frame[player_index] - frame) < 60)  // Within 1 second
  {
    INFO_LOG_FMT(BRAWLBACK, 
                 "Desync notification suppressed (similar to last desync - last frame: {})",
                 m_last_desync_frame[player_index]);
    return;
  }

  m_last_desync_frame[player_index] = frame;
  m_last_desync_notification = now;

  // Send desync notification to server
  auto packet_send_start = std::chrono::high_resolution_clock::now();

  sf::Packet packet;
  packet << MessageID::DesyncDetected;
  packet << static_cast<int>(player_index);
  packet << frame;

  SendAsync(std::move(packet));

  auto packet_send_duration = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::high_resolution_clock::now() - packet_send_start);

  // Show local notification
  std::string player_name = GetPlayerName(player_index);
  std::string message =
      fmt::format("Desync detected with player {} at frame {}", player_name, frame);

  INFO_LOG_FMT(BRAWLBACK, 
               "DESYNC NOTIFICATION SENT - Message: {} | PacketSendTime: {}µs",
               message, packet_send_duration.count());

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
bool NetPlayClient::process_pending_saves()
{
  const auto beginTime = std::chrono::steady_clock::now();
  const size_t pendingCount = g_GekkoPendingSaves.size();
  for (const auto& save : g_GekkoPendingSaves)
  {
    if (!save_gekko_state(save))
    {
      g_GekkoPendingSaves.clear();
      return false;
    }
  }
  g_GekkoPendingSaves.clear();
  g_GekkoLastPendingSaveUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - beginTime)
                                 .count();
  if (g_GekkoLogEnabled && pendingCount > 0)
  {
    std::ostringstream stream;
    stream << "pending_saves result=ok count=" << pendingCount
           << " elapsed_us=" << g_GekkoLastPendingSaveUs
           << " last_save_us=" << g_GekkoLastSaveStateUs;
    write_gekko_log(stream.str());
  }
  return true;
}

int NetPlayClient::rollback_execute_end_frame()
{
  const auto beginTime = std::chrono::steady_clock::now();
  write_gekko_log("end_frame begin");
  const auto pendingSaveBeginTime = std::chrono::steady_clock::now();
  if (!process_pending_saves())
  {
    write_gekko_log("end_frame result=fail reason=save");
    return 0;
  }
  const auto pendingSaveUs = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - pendingSaveBeginTime)
                                 .count();
  long long debugEndUs = 0;
  g_GekkoHasLatchedInput = false;
  if (g_GekkoLogEnabled)
  {
    const auto totalUs = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - beginTime)
                             .count();
    std::ostringstream stream;
    stream << "end_frame result=ok total_us=" << totalUs << " pending_save_us=" << pendingSaveUs
           << " debug_end_us=" << debugEndUs;
    write_gekko_log(stream.str());
  }
  else
  {
    write_gekko_log("end_frame result=ok");
  }
  if (advance_frames > 0)
    current_frame++;
  return 1;
}
void OnFrameEnd()
{
  INFO_LOG_FMT(BRAWLBACK, "-- End of Frame {} --",
               netplay_client ? netplay_client->current_frame : -1);
  if (IsNetPlayRunning() && netplay_client)
  {
    if (netplay_client->IsInRollbackMode())
    {
      netplay_client->rollback_execute_end_frame();
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

