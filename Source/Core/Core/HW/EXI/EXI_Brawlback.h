#pragma once
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "Core/Brawlback/TimeSync.h"
#include "Core/Brawlback/include/brawlback-common/SavestateMemRegion.h"
#include "Core/HW/EXI/EXI_Device.h"
using namespace Brawlback;

enum EXICommand : u8
{
  CMD_UNKNOWN = 0,
  CMD_END_FRAME = 1,
  CMD_END_LOOP = 2,
  CMD_START_LOOP = 3,
  CMD_GET_PORT = 4,
  CMD_GET_REMOTE_INPUTS = 5,
  CMD_GET_LOCAL_INPUTS = 6,
  CMD_ROLLBACK_CHECK = 7,
  CMD_START_LOOP_SIZE = 8,
  CMD_LOAD_STATE = 9,
  CMD_LOAD_STATE_SIZE = 10,
  CMD_GET_MISSING_REGIONS = 11,
  CMD_START_LOOP_ROLLBACK = 12,
  CMD_SEND_INPUTS = 13
};

struct BrawlbackRegionState
{
  u8* buffer;
  u64 size;
  u32 address;
};

struct Savestate
{
  std::vector<BrawlbackRegionState> states;
  u32 frame;
};

struct AllocationRegionEntry
{
  bu32 address;
  bu32 size;
  char nameBuffer[30];
  u8 nameSize;
};

class CEXIBrawlback : public ExpansionInterface::IEXIDevice
{
public:
  explicit CEXIBrawlback(Core::System& system);
  void DMAWrite(u32 address, u32 size) override;
  void DMARead(u32 address, u32 size) override;

  bool IsPresent() const;

private:
  // byte vector for sending into to the game
  std::vector<u8> read_queue = {};
  int start_loop_payload_size = 0;        // Total payload size in bytes
  int start_loop_max_chunk_size = 16287;  // Default max chunk size (16288 - 1 for cmd byte)
  Savestate savestates[5] = {};

  // Chunked data buffer for accumulating start loop data
  std::vector<u8> start_loop_buffer = {};
  int start_loop_bytes_received = 0;

  // Chunked data buffer for accumulating load state data
  std::vector<u8> load_state_buffer = {};
  int load_state_payload_size = 0;
  int load_state_max_chunk_size = 16287;
  int load_state_bytes_received = 0;

  // Store missing regions from last load state operation
  std::vector<AllocationRegionEntry> missing_regions;

  // --- DMA handlers
  void handleEndFrame();
  void handleEndLoop();
  void handleStartLoopSize(u8* payload);
  void handleStartLoop(u8* payload);
  void handleStartLoopRollback(u8* payload);
  void handleStartLoopComplete();
  void handleLoadStateSize(u8* payload);
  void handleLoadState(u8* payload);
  void handleLoadStateComplete();
  void handleGetMissingRegions();
  void handleGetPort();
  void handleGetInputs(bool local);
  void handleGetNetworkingMode();

protected:
  void TransferByte(u8& byte) override;
};
