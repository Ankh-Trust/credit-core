// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2017 The Dash Core Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "servicenodeman.h"

#include "activeservicenode.h"
#include "addrman.h"
#include "alert.h"
#include "clientversion.h"
#include "servicenode-payments.h"
#include "servicenode-sync.h"
#include "governance.h"
#include "init.h"
#include "messagesigner.h"
#include "netfulfilledman.h"
#include "netmessagemaker.h"
#ifdef ENABLE_WALLET
#include "privatesend-client.h"
#endif // ENABLE_WALLET
#include "script/standard.h"
#include "ui_interface.h"
#include "util.h"
#include "warnings.h"

/** ServiceNode manager */
CServiceNodeMan snodeman;

const std::string CServiceNodeMan::SERIALIZATION_VERSION_STRING = "CServiceNodeMan-Version-5";
const int CServiceNodeMan::LAST_PAID_SCAN_BLOCKS = 100;

struct CompareLastPaidBlock {
    bool operator()(const std::pair<int, const CServiceNode*>& t1,
        const std::pair<int, const CServiceNode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->outpoint < t2.second->outpoint);
    }
};

struct CompareScoreSN {
    bool operator()(const std::pair<arith_uint256, const CServiceNode*>& t1,
        const std::pair<arith_uint256, const CServiceNode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->outpoint < t2.second->outpoint);
    }
};

struct CompareByAddr

{
    bool operator()(const CServiceNode* t1,
        const CServiceNode* t2) const
    {
        return t1->addr < t2->addr;
    }
};

CServiceNodeMan::CServiceNodeMan()
    : cs(),
      mapServiceNodes(),
      mAskedUsForServiceNodeList(),
      mWeAskedForServiceNodeList(),
      mWeAskedForServiceNodeListEntry(),
      mWeAskedForVerification(),
      mSnbRecoveryRequests(),
      mSnbRecoveryGoodReplies(),
      listScheduledSnbRequestConnections(),
      fServiceNodesAdded(false),
      fServiceNodesRemoved(false),
      vecDirtyGovernanceObjectHashes(),
      nLastSentinelPingTime(0),
      mapSeenServiceNodeBroadcast(),
      mapSeenServiceNodePing(),
      nPsqCount(0)
{
}

bool CServiceNodeMan::Add(CServiceNode& sn)
{
    LOCK(cs);

    if (Has(sn.outpoint))
        return false;

    LogPrint("servicenode", "CServiceNodeMan::Add -- Adding new ServiceNode: addr=%s, %i now\n", sn.addr.ToString(), size() + 1);
    mapServiceNodes[sn.outpoint] = sn;
    fServiceNodesAdded = true;
    return true;
}

void CServiceNodeMan::AskForSN(CNode* pnode, const COutPoint& outpoint, CConnman& connman)
{
    if (!pnode)
        return;

    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    LOCK(cs);

    CService addrSquashed = Params().AllowMultiplePorts() ? (CService)pnode->addr : CService(pnode->addr, 0);
    auto it1 = mWeAskedForServiceNodeListEntry.find(outpoint);
    if (it1 != mWeAskedForServiceNodeListEntry.end()) {
        auto it2 = it1->second.find(addrSquashed);
        if (it2 != it1->second.end()) {
            if (GetTime() < it2->second) {
                // we've asked recently, should not repeat too often or we could get banned
                return;
            }
            // we asked this node for this outpoint but it's ok to ask again already
            LogPrintf("CServiceNodeMan::AskForSN -- Asking same peer %s for missing ServiceNode entry again: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
        } else {
            // we already asked for this outpoint but not this node
            LogPrintf("CServiceNodeMan::AskForSN -- Asking new peer %s for missing ServiceNode entry: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
        }
    } else {
        // we never asked any node for this outpoint
        LogPrintf("CServiceNodeMan::AskForSN -- Asking peer %s for missing ServiceNode entry for the first time: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
    }
    mWeAskedForServiceNodeListEntry[outpoint][addrSquashed] = GetTime() + PSEG_UPDATE_SECONDS;

    if (pnode->GetSendVersion() == 71000) {
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::PSEG, CTxIn(outpoint)));
    } else {
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::PSEG, outpoint));
    }
}

bool CServiceNodeMan::AllowMixing(const COutPoint& outpoint)
{
    LOCK(cs);
    CServiceNode* psn = Find(outpoint);
    if (!psn) {
        return false;
    }
    nPsqCount++;
    psn->nLastPsq = nPsqCount;
    psn->fAllowMixingTx = true;

    return true;
}

bool CServiceNodeMan::DisallowMixing(const COutPoint& outpoint)
{
    LOCK(cs);
    CServiceNode* psn = Find(outpoint);
    if (!psn) {
        return false;
    }
    psn->fAllowMixingTx = false;

    return true;
}

bool CServiceNodeMan::PoSeBan(const COutPoint& outpoint)
{
    LOCK(cs);
    CServiceNode* psn = Find(outpoint);
    if (!psn) {
        return false;
    }
    psn->PoSeBan();

    return true;
}

void CServiceNodeMan::Check()
{
    LOCK2(cs_main, cs);

    LogPrint("servicenode", "CServiceNodeMan::Check -- nLastSentinelPingTime=%d, IsSentinelPingActive()=%d\n", nLastSentinelPingTime, IsSentinelPingActive());

    for (auto& snpair : mapServiceNodes) {
        // NOTE: internally it checks only every SERVICENODE_CHECK_SECONDS seconds
        // since the last time, so expect some SNs to skip this
        snpair.second.Check();
    }
}


