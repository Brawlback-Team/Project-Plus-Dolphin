#pragma once
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "Core/Brawlback/TimeSync.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/Brawlback/include/brawlback-common/SavestateMemRegion.h"
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
  CMD_ROLLBACK_CHECK = 7
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

  // --- DMA handlers
  void handleEndFrame();
  void handleEndLoop();
  void handleStartLoop(u8* payload);
  void handleGetPort();
  void handleGetInputs(bool local);
  void handleGetNetworkingMode();

protected:
  void TransferByte(u8& byte) override;

};
