// Copyright (c) 2019-2019 The Ankh Core Developers
// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2017 The Dash Core Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "servicenode-sync.h"

#include "activeservicenode.h"
#include "checkpoints.h"
#include "servicenode-payments.h"
#include "servicenode.h"
#include "servicenodeman.h"
#include "governance.h"
#include "init.h"
#include "netfulfilledman.h"
#include "netmessagemaker.h"
#include "spork.h"
#include "ui_interface.h"
#include "util.h"
#include "validation.h"

class CServiceNodeSync;
CServiceNodeSync servicenodeSync;

void CServiceNodeSync::Fail()
{
    nTimeLastFailure = GetTime();
    nRequestedServiceNodeAssets = SERVICENODE_SYNC_FAILED;
}

void CServiceNodeSync::Reset()
{
    nRequestedServiceNodeAssets = SERVICENODE_SYNC_INITIAL;
    nRequestedServiceNodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
    nTimeLastBumped = GetTime();
    nTimeLastFailure = 0;
}

void CServiceNodeSync::BumpAssetLastTime(const std::string strFuncName)
{
    if (IsSynced() || IsFailed())
        return;
    nTimeLastBumped = GetTime();
    LogPrint("dnsync", "CServiceNodeSync::BumpAssetLastTime -- %s\n", strFuncName);
}

std::string CServiceNodeSync::GetAssetName()
{
    switch (nRequestedServiceNodeAssets) {
    case (SERVICENODE_SYNC_INITIAL):
        return "SERVICENODE_SYNC_INITIAL";
    case (SERVICENODE_SYNC_WAITING):
        return "SERVICENODE_SYNC_WAITING";
    case (SERVICENODE_SYNC_LIST):
        return "SERVICENODE_SYNC_LIST";
    case (SERVICENODE_SYNC_DNW):
        return "SERVICENODE_SYNC_DNW";
    case (SERVICENODE_SYNC_GOVERNANCE):
        return "SERVICENODE_SYNC_GOVERNANCE";
    case (SERVICENODE_SYNC_FAILED):
        return "SERVICENODE_SYNC_FAILED";
    case SERVICENODE_SYNC_FINISHED:
        return "SERVICENODE_SYNC_FINISHED";
    default:
        return "UNKNOWN";
    }
}

