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
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Event.h"
#include "Common/SPSCQueue.h"
#include "Common/TraversalClient.h"
#include "Core/Core.h"
#include "Core/NetPlayProto.h"
#include "Core/SyncIdentifier.h"
#include "InputCommon/GCPadStatus.h"
#include "Brawlback/include/brawlback-common/BrawlbackConstants.h"

class BootSessionData;

// Forward declare GekkoNet types
struct GekkoSession;
struct GekkoNetAdapter;
struct GekkoNetAddress;
struct GekkoNetResult;

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
// Brawl pad memory layout constants (EXI-style injection)
// gfPadSystem instance at 0x805bacc0, raw pads at +0x40, stride 0x40
static constexpr u32 BRAWL_PADSYSTEM_INSTANCE = 0x805bacc0;
static constexpr u32 BRAWL_PAD_RAW_BASE = BRAWL_PADSYSTEM_INSTANCE + 0x40;
static constexpr u32 BRAWL_PAD_STRIDE = 0x40;
// gfPadStatus button field offsets (each is a u32, button bits in lower 16 bits)
static constexpr u32 PAD_OFF_BUTTONS = 0x06;
static constexpr u32 PAD_OFF_STICKS = 0x30;  // m_stickX

// Forward declare GekkoNet adapter functions
void GekkoNetAdapter_SendData(GekkoNetAddress* address, const char* data, int length);
GekkoNetResult** GekkoNetAdapter_ReceiveData(int* length);
void GekkoNetAdapter_FreeData(void* data);

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
  virtual void OnRollbackModeChanged(bool enabled) = 0;
  virtual bool IsSpectator() = 0;
  virtual void OnHostInputAuthorityChanged(bool enabled) = 0;
  virtual void OnDesync(u32 frame, const std::string& player) = 0;
  virtual void OnConnectionLost() = 0;
  virtual void OnConnectionError(const std::string& message) = 0;
  virtual void OnTraversalError(Common::TraversalClient::FailureReason error) = 0;
  virtual void OnTraversalStateChanged(Common::TraversalClient::State state) = 0;
  virtual void OnGameStartAborted() = 0;
  virtual void OnGolferChanged(bool is_golfer, const std::string& golfer_name) = 0;
  virtual void OnTtlDetermined(u8 ttl) = 0;

  virtual bool IsRecording() = 0;
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
                                         std::span<const int> players) = 0;
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
  // Friend declarations for GekkoNet adapter callbacks
  friend void GekkoNetAdapter_SendData(GekkoNetAddress* address, const char* data, int length);
  friend GekkoNetResult** GekkoNetAdapter_ReceiveData(int* length);
  friend void GekkoNetAdapter_FreeData(void* data);

