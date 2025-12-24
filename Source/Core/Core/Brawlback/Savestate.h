#pragma once

#include "SlippiUtility.h"

using namespace SlippiUtility::Savestate;

// thank you Slippi :)

class BrawlbackSavestate
{

public:


    BrawlbackSavestate(std::vector<SlippiUtility::Savestate::ssBackupLoc> staticRegions);
    ~BrawlbackSavestate();


    void Capture(std::vector<SlippiUtility::Savestate::ssBackupLoc> dynamicRegions);
    void Load(std::vector<PreserveBlock> blocks);

    //static bool shouldForceInit;

    std::vector<ssBackupLoc>* getBackupLocs() { return &staticLocs; }

    int frame = -1;
    int checksum = -1;
private:


    std::vector<ssBackupLoc> staticLocs = {};
    std::vector<ssBackupLoc> dynamicLocs = {};
    std::unordered_map<PreserveBlock, std::vector<u8>, preserve_hash_fn, preserve_eq_fn> preservationMap;
    std::vector<u8> dolphinSsBackup = {};

    void getDolphinState(PointerWrap& p);


    void initStaticLocs(std::vector<SlippiUtility::Savestate::ssBackupLoc> staticRegions);
    void initDynamicLocs(std::vector<SlippiUtility::Savestate::ssBackupLoc> dynamicRegions);

    //std::thread firstHalf;
    //std::thread secondHalf;


};