void CServiceNodeSync::SwitchToNextAsset(CConnman& connman)
{
    switch (nRequestedServiceNodeAssets) {
    case (SERVICENODE_SYNC_FAILED):
        throw std::runtime_error("Can't switch to next asset from failed, should use Reset() first!");
        break;
    case (SERVICENODE_SYNC_INITIAL):
        nRequestedServiceNodeAssets = SERVICENODE_SYNC_WAITING;
        LogPrint("servicenode", "CServiceNodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
        break;
    case (SERVICENODE_SYNC_WAITING):
        LogPrint("servicenode", "CServiceNodeSync::SwitchToNextAsset -- Completed %s in %llds\n", GetAssetName(), GetTime() - nTimeAssetSyncStarted);
        nRequestedServiceNodeAssets = SERVICENODE_SYNC_LIST;
        LogPrint("servicenode", "CServiceNodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
        break;
    case (SERVICENODE_SYNC_LIST):
        LogPrint("servicenode", "CServiceNodeSync::SwitchToNextAsset -- Completed %s in %llds\n", GetAssetName(), GetTime() - nTimeAssetSyncStarted);
        nRequestedServiceNodeAssets = SERVICENODE_SYNC_DNW;
        LogPrint("servicenode", "CServiceNodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
        break;
    case (SERVICENODE_SYNC_DNW):
        LogPrint("servicenode", "CServiceNodeSync::SwitchToNextAsset -- Completed %s in %llds\n", GetAssetName(), GetTime() - nTimeAssetSyncStarted);
        nRequestedServiceNodeAssets = SERVICENODE_SYNC_GOVERNANCE;
        LogPrint("servicenode", "CServiceNodeSync::SwitchToNextAsset -- Starting %s\n", GetAssetName());
        break;
    case (SERVICENODE_SYNC_GOVERNANCE):
        LogPrint("servicenode", "CServiceNodeSync::SwitchToNextAsset -- Completed %s in %llds\n", GetAssetName(), GetTime() - nTimeAssetSyncStarted);
        nRequestedServiceNodeAssets = SERVICENODE_SYNC_FINISHED;
        uiInterface.NotifyAdditionalDataSyncProgressChanged(1);
        //try to activate our servicenode if possible
        activeServiceNode.ManageState(connman);

        // TODO: Find out whether we can just use LOCK instead of:
        // TRY_LOCK(cs_vNodes, lockRecv);
        // if(lockRecv) { ... }

        connman.ForEachNode(CConnman::AllNodes, [](CNode* pnode) {
            netfulfilledman.AddFulfilledRequest(pnode->addr, "full-sync");
        });
        LogPrint("servicenode", "CServiceNodeSync::SwitchToNextAsset -- Sync has finished\n");

        break;
    }
    nRequestedServiceNodeAttempt = 0;
    nTimeAssetSyncStarted = GetTime();
    BumpAssetLastTime("CServiceNodeSync::SwitchToNextAsset");
}

std::string CServiceNodeSync::GetSyncStatus()
{
    switch (servicenodeSync.nRequestedServiceNodeAssets) {
    case SERVICENODE_SYNC_INITIAL:
        return _("Synchronizing blockchain...");
    case SERVICENODE_SYNC_WAITING:
        return _("Synchronization pending...");
    case SERVICENODE_SYNC_LIST:
        return _("Synchronizing ServiceNodes...");
    case SERVICENODE_SYNC_DNW:
        return _("Synchronizing ServiceNode payments...");
    case SERVICENODE_SYNC_GOVERNANCE:
        return _("Synchronizing governance objects...");
    case SERVICENODE_SYNC_FAILED:
        return _("Synchronization failed");
    case SERVICENODE_SYNC_FINISHED:
        return _("Synchronization finished");
    default:
        return "";
    }
}

void CServiceNodeSync::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv)
{
    if (strCommand == NetMsgType::SYNCSTATUSCOUNT) { //Sync status count

        //do not care about stats if sync process finished or failed
        if (IsSynced() || IsFailed())
            return;

        int nItemID;
        int nCount;
        vRecv >> nItemID >> nCount;

        LogPrint("servicenode", "SYNCSTATUSCOUNT -- got inventory count: nItemID=%d  nCount=%d  peer=%d\n", nItemID, nCount, pfrom->id);
    }
}

double CServiceNodeSync::SyncProgress()
{
    // Calculate additional data "progress" for syncstatus RPC call
    double nSyncProgress = double(nRequestedServiceNodeAttempt + (nRequestedServiceNodeAssets - 1) * 8) / (8 * 4);
    if (nSyncProgress < 0)
        nSyncProgress = 0;

    if (nSyncProgress > 1)
        nSyncProgress = 1;

    return nSyncProgress;
}

void CServiceNodeSync::ProcessTick(CConnman& connman)
{
    static int nTick = 0;
    nTick++;

    // reset the sync process if the last call to this function was more than 60 minutes ago (client was in sleep mode)
    static int64_t nTimeLastProcess = GetTime();
    if (GetTime() - nTimeLastProcess > 60 * 60) {
        LogPrintf("CServiceNodeSync::HasSyncFailures -- WARNING: no actions for too long, restarting sync...\n");
        Reset();
        SwitchToNextAsset(connman);
        nTimeLastProcess = GetTime();
        return;
    }
    nTimeLastProcess = GetTime();

    // reset sync status in case of any other sync failure
    if (IsFailed()) {
        if (nTimeLastFailure + (1 * 60) < GetTime()) { // 1 minute cooldown after failed sync
            LogPrintf("CServiceNodeSync::HasSyncFailures -- WARNING: failed to sync, trying again...\n");
            Reset();
            SwitchToNextAsset(connman);
        }
        return;
    }

    // gradually request the rest of the votes after sync finished
    if (IsSynced()) {
        std::vector<CNode*> vNodesCopy = connman.CopyNodeVector(CConnman::FullyConnectedOnly);
        governance.RequestGovernanceObjectVotes(vNodesCopy, connman);
        connman.ReleaseNodeVector(vNodesCopy);
        return;
    }

    // Calculate "progress" for LOG reporting / GUI notification
    double nSyncProgress = double(nRequestedServiceNodeAttempt + (nRequestedServiceNodeAssets - 1) * 8) / (8 * 4);
    LogPrint("servicenode", "CServiceNodeSync::ProcessTick -- nTick %d nRequestedServiceNodeAssets %d nRequestedServiceNodeAttempt %d nSyncProgress %f\n", nTick, nRequestedServiceNodeAssets, nRequestedServiceNodeAttempt, nSyncProgress);
    uiInterface.NotifyAdditionalDataSyncProgressChanged(nSyncProgress);

    std::vector<CNode*> vNodesCopy = connman.CopyNodeVector(CConnman::FullyConnectedOnly);

    for (auto& pnode : vNodesCopy) {
        CNetMsgMaker msgMaker(pnode->GetSendVersion());
        // Don't try to sync any data from outbound "servicenode" connections -
        // they are temporary and should be considered unreliable for a sync process.
        // Inbound connection this early is most likely a "servicenode" connection
        // initialted from another node, so skip it too.
        if (pnode->fServiceNode || (fServiceNodeMode && pnode->fInbound))
            continue;
        // QUICK MODE (REGTEST ONLY!)
        if (Params().NetworkIDString() == CBaseChainParams::REGTEST) {
            if (nRequestedServiceNodeAttempt <= 2) {
                connman.PushMessage(pnode, msgMaker.Make(NetMsgType::GETSPORKS)); //get current network sporks
            } else if (nRequestedServiceNodeAttempt < 4) {
                dnodeman.PsegUpdate(pnode, connman);
            } else if (nRequestedServiceNodeAttempt < 6) {
                //sync payment votes
                if (pnode->nVersion == 70900) {
                    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::SERVICENODEPAYMENTSYNC, snpayments.GetStorageLimit())); //sync payment votes
                } else {
                    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::SERVICENODEPAYMENTSYNC)); //sync payment votes
                }
                SendGovernanceSyncRequest(pnode, connman);
            } else {
                nRequestedServiceNodeAssets = SERVICENODE_SYNC_FINISHED;
            }
            nRequestedServiceNodeAttempt++;
            connman.ReleaseNodeVector(vNodesCopy);
            return;
        }

        // NORMAL NETWORK MODE - TESTNET/MAINNET
        {
            if (netfulfilledman.HasFulfilledRequest(pnode->addr, "full-sync")) {
                // We already fully synced from this node recently,
                // disconnect to free this connection slot for another peer.
                pnode->fDisconnect = true;
                LogPrint("servicenode", "CServiceNodeSync::ProcessTick -- disconnecting from recently synced peer %d\n", pnode->id);
                continue;
            }

            // SPORK : ALWAYS ASK FOR SPORKS AS WE SYNC

            if (!netfulfilledman.HasFulfilledRequest(pnode->addr, "spork-sync")) {
                // always get sporks first, only request once from each peer
                netfulfilledman.AddFulfilledRequest(pnode->addr, "spork-sync");
                // get current network sporks
                connman.PushMessage(pnode, msgMaker.Make(NetMsgType::GETSPORKS));
                LogPrint("servicenode", "CServiceNodeSync::ProcessTick -- nTick %d nRequestedServiceNodeAssets %d -- requesting sporks from peer %d\n", nTick, nRequestedServiceNodeAssets, pnode->id);
            }

            // INITIAL TIMEOUT

            if (nRequestedServiceNodeAssets == SERVICENODE_SYNC_WAITING) {
                if (GetTime() - nTimeLastBumped > SERVICENODE_SYNC_TIMEOUT_SECONDS) {
                    // At this point we know that:
                    // a) there are peers (because we are looping on at least one of them);
                    // b) we waited for at least SERVICENODE_SYNC_TIMEOUT_SECONDS since we reached
                    //    the headers tip the last time (i.e. since we switched from
                    //     SERVICENODE_SYNC_INITIAL to SERVICENODE_SYNC_WAITING and bumped time);
                    // c) there were no blocks (UpdatedBlockTip, NotifyHeaderTip) or headers (AcceptedBlockHeader)
                    //    for at least SERVICENODE_SYNC_TIMEOUT_SECONDS.
                    // We must be at the tip already, let's move to the next asset.
                    SwitchToNextAsset(connman);
                }
            }

            // DNLIST : SYNC SERVICENODE LIST FROM OTHER CONNECTED CLIENTS

            if (nRequestedServiceNodeAssets == SERVICENODE_SYNC_LIST) {
                LogPrint("servicenode", "CServiceNodeSync::ProcessTick -- nTick %d nRequestedServiceNodeAssets %d nTimeLastBumped %lld GetTime() %lld diff %lld\n", nTick, nRequestedServiceNodeAssets, nTimeLastBumped, GetTime(), GetTime() - nTimeLastBumped);
                // check for timeout first
                if (GetTime() - nTimeLastBumped > SERVICENODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrint("servicenode", "CServiceNodeSync::ProcessTick -- nTick %d nRequestedServiceNodeAssets %d -- timeout\n", nTick, nRequestedServiceNodeAssets);
                    if (nRequestedServiceNodeAttempt == 0) {
                        LogPrintf("CServiceNodeSync::ProcessTick -- ERROR: failed to sync %s\n", GetAssetName());
                        // there is no way we can continue without ServiceNode list, fail here and try later
                        Fail();
                        connman.ReleaseNodeVector(vNodesCopy);
                        return;
                    }
                    SwitchToNextAsset(connman);
                    connman.ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // request from three peers max
                if (nRequestedServiceNodeAttempt > 2) {
                    connman.ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // only request once from each peer
                if (netfulfilledman.HasFulfilledRequest(pnode->addr, "servicenode-list-sync"))
                    continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "servicenode-list-sync");

                if (pnode->nVersion < snpayments.GetMinServiceNodePaymentsProto())
                    continue;
                nRequestedServiceNodeAttempt++;

                dnodeman.PsegUpdate(pnode, connman);

                connman.ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }

            // DNW : SYNC SERVICENODE PAYMENT VOTES FROM OTHER CONNECTED CLIENTS

            if (nRequestedServiceNodeAssets == SERVICENODE_SYNC_DNW) {
                LogPrint("snpayments", "CServiceNodeSync::ProcessTick -- nTick %d nRequestedServiceNodeAssets %d nTimeLastBumped %lld GetTime() %lld diff %lld\n", nTick, nRequestedServiceNodeAssets, nTimeLastBumped, GetTime(), GetTime() - nTimeLastBumped);
                // check for timeout first
                // This might take a lot longer than SERVICENODE_SYNC_TIMEOUT_SECONDS due to new blocks,
                // but that should be OK and it should timeout eventually.
                if (GetTime() - nTimeLastBumped > SERVICENODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrint("servicenode", "CServiceNodeSync::ProcessTick -- nTick %d nRequestedServiceNodeAssets %d -- timeout\n", nTick, nRequestedServiceNodeAssets);
                    if (nRequestedServiceNodeAttempt == 0) {
                        LogPrintf("CServiceNodeSync::ProcessTick -- ERROR: failed to sync %s\n", GetAssetName());
                        // probably not a good idea to proceed without winner list
                        Fail();
                        connman.ReleaseNodeVector(vNodesCopy);
                        return;
                    }
                    SwitchToNextAsset(connman);
                    connman.ReleaseNodeVector(vNodesCopy);
                    return;
                }
                // check for data
                // if snpayments already has enough blocks and votes, switch to the next asset
                // try to fetch data from at least two peers though
                if (nRequestedServiceNodeAttempt > 1 && snpayments.IsEnoughData()) {
                    LogPrint("servicenode", "CServiceNodeSync::ProcessTick -- nTick %d nRequestedServiceNodeAssets %d -- found enough data\n", nTick, nRequestedServiceNodeAssets);
                    SwitchToNextAsset(connman);
                    connman.ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // request from three peers max
                if (nRequestedServiceNodeAttempt > 2) {
                    connman.ReleaseNodeVector(vNodesCopy);
                    return;
                }

                // only request once from each peer
                if (netfulfilledman.HasFulfilledRequest(pnode->addr, "servicenode-payment-sync"))
                    continue;
                netfulfilledman.AddFulfilledRequest(pnode->addr, "servicenode-payment-sync");

                if (pnode->nVersion < snpayments.GetMinServiceNodePaymentsProto())
                    continue;
                nRequestedServiceNodeAttempt++;

                // ask node for all payment votes it has (new nodes will only return votes for future payments)
                if (pnode->nVersion == 70900) {
                    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::SERVICENODEPAYMENTSYNC, snpayments.GetStorageLimit()));
                } else {
                    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::SERVICENODEPAYMENTSYNC));
                } // ask node for missing pieces only (old nodes will not be asked)
                snpayments.RequestLowDataPaymentBlocks(pnode, connman);

                connman.ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }

            // GOVOBJ : SYNC GOVERNANCE ITEMS FROM OUR PEERS

            if (nRequestedServiceNodeAssets == SERVICENODE_SYNC_GOVERNANCE) {
                LogPrint("gobject", "CServiceNodeSync::ProcessTick -- nTick %d nRequestedServiceNodeAssets %d nTimeLastBumped %lld GetTime() %lld diff %lld\n", nTick, nRequestedServiceNodeAssets, nTimeLastBumped, GetTime(), GetTime() - nTimeLastBumped);

                // check for timeout first
                if (GetTime() - nTimeLastBumped > SERVICENODE_SYNC_TIMEOUT_SECONDS) {
                    LogPrint("servicenode", "CServiceNodeSync::ProcessTick -- nTick %d nRequestedServiceNodeAssets %d -- timeout\n", nTick, nRequestedServiceNodeAssets);
                    if (nRequestedServiceNodeAttempt == 0) {
                        LogPrintf("CServiceNodeSync::ProcessTick -- WARNING: failed to sync %s\n", GetAssetName());
                        // it's kind of ok to skip this for now, hopefully we'll catch up later?
                    }
                    SwitchToNextAsset(connman);
                    connman.ReleaseNodeVector(vNodesCopy);
                    return;
                }
                // only request obj sync once from each peer, then request votes on per-obj basis
                if (netfulfilledman.HasFulfilledRequest(pnode->addr, "governance-sync")) {
                    governance.RequestGovernanceObjectVotes(pnode, connman);
                    int nObjsLeftToAsk = governance.RequestGovernanceObjectVotes(pnode, connman);
                    static int64_t nTimeNoObjectsLeft = 0;
                    // check for data
                    if (nObjsLeftToAsk == 0) {
                        static int nLastTick = 0;
                        static int nLastVotes = 0;
                        if (nTimeNoObjectsLeft == 0) {
                            // asked all objects for votes for the first time
                            nTimeNoObjectsLeft = GetTime();
                        }
                        // make sure the condition below is checked only once per tick
                        if (nLastTick == nTick)
                            continue;
                        if (GetTime() - nTimeNoObjectsLeft > SERVICENODE_SYNC_TIMEOUT_SECONDS &&
                            governance.GetVoteCount() - nLastVotes < std::max(int(0.0001 * nLastVotes), SERVICENODE_SYNC_TICK_SECONDS)) {
                            // We already asked for all objects, waited for SERVICENODE_SYNC_TIMEOUT_SECONDS
                            // after that and less then 0.01% or SERVICENODE_SYNC_TICK_SECONDS
                            // (i.e. 1 per second) votes were recieved during the last tick.
                            // We can be pretty sure that we are done syncing.
                            LogPrint("servicenode", "CServiceNodeSync::ProcessTick -- nTick %d nRequestedServiceNodeAssets %d -- asked for all objects, nothing to do\n", nTick, nRequestedServiceNodeAssets);
                            // reset nTimeNoObjectsLeft to be able to use the same condition on resync
                            nTimeNoObjectsLeft = 0;
                            SwitchToNextAsset(connman);
                            connman.ReleaseNodeVector(vNodesCopy);
                            return;
                        }
                        nLastTick = nTick;
                        nLastVotes = governance.GetVoteCount();
                    }
                    continue;
                }

                netfulfilledman.AddFulfilledRequest(pnode->addr, "governance-sync");

                if (pnode->nVersion < MIN_GOVERNANCE_PEER_PROTO_VERSION)
                    continue;
                nRequestedServiceNodeAttempt++;

                SendGovernanceSyncRequest(pnode, connman);

                connman.ReleaseNodeVector(vNodesCopy);
                return; //this will cause each peer to get one request each six seconds for the various assets we need
            }
        }
    }
    // looped through all nodes, release them
    connman.ReleaseNodeVector(vNodesCopy);
}

