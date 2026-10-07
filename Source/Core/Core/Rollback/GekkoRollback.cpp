// Copyright 2026 Project+ Rollback Authors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/Rollback/GekkoRollback.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_M_X86_64) || defined(__x86_64__)
#include <xmmintrin.h>
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
#include <arm64intr.h>
#endif

#include <gekkonet.h>
#include <fmt/format.h>

#include "Common/Config/Config.h"
#include "Common/ENet.h"
#include "Common/FPURoundMode.h"
#include "Common/Logging/Log.h"
#include "Common/Timer.h"
#include "Core/Config/MainSettings.h"
#include "Core/Config/NetplaySettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/Rollback/DirtyBitmap.h"
#include "Core/Rollback/DirtyPages.h"
#include "Core/Rollback/Rollback.h"
#include "Core/System.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/GCAdapter.h"

namespace Rollback
{
namespace
{
// All physical controller buttons plus the adapter's synchronized calibration event.
constexpr u16 PAD_WIRE_BUTTONS = 0x1F7F | PAD_GET_ORIGIN;
constexpr int MAX_PORTS = 4;
// Gekko exposes up to 10 rollback frames in the lobby. Keep two additional snapshots so the
// current and confirmed boundary states cannot alias the oldest rollback state in the ring.
constexpr int RING_SNAPSHOT_SLOTS = 12;
constexpr int WAIT_SLEEP_US = 100;
constexpr auto SIMULATED_P2_LATENCY = std::chrono::milliseconds(40);
constexpr u64 STRESS_PACKET_DELAY_FRAMES = 3;
constexpr u64 STRESS_INPUT_PERIOD_FRAMES = 10;
constexpr std::string_view SIMULATED_P1_ADDRESS = "in-process-p1";
constexpr std::string_view SIMULATED_P2_ADDRESS = "in-process-p2";
constexpr u32 BRAWL_FRAME_HOOK_ADDR = 0x80017504;
constexpr u32 BRAWL_EXPECTED_OPCODE = 0x90170100;  // stw r0, 0x100(r23)

// Host floating point scope to protect guest FPU rounding modes
u64 HostFloatControl()
{
#if defined(_M_X86_64) || defined(__x86_64__)
  return _mm_getcsr();
#elif defined(_MSC_VER) && (defined(_M_ARM64) || defined(_M_ARM64EC))
  return _ReadStatusReg(ARM64_FPCR);
#elif defined(__aarch64__)
  u64 fpcr;
  __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
  return fpcr;
#else
  return 0;
#endif
}

class HostFloatScope
{
public:
  explicit HostFloatScope(PowerPC::PowerPCState& ppc_state) : m_ppc_state(ppc_state)
  {
    Common::FPU::LoadDefaultSIMDState();
  }
  ~HostFloatScope() { PowerPC::RoundingModeUpdated(m_ppc_state); }
  HostFloatScope(const HostFloatScope&) = delete;
  HostFloatScope& operator=(const HostFloatScope&) = delete;

private:
  PowerPC::PowerPCState& m_ppc_state;
};

struct QueuedEvent
{
  GekkoGameEventType type = GekkoEmptyGameEvent;
  int frame = -1;
  std::vector<unsigned char> inputs;
  unsigned int* checksum = nullptr;
  unsigned int* state_len = nullptr;
  bool rolling_back = false;
  bool running_ahead = false;
};

struct SimulatedPacket
{
  std::chrono::steady_clock::time_point delivery_time;
  u64 delivery_frame = 0;
  bool frame_delayed = false;
  std::vector<char> data;
};

struct SampledLocalInput
{
  s64 frame = 0;
  WirePad input{};
};

std::deque<SimulatedPacket> s_packets_to_main;
std::deque<SimulatedPacket> s_packets_to_fake;
std::vector<GekkoNetResult*> s_main_results;
std::vector<GekkoNetResult*> s_fake_results;
std::vector<GekkoNetResult*> s_shared_results;
u64 s_simulated_link_frame = 0;
u64 s_last_stress_peer_update_frame = 0;
bool s_stress_mode = false;
bool s_stress_frame_delay_active = false;

bool ParsePeerEndpoint(std::string_view endpoint, ENetAddress* address)
{
  const size_t colon = endpoint.rfind(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 == endpoint.size())
    return false;

  unsigned int port = 0;
  const char* port_begin = endpoint.data() + colon + 1;
  const char* port_end = endpoint.data() + endpoint.size();
  const auto [end, error] = std::from_chars(port_begin, port_end, port);
  if (error != std::errc{} || end != port_end || port == 0 || port > 65535)
    return false;

  const std::string host(endpoint.substr(0, colon));
  address->port = static_cast<u16>(port);
  return enet_address_set_host(address, host.c_str()) == 0;
}

void SharedAdapterSend(GekkoNetAddress* address, const char* data, int length)
{
  if (!address || address->size != sizeof(ENetAddress) || length < 0)
    return;

  ENetAddress endpoint{};
  std::memcpy(&endpoint, address->data, sizeof(endpoint));
  if (!Common::ENet::SendRollbackDatagram(endpoint, data, static_cast<size_t>(length)))
    WARN_LOG_FMT(CORE, "GekkoNet: failed to send shared-socket datagram");
}

GekkoNetResult** SharedAdapterReceive(int* length)
{
  s_shared_results.clear();
  for (Common::ENet::RollbackDatagram& datagram : Common::ENet::DrainRollbackDatagrams())
  {
    auto* result = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
    result->addr.size = sizeof(ENetAddress);
    result->addr.data = std::malloc(sizeof(ENetAddress));
    std::memcpy(result->addr.data, &datagram.address, sizeof(ENetAddress));
    result->data_len = static_cast<unsigned int>(datagram.payload.size());
    result->data = std::malloc(datagram.payload.size());
    std::memcpy(result->data, datagram.payload.data(), datagram.payload.size());
    s_shared_results.push_back(result);
  }
  *length = static_cast<int>(s_shared_results.size());
  return s_shared_results.data();
}

void SharedAdapterFree(void* data)
{
  std::free(data);
}

GekkoNetAdapter s_shared_adapter{SharedAdapterSend, SharedAdapterReceive, SharedAdapterFree};

void QueueSimulatedPacket(std::deque<SimulatedPacket>* destination, const char* data, int length)
{
  SimulatedPacket packet;
  packet.data.assign(data, data + length);
  if (s_stress_mode && s_stress_frame_delay_active)
  {
    packet.delivery_frame = s_simulated_link_frame + STRESS_PACKET_DELAY_FRAMES;
    packet.frame_delayed = true;
  }
  else
  {
    // Stress mode keeps handshake traffic immediate. Frame-delayed delivery begins only after
    // Gekko reports that the session has started, avoiding a frame-zero synchronization deadlock.
    packet.delivery_time = std::chrono::steady_clock::now() +
                           (s_stress_mode ? std::chrono::milliseconds(0) : SIMULATED_P2_LATENCY);
  }
  destination->push_back(std::move(packet));
}

void MainAdapterSend(GekkoNetAddress*, const char* data, int length)
{
  QueueSimulatedPacket(&s_packets_to_fake, data, length);
}

void FakeAdapterSend(GekkoNetAddress*, const char* data, int length)
{
  QueueSimulatedPacket(&s_packets_to_main, data, length);
}

GekkoNetResult** ReceiveSimulatedPackets(std::deque<SimulatedPacket>* inbox,
                                         std::vector<GekkoNetResult*>* results,
                                         std::string_view sender, int* length)
{
  results->clear();
  const auto now = std::chrono::steady_clock::now();
  while (!inbox->empty())
  {
    const SimulatedPacket& front = inbox->front();
    const bool ready = front.frame_delayed ? s_simulated_link_frame >= front.delivery_frame :
                                             front.delivery_time <= now;
    if (!ready)
      break;
    SimulatedPacket packet = std::move(inbox->front());
    inbox->pop_front();

    auto* result = static_cast<GekkoNetResult*>(std::malloc(sizeof(GekkoNetResult)));
    result->addr.size = static_cast<unsigned int>(sender.size());
    result->addr.data = std::malloc(sender.size());
    std::memcpy(result->addr.data, sender.data(), sender.size());
    result->data_len = static_cast<unsigned int>(packet.data.size());
    result->data = std::malloc(packet.data.size());
    std::memcpy(result->data, packet.data.data(), packet.data.size());
    results->push_back(result);
  }
  *length = static_cast<int>(results->size());
  return results->data();
}

GekkoNetResult** MainAdapterReceive(int* length)
{
  return ReceiveSimulatedPackets(&s_packets_to_main, &s_main_results, SIMULATED_P2_ADDRESS, length);
}

GekkoNetResult** FakeAdapterReceive(int* length)
{
  return ReceiveSimulatedPackets(&s_packets_to_fake, &s_fake_results, SIMULATED_P1_ADDRESS, length);
}

void SimulatedAdapterFree(void* data)
{
  std::free(data);
}

GekkoNetAdapter s_main_simulated_adapter{MainAdapterSend, MainAdapterReceive, SimulatedAdapterFree};
GekkoNetAdapter s_fake_simulated_adapter{FakeAdapterSend, FakeAdapterReceive, SimulatedAdapterFree};

void ResetSimulatedLink()
{
  s_packets_to_main.clear();
  s_packets_to_fake.clear();
  s_main_results.clear();
  s_fake_results.clear();
  s_simulated_link_frame = 0;
  s_last_stress_peer_update_frame = 0;
  s_stress_mode = false;
  s_stress_frame_delay_active = false;
}

struct GekkoManager
{
  std::recursive_mutex mutex;
  GekkoSession* session = nullptr;
  GekkoSession* simulated_peer_session = nullptr;
  std::unique_ptr<SnapshotRing> ring;

