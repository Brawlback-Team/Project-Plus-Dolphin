// Copyright 2010 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <SFML/Network/Packet.hpp>
#include <array>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/SPSCQueue.h"
#include "Common/TraversalClient.h"
#include "Core/NetPlayProto.h"
#include "Core/SyncIdentifier.h"
#include "InputCommon/GCPadStatus.h"
#include "Brawlback/TimeSync.h"
#include "Brawlback/include/brawlback-common/BrawlbackPad.h"
#include "Brawlback/include/json.hpp"

using json = nlohmann::json;

class BootSessionData;

namespace IOS::HLE::FS
{
class FileSystem;
}

namespace UICommon
{
class GameFile;
}

namespace WiimoteEmu
{
struct SerializedWiimoteState;
}

namespace NetPlay
{
// Brawlback
struct Inputs
{
  GCPadStatus emu_pad;
  BrawlbackPad game_pad;
  s32 frame;
};
class NetPlayUI
{
public:
  virtual ~NetPlayUI() {}
  virtual void BootGame(const std::string& filename,
                        std::unique_ptr<BootSessionData> boot_session_data) = 0;
  virtual void StopGame() = 0;
  virtual bool IsHosting() const = 0;

  virtual void Update() = 0;
  virtual void AppendChat(const std::string& msg) = 0;

  virtual void OnMsgChangeGame(const SyncIdentifier& sync_identifier,
                               const std::string& netplay_name) = 0;
  virtual void OnMsgChangeGBARom(int pad, const NetPlay::GBAConfig& config) = 0;
  virtual void OnMsgStartGame() = 0;
  virtual void OnMsgStopGame() = 0;
  virtual void OnMsgPowerButton() = 0;
  virtual void OnPlayerConnect(const std::string& player) = 0;
  virtual void OnPlayerDisconnect(const std::string& player) = 0;
  virtual void OnMinimumPadBufferChanged(u32 buffer) = 0;
  virtual void OnPlayerPadBufferChanged(u32 buffer) = 0;
  virtual void OnHostInputAuthorityChanged(bool enabled) = 0;
  virtual void OnRollbackModeChanged(bool enabled) = 0;
  virtual void OnDesync(u32 frame, const std::string& player) = 0;
  virtual void OnConnectionLost() = 0;
  virtual void OnConnectionError(const std::string& message) = 0;
  virtual void OnTraversalError(Common::TraversalClient::FailureReason error) = 0;
  virtual void OnTraversalStateChanged(Common::TraversalClient::State state) = 0;
  virtual void OnGameStartAborted() = 0;
  virtual void OnGolferChanged(bool is_golfer, const std::string& golfer_name) = 0;
  virtual void OnTtlDetermined(u8 ttl) = 0;

  virtual bool IsRecording() = 0;
  virtual bool IsSpectator() = 0;
  virtual std::shared_ptr<const UICommon::GameFile>
  FindGameFile(const SyncIdentifier& sync_identifier,
               SyncIdentifierComparison* found = nullptr) = 0;
  virtual std::string FindGBARomPath(const std::array<u8, 20>& hash, std::string_view title,
                                     int device_number) = 0;
  virtual void ShowGameDigestDialog(const std::string& title) = 0;
  virtual void SetGameDigestProgress(int pid, int progress) = 0;
  virtual void SetGameDigestResult(int pid, const std::string& result) = 0;
  virtual void AbortGameDigest() = 0;

  virtual void OnIndexAdded(bool success, std::string error) = 0;
  virtual void OnIndexRefreshFailed(std::string error) = 0;

  virtual void ShowChunkedProgressDialog(const std::string& title, u64 data_size,
                                         const std::vector<int>& players) = 0;
  virtual void HideChunkedProgressDialog() = 0;
  virtual void SetChunkedProgress(int pid, u64 progress) = 0;

  virtual void SetHostWiiSyncData(std::vector<u64> titles, std::string redirect_folder) = 0;
};

class Player
{
public:
  PlayerId pid{};
  std::string name;
  std::string revision;
  u32 ping = 0;
  u32 buffer = 0;
  SyncIdentifierComparison game_status = SyncIdentifierComparison::Unknown;

