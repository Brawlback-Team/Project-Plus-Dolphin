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
  CEXIBrawlback(Core::System& system);
  ~CEXIBrawlback() override;


  void DMAWrite(u32 address, u32 size) override;
  void DMARead(u32 address, u32 size) override;

  bool IsPresent() const;

  std::string replayDirectory;

private:
  // byte vector for sending into to the game
  std::vector<u8> read_queue = {};

  // --- DMA handlers
  void handleCaptureSavestate(u8* data);
  void handleLoadSavestate(u8* data);
  void handleLocalPadData(u8* data);
  void handleFrameDataRequest(u8* data);
  void handleFrameAdvanceRequest(u8* data);
  void handleFindMatch(u8* payload);
  void handleStartMatch(u8* payload);
  void handleEndMatch(u8* payload);
  void handleStartReplaysStruct(u8* payload);
  void serializeStartReplay(const StartReplay& startReplay);
  void serializeReplay(const Replay& replay);
  void handleReplaysStruct(u8* payload);
  void handleEndOfReplay();
  void handleDumpAll(u8*);
  void handleAlloc(u8* payload);
  void handleTrackAlarm(u8* payload, bool track);
  void handleDealloc(u8* payload);
  void handleFrameCounterLoc(u8* payload);
  void handleGetNextFrame(u8* payload, int index);
  void handleNumReplays();
  void handleGetStartReplay(u8* payload);
  void handleCancelMatchmaking();
  void handleUpdateSync(u8* payload);
  bool isRollbackMode(bu32 localFrame, u8 playerIdx);
  void handleEndFrame();
  void handleEndLoop();
  void handleStartLoop(u8* payload);
  void handleGetPort();
  void handleGetInputs(bool local);

  template <typename T>
  void SendCmdToGame(EXICommand cmd, T* payload);

  void SendCmdToGame(EXICommand cmd);
  // -------------------------------

  // --- Replays
  void fixStartReplayEndianness(StartReplay& startReplay);
  void fixReplayEndianness(Replay& replay);
  std::vector<std::vector<u8>> getReplays(std::string path);
  std::vector<std::string> getReplayNames(std::string path);
  u8 getNumReplays(std::string path);
  json getReplayJsonAtIndex(int index);
  std::string getReplayNameAtIndex(int index);
  u8 curIndex;
  json curReplayJson;
  std::string curReplayName;
  // -------------------------------

  // --- Net
  void NetplayThreadFunc();
  void ProcessNetReceive(ENetEvent* event);
  void ProcessRemoteFrameData(PlayerFrameData* framedata, u8 numFramedatas);
  void ProcessIndividualRemoteFrameData(PlayerFrameData* framedata);
  void ProcessGameSettings(GameSettings* opponentGameSettings);
  void ProcessFrameAck(FrameAck* frameAck);
  bu32 GetLatestRemoteFrame();
  ENetHost* server = nullptr;
  ENetPeer* peer = nullptr;
  std::thread netplay_thread;
  std::unique_ptr<BrawlbackNetplay> netplay;

  bool isConnected = false;
#ifdef _WIN32
  HANDLE m_qos_handle;
  QOS_FLOWID m_qos_flow_id;