  int players = 2;
  int local_player = 1;
  int local_handle = -1;
  int simulated_peer_p2_handle = -1;
  int remote_handle = -1;
  bool native_adapter_active = false;
  bool local_uses_gc_adapter = false;
  bool debug_p2_cstick = false;
  bool simulate_remote_p2 = false;
  bool stress_test = false;
  bool tracked_bitmap_clear = false;
  bool full_scan_benchmark = false;
  int configured_local_delay = 0;
  u32 session_id = 0;
  u64 local_input_frame = 0;
  std::vector<int> player_handles;
  std::deque<QueuedEvent> pending_events;
  std::deque<SampledLocalInput> sampled_local_inputs;
  std::array<GCPadStatus, MAX_PORTS> latched_pads{};
  std::atomic<bool> active{false};
  std::atomic<bool> stop_requested{false};

  // Pacing
  double speed_scale = 1.0;
  double target_scale = 1.0;
  int timesync_counter = 0;
  std::optional<std::pair<s64, TimePoint>> throttle_reference_before_load;
  Common::PrecisionTimer wait_timer;

  // Rolling diagnostics. These distinguish emulator snapshot cost from time spent waiting for
  // GekkoNet to authorize the next frame.
  std::chrono::steady_clock::time_point perf_window_start{};
  DirtyPages::Counters perf_dirty_pages_start{};
  u64 perf_real_frames = 0;
  u64 perf_replay_frames = 0;
  u64 perf_save_count = 0;
  u64 perf_load_count = 0;
  u64 perf_pump_count = 0;
  double perf_save_ms = 0.0;
  double perf_save_max_ms = 0.0;
  double perf_load_ms = 0.0;
  double perf_load_max_ms = 0.0;
  double perf_pump_ms = 0.0;
  double perf_pump_max_ms = 0.0;
  std::optional<WirePad> last_local_wire;
  u64 perf_local_input_changes = 0;
  std::array<u64, sizeof(WirePad)> perf_local_byte_changes{};
  std::optional<WirePad> last_applied_local_wire;
  u64 perf_delay_checks = 0;
  u64 perf_delay_matches = 0;
  u64 perf_delay_mismatches = 0;
  u64 perf_delay_missing = 0;
  u64 perf_delay_transitions = 0;
  u64 perf_delay_transition_frames = 0;
  u64 perf_delay_transition_min = 0;
  u64 perf_delay_transition_max = 0;
  std::optional<std::chrono::steady_clock::time_point> frame_execution_start;
  bool frame_execution_resim = false;
  u64 perf_replay_exec_count = 0;
  double perf_replay_exec_ms = 0.0;
  double perf_replay_exec_max_ms = 0.0;
  u64 perf_replay_save_count = 0;
  double perf_replay_save_ms = 0.0;
  double perf_replay_save_max_ms = 0.0;
  std::optional<std::chrono::steady_clock::time_point> rollback_burst_start;
  u64 perf_rollback_burst_count = 0;
  double perf_rollback_burst_ms = 0.0;
  double perf_rollback_burst_max_ms = 0.0;
  double perf_burst_load_ms = 0.0;
  double perf_burst_load_max_ms = 0.0;
  double perf_burst_exec_ms = 0.0;
  double perf_burst_exec_max_ms = 0.0;
  double perf_burst_save_ms = 0.0;
  double perf_burst_save_max_ms = 0.0;
  double perf_burst_other_ms = 0.0;
  double perf_burst_other_max_ms = 0.0;
  double current_burst_load_ms = 0.0;
  double current_burst_exec_ms = 0.0;
  double current_burst_save_ms = 0.0;
  u64 current_rollback_replays = 0;
  u64 perf_rollback_depth_total = 0;
  u64 perf_rollback_depth_max = 0;
  std::optional<std::chrono::steady_clock::time_point> last_real_advance;
  u64 perf_real_interval_count = 0;
  double perf_real_interval_ms = 0.0;
  double perf_real_interval_max_ms = 0.0;
};

GekkoManager g_manager;

}  // namespace

WirePad EncodePad(const GCPadStatus& status)
{
  WirePad pad{};
  const u16 buttons = status.button & PAD_WIRE_BUTTONS;
  pad.data[0] = static_cast<u8>(buttons >> 8);
  pad.data[1] = static_cast<u8>(buttons & 0xFF);
  pad.data[2] = static_cast<u8>(status.stickX - GCPadStatus::MAIN_STICK_CENTER_X);
  pad.data[3] = static_cast<u8>(status.stickY - GCPadStatus::MAIN_STICK_CENTER_Y);
  pad.data[4] = static_cast<u8>(status.substickX - GCPadStatus::C_STICK_CENTER_X);
  pad.data[5] = static_cast<u8>(status.substickY - GCPadStatus::C_STICK_CENTER_Y);
  pad.data[6] = status.triggerLeft;
  pad.data[7] = status.triggerRight;
  return pad;
}

GCPadStatus DecodePad(const WirePad& pad)
{
  GCPadStatus status{};
  status.button = static_cast<u16>((pad.data[0] << 8) | pad.data[1]) & PAD_WIRE_BUTTONS;
  status.stickX = static_cast<u8>(pad.data[2] + GCPadStatus::MAIN_STICK_CENTER_X);
  status.stickY = static_cast<u8>(pad.data[3] + GCPadStatus::MAIN_STICK_CENTER_Y);
  status.substickX = static_cast<u8>(pad.data[4] + GCPadStatus::C_STICK_CENTER_X);
  status.substickY = static_cast<u8>(pad.data[5] + GCPadStatus::C_STICK_CENTER_Y);
  status.triggerLeft = pad.data[6];
  status.triggerRight = pad.data[7];
  status.analogA = 0;
  status.analogB = 0;
  status.isConnected = true;
  return status;
}

std::optional<GCPadStatus> GetRollbackPad(int port)
{
  if (!g_manager.active.load(std::memory_order_relaxed))
    return std::nullopt;

  if (port >= 0 && port < MAX_PORTS)
  {
    std::lock_guard lk(g_manager.mutex);
    return g_manager.latched_pads[port];
  }
  return std::nullopt;
}

bool IsGekkoSessionActive()
{
  return g_manager.active.load(std::memory_order_relaxed);
}

void SetGekkoLocalDelay(int local_delay)
{
  const int clamped_delay = std::clamp(local_delay, 0, 60);
  std::lock_guard lk(g_manager.mutex);
  if (!g_manager.session || g_manager.local_handle < 0)
    return;

  const int previous_delay = g_manager.configured_local_delay;
  if (previous_delay == clamped_delay)
    return;

  gekko_set_local_delay(g_manager.session, g_manager.local_handle,
                        static_cast<unsigned char>(clamped_delay));
  g_manager.configured_local_delay = clamped_delay;
  g_manager.perf_delay_checks = 0;
  g_manager.perf_delay_matches = 0;
  g_manager.perf_delay_mismatches = 0;
  g_manager.perf_delay_missing = 0;
  g_manager.perf_delay_transitions = 0;
  g_manager.perf_delay_transition_frames = 0;
  g_manager.perf_delay_transition_min = 0;
  g_manager.perf_delay_transition_max = 0;
  NOTICE_LOG_FMT(CORE, "GekkoNet: Live local input delay changed {} -> {} at sample frame {}",
                 previous_delay, clamped_delay, g_manager.local_input_frame);
}

bool StartGekkoSession(const std::string& game_name, u32 session_id, int players, int local_player,
                       const std::vector<std::string>& player_endpoints, int local_delay,
                       int prediction_window, bool debug_p2_cstick, bool simulate_remote_p2,
                       bool stress_test)
{
  StopGekkoSession();

  std::lock_guard lk(g_manager.mutex);

  if (players < 1 || players > 4 || local_player < 1 || local_player > players)
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: Invalid session parameters (players={}, local={})", players,
                  local_player);
    return false;
  }
  if (simulate_remote_p2 && (players != 2 || local_player != 1))
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: simulated remote player 2 requires a two-player host session");
    return false;
  }
  if (stress_test && !simulate_remote_p2)
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: rollback stress test requires the simulated remote peer");
    return false;
  }

  if (!gekko_create(&g_manager.session, GekkoGameSession))
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: Failed to create session");
    return false;
  }

  const int clamped_delay = std::clamp(local_delay, 0, 60);
  const int clamped_prediction =
      std::clamp(std::max(prediction_window, stress_test ? 3 : 1), 1, 10);

  GekkoConfig config{};
  config.num_players = static_cast<unsigned char>(players);
  config.max_spectators = 0;
  config.input_prediction_window = static_cast<unsigned char>(clamped_prediction);
  config.spectator_delay = 0;
  config.input_size = static_cast<unsigned int>(sizeof(WirePad));
  config.state_size = 0;  // State is maintained internally in Dolphin's SnapshotRing
  // GekkoNet's limited-saving mode rebuilds confirmed snapshots by loading and replaying frames.
  // That is counterproductive for whole-emulator states, whose loads and replay frames are much
  // more expensive than a small game's state copy. Keep per-frame snapshots until GekkoNet has an
  // Orca-style sparse forward-snapshot policy.
  config.limited_saving = false;
  // Dolphin's rollback snapshots deliberately do not provide hashes to GekkoNet, so its checksum
  // comparison cannot produce meaningful desync results.
  config.desync_detection = false;
  config.check_distance = 10;

  gekko_start(g_manager.session, &config);

  GekkoNetAdapter* adapter = nullptr;
  if (simulate_remote_p2)
    adapter = &s_main_simulated_adapter;
  else
  {
    if (!Common::ENet::StartRollbackDatagrams(session_id))
    {
      ERROR_LOG_FMT(CORE, "GekkoNet: no shared NetPlay UDP socket is registered");
      gekko_destroy(&g_manager.session);
      return false;
    }
    adapter = &s_shared_adapter;
  }

  if (!adapter)
  {
    ERROR_LOG_FMT(CORE, "GekkoNet: Failed to create network adapter");
    gekko_destroy(&g_manager.session);
    return false;
  }
  g_manager.native_adapter_active = !simulate_remote_p2;
  gekko_net_adapter_set(g_manager.session, adapter);
  const auto destroy_failed_session = [&] {
    gekko_destroy(&g_manager.session);
    if (g_manager.native_adapter_active)
    {
      Common::ENet::StopRollbackDatagrams();
      g_manager.native_adapter_active = false;
    }
  };

  gekko_set_runahead(g_manager.session, 0);

  g_manager.players = players;
  g_manager.local_player = local_player;
  // Capture the first local device before NetPlay's boot layer remaps local SI devices to their
  // in-game seats. This remains port 0 on every client regardless of its network player number.
  g_manager.local_uses_gc_adapter =
      Config::Get(Config::GetInfoForSIDevice(0)) == SerialInterface::SIDEVICE_WIIU_ADAPTER;
  g_manager.debug_p2_cstick = debug_p2_cstick;
  g_manager.simulate_remote_p2 = simulate_remote_p2;
  g_manager.stress_test = stress_test;
  g_manager.configured_local_delay = clamped_delay;
  g_manager.session_id = session_id;
  g_manager.local_input_frame = 0;
  // GetRollbackPad is active throughout boot, before Gekko emits the first Advance event. Never
  // expose pads left latched at the end of the previous session to the next game's SI startup.
  g_manager.latched_pads.fill(GCPadStatus{});
  g_manager.local_handle = -1;
  g_manager.simulated_peer_p2_handle = -1;
  g_manager.remote_handle = -1;
  g_manager.player_handles.assign(players, -1);
  g_manager.pending_events.clear();
  g_manager.sampled_local_inputs.clear();
  g_manager.perf_window_start = {};
  g_manager.perf_dirty_pages_start = DirtyPages::GetCounters();
  g_manager.perf_real_frames = 0;
  g_manager.perf_replay_frames = 0;
  g_manager.perf_save_count = 0;
  g_manager.perf_load_count = 0;
  g_manager.perf_pump_count = 0;
  g_manager.perf_save_ms = 0.0;
  g_manager.perf_save_max_ms = 0.0;
  g_manager.perf_load_ms = 0.0;
  g_manager.perf_load_max_ms = 0.0;
  g_manager.perf_pump_ms = 0.0;
  g_manager.perf_pump_max_ms = 0.0;
  g_manager.last_local_wire.reset();
  g_manager.perf_local_input_changes = 0;
  g_manager.perf_local_byte_changes.fill(0);
  g_manager.last_applied_local_wire.reset();
  g_manager.perf_delay_checks = 0;
  g_manager.perf_delay_matches = 0;
  g_manager.perf_delay_mismatches = 0;
  g_manager.perf_delay_missing = 0;
  g_manager.perf_delay_transitions = 0;
  g_manager.perf_delay_transition_frames = 0;
  g_manager.perf_delay_transition_min = 0;
  g_manager.perf_delay_transition_max = 0;
  g_manager.frame_execution_start.reset();
  g_manager.perf_replay_exec_count = 0;
  g_manager.perf_replay_exec_ms = 0.0;
  g_manager.perf_replay_exec_max_ms = 0.0;
  g_manager.perf_replay_save_count = 0;
  g_manager.perf_replay_save_ms = 0.0;
  g_manager.perf_replay_save_max_ms = 0.0;
  g_manager.rollback_burst_start.reset();
  g_manager.perf_rollback_burst_count = 0;
  g_manager.perf_rollback_burst_ms = 0.0;
  g_manager.perf_rollback_burst_max_ms = 0.0;
  g_manager.perf_burst_load_ms = 0.0;
  g_manager.perf_burst_load_max_ms = 0.0;
  g_manager.perf_burst_exec_ms = 0.0;
  g_manager.perf_burst_exec_max_ms = 0.0;
  g_manager.perf_burst_save_ms = 0.0;
  g_manager.perf_burst_save_max_ms = 0.0;
  g_manager.perf_burst_other_ms = 0.0;
  g_manager.perf_burst_other_max_ms = 0.0;
  g_manager.current_burst_load_ms = 0.0;
  g_manager.current_burst_exec_ms = 0.0;
  g_manager.current_burst_save_ms = 0.0;
  g_manager.current_rollback_replays = 0;
  g_manager.perf_rollback_depth_total = 0;
  g_manager.perf_rollback_depth_max = 0;
  g_manager.last_real_advance.reset();
  g_manager.perf_real_interval_count = 0;
  g_manager.perf_real_interval_ms = 0.0;
  g_manager.perf_real_interval_max_ms = 0.0;
  g_manager.throttle_reference_before_load.reset();

  for (int p = 1; p <= players; ++p)
  {
    if (p == local_player)
    {
      const int handle = gekko_add_actor(g_manager.session, GekkoLocalPlayer, nullptr);
      if (handle < 0)
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Failed to add local actor");
        destroy_failed_session();
        return false;
      }
      g_manager.local_handle = handle;
      g_manager.player_handles[p - 1] = handle;
      gekko_set_local_delay(g_manager.session, handle, static_cast<unsigned char>(clamped_delay));
    }
    else if (simulate_remote_p2 && p == 2)
    {
      GekkoNetAddress addr{const_cast<char*>(SIMULATED_P2_ADDRESS.data()),
                           static_cast<unsigned int>(SIMULATED_P2_ADDRESS.size())};
      const int handle = gekko_add_actor(g_manager.session, GekkoRemotePlayer, &addr);
      if (handle < 0)
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Failed to add simulated remote player 2 actor");
        destroy_failed_session();
        return false;
      }
      g_manager.player_handles[p - 1] = handle;
      g_manager.remote_handle = handle;
    }
    else
    {
      if (p >= static_cast<int>(player_endpoints.size()) || player_endpoints[p].empty())
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Missing native endpoint for player {}", p);
        destroy_failed_session();
        return false;
      }
      NOTICE_LOG_FMT(CORE, "GekkoNet: Native player {} endpoint {}", p, player_endpoints[p]);
      ENetAddress endpoint{};
      if (!ParsePeerEndpoint(player_endpoints[p], &endpoint))
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Invalid native endpoint for player {}: {}", p,
                      player_endpoints[p]);
        destroy_failed_session();
        return false;
      }
      GekkoNetAddress addr{&endpoint, sizeof(endpoint)};
      const int handle = gekko_add_actor(g_manager.session, GekkoRemotePlayer, &addr);
      if (handle < 0)
      {
        ERROR_LOG_FMT(CORE, "GekkoNet: Failed to add remote actor");
        destroy_failed_session();
        return false;
      }
      if (g_manager.remote_handle < 0)
        g_manager.remote_handle = handle;
      g_manager.player_handles[p - 1] = handle;
    }
  }

  if (simulate_remote_p2)
  {
    ResetSimulatedLink();
    s_stress_mode = stress_test;
    if (!gekko_create(&g_manager.simulated_peer_session, GekkoGameSession))
    {
      ERROR_LOG_FMT(CORE, "GekkoNet: Failed to create simulated player 2 peer session");
      destroy_failed_session();
      return false;
    }
    gekko_start(g_manager.simulated_peer_session, &config);
    gekko_net_adapter_set(g_manager.simulated_peer_session, &s_fake_simulated_adapter);
    gekko_set_runahead(g_manager.simulated_peer_session, 0);

    GekkoNetAddress p1_addr{const_cast<char*>(SIMULATED_P1_ADDRESS.data()),
                            static_cast<unsigned int>(SIMULATED_P1_ADDRESS.size())};
    const int fake_remote_p1 =
        gekko_add_actor(g_manager.simulated_peer_session, GekkoRemotePlayer, &p1_addr);
    const int fake_local_p2 =
        gekko_add_actor(g_manager.simulated_peer_session, GekkoLocalPlayer, nullptr);
    if (fake_remote_p1 < 0 || fake_local_p2 < 0)
    {
      ERROR_LOG_FMT(CORE, "GekkoNet: Failed to add actors to simulated player 2 peer");
      gekko_destroy(&g_manager.simulated_peer_session);
      destroy_failed_session();
      return false;
    }
    g_manager.simulated_peer_p2_handle = fake_local_p2;
    gekko_set_local_delay(g_manager.simulated_peer_session, fake_local_p2,
                          static_cast<unsigned char>(stress_test ? 0 : clamped_delay));
  }

  g_manager.tracked_bitmap_clear = Config::Get(Config::NETPLAY_ROLLBACK_TRACKED_BITMAP_CLEAR);
  g_manager.full_scan_benchmark = Config::Get(Config::NETPLAY_ROLLBACK_FULL_SCAN_BENCHMARK);
  g_manager.ring = std::make_unique<SnapshotRing>(RING_SNAPSHOT_SLOTS,
                                                  g_manager.tracked_bitmap_clear,
                                                  g_manager.full_scan_benchmark);
  g_manager.stop_requested.store(false, std::memory_order_relaxed);
  g_manager.active.store(true, std::memory_order_release);

  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet: Started {} shared NetPlay UDP session (players={}, local={}, session={}, "
      "delay={}, "
      "rollback_window={}, local_input_source={}, debug_p2_cstick={}, simulate_remote_p2={}, "
      "simulated_one_way_latency_ms={}, stress_3f_every_10f={}, tracked_bitmap_clear={}, "
      "full_scan_benchmark={})",
      game_name, players, local_player, session_id, clamped_delay, clamped_prediction,
      g_manager.local_uses_gc_adapter ? "gc_adapter" : "emulated_pad", debug_p2_cstick,
      simulate_remote_p2, simulate_remote_p2 && !stress_test ? SIMULATED_P2_LATENCY.count() : 0,
      stress_test, g_manager.tracked_bitmap_clear, g_manager.full_scan_benchmark);
  return true;
}

