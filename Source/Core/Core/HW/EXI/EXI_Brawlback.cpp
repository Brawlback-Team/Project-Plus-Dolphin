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
#include <incremental-rollback/incremental_rb.h>
#include <chrono>
#include "Common/CommonTypes.h"
#include <Common/MemoryUtil.h>
#include <Core/NetPlayClient.h>
#include <incremental-rollback/mem.h>
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
void CEXIBrawlback::handleEndLoop(u8* payload)
{
  if (NetPlay::IsNetPlayRunning() && NetPlay::IsStarted())
  {
    std::vector<SavestateMemRegionInfo> regions(sizeOfSavestatesRegions);
    for (int i = 0; i < sizeOfSavestatesRegions; i++)
    {
      memcpy(&regions[i], payload + i * sizeof(SavestateMemRegionInfo), sizeof(SavestateMemRegionInfo));
      SwapEndianSavestateMemRegionInfo(regions[i]);
    }
    IncrementalRB::OnFrameEnd(NetPlay::CurrentFrame(), NetPlay::IsRollingBack(), regions);
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
void CEXIBrawlback::handleStartLoop(u8* payload)
{
  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode())
  {
    BrawlbackPad pad;
    memcpy(&pad, payload, sizeof(BrawlbackPad));
    SwapBrawlbackPadDataEndianess(pad);
    NetPlay::OnFrameStart(pad);

    Core::System::GetInstance().GetMemory().SetTrackDirtyPagesEnabled(true);
    s32 advanceFrames = NetPlay::AdvanceFrames();
    std::lock_guard<std::mutex> lock(read_queue_mutex);
    this->read_queue.clear();
    auto dataPtr = reinterpret_cast<u8*>(&advanceFrames);
    this->read_queue.insert(this->read_queue.end(), dataPtr, dataPtr + sizeof(s32));
  }
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
  u32 mode = NetPlay::IsInRollbackMode();
  std::lock_guard<std::mutex> lock(read_queue_mutex);
  this->read_queue.clear();
  auto dataPtr = reinterpret_cast<u8*>(&mode);
  this->read_queue.insert(this->read_queue.end(), dataPtr, dataPtr + sizeof(u32));
}
void CEXIBrawlback::handleGetSizeSavestates(u8* payload)
{
  if (NetPlay::IsNetPlayRunning() && NetPlay::IsInRollbackMode())
  {
    memcpy(&sizeOfSavestatesRegions, payload, sizeof(int));
    sizeOfSavestatesRegions = swap_endian(sizeOfSavestatesRegions);
  }
}
CEXIBrawlback::CEXIBrawlback(Core::System& system) : IEXIDevice(system)
{
}
// recieve data from game into emulator
void CEXIBrawlback::DMAWrite(u32 address, u32 size)
{
  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();
  // INFO_LOG_FMT(BRAWLBACK, "DMAWrite size: %u\n", size);
  u8* mem = memory.GetSpanForAddress(address).data();

  if (!mem)
  {
    INFO_LOG_FMT(BRAWLBACK, "Invalid address in DMAWrite!");
    // this->read_queue.clear();
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
  // just using these CMD's to track frame times lol
  case CMD_END_LOOP:
    handleEndLoop(payload);
    break;
  case CMD_START_LOOP:
    handleStartLoop(payload);
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
  case CMD_SIZE_SAVESTATES:
    handleGetSizeSavestates(payload);
    break;
  default:
    // INFO_LOG_FMT(BRAWLBACK, "Default DMAWrite %u\n", (unsigned int)command_byte);
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
