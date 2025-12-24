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

namespace fs = std::filesystem;
// --- Mutexes
std::mutex read_queue_mutex = std::mutex();
std::mutex remotePadQueueMutex = std::mutex();
// -------------------------------
void writeToFile(std::string filename, uint8_t* ptr, size_t len)
{
  std::ofstream fp;
  fp.open(filename, std::ios::out | std::ios::binary);
  fp.write((char*)ptr, len);
}
std::vector<u8> read_vector_from_disk(std::string file_path)
{
  std::ifstream instream(file_path, std::ios::in | std::ios::binary);
  std::vector<u8> data((std::istreambuf_iterator<char>(instream)),
                       std::istreambuf_iterator<char>());
  return data;
}
CEXIBrawlback::CEXIBrawlback(Core::System& system) : ExpansionInterface::IEXIDevice(system)
{
  INFO_LOG_FMT(BRAWLBACK, "------- {}\n", SConfig::GetInstance().GetGameID());
#ifdef _WIN32
  if (std::filesystem::exists(Sync::getSyncLogFilePath()))
  {
    std::filesystem::remove(Sync::getSyncLogFilePath());
  }
#endif

  INFO_LOG_FMT(BRAWLBACK, "BRAWLBACK exi ctor");
  // TODO: initialize this only when finding matches
  auto enet_init_res = enet_initialize();
  if (enet_init_res < 0)
  {
    ERROR_LOG_FMT(BRAWLBACK, "Failed to init enet! {}\n", enet_init_res);
  }
  else if (enet_init_res == 0)
  {
    INFO_LOG_FMT(BRAWLBACK, "Enet init success");
  }
  this->netplay = std::make_unique<BrawlbackNetplay>();
  this->matchmaking = std::make_unique<Matchmaking>(this->getUserInfo());
  this->timeSync = std::make_unique<TimeSync>();
}

Brawlback::UserInfo CEXIBrawlback::getUserInfo()
{
  Brawlback::UserInfo info;

  std::string lylat;
#ifdef _WIN32
  std::string lylat_json_path = File::GetExeDirectory() + "/lylat.json";
#else
  // This will look on "~/Libraries/Application Support/Dolphin/lylat.json" on macosx
  std::string lylat_json_path = File::GetUserPath(D_USER_IDX) + "lylat.json";
#endif
  INFO_LOG_FMT(BRAWLBACK, "Reading lylat json from {}\n", lylat_json_path);
  std::string data;
  if (!File::ReadFileToString(lylat_json_path, data))
  {
    ERROR_LOG_FMT(BRAWLBACK, "Could not find lylat.json.");
    return info;
  }

  json j = json::parse(data);
  // INFO_LOG_FMT(BRAWLBACK, "JSON Contents: {}", j.dump(4).c_str());

  info.uid = j["uid"].get<std::string>();
  info.playKey = j["playKey"].get<std::string>();
  info.connectCode = j["connectCode"].get<std::string>();
  info.displayName = "Dev Test";
  info.latestVersion = "test";
  info.fileContents = "test";

  return info;
}

CEXIBrawlback::~CEXIBrawlback()
{
  enet_deinitialize();
  enet_host_destroy(this->server);
  this->server = nullptr;
  this->isConnected = false;
  if (this->netplay_thread.joinable())
  {
    this->netplay_thread.join();
  }

  delete this->matchmaking.release();
  if (this->matchmaking_thread.joinable())
  {
    this->matchmaking_thread.join();
  }

}

void CEXIBrawlback::handleCaptureSavestate(u8* data)
{
  // current frame we are saving (and swap endianness)
  bu32 frame;
  std::memcpy(&frame, data, sizeof(bu32));
  frame = swap_endian(frame);
  INFO_LOG_FMT(BRAWLBACK, "Game Frame Page is Dirty?: {}\n",
               Memory::isFramePointerDirty());
  SaveState(frame);
  this->lastStatedFrame = frame;
}

void CEXIBrawlback::SaveState(bu32 frame)
{
  IncrementalRB::SaveWrittenPages(frame - 1, this->framesToAdvance > 1 && frame - 1 < this->stopRollbackFrame);
}

void CEXIBrawlback::handleLoadSavestate(u8* data)
{
  // frame we should rollback to
  std::memcpy(&stopRollbackFrame, data, sizeof(bu32));
  stopRollbackFrame = swap_endian(stopRollbackFrame);
  IncrementalRB::Rollback(this->lastStatedFrame, stopRollbackFrame);
}

void CEXIBrawlback::SendCmdToGame(EXICommand cmd)
{
  // std::lock_guard<std::mutex> lock (read_queue_mutex);
  this->read_queue.clear();
  this->read_queue.push_back(cmd);
}

PlayerFrameData CreateBlankPlayerFrameData(u32 frame, u8 playerIdx)
{
  PlayerFrameData dummy_framedata = PlayerFrameData();
  dummy_framedata.frame = frame;
  dummy_framedata.playerIdx = playerIdx;
  dummy_framedata.syncData = {};
  dummy_framedata.pad = {};                 // empty pad
  dummy_framedata.sysPad = {};  // empty pad
  return dummy_framedata;
}
BrawlbackFrameData CreateBlankFrameData(u32 frame)
{
  BrawlbackFrameData fd;
  fd.randomSeed = 0;
  fd.skipFrame = false;
  for (int i = 0; i < MAX_NUM_PLAYERS; i++)
  {
    fd.playerFrameDatas[i] = CreateBlankPlayerFrameData(frame, i);
  }
  return fd;
}

static int numTimesyncs = 0;
static int numRollbacks = 0;
static int synclogFrameTracker = 0;

// `data` is a ptr to a FrameData struct
// this is called every frame at the beginning of the frame
void CEXIBrawlback::handleLocalPadData(u8* data)
{
  this->framesToAdvance = 1;
  PlayerFrameData playerFramedata;
  std::memcpy(&playerFramedata, data, sizeof(PlayerFrameData));

  // first 4 bytes are current game frame
  SwapPlayerFrameDataEndianness(playerFramedata);
  auto frame = playerFramedata.frame;
  u8 playerIdx = playerFramedata.playerIdx;

  if (frame == GAME_START_FRAME && !this->hasGameStarted)
  {
    // push framedatas for first few delay frames
    for (int i = GAME_START_FRAME; i < FRAME_DELAY; i++)
    {
      auto pfd = CreateBlankPlayerFrameData(i, playerIdx);
      this->remotePlayerFrameData[playerIdx].push_back(std::make_unique<PlayerFrameData>(pfd));
      this->localPlayerFrameData.push_back(std::make_unique<PlayerFrameData>(pfd));
    }
    this->timeSync->startGame(this->numPlayers);
    this->hasGameStarted = true;
  }
  // this just for debugging purposes. Tracks and displays the number of timesyncs done every 60
  // frames
  if (frame % 60 == 0)
  {
    OSD::AddTypedMessage(OSD::MessageType::NetPlayBuffer,
                         "Timesyncs: " + std::to_string(numTimesyncs) +
                             "  Rollbacks: " + std::to_string(numRollbacks) + "\n",
                         OSD::Duration::NORMAL, OSD::Color::CYAN);
    numTimesyncs = 0;
    numRollbacks = 0;
  }

  // TODO: formatting??
  if (frame % PING_DISPLAY_INTERVAL == 0)
    OSD::AddTypedMessage(
        OSD::MessageType::BrawlbackBuffer,
        "Time offset: " +
            std::to_string((double)timeSync->calcTimeOffsetUs(this->numPlayers) / 1000) + " ms\n");

  int remote_frame = (int)this->GetLatestRemoteFrame();
  bool shouldTimeSync = this->timeSync->shouldStallFrame(frame, remote_frame, this->numPlayers);
  if (shouldTimeSync)
  {
    INFO_LOG_FMT(BRAWLBACK, "Should time sync\n");
    // Send inputs that have not yet been acked
    this->handleSendInputs(frame);
    // this->SendCmdToGame(EXICommand::CMD_TIMESYNC);
    this->framesToAdvance = 0;
    numTimesyncs += 1;
  }
  else
  {
    // store these local inputs (with frame delay)
    this->storeLocalInputs(&playerFramedata);
    // broadcasts local inputs
    this->handleSendInputs(frame);
  }
}

