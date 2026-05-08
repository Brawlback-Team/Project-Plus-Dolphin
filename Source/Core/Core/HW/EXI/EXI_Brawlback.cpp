#include "EXI_Brawlback.h"
#include <Core/Brawlback/include/brawlback-common/ExiStructures.h>
#include <algorithm>
#include <chrono>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <vector>
#include "Core/ConfigManager.h"
#include "Core/HW/Memmap.h"
#include "VideoCommon/OnScreenDisplay.h"
#include <regex>
#include <Core/System.h>
#include <chrono>
#include "Common/CommonTypes.h"
#include <Common/MemoryUtil.h>
#include <Core/NetPlayClient.h>
// --- Mutexes
std::mutex read_queue_mutex = std::mutex();
// -------------------------------
// https://mklimenko.github.io/english/2018/08/22/robust-endian-swap/
template <typename T>
T swap_endian(T val)
{
  union U
  {
    T val;
    std::array<std::uint8_t, sizeof(T)> raw;
  } src, dst;

  src.val = val;
  std::reverse_copy(src.raw.begin(), src.raw.end(), dst.raw.begin());
  val = dst.val;
  return val;
}

void CEXIBrawlback::handleEndFrame()
{
  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode())
  {
    NetPlay::OnFrameEnd();
  }
}
inline void SwapEndianSavestateMemRegionInfo(SavestateMemRegionInfo& memRegion)
{
  memRegion.address = swap_endian(memRegion.address);
  memRegion.size = swap_endian(memRegion.size);
}
void CEXIBrawlback::handleEndLoop()
{
  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode())
  {
    if (NetPlay::IsRollingBack() && NetPlay::CurrentFrame() != NetPlay::StopFrame())
    {
      NetPlay::IncrementCurrentFrame();
    }
  }
}
inline void SwapBrawlbackPadDataEndianess(BrawlbackPad& pad)
{
  pad._buttons = swap_endian(pad._buttons);
  pad.buttons = swap_endian(pad.buttons);
  pad.holdButtons = swap_endian(pad.holdButtons);
  pad.rapidFireButtons = swap_endian(pad.rapidFireButtons);
  pad.releasedButtons = swap_endian(pad.releasedButtons);
  pad.newPressedButtons = swap_endian(pad.newPressedButtons);
}
void CEXIBrawlback::handleStartLoopSize(u8* payload)
{
  struct StartLoopPayloadInfo
  {
    bu32 payloadSize;
    bu32 exiMaxPayloadBytes;
  };

  StartLoopPayloadInfo info;
  memcpy(&info, payload, sizeof(StartLoopPayloadInfo));

  info.payloadSize = swap_endian(info.payloadSize);
  info.exiMaxPayloadBytes = swap_endian(info.exiMaxPayloadBytes);

  start_loop_payload_size = info.payloadSize;               // Total bytes in payload
  start_loop_max_chunk_size = info.exiMaxPayloadBytes - 1;  // Subtract 1 for command byte

  INFO_LOG_FMT(BRAWLBACK, "PAYLOAD SIZE: {} - MAX SIZE: {}\n", start_loop_payload_size,
               start_loop_max_chunk_size);

  // Reset buffer for new data
  start_loop_buffer.clear();

  // Handle case where payload size is 0
  if (start_loop_payload_size == 0)
  {
    INFO_LOG_FMT(BRAWLBACK, "Warning: Payload size is 0, skipping buffer operations");
    start_loop_bytes_received = 0;
    return;
  }

  start_loop_buffer.reserve(start_loop_payload_size);
  start_loop_bytes_received = 0;
}

void CEXIBrawlback::handleStartLoop(u8* payload)
{
  // Skip if payload size is 0
  if (start_loop_payload_size == 0)
  {
    return;
  }

  // Calculate how many bytes we expect in this chunk
  int bytes_remaining = start_loop_payload_size - start_loop_bytes_received;
  int expected_chunk_size = std::min(bytes_remaining, start_loop_max_chunk_size);

  // Append this chunk to the buffer
  start_loop_buffer.insert(start_loop_buffer.end(), payload, payload + expected_chunk_size);
  start_loop_bytes_received += expected_chunk_size;

  // Check if we've received all the data
  if (start_loop_bytes_received >= start_loop_payload_size)
  {
    handleStartLoopComplete();
  }
}