void StopGekkoSession()
{
  std::lock_guard lk(g_manager.mutex);
  const bool had_session = g_manager.active.load(std::memory_order_relaxed) || g_manager.session ||
                           g_manager.simulated_peer_session || g_manager.native_adapter_active;

  g_manager.stop_requested.store(true, std::memory_order_relaxed);
  g_manager.active.store(false, std::memory_order_release);

  if (g_manager.session)
  {
    gekko_destroy(&g_manager.session);
    g_manager.session = nullptr;
  }
  if (g_manager.simulated_peer_session)
  {
    gekko_destroy(&g_manager.simulated_peer_session);
    g_manager.simulated_peer_session = nullptr;
  }
  if (g_manager.native_adapter_active)
  {
    Common::ENet::StopRollbackDatagrams();
    g_manager.native_adapter_active = false;
  }
  ResetSimulatedLink();
  g_manager.pending_events.clear();
  g_manager.throttle_reference_before_load.reset();
  if (g_manager.ring)
    g_manager.ring->Reset(Core::System::GetInstance());
  g_manager.ring.reset();
  Rollback::SetResimulating(false);
  if (had_session)
    NOTICE_LOG_FMT(CORE, "GekkoNet: Session stopped");
}

static void ApplyDebugCStickPattern(GCPadStatus* status, u64 frame)
{
  // Change to the next of eight directions every two frames. Diagonals are normalized to
  // approximately the same radius as cardinal directions.
  constexpr int DIRECTION_FRAMES = 2;
  constexpr std::array<std::pair<int, int>, 8> DIRECTIONS{{
      {90, 0},
      {64, -64},
      {0, -90},
      {-64, -64},
      {-90, 0},
      {-64, 64},
      {0, 90},
      {64, 64},
  }};

  const auto [x, y] = DIRECTIONS[(frame / DIRECTION_FRAMES) % DIRECTIONS.size()];
  status->substickX = static_cast<u8>(GCPadStatus::C_STICK_CENTER_X + x);
  status->substickY = static_cast<u8>(GCPadStatus::C_STICK_CENTER_Y + y);

  // Mash both triggers on even frames and release them on odd frames. Override both the digital
  // click and analog pressure so the wire input changes unambiguously every frame.
  constexpr u16 TRIGGER_BUTTONS = PAD_TRIGGER_L | PAD_TRIGGER_R;
  const bool triggers_pressed = (frame & 1) == 0;
  if (triggers_pressed)
    status->button |= TRIGGER_BUTTONS;
  else
    status->button &= ~TRIGGER_BUTTONS;
  status->triggerLeft = triggers_pressed ? 255 : 0;
  status->triggerRight = triggers_pressed ? 255 : 0;
}