void CEXIBrawlback::handleFrameDataRequest(u8* data)
{
  bu32 currentFrame;
  std::memcpy(&currentFrame, data, sizeof(bu32));
  currentFrame = swap_endian(currentFrame);
  // INFO_LOG_FMT(BRAWLBACK, "Game requested inputs for frame %i\n", currentFrame);

  BrawlbackFrameData framedataToSendToGame;

  if (this->framesToAdvance != 0)
  {
    for (s32 i = 0; i < this->numPlayers; i++)
    {
      if (this->framesToAdvance == 0)
      {
        INFO_LOG_FMT(BRAWLBACK, "Stalling on this frame, so using blank inputs\n");
        // TODO: should pad data really be sent blank here even if stalling? what if we need to
        // advance but make the game still process some inputs?
        framedataToSendToGame.playerFrameDatas[i] = CreateBlankPlayerFrameData(currentFrame, i);
        continue;
      }

      // remote inputs
      if (i != this->localPlayerIdx)
      {
        framedataToSendToGame.playerFrameDatas[i] = this->getRemoteInputs(currentFrame, i, framedataToSendToGame.skipFrame);
      }
    }
    // since getRemoteInputs may change the current frame (in the case of a rollback), always get
    // local inputs last
    framedataToSendToGame.playerFrameDatas[this->localPlayerIdx] =
        this->getLocalInputs(currentFrame);
  }
  else
  {
    framedataToSendToGame = CreateBlankFrameData(currentFrame);
  }
  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();
  if (currentFrame == GAME_START_FRAME)
  {
    memory.InitDirtyPages();
  }
  std::lock_guard<std::mutex> lock(read_queue_mutex);
  this->read_queue.clear();
  auto frameDataPtr = reinterpret_cast<u8*>(&framedataToSendToGame);
  this->read_queue.insert(this->read_queue.end(), frameDataPtr,
                          frameDataPtr + sizeof(BrawlbackFrameData));
}

PlayerFrameData CEXIBrawlback::getLocalInputs(const bu32& frame)
{
  const PlayerFrameData* localFrameData =
      findInPlayerFrameDataQueue(this->localPlayerFrameData, frame);
  if (!localFrameData || this->localPlayerFrameData.empty())
  {
    // this shouldn't happen
    ERROR_LOG_FMT(BRAWLBACK, "Couldn't find local inputs! Using empty pad.\n");
    return CreateBlankPlayerFrameData(frame, this->localPlayerIdx);
  }
  // INFO_LOG_FMT(BRAWLBACK, "Got local inputs frame = %u\n", localFrameData->frame);
  return *localFrameData;
}

void CEXIBrawlback::updateSync(bu32& locFrame, bu8 playerIdx)
{
  // https://gist.github.com/rcmagic/f8d76bca32b5609e85ab156db38387e9#file-rollbackpseudocode-txt-L46

  bu32 remoteFrame = this->GetLatestRemoteFrame();
  bs32 finalFrame = MIN(remoteFrame, locFrame);

  bool isSynchronized = true;

  if (isPredicting && this->shouldRollback(locFrame) && latestConfirmedFrame)
  {
    const PlayerFrameData playerPredictedInputs = predictedInputs.playerFrameDatas[playerIdx];
    INFO_LOG_FMT(BRAWLBACK,
                 "Checking predicted inputs against remote inputs. predicted frame = {}\n",
                 playerPredictedInputs.frame);

    for (int i = this->latestConfirmedFrame + 1; i <= finalFrame; i++)
    {
      const PlayerFrameData* remoteInputs =
          findInPlayerFrameDataQueue(this->remotePlayerFrameData[playerIdx], i);
      if (!remoteInputs)
      {
        ERROR_LOG_FMT(BRAWLBACK, "Couldn't find remote inputs for frame {} in updateSync!\n", i);
      }
      else
      {
        if (!isInputsEqual((*remoteInputs).pad, playerPredictedInputs.pad))
        {
          // remote inputs don't match predicted
          this->latestConfirmedFrame = i - 1;
          isSynchronized = false;
          INFO_LOG_FMT(BRAWLBACK, "Remote didn't match predicted inputs frame = {}\n", i);
          break;
        }
      }
    }
  }
  else
  {
    // INFO_LOG_FMT(BRAWLBACK, "Not predicting\n");
  }

  // remote inputs match predicted inputs or not predicting
  if (isSynchronized)
  {
    this->latestConfirmedFrame = finalFrame;
    // INFO_LOG_FMT(BRAWLBACK, "is synchronized!\n");
  }
  else
  {
    // not synchronized, rollback & resim
    INFO_LOG_FMT(BRAWLBACK, "Should rollback! frame = {} latestConfirmedFrame = {}\n", locFrame,
                 latestConfirmedFrame);
    IncrementalRB::Rollback(locFrame, latestConfirmedFrame);
    // if on frame 10 we rollback to frame 7 we need to simulate frames 7,8,9, and 10 to get to
    // where we were before. 10 - 7 + 1 = 4
    this->framesToAdvance = locFrame - this->latestConfirmedFrame + 1;
    INFO_LOG_FMT(BRAWLBACK, "Num frames to simulate = {}\n", framesToAdvance);
    this->stopRollbackFrame = locFrame;
    locFrame = this->latestConfirmedFrame;
    this->startRollbackFrame = locFrame;
  }

  // INFO_LOG_FMT(BRAWLBACK, "UpdateSync latestConfirmedFrame = %i\n", latestConfirmedFrame);
}
bool CEXIBrawlback::shouldRollback(bu32 locFrame)
{
  // https://gist.github.com/rcmagic/f8d76bca32b5609e85ab156db38387e9#file-rollbackpseudocode-txt-L30
  // local_frame > sync_frame AND remote_frame > sync_frame      # No need to rollback if we don't
  // have a frame after the previous sync frame to synchronize to.
  return locFrame > this->latestConfirmedFrame &&
         this->GetLatestRemoteFrame() > this->latestConfirmedFrame;
}

bool CEXIBrawlback::isRollbackMode(bu32 locFrame, u8 playerIdx)
{
  return
      ROLLBACK_IMPL &&  // delay-based/rollback toggle
         locFrame >
          GAME_FULL_START_FRAME &&  // give the game a bit of time in delay-based mode to sync up
      !this->remotePlayerFrameData.empty() &&  // some sanity checks
      !this->remotePlayerFrameData[playerIdx].empty() &&
      this->remotePlayerFrameData[playerIdx].size() >= MAX_ROLLBACK_FRAMES;
}

void CEXIBrawlback::handleEndFrame()
{
  NetPlay::OnFrameEnd();
}

void CEXIBrawlback::handleEndLoop()
{
  if (NetPlay::IsNetPlayRunning() && NetPlay::IsStarted())
  {
    IncrementalRB::OnFrameEnd(NetPlay::CurrentFrame(), NetPlay::IsRollingBack());
    if (NetPlay::IsRollingBack())
    {
      NetPlay::IncrementCurrentFrame();
    }
  }
}