void CEXIBrawlback::handleStartLoopRollback(u8* payload)
{
  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode()) 
  {
    BrawlbackPad pad;
    memcpy(&pad, payload, sizeof(BrawlbackPad));
    SwapBrawlbackPadDataEndianess(pad);
    NetPlay::OnFrameStart(pad);
    
    s32 advanceFrames = NetPlay::AdvanceFrames();
    {
      std::lock_guard<std::mutex> lock(read_queue_mutex);
      this->read_queue.clear();
      auto dataPtr = reinterpret_cast<u8*>(&advanceFrames);
      this->read_queue.insert(this->read_queue.end(), dataPtr, dataPtr + sizeof(s32));
    }
  }
}
void CEXIBrawlback::handleStartLoopComplete()
{
  struct AllocationRegionEntry
  {
    bu32 address;
    bu32 size;
  };
  struct AllocationRegionDataHeader
  {
    bu32 regionCount;
  };

  // Parse header from accumulated buffer
  AllocationRegionDataHeader header;
  memcpy(&header, start_loop_buffer.data(), sizeof(AllocationRegionDataHeader));

  header.regionCount = swap_endian(header.regionCount);

  u32 savestateIndex = NetPlay::CurrentFrame() % 5;
  savestates[savestateIndex].frame = NetPlay::CurrentFrame();

  // Clear existing buffers for this savestate
  for (State& state : savestates[savestateIndex].states)
  {
    if (state.buffer != nullptr)
    {
      Common::FreeAlignedMemory(state.buffer);
    }
  }
  savestates[savestateIndex].states.clear();

  // Parse entries array (comes after header in buffer)
  AllocationRegionEntry* entries = reinterpret_cast<AllocationRegionEntry*>(
      start_loop_buffer.data() + sizeof(AllocationRegionDataHeader));

  INFO_LOG_FMT(BRAWLBACK, "Processing complete data - Frame: {}, Region count: {}", NetPlay::CurrentFrame(),
               header.regionCount);

  // Allocate and copy each region into its own buffer
  auto& memory = Core::System::GetInstance().GetMemory();
  for (u32 i = 0; i < header.regionCount; i++)
  {
    u32 regionAddress = swap_endian(entries[i].address);
    u32 regionSize = swap_endian(entries[i].size);

    // Validate size before allocation
    if (regionSize == 0 || regionSize > 0x10000000)  // 256MB max sanity check
    {
      INFO_LOG_FMT(BRAWLBACK, "Invalid region size: {} at index {}", regionSize, i);
      continue;
    }

    // Allocate buffer for this region
    u8* regionBuffer = static_cast<u8*>(Common::AllocateAlignedMemory(regionSize, 64));

    // Copy region data from game memory
    u8* sourceData = memory.GetSpanForAddress(regionAddress).data();
    if (sourceData != nullptr && regionBuffer != nullptr)
    {
      memcpy(regionBuffer, sourceData, regionSize);

      // Store buffer and size in state
      State state;
      state.buffer = regionBuffer;
      state.size = regionSize;
      state.address = regionAddress;
      savestates[savestateIndex].states.push_back(state);
    }
    else
    {
      INFO_LOG_FMT(BRAWLBACK, "Invalid source address: 0x{:08X}", regionAddress);
      if (regionBuffer != nullptr)
      {
        Common::FreeAlignedMemory(regionBuffer);
      }
    }
  }

  // Clear the buffer after processing
  start_loop_buffer.clear();
  start_loop_bytes_received = 0;
}
void CEXIBrawlback::handleLoadStateSize(u8* payload)
{
  struct LoadStatePayloadInfo
  {
    bu32 payloadSize;
    bu32 exiMaxPayloadBytes;
  };

  LoadStatePayloadInfo info;
  memcpy(&info, payload, sizeof(LoadStatePayloadInfo));

  info.payloadSize = swap_endian(info.payloadSize);
  info.exiMaxPayloadBytes = swap_endian(info.exiMaxPayloadBytes);

  load_state_payload_size = info.payloadSize;
  load_state_max_chunk_size = info.exiMaxPayloadBytes - 1;  // Subtract 1 for command byte

  INFO_LOG_FMT(BRAWLBACK, "Load state payload size: {} bytes, Max chunk size: {}",
               load_state_payload_size, load_state_max_chunk_size);

  // Reset buffer for new data
  load_state_buffer.clear();

  // Handle case where payload size is 0
  if (load_state_payload_size == 0)
  {
    INFO_LOG_FMT(BRAWLBACK, "Warning: Load state payload size is 0, skipping buffer operations");
    load_state_bytes_received = 0;
    return;
  }

  load_state_buffer.reserve(load_state_payload_size);
  load_state_bytes_received = 0;
}