void CServiceNodeMan::CheckAndRemove(CConnman& connman)
{
    if (!servicenodeSync.IsServiceNodeListSynced())
        return;

    LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove\n");

    {
        // Need LOCK2 here to ensure consistent locking order because code below locks cs_main
        // in CheckSnbAndUpdateServiceNodeList()
        LOCK2(cs_main, cs);

        Check();

        // Remove spent ServiceNodes, prepare structures and make requests to reasure the state of inactive ones
        rank_pair_vec_t vecServiceNodeRanks;
        // ask for up to SNB_RECOVERY_MAX_ASK_ENTRIES ServiceNode entries at a time
        int nAskForSnbRecovery = SNB_RECOVERY_MAX_ASK_ENTRIES;
        std::map<COutPoint, CServiceNode>::iterator it = mapServiceNodes.begin();
        while (it != mapServiceNodes.end()) {
            CServiceNodeBroadcast snb = CServiceNodeBroadcast(it->second);
            uint256 hash = snb.GetHash();
            // If collateral was spent ...
            if (it->second.IsOutpointSpent()) {
                LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove -- Removing ServiceNode: %s  addr=%s  %i now\n", it->second.GetStateString(), it->second.addr.ToString(), size() - 1);
                // erase all of the broadcasts we've seen from this txin, ...
                mapSeenServiceNodeBroadcast.erase(hash);
                mWeAskedForServiceNodeListEntry.erase(it->first);
                // and finally remove it from the list
                it->second.FlagGovernanceItemsAsDirty();
                mapServiceNodes.erase(it++);
                fServiceNodesRemoved = true;
            } else {
                bool fAsk = (nAskForSnbRecovery > 0) &&
                            servicenodeSync.IsSynced() &&
                            it->second.IsNewStartRequired() &&
                            !IsSnbRecoveryRequested(hash) &&
                            !IsArgSet("-connect");
                if (fAsk) {
                    // this SN is in a non-recoverable state and we haven't asked other nodes yet
                    std::set<CService> setRequested;
                    // calulate only once and only when it's needed
                    if (vecServiceNodeRanks.empty()) {
                        int nRandomBlockHeight = GetRandInt(nCachedBlockHeight);
                        GetServiceNodeRanks(vecServiceNodeRanks, nRandomBlockHeight);
                    }
                    bool fAskedForSnbRecovery = false;
                    // ask first SNB_RECOVERY_QUORUM_TOTAL ServiceNodes we can connect to and we haven't asked recently
                    for (int i = 0; setRequested.size() < SNB_RECOVERY_QUORUM_TOTAL && i < (int)vecServiceNodeRanks.size(); i++) {
                        // avoid banning
                        if (mWeAskedForServiceNodeListEntry.count(it->first) && mWeAskedForServiceNodeListEntry[it->first].count(vecServiceNodeRanks[i].second.addr))
                            continue;
                        // didn't ask recently, ok to ask now
                        CService addr = vecServiceNodeRanks[i].second.addr;
                        setRequested.insert(addr);
                        listScheduledSnbRequestConnections.push_back(std::make_pair(addr, hash));
                        fAskedForSnbRecovery = true;
                    }
                    if (fAskedForSnbRecovery) {
                        LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove -- Recovery initiated, servicenode=%s\n", it->first.ToStringShort());
                        nAskForSnbRecovery--;
                    }
                    // wait for snb recovery replies for SNB_RECOVERY_WAIT_SECONDS seconds
                    mSnbRecoveryRequests[hash] = std::make_pair(GetTime() + SNB_RECOVERY_WAIT_SECONDS, setRequested);
                }
                ++it;
            }
        }
        // proces replies for SERVICENODE_NEW_START_REQUIRED ServiceNodes
        LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove -- mSnbRecoveryGoodReplies size=%d\n", (int)mSnbRecoveryGoodReplies.size());
        std::map<uint256, std::vector<CServiceNodeBroadcast> >::iterator itSnbReplies = mSnbRecoveryGoodReplies.begin();
        while (itSnbReplies != mSnbRecoveryGoodReplies.end()) {
            if (mSnbRecoveryRequests[itSnbReplies->first].first < GetTime()) {
                // all nodes we asked should have replied now
                if (itSnbReplies->second.size() >= SNB_RECOVERY_QUORUM_REQUIRED) {
                    // majority of nodes we asked agrees that this SN doesn't require new snb, reprocess one of new snbs
                    LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove -- reprocessing snb, ServiceNode=%s\n", itSnbReplies->second[0].outpoint.ToStringShort());
                    // mapSeenServiceNodeBroadcast.erase(itSnbReplies->first);
                    int nDos;
                    itSnbReplies->second[0].fRecovery = true;
                    CheckSnbAndUpdateServiceNodeList(nullptr, itSnbReplies->second[0], nDos, connman);
                }
                LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove -- removing snb recovery reply, ServiceNode=%s, size=%d\n", itSnbReplies->second[0].outpoint.ToStringShort(), (int)itSnbReplies->second.size());
                mSnbRecoveryGoodReplies.erase(itSnbReplies++);
            } else {
                ++itSnbReplies;
            }
        }
    }
    {
        // no need for cm_main below
        LOCK(cs);

        auto itSnbRequest = mSnbRecoveryRequests.begin();
        while (itSnbRequest != mSnbRecoveryRequests.end()) {
            // Allow this snb to be re-verified again after SNB_RECOVERY_RETRY_SECONDS seconds
            // if SN is still in SERVICENODE_NEW_START_REQUIRED state.
            if (GetTime() - itSnbRequest->second.first > SNB_RECOVERY_RETRY_SECONDS) {
                mSnbRecoveryRequests.erase(itSnbRequest++);
            } else {
                ++itSnbRequest;
            }
        }

        // check who's asked for the ServiceNode list
        auto it1 = mAskedUsForServiceNodeList.begin();
        while (it1 != mAskedUsForServiceNodeList.end()) {
            if ((*it1).second < GetTime()) {
                mAskedUsForServiceNodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check who we asked for the ServiceNode list
        it1 = mWeAskedForServiceNodeList.begin();
        while (it1 != mWeAskedForServiceNodeList.end()) {
            if ((*it1).second < GetTime()) {
                mWeAskedForServiceNodeList.erase(it1++);
            } else {
                ++it1;
            }
        }

        // check which ServiceNodes we've asked for
        auto it2 = mWeAskedForServiceNodeListEntry.begin();
        while (it2 != mWeAskedForServiceNodeListEntry.end()) {
            auto it3 = it2->second.begin();
            while (it3 != it2->second.end()) {
                if (it3->second < GetTime()) {
                    it2->second.erase(it3++);
                } else {
                    ++it3;
                }
            }
            if (it2->second.empty()) {
                mWeAskedForServiceNodeListEntry.erase(it2++);
            } else {
                ++it2;
            }
        }

        auto it3 = mWeAskedForVerification.begin();
        while (it3 != mWeAskedForVerification.end()) {
            if (it3->second.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS) {
                mWeAskedForVerification.erase(it3++);
            } else {
                ++it3;
            }
        }

        // NOTE: do not expire mapSeenServiceNodeBroadcast entries here, clean them on snb updates!

        // remove expired mapSeenServiceNodePing
        std::map<uint256, CServiceNodePing>::iterator it4 = mapSeenServiceNodePing.begin();
        while (it4 != mapSeenServiceNodePing.end()) {
            if ((*it4).second.IsExpired()) {
                LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove -- Removing expired ServiceNode ping: hash=%s\n", (*it4).second.GetHash().ToString());
                mapSeenServiceNodePing.erase(it4++);
            } else {
                ++it4;
            }
        }

        // remove expired mapSeenServiceNodeVerification
        std::map<uint256, CServiceNodeVerification>::iterator itv2 = mapSeenServiceNodeVerification.begin();
        while (itv2 != mapSeenServiceNodeVerification.end()) {
            if ((*itv2).second.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS) {
                LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove -- Removing expired ServiceNode verification: hash=%s\n", (*itv2).first.ToString());
                mapSeenServiceNodeVerification.erase(itv2++);
            } else {
                ++itv2;
            }
        }

        LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove -- %s\n", ToString());
    }

    if (fServiceNodesRemoved) {
        NotifyServiceNodeUpdates(connman);
    }
}

void CServiceNodeMan::Clear()
{
    LOCK(cs);
    mapServiceNodes.clear();
    mAskedUsForServiceNodeList.clear();
    mWeAskedForServiceNodeList.clear();
    mWeAskedForServiceNodeListEntry.clear();
    mapSeenServiceNodeBroadcast.clear();
    mapSeenServiceNodePing.clear();
    nPsqCount = 0;
    nLastSentinelPingTime = 0;
}

int CServiceNodeMan::CountServiceNodes(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? snpayments.GetMinServiceNodePaymentsProto() : nProtocolVersion;

    for (auto& snpair : mapServiceNodes) {
        if (snpair.second.nProtocolVersion < nProtocolVersion)
            continue;
        nCount++;
    }

    return nCount;
}

int CServiceNodeMan::CountEnabled(int nProtocolVersion)
{
    LOCK(cs);
    int nCount = 0;
    nProtocolVersion = nProtocolVersion == -1 ? snpayments.GetMinServiceNodePaymentsProto() : nProtocolVersion;

    for (auto& snpair : mapServiceNodes) {
        if (snpair.second.nProtocolVersion < nProtocolVersion || !snpair.second.IsEnabled())
            continue;
        nCount++;
    }

    return nCount;
}

/* Only IPv4 ServiceNodes are allowed, saving this for later
int CServiceNodeMan::CountByIP(int nNetworkType)
{
    LOCK(cs);
    int nNodeCount = 0;

    for (auto& snpair : mapServiceNodes)
        if ((nNetworkType == NET_IPV4 && snpair.second.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && snpair.second.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && snpair.second.addr.IsIPv6())) {
                nNodeCount++;
        }

    return nNodeCount;
}
*/

void CServiceNodeMan::PsegUpdate(CNode* pnode, CConnman& connman)
{
    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    LOCK(cs);

    CService addrSquashed = Params().AllowMultiplePorts() ? (CService)pnode->addr : CService(pnode->addr, 0);
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            auto it = mWeAskedForServiceNodeList.find(addrSquashed);
            if (it != mWeAskedForServiceNodeList.end() && GetTime() < (*it).second) {
                LogPrintf("CServiceNodeMan::PsegUpdate -- we already asked %s for the list; skipping...\n", addrSquashed.ToString());
                return;
            }
        }
    }

    if (pnode->GetSendVersion() == 71000) {
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::PSEG, CTxIn()));
    } else {
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::PSEG, COutPoint()));
    }
    int64_t askAgain = GetTime() + PSEG_UPDATE_SECONDS;
    mWeAskedForServiceNodeList[pnode->addr] = askAgain;

    LogPrint("servicenode", "CServiceNodeMan::PsegUpdate -- asked %s for the list\n", pnode->addr.ToString());
}

CServiceNode* CServiceNodeMan::Find(const COutPoint& outpoint)
{
    LOCK(cs);
    auto it = mapServiceNodes.find(outpoint);
    return it == mapServiceNodes.end() ? nullptr : &(it->second);
}

bool CServiceNodeMan::Get(const COutPoint& outpoint, CServiceNode& servicenodeRet)
{
    // Theses mutexes are recursive so double locking by the same thread is safe.
    LOCK(cs);
    auto it = mapServiceNodes.find(outpoint);
    if (it == mapServiceNodes.end()) {
        return false;
    }

    servicenodeRet = it->second;
    return true;
}

bool CServiceNodeMan::GetServiceNodeInfo(const COutPoint& outpoint, servicenode_info_t& snInfoRet)
{
    LOCK(cs);
    auto it = mapServiceNodes.find(outpoint);
    if (it == mapServiceNodes.end()) {
        return false;
    }
    snInfoRet = it->second.GetInfo();
    return true;
}

bool CServiceNodeMan::GetServiceNodeInfo(const CPubKey& pubKeyServiceNode, servicenode_info_t& snInfoRet)
{
    LOCK(cs);
    for (auto& snpair : mapServiceNodes) {
        if (snpair.second.pubKeyServiceNode == pubKeyServiceNode) {
            snInfoRet = snpair.second.GetInfo();
            return true;
        }
    }
    return false;
}

bool CServiceNodeMan::GetServiceNodeInfo(const CScript& payee, servicenode_info_t& snInfoRet)
{
    LOCK(cs);
    for (const auto& snpair : mapServiceNodes) {
        CScript scriptCollateralAddress = GetScriptForDestination(snpair.second.pubKeyCollateralAddress.GetID());
        if (scriptCollateralAddress == payee) {
            snInfoRet = snpair.second.GetInfo();
            return true;
        }
    }
    return false;
}

