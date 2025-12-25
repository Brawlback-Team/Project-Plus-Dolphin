#pragma once

#include <unordered_map>
#include <array>
#include <fstream>
#include <optional>
#include <random>
#include <algorithm>

#include "Common/FileUtil.h"
#include "Common/CommonTypes.h"
#include "Common/Timer.h"
#include "Common/Logging/Log.h"
#include "Common/Logging/LogManager.h"

#include "SlippiUtility.h"
#include "Brawltypes.h"
#include "Savestate.h"
#include "include/brawlback-common/ExiStructures.h"
#include "include/brawlback-common/BrawlbackConstants.h"

// ---
// mem dumping related
#include "Core/HW/AddressSpace.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
// ---

namespace Brawlback {

    struct UserInfo
    {
      std::string uid = "";
      std::string playKey = "";
      std::string displayName = "";
      std::string connectCode = "";
      std::string latestVersion = "";
      std::string fileContents = "";

      int port;
    };

    // TODO: put this in the submodule and pack it
    struct GameReport {
        double damage[MAX_NUM_PLAYERS];
        s32 stocks[MAX_NUM_PLAYERS];
        s32 frame_duration;
    };

    enum EXICommand : u8
    {
      CMD_UNKNOWN = 0,
      CMD_END_FRAME = 1,
      CMD_END_LOOP = 2,
      CMD_START_LOOP = 3,
      CMD_GET_PORT = 4,
      CMD_GET_REMOTE_INPUTS = 5,
      CMD_GET_LOCAL_INPUTS = 6
    };

    enum NetPacketCommand : u8 
    {
        CMD_FRAME_DATA = 1,
        CMD_GAME_SETTINGS = 2,
        CMD_FRAME_DATA_ACK = 3,
        CMD_CONNECT = 4
    };

    struct FrameOffsetData {
        int idx;
        std::vector<s32> buf;
    };
    namespace Match
    {   
        bool isPlayerFrameDataEqual(const PlayerFrameData& p1, const PlayerFrameData& p2);
    }

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

    inline void SwapBrawlbackPadDataEndianess(BrawlbackPad& pad)
    {
      pad._buttons = swap_endian(pad._buttons);
      pad.buttons = swap_endian(pad.buttons);
      pad.holdButtons = swap_endian(pad.holdButtons);
      pad.rapidFireButtons = swap_endian(pad.rapidFireButtons);
      pad.releasedButtons = swap_endian(pad.releasedButtons);
      pad.newPressedButtons = swap_endian(pad.newPressedButtons);
    }

    inline void SwapPlayerFrameDataEndianness(PlayerFrameData& pfd) {
        pfd.frame = swap_endian(pfd.frame);
        pfd.syncData.anim = swap_endian(pfd.syncData.anim);
        pfd.syncData.locX = swap_endian(pfd.syncData.locX);
        pfd.syncData.locY = swap_endian(pfd.syncData.locY);
        pfd.syncData.percent = swap_endian(pfd.syncData.percent);
        pfd.randomSeed = swap_endian(pfd.randomSeed);
        SwapBrawlbackPadDataEndianess(pfd.pad);
        SwapBrawlbackPadDataEndianess(pfd.sysPad);
    }
    inline void SwapFrameDataEndianness(BrawlbackFrameData& fd)
    {
        for (int i = 0; i < MAX_NUM_PLAYERS; i++) {
            SwapPlayerFrameDataEndianness(fd.playerFrameDatas[i]);
        }
        fd.randomSeed = swap_endian(fd.randomSeed);
        fd.skipFrame = swap_endian(fd.skipFrame);
    }

    inline void PrintSyncData(const SyncData& data) {
        INFO_LOG_FMT(
            BRAWLBACK,
            "xPos = {}  yPos = {}  facingDir = {}  anim = {}  percent = {}  stocks = {}\n",
            data.locX, data.locY, (s32)data.facingDir, data.anim, data.percent, (u32)data.stocks);
    }

    // checks if the specified `button` is held down based on the buttonBits bitfield
    bool isButtonPressed(u16 buttonBits, PADButtonBits button);
    namespace Mem {
        void print_byte(uint8_t byte);
        void print_half(u16 half);
        void print_word(u32 word);