void CEXIBrawlback::handleStartLoop(u8* payload)
{
  BrawlbackPad pad;
  memcpy(&pad, payload, sizeof(BrawlbackPad));
  SwapBrawlbackPadDataEndianess(pad);
  NetPlay::OnFrameStart(pad);

  s32 advanceFrames = NetPlay::AdvanceFrames();
  std::lock_guard<std::mutex> lock(read_queue_mutex);
  this->read_queue.clear();
  auto dataPtr = reinterpret_cast<u8*>(&advanceFrames);
  this->read_queue.insert(this->read_queue.end(), dataPtr, dataPtr + sizeof(s32));
}

void CEXIBrawlback::handleGetPort()
{
  u8 port = 0;
  if (NetPlay::IsNetPlayRunning())
  {
    for (int i = 0; i < NetPlay::netplay_client->GetPadMapping().size(); i++)
    {
      if (NetPlay::netplay_client->GetPadMapping().at(i) == NetPlay::netplay_client->GetLocalPlayerId())
        port = i;
    }
  }
  std::lock_guard<std::mutex> lock(read_queue_mutex);
  this->read_queue.clear();
  auto dataPtr = reinterpret_cast<u8*>(&port);
  this->read_queue.insert(this->read_queue.end(), dataPtr, dataPtr + sizeof(u8));
}