bool CServiceNodeMan::Has(const COutPoint& outpoint)
{
    LOCK(cs);
    return mapServiceNodes.find(outpoint) != mapServiceNodes.end();
}

//
// Deterministically select the oldest/best ServiceNode to pay on the network
//
bool CServiceNodeMan::GetNextServiceNodeInQueueForPayment(bool fFilterSigTime, int& nCountRet, servicenode_info_t& snInfoRet)
{
    return GetNextServiceNodeInQueueForPayment(nCachedBlockHeight, fFilterSigTime, nCountRet, snInfoRet);
}

bool CServiceNodeMan::GetNextServiceNodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCountRet, servicenode_info_t& snInfoRet)
{
    snInfoRet = servicenode_info_t();
    nCountRet = 0;

    if (!servicenodeSync.IsWinnersListSynced()) {
        // without winner list we can't reliably find the next winner anyway
        return false;
    }

    // Need LOCK2 here to ensure consistent locking order because the GetBlockHash call below locks cs_main
    LOCK2(cs_main, cs);

    std::vector<std::pair<int, const CServiceNode*> > vecServiceNodeLastPaid;

    /*
        Make a vector with all of the last paid times
    */

    int nSnCount = CountServiceNodes();

    for (const auto& snpair : mapServiceNodes) {
        if (!snpair.second.IsValidForPayment())
            continue;

        // //check protocol version
        if (snpair.second.nProtocolVersion < snpayments.GetMinServiceNodePaymentsProto())
            continue;

        //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        if (snpayments.IsScheduled(snpair.second, nBlockHeight))
            continue;

        //it's too new, wait for a cycle
        if (fFilterSigTime && snpair.second.sigTime + (nSnCount * 2.6 * 60) > GetAdjustedTime())
            continue;

        //make sure it has at least as many confirmations as there are ServiceNodes
        if (GetUTXOConfirmations(snpair.first) < nSnCount)
            continue;

        vecServiceNodeLastPaid.push_back(std::make_pair(snpair.second.GetLastPaidBlock(), &snpair.second));
    }

    nCountRet = (int)vecServiceNodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if (fFilterSigTime && nCountRet < nSnCount / 3)
        return GetNextServiceNodeInQueueForPayment(nBlockHeight, false, nCountRet, snInfoRet);

    // Sort them low to high
    sort(vecServiceNodeLastPaid.begin(), vecServiceNodeLastPaid.end(), CompareLastPaidBlock());

    uint256 blockHash;
    if (!GetBlockHash(blockHash, nBlockHeight - 101)) {
        LogPrintf("CServiceNode::GetNextServiceNodeInQueueForPayment -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", nBlockHeight - 101);
        return false;
    }
    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the +8-10 blocks, allowing for double payments very rarely
    //  -- 1/100 payments should be a double payment on mainnet - (1/(3000/10))*2
    //  -- (chance per block * chances before IsScheduled will fire)
    int nTenthNetwork = nSnCount / 10;
    int nCountTenth = 0;
    arith_uint256 nHighest = 0;
    const CServiceNode* pBestServiceNode = nullptr;
    for (const auto& s : vecServiceNodeLastPaid) {
        arith_uint256 nScore = s.second->CalculateScore(blockHash);
        if (nScore > nHighest) {
            nHighest = nScore;
            pBestServiceNode = s.second;
        }
        nCountTenth++;
        if (nCountTenth >= nTenthNetwork)
            break;
    }
    if (pBestServiceNode) {
        snInfoRet = pBestServiceNode->GetInfo();
    }
    return snInfoRet.fInfoValid;
}

servicenode_info_t CServiceNodeMan::FindRandomNotInVec(const std::vector<COutPoint>& vecToExclude, int nProtocolVersion)
{
    LOCK(cs);

    nProtocolVersion = nProtocolVersion == -1 ? snpayments.GetMinServiceNodePaymentsProto() : nProtocolVersion;

    int nCountEnabled = CountEnabled(nProtocolVersion);
    int nCountNotExcluded = nCountEnabled - vecToExclude.size();

    LogPrintf("CServiceNodeMan::FindRandomNotInVec -- %d enabled ServiceNodes, %d ServiceNodes to choose from\n", nCountEnabled, nCountNotExcluded);
    if (nCountNotExcluded < 1)
        return servicenode_info_t();

    // fill a vector of pointers
    std::vector<const CServiceNode*> vpServiceNodesShuffled;
    for (const auto& snpair : mapServiceNodes) {
        vpServiceNodesShuffled.push_back(&snpair.second);
    }

    FastRandomContext insecure_rand;
    // shuffle pointers
    std::random_shuffle(vpServiceNodesShuffled.begin(), vpServiceNodesShuffled.end(), insecure_rand);
    bool fExclude;

    // loop through
    for (const auto& psn : vpServiceNodesShuffled) {
        if (psn->nProtocolVersion < nProtocolVersion || !psn->IsEnabled())
            continue;
        fExclude = false;
        for (const auto& outpointToExclude : vecToExclude) {
            if (psn->outpoint == outpointToExclude) {
                fExclude = true;
                break;
            }
        }
        if (fExclude)
            continue;
        // found the one not in vecToExclude
        LogPrint("servicenode", "CServiceNodeMan::FindRandomNotInVec -- found, ServiceNode=%s\n", psn->outpoint.ToStringShort());
        return psn->GetInfo();
    }

    LogPrint("servicenode", "CServiceNodeMan::FindRandomNotInVec -- failed\n");
    return servicenode_info_t();
}

bool CServiceNodeMan::GetServiceNodeScores(const uint256& nBlockHash, CServiceNodeMan::score_pair_vec_t& vecServiceNodeScoresRet, int nMinProtocol)
{
    vecServiceNodeScoresRet.clear();

    if (!servicenodeSync.IsServiceNodeListSynced())
        return false;

    AssertLockHeld(cs);

    if (mapServiceNodes.empty())
        return false;

    // calculate scores
    for (const auto& snpair : mapServiceNodes) {
        if (snpair.second.nProtocolVersion >= nMinProtocol) {
            vecServiceNodeScoresRet.push_back(std::make_pair(snpair.second.CalculateScore(nBlockHash), &snpair.second));
        }
    }

    sort(vecServiceNodeScoresRet.rbegin(), vecServiceNodeScoresRet.rend(), CompareScoreSN());
    return !vecServiceNodeScoresRet.empty();
}

bool CServiceNodeMan::GetServiceNodeRank(const COutPoint& outpoint, int& nRankRet, int nBlockHeight, int nMinProtocol)
{
    nRankRet = -1;

    if (!servicenodeSync.IsServiceNodeListSynced())
        return false;

    // make sure we know about this block
    uint256 nBlockHash = uint256();
    if (!GetBlockHash(nBlockHash, nBlockHeight)) {
        LogPrintf("CServiceNodeMan::%s -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", __func__, nBlockHeight);
        return false;
    }

    LOCK(cs);

    score_pair_vec_t vecServiceNodeScores;
    if (!GetServiceNodeScores(nBlockHash, vecServiceNodeScores, nMinProtocol))
        return false;

    int nRank = 0;
    for (const auto& scorePair : vecServiceNodeScores) {
        nRank++;
        if (scorePair.second->outpoint == outpoint) {
            nRankRet = nRank;
            return true;
        }
    }

    return false;
}

bool CServiceNodeMan::GetServiceNodeRanks(CServiceNodeMan::rank_pair_vec_t& vecServiceNodeRanksRet, int nBlockHeight, int nMinProtocol)
{
    vecServiceNodeRanksRet.clear();

    if (!servicenodeSync.IsServiceNodeListSynced())
        return false;

    // make sure we know about this block
    uint256 nBlockHash = uint256();
    if (!GetBlockHash(nBlockHash, nBlockHeight)) {
        LogPrintf("CServiceNodeMan::%s -- ERROR: GetBlockHash() failed at nBlockHeight %d\n", __func__, nBlockHeight);
        return false;
    }

    LOCK(cs);

    score_pair_vec_t vecServiceNodeScores;
    if (!GetServiceNodeScores(nBlockHash, vecServiceNodeScores, nMinProtocol))
        return false;

    int nRank = 0;
    for (const auto& scorePair : vecServiceNodeScores) {
        nRank++;
        vecServiceNodeRanksRet.push_back(std::make_pair(nRank, *scorePair.second));
    }

    return true;
}

void CServiceNodeMan::ProcessServiceNodeConnections(CConnman& connman)
{
    //we don't care about this for regtest
    if (Params().NetworkIDString() == CBaseChainParams::REGTEST)
        return;

    std::vector<servicenode_info_t> vecSnInfo; // will be empty when no wallet
#ifdef ENABLE_WALLET
    privateSendClient.GetMixingServiceNodesInfo(vecSnInfo);
#endif // ENABLE_WALLET

    connman.ForEachNode(CConnman::AllNodes, [&vecSnInfo](CNode* pnode) {
        if (pnode->fServiceNode) {
#ifdef ENABLE_WALLET
            bool fFound = false;
            for (const auto& snInfo : vecSnInfo) {
                if (pnode->addr == snInfo.addr) {
                    fFound = true;
                    break;
                }
            }
            if (fFound)
                return; // do NOT disconnect mixing servicenodes
#endif                  // ENABLE_WALLET
            LogPrintf("Closing ServiceNode connection: peer=%d, addr=%s\n", pnode->id, pnode->addr.ToString());
            pnode->fDisconnect = true;
        }
    });
}