static void ApplyStressCStickPattern(GCPadStatus* status, u64 frame)
{
  // A transition every ten frames defeats repeat-last-input prediction. Alternating left/right
  // avoids a neutral interval whose first prediction could accidentally be correct.
  const bool right = ((frame / STRESS_INPUT_PERIOD_FRAMES) & 1) == 0;
  status->substickX = static_cast<u8>(GCPadStatus::C_STICK_CENTER_X + (right ? 90 : -90));
  status->substickY = GCPadStatus::C_STICK_CENTER_Y;
}

static void SubmitLocalInput()
{
  g_controller_interface.SetCurrentInputChannel(ciface::InputChannel::SerialInterface);
  g_controller_interface.UpdateInput();

  // The Gekko player number is an in-game/network slot, not a local controller index.
  // Like Dolphin NetPlay, each client maps its first configured local pad to its assigned slot.
  // Using local_player - 1 here made player 2 read GCPad2 even though its controller is GCPad1.
  constexpr int LOCAL_PAD = 0;
  GCPadStatus local_status;
  if (g_manager.local_uses_gc_adapter)
  {
    local_status = GCAdapter::Input(LOCAL_PAD);
  }
  else
  {
    local_status = Pad::GetStatus(LOCAL_PAD);
  }
  if (g_manager.debug_p2_cstick && g_manager.local_player == 2)
    ApplyDebugCStickPattern(&local_status, g_manager.local_input_frame);
  WirePad wire = EncodePad(local_status);

  if (g_manager.last_local_wire)
  {
    bool changed = false;
    for (std::size_t i = 0; i < sizeof(WirePad); ++i)
    {
      if (wire.data[i] != g_manager.last_local_wire->data[i])
      {
        changed = true;
        ++g_manager.perf_local_byte_changes[i];
      }
    }
    if (changed)
      ++g_manager.perf_local_input_changes;
  }
  g_manager.last_local_wire = wire;

  // Keep the raw host sample beside the Gekko frame on which it was submitted. When Gekko later
  // emits the local player's input for an Advance event, this lets diagnostics verify the actual
  // delay behavior instead of merely reporting the configured value.
  g_manager.sampled_local_inputs.push_back(
      {static_cast<s64>(g_manager.local_input_frame), wire});

  gekko_add_local_input(g_manager.session, g_manager.local_handle, &wire);

  if (g_manager.simulated_peer_session && g_manager.simulated_peer_p2_handle >= 0)
  {
    GCPadStatus simulated_p2{};
    if (g_manager.stress_test)
      ApplyStressCStickPattern(&simulated_p2, g_manager.local_input_frame);
    else
      ApplyDebugCStickPattern(&simulated_p2, g_manager.local_input_frame);
    WirePad simulated_wire = EncodePad(simulated_p2);
    gekko_add_local_input(g_manager.simulated_peer_session, g_manager.simulated_peer_p2_handle,
                          &simulated_wire);
  }
  ++g_manager.local_input_frame;
  s_simulated_link_frame = g_manager.local_input_frame;
}