public:
  void ThreadFunc();
  void SendAsync(sf::Packet&& packet, u8 channel_id = DEFAULT_CHANNEL);

  NetPlayClient(const std::string& address, const u16 port, NetPlayUI* dialog, std::string name,
                const NetTraversalConfig& traversal_config);
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
  static SyncIdentifier GetBrawlFileIdentifier();
  void AdjustPlayerPadBufferSize(u32 buffer);
  void OnPadBufferPlayer(sf::Packet& packet);
  void OnPadBufferMinimum(sf::Packet& packet);

  // Send and receive pads values
  struct WiimoteDataBatchEntry
  {
    int wiimote;
    WiimoteEmu::SerializedWiimoteState* state;
  };
  bool WiimoteUpdate(const std::span<WiimoteDataBatchEntry>& entries);
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

  const PadMappingArray& GetPadMapping() const;
  const GBAConfigArray& GetGBAConfig() const;
  const PadMappingArray& GetWiimoteMapping() const;

  void AdjustMinimumPadBufferSize(unsigned int size);

  void AdjustPadBufferSize(unsigned int size);

  void SetWiiSyncData(std::unique_ptr<IOS::HLE::FS::FileSystem> fs, std::vector<u64> titles,
                      std::string redirect_folder);

  static SyncIdentifier GetSDCardIdentifier();

  void OnFrameEnd(std::unique_lock<std::mutex>& lock);
  void OnFrameStart(std::unique_lock<std::mutex>& lock);
  void InjectPadsForIteration(int iteration_index);
  void PauseForLocalAdvantage();
  void CheckForLocalAdvantage();
  bool IsRollingBack();
  bool IsInRollbackMode();
  int GetFramesToAdvance();

  // GekkoNet outer game loop iteration control
  int GetGekkoCurrentIteration() const { return m_gekko_current_iteration; }
  void SetGekkoCurrentIteration(int iteration) { m_gekko_current_iteration = iteration; }
  bool GetGekkoPendingFinalSave() const { return m_gekko_pending_final_save; }
  void SetGekkoPendingFinalSave(bool value) { m_gekko_pending_final_save = value; }
  bool GetShouldSaveAfterIteration(int iteration_index) const
  {
    if (iteration_index < 0 || iteration_index >= m_gekko_pending_ops.adv_count)
      return false;
    return m_gekko_pending_ops.save_after[iteration_index];
  }
  void WriteGekkoChecksumForIteration(int iteration_index, u32 checksum)
  {
    if (iteration_index < 0 || iteration_index >= m_gekko_pending_ops.adv_count)
      return;
    if (u32* ptr = m_gekko_pending_ops.save_checksum_ptrs[iteration_index])
      *ptr = checksum;
  }

  // Only for use in NetPlayClient.cpp >:(
  u64 current_frame = 0;
  bool time_synced = false;

  bool done_fast_forwarding;

  // EXI-style SI override interface for GekkoNet pad injection
  // Called from SI device code to override pad reads during resimulation
  static bool GetOverrideInput(int pad_num, GCPadStatus* status);
  
  inline GekkoSession* GetGekkoSession() { return m_gekko_session; }