std::pair<CService, std::set<uint256> > CServiceNodeMan::PopScheduledSnbRequestConnection()
{
    LOCK(cs);
    if (listScheduledSnbRequestConnections.empty()) {
        return std::make_pair(CService(), std::set<uint256>());
    }

    std::set<uint256> setResult;

    listScheduledSnbRequestConnections.sort();
    std::pair<CService, uint256> pairFront = listScheduledSnbRequestConnections.front();

    // squash hashes from requests with the same CService as the first one into setResult
    std::list<std::pair<CService, uint256> >::iterator it = listScheduledSnbRequestConnections.begin();
    while (it != listScheduledSnbRequestConnections.end()) {
        if (pairFront.first == it->first) {
            setResult.insert(it->second);
            it = listScheduledSnbRequestConnections.erase(it);
        } else {
            // since list is sorted now, we can be sure that there is no more hashes left
            // to ask for from this addr
            break;
        }
    }
    return std::make_pair(pairFront.first, setResult);
}

void CServiceNodeMan::ProcessPendingSnbRequests(CConnman& connman)
{
    std::pair<CService, std::set<uint256> > p = PopScheduledSnbRequestConnection();
    if (!(p.first == CService() || p.second.empty())) {
        if (connman.IsServiceNodeOrDisconnectRequested(p.first))
            return;
        mapPendingSNB.insert(std::make_pair(p.first, std::make_pair(GetTime(), p.second)));
        connman.AddPendingServiceNode(p.first);
    }

    std::map<CService, std::pair<int64_t, std::set<uint256> > >::iterator itPendingSNB = mapPendingSNB.begin();
    while (itPendingSNB != mapPendingSNB.end()) {
        bool fDone = connman.ForNode(itPendingSNB->first, [&](CNode* pnode) {
            // compile request vector
            std::vector<CInv> vToFetch;
            for (auto& nHash : itPendingSNB->second.second) {
                if (nHash != uint256()) {
                    vToFetch.push_back(CInv(MSG_SERVICENODE_ANNOUNCE, nHash));
                    LogPrint("servicenode", "-- asking for snb %s from addr=%s\n", nHash.ToString(), pnode->addr.ToString());
                }
            }

            // ask for data
            CNetMsgMaker msgMaker(pnode->GetSendVersion());
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::GETDATA, vToFetch));
            return true;
        });

        int64_t nTimeAdded = itPendingSNB->second.first;
        if (fDone || (GetTime() - nTimeAdded > 15)) {
            if (!fDone) {
                LogPrint("servicenode", "CServiceNodeMan::%s -- failed to connect to %s\n", __func__, itPendingSNB->first.ToString());
            }
            mapPendingSNB.erase(itPendingSNB++);
        } else {
            ++itPendingSNB;
        }
    }
}

void CServiceNodeMan::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if (fLiteMode)
        return; // disable all Credit specific functionality

    if (strCommand == NetMsgType::SNANNOUNCE) { //ServiceNode Broadcast

        CServiceNodeBroadcast snb;
        vRecv >> snb;

        pfrom->setAskFor.erase(snb.GetHash());

        if (!servicenodeSync.IsBlockchainSynced())
            return;

        LogPrint("servicenode", "SNANNOUNCE -- ServiceNode announce, ServiceNode=%s\n", snb.outpoint.ToStringShort());

        int nDos = 0;

        if (CheckSnbAndUpdateServiceNodeList(pfrom, snb, nDos, connman)) {
            // use announced ServiceNode as a peer
            connman.AddNewAddress(CAddress(snb.addr, NODE_NETWORK), pfrom->addr, 2 * 60 * 60);
        } else if (nDos > 0) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), nDos);
        }

        if (fServiceNodesAdded) {
            NotifyServiceNodeUpdates(connman);
        }
    } else if (strCommand == NetMsgType::SNPING) { //ServiceNode Ping

        CServiceNodePing snp;
        vRecv >> snp;

        uint256 nHash = snp.GetHash();

        pfrom->setAskFor.erase(nHash);

        if (!servicenodeSync.IsBlockchainSynced())
            return;

        LogPrint("servicenode", "SNPING -- ServiceNode ping, ServiceNode=%s\n", snp.servicenodeOutpoint.ToStringShort());

        // Need LOCK2 here to ensure consistent locking order because the CheckAndUpdate call below locks cs_main
        LOCK2(cs_main, cs);

        if (mapSeenServiceNodePing.count(nHash))
            return; //seen
        mapSeenServiceNodePing.insert(std::make_pair(nHash, snp));

        LogPrint("servicenode", "SNPING -- ServiceNode ping, ServiceNode=%s new\n", snp.servicenodeOutpoint.ToStringShort());

        // see if we have this ServiceNode
        CServiceNode* psn = Find(snp.servicenodeOutpoint);

        if (psn && snp.fSentinelIsCurrent)
            UpdateLastSentinelPingTime();

        // too late, new SNANNOUNCE is required
        if (psn && psn->IsNewStartRequired())
            return;

        int nDos = 0;
        if (snp.CheckAndUpdate(psn, false, nDos, connman))
            return;

        if (nDos > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDos);
        } else if (psn != NULL) {
            // nothing significant failed, sn is a known one too
            return;
        }

        // something significant is broken or sn is unknown,
        // we might have to ask for a ServiceNode entry once
        AskForSN(pfrom, snp.servicenodeOutpoint, connman);

    } else if (strCommand == NetMsgType::PSEG) { //Get ServiceNode list or specific entry
        // Ignore such requests until we are fully synced.
        // We could start processing this after ServiceNode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!servicenodeSync.IsSynced())
            return;

        COutPoint servicenodeOutpoint;

        if (pfrom->nVersion == 71000) {
            CTxIn vin;
            vRecv >> vin;
            servicenodeOutpoint = vin.prevout;
        } else {
            vRecv >> servicenodeOutpoint;
        }

        LogPrint("servicenode", "PSEG -- ServiceNode list, ServiceNode=%s\n", servicenodeOutpoint.ToStringShort());

        if (servicenodeOutpoint.IsNull()) {
            SyncAll(pfrom, connman);
        } else {
            SyncSingle(pfrom, servicenodeOutpoint, connman);
        }

    } else if (strCommand == NetMsgType::SNVERIFY) { // ServiceNode Verify

        // Need LOCK2 here to ensure consistent locking order because the all functions below call GetBlockHash which locks cs_main
        LOCK2(cs_main, cs);

        CServiceNodeVerification snv;
        vRecv >> snv;

        pfrom->setAskFor.erase(snv.GetHash());

        if (!servicenodeSync.IsServiceNodeListSynced())
            return;

        if (snv.vchSig1.empty()) {
            // CASE 1: someone asked me to verify myself /IP we are using/
            SendVerifyReply(pfrom, snv, connman);
        } else if (snv.vchSig2.empty()) {
            // CASE 2: we _probably_ got verification we requested from some ServiceNode
            ProcessVerifyReply(pfrom, snv);
        } else {
            // CASE 3: we _probably_ got verification broadcast signed by some ServiceNode which verified another one
            ProcessVerifyBroadcast(pfrom, snv);
        }
    }
}

void CServiceNodeMan::SyncSingle(CNode* pnode, const COutPoint& outpoint, CConnman& connman)
{
    // do not provide any data until our node is synced
    if (!servicenodeSync.IsSynced())
        return;

    LOCK(cs);

    auto it = mapServiceNodes.find(outpoint);

    if (it != mapServiceNodes.end()) {
        if (it->second.addr.IsRFC1918() || it->second.addr.IsLocal())
            return; // do not send local network ServiceNode
        // NOTE: send ServiceNode regardless of its current state, the other node will need it to verify old votes.
        LogPrint("servicenode", "CServiceNodeMan::%s -- Sending ServiceNode entry: ServiceNode=%s  addr=%s\n", __func__, outpoint.ToStringShort(), it->second.addr.ToString());
        PushPsegInvs(pnode, it->second);
        LogPrintf("CServiceNodeMan::%s -- Sent 1 ServiceNode inv to peer=%d\n", __func__, pnode->id);
    }
}