void CServiceNodeSync::SendGovernanceSyncRequest(CNode* pnode, CConnman& connman)
{
    CNetMsgMaker msgMaker(pnode->GetSendVersion());

    if (pnode->nVersion >= GOVERNANCE_FILTER_PROTO_VERSION) {
        CBloomFilter filter;
        filter.clear();

        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DNGOVERNANCESYNC, uint256(), filter));
    } else {
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DNGOVERNANCESYNC, uint256()));
    }
}

void CServiceNodeSync::AcceptedBlockHeader(const CBlockIndex* pindexNew)
{
    LogPrint("dnsync", "CServiceNodeSync::AcceptedBlockHeader -- pindexNew->nHeight: %d\n", pindexNew->nHeight);

    if (!IsBlockchainSynced()) {
        // Postpone timeout each time new block header arrives while we are still syncing blockchain
        BumpAssetLastTime("CServiceNodeSync::AcceptedBlockHeader");
    }
}

void CServiceNodeSync::NotifyHeaderTip(const CBlockIndex* pindexNew, bool fInitialDownload, CConnman& connman)
{
    LogPrint("dnsync", "CServiceNodeSync::NotifyHeaderTip -- pindexNew->nHeight: %d fInitialDownload=%d\n", pindexNew->nHeight, fInitialDownload);

    if (IsFailed() || IsSynced() || !pindexBestHeader)
        return;

    if (!IsBlockchainSynced()) {
        // Postpone timeout each time new block arrives while we are still syncing blockchain
        BumpAssetLastTime("CServiceNodeSync::NotifyHeaderTip");
    }
}