void CEXIBrawlback::handleLoadState(u8* payload)
{
  // Skip if payload size is 0
  if (load_state_payload_size == 0)
  {
    return;
  }

  // Calculate how many bytes we expect in this chunk
  int bytes_remaining = load_state_payload_size - load_state_bytes_received;
  int expected_chunk_size = std::min(bytes_remaining, load_state_max_chunk_size);

  // Append this chunk to the buffer
  load_state_buffer.insert(load_state_buffer.end(), payload, payload + expected_chunk_size);
  load_state_bytes_received += expected_chunk_size;

  INFO_LOG_FMT(BRAWLBACK, "Received load state chunk: {} bytes ({}/{})", expected_chunk_size,
               load_state_bytes_received, load_state_payload_size);

  // Check if we've received all the data
  if (load_state_bytes_received >= load_state_payload_size)
  {
    INFO_LOG_FMT(BRAWLBACK, "All load state chunks received, processing...");
    handleLoadStateComplete();
  }
}

void CEXIBrawlback::handleLoadStateComplete()
{
  struct LoadStatePayloadHeader
  {
    bu32 regionCount;
  };

  // Parse header from accumulated buffer
  LoadStatePayloadHeader header;
  memcpy(&header, load_state_buffer.data(), sizeof(LoadStatePayloadHeader));

  header.regionCount = swap_endian(header.regionCount);

  bu32 frame = NetPlay::CurrentFrame() - NetPlay::AdvanceFrames();

  INFO_LOG_FMT(BRAWLBACK, "Loading state for frame: {}, Region count: {}", frame,
               header.regionCount);

  // Parse entries array (comes after header in buffer)
  AllocationRegionEntry* entries = reinterpret_cast<AllocationRegionEntry*>(
      load_state_buffer.data() + sizeof(LoadStatePayloadHeader));

  // Get the savestate for this frame
  Savestate& frame_state = savestates[frame % 5];

  // Restore each region specified in the payload
  auto& memory = Core::System::GetInstance().GetMemory();

  // Clear previous missing and additional regions
  missing_regions.clear();

  // First, restore all regions from the saved state that are in the entries list
  // and track which saved regions are NOT in the entries list (additional_regions)
  u32 restored_count = 0;
  for (const State& state : frame_state.states)
  {
    if (state.buffer != nullptr)
    {
      memory.CopyToEmu(state.address, state.buffer, state.size);
    }
  }

  INFO_LOG_FMT(BRAWLBACK, "Restored {} regions from savestate", restored_count);
  
  // Now find regions requested in entries but not present in savestate (missing_regions)
  for (u32 i = 0; i < header.regionCount; i++)
  {
    u32 regionAddress = swap_endian(entries[i].address);
    u32 regionSize = swap_endian(entries[i].size);

    // Check if this region exists in the saved state
    bool found_in_entries = false;
    for (const State& state : frame_state.states)
    {
      if (state.address == regionAddress && state.size == regionSize)
      {
        found_in_entries = true;
        break;
      }
    }

    // If region wasn't in the savestate, it's missing - collect it
    if (!found_in_entries)
    {
      INFO_LOG_FMT(BRAWLBACK,
                   "Missing region (in entries, not in savestate) - Address=0x{:08X}, Size={}", 
                   regionAddress, regionSize);
      
      // Add to missing regions list (keep in big-endian for game)
      AllocationRegionEntry entry;
      entry.address = entries[i].address;  // Keep original endianness
      entry.size = entries[i].size;        // Keep original endianness
      missing_regions.push_back(entry);
    }
  }

  INFO_LOG_FMT(BRAWLBACK, "State load complete for frame {} - {} missing regions", frame,
               missing_regions.size());
  
  // Send the count of missing regions first
  {
    std::lock_guard<std::mutex> lock(read_queue_mutex);
    this->read_queue.clear();

    bu32 missingCount = static_cast<bu32>(missing_regions.size());
    auto countPtr = reinterpret_cast<u8*>(&missingCount);
    this->read_queue.insert(this->read_queue.end(), countPtr, countPtr + sizeof(bu32));
  }

  // Clear the buffer after processing
  load_state_buffer.clear();
  load_state_bytes_received = 0;
}