  bool IsHost() const { return pid == 1; }
};

class NetPlayClient : public Common::TraversalClientClient
{
public:
  void ThreadFunc();
  void SendAsync(sf::Packet&& packet, u8 channel_id = DEFAULT_CHANNEL, _ENetPacketFlag flag = ENET_PACKET_FLAG_RELIABLE);

  NetPlayClient(const std::string& address, const u16 port, NetPlayUI* dialog,
                const std::string& name, const NetTraversalConfig& traversal_config);
  ~NetPlayClient() override;

  std::vector<const Player*> GetPlayers();
  const NetSettings& GetNetSettings() const;

  // Called from the GUI thread.
  bool IsConnected() const { return m_is_connected; }
  bool StartGame(const std::string& path);
  void InvokeStop();
  bool StopGame();
  void Stop();
  bool ChangeGame(const std::string& game);
  void SendChatMessage(const std::string& msg);
  void RequestStopGame();
  void SendPowerButtonEvent();
  void RequestGolfControl(PlayerId pid);
  void RequestGolfControl();
  std::string GetCurrentGolfer();

  // Send and receive pads values
  struct WiimoteDataBatchEntry
  {
    int wiimote;
    WiimoteEmu::SerializedWiimoteState* state;
  };
  bool WiimoteUpdate(const std::span<WiimoteDataBatchEntry>& entries);
  GCPadStatus GetDefaultPad(int config);
  bool GetNetPads(int pad_nb, bool from_vi, GCPadStatus* pad_status);

  u64 GetInitialRTCValue() const;

  void OnTraversalStateChanged() override;
  void OnConnectReady(ENetAddress addr) override;
  void OnConnectFailed(Common::TraversalConnectFailedReason reason) override;
  void OnTtlDetermined(u8 ttl) override {}

  bool IsFirstInGamePad(int ingame_pad) const;
  int NumLocalPads() const;
  int NumLocalWiimotes() const;

  int InGamePadToLocalPad(int ingame_pad) const;
  int LocalPadToInGamePad(int local_pad) const;
  int InGameWiimoteToLocalWiimote(int ingame_wiimote) const;
  int LocalWiimoteToInGameWiimote(int local_wiimote) const;

  bool PlayerHasControllerMapped(PlayerId pid) const;
  bool LocalPlayerHasControllerMapped() const;
  bool IsLocalPlayer(PlayerId pid) const;
  const PlayerId& GetLocalPlayerId() const;

  static void SendTimeBase();
  bool DoAllPlayersHaveGame();
  
  void AdjustPlayerPadBufferSize(u32 buffer);

  // the number of ticks in-between frames
  constexpr static int buffer_accuracy = 4;

  inline u32 BufferSizeForPort(int pad) const
  {
    if (m_pad_map[pad] <= 0)
      return 0;

    return std::max(m_minimum_buffer_size, m_players.at(m_pad_map.at(pad)).buffer);
  }

  // used for chat, not the best place for it
  inline std::string FindPlayerPadName(const Player* player) const
  {
    for (int i = 0; i < 4; i++)
    {
      if (m_pad_map[i] == player->pid)
        return " (port " + std::to_string(i + 1) + ")";
    }

    return "";
  }

  NetPlayUI* dialog = nullptr;
  Player* local_player = nullptr;

  const PadMappingArray& GetPadMapping() const;
  const GBAConfigArray& GetGBAConfig() const;
  const PadMappingArray& GetWiimoteMapping() const;

  void AdjustMinimumPadBufferSize(unsigned int size);

  void SetWiiSyncData(std::unique_ptr<IOS::HLE::FS::FileSystem> fs, std::vector<u64> titles,
                      std::string redirect_folder);

  static SyncIdentifier GetSDCardIdentifier();
  static SyncIdentifier GetBrawlFileIdentifier();

  // Brawlback
  struct RingBufferInput
  {
    static constexpr size_t CAPACITY = 256;  // Power of 2 for fast modulo
    std::array<Inputs, CAPACITY> buffer;
    size_t head = 0;
    size_t tail = 0;
    s32 min_frame = 0;
    s32 max_frame = -1;

