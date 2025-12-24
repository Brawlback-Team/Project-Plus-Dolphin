#include <filesystem>

#include "BrawlbackUtility.h"

#include "VideoCommon/OnScreenDisplay.h"
#include <Core/HW/Memmap.h>
#include <fmt/format.h>
#include <Core/System.h>
namespace Brawlback
{
    
    bool isButtonPressed(u16 buttonBits, PADButtonBits button)
    {
        return (buttonBits & (PADButtonBits::Z << 8)) != 0;
    }
    /*
     * Simple checksum function stolen from wikipedia:
     *
     *   http://en.wikipedia.org/wiki/Fletcher%27s_checksum
     */

    int fletcher32_checksum(short* data, size_t len)
    {
      int sum1 = 0xffff, sum2 = 0xffff;

      while (len)
      {
        size_t tlen = len > 360 ? 360 : len;
        len -= tlen;
        do
        {
          sum1 += *data++;
          sum2 += sum1;
        } while (--tlen);
        sum1 = (sum1 & 0xffff) + (sum1 >> 16);
        sum2 = (sum2 & 0xffff) + (sum2 >> 16);
      }

      /* Second reduction step to reduce sums to 16 bits */
      sum1 = (sum1 & 0xffff) + (sum1 >> 16);
      sum2 = (sum2 & 0xffff) + (sum2 >> 16);
      return sum2 << 16 | sum1;
    }

    // super slow
    int SavestateChecksum(std::vector<ssBackupLoc>* backupLocs)
    {
      auto& system = Core::System::GetInstance();
      auto& memory = system.GetMemory();
      if (backupLocs->empty())
        return -1;
      std::vector<int> sums = {};
      for (ssBackupLoc backupLoc : *backupLocs)
      {
        size_t size = backupLoc.endAddress - backupLoc.startAddress;
        auto data = memory.GetPointerForRange(backupLoc.startAddress, size);
        if (data && size)
          sums.push_back(fletcher32_checksum((short*)data, backupLoc.endAddress - backupLoc.startAddress));
        else
          ERROR_LOG_FMT(BRAWLBACK,
                               "Invalid data or size of savestate when computing checksum!\n");
      }
      return fletcher32_checksum((short*)sums.data(), sizeof(int) * sums.size());
    }

    PlayerFrameData* findInPlayerFrameDataQueue(const PlayerFrameDataQueue& queue, u32 frame)
    {
      PlayerFrameData* ret = nullptr;
#if 0
        if (!queue.empty()) {
            // this works under the assumption that the framedata queue is ordered sequentially by frame
            u32 begin = queue.front()->frame;
            u32 end = queue.back()->frame;
            if (frame >= begin && frame <= end) {
                int idx = frame - begin;
                ASSERT(idx >= 0 && idx < queue.size());
                PlayerFrameData* framedata = queue[idx].get();
                ASSERT(framedata->frame == frame);
                ret = framedata;
            }
        }
#else
      for (int i = ((int)queue.size()) - 1; i >= 0; i--)
      {
        if (queue[i]->frame == frame)
        {
          ret = queue[i].get();
          break;
        }
      }
#endif
      return ret;
    }

    namespace Match {
        
        bool isPlayerFrameDataEqual(const PlayerFrameData& p1, const PlayerFrameData& p2)
    {
            //bool frames = p1.frame == p2.frame;
            //bool idxs = p1.playerIdx == p2.playerIdx;
            bool _buttons = p1.pad._buttons == p2.pad._buttons;
            bool buttons = p1.pad.buttons == p2.pad.buttons;
            bool holdButtons = p1.pad.holdButtons == p2.pad.holdButtons;
            bool rapidFireButtons = p1.pad.rapidFireButtons == p2.pad.rapidFireButtons;
            bool releasedButtons = p1.pad.releasedButtons == p2.pad.releasedButtons;
            bool newPressedButtons = p1.pad.newPressedButtons == p2.pad.newPressedButtons;
            bool sticks = p1.pad.stickX == p2.pad.stickX &&
                          p1.pad.stickY == p2.pad.stickY &&
                          p1.pad.cStickX == p2.pad.cStickX &&
                          p1.pad.cStickY == p2.pad.cStickY;
            bool triggers = p1.pad.LAnalogue == p2.pad.LAnalogue &&
                            p1.pad.RAnalogue == p2.pad.RAnalogue;
            return _buttons && buttons && holdButtons && rapidFireButtons && releasedButtons && newPressedButtons && sticks && triggers;
        }

    }


    namespace Mem {
        
        const char* bit_rep[16] = {
            "0000", "0001", "0010", "0011", "0100", "0101", "0110", "0111",
            "1000", "1001", "1010", "1011", "1100", "1101", "1110", "1111",
        };

        void print_byte(uint8_t byte)
        {
            ERROR_LOG_FMT(BRAWLBACK, "Byte: {}{}\n", bit_rep[byte >> 4],
                          bit_rep[byte & 0x0F]);
        }
        void print_half(u16 half) {
            u8 byte0 = half >> 8;
            u8 byte1 = half & 0xFF;

            print_byte(byte0);
            print_byte(byte1);
        }
        void print_word(u32 word) {
            u8 byte0 = word >> 24;
            u8 byte1 = (word & 0xFF0000) >> 16;
            u8 byte2 = (word & 0xFF00) >> 8;
            u8 byte3 = word & 0xFF;

            print_byte(byte0);
            print_byte(byte1);
            print_byte(byte2);
            print_byte(byte3);
        }