void CServiceNodeMan::SyncAll(CNode* pnode, CConnman& connman)
{
    // do not provide any data until our node is synced
    if (!servicenodeSync.IsSynced())
        return;

    // local network
    bool isLocal = (pnode->addr.IsRFC1918() || pnode->addr.IsLocal());

    CService addrSquashed = Params().AllowMultiplePorts() ? (CService)pnode->addr : CService(pnode->addr, 0);
    // should only ask for this once
    if (!isLocal && Params().NetworkIDString() == CBaseChainParams::MAIN) {
        LOCK2(cs_main, cs);
        auto it = mAskedUsForServiceNodeList.find(addrSquashed);
        if (it != mAskedUsForServiceNodeList.end() && it->second > GetTime()) {
            Misbehaving(pnode->GetId(), 34);
            LogPrintf("CServiceNodeMan::%s -- peer already asked me for the list, peer=%d\n", __func__, pnode->id);
            return;
        }
        int64_t askAgain = GetTime() + PSEG_UPDATE_SECONDS;
        mAskedUsForServiceNodeList[addrSquashed] = askAgain;
    }

    int nInvCount = 0;

    LOCK(cs);

    for (const auto& snpair : mapServiceNodes) {
        if (Params().RequireRoutableExternalIP() &&
            (snpair.second.addr.IsRFC1918() || snpair.second.addr.IsLocal()))
            continue; // do not send local network ServiceNode
        // NOTE: send ServiceNode regardless of its current state, the other node will need it to verify old votes.
        LogPrint("servicenode", "CServiceNodeMan::%s -- Sending ServiceNode entry: ServiceNode=%s  addr=%s\n", __func__, snpair.first.ToStringShort(), snpair.second.addr.ToString());
        PushPsegInvs(pnode, snpair.second);
        nInvCount++;
    }

    connman.PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::SYNCSTATUSCOUNT, SERVICENODE_SYNC_LIST, nInvCount));
    LogPrintf("CServiceNodeMan::%s -- Sent %d ServiceNode invs to peer=%d\n", __func__, nInvCount, pnode->id);
}

void CServiceNodeMan::PushPsegInvs(CNode* pnode, const CServiceNode& sn)
{
    AssertLockHeld(cs);

    CServiceNodeBroadcast snb(sn);
    CServiceNodePing snp = snb.lastPing;
    uint256 hashSNB = snb.GetHash();
    uint256 hashSNP = snp.GetHash();
    pnode->PushInventory(CInv(MSG_SERVICENODE_ANNOUNCE, hashSNB));
    pnode->PushInventory(CInv(MSG_SERVICENODE_PING, hashSNP));
    mapSeenServiceNodeBroadcast.insert(std::make_pair(hashSNB, std::make_pair(GetTime(), snb)));
    mapSeenServiceNodePing.insert(std::make_pair(hashSNP, snp));
}

// Verification of ServiceNode via unique direct requests.

void CServiceNodeMan::DoFullVerificationStep(CConnman& connman)
{
    if (activeServiceNode.outpoint == COutPoint())
        return;
    if (!servicenodeSync.IsSynced())
        return;

    rank_pair_vec_t vecServiceNodeRanks;
    GetServiceNodeRanks(vecServiceNodeRanks, nCachedBlockHeight - 1, MIN_POSE_PROTO_VERSION);

    LOCK(cs);

    int nCount = 0;

    int nMyRank = -1;
    int nRanksTotal = (int)vecServiceNodeRanks.size();

    // send verify requests only if we are in top MAX_POSE_RANK
    for (auto& rankPair : vecServiceNodeRanks) {
        if (rankPair.first > MAX_POSE_RANK) {
            LogPrint("servicenode", "CServiceNodeMan::DoFullVerificationStep -- Must be in top %d to send verify request\n",
                (int)MAX_POSE_RANK);
            return;
        }
        if (rankPair.second.outpoint == activeServiceNode.outpoint) {
            nMyRank = rankPair.first;
            LogPrint("servicenode", "CServiceNodeMan::DoFullVerificationStep -- Found self at rank %d/%d, verifying up to %d ServiceNodes\n",
                nMyRank, nRanksTotal, (int)MAX_POSE_CONNECTIONS);
            break;
        }
    }

    // edge case: list is too short and this ServiceNode is not enabled
    if (nMyRank == -1)
        return;

    // send verify requests to up to MAX_POSE_CONNECTIONS ServiceNodes
    // starting from MAX_POSE_RANK + nMyRank and using MAX_POSE_CONNECTIONS as a step
    int nOffset = MAX_POSE_RANK + nMyRank - 1;
    if (nOffset >= (int)vecServiceNodeRanks.size())
        return;

    std::vector<const CServiceNode*> vSortedByAddr;
    for (const auto& snpair : mapServiceNodes) {
        vSortedByAddr.push_back(&snpair.second);
    }

    sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

    auto it = vecServiceNodeRanks.begin() + nOffset;
    while (it != vecServiceNodeRanks.end()) {
        if (it->second.IsPoSeVerified() || it->second.IsPoSeBanned()) {
            LogPrint("servicenode", "CServiceNodeMan::DoFullVerificationStep -- Already %s%s%s ServiceNode %s address %s, skipping...\n",
                it->second.IsPoSeVerified() ? "verified" : "",
                it->second.IsPoSeVerified() && it->second.IsPoSeBanned() ? " and " : "",
                it->second.IsPoSeBanned() ? "banned" : "",
                it->second.outpoint.ToStringShort(), it->second.addr.ToString());
            nOffset += MAX_POSE_CONNECTIONS;
            if (nOffset >= (int)vecServiceNodeRanks.size())
                break;
            it += MAX_POSE_CONNECTIONS;
            continue;
        }
        LogPrint("servicenode", "CServiceNodeMan::DoFullVerificationStep -- Verifying ServiceNode %s rank %d/%d address %s\n",
            it->second.outpoint.ToStringShort(), it->first, nRanksTotal, it->second.addr.ToString());
        if (SendVerifyRequest(CAddress(it->second.addr, NODE_NETWORK), vSortedByAddr, connman)) {
            nCount++;
            if (nCount >= MAX_POSE_CONNECTIONS)
                break;
        }
        nOffset += MAX_POSE_CONNECTIONS;
        if (nOffset >= (int)vecServiceNodeRanks.size())
            break;
        it += MAX_POSE_CONNECTIONS;
    }

    LogPrint("servicenode", "CServiceNodeMan::DoFullVerificationStep -- Sent verification requests to %d ServiceNodes\n", nCount);
}

// This function tries to find ServiceNodes with the same addr,
// find a verified one and ban all the other. If there are many nodes
// with the same addr but none of them is verified yet, then none of them are banned.
// It could take many times to run this before most of the duplicate nodes are banned.

void CServiceNodeMan::CheckSameAddr()
{
    if (!servicenodeSync.IsSynced() || mapServiceNodes.empty())
        return;

    std::vector<CServiceNode*> vBan;
    std::vector<CServiceNode*> vSortedByAddr;

    {
        LOCK(cs);

        CServiceNode* pprevServiceNode = nullptr;
        CServiceNode* pverifiedServiceNode = nullptr;

        for (auto& snpair : mapServiceNodes) {
            vSortedByAddr.push_back(&snpair.second);
        }

        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

        for (const auto& psn : vSortedByAddr) {
            // check only (pre)enabled ServiceNodes
            if (!psn->IsEnabled() && !psn->IsPreEnabled())
                continue;
            // initial step
            if (!pprevServiceNode) {
                pprevServiceNode = psn;
                pverifiedServiceNode = psn->IsPoSeVerified() ? psn : nullptr;
                continue;
            }
            // second+ step
            if (psn->addr == pprevServiceNode->addr) {
                if (pverifiedServiceNode) {
                    // another ServiceNode with the same ip is verified, ban this one
                    vBan.push_back(psn);
                } else if (psn->IsPoSeVerified()) {
                    // this ServiceNode with the same ip is verified, ban previous one
                    vBan.push_back(pprevServiceNode);
                    // and keep a reference to be able to ban following ServiceNodes with the same ip
                    pverifiedServiceNode = psn;
                }
            } else {
                pverifiedServiceNode = psn->IsPoSeVerified() ? psn : nullptr;
            }
            pprevServiceNode = psn;
        }
    }

    // ban duplicates
    for (auto& psn : vBan) {
        LogPrintf("CServiceNodeMan::CheckSameAddr -- increasing PoSe ban score for ServiceNode %s\n", psn->outpoint.ToStringShort());
        psn->IncreasePoSeBanScore();
    }
}

bool CServiceNodeMan::SendVerifyRequest(const CAddress& addr, const std::vector<const CServiceNode*>& vSortedByAddr, CConnman& connman)
{
    if (netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::SNVERIFY) + "-request")) {
        // we already asked for verification, not a good idea to do this too often, skip it
        LogPrint("servicenode", "CServiceNodeMan::SendVerifyRequest -- too many requests, skipping... addr=%s\n", addr.ToString());
        return false;
    }

    if (connman.IsServiceNodeOrDisconnectRequested(addr))
        return false;

    connman.AddPendingServiceNode(addr);
    // use random nonce, store it and require node to reply with correct one later
    CServiceNodeVerification snv(addr, GetRandInt(999999), nCachedBlockHeight - 1);
    LOCK(cs_mapPendingSNV);
    mapPendingSNV.insert(std::make_pair(addr, std::make_pair(GetTime(), snv)));
    LogPrintf("CServiceNodeMan::SendVerifyRequest -- verifying node using nonce %d addr=%s\n", snv.nonce, addr.ToString());
    return true;
}

