#pragma once
#include <deque>
#include <memory>
#include <string>
#include <vector>
#include "Core/Brawlback/BrawlbackUtility.h"
#include "Core/Brawlback/Netplay/Matchmaking.h"
#include "Core/Brawlback/Netplay/Netplay.h"
#include "Core/Brawlback/Savestate.h"
#include "Core/Brawlback/TimeSync.h"
#include "Core/HW/EXI/EXI_Device.h"
#ifdef _WIN32
#include <Qos2.h>
#endif
using namespace Brawlback;

class CEXIBrawlback : public ExpansionInterface::IEXIDevice
{
public:
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

protected:
  void TransferByte(u8& byte) override;
};