void CEXIBrawlback::handleGetMissingRegions()
{
  std::lock_guard<std::mutex> lock(read_queue_mutex);
  this->read_queue.clear();
  
  // Calculate total size needed
  size_t totalSize = missing_regions.size() * sizeof(AllocationRegionEntry);
  this->read_queue.reserve(totalSize);
  
  // Send all entries in one operation if there are any
  if (!missing_regions.empty())
  {
    auto entriesPtr = reinterpret_cast<const u8*>(missing_regions.data());
    this->read_queue.insert(this->read_queue.end(), entriesPtr, 
                           entriesPtr + (missing_regions.size() * sizeof(AllocationRegionEntry)));
  }
  
  INFO_LOG_FMT(BRAWLBACK, "Queued {} bytes for game to read ({} missing regions)",
               this->read_queue.size(), missing_regions.size());
}

void CEXIBrawlback::handleGetPort()
{
  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode())
  {
    u8 port = 0;
    for (int i = 0; i < NetPlay::netplay_client->GetPadMapping().size(); i++)
    {
      if (NetPlay::netplay_client->GetPadMapping().at(i) == NetPlay::netplay_client->GetLocalPlayerId())
        port = i;
    }
    std::lock_guard<std::mutex> lock(read_queue_mutex);
    this->read_queue.clear();
    auto dataPtr = reinterpret_cast<u8*>(&port);
    this->read_queue.insert(this->read_queue.end(), dataPtr, dataPtr + sizeof(u8));
  }
}