void CServiceNodeMan::ProcessPendingSnvRequests(CConnman& connman)
{
    LOCK(cs_mapPendingSNV);

    std::map<CService, std::pair<int64_t, CServiceNodeVerification> >::iterator itPendingSNV = mapPendingSNV.begin();

    while (itPendingSNV != mapPendingSNV.end()) {
        bool fDone = connman.ForNode(itPendingSNV->first, [&](CNode* pnode) {
            netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::SNVERIFY) + "-request");
            // use random nonce, store it and require node to reply with correct one later
            mWeAskedForVerification[pnode->addr] = itPendingSNV->second.second;
            LogPrint("servicenode", "-- verifying node using nonce %d addr=%s\n", itPendingSNV->second.second.nonce, pnode->addr.ToString());
            CNetMsgMaker msgMaker(pnode->GetSendVersion()); // TODO this gives a warning about version not being set (we should wait for VERSION exchange)
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::SNVERIFY, itPendingSNV->second.second));
            return true;
        });

        int64_t nTimeAdded = itPendingSNV->second.first;
        if (fDone || (GetTime() - nTimeAdded > 15)) {
            if (!fDone) {
                LogPrint("servicenode", "CServiceNodeMan::%s -- failed to connect to %s\n", __func__, itPendingSNV->first.ToString());
            }
            mapPendingSNV.erase(itPendingSNV++);
        } else {
            ++itPendingSNV;
        }
    }
}

void CServiceNodeMan::SendVerifyReply(CNode* pnode, CServiceNodeVerification& snv, CConnman& connman)
{
    AssertLockHeld(cs_main);

    // only ServiceNodes can sign this, why would someone ask regular node?
    if (!fServiceNodeMode) {
        // do not ban, malicious node might be using my IP
        // and trying to confuse the node which tries to verify it
        return;
    }

    if (netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::SNVERIFY) + "-reply")) {
        // peer should not ask us that often
        LogPrintf("CServiceNodeMan::SendVerifyReply -- ERROR: peer already asked me recently, peer=%d\n", pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if (!GetBlockHash(blockHash, snv.nBlockHeight)) {
        LogPrintf("CServiceNodeMan::SendVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", snv.nBlockHeight, pnode->id);
        return;
    }

    std::string strError;

    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
        uint256 hash = snv.GetSignatureHash1(blockHash);

        if (!CHashSigner::SignHash(hash, activeServiceNode.keyServiceNode, snv.vchSig1)) {
            LogPrintf("CServiceNodeMan::SendVerifyReply -- SignHash() failed\n");
            return;
        }

        if (!CHashSigner::VerifyHash(hash, activeServiceNode.pubKeyServiceNode, snv.vchSig1, strError)) {
            LogPrintf("CServiceNodeMan::SendVerifyReply -- VerifyHash() failed, error: %s\n", strError);
            return;
        }
    } else {
        std::string strMessage = strprintf("%s%d%s", activeServiceNode.service.ToString(false), snv.nonce, blockHash.ToString());

        if (!CMessageSigner::SignMessage(strMessage, snv.vchSig1, activeServiceNode.keyServiceNode)) {
            LogPrintf("CServiceNodeMan::SendVerifyReply -- SignMessage() failed\n");
            return;
        }

        if (!CMessageSigner::VerifyMessage(activeServiceNode.pubKeyServiceNode, snv.vchSig1, strMessage, strError)) {
            LogPrintf("CServiceNodeMan::SendVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
            return;
        }
    }

    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::SNVERIFY, snv));
    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::SNVERIFY) + "-reply");
}

void CServiceNodeMan::ProcessVerifyReply(CNode* pnode, CServiceNodeVerification& snv)
{
    AssertLockHeld(cs_main);

    std::string strError;

    // did we even ask for it? if that's the case we should have matching fulfilled request
    if (!netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::SNVERIFY) + "-request")) {
        LogPrintf("CServiceNodeMan::ProcessVerifyReply -- ERROR: we didn't ask for verification of %s, peer=%d\n", pnode->addr.ToString(), pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nonce for a known address must match the one we sent
    if (mWeAskedForVerification[pnode->addr].nonce != snv.nonce) {
        LogPrintf("CServiceNodeMan::ProcessVerifyReply -- ERROR: wrong nounce: requested=%d, received=%d, peer=%d\n",
            mWeAskedForVerification[pnode->addr].nonce, snv.nonce, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nBlockHeight for a known address must match the one we sent
    if (mWeAskedForVerification[pnode->addr].nBlockHeight != snv.nBlockHeight) {
        LogPrintf("CServiceNodeMan::ProcessVerifyReply -- ERROR: wrong nBlockHeight: requested=%d, received=%d, peer=%d\n",
            mWeAskedForVerification[pnode->addr].nBlockHeight, snv.nBlockHeight, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if (!GetBlockHash(blockHash, snv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("ServiceNodeMan::ProcessVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", snv.nBlockHeight, pnode->id);
        return;
    }

    // we already verified this address, why node is spamming?
    if (netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::SNVERIFY) + "-done")) {
        LogPrintf("CServiceNodeMan::ProcessVerifyReply -- ERROR: already verified %s recently\n", pnode->addr.ToString());
        Misbehaving(pnode->id, 20);
        return;
    }

    {
        LOCK(cs);

        CServiceNode* prealServiceNode = nullptr;
        std::vector<CServiceNode*> vpServiceNodesToBan;

        uint256 hash1 = snv.GetSignatureHash1(blockHash);
        std::string strMessage1 = strprintf("%s%d%s", pnode->addr.ToString(false), snv.nonce, blockHash.ToString());

        for (auto& snpair : mapServiceNodes) {
            if (CAddress(snpair.second.addr, NODE_NETWORK) == pnode->addr) {
                bool fFound = false;
                if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
                    fFound = CHashSigner::VerifyHash(hash1, snpair.second.pubKeyServiceNode, snv.vchSig1, strError);
                    // we don't care about snv with signature in old format
                } else {
                    fFound = CMessageSigner::VerifyMessage(snpair.second.pubKeyServiceNode, snv.vchSig1, strMessage1, strError);
                }
                if (fFound) {
                    // found it!
                    prealServiceNode = &snpair.second;
                    if (!snpair.second.IsPoSeVerified()) {
                        snpair.second.DecreasePoSeBanScore();
                    }
                    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::SNVERIFY) + "-done");

                    // we can only broadcast it if we are an activated servicenode
                    if (activeServiceNode.outpoint.IsNull())
                        continue;
                    // update ...
                    snv.addr = snpair.second.addr;
                    snv.servicenodeOutpoint1 = snpair.second.outpoint;
                    snv.servicenodeOutpoint2 = activeServiceNode.outpoint;
                    // ... and sign it
                    std::string strError;

                    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
                        uint256 hash2 = snv.GetSignatureHash2(blockHash);

                        if (!CHashSigner::SignHash(hash2, activeServiceNode.keyServiceNode, snv.vchSig2)) {
                            LogPrintf("ServiceNodeMan::ProcessVerifyReply -- SignHash() failed\n");
                            return;
                        }

                        if (!CHashSigner::VerifyHash(hash2, activeServiceNode.pubKeyServiceNode, snv.vchSig2, strError)) {
                            LogPrintf("ServiceNodeMan::ProcessVerifyReply -- VerifyHash() failed, error: %s\n", strError);
                            return;
                        }
                    } else {
                        std::string strMessage2 = strprintf("%s%d%s%s%s", snv.addr.ToString(false), snv.nonce, blockHash.ToString(),
                            snv.servicenodeOutpoint1.ToStringShort(), snv.servicenodeOutpoint2.ToStringShort());

                        if (!CMessageSigner::SignMessage(strMessage2, snv.vchSig2, activeServiceNode.keyServiceNode)) {
                            LogPrintf("ServiceNodeMan::ProcessVerifyReply -- SignMessage() failed\n");
                            return;
                        }

                        if (!CMessageSigner::VerifyMessage(activeServiceNode.pubKeyServiceNode, snv.vchSig2, strMessage2, strError)) {
                            LogPrintf("ServiceNodeMan::ProcessVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
                            return;
                        }
                    }

                    mWeAskedForVerification[pnode->addr] = snv;
                    mapSeenServiceNodeVerification.insert(std::make_pair(snv.GetHash(), snv));
                    snv.Relay();

                } else {
                    vpServiceNodesToBan.push_back(&snpair.second);
                }
            }
        }
        // no real ServiceNode found?...
        if (!prealServiceNode) {
            // this should never be the case normally,
            // only if someone is trying to game the system in some way or smth like that
            LogPrintf("CServiceNodeMan::ProcessVerifyReply -- ERROR: no real ServiceNode found for addr %s\n", pnode->addr.ToString());
            Misbehaving(pnode->id, 20);
            return;
        }
        LogPrintf("CServiceNodeMan::ProcessVerifyReply -- verified real ServiceNode %s for addr %s\n",
            prealServiceNode->outpoint.ToStringShort(), pnode->addr.ToString());
        // increase ban score for everyone else
        for (const auto& psn : vpServiceNodesToBan) {
            psn->IncreasePoSeBanScore();
            LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyReply -- increased PoSe ban score for %s addr %s, new score %d\n",
                prealServiceNode->outpoint.ToStringShort(), pnode->addr.ToString(), psn->nPoSeBanScore);
        }
        if (!vpServiceNodesToBan.empty())
            LogPrintf("CServiceNodeMan::ProcessVerifyReply -- PoSe score increased for %d fake ServiceNodes, addr %s\n",
                (int)vpServiceNodesToBan.size(), pnode->addr.ToString());
    }
}