protected:
  struct AsyncQueueEntry
  {
    sf::Packet packet;
    u8 channel_id = 0;
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

  // the number of ticks in-between frames
  constexpr static int buffer_accuracy = 4;

  inline u32 BufferSizeForPort(int pad) const
  {
    if (GetPadMapping()[pad] <= 0)
      return 0;

    return std::max(m_minimum_buffer_size, m_players.at(GetPadMapping().at(pad)).buffer);
  }

  // used for chat, not the best place for it
  inline std::string FindPlayerPadName(const Player* player) const
  {
    for (int i = 0; i < 4; i++)
    {
      if (GetPadMapping()[i] == player->pid)
        return " (port " + std::to_string(i + 1) + ")";
    }

    return "";
  }

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
  unsigned int m_target_buffer_size = 20;
  unsigned int m_minimum_buffer_size = 2;
  bool m_host_input_authority = false;
  PlayerId m_current_golfer = 1;

  // This bool will stall the client at the start of GetNetPads, used for switching input control
  // without deadlocking. Use the correspondingly named Event to wake it up.
  bool m_wait_on_input;
  bool m_wait_on_input_received;

  Player* m_local_player = nullptr;

  u32 m_current_game = 0;

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

  void AddPadStateToPacket(int in_game_pad, const GCPadStatus& np, sf::Packet& packet);
  void AddWiimoteStateToPacket(int in_game_pad, const WiimoteEmu::SerializedWiimoteState& np,
                               sf::Packet& packet);
  void Send(const sf::Packet& packet, u8 channel_id = DEFAULT_CHANNEL);
  void Disconnect();
  bool Connect();
  void SendGameStatus();
  void ComputeGameDigest(const SyncIdentifier& sync_identifier);
  void DisplayPlayersPing();
  u32 GetPlayersMaxPing() const;

  // GekkoNet session management
  void InitGekkoSession(const std::string& remote_addr, unsigned short local_port, bool is_host);
  void DestroyGekkoSession();
  void HandleGekkoFrame();
  bool ProcessGekkoEvents();
  void InjectGekkoInput(const std::array<GCPadStatus, 4>& pads);

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
  void OnPadBuffer(sf::Packet& packet);
  void OnHostInputAuthority(sf::Packet& packet);
  void OnGolfSwitch(sf::Packet& packet);
  void OnGolfPrepare(sf::Packet& packet);
  void OnChangeGame(sf::Packet& packet);
  void OnGameStatus(sf::Packet& packet);
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
  size_t GetLatestRemoteFrame();

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

  // GekkoNet adapter state
  struct GekkoNetPacket
  {
    std::vector<char> data;
    GekkoNetPacket() = default;
    explicit GekkoNetPacket(const char* d, size_t len) : data(d, d + len) {}
  };
  std::vector<GekkoNetPacket> m_gekko_received_packets;
  std::vector<void*> m_gekko_packet_ptrs;  // For adapter receive_data return
  std::mutex m_gekko_packet_mutex;
  std::mutex m_gekko_poll_mutex;  // Protects gekko_network_poll calls
  struct GekkoNetAdapter* m_gekko_adapter = nullptr;

  // GekkoNet session state
  GekkoSession* m_gekko_session = nullptr;
  int m_gekko_local_handle = -1;
  int m_gekko_remote_handle = -1;
  bool m_gekko_session_started = false;
  std::string m_gekko_remote_addr;
  bool m_gekko_seen_frame_zero = false;
  int m_gekko_connect_wait_ticks = 0;
  bool m_use_gekko_netplay = false;
  GCPadStatus m_gekko_last_local_input;
  bool m_is_rolling_back = false;

  // GekkoNet pending operations for current frame
  struct GekkonetPendingOps
  {
    static constexpr int MAX_ADVANCE = MAX_ROLLBACK_FRAMES + 1;
    int adv_count = 0;
    u32 adv_frames[MAX_ADVANCE]{};
    bool adv_rollback[MAX_ADVANCE]{};
    std::array<std::array<GCPadStatus, MAX_ADVANCE>, 4> adv_pads{};
    bool save_after[MAX_ADVANCE]{};
    u32* save_checksum_ptrs[MAX_ADVANCE]{};
    bool load_before[MAX_ADVANCE]{};
    int load_before_frame[MAX_ADVANCE]{};

    void Clear()
    {
      adv_count = 0;
      std::memset(adv_frames, 0, sizeof(adv_frames));
      std::memset(adv_rollback, 0, sizeof(adv_rollback));
      std::memset(save_after, 0, sizeof(save_after));
      std::memset(save_checksum_ptrs, 0, sizeof(save_checksum_ptrs));
      std::memset(load_before, 0, sizeof(load_before));
      std::memset(load_before_frame, 0, sizeof(load_before_frame));
      // Initialize with default GCPadStatus (centered sticks at 0x80)
      for (auto& player_pads : adv_pads)
        player_pads.fill(GCPadStatus{});
    }
  };
  GekkonetPendingOps m_gekko_pending_ops;
  bool m_gekko_pending_final_save = false;
  int m_gekko_current_iteration = 0;
  bool m_gekko_frame_save_initialized = false;
  static inline std::atomic<bool> s_override_active{false};
  static inline std::array<GCPadStatus, 4> s_override_pads{};

  void InjectPads(const std::array<GCPadStatus, 4>& pads, Core::System& system);
};

void NetPlay_Enable(NetPlayClient* const np);
void NetPlay_Disable();
bool NetPlay_GetWiimoteData(const std::span<NetPlayClient::WiimoteDataBatchEntry>& entries);
unsigned int NetPlay_GetLocalWiimoteForSlot(unsigned int slot);
void OnFrameStart();
void InjectPadsForIteration(int iteration_index);
bool HasPendingSave();
void ClearPendingSave();
bool ShouldSaveAfterIteration(int iteration_index);
bool ShouldSaveBeforeFirstIteration();
void WriteGekkoChecksumForIteration(int iteration_index, u32 checksum);
int GetCurrentIteration();
void SetCurrentIteration(int iteration);
void PauseForLocalAdvantage();
bool IsTimeSynced();
bool IsRollingBack();
bool IsInRollbackMode();
int GetFramesToAdvance();
bool GetShouldSleep();
}  // namespace NetPlay