    std::optional<Inputs> PopFront()
    {
      if (Empty())
        return std::nullopt;

      Inputs result = buffer[tail];
      tail = (tail + 1) & (CAPACITY - 1);

      // Update min_frame if buffer is not empty
      if (!Empty())
        min_frame = buffer[tail].frame;
      else
      {
        // Reset when buffer becomes empty
        min_frame = 0;
        max_frame = -1;
      }

      return result;
    }

    void Push(const Inputs& input)
    {
      if (input.frame <= max_frame)
        return;  // Ignore old inputs

      size_t next_head = (head + 1) & (CAPACITY - 1);
      if (next_head == tail)
      {
        // Buffer full, advance tail
        tail = (tail + 1) & (CAPACITY - 1);
        min_frame = buffer[tail].frame;
      }

      buffer[head] = input;
      max_frame = input.frame;
      head = next_head;
    }

    std::optional<Inputs> Get(s32 frame) const
    {
      if (frame < min_frame || frame > max_frame)
        return std::nullopt;

      // Linear search for the frame
      for (size_t i = tail; i != head; i = (i + 1) & (CAPACITY - 1))
      {
        if (buffer[i].frame == frame)
          return buffer[i];
      }
      return std::nullopt;
    }

    const Inputs* GetBack() const
    {
      if (head == tail)
        return nullptr;
      size_t last_idx = (head == 0) ? (CAPACITY - 1) : (head - 1);
      return &buffer[last_idx];
    }

    const Inputs* GetFront() const
    {
      if (head == tail)
        return nullptr;
      return &buffer[tail];
    }

    bool Empty() const { return head == tail; }

    size_t Size() const
    {
      if (head >= tail)
        return head - tail;
      return CAPACITY - tail + head;
    }

    bool HasFrame(s32 frame) const { return frame >= min_frame && frame <= max_frame; }

    void Clear()
    {
      head = 0;
      tail = 0;
      min_frame = 0;
      max_frame = -1;
    }
  };
  void OnFrameStart(BrawlbackPad& pad);
  bool IsRollingBack();
  bool IsStarted();
  bool IsInRollbackMode();
  u32 GetLatestRemoteFrame(int local_player_port);
  void StoreInputs(Inputs& pad, int local_player_port);
  size_t GetInputsSize();
  std::optional<Inputs> FindRemoteInputs(int playerIdx, s32 frame);
  std::optional<BrawlbackPad> GetPredictedInputs(int playerIdx, s32 frame);
  void SendRollbackVerification(s32 frame, u64 hash);

  // Only for use in NetPlayClient.cpp >:(
  s32 current_frame = 0;
  // Only for use in NetPlayClient.cpp >:(
  s32 frame_to_stop_at = 0;
  s32 latest_confirmed_frame = 0;
  s32 advance_frames = 1;

  bool done_fast_forwarding;
  bool start_inputs = false;

  std::unique_ptr<Brawlback::TimeSync> time_sync;

  void NotifyDesync(int player_index, s32 frame);
  std::string GetPlayerName(int player_index);


protected:
  struct AsyncQueueEntry
  {
    sf::Packet packet;
    u8 channel_id = 0;
    _ENetPacketFlag flag = ENET_PACKET_FLAG_RELIABLE;
  };

  void ClearBuffers();

  struct
  {
    std::recursive_mutex game;
    // lock order
    std::recursive_mutex players;
    std::recursive_mutex async_queue_write;
  } m_crit;

  Common::SPSCQueue<AsyncQueueEntry> m_async_queue;

  std::array<Common::SPSCQueue<GCPadStatus>, 4> m_pad_buffer;
  std::array<Common::SPSCQueue<WiimoteEmu::SerializedWiimoteState>, 4> m_wiimote_buffer;

  std::array<GCPadStatus, 4> m_last_pad_status{};
  std::array<bool, 4> m_first_pad_status_received{};

  std::chrono::time_point<std::chrono::steady_clock> m_buffer_under_target_last;

  NetPlayUI* m_dialog = nullptr;

  ENetHost* m_client = nullptr;
  ENetPeer* m_server = nullptr;
  std::thread m_thread;

  SyncIdentifier m_selected_game;
  Common::Flag m_is_running{false};
  Common::Flag m_do_loop{true};