void CServiceNodeMan::ProcessVerifyBroadcast(CNode* pnode, const CServiceNodeVerification& snv)
{
    AssertLockHeld(cs_main);

    std::string strError;

    if (mapSeenServiceNodeVerification.find(snv.GetHash()) != mapSeenServiceNodeVerification.end()) {
        // we already have one
        return;
    }
    mapSeenServiceNodeVerification[snv.GetHash()] = snv;

    // we don't care about history
    if (snv.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS) {
        LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyBroadcast -- Outdated: current block %d, verification block %d, peer=%d\n",
            nCachedBlockHeight, snv.nBlockHeight, pnode->id);
        return;
    }

    if (snv.servicenodeOutpoint1 == snv.servicenodeOutpoint2) {
        LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyBroadcast -- ERROR: same outpoints %s, peer=%d\n",
            snv.servicenodeOutpoint1.ToStringShort(), pnode->id);
        // that was NOT a good idea to cheat and verify itself,
        // ban the node we received such message from
        Misbehaving(pnode->id, 100);
        return;
    }

    uint256 blockHash;
    if (!GetBlockHash(blockHash, snv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- Can't get block hash for unknown block height %d, peer=%d\n", snv.nBlockHeight, pnode->id);
        return;
    }

    int nRank;

    if (!GetServiceNodeRank(snv.servicenodeOutpoint2, nRank, snv.nBlockHeight, MIN_POSE_PROTO_VERSION)) {
        LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyBroadcast -- Can't calculate rank for ServiceNode %s\n",
            snv.servicenodeOutpoint2.ToStringShort());
        return;
    }

    if (nRank > MAX_POSE_RANK) {
        LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyBroadcast -- ServiceNode %s is not in top %d, current rank %d, peer=%d\n",
            snv.servicenodeOutpoint2.ToStringShort(), (int)MAX_POSE_RANK, nRank, pnode->id);
        return;
    }

    {
        LOCK(cs);

        CServiceNode* psn1 = Find(snv.servicenodeOutpoint1);
        if (!psn1) {
            LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- can't find ServiceNode1 %s\n", snv.servicenodeOutpoint1.ToStringShort());
            return;
        }

        CServiceNode* psn2 = Find(snv.servicenodeOutpoint2);
        if (!psn2) {
            LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- can't find ServiceNode %s\n", snv.servicenodeOutpoint2.ToStringShort());
            return;
        }

        if (psn1->addr != snv.addr) {
            LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- addr %s does not match %s\n", snv.addr.ToString(), psn1->addr.ToString());
            return;
        }

        if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
            uint256 hash1 = snv.GetSignatureHash1(blockHash);
            uint256 hash2 = snv.GetSignatureHash2(blockHash);

            if (!CHashSigner::VerifyHash(hash1, psn1->pubKeyServiceNode, snv.vchSig1, strError)) {
                LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- VerifyHash() failed, error: %s\n", strError);
                return;
            }

            if (!CHashSigner::VerifyHash(hash2, psn2->pubKeyServiceNode, snv.vchSig2, strError)) {
                LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- VerifyHash() failed, error: %s\n", strError);
                return;
            }
        } else {
            std::string strMessage1 = strprintf("%s%d%s", snv.addr.ToString(false), snv.nonce, blockHash.ToString());
            std::string strMessage2 = strprintf("%s%d%s%s%s", snv.addr.ToString(false), snv.nonce, blockHash.ToString(),
                snv.servicenodeOutpoint1.ToStringShort(), snv.servicenodeOutpoint2.ToStringShort());

            if (!CMessageSigner::VerifyMessage(psn1->pubKeyServiceNode, snv.vchSig1, strMessage1, strError)) {
                LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- VerifyMessage() for servicenode1 failed, error: %s\n", strError);
                return;
            }

            if (!CMessageSigner::VerifyMessage(psn2->pubKeyServiceNode, snv.vchSig2, strMessage2, strError)) {
                LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- VerifyMessage() for servicenode2 failed, error: %s\n", strError);
                return;
            }
        }

        if (!psn1->IsPoSeVerified()) {
            psn1->DecreasePoSeBanScore();
        }
        snv.Relay();

        LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- verified ServiceNode %s for addr %s\n",
            psn1->outpoint.ToStringShort(), psn1->addr.ToString());

        // increase ban score for everyone else with the same addr
        int nCount = 0;
        for (auto& snpair : mapServiceNodes) {
            if (snpair.second.addr != snv.addr || snpair.first == snv.servicenodeOutpoint1)
                continue;
            snpair.second.IncreasePoSeBanScore();
            nCount++;
            LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                snpair.first.ToStringShort(), snpair.second.addr.ToString(), snpair.second.nPoSeBanScore);
        }
        if (nCount)
            LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyBroadcast -- PoSe score increased for %d fake ServiceNodes, addr %s\n",
                nCount, psn1->addr.ToString());
    }
}

std::string CServiceNodeMan::ToString() const
{
    std::ostringstream info;

    info << "ServiceNodes: " << (int)mapServiceNodes.size() << ", peers who asked us for ServiceNode list: " << (int)mAskedUsForServiceNodeList.size() << ", peers we asked for ServiceNode list: " << (int)mWeAskedForServiceNodeList.size() << ", entries in ServiceNode list we asked for: " << (int)mWeAskedForServiceNodeListEntry.size() << ", nPsqCount: " << (int)nPsqCount;

    return info.str();
}

bool CServiceNodeMan::CheckSnbAndUpdateServiceNodeList(CNode* pfrom, CServiceNodeBroadcast snb, int& nDos, CConnman& connman)
{
    // Need to lock cs_main here to ensure consistent locking order because the SimpleCheck call below locks cs_main
    LOCK(cs_main);

    {
        LOCK(cs);
        nDos = 0;
        LogPrint("servicenode", "CServiceNodeMan::CheckSnbAndUpdateServiceNodeList -- servicenode=%s\n", snb.outpoint.ToStringShort());

        uint256 hash = snb.GetHash();
        if (mapSeenServiceNodeBroadcast.count(hash) && !snb.fRecovery) { //seen
            LogPrint("servicenode", "CServiceNodeMan::CheckSnbAndUpdateServiceNodeList -- servicenode=%s seen\n", snb.outpoint.ToStringShort());
            // less then 2 pings left before this SN goes into non-recoverable state, bump sync timeout
            if (GetTime() - mapSeenServiceNodeBroadcast[hash].first > SERVICENODE_NEW_START_REQUIRED_SECONDS - SERVICENODE_MIN_SNP_SECONDS * 2) {
                LogPrint("servicenode", "CServiceNodeMan::CheckSnbAndUpdateServiceNodeList -- servicenode=%s seen update\n", snb.outpoint.ToStringShort());
                mapSeenServiceNodeBroadcast[hash].first = GetTime();
                servicenodeSync.BumpAssetLastTime("CServiceNodeMan::CheckSnbAndUpdateServiceNodeList - seen");
            }
            // did we ask this node for it?
            if (pfrom && IsSnbRecoveryRequested(hash) && GetTime() < mSnbRecoveryRequests[hash].first) {
                LogPrint("servicenode", "CServiceNodeMan::CheckSnbAndUpdateServiceNodeList -- snb=%s seen request\n", hash.ToString());
                if (mSnbRecoveryRequests[hash].second.count(pfrom->addr)) {
                    LogPrint("servicenode", "CServiceNodeMan::CheckSnbAndUpdateServiceNodeList -- snb=%s seen request, addr=%s\n", hash.ToString(), pfrom->addr.ToString());
                    // do not allow node to send same snb multiple times in recovery mode
                    mSnbRecoveryRequests[hash].second.erase(pfrom->addr);
                    // does it have newer lastPing?
                    if (snb.lastPing.sigTime > mapSeenServiceNodeBroadcast[hash].second.lastPing.sigTime) {
                        // simulate Check
                        CServiceNode snTemp = CServiceNode(snb);
                        snTemp.Check();
                        LogPrint("servicenode", "CServiceNodeMan::CheckSnbAndUpdateServiceNodeList -- snb=%s seen request, addr=%s, better lastPing: %d min ago, projected sn state: %s\n", hash.ToString(), pfrom->addr.ToString(), (GetAdjustedTime() - snb.lastPing.sigTime) / 60, snTemp.GetStateString());
                        if (snTemp.IsValidStateForAutoStart(snTemp.nActiveState)) {
                            // this node thinks it's a good one
                            LogPrint("servicenode", "CServiceNodeMan::CheckSnbAndUpdateServiceNodeList -- servicenode=%s seen good\n", snb.outpoint.ToStringShort());
                            mSnbRecoveryGoodReplies[hash].push_back(snb);
                        }
                    }
                }
            }
            return true;
        }
        mapSeenServiceNodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), snb)));

        LogPrint("servicenode", "CServiceNodeMan::CheckSnbAndUpdateServiceNodeList -- servicenode=%s new\n", snb.outpoint.ToStringShort());

        if (!snb.SimpleCheck(nDos)) {
            LogPrint("servicenode", "CServiceNodeMan::CheckSnbAndUpdateServiceNodeList -- SimpleCheck() failed, servicenode=%s\n", snb.outpoint.ToStringShort());
            return false;
        }

        // search ServiceNode list
        CServiceNode* psn = Find(snb.outpoint);
        if (psn) {
            CServiceNodeBroadcast snbOld = mapSeenServiceNodeBroadcast[CServiceNodeBroadcast(*psn).GetHash()].second;
            if (!snb.Update(psn, nDos, connman)) {
                LogPrint("servicenode", "CServiceNodeMan::CheckSnbAndUpdateServiceNodeList -- Update() failed, servicenode=%s\n", snb.outpoint.ToStringShort());
                return false;
            }
            if (hash != snbOld.GetHash()) {
                mapSeenServiceNodeBroadcast.erase(snbOld.GetHash());
            }
            return true;
        }
    }

    if (snb.CheckOutpoint(nDos)) {
        Add(snb);
        servicenodeSync.BumpAssetLastTime("CServiceNodeMan::CheckSnbAndUpdateServiceNodeList - new");
        // if it matches our ServiceNode privkey...
        if (fServiceNodeMode && snb.pubKeyServiceNode == activeServiceNode.pubKeyServiceNode) {
            snb.nPoSeBanScore = -SERVICENODE_POSE_BAN_MAX_SCORE;
            if (snb.nProtocolVersion == PROTOCOL_VERSION) {
                // ... and PROTOCOL_VERSION, then we've been remotely activated ...
                LogPrintf("CServiceNodeMan::CheckSnbAndUpdateServiceNodeList -- Got NEW ServiceNode entry: servicenode=%s  sigTime=%lld  addr=%s\n",
                    snb.outpoint.ToStringShort(), snb.sigTime, snb.addr.ToString());
                activeServiceNode.ManageState(connman);
            } else {
                // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
                // but also do not ban the node we get this message from
                LogPrintf("CServiceNodeMan::CheckSnbAndUpdateServiceNodeList -- wrong PROTOCOL_VERSION, re-activate your SN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", snb.nProtocolVersion, PROTOCOL_VERSION);
                return false;
            }
        }
        snb.Relay(connman);
    } else {
        LogPrintf("CServiceNodeMan::CheckSnbAndUpdateServiceNodeList -- Rejected ServiceNode entry: %s  addr=%s\n", snb.outpoint.ToStringShort(), snb.addr.ToString());
        return false;
    }

    return true;
}