void CServiceNodeSync::UpdatedBlockTip(const CBlockIndex* pindexNew, bool fInitialDownload, CConnman& connman)
{
    LogPrint("dnsync", "CServiceNodeSync::UpdatedBlockTip -- pindexNew->nHeight: %d fInitialDownload=%d\n", pindexNew->nHeight, fInitialDownload);

    if (IsFailed() || IsSynced() || !pindexBestHeader)
        return;

    if (!IsBlockchainSynced()) {
        // Postpone timeout each time new block arrives while we are still syncing blockchain
        BumpAssetLastTime("CServiceNodeSync::UpdatedBlockTip");
    }

    if (fInitialDownload) {
        // switched too early
        if (IsBlockchainSynced()) {
            Reset();
        }

        // no need to check any further while still in IBD mode
        return;
    }

    // Note: since we sync headers first, it should be ok to use this
    static bool fReachedBestHeader = false;
    bool fReachedBestHeaderNew = pindexNew->GetBlockHash() == pindexBestHeader->GetBlockHash();

    if (fReachedBestHeader && !fReachedBestHeaderNew) {
        // Switching from true to false means that we previousely stuck syncing headers for some reason,
        // probably initial timeout was not enough,
        // because there is no way we can update tip not having best header
        Reset();
        fReachedBestHeader = false;
        return;
    }

    fReachedBestHeader = fReachedBestHeaderNew;

    LogPrint("dnsync", "CServiceNodeSync::NotifyHeaderTip -- pindexNew->nHeight: %d pindexBestHeader->nHeight: %d fInitialDownload=%d fReachedBestHeader=%d\n",
        pindexNew->nHeight, pindexBestHeader->nHeight, fInitialDownload, fReachedBestHeader);

    if (!IsBlockchainSynced() && fReachedBestHeader) {
        if (fLiteMode) {
            // nothing to do in lite mode, just finish the process immediately
            nRequestedServiceNodeAssets = SERVICENODE_SYNC_FINISHED;
            return;
        }
        // Reached best header while being in initial mode.
        // We must be at the tip already, let's move to the next asset.
        SwitchToNextAsset(connman);
    }
}

void CServiceNodeSync::DoMaintenance(CConnman &connman)
{
    if (ShutdownRequested()) return;
     ProcessTick(connman);
}