static void PumpSimulatedPeer()
{
  if (!g_manager.simulated_peer_session)
    return;

  gekko_network_poll(g_manager.simulated_peer_session);
  // Once deterministic frame delay is active, advance the hidden protocol peer exactly once per
  // visible real frame. Repeated calls in the 100 us wait loop still poll packets but cannot make
  // the hidden session race ahead and change the requested rollback depth.
  if (g_manager.stress_test && s_stress_frame_delay_active &&
      s_last_stress_peer_update_frame == s_simulated_link_frame)
  {
    return;
  }
  if (g_manager.stress_test && s_stress_frame_delay_active)
    s_last_stress_peer_update_frame = s_simulated_link_frame;

  int event_count = 0;
  GekkoGameEvent** events = gekko_update_session(g_manager.simulated_peer_session, &event_count);
  for (int i = 0; i < event_count; ++i)
  {
    GekkoGameEvent* event = events[i];
    if (!event)
      continue;
    // The hidden peer exists only to exercise GekkoNet's real remote-input path. Its emulation
    // state is the visible Dolphin instance, so acknowledge peer-side state events without making
    // a second, non-authoritative save/load implementation.
    if (event->type == GekkoSaveEvent)
    {
      if (event->data.save.checksum)
        *event->data.save.checksum = 0;
      if (event->data.save.state_len)
        *event->data.save.state_len = 0;
    }
  }
  int session_event_count = 0;
  gekko_session_events(g_manager.simulated_peer_session, &session_event_count);
}

static void LatchInputs(const unsigned char* raw_inputs)
{
  if (!raw_inputs)
    return;

  for (int p = 0; p < g_manager.players; ++p)
  {
    const int handle = g_manager.player_handles[p];
    if (handle >= 0 && handle < g_manager.players)
    {
      WirePad wire{};
      std::memcpy(&wire, raw_inputs + (handle * sizeof(WirePad)), sizeof(WirePad));
      g_manager.latched_pads[p] = DecodePad(wire);
    }
  }
}

static bool InputsEqual(const WirePad& lhs, const WirePad& rhs)
{
  return std::memcmp(&lhs, &rhs, sizeof(WirePad)) == 0;
}

static void MeasureLocalInputDelay(int advance_frame, const std::vector<unsigned char>& inputs)
{
  if (g_manager.local_handle < 0)
    return;

  const std::size_t input_offset =
      static_cast<std::size_t>(g_manager.local_handle) * sizeof(WirePad);
  if (inputs.size() < input_offset + sizeof(WirePad))
  {
    ++g_manager.perf_delay_missing;
    return;
  }

  WirePad applied{};
  std::memcpy(&applied, inputs.data() + input_offset, sizeof(WirePad));

  const s64 expected_sample_frame =
      static_cast<s64>(advance_frame) - g_manager.configured_local_delay;
  if (expected_sample_frame >= 0)
  {
    const auto expected = std::find_if(
        g_manager.sampled_local_inputs.begin(), g_manager.sampled_local_inputs.end(),
        [expected_sample_frame](const SampledLocalInput& sample) {
          return sample.frame == expected_sample_frame;
        });
    if (expected == g_manager.sampled_local_inputs.end())
    {
      ++g_manager.perf_delay_missing;
    }
    else
    {
      ++g_manager.perf_delay_checks;
      if (InputsEqual(applied, expected->input))
        ++g_manager.perf_delay_matches;
      else
        ++g_manager.perf_delay_mismatches;
    }
  }

  // An applied transition can be matched to the newest identical sampled transition. This is an
  // independent observation of the effective delay and will report zero if the host bypasses the
  // configured Gekko delay. It is intentionally transition-based because an unchanged held input
  // cannot reveal how many frames it was delayed.
  if (g_manager.last_applied_local_wire &&
      !InputsEqual(applied, *g_manager.last_applied_local_wire))
  {
    for (std::size_t i = g_manager.sampled_local_inputs.size(); i-- > 1;)
    {
      const SampledLocalInput& sample = g_manager.sampled_local_inputs[i];
      const SampledLocalInput& previous = g_manager.sampled_local_inputs[i - 1];
      if (sample.frame > advance_frame || !InputsEqual(sample.input, applied) ||
          InputsEqual(sample.input, previous.input))
      {
        continue;
      }

      const u64 observed_delay = static_cast<u64>(advance_frame - sample.frame);
      ++g_manager.perf_delay_transitions;
      g_manager.perf_delay_transition_frames += observed_delay;
      if (g_manager.perf_delay_transitions == 1)
        g_manager.perf_delay_transition_min = observed_delay;
      else
        g_manager.perf_delay_transition_min =
            std::min(g_manager.perf_delay_transition_min, observed_delay);
      g_manager.perf_delay_transition_max =
          std::max(g_manager.perf_delay_transition_max, observed_delay);
      break;
    }
  }
  g_manager.last_applied_local_wire = applied;

  constexpr s64 SAMPLE_HISTORY_FRAMES = 64;
  const s64 oldest_needed = static_cast<s64>(advance_frame) - SAMPLE_HISTORY_FRAMES;
  while (g_manager.sampled_local_inputs.size() > 2 &&
         g_manager.sampled_local_inputs[1].frame < oldest_needed)
  {
    g_manager.sampled_local_inputs.pop_front();
  }
}