void CServiceNodeMan::UpdateLastPaid(const CBlockIndex* pindex)
{
    LOCK(cs);

    if (fLiteMode || !servicenodeSync.IsWinnersListSynced() || mapServiceNodes.empty())
        return;

    static int nLastRunBlockHeight = 0;
    // Scan at least LAST_PAID_SCAN_BLOCKS but no more than snpayments.GetStorageLimit()
    int nMaxBlocksToScanBack = std::max(LAST_PAID_SCAN_BLOCKS, nCachedBlockHeight - nLastRunBlockHeight);
    nMaxBlocksToScanBack = std::min(nMaxBlocksToScanBack, snpayments.GetStorageLimit());

    LogPrint("servicenode", "CServiceNodeMan::UpdateLastPaid -- nCachedBlockHeight=%d, nLastRunBlockHeight=%d, nMaxBlocksToScanBack=%d\n",
        nCachedBlockHeight, nLastRunBlockHeight, nMaxBlocksToScanBack);

    for (auto& snpair : mapServiceNodes) {
        snpair.second.UpdateLastPaid(pindex, nMaxBlocksToScanBack);
    }

    nLastRunBlockHeight = nCachedBlockHeight;
}

void CServiceNodeMan::UpdateLastSentinelPingTime()
{
    LOCK(cs);
    nLastSentinelPingTime = GetTime();
}

bool CServiceNodeMan::IsSentinelPingActive()
{
    LOCK(cs);
    // Check if any ServiceNodes have voted recently, otherwise return false
    return (GetTime() - nLastSentinelPingTime) <= SERVICENODE_SENTINEL_PING_MAX_SECONDS;
}

bool CServiceNodeMan::AddGovernanceVote(const COutPoint& outpoint, uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    CServiceNode* psn = Find(outpoint);
    if (!psn) {
        return false;
    }
    psn->AddGovernanceVote(nGovernanceObjectHash);
    return true;
}

void CServiceNodeMan::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    for (auto& snpair : mapServiceNodes) {
        snpair.second.RemoveGovernanceObject(nGovernanceObjectHash);
    }
}

void CServiceNodeMan::CheckServiceNode(const CPubKey& pubKeyServiceNode, bool fForce)
{
    LOCK(cs);
    for (auto& snpair : mapServiceNodes) {
        if (snpair.second.pubKeyServiceNode == pubKeyServiceNode) {
            snpair.second.Check(fForce);
            return;
        }
    }
}

bool CServiceNodeMan::IsServiceNodePingedWithin(const COutPoint& outpoint, int nSeconds, int64_t nTimeToCheckAt)
{
    LOCK(cs);
    CServiceNode* psn = Find(outpoint);
    return psn ? psn->IsPingedWithin(nSeconds, nTimeToCheckAt) : false;
}

void CServiceNodeMan::SetServiceNodeLastPing(const COutPoint& outpoint, const CServiceNodePing& snp)
{
    LOCK(cs);
    CServiceNode* psn = Find(outpoint);
    if (!psn) {
        return;
    }
    psn->lastPing = snp;
    if (snp.fSentinelIsCurrent) {
        UpdateLastSentinelPingTime();
    }
    mapSeenServiceNodePing.insert(std::make_pair(snp.GetHash(), snp));

    CServiceNodeBroadcast snb(*psn);
    uint256 hash = snb.GetHash();
    if (mapSeenServiceNodeBroadcast.count(hash)) {
        mapSeenServiceNodeBroadcast[hash].second.lastPing = snp;
    }
}

void CServiceNodeMan::UpdatedBlockTip(const CBlockIndex* pindex)
{
    nCachedBlockHeight = pindex->nHeight;
    LogPrint("servicenode", "CServiceNodeMan::UpdatedBlockTip -- nCachedBlockHeight=%d\n", nCachedBlockHeight);

    CheckSameAddr();

    if (fServiceNodeMode) {
        // normal wallet does not need to update this every block, doing update on rpc call should be enough
        UpdateLastPaid(pindex);
    }
}

void CServiceNodeMan::WarnServiceNodeDaemonUpdates()
{
    LOCK(cs);

    static bool fWarned = false;

    if (fWarned || !size() || !servicenodeSync.IsServiceNodeListSynced())
        return;

    int nUpdatedServiceNodes{0};

    for (const auto& snpair : mapServiceNodes) {
        if (snpair.second.lastPing.nDaemonVersion > CLIENT_VERSION) {
            ++nUpdatedServiceNodes;
        }
    }

    // Warn only when at least half of known servicenodes already updated
    if (nUpdatedServiceNodes < size() / 2)
        return;

    std::string strWarning;
    if (nUpdatedServiceNodes != size()) {
        strWarning = strprintf(_("Warning: At least %d of %d servicenodes are running on a newer software version. Please check latest releases, you might need to update too."),
            nUpdatedServiceNodes, size());
    } else {
        // someone was postponing this update for way too long probably
        strWarning = strprintf(_("Warning: Every servicenode (out of %d known ones) is running on a newer software version. Please check latest releases, it's very likely that you missed a major/critical update."),
            size());
    }

    // notify GetWarnings(), called by Qt and the JSON-RPC code to warn the user
    SetMiscWarning(strWarning);
    // trigger GUI update
    uiInterface.NotifyAlertChanged(SerializeHash(strWarning), CT_NEW);
    // trigger cmd-line notification
    CAlert::Notify(strWarning);

    fWarned = true;
}

void CServiceNodeMan::NotifyServiceNodeUpdates(CConnman& connman)
{
    // Avoid double locking
    bool fServiceNodesAddedLocal = false;
    bool fServiceNodesRemovedLocal = false;
    {
        LOCK(cs);
        fServiceNodesAddedLocal = fServiceNodesAdded;
        fServiceNodesRemovedLocal = fServiceNodesRemoved;
    }

    if (fServiceNodesAddedLocal) {
        governance.CheckServiceNodeOrphanObjects(connman);
        governance.CheckServiceNodeOrphanVotes(connman);
    }
    if (fServiceNodesRemovedLocal) {
        governance.UpdateCachesAndClean();
    }

    LOCK(cs);
    fServiceNodesAdded = false;
    fServiceNodesRemoved = false;
}

void CServiceNodeMan::DoMaintenance(CConnman& connman)
{
    if (fLiteMode)
        return; // disable all Credit specific functionality

    if (!servicenodeSync.IsBlockchainSynced() || ShutdownRequested())
        return;

    static unsigned int nTick = 0;

    nTick++;

    // make sure to check all servicenodes first
    snodeman.Check();

    snodeman.ProcessPendingSnbRequests(connman);
    snodeman.ProcessPendingSnvRequests(connman);

    if (nTick % 60 == 0) {
        snodeman.ProcessServiceNodeConnections(connman);
        snodeman.CheckAndRemove(connman);
        snodeman.WarnServiceNodeDaemonUpdates();
    }

    if (fServiceNodeMode && (nTick % (60 * 5) == 0)) {
        snodeman.DoFullVerificationStep(connman);
    }
}