#endif

  // -------------------------------

  // --- Matchmaking
  void connectToOpponent();
  void MatchmakingThreadFunc();
  Brawlback::UserInfo getUserInfo();
  Matchmaking::MatchSearchSettings lastSearch;
  std::unique_ptr<Matchmaking> matchmaking;
  std::thread matchmaking_thread;
  // -------------------------------

  // --- Game info
  bool isHost = true;
  int localPlayerIdx = -1;
  u8 numPlayers = 0;
  bool hasGameStarted = false;
  GameSettings gameSettings;
  int gameIndex = 0;
  bool firstDump = true;

  // -------------------------------

  // --- Time sync
  std::unique_ptr<TimeSync> timeSync;
  // -------------------------------

  // --- Rollback
  bool isPredicting; // if we are using past inputs for this frame or not
  BrawlbackFrameData predictedInputs;  // predicted inputs from some previous frame
  bu32 framesToAdvance = 1; // number of "frames" to advance the simulation on this frame
  bu32 latestConfirmedFrame = 0; // Tracks the last frame where we synchronized the game state with the remote client
  bu32 stopRollbackFrame = -1;
  bu32 startRollbackFrame = -1;
  bu32 localFrame = 0;
  std::vector<SavestateMemRegionInfo> deallocRegions = {};
  void updateSync(bu32& localFrame, u8 playerIdx);
  bool shouldRollback(bu32 localFrame);
  void LoadState(bu32 rollbackFrame);
  void SaveState(bu32 frame);
  std::vector<SlippiUtility::Savestate::ssBackupLoc> staticRegions = {
      {0x80A471A0, 0x80b8db60, nullptr, "OverlayCommon 4/4"},
      {0x806414a0, 0x806414a0 + 0x60, nullptr, "g_soSlow"},
      {0x8062f440, 0x8062f440 + 0x500, nullptr, "g_grCollisionManager"},
      {0x8063ff60, 0x8063ff60 + 0x1520, nullptr, "g_soCollisionManager"},
      {0x8063dcc0, 0x8063dcc0 + 0x2280, nullptr, "g_soCollisionManager->collisionLogs"},
      {0x8063da80, 0x8063da80 + 0x220, nullptr, "g_soCollisionManager->logGroups"},
      {0x80624780, 0x80624780 + 0x50, nullptr, "ftEntryManager"},
      {0x806232e0, 0x806232e0 + 0x1480, nullptr, "ftEntryManager->ftEntries"},
      {0x806273c0, 0x806273c0 + 0x540, nullptr, "ftSlotManager"},
      {0x80624800, 0x80624800 + 0x2ba0, nullptr, "ftSlotManager->ftSlots"},
      {0x80622d20, 0x80622d20 + 0x388, nullptr, "aiMgr"},
      {0x80621260, 0x80621260 + 0x1aa0, nullptr, "aiMgr->aiStatArray__15"},
      {0x8061f920, 0x8061f920 + 0x1920, nullptr, "aiMgr->aiWeaponMgr"},
      {0x8061f460, 0x8061f460 + 0x4a0, nullptr, "aiMgr->aiEnemyMgr"},
      {0x80615620, 0x80615620 + 0x6f20, nullptr, "aiMgr->field_0x34c"},
      {0x8061c560, 0x8061c560 + 0x2EE0, nullptr, "aiMgr->field_0x348"},
      {0x8063d9a0, 0x8063d9a0 + 0xc0, nullptr, "g_soJostleManager"},
      {0x8063d8e0, 0x8063d8e0 + 0xa0, nullptr, "g_soCutInManager"},
      {0x806312e0, 0x806312e0 + 0xbba0, nullptr, "g_soGeneralTermManager->generalTerms"},
      {0x8062fb40, 0x8062fb40 + 0x1780, nullptr, "g_soGeneralTermManager->indices"},
      {0x8062f9e0, 0x8062f9e0 + 0x140, nullptr, "g_soEffectScreenManager"},
      {0x8062f960, 0x8062f960 + 0x60, nullptr, "g_soCatchManager"},
      {0x80663300, 0x80663300 + 0x140, nullptr, "g_efManager"},  // this is a gfTask "EffectManager", if there's issues these are a good place to start
      {0x80663280, 0x80663280 + 0x60, nullptr, "g_efManager[1].field8_0x20"},
      {0x80663060, 0x80663060 + 0x200, nullptr, "g_efScreen"},
      {0x8062b360, 0x8062b360 + 0x14c0, nullptr, "g_itManager"},
      {0x80613e00, 0x80613e00 + 0x28, nullptr, "g_clManager"},
      {0x80641520, 0x80641520 + 0x20, nullptr, "g_soWorld"},
      {0x80629a00, 0x80629a00 + 0x160, nullptr, "g_ftManager"},
      {0x80623180, 0x80623180 + 0x20, nullptr, "g_ftAudienceManager"},
      {0x806230e0, 0x806230e0 + 0x80, nullptr, "g_ftAudienceManager->audienceImpl"},
      {0x80627920, 0x80627920 + 0x10c0, nullptr, "g_ftManager->ftDataProviderPtr"},
      {0x8062f3e0, 0x8062f3e0 + 0x40, nullptr, "g_soArchiveDb"},
      {0x8062f360, 0x8062f360 + 0x60, nullptr, "g_utArchiveManagers[2]"},
      {0x8062c840, 0x8062c840 + 0x1e00, nullptr, "g_utArchiveManagers[2]->utListStart"},
      {0x80629980, 0x80629980 + 0x60, nullptr, "g_utArchiveManagers[0]"},
      {0x80628a00, 0x80628a00 + 0xAA0, nullptr, "g_utArchiveManagers[0]->utListStart"},
      {0x8123ab60, 0x8128cb60, nullptr, "Fighter1Instance"},
      {0x8128cb60, 0x812deb60, nullptr, "Fighter2Instance"},
      {0x8049edd8, 0x8049edd8 + 0x5064, nullptr,
       "g_efManager[1].field7_0x1c = nw4r2ef12EffectSystemFv"},
      {0x805b62a0, 0x805b62a0 + 0x100, nullptr, "gfRumble"},
      {0x80662b40, 0x80662b40 + 0x500, nullptr, "g_efManager->field_0x4c"},
      {0x80662620, 0x80662620 + 0x500, nullptr, "g_efManager->field_0x50"},
      {0x80672f40, 0x80672f40 + 0x520, nullptr, "g_gfSceneRoot"},
      {0x8154e560, 0x81601960, nullptr, "Physics"},
      {0x8066b5e0, 0x8066b5e0 + 0x4220, nullptr, "g_gfSceneRoot->field_0x40"},
      {0x806673a0, 0x806673a0 + 0x4220, nullptr, "g_gfSceneRoot->field_0x44"},
      {0x80663fe0, 0x80663fe0 + 0x33a0, nullptr, "g_gfSceneRoot->field_0x48"},
      {0x80672920, 0x80672920 + 0x600, nullptr, "g_gfSceneRoot->field_0x38c[0][1]"},
      {0x80672300, 0x80672300 + 0x600, nullptr, "g_gfSceneRoot->field_0x38c[1][1]"},
      {0x80671ce0, 0x80671ce0 + 0x600, nullptr, "g_gfSceneRoot->field_0x38c[2][1]"},
      {0x806716c0, 0x806716c0 + 0x600, nullptr, "g_gfSceneRoot->field_0x38c[3][1]"},
      {0x806710a0, 0x806710a0 + 0x600, nullptr, "g_gfSceneRoot->field_0x38c[4][1]"},
      {0x80670a80, 0x80670a80 + 0x600, nullptr, "g_gfSceneRoot->field_0x38c[5][1]"},
      {0x80670460, 0x80670460 + 0x600, nullptr, "g_gfSceneRoot->field_0x38c[6][1]"},
      {0x8066fe40, 0x8066fe40 + 0x600, nullptr, "g_gfSceneRoot->field_0x38c[7][1]"},
      {0x8066f820, 0x8066f820 + 0x600, nullptr, "g_gfSceneRoot->field_0x38c[8][1]"},
      {0x8049e57c, 0x8049e57c + 0xC, nullptr, "g_grCollisionList"},
      {0x805ba480, 0x805baca0, nullptr, "Pad Buffer"},
  };
  std::vector<SlippiUtility::Savestate::ssBackupLoc> dynamicRegions = {};
  u32 lastStatedFrame = 0;
  int savestateNum = 0;
  // -------------------------------
  // -------------------------------

  // --- Framedata (player inputs)
  void handleSendInputs(bu32 frame);
  PlayerFrameData getLocalInputs(const bu32& frame);
  PlayerFrameData getRemoteInputs(bu32& frame, bu8 playerIdx, bool& skipFrame);
  void storeLocalInputs(PlayerFrameData* localPlayerFramedata);

  // local player input history. Always holds FRAMEDATA_MAX_QUEUE_SIZE of past inputs
  PlayerFrameDataQueue localPlayerFrameData = {};


  // remote player input history (indexes are player indexes). Always holds FRAMEDATA_MAX_QUEUE_SIZE of past inputs
  std::array<PlayerFrameDataQueue, MAX_NUM_PLAYERS> remotePlayerFrameData = {};
  // -------------------------------

protected:
  void TransferByte(u8& byte) override;
};