static void QueueEvents(GekkoGameEvent** events, int event_count)
{
  for (int i = 0; i < event_count; ++i)
  {
    const GekkoGameEvent* event = events[i];
    if (!event)
      continue;

    QueuedEvent queued;
    queued.type = event->type;
    switch (event->type)
    {
    case GekkoSaveEvent:
      queued.frame = event->data.save.frame;
      queued.checksum = event->data.save.checksum;
      queued.state_len = event->data.save.state_len;
      break;
    case GekkoLoadEvent:
      queued.frame = event->data.load.frame;
      break;
    case GekkoAdvanceEvent:
      queued.frame = event->data.adv.frame;
      queued.rolling_back = event->data.adv.rolling_back;
      queued.running_ahead = event->data.adv.running_ahead;
      if (event->data.adv.inputs && event->data.adv.input_len != 0)
      {
        queued.inputs.assign(event->data.adv.inputs,
                             event->data.adv.inputs + event->data.adv.input_len);
      }
      break;
    default:
      break;
    }
    g_manager.pending_events.emplace_back(std::move(queued));
  }
}

static double ElapsedMs(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
      .count();
}

// Native GekkoNet uses frame -1 for the snapshot immediately before frame 0. SnapshotRing uses
// -1 as its empty-slot sentinel, so offset every Gekko frame by one without changing what Orca
// saves or restores.
static s64 SnapshotFrameKey(int gekko_frame)
{
  return static_cast<s64>(gekko_frame) + 1;
}

static void RecordPumpTime(std::chrono::steady_clock::time_point start)
{
  const double elapsed = ElapsedMs(start);
  ++g_manager.perf_pump_count;
  g_manager.perf_pump_ms += elapsed;
  g_manager.perf_pump_max_ms = std::max(g_manager.perf_pump_max_ms, elapsed);
}

