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
  CMD_SEND_INPUTS = 13,
  // Callback-code queue commands
  CMD_GET_CALLBACK_CODES = 14,
  CMD_EXECUTE_SAVE = 15,
  CMD_EXECUTE_LOAD = 16,
  CMD_EXECUTE_ADVANCE = 17
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

  // Chunked data buffer
  std::vector<u8> start_loop_buffer = {};
  int start_loop_bytes_received = 0;
  bool receiving_chunked_data = false;

  // Chunked data buffer for accumulating load state data
  std::vector<u8> load_state_buffer = {};
  int load_state_payload_size = 0;
  int load_state_max_chunk_size = 16287;
  int load_state_bytes_received = 0;
  u32 load_state_frame = 0;

  // Store missing regions from last load state operation
  std::vector<AllocationRegionEntry> missing_regions;

  // Region list parsed from the last complete start_loop payload;
  // consumed by handleExecuteSave to do the actual memory reads.
  struct PendingSaveRegionList
  {
    u32 frame = 0;
    std::vector<AllocationRegionEntry> entries;
  };
  std::vector<PendingSaveRegionList> pending_save_region_list;

  // Region list parsed from the last complete load_state payload;
  // consumed by handleExecuteLoad to do the actual restore and missing-region detection.
  struct PendingLoadRegionList
  {
    u32 frame = 0;
    std::vector<AllocationRegionEntry> entries;
  };
  std::vector<PendingLoadRegionList> pending_load_region_list;

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
  void handleGetInputs(bool local, u8* payload);
  void handleGetNetworkingMode();
  // Callback-code queue
  void handleGetCallbackCodes();
  void handleExecuteSave();
  void handleExecuteLoad();
  void handleExecuteAdvance();

protected:
  void TransferByte(u8& byte) override;
};