        template <typename T>
        std::vector<u8> structToByteVector(T* s) {
            auto ptr = reinterpret_cast<u8*>(s);
            return std::vector<u8>(ptr, ptr + sizeof(T));
        }

        void fillByteVectorWithBuffer(std::vector<u8>& vec, u8* buf, size_t size);
        bool isIntersect(PreserveBlock a, PreserveBlock b);
        void manipulate2(std::vector<PreserveBlock>& a, PreserveBlock y);
        std::vector<PreserveBlock> removeInterval(std::vector<PreserveBlock>& in, PreserveBlock& t);
    }
    namespace Sync {
        std::string getSyncLogFilePath();
        std::string str_byte(uint8_t byte);
        std::string str_half(u16 half);
        void SyncLog(const std::string& msg);
        std::string stringifyFramedata(const BrawlbackFrameData& fd, int numPlayers);
        std::string stringifyFramedata(const PlayerFrameData& pfd);
        std::string stringifyPad(const BrawlbackPad& pad);
    }
    
    typedef std::deque<std::unique_ptr<PlayerFrameData>> PlayerFrameDataQueue;

    PlayerFrameData* findInPlayerFrameDataQueue(const PlayerFrameDataQueue& queue,
                                                           u32 frame);

    inline bool isSyncDataEqual(const SyncData& p1, const SyncData& p2)
    {
        bool xLoc = p1.locX == p2.locX;
        bool yLoc = p1.locY == p2.locY;
        bool anim = p1.anim == p2.anim;
        bool facingDir = p1.facingDir == p2.facingDir;
        return xLoc && yLoc && anim && facingDir;
    }
    inline bool isInputsEqual(const BrawlbackPad& p1, const BrawlbackPad& p2) {
        // TODO: this code is duplicated on the .cpp make it dry or I don't know
        bool _buttons = p1._buttons == p2._buttons;
        bool buttons = p1.buttons == p2.buttons;
        bool holdButtons = p1.holdButtons == p2.holdButtons;
        bool rapidFireButtons = p1.rapidFireButtons == p2.rapidFireButtons;
        bool releasedButtons = p1.releasedButtons == p2.releasedButtons;
        bool newPressedButtons = p1.newPressedButtons == p2.newPressedButtons;
        bool triggers = p1.LAnalogue == p2.LAnalogue && p1.RAnalogue == p2.RAnalogue;
        bool analogSticks = p1.stickX == p2.stickX && p1.stickY == p2.stickY;
        bool cSticks = p1.cStickX == p2.cStickX && p1.cStickY == p2.cStickY;
        return _buttons && buttons && holdButtons && rapidFireButtons && releasedButtons && newPressedButtons && analogSticks && cSticks && triggers;

        //return memcmp(&p1, &p2, sizeof(BrawlbackPad)) == 0;
    }

    inline PlayerFrameData generateRandomInput(s32 frame, u8 pIdx) {
        PlayerFrameData ret;
        ret.frame = frame;
        ret.playerIdx = pIdx;
        std::default_random_engine generator = std::default_random_engine((s32)Common::Timer::NowUs());
        ret.pad.buttons = (u16)((generator() % 65535));
        //ret.pad.stickX = (u8)(127-generator() % (127*2));
        ret.pad.stickX = 0;
        ret.pad.stickY = (u8)(127-generator() % (127*2));
        ret.pad.cStickX = (u8)(127-generator() % (127*2));
        ret.pad.cStickY = (u8)(127-generator() % (127*2));
        ret.pad.LAnalogue = (u8)(127-generator() % (127*2));
        ret.pad.RAnalogue = (u8)(127-generator() % (127*2));
        return ret;
    }

    template <typename T>
    T Clamp(T input, T Max, T Min) {
        return input > Max ? Max : ( input < Min ? Min : input );
    }


    template <typename T>
    inline T MAX(T x, T y) { return (((x) > (y)) ? (x) : (y)); }
    template <typename T>
    inline T MIN(T x, T y) { return (((x) < (y)) ? (x) : (y)); }
    // 1 if in range (inclusive), 0 otherwise
    template <typename T>
    inline T RANGE(T i, T min, T max) { return ((i < min) || (i > max) ? 0 : 1); }

    namespace Dump {
        void DoMemDumpIteration(int& dump_num);
        void DumpMem(AddressSpace::Type memType, const std::string& dumpPath);
    }

}