static void MaybeLogPerformance()
{
  constexpr u64 REPORT_FRAMES = 300;
  if (g_manager.perf_real_frames < REPORT_FRAMES)
    return;

  const auto now = std::chrono::steady_clock::now();
  const double wall_ms =
      std::chrono::duration<double, std::milli>(now - g_manager.perf_window_start).count();
  const double fps = wall_ms > 0.0 ? g_manager.perf_real_frames * 1000.0 / wall_ms : 0.0;
  const auto average = [](double total, u64 count) { return count ? total / count : 0.0; };
  const DirtyPages::Counters dirty_pages = DirtyPages::GetCounters();
  const Common::ENet::RollbackDatagramStats udp = Common::ENet::GetRollbackDatagramStats();

  NOTICE_LOG_FMT(
      CORE,
      "GekkoNet perf: {:.1f} fps over {} real frames; save {:.3f}/{:.3f} ms avg/max ({}); "
      "pump {:.3f}/{:.3f} ms ({}); load {:.3f}/{:.3f} ms ({}); replays {}; "
      "replay_exec {:.3f}/{:.3f} ms ({}); replay_save {:.3f}/{:.3f} ms ({}); "
      "burst {:.3f}/{:.3f} ms ({}); burst_parts load {:.3f}/{:.3f} exec {:.3f}/{:.3f} "
      "save {:.3f}/{:.3f} other {:.3f}/{:.3f} ms avg/max; "
      "real_interval {:.3f}/{:.3f} ms ({}); input_changes {} bytes [{},{},{},{},{},{},{},{}]; "
      "delay_measure cfg={} verified={}/{} mismatch={} missing={} observed "
      "{:.2f}/{}/{} frames avg/min/max ({} transitions); "
      "rollback_depth {:.2f}/{} avg/max; RAM dirty/saved/unchanged pages {}/{}/{}; "
      "udp tx/rx/reject {}/{}/{}; ahead={:.2f}; stress_3f_every_10f={}; "
      "tracked_bitmap_clear={}; full_scan_benchmark={}",
      fps, g_manager.perf_real_frames, average(g_manager.perf_save_ms, g_manager.perf_save_count),
      g_manager.perf_save_max_ms, g_manager.perf_save_count,
      average(g_manager.perf_pump_ms, g_manager.perf_pump_count), g_manager.perf_pump_max_ms,
      g_manager.perf_pump_count, average(g_manager.perf_load_ms, g_manager.perf_load_count),
      g_manager.perf_load_max_ms, g_manager.perf_load_count, g_manager.perf_replay_frames,
      average(g_manager.perf_replay_exec_ms, g_manager.perf_replay_exec_count),
      g_manager.perf_replay_exec_max_ms, g_manager.perf_replay_exec_count,
      average(g_manager.perf_replay_save_ms, g_manager.perf_replay_save_count),
      g_manager.perf_replay_save_max_ms, g_manager.perf_replay_save_count,
      average(g_manager.perf_rollback_burst_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_rollback_burst_max_ms, g_manager.perf_rollback_burst_count,
      average(g_manager.perf_burst_load_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_burst_load_max_ms,
      average(g_manager.perf_burst_exec_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_burst_exec_max_ms,
      average(g_manager.perf_burst_save_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_burst_save_max_ms,
      average(g_manager.perf_burst_other_ms, g_manager.perf_rollback_burst_count),
      g_manager.perf_burst_other_max_ms,
      average(g_manager.perf_real_interval_ms, g_manager.perf_real_interval_count),
      g_manager.perf_real_interval_max_ms, g_manager.perf_real_interval_count,
      g_manager.perf_local_input_changes, g_manager.perf_local_byte_changes[0],
      g_manager.perf_local_byte_changes[1], g_manager.perf_local_byte_changes[2],
      g_manager.perf_local_byte_changes[3], g_manager.perf_local_byte_changes[4],
      g_manager.perf_local_byte_changes[5], g_manager.perf_local_byte_changes[6],
      g_manager.perf_local_byte_changes[7],
      g_manager.configured_local_delay, g_manager.perf_delay_matches,
      g_manager.perf_delay_checks, g_manager.perf_delay_mismatches,
      g_manager.perf_delay_missing,
      average(static_cast<double>(g_manager.perf_delay_transition_frames),
              g_manager.perf_delay_transitions),
      g_manager.perf_delay_transition_min, g_manager.perf_delay_transition_max,
      g_manager.perf_delay_transitions,
      average(static_cast<double>(g_manager.perf_rollback_depth_total),
              g_manager.perf_rollback_burst_count),
      g_manager.perf_rollback_depth_max,
      dirty_pages.dirty_pages - g_manager.perf_dirty_pages_start.dirty_pages,
      dirty_pages.pages_recorded - g_manager.perf_dirty_pages_start.pages_recorded,
      (dirty_pages.dirty_pages - g_manager.perf_dirty_pages_start.dirty_pages) -
          (dirty_pages.pages_recorded - g_manager.perf_dirty_pages_start.pages_recorded),
      udp.sent, udp.received, udp.rejected, gekko_frames_ahead(g_manager.session),
      g_manager.stress_test, g_manager.tracked_bitmap_clear, g_manager.full_scan_benchmark);

  g_manager.perf_window_start = now;
  g_manager.perf_dirty_pages_start = dirty_pages;
  g_manager.perf_real_frames = 0;
  g_manager.perf_replay_frames = 0;
  g_manager.perf_save_count = 0;
  g_manager.perf_load_count = 0;
  g_manager.perf_pump_count = 0;
  g_manager.perf_save_ms = 0.0;
  g_manager.perf_save_max_ms = 0.0;
  g_manager.perf_load_ms = 0.0;
  g_manager.perf_load_max_ms = 0.0;
  g_manager.perf_pump_ms = 0.0;
  g_manager.perf_pump_max_ms = 0.0;
  g_manager.perf_local_input_changes = 0;
  g_manager.perf_local_byte_changes.fill(0);
  g_manager.perf_delay_checks = 0;
  g_manager.perf_delay_matches = 0;
  g_manager.perf_delay_mismatches = 0;
  g_manager.perf_delay_missing = 0;
  g_manager.perf_delay_transitions = 0;
  g_manager.perf_delay_transition_frames = 0;
  g_manager.perf_delay_transition_min = 0;
  g_manager.perf_delay_transition_max = 0;
  g_manager.perf_replay_exec_count = 0;
  g_manager.perf_replay_exec_ms = 0.0;
  g_manager.perf_replay_exec_max_ms = 0.0;
  g_manager.perf_replay_save_count = 0;
  g_manager.perf_replay_save_ms = 0.0;
  g_manager.perf_replay_save_max_ms = 0.0;
  g_manager.perf_rollback_burst_count = 0;
  g_manager.perf_rollback_burst_ms = 0.0;
  g_manager.perf_rollback_burst_max_ms = 0.0;
  g_manager.perf_burst_load_ms = 0.0;
  g_manager.perf_burst_load_max_ms = 0.0;
  g_manager.perf_burst_exec_ms = 0.0;
  g_manager.perf_burst_exec_max_ms = 0.0;
  g_manager.perf_burst_save_ms = 0.0;
  g_manager.perf_burst_save_max_ms = 0.0;
  g_manager.perf_burst_other_ms = 0.0;
  g_manager.perf_burst_other_max_ms = 0.0;
  g_manager.perf_rollback_depth_total = 0;
  g_manager.perf_rollback_depth_max = 0;
  g_manager.perf_real_interval_count = 0;
  g_manager.perf_real_interval_ms = 0.0;
  g_manager.perf_real_interval_max_ms = 0.0;
}

// Consumes state operations up to one advance. Returning true means that the caller must return
// to the CPU so the selected frame can actually run. GekkoNet can emit a complete rollback as one
// batch (load, several replay advances/saves, then a real advance); Dolphin must spread that batch
// across real frame boundaries instead of merely relatching all of its inputs in one callback.
static bool PrepareQueuedFrame(Core::System& system)
{
  while (!g_manager.pending_events.empty())
  {
    QueuedEvent event = std::move(g_manager.pending_events.front());
    g_manager.pending_events.pop_front();

    switch (event.type)
    {
    case GekkoSaveEvent:
      if (event.frame >= -1 && g_manager.ring)
      {
        const auto start = std::chrono::steady_clock::now();
        g_manager.ring->Save(system, SnapshotFrameKey(event.frame));
        const double elapsed = ElapsedMs(start);
        ++g_manager.perf_save_count;
        g_manager.perf_save_ms += elapsed;
        g_manager.perf_save_max_ms = std::max(g_manager.perf_save_max_ms, elapsed);
        if (Rollback::IsResimulating() && g_manager.rollback_burst_start)
        {
          ++g_manager.perf_replay_save_count;
          g_manager.perf_replay_save_ms += elapsed;
          g_manager.perf_replay_save_max_ms =
              std::max(g_manager.perf_replay_save_max_ms, elapsed);
          g_manager.current_burst_save_ms += elapsed;
        }
        if (event.checksum)
          *event.checksum = 0;
        if (event.state_len)
          *event.state_len = 0;
      }
      break;

    case GekkoLoadEvent:
      if (event.frame >= -1 && g_manager.ring)
      {
        DEBUG_LOG_FMT(CORE, "GekkoNet: Loading rollback frame {}", event.frame);
        if (!Rollback::IsResimulating() && !g_manager.throttle_reference_before_load)
        {
          g_manager.throttle_reference_before_load = system.GetCoreTiming().GetThrottleReference();
        }
        if (!g_manager.rollback_burst_start)
        {
          g_manager.rollback_burst_start = std::chrono::steady_clock::now();
          g_manager.current_rollback_replays = 0;
          g_manager.current_burst_load_ms = 0.0;
          g_manager.current_burst_exec_ms = 0.0;
          g_manager.current_burst_save_ms = 0.0;
        }
        const auto start = std::chrono::steady_clock::now();
        if (!g_manager.ring->Load(system, SnapshotFrameKey(event.frame)))
        {
          ERROR_LOG_FMT(CORE, "GekkoNet: Failed to restore rollback frame {}", event.frame);
          g_manager.stop_requested.store(true, std::memory_order_relaxed);
          return false;
        }
        const double elapsed = ElapsedMs(start);
        ++g_manager.perf_load_count;
        g_manager.perf_load_ms += elapsed;
        g_manager.perf_load_max_ms = std::max(g_manager.perf_load_max_ms, elapsed);
        g_manager.current_burst_load_ms += elapsed;
      }
      break;

    case GekkoAdvanceEvent:
    {
      const bool resimulating = event.rolling_back || event.running_ahead;
      if (!resimulating)
        MeasureLocalInputDelay(event.frame, event.inputs);
      LatchInputs(event.inputs.data());
      Rollback::SetResimulating(resimulating);
      if (!resimulating && g_manager.throttle_reference_before_load)
      {
        system.GetCoreTiming().SetThrottleReference(*g_manager.throttle_reference_before_load);
        g_manager.throttle_reference_before_load.reset();
      }
      const auto advance_now = std::chrono::steady_clock::now();
      if (!resimulating && g_manager.rollback_burst_start)
      {
        const double elapsed =
            std::chrono::duration<double, std::milli>(advance_now - *g_manager.rollback_burst_start)
                .count();
        ++g_manager.perf_rollback_burst_count;
        g_manager.perf_rollback_burst_ms += elapsed;
        g_manager.perf_rollback_burst_max_ms =
            std::max(g_manager.perf_rollback_burst_max_ms, elapsed);
        const double other_ms =
            std::max(0.0, elapsed - g_manager.current_burst_load_ms -
                              g_manager.current_burst_exec_ms - g_manager.current_burst_save_ms);
        g_manager.perf_burst_load_ms += g_manager.current_burst_load_ms;
        g_manager.perf_burst_load_max_ms =
            std::max(g_manager.perf_burst_load_max_ms, g_manager.current_burst_load_ms);
        g_manager.perf_burst_exec_ms += g_manager.current_burst_exec_ms;
        g_manager.perf_burst_exec_max_ms =
            std::max(g_manager.perf_burst_exec_max_ms, g_manager.current_burst_exec_ms);
        g_manager.perf_burst_save_ms += g_manager.current_burst_save_ms;
        g_manager.perf_burst_save_max_ms =
            std::max(g_manager.perf_burst_save_max_ms, g_manager.current_burst_save_ms);
        g_manager.perf_burst_other_ms += other_ms;
        g_manager.perf_burst_other_max_ms =
            std::max(g_manager.perf_burst_other_max_ms, other_ms);
        g_manager.perf_rollback_depth_total += g_manager.current_rollback_replays;
        g_manager.perf_rollback_depth_max =
            std::max(g_manager.perf_rollback_depth_max, g_manager.current_rollback_replays);
        g_manager.current_rollback_replays = 0;
        g_manager.rollback_burst_start.reset();
      }
      if (resimulating)
      {
        ++g_manager.perf_replay_frames;
        ++g_manager.current_rollback_replays;
      }
      else
      {
        if (g_manager.last_real_advance)
        {
          const double interval =
              std::chrono::duration<double, std::milli>(advance_now - *g_manager.last_real_advance)
                  .count();
          ++g_manager.perf_real_interval_count;
          g_manager.perf_real_interval_ms += interval;
          g_manager.perf_real_interval_max_ms =
              std::max(g_manager.perf_real_interval_max_ms, interval);
        }
        g_manager.last_real_advance = advance_now;
        if (g_manager.perf_real_frames == 0)
          g_manager.perf_window_start = std::chrono::steady_clock::now();
        ++g_manager.perf_real_frames;
      }
      g_manager.frame_execution_start = advance_now;
      g_manager.frame_execution_resim = resimulating;
      system.GetSerialInterface().RelatchInputs();
      if (event.rolling_back || event.running_ahead)
      {
        DEBUG_LOG_FMT(CORE, "GekkoNet: Replaying frame {} (rollback={}, runahead={})", event.frame,
                      event.rolling_back, event.running_ahead);
      }
      return true;
    }

    default:
      break;
    }
  }
  return false;
}

void OnFrameBoundary(const Core::CPUThreadGuard& guard)
{
  auto& system = guard.GetSystem();
  HostFloatScope float_scope(system.GetPPCState());

  if (!g_manager.active.load(std::memory_order_relaxed))
    return;

  // A launcher installs this hook before Brawl is in RAM. Until the expected instruction is live,
  // whatever happens to execute at this address is not Brawl's frame boundary.
  const u32 opcode = system.GetMemory().Read_U32(BRAWL_FRAME_HOOK_ADDR & 0x1FFFFFFF);
  if (opcode != BRAWL_EXPECTED_OPCODE)
    return;

  // Inside a JIT block PC and NPC are stale. Pin both to the boundary so snapshots resume at the
  // hook. This is a Start hook, so the original instruction executes normally after this returns.
  auto& ppc_state = system.GetPPCState();
  ppc_state.pc = BRAWL_FRAME_HOOK_ADDR;
  ppc_state.npc = BRAWL_FRAME_HOOK_ADDR;

  std::lock_guard lk(g_manager.mutex);
  if (!g_manager.session)
    return;

  if (g_manager.frame_execution_start)
  {
    const double elapsed = ElapsedMs(*g_manager.frame_execution_start);
    if (g_manager.frame_execution_resim)
    {
      ++g_manager.perf_replay_exec_count;
      g_manager.perf_replay_exec_ms += elapsed;
      g_manager.perf_replay_exec_max_ms = std::max(g_manager.perf_replay_exec_max_ms, elapsed);
      if (g_manager.rollback_burst_start)
        g_manager.current_burst_exec_ms += elapsed;
    }
    g_manager.frame_execution_start.reset();
  }

  MaybeLogPerformance();

  // A rollback batch is replayed one emulated frame at a time. State saves left after the prior
  // advance are intentionally handled now, after that frame has completed.
  if (PrepareQueuedFrame(system))
    return;

  // Submit before polling so this boundary's inputs are eligible to be sent immediately. The
  // simulated peer uses the same GekkoNet path, with packets released after the configured delay.
  SubmitLocalInput();
  PumpSimulatedPeer();
  gekko_network_poll(g_manager.session);

  // Symmetric timesync pacing (RMG-K parameters). Tight deadzone + fast lerp keeps
  // framesAhead near zero so both players share prediction/rollback load.
  static constexpr double kSymDeadzone = 0.20;
  static constexpr double kSymStrength = 0.015;
  static constexpr double kSymMinScale = 0.97;
  static constexpr double kSymMaxScale = 1.03;
  static constexpr double kSymLerp = 0.35;

  const float frames_ahead = gekko_frames_ahead(g_manager.session);

  double newTarget = 1.0;
  if (frames_ahead >= static_cast<float>(kSymDeadzone) ||
      frames_ahead <= -static_cast<float>(kSymDeadzone))
  {
    newTarget = 1.0 - (static_cast<double>(frames_ahead) * kSymStrength);
    newTarget = std::clamp(newTarget, kSymMinScale, kSymMaxScale);
  }
  g_manager.target_scale = newTarget;

  g_manager.speed_scale += (g_manager.target_scale - g_manager.speed_scale) * kSymLerp;
  Core::System::GetInstance().GetCoreTiming().SetTimesyncScale(
      static_cast<float>(g_manager.speed_scale));

  static u32 s_boundary_ticks = 0;
  if (++s_boundary_ticks % 60 == 1)
  {
    NOTICE_LOG_FMT(CORE,
                   "GekkoNet: Frame boundary tick #{}, local_player={}, ahead={:.2f}, "
                   "target={:.4f}, scale={:.4f}",
                   s_boundary_ticks, g_manager.local_player, frames_ahead, g_manager.target_scale,
                   g_manager.speed_scale);
  }

  // Run GekkoNet event pump - must wait for an advance event each frame.
  const auto pump_start = std::chrono::steady_clock::now();
  for (;;)
  {
    if (g_manager.stop_requested.load(std::memory_order_relaxed))
      return;

    int event_count = 0;
    GekkoGameEvent** events = gekko_update_session(g_manager.session, &event_count);

    // Check session lifecycle events
    int s_count = 0;
    GekkoSessionEvent** s_events = gekko_session_events(g_manager.session, &s_count);
    for (int s = 0; s < s_count; ++s)
    {
      const auto* sev = s_events[s];
      if (!sev)
        continue;
      switch (sev->type)
      {
      case GekkoPlayerSyncing:
        NOTICE_LOG_FMT(CORE, "GekkoNet: Player handle {} syncing ({}/{})", sev->data.syncing.handle,
                       sev->data.syncing.current, sev->data.syncing.max);
        break;
      case GekkoPlayerConnected:
        NOTICE_LOG_FMT(CORE, "GekkoNet: Player handle {} connected!", sev->data.connected.handle);
        break;
      case GekkoSessionStarted:
        NOTICE_LOG_FMT(CORE, "GekkoNet: Session started! Rollback active.");
        if (g_manager.stress_test && !s_stress_frame_delay_active)
        {
          s_stress_frame_delay_active = true;
          s_last_stress_peer_update_frame = 0;
          NOTICE_LOG_FMT(CORE, "GekkoNet benchmark: deterministic 3-frame packets active; Player 2 "
                               "changes input every 10 frames");
        }
        break;
      default:
        break;
      }
    }

    QueueEvents(events, event_count);
    if (PrepareQueuedFrame(system))
    {
      RecordPumpTime(pump_start);
      return;
    }

    // No advance yet - poll network and retry.
    // A sub-millisecond std::this_thread::sleep_for can consume a full scheduler quantum on
    // Windows. That turns a brief GekkoNet wait into a lost emulated frame. Dolphin's precision
    // timer spins for this short interval and preserves the intended 100 us polling cadence.
    g_manager.wait_timer.SleepUntil(Clock::now() + std::chrono::microseconds(WAIT_SLEEP_US));
    PumpSimulatedPeer();
    gekko_network_poll(g_manager.session);
  }
}

}  // namespace Rollback
