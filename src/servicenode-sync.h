// Copyright (c) 2019-2019 The Ankh Core Developers
// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2017 The Dash Core Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CREDIT_SERVICENODE_SYNC_H
#define CREDIT_SERVICENODE_SYNC_H

#include "chain.h"
#include "net.h"

#include <univalue.h>

class CServiceNodeSync;

static const int SERVICENODE_SYNC_FAILED = -1;
static const int SERVICENODE_SYNC_INITIAL = 0; // sync just started, was reset recently or still in IDB
static const int SERVICENODE_SYNC_WAITING = 1; // waiting after initial to see if we can get more headers/blocks
static const int SERVICENODE_SYNC_LIST = 2;
static const int SERVICENODE_SYNC_DNW = 3;
static const int SERVICENODE_SYNC_GOVERNANCE = 4;
static const int SERVICENODE_SYNC_GOVOBJ = 10;
static const int SERVICENODE_SYNC_GOVOBJ_VOTE = 11;
static const int SERVICENODE_SYNC_FINISHED = 999;

static const int SERVICENODE_SYNC_TICK_SECONDS = 6;
static const int SERVICENODE_SYNC_TIMEOUT_SECONDS = 25;

static const int SERVICENODE_SYNC_ENOUGH_PEERS = 10;

extern CServiceNodeSync servicenodeSync;

//
// CServiceNodeSync : Sync ServiceNode assets in stages
//

class CServiceNodeSync
{
private:
    // Keep track of current asset
    int nRequestedServiceNodeAssets;
    // Count peers we've requested the asset from
    int nRequestedServiceNodeAttempt;

    // Time when current ServiceNode asset sync started
    int64_t nTimeAssetSyncStarted;

    // ... last bumped
    int64_t nTimeLastBumped;

    // ... or failed
    int64_t nTimeLastFailure;

    void Fail();

public:
    CServiceNodeSync() { Reset(); }

    void SendGovernanceSyncRequest(CNode* pnode, CConnman& connman);

    bool IsFailed() { return nRequestedServiceNodeAssets == SERVICENODE_SYNC_FAILED; }
    bool IsBlockchainSynced() { return nRequestedServiceNodeAssets > SERVICENODE_SYNC_WAITING; }
    bool IsServiceNodeListSynced() { return nRequestedServiceNodeAssets > SERVICENODE_SYNC_LIST; }
    bool IsWinnersListSynced() { return nRequestedServiceNodeAssets > SERVICENODE_SYNC_DNW; }
    bool IsSynced() { return nRequestedServiceNodeAssets == SERVICENODE_SYNC_FINISHED; }

    int GetAssetID() { return nRequestedServiceNodeAssets; }
    int GetAttempt() { return nRequestedServiceNodeAttempt; }
    void BumpAssetLastTime(const std::string strFuncName);
    int64_t GetAssetStartTime() { return nTimeAssetSyncStarted; }
    std::string GetAssetName();
    std::string GetSyncStatus();

    void Reset();
    void SwitchToNextAsset(CConnman& connman);

    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv);
    void ProcessTick(CConnman& connman);

    void AcceptedBlockHeader(const CBlockIndex* pindexNew);
    void NotifyHeaderTip(const CBlockIndex* pindexNew, bool fInitialDownload, CConnman& connman);
    void UpdatedBlockTip(const CBlockIndex* pindexNew, bool fInitialDownload, CConnman& connman);

    void DoMaintenance(CConnman& connman);
    double SyncProgress();

};

#endif // CREDIT_SERVICENODE_SYNC_H