void CEXIBrawlback::handleGetInputs(bool local)
{
  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode())
  {
    // TODO: Doubles?
    std::optional<NetPlay::Inputs> pad = std::nullopt;
    std::optional<BrawlbackPad> predicted_pad = std::nullopt;
    BrawlbackPad game_pad = BrawlbackPad{};
    for (int i = 0; i < NetPlay::netplay_client->GetPadMapping().size(); i++)
    {
      if (local && NetPlay::netplay_client->GetPadMapping().at(i) ==
                       NetPlay::netplay_client->GetLocalPlayerId())
      {
        pad = NetPlay::netplay_client->FindRemoteInputs(i, NetPlay::netplay_client->current_frame);
        predicted_pad =
            NetPlay::netplay_client->GetPredictedInputs(i, NetPlay::netplay_client->current_frame);
        break;
      }
      else if (!local && NetPlay::netplay_client->GetPadMapping().at(i) !=
                             NetPlay::netplay_client->GetLocalPlayerId())
      {
        pad = NetPlay::netplay_client->FindRemoteInputs(i, NetPlay::netplay_client->current_frame);
        predicted_pad =
            NetPlay::netplay_client->GetPredictedInputs(i, NetPlay::netplay_client->current_frame);
        break;
      }
    }
    if (pad != std::nullopt)
    {
      game_pad = pad.value().game_pad;
    }
    else if (predicted_pad != std::nullopt)
    {
      game_pad = predicted_pad.value();
    }
    std::lock_guard<std::mutex> lock(read_queue_mutex);
    this->read_queue.clear();
    auto dataPtr = reinterpret_cast<u8*>(&game_pad);
    this->read_queue.insert(this->read_queue.end(), dataPtr, dataPtr + sizeof(BrawlbackPad));
  }
}
void CEXIBrawlback::handleGetNetworkingMode()
{
  u32 mode = NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode();
  std::lock_guard<std::mutex> lock(read_queue_mutex);
  this->read_queue.clear();
  auto dataPtr = reinterpret_cast<u8*>(&mode);
  this->read_queue.insert(this->read_queue.end(), dataPtr, dataPtr + sizeof(u32));
}
CEXIBrawlback::CEXIBrawlback(Core::System& system) : IEXIDevice(system)
{
}
// recieve data from game into emulator
void CEXIBrawlback::DMAWrite(u32 address, u32 size)
{
  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();
  u8* mem = memory.GetSpanForAddress(address).data();

  if (!mem)
  {
    INFO_LOG_FMT(BRAWLBACK, "Invalid address in DMAWrite!");
    return;
  }

  u8 command_byte = mem[0];  // first byte is always cmd byte
  u8* payload = &mem[1];     // rest is payload

  // no payload
  if (size <= 1)
    payload = nullptr;

  static u64 frameTime = Common::Timer::NowUs();
  switch (command_byte)
  {
  case CMD_UNKNOWN:
    INFO_LOG_FMT(BRAWLBACK, "Unknown DMAWrite command byte!");
    break;
  case CMD_END_FRAME:
    handleEndFrame();
    break;
  case CMD_END_LOOP:
    handleEndLoop();
    break;
  case CMD_START_LOOP:
    handleStartLoop(payload);
    break;
  case CMD_START_LOOP_ROLLBACK:
    handleStartLoopRollback(payload);
    break;
  case CMD_START_LOOP_SIZE:
    handleStartLoopSize(payload);
    break;
  case CMD_LOAD_STATE:
    handleLoadState(payload);
    break;
  case CMD_LOAD_STATE_SIZE:
    handleLoadStateSize(payload);
    break;
  case CMD_GET_MISSING_REGIONS:
    handleGetMissingRegions();
    break;
  case CMD_GET_PORT:
    handleGetPort();
    break;
  case CMD_GET_REMOTE_INPUTS:
    handleGetInputs(false);
    break;
  case CMD_GET_LOCAL_INPUTS:
    handleGetInputs(true);
    break;
  case CMD_ROLLBACK_CHECK:
    handleGetNetworkingMode();
    break;
  default:
    break;
  }
}

// send data from emulator to game
void CEXIBrawlback::DMARead(u32 address, u32 size)
{
  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();
  std::lock_guard<std::mutex> lock(read_queue_mutex);

  if (this->read_queue.empty())
  {                                                       // we have nothing to send to the game
    this->read_queue.push_back(EXICommand::CMD_UNKNOWN);  // result code
  }

  // game is trying to get cmd byte (don't clear read_queue)
  if (size == 1)
  {
    memory.CopyToEmu(address, &this->read_queue[0], size);
    this->read_queue.erase(this->read_queue.begin());
    return;
  }

  this->read_queue.resize(size, 0);
  auto qAddr = &this->read_queue[0];
  memory.CopyToEmu(address, qAddr, size);
  this->read_queue.clear();
}

// honestly dunno why these are overriden like this, but slippi does it sooooooo  lol
bool CEXIBrawlback::IsPresent() const
{
  return true;
}

void CEXIBrawlback::TransferByte(u8& byte)
{
}