  // In non-host input authority mode, this is how many packets each client should
  // try to keep in-flight to the other clients. In host input authority mode, this is how
  // many incoming input packets need to be queued up before the client starts
  // speeding up the game to drain the buffer.
  unsigned int m_minimum_buffer_size = 2;
  bool m_host_input_authority = false;
  bool m_rollback_mode = false;
  PlayerId m_current_golfer = 1;

  // This bool will stall the client at the start of GetNetPads, used for switching input control
  // without deadlocking. Use the correspondingly named Event to wake it up.
  bool m_wait_on_input;
  bool m_wait_on_input_received;

  Player* m_local_player = nullptr;

  u32 m_current_game = 0;

  PadMappingArray m_pad_map{};
  GBAConfigArray m_gba_config{};
  PadMappingArray m_wiimote_map{};

  bool m_is_recording = false;

private:
  enum class ConnectionState
  {
    WaitingForTraversalClientConnection,
    WaitingForTraversalClientConnectReady,
    Connecting,
    WaitingForHelloResponse,
    Connected,
    Failure
  };

  void SendStartGamePacket();
  void SendStopGamePacket();

  void SyncSaveDataResponse(bool success);
  void SyncCodeResponse(bool success);

  bool PollLocalPad(int local_pad, sf::Packet& packet);
  void SendPadHostPoll(PadIndex pad_num);

  bool AddLocalWiimoteToBuffer(int local_wiimote, const WiimoteEmu::SerializedWiimoteState& state,
                               sf::Packet& packet);

  void UpdateDevices();
  void AddPadStateToPacket(int in_game_pad, const GCPadStatus& np, sf::Packet& packet);
  void AddWiimoteStateToPacket(int in_game_pad, const WiimoteEmu::SerializedWiimoteState& np,
                               sf::Packet& packet);
  void Send(const sf::Packet& packet, u8 channel_id = DEFAULT_CHANNEL, _ENetPacketFlag flag  = ENET_PACKET_FLAG_RELIABLE);
  void Disconnect();
  bool Connect();
  void SendGameStatus();
  void ComputeGameDigest(const SyncIdentifier& sync_identifier);
  void DisplayPlayersPing();
  u32 GetPlayersMaxPing() const;

  u32 CalculatePingVariance();

  void OnData(sf::Packet& packet);
  void OnPlayerJoin(sf::Packet& packet);
  void OnPlayerLeave(sf::Packet& packet);
  void OnChatMessage(sf::Packet& packet);
  void OnChunkedDataStart(sf::Packet& packet);
  void OnChunkedDataEnd(sf::Packet& packet);
  void OnChunkedDataPayload(sf::Packet& packet);
  void OnChunkedDataAbort(sf::Packet& packet);
  void OnPadMapping(sf::Packet& packet);
  void OnWiimoteMapping(sf::Packet& packet);
  void OnGBAConfig(sf::Packet& packet);
  void OnPadData(sf::Packet& packet);
  void OnPadHostData(sf::Packet& packet);
  void OnWiimoteData(sf::Packet& packet);
  void OnPadBufferMinimum(sf::Packet& packet);
  void OnPadBufferPlayer(sf::Packet& packet);
  void OnHostInputAuthority(sf::Packet& packet);
  void OnRollbackMode(sf::Packet& packet);
  void OnGolfSwitch(sf::Packet& packet);
  void OnGolfPrepare(sf::Packet& packet);
  void OnChangeGame(sf::Packet& packet);
  void OnGameStatus(sf::Packet& packet);
  void ToJson(json& j, const BrawlbackPad& i);
  void ToJson(json& j, const GCPadStatus& i);
  void ToJson(json& j, const Inputs& i);
  void FromJson(const json& j, BrawlbackPad& i);
  void FromJson(const json& j, GCPadStatus& i);
  void FromJson(const json& j, Inputs& i);
  void OnStartGame(sf::Packet& packet);
  void OnStopGame(sf::Packet& packet);
  void OnPowerButton();
  void OnPing(sf::Packet& packet);
  void OnPlayerPingData(sf::Packet& packet);
  void OnDesyncDetected(sf::Packet& packet);
  void OnSyncSaveData(sf::Packet& packet);
  void OnSyncSaveDataNotify(sf::Packet& packet);
  void OnSyncSaveDataRaw(sf::Packet& packet);
  void OnSyncSaveDataGCI(sf::Packet& packet);
  void OnSyncSaveDataWii(sf::Packet& packet);
  void OnSyncSaveDataGBA(sf::Packet& packet);
  void OnSyncCodes(sf::Packet& packet);
  void OnSyncCodesNotify();
  void OnSyncCodesNotifyGecko(sf::Packet& packet);
  void OnSyncCodesDataGecko(sf::Packet& packet);
  void OnSyncCodesNotifyAR(sf::Packet& packet);
  void OnSyncCodesDataAR(sf::Packet& packet);
  void OnComputeGameDigest(sf::Packet& packet);
  void OnGameDigestProgress(sf::Packet& packet);
  void OnGameDigestResult(sf::Packet& packet);
  void OnGameDigestError(sf::Packet& packet);
  void OnGameDigestAbort();
  void OnFrameAck(sf::Packet& packet);