void CEXIBrawlback::handleGetInputs(bool local)
{
  // TODO: Doubles?
  std::optional<NetPlay::Inputs> pad = std::nullopt;
  std::optional<BrawlbackPad> predicted_pad = std::nullopt;
  BrawlbackPad game_pad = BrawlbackPad{};
  if (NetPlay::IsNetPlayRunning())
  {
    for (int i = 0; i < NetPlay::netplay_client->GetPadMapping().size(); i++)
    {
      if (local && NetPlay::netplay_client->GetPadMapping().at(i) ==
        NetPlay::netplay_client->GetLocalPlayerId())
      {
        pad = NetPlay::netplay_client->FindRemoteInputs(i, NetPlay::netplay_client->current_frame);
        predicted_pad = NetPlay::netplay_client->GetPredictedInputs(i, NetPlay::netplay_client->current_frame);
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

PlayerFrameData CEXIBrawlback::getRemoteInputs(bu32& locFrame, u8 playerIdx, bool& skipFrame)
{
  PlayerFrameData finalRemoteInputs;
  
  if (isRollbackMode(locFrame, playerIdx))
  {
    const PlayerFrameData* remoteFrameData =
        findInPlayerFrameDataQueue(this->remotePlayerFrameData[playerIdx], locFrame);

    if (remoteFrameData)
    {
      finalRemoteInputs = *remoteFrameData;
      // INFO_LOG_FMT(BRAWLBACK, "Found remote inputs frame = %u\n", finalRemoteInputs.frame);
      isPredicting = false;
    }
    else
    {
      INFO_LOG_FMT(BRAWLBACK, "No remote framedata!\n");
      // couldn't find remote inputs. Time to predict
      u32 predictedInputsFrame = this->latestConfirmedFrame;
      PlayerFrameData* previousInputs =
          findInPlayerFrameDataQueue(this->remotePlayerFrameData[playerIdx], predictedInputsFrame);
      if (!previousInputs)
      {
        ERROR_LOG_FMT(BRAWLBACK, "Failed to find predicted inputs for frame {} in getRemoteInputs!\n",
                  predictedInputsFrame);
        finalRemoteInputs = CreateBlankPlayerFrameData(locFrame, playerIdx);
      }
      else
      {
        INFO_LOG_FMT(BRAWLBACK, "Found predicted inputs for frame = {}\n", previousInputs->frame);
        finalRemoteInputs = *previousInputs;
        predictedInputs.playerFrameDatas[playerIdx] = *previousInputs;
        isPredicting = true;
      }
    }
  }
  else
  {
    const PlayerFrameData* remoteFrameData =
        findInPlayerFrameDataQueue(this->remotePlayerFrameData[playerIdx], locFrame);
    for (int i = 0; i < this->remotePlayerFrameData[playerIdx].size(); i++)
    {
      INFO_LOG_FMT(BRAWLBACK, "REMOTE PLAYER FRAME DATA FRAME (AT PLAYER INDEX {}): {}: {}",
                   playerIdx, i, this->remotePlayerFrameData[playerIdx][i]->frame);
    }
    // no rollbacks, use delay-based
    if (!remoteFrameData)
    {
      this->framesToAdvance = 0;
      finalRemoteInputs = CreateBlankPlayerFrameData(locFrame, playerIdx);
    }
    else
    {
      finalRemoteInputs = *remoteFrameData;
    }
  }

  return finalRemoteInputs;
}

void CEXIBrawlback::handleFrameAdvanceRequest(u8* data)
{
  // game requests the number of frames to simulate on this frame
  std::lock_guard<std::mutex> lock(read_queue_mutex);
  this->read_queue.clear();
  auto dataPtr = reinterpret_cast<u8*>(&this->framesToAdvance);
  this->read_queue.insert(this->read_queue.end(), dataPtr, dataPtr + sizeof(bu32));
}

void CEXIBrawlback::storeLocalInputs(PlayerFrameData* localPlayerFramedata)
{
#ifdef RANDOM_INPUTS
  if (this->localPlayerIdx == 0)
    *localPlayerFramedata =
        generateRandomInput(localPlayerFramedata->frame, localPlayerFramedata->playerIdx);
#endif
  // local inputs offset by FRAME_DELAY to mask latency
  // Once we hit frame X, we send inputs for that frame, but pretend they're from frame X+2
  // so those inputs now have an extra 2 frames to get to the opponent before the opponent's
  // client hits frame X+2.
  localPlayerFramedata->frame += FRAME_DELAY;

  /*std::fstream synclogFile;
  File::OpenFStream(synclogFile, File::GetExeDirectory() + "/local_inputs.txt", std::ios_base::out |
  std::ios_base::app); synclogFile << Sync::stringifyFramedata(*localPlayerFramedata) << "\n";
  synclogFile.close();*/

  // pFD->frame += FRAME_DELAY;
  std::unique_ptr<PlayerFrameData> pFD = std::make_unique<PlayerFrameData>(*localPlayerFramedata);
  // INFO_LOG_FMT(BRAWLBACK, "Frame %u PlayerIdx: %u numPlayers %u\n", localPlayerFramedata->frame,
  // localPlayerFramedata->playerIdx, this->numPlayers);

  // make sure we're storing inputs sequentially
  bool is_sequential_input = this->localPlayerFrameData.empty() || pFD->frame == this->localPlayerFrameData.back()->frame + 1;
  if (is_sequential_input)
  {
    // store local framedata
    if (this->localPlayerFrameData.size() > FRAMEDATA_MAX_QUEUE_SIZE)
    {
      // INFO_LOG_FMT(BRAWLBACK, "Popping local framedata for frame %u\n",
      // this->localPlayerFrameData.front()->frame);
      this->localPlayerFrameData.pop_front();
    }
    INFO_LOG_FMT(BRAWLBACK, "PUSHING PFD FOR FRAME {}\n", pFD->frame);
    this->localPlayerFrameData.push_back(std::move(pFD));
  }
  else
  {
    ERROR_LOG_FMT(BRAWLBACK, "Didn't push local framedata for frame {}\n", pFD->frame);
    WARN_LOG_FMT(BRAWLBACK, "iS THIS TRUE?: PFD->FRAME: {} == LOCAL PLAYER FRAME DATA BACK FRAME + 1: {}?\n", pFD->frame, this->localPlayerFrameData.back()->frame + 1);
  }
}

void CEXIBrawlback::handleSendInputs(u32 frame)
{
  if (this->localPlayerFrameData.empty())
    return;

  // broadcast this local framedata

  // each frame we send local inputs to the other client(s)
  // those clients then acknowledge those inputs and send that ack(nowledgement)
  // back to us. All acked inputs are irrelevant (unless we need to rollback)
  // since we know for a fact the remote client has them.
  // We track what frame of inputs has been acked ("localHeadFrame")
  // so that when we go to send inputs, we only send the un-acked ones.
  // We send *all* unacked inputs so that when the remote client doesn't receive inputs, and needs
  // to rollback the next packet will have all the inputs that that client hasn't received.
  int minAckFrame = this->timeSync->getMinAckFrame(this->numPlayers);

  // clamp to current frame to prevent it dropping local inputs that haven't been used yet
  minAckFrame = MIN<u32>(minAckFrame, frame);

  int localPadQueueSize = (int)this->localPlayerFrameData.size();
  if (localPadQueueSize == 0)
    return;  // no inputs, nothing to send
  int endIdx = -1;
  if (frame != 0)
    endIdx = localPadQueueSize-1 - (this->localPlayerFrameData.back()->frame - minAckFrame);


  std::vector<PlayerFrameData*> localFramedatas = {};
  // push framedatas from back to front
  // this means the resulting vector (localFramedatas) will have the most
  // recent framedata in the first position, and the oldest framedata in the last position
  for (int i = localPadQueueSize - 1; i > endIdx; i--)
  {
    if (i >= 0 && i < this->localPlayerFrameData.size())
    {
      const auto& localFramedata = this->localPlayerFrameData[i];
      // make sure we queue up these inputs sequentially
      if (localFramedatas.empty() ||
          (!localFramedatas.empty() && localFramedatas.back()->frame > localFramedata->frame))
      {
        PlayerFrameData* inputToSend = localFramedata.get();
        localFramedatas.push_back(inputToSend);
        INFO_LOG_FMT(BRAWLBACK, "INPUT TO SEND FRAME: {}\n", inputToSend->frame);
      }
    }
    else
    {
      ERROR_LOG_FMT(BRAWLBACK, "Requested more frame data than was available! This is very wrong!\n");
    }
  }

  // INFO_LOG_FMT(BRAWLBACK, "Broadcasting %i framedatas\n", localFramedatas.size());
  this->netplay->BroadcastPlayerFrameDataWithPastFrames(this->server, localFramedatas);

  u32 mostRecentFrame = this->localPlayerFrameData.back()->frame;  // current frame with delay
  this->timeSync->TimeSyncUpdate(mostRecentFrame, this->numPlayers);
}

bu32 CEXIBrawlback::GetLatestRemoteFrame()
{
  bu32 lowestFrame = 0;
  for (int i = 0; i < this->numPlayers; i++)
  {
    if (i == this->localPlayerIdx)
      continue;

    if (this->remotePlayerFrameData[i].empty())
    {
      return 0;
    }

    bu32 f = this->remotePlayerFrameData[i].back()->frame;
    if (f < lowestFrame || lowestFrame == 0)
    {
      lowestFrame = f;
    }
  }

  return lowestFrame;
}

void BroadcastFramedataAck(u32 frame, u8 playerIdx, BrawlbackNetplay* netplay, ENetHost* server)
{
  FrameAck ackData;
  ackData.frame = (int)frame;
  ackData.playerIdx = playerIdx;
  sf::Packet ackDataPacket = sf::Packet();
  u8 cmdbyte = NetPacketCommand::CMD_FRAME_DATA_ACK;
  ackDataPacket.append(&cmdbyte, sizeof(cmdbyte));
  ackDataPacket.append(&ackData, sizeof(FrameAck));
  netplay->BroadcastPacket(ackDataPacket, ENET_PACKET_FLAG_UNSEQUENCED, server);
  // INFO_LOG_FMT(BRAWLBACK, "Sent ack for frame %u  pidx %u", frame, (unsigned int)playerIdx);
}

void CEXIBrawlback::ProcessIndividualRemoteFrameData(PlayerFrameData* framedata)
{
  u8 playerIdx = framedata->playerIdx;
  u32 frame = framedata->frame;
  PlayerFrameDataQueue& remoteFramedataQueue = this->remotePlayerFrameData[playerIdx];

  if (!remoteFramedataQueue.empty())
  {
    // if the remote frame we're trying to process is not newer than the most recent frame, we don't
    // care about it
    if (frame <= remoteFramedataQueue.back()->frame)
      return;
    // make sure the inputs we're adding are sequential
    if (frame != remoteFramedataQueue.back()->frame + 1)
    {
      ERROR_LOG_FMT(BRAWLBACK, "Remote input is not sequential! ProcessIndividualRemoteFrameData\n");
      return;
    }
  }

  // INFO_LOG_FMT(BRAWLBACK, "Received opponent framedata. Player %u frame: %u\n", (unsigned
  // int)playerIdx, frame);

  std::unique_ptr<PlayerFrameData> f = std::make_unique<PlayerFrameData>(*framedata);
  remoteFramedataQueue.push_back(std::move(f));

  // clamp size of remote player framedata queue
  while (remoteFramedataQueue.size() > FRAMEDATA_MAX_QUEUE_SIZE)
  {
    // WARN_LOG_FMT(BRAWLBACK, "Hit remote player framedata queue max size! %u\n",
    // remoteFramedataQueue.size());
    remoteFramedataQueue.pop_front();
  }
}
const char* bit_rep[16] = {
    "0000", "0001", "0010", "0011", "0100", "0101", "0110", "0111",
    "1000", "1001", "1010", "1011", "1100", "1101", "1110", "1111",
};
void print_byte(u8 byte)
{
  INFO_LOG_FMT(BRAWLBACK, "{}{}", bit_rep[byte >> 4], bit_rep[byte & 0x0F]);
}
void print_half(u16 half)
{
  u8 byte0 = half >> 8;
  u8 byte1 = half & 0xFF;

  print_byte(byte0);
  print_byte(byte1);
}

void printInputs(const BrawlbackPad& pad)
{
  INFO_LOG_FMT(BRAWLBACK, " -- Pad --\n");
  INFO_LOG_FMT(BRAWLBACK, "StickX: {} ", pad.stickX);
  INFO_LOG_FMT(BRAWLBACK, "StickY: {} ", pad.stickY);
  INFO_LOG_FMT(BRAWLBACK, "CStickX: {} ", pad.cStickX);
  INFO_LOG_FMT(BRAWLBACK, "CStickY: {}\n", pad.cStickY);
  INFO_LOG_FMT(BRAWLBACK, "Buttons: ");
  print_half(pad.newPressedButtons);
  INFO_LOG_FMT(BRAWLBACK, " LTrigger: {}    RTrigger {}\n", pad.LAnalogue, pad.RAnalogue);
  // OSReport(" ---------\n");
}

void CEXIBrawlback::ProcessRemoteFrameData(PlayerFrameData* framedatas, u8 numFramedatas_u8)
{
  s32 numFramedatas = (s32)numFramedatas_u8;
  // framedatas may point to one or more PlayerFrameData's.
  // Also note. this array is the reverse of the local pad queue, in that
  // the 0th element here is the most recent framedata.
  PlayerFrameData* mostRecentFramedata = &framedatas[0];
  u32 frame = mostRecentFramedata->frame;
  u8 playerIdx = mostRecentFramedata->playerIdx;  // remote player idx

  // acknowledge that we received opponent's framedata
  BroadcastFramedataAck(frame, playerIdx, this->netplay.get(), this->server);
  // ---------------------

  // Just print for other player
  if (this->isHost && playerIdx == 1)
  {
    // INFO_LOG_FMT(BRAWLBACK, "Received remote inputs from %i", playerIdx);
    if (mostRecentFramedata->sysPad.newPressedButtons > 0)
    {
      printInputs(mostRecentFramedata->sysPad);
    }
  }
  // if (!this->remotePlayerFrameData[playerIdx].empty())
  // INFO_LOG_FMT(BRAWLBACK, "Received remote inputs. Head frame %u  received head frame %u\n",
  // this->remotePlayerFrameData[playerIdx].back()->frame, frame);

  if (numFramedatas > 0)
  {
    std::lock_guard<std::mutex> lock(remotePadQueueMutex);

    std::stringstream s;
    s << "Received " << numFramedatas << " framedatas. [";
    for (int i = 0; i < numFramedatas; i++)
    {
      s << framedatas[i].frame << " , ";
    }
    s << "]";
    // INFO_LOG_FMT(BRAWLBACK, "%s\n", s.str().c_str());

    u32 maxFrame = 0;
    // index 0 is most recent, and we want to process new framedata oldest first, then newer ones
    for (s32 i = numFramedatas - 1; i >= 0; i--)
    {
      PlayerFrameData* framedata = &framedatas[i];
      maxFrame = framedata->frame > maxFrame ? framedata->frame : maxFrame;
      this->ProcessIndividualRemoteFrameData(framedata);
    }

    this->timeSync->ReceivedRemoteFramedata(maxFrame, localPlayerIdx, this->hasGameStarted);
  }
}

void CEXIBrawlback::ProcessFrameAck(FrameAck* frameAck)
{
  if (frameAck->playerIdx != this->localPlayerIdx)  // should be local player
    ERROR_LOG_FMT(BRAWLBACK, "FrameAck playeridx is not local player idx! (This is wrong...)\n");
  else
    this->timeSync->ProcessFrameAck(frameAck);
}

void CEXIBrawlback::ProcessGameSettings(GameSettings* opponentGameSettings)
{
  // merge game settings for all remote/local players, then pass that back to the game

  this->localPlayerIdx = this->isHost ? 0 : 1;
  // assumes 1v1
  int remotePlayerIdx = this->isHost ? 1 : 0;

  // doing this so functionally equivalen to what White had before
  //  Probably should just use gameSettings directly...
  GameSettings& mergedGameSettings = gameSettings;
  INFO_LOG_FMT(BRAWLBACK, "ProcessGameSettings for player {}\n", this->localPlayerIdx);
  INFO_LOG_FMT(BRAWLBACK, "Remote player idx: {}\n", remotePlayerIdx);

  mergedGameSettings.localPlayerIdx = this->localPlayerIdx;

  // TODO: again assign proper stuff based on reality and since we really don't know what the player
  // port is right now since netplay menu is not being used for this atm, we are going to assume
  // that always p1 vs p1 are getting connected when netlay menu is set, get player port from game.
  mergedGameSettings.localPlayerPort = 0;  // p1

  this->numPlayers = mergedGameSettings.numPlayers;
  INFO_LOG_FMT(BRAWLBACK, "Num players from emu: {}\n", (unsigned int)this->numPlayers);

  // this is kinda broken kinda unstable and weird.
  // hardcoded "fix" for testing. Get rid of this when you're confident this is stable
  if (this->numPlayers == 0)
  {
    this->numPlayers = 2;
    mergedGameSettings.numPlayers = 2;
  }

  if (!this->isHost)
  {  // is not host
    mergedGameSettings.randomSeed = opponentGameSettings->randomSeed;

    // get a random stage on the non-host side.
    mergedGameSettings.stageID = matchmaking->GetRandomStage();

    // if not host, your character will be p2, if host, your char will be p1

    // copy char into both slots
    mergedGameSettings.playerSettings[1].charID = mergedGameSettings.playerSettings[0].charID;
    mergedGameSettings.playerSettings[1].charColor = mergedGameSettings.playerSettings[0].charColor;
    mergedGameSettings.playerSettings[1].rumble = mergedGameSettings.playerSettings[0].rumble;
    mergedGameSettings.playerSettings[1].colorFileIndex = mergedGameSettings.playerSettings[0].colorFileIndex;
    // copy char from opponent p1 into our p1
    mergedGameSettings.playerSettings[0].charID = opponentGameSettings->playerSettings[0].charID;
    mergedGameSettings.playerSettings[0].charColor = opponentGameSettings->playerSettings[0].charColor;
    mergedGameSettings.playerSettings[0].rumble = opponentGameSettings->playerSettings[0].rumble;
    mergedGameSettings.playerSettings[0].colorFileIndex = opponentGameSettings->playerSettings[0].colorFileIndex;
  }
  else
  {  // is host
    // copy char from opponent's p2 into our p2
    mergedGameSettings.playerSettings[1].charID = opponentGameSettings->playerSettings[1].charID;
    mergedGameSettings.playerSettings[1].charColor = opponentGameSettings->playerSettings[1].charColor;
    mergedGameSettings.playerSettings[1].rumble = opponentGameSettings->playerSettings[1].rumble;
    mergedGameSettings.playerSettings[1].colorFileIndex = opponentGameSettings->playerSettings[1].colorFileIndex;

    // set our stage based on the one the other client generated
    mergedGameSettings.stageID = opponentGameSettings->stageID;
  }
  mergedGameSettings.playerSettings[localPlayerIdx].playerType = PlayerType::PLAYERTYPE_LOCAL;
  mergedGameSettings.playerSettings[remotePlayerIdx].playerType = PlayerType::PLAYERTYPE_REMOTE;

  // TODO: for now just set port 3 and 4 as disconnect/NONE
  mergedGameSettings.playerSettings[2].playerType = PlayerType::PLAYERTYPE_NONE;
  mergedGameSettings.playerSettings[3].playerType = PlayerType::PLAYERTYPE_NONE;

  // if we're not host, we just connected to host and received their game settings,
  // now we need to send our game settings back to them so they can start their game too
  if (!this->isHost)
  {
    this->netplay->BroadcastGameSettings(this->server, &mergedGameSettings);
  }

  std::lock_guard<std::mutex> lock(read_queue_mutex);
  this->read_queue.clear();
  this->read_queue.push_back(EXICommand::CMD_SETUP_PLAYERS);
  auto gameSettingsPtr = reinterpret_cast<u8*>(&mergedGameSettings);
  this->read_queue.insert(this->read_queue.end(), gameSettingsPtr,
                          gameSettingsPtr + sizeof(GameSettings));
}

// called from netplay thread
void CEXIBrawlback::ProcessNetReceive(ENetEvent* event)
{
  ENetPacket* pckt = event->packet;
  if (pckt && pckt->data && pckt->dataLength > 0)
  {
    u8* fullpckt_data = pckt->data;
    u8 cmd_byte = fullpckt_data[0];
    u8* data = &fullpckt_data[1];

    switch (cmd_byte)
    {
    case NetPacketCommand::CMD_FRAME_DATA:
    {
      u8 numFramedatas = data[0];
      PlayerFrameData* framedata = (PlayerFrameData*)(&data[1]);
      this->ProcessRemoteFrameData(framedata, numFramedatas);
    }
    break;
    case NetPacketCommand::CMD_FRAME_DATA_ACK:
    {
      FrameAck* frameAck = (FrameAck*)data;
      this->ProcessFrameAck(frameAck);
    }
    break;
    case NetPacketCommand::CMD_GAME_SETTINGS:
    {
      INFO_LOG_FMT(BRAWLBACK, "Received game settings from opponent");
      GameSettings* gameSettingsFromOpponent = (GameSettings*)data;
      this->ProcessGameSettings(gameSettingsFromOpponent);
    }
    break;
    case NetPacketCommand::CMD_CONNECT:
    {
      INFO_LOG_FMT(BRAWLBACK, "Recieved connection packet from opponent");
    }
    break;
    default:
      WARN_LOG_FMT(BRAWLBACK, "Unknown packet cmd byte!");
      std::stringstream ss;
      ss << fullpckt_data;
      INFO_LOG_FMT(BRAWLBACK, "Packet as string: {}\n", ss.str());
      break;
    }
  }
}

void CEXIBrawlback::NetplayThreadFunc()
{
  Common::SetCurrentThreadName("BrawlbackNetplay");

  enet_uint32 timeout;
  // loop until we connect to someone, then after we connected,
  // do another loop for passing data between the connected clients
  ENetEvent event;
  if (this->server != nullptr)
  {
    INFO_LOG_FMT(BRAWLBACK, "Waiting for connection to opponent {}:{}...",
                 this->server->peers[0].address.host, this->server->peers[0].address.port);
  }
  bool qos_success = false;
#ifdef _WIN32
  QOS_VERSION ver = {1, 0};

  if (QOSCreateHandle(&ver, &m_qos_handle))
  {
    struct sockaddr_in sin = {0};

    sin.sin_family = AF_INET;
    sin.sin_port = ENET_HOST_TO_NET_16(this->peer->host->address.port);
    sin.sin_addr.s_addr = this->peer->host->address.host;

    if (QOSAddSocketToFlow(m_qos_handle, this->peer->host->socket,
                           reinterpret_cast<PSOCKADDR>(&sin),
                           // this is 0x38
                           QOSTrafficTypeControl, QOS_NON_ADAPTIVE_FLOW, &m_qos_flow_id))
    {
      DWORD dscp = 0x2e;

      // this will fail if we're not admin
      // sets DSCP to the same as linux (0x2e)
      QOSSetFlow(m_qos_handle, m_qos_flow_id, QOSSetOutgoingDSCPValue, sizeof(DWORD), &dscp, 0,
                 nullptr);

      qos_success = true;
    }
#else
#ifdef __linux__
  // highest priority
  int priority = 7;
  setsockopt(this->peer->socket, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority));
#endif

  // https://www.tucny.com/Home/dscp-tos
  // ef is better than cs7
  int tos_val = 0xb8;
  qos_success =
      setsockopt(this->server->socket, IPPROTO_IP, IP_TOS, &tos_val, sizeof(tos_val)) == 0;
#endif
  }
  timeout = this->isHost ? 5000 : 1000;
  while (!this->isConnected)
  {
    auto net = enet_host_service(this->server, &event, timeout);
    if (net > 0)
    {
      switch (event.type)
      {
      case ENET_EVENT_TYPE_CONNECT:
        INFO_LOG_FMT(BRAWLBACK, "Connected!");
        if (event.peer)
        {
          INFO_LOG_FMT(BRAWLBACK, "A new client connected from {:#x}:{:d}\n",
                       event.peer->address.host, event.peer->address.port);
          this->isConnected = true;
        }
        else
        {
          WARN_LOG_FMT(BRAWLBACK, "Connect event received, but peer was null!");
        }
        break;
      case ENET_EVENT_TYPE_NONE:
        // INFO_LOG_FMT(BRAWLBACK, "Enet event type none. Nothing to do");
        break;
      }
    }
  }
  if (this->isHost)
  {  // if we're host, send our game settings to clients right after connecting
    this->netplay->BroadcastGameSettings(this->server, &this->gameSettings);
  }

  INFO_LOG_FMT(BRAWLBACK, "Starting main net data loop");
  // main enet loop
  while (enet_host_service(this->server, &event, 0) >= 0 && this->isConnected &&
         !this->timeSync->getIsConnectionStalled())
  {
    this->netplay->FlushAsyncQueue(this->server);
    switch (event.type)
    {
    case ENET_EVENT_TYPE_DISCONNECT:
      // INFO_LOG_FMT(BRAWLBACK, "%s:%u disconnected.\n", event.peer -> address.host, event.peer ->
      // address.port);
      INFO_LOG_FMT(BRAWLBACK, "disconnected.\n");
      this->isConnected = false;
      break;
    case ENET_EVENT_TYPE_NONE:
      // INFO_LOG_FMT(BRAWLBACK, "Enet event type none. Nothing to do");
      break;
    case ENET_EVENT_TYPE_RECEIVE:
      this->ProcessNetReceive(&event);
      enet_packet_destroy(event.packet);
      break;
    }
  }

  ERROR_LOG_FMT(BRAWLBACK, "~~~~~~~~~~~~~ END ENET THREAD ~~~~~~~~~~~~~~~");
}

void CEXIBrawlback::MatchmakingThreadFunc()
{
  Common::SetCurrentThreadName("BrawlbackMatchmakingPhase2");
  while (this->matchmaking)
  {
    switch (this->matchmaking->GetMatchmakeState())
    {
    case Matchmaking::ProcessState::OPPONENT_CONNECTING:
      this->matchmaking->SetMatchmakeState(Matchmaking::ProcessState::CONNECTION_SUCCESS);
      this->connectToOpponent();
      break;
    case Matchmaking::ProcessState::CONNECTION_SUCCESS:
      break;
    case Matchmaking::ProcessState::ERROR_ENCOUNTERED:
      ERROR_LOG_FMT(BRAWLBACK, "MATCHMAKING: ERROR TRYING TO CONNECT!");
      return;
      break;
    default:
      break;
    }
  }
  INFO_LOG_FMT(BRAWLBACK, "~~~~~~~~~~~~~~ END MATCHMAKING PHASE 2 THREAD ~~~~~~~~~~~~~~\n");
}

void CEXIBrawlback::connectToOpponent()
{
  this->isHost = this->matchmaking->IsHost();
  ENetAddress addr;
  std::string address;
  if (this->isHost)
  {
    INFO_LOG_FMT(BRAWLBACK, "Matchmaking: Creating server...");
  }
  else
  {
    INFO_LOG_FMT(BRAWLBACK, "Matchmaking: Creating client...");
  }
  addr.host = ENET_HOST_ANY;
  addr.port = this->matchmaking->GetLocalPort();
  this->server = enet_host_create(&addr, 10, 3, 0, 0);
  if (this->server == nullptr)
  {
    ERROR_LOG_FMT(BRAWLBACK, "Failed to connect to any client/host");
    return;
  }
  bool connectedToAtLeastOne = false;
  for (int i = 0; i < this->matchmaking->RemotePlayerCount(); i++)
  {
    address = this->matchmaking->GetRemoteIPAddresses()[i];
    int set_host_res = enet_address_set_host(&addr, address.c_str());
    if (set_host_res < 0)
    {
      WARN_LOG_FMT(BRAWLBACK, "Failed to enet_address_set_host");
    }
    addr.port = this->matchmaking->GetRemotePorts()[i];
    this->peer = enet_host_connect(this->server, &addr, 3, 0);
    if (this->peer == nullptr)
    {
      WARN_LOG_FMT(BRAWLBACK, "Failed to enet_host_connect");
    }
    {
      using namespace std::chrono_literals;
      auto PEER_TIMEOUT = 30s;
      enet_peer_timeout(this->peer, 0, PEER_TIMEOUT.count(), PEER_TIMEOUT.count());
    }
    connectedToAtLeastOne = true;
  }
  if (!connectedToAtLeastOne)
  {
    ERROR_LOG_FMT(BRAWLBACK, "Failed to connect to any client/host");
    return;
  }
  this->server->mtu = std::min(this->server->mtu, NetPlay::MAX_ENET_MTU);

  this->netplay_thread = std::thread(&CEXIBrawlback::NetplayThreadFunc, this);
}

void CEXIBrawlback::handleFindMatch(u8* payload)
{
  // if (!payload) return;

#ifdef LOCAL_TESTING
  ENetAddress address;
  address.host = ENET_HOST_ANY;
  address.port = BRAWLBACK_PORT;

  this->server = enet_host_create(&address, 3, 0, 0, 0);

#define IP_FILENAME "/connect.txt"
  std::string connectIP = "127.0.0.1";

  if (File::Exists(File::GetExeDirectory() + IP_FILENAME))
  {
    std::fstream file;
    File::OpenFStream(file, File::GetExeDirectory() + IP_FILENAME, std::ios_base::in);
    connectIP.clear();
    std::getline(file, connectIP);  // read in only one line
    file.close();
    INFO_LOG_FMT(BRAWLBACK, "IP: %s\n", connectIP.c_str());
  }
  else
  {
    INFO_LOG_FMT(BRAWLBACK, "Creating connect file\n");
    std::fstream file;
    file.open(File::GetExeDirectory() + IP_FILENAME, std::ios_base::out);
    file << connectIP;
    file.close();
  }

  // just for testing. This should be replaced with a check to see if we are the "host" of the
  // match or not
  if (this->server == NULL)
  {
    this->isHost = false;
    WARN_LOG_FMT(BRAWLBACK, "Failed to init enet server!");
    WARN_LOG_FMT(BRAWLBACK, "Creating client instead...");
    this->server = enet_host_create(NULL, 3, 0, 0, 0);
    // for (int i = 0; i < 1; i++) { // make peers for all connecting opponents

    ENetAddress addr;
    int set_host_res = enet_address_set_host(&addr, connectIP.c_str());
    if (set_host_res < 0)
    {
      WARN_LOG_FMT(BRAWLBACK, "Failed to enet_address_set_host");
      return;
    }
    addr.port = BRAWLBACK_PORT;

    ENetPeer* peer = enet_host_connect(this->server, &addr, 1, 0);
    if (peer == NULL)
    {
      WARN_LOG_FMT(BRAWLBACK, "Failed to enet_host_connect");
      return;
    }

    //}
  }

  INFO_LOG_FMT(BRAWLBACK, "Net initialized, starting netplay thread");
  // loop to receive data over net
  this->netplay_thread = std::thread(&CEXIBrawlback::NetplayThreadFunc, this);
  return;
#endif

  Matchmaking::MatchSearchSettings search;
  std::string connectCode;

  // TODO: uncomment these lines when payload includes the actual mode and connect codes
#ifdef REMOVE_THIS_WHEN_PAYLOAD_IS_SET
  search.mode = (SlippiMatchmaking::OnlinePlayMode)payload[0];
  std::string shiftJisCode;
  shiftJisCode.insert(shiftJisCode.begin(), &payload[1], &payload[1] + 18);
  shiftJisCode.erase(std::find(shiftJisCode.begin(), shiftJisCode.end(), 0x00), shiftJisCode.end());
  connectCode = shiftJisCode;
#else
  search.mode = Matchmaking::OnlinePlayMode::UNRANKED;
#endif

  switch (search.mode)
  {
  case Matchmaking::OnlinePlayMode::DIRECT:
  case Matchmaking::OnlinePlayMode::TEAMS:
    search.connectCode = connectCode;
    break;
  default:
    break;
  }

  // Store this search so we know what was queued for
  lastSearch = search;
  matchmaking->FindMatch(search);
  this->matchmaking_thread = std::thread(&CEXIBrawlback::MatchmakingThreadFunc, this);
}

void CEXIBrawlback::handleStartMatch(u8* payload)
{
  // if (!payload) return;
  std::memcpy(&gameSettings, payload, sizeof(GameSettings));
}

#include "../../Externals/curl/curl/include/curl/curl.h"
#include <Common/MemoryUtil.h>
#include <incremental-rollback/mem.h>

void swapGameReportEndian(GameReport& report)
{
  for (int i = 0; i < MAX_NUM_PLAYERS; i++)
  {
    report.damage[i] = swap_endian(report.damage[i]);
    report.stocks[i] = swap_endian(report.stocks[i]);
  }
  report.frame_duration = swap_endian(report.frame_duration);
}

void CEXIBrawlback::handleEndMatch(u8* payload)
{
  GameReport report;
  memcpy(&report, payload, sizeof(GameReport));

  swapGameReportEndian(report);

  auto userInfo = this->matchmaking->GetUserInfo();
  INFO_LOG_FMT(BRAWLBACK, "Sending match end report\n");

  json request;
  request["uid"] = userInfo.uid;
  request["playKey"] = userInfo.playKey;
  request["gameIndex"] = gameIndex;
  request["gameDurationFrames"] = report.frame_duration;

  json players = json::array();
  for (int i = 0; i < this->numPlayers; i++)
  {
    json p;
    const Brawlback::UserInfo& playerInfo = this->matchmaking->GetPlayerInfo()[i];
    p["uid"] = playerInfo.uid;
    p["damageDone"] = report.damage[i];
    p["stocksRemaining"] = report.stocks[i];

    players[i] = p;
  }

  request["players"] = players;

  auto requestString = request.dump();

  static const std::string reportURL = "https://lylat.gg/reports";

  CURL* curl = curl_easy_init();  // TODO: init this earlier
  // Send report
  curl_easy_setopt(curl, CURLOPT_POST, true);
  curl_easy_setopt(curl, CURLOPT_URL, reportURL.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestString.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, requestString.length());
  CURLcode res = curl_easy_perform(curl);

  if (res != 0)
  {
    ERROR_LOG_FMT(BRAWLBACK, "[GameReport] Got error executing request. Err code: {#d}", (int)res);
  }
  else
  {
    INFO_LOG_FMT(BRAWLBACK, "Successfully send end match report to {}\n", reportURL.c_str());
  }

  if (curl)
  {
    curl_easy_cleanup(curl);
  }
  this->firstDump = true;
  gameIndex++;
}

void CEXIBrawlback::handleStartReplaysStruct(u8* payload)
{
  StartReplay startReplay;
  std::memcpy(&startReplay, payload, sizeof(StartReplay));

  auto start = this->curReplayJson["start"];
  for (int i = 0; i < startReplay.numPlayers; i++)
  {
    auto player = start["players"][i];
    auto replayPlayer = startReplay.players[i];
    player["ftKind"] = replayPlayer.fighterKind;
    auto position = player["startPlayerPos"];
    auto replayPosition = replayPlayer.startPlayer;

    position["x"] = replayPosition.xPos;
    position["y"] = replayPosition.yPos;
    position["z"] = replayPosition.zPos;
  }
  start["stage"] = startReplay.stage;
  start["randomSeed"] = startReplay.randomSeed;
  start["otherRandomSeed"] = startReplay.otherRandomSeed;
}

void CEXIBrawlback::handleReplaysStruct(u8* payload)
{
  Replay replay;
  std::memcpy(&replay, payload, sizeof(Replay));

  const auto frameName = fmt::format("frame_{}", replay.frameCounter);
  // this->curReplayJson[frameName]["persistentFrameCounter"] = replay.persistentFrameCounter;
  for (int i = 0; i < replay.numItems; i++)
  {
    auto item = this->curReplayJson[frameName]["items"][i];
    auto replayItem = replay.items[i];

    item["itemId"] = replayItem.itemId;
    item["itemVariant"] = replayItem.itemVariant;
  }
  for (int i = 0; i < replay.numPlayers; i++)
  {
    auto player = this->curReplayJson[frameName]["players"][i];
    auto inputs = player["inputs"];
    auto position = player["position"];

    auto replayPlayer = replay.players[i];
    auto replayInputs = replayPlayer.inputs;
    auto replayPosition = replayPlayer.pos;

    player["actionState"] = replayPlayer.actionState;
    player["damage"] = replayPlayer.damage;
    player["stockCount"] = replayPlayer.stockCount;

    // Commenting out until replay playback gets merged on master
    /*inputs["attack"] = replayInputs.attack;
    inputs["cStick"] = replayInputs.cStick;
    inputs["dTaunt"] = replayInputs.dTaunt;
    inputs["jump"] = replayInputs.jump;
    inputs["leftStickX"] = replayInputs.leftStickX;
    inputs["leftStickY"] = replayInputs.leftStickY;
    inputs["shield"] = replayInputs.shield;
    inputs["special"] = replayInputs.special;
    inputs["sTaunt"] = replayInputs.sTaunt;
    inputs["tapJump"] = replayInputs.tapJump;
    inputs["uTaunt"] = replayInputs.uTaunt;
    */

    position["x"] = replayPosition.xPos;
    position["y"] = replayPosition.yPos;
    position["z"] = replayPosition.zPos;
  }
}

void CEXIBrawlback::handleEndOfReplay()
{
  auto ubjson = json::to_ubjson(this->curReplayJson);

  const auto p1 = std::chrono::system_clock::now();
  const auto timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count();
  writeToFile("replay_" + std::to_string(timestamp) + ".brba", ubjson.data(), ubjson.size());
}

void SwapEndianSavestateMemRegionInfo(SavestateMemRegionInfo& memRegion)
{
  memRegion.address = swap_endian(memRegion.address);
  memRegion.size = swap_endian(memRegion.size);
}

void CEXIBrawlback::handleDumpAll(u8* payload)
{
  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();
  SavestateMemRegionInfo dumpAll;
  memcpy(&dumpAll, payload, sizeof(SavestateMemRegionInfo));
  SwapEndianSavestateMemRegionInfo(dumpAll);

  SlippiUtility::Savestate::ssBackupLoc addDumpAll;
  addDumpAll.data = nullptr;
  addDumpAll.startAddress = dumpAll.address;
  addDumpAll.endAddress = dumpAll.address + dumpAll.size;
  addDumpAll.regionName = std::string((char*)dumpAll.nameBuffer, dumpAll.nameSize);
  addDumpAll.frame = 0;
  if (addDumpAll.regionName == "Fighter1Resoruce" || addDumpAll.regionName == "Fighter2Resoruce" || addDumpAll.regionName == "IteamResource")
  {
    u8* data = static_cast<u8*>(Common::AllocateAlignedMemory(3, 64));
    memory.CopyFromEmuSwapped(data, addDumpAll.startAddress, 3);
    if (std::string((char*)data, 3) != "ARC")
    {
      dynamicRegions.push_back(addDumpAll);
    }
  }
  else
  {
    dynamicRegions.push_back(addDumpAll);
  }
}

void CEXIBrawlback::handleAlloc(u8* payload)
{
  auto& system = Core::System::GetInstance();
  auto& memory = system.GetMemory();
  SavestateMemRegionInfo alloc;
  memcpy(&alloc, payload, sizeof(SavestateMemRegionInfo));
  SwapEndianSavestateMemRegionInfo(alloc);
  SlippiUtility::Savestate::ssBackupLoc addAlloc;
  addAlloc.data = nullptr;
  addAlloc.startAddress = alloc.address;
  addAlloc.endAddress = alloc.address + alloc.size;
  addAlloc.regionName = std::string((char*)alloc.nameBuffer, alloc.nameSize);
  addAlloc.frame = this->lastStatedFrame;
  if (addAlloc.regionName == "Fighter1Resoruce" || addAlloc.regionName == "Fighter2Resoruce" ||
      addAlloc.regionName == "IteamResource")
  {
    u8* data = static_cast<u8*>(Common::AllocateAlignedMemory(3, 64));
    memory.CopyFromEmuSwapped(data, addAlloc.startAddress, 3);
    if (std::string((char*)data, 3) != "ARC")
    {
      dynamicRegions.push_back(addAlloc);
    }
  }
  else
  {
    dynamicRegions.push_back(addAlloc);
  }
}

void CEXIBrawlback::handleDealloc(u8* payload)
{
  SavestateMemRegionInfo dealloc;
  memcpy(&dealloc, payload, sizeof(SavestateMemRegionInfo));
  auto startAddress = swap_endian(dealloc.address);
  dynamicRegions.erase(std::remove_if(std::begin(dynamicRegions), std::end(dynamicRegions), [&startAddress](ssBackupLoc obj) { return (obj.startAddress == startAddress); }), std::end(dynamicRegions));
}
void CEXIBrawlback::handleFrameCounterLoc(u8* payload)
{
  bu32 frameCounterLocation;
  memcpy(&frameCounterLocation, payload, sizeof(bu32));
  frameCounterLocation = swap_endian(frameCounterLocation);

  IncrementalRB::AddFrameCounterLocation(frameCounterLocation);
}
void CEXIBrawlback::handleCancelMatchmaking()
{
  this->matchmaking->CloseMatching();
  if (this->matchmaking_thread.joinable())
  {
    this->matchmaking_thread.join();
  }
}
void CEXIBrawlback::handleUpdateSync(u8* payload)
{
  bu32 frame;
  std::memcpy(&frame, payload, sizeof(bu32));
  frame = swap_endian(frame);
  if (this->framesToAdvance != 0)
  {
    for (s32 i = 0; i < this->numPlayers; i++)
    {
      if (isRollbackMode(frame, i))
      {
        this->updateSync(frame, i);
      }
    }
  }
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
  case CMD_ONLINE_INPUTS:
    // INFO_LOG_FMT(BRAWLBACK, "DMAWrite: CMD_ONLINE_INPUTS");
    handleLocalPadData(payload);
    break;
  case CMD_FRAMEDATA:
    handleFrameDataRequest(payload);
    break;
  case CMD_UPDATESYNC:
    handleUpdateSync(payload);
    break;
  case CMD_FRAMEADVANCE:
    handleFrameAdvanceRequest(payload);
    break;
  case CMD_MATCH_END:
    handleEndMatch(payload);
    break;
  case CMD_CAPTURE_SAVESTATE:
    // INFO_LOG_FMT(BRAWLBACK, "DMAWrite: CMD_CAPTURE_SAVESTATE");
    handleCaptureSavestate(payload);
    break;
  case CMD_LOAD_SAVESTATE:
    // INFO_LOG_FMT(BRAWLBACK, "DMAWrite: CMD_LOAD_SAVESTATE");
    handleLoadSavestate(payload);
    break;
  case CMD_FIND_OPPONENT:
    // INFO_LOG_FMT(BRAWLBACK, "DMAWrite: CMD_FIND_OPPONENT");
    handleFindMatch(payload);
    break;
  case CMD_START_MATCH:
    // INFO_LOG_FMT(BRAWLBACK, "DMAWrite: CMD_START_MATCH");
    handleStartMatch(payload);
    break;
  case CMD_REPLAY_START_REPLAYS_STRUCT:
    handleStartReplaysStruct(payload);
    break;
  case CMD_REPLAY_REPLAYS_STRUCT:
    handleReplaysStruct(payload);
    break;
  case CMD_REPLAYS_REPLAYS_END:
    handleEndOfReplay();
    break;
  case CMD_SEND_DUMPALL:
    handleDumpAll(payload);
    break;
  case CMD_SEND_ALLOCS:
    handleAlloc(payload);
    break;
  case CMD_SEND_DEALLOCS:
    handleDealloc(payload);
    break;
  case CMD_SEND_FRAMECOUNTERLOC:
    handleFrameCounterLoc(payload);
    break;
  case CMD_CANCEL_MATCHMAKING:
    handleCancelMatchmaking();
    break;
  case CMD_END_FRAME:
    handleEndFrame();
    break;
  // just using these CMD's to track frame times lol
  case CMD_END_LOOP:
    handleEndLoop();
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
  case CMD_TIMER_START:
  {
    frameTime = Common::Timer::NowUs();
    break;
  }
  case CMD_TIMER_END:
  {
    // u32 timeDiff = Common::Timer::NowUs() - frameTime;
    // INFO_LOG_FMT(BRAWLBACK, "Game logic took %f ms\n", (double)(timeDiff / 1000.0));
    break;
  }

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