        void fillByteVectorWithBuffer(std::vector<u8>& vec, u8* buf, size_t size) {
            u32 idx = 0;
            while (idx < size) {
                vec.push_back(buf[idx]);
                idx++;
            }
        }

        
        bool isIntersect(PreserveBlock a, PreserveBlock b)
        {
          return std::max(a.address, a.address + a.length) >= std::min(b.address, b.address + b.length);
        }
        void manipulate2(std::vector<PreserveBlock>& a, PreserveBlock y)
        {
          PreserveBlock x = a.back();
          a.pop_back();
          PreserveBlock z = x;
          x.address = y.address + y.length;
          z.length = y.address - z.address;
          if (z.address < z.address + z.length)
            a.push_back(z);
          if (x.address < x.address + x.length)
            a.push_back(x);
        }
        std::vector<PreserveBlock> removeInterval(std::vector<PreserveBlock>& in, PreserveBlock& t)
        {
          std::vector<PreserveBlock> ans;
          std::size_t n = in.size();
          for (int i = 0; i < n; i++)
          {
            ans.push_back(in[i]);
            PreserveBlock a;
            PreserveBlock b;
            a = ans.back();
            b = t;
            if (a.address > b.address)
              std::swap(a, b);
            if (isIntersect(a, b))
            {
              manipulate2(ans, t);
            }
          }
          return ans;
        }
        
    }

    namespace Sync {
        // utilities to use for logging game info & finding desyncs
        using Mem::bit_rep;

        std::string getSyncLogFilePath() { return File::GetExeDirectory() + "/synclog.txt"; }
        
        std::string str_byte(uint8_t byte)
        {
            std::string ret = std::string(bit_rep[byte >> 4]) + std::string(bit_rep[byte & 0x0F]);
            //INFO_LOG_FMT(BRAWLBACK, "Byte: {}{}\n", bit_rep[byte >> 4], bit_rep[byte & 0x0F]);
            return ret;
        }
        std::string str_half(u16 half) {
            u8 byte0 = half >> 8;
            u8 byte1 = half & 0xFF;

            std::string ret;
            ret.append(str_byte(byte0));
            ret.append(str_byte(byte1));
            return ret;
        }

        void SyncLog(const std::string& msg) {
            if (!msg.empty()) {
                std::fstream synclogFile;
                File::OpenFStream(synclogFile, getSyncLogFilePath(), std::ios_base::out | std::ios_base::app);
                synclogFile << msg << "\n";
                synclogFile.close();
            }
        }

        std::string stringifyPad(const BrawlbackPad& pad) {
            std::string inputs;

            std::string sticks = "[StickX: " + std::to_string(pad.stickX) + "] [StickY: " + std::to_string(pad.stickY) + "]\n";
            inputs.append(sticks);
            
            std::string csticks = "[CStickX: " + std::to_string(pad.cStickX) + "] [CStickY: " + std::to_string(pad.cStickY) + "]\n";
            inputs.append(csticks);
            
            std::string triggers = "[LTrigger: " + std::to_string(pad.LAnalogue) +
                                   "] [RTrigger: " + std::to_string(pad.RAnalogue) + "] ";
            inputs.append(triggers);
            
            std::string buttons = "[Buttons: " + str_half(pad.buttons) + "]\n";
            inputs.append(buttons);
            return inputs;
        }

        std::string stringifyFramedata(const PlayerFrameData& pfd) {
            std::string ret;

            std::string info;
            info.append("[Frame " + std::to_string(pfd.frame) + "] [P" + std::to_string(pfd.playerIdx) + "]\n");

            ret.append(info);
            ret.append(stringifyPad(pfd.pad));
            ret.append(stringifyPad(pfd.sysPad));
            return ret;
        }

        std::string stringifyFramedata(const BrawlbackFrameData& fd, int numPlayers)
        {
          std::string ret;
          for (int i = 0; i < numPlayers; i++)
          {
            const auto& pfd = fd.playerFrameDatas[i];
            if (pfd.frame != 0)
              ret.append(Sync::stringifyFramedata(pfd));
          }
          return ret;
        }


    }

    namespace Dump {
        
        void DumpArray(const std::string& filename, const u8* data, size_t length)
        {
            if (!data)
                return;

            File::IOFile f(filename, "wb");

            if (!f)
            {
                ERROR_LOG_FMT(BRAWLBACK,
                              "Failed to dump {}: Can't open file\n", filename);
                return;
            }

            if (!f.WriteBytes(data, length))
            {
                ERROR_LOG_FMT(BRAWLBACK,
                              "Failed to dump {}: Failed to write to file\n", filename);
            }
        }

        void DoMemDumpIteration(int& dump_num) {
            std::string dump_num_str = std::to_string(dump_num);
            std::string frame_folder = File::GetUserPath(D_DUMP_IDX) + "memdumps/dump" + dump_num_str;
            

            std::string mem1_file = frame_folder + "/mem1_" + dump_num_str + ".raw";
            std::string mem2_file = frame_folder + "/mem2_" + dump_num_str + ".raw";
            File::CreateFullPath(mem1_file);
            File::CreateFullPath(mem2_file);
            Dump::DumpMem(AddressSpace::Type::Mem1, mem1_file);
            Dump::DumpMem(AddressSpace::Type::Mem2, mem2_file);
            dump_num += 1;
        }

        void DumpMem(AddressSpace::Type memType, const std::string& dumpPath) {
            AddressSpace::Accessors* accessors = AddressSpace::GetAccessors(memType);
            DumpArray(dumpPath, accessors->begin(),
                    std::distance(accessors->begin(), accessors->end()));
        }
    }

}