  bool m_is_connected = false;
  ConnectionState m_connection_state = ConnectionState::Failure;

  PlayerId m_pid = 0;
  NetSettings m_net_settings{};
  std::map<PlayerId, Player> m_players;
  std::string m_host_spec;
  std::string m_player_name;
  bool m_connecting = false;
  Common::TraversalClient* m_traversal_client = nullptr;
  std::thread m_game_digest_thread;
  bool m_should_compute_game_digest = false;
  Common::Event m_gc_pad_event;
  Common::Event m_wii_pad_event;
  Common::Event m_first_pad_status_received_event;
  Common::Event m_wait_on_input_event;
  u8 m_sync_save_data_count = 0;
  u8 m_sync_save_data_success_count = 0;
  u16 m_sync_gecko_codes_count = 0;
  u16 m_sync_gecko_codes_success_count = 0;
  bool m_sync_gecko_codes_complete = false;
  u16 m_sync_ar_codes_count = 0;
  u16 m_sync_ar_codes_success_count = 0;
  bool m_sync_ar_codes_complete = false;
  std::unordered_map<u32, sf::Packet> m_chunked_data_receive_queue;

  u64 m_initial_rtc = 0;
  u32 m_timebase_frame = 0;

  std::unique_ptr<IOS::HLE::FS::FileSystem> m_wii_sync_fs;
  std::vector<u64> m_wii_sync_titles;
  std::string m_wii_sync_redirect_folder;

  // Brawlback
  std::vector<RingBufferInput> inputs;
  std::vector<Inputs> predicted_inputs;
  std::vector<int> pad_config;
  int delay = 1;
  std::condition_variable wait_for_inputs;
  bool is_in_match = false;

  bool LoadFromFrame(s32 origFrame, s32 frame);
  void SendInputs(sf::Packet& packet, int local_player_port, MessageID frame_data_cmd);
  void PrintInputs(BrawlbackPad& pad);
  void HandleInputs(Inputs pad);

  // Desync tracking
  std::map<int, s32> m_last_desync_frame;  // Track last desync per player
  std::chrono::steady_clock::time_point m_last_desync_notification;
  constexpr static int DESYNC_NOTIFICATION_COOLDOWN_MS = 5000;  // Don't spam notifications
};

void NetPlay_Enable(NetPlayClient* const np);
void NetPlay_Disable();
bool NetPlay_GetWiimoteData(const std::span<NetPlayClient::WiimoteDataBatchEntry>& entries);
unsigned int NetPlay_GetLocalWiimoteForSlot(unsigned int slot);

// Brawlback
void OnFrameEnd();
void OnFrameStart(BrawlbackPad& pad);
// tells when Dolphin is actually mid rollback
bool IsRollingBack();
bool IsStarted();
    // tells if we're using rollback networking
bool IsInRollbackMode();
s32 StopFrame();
s32 CurrentFrame();
s32 AdvanceFrames();
void IncrementCurrentFrame();
size_t FramesSize();


extern NetPlayClient* netplay_client;
}  // namespace NetPlay
