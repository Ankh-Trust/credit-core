// Copyright (c) 2019-2019 The Ankh Core Developers
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
CServiceNodeMan dnodeman;

const std::string CServiceNodeMan::SERIALIZATION_VERSION_STRING = "CServiceNodeMan-Version-3";
const int CServiceNodeMan::LAST_PAID_SCAN_BLOCKS = 100;

struct CompareLastPaidBlock {
    bool operator()(const std::pair<int, const CServiceNode*>& t1,
        const std::pair<int, const CServiceNode*>& t2) const
    {
        return (t1.first != t2.first) ? (t1.first < t2.first) : (t1.second->outpoint < t2.second->outpoint);
    }
};

struct CompareScoreDN {
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
      mDnbRecoveryRequests(),
      mDnbRecoveryGoodReplies(),
      listScheduledDnbRequestConnections(),
      fServiceNodesAdded(false),
      fServiceNodesRemoved(false),
      vecDirtyGovernanceObjectHashes(),
      nLastSentinelPingTime(0),
      mapSeenServiceNodeBroadcast(),
      mapSeenServiceNodePing(),
      nPsqCount(0)
{
}

bool CServiceNodeMan::Add(CServiceNode& dn)
{
    LOCK(cs);

    if (Has(dn.outpoint))
        return false;

    LogPrint("servicenode", "CServiceNodeMan::Add -- Adding new ServiceNode: addr=%s, %i now\n", dn.addr.ToString(), size() + 1);
    mapServiceNodes[dn.outpoint] = dn;
    fServiceNodesAdded = true;
    return true;
}

void CServiceNodeMan::AskForDN(CNode* pnode, const COutPoint& outpoint, CConnman& connman)
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
            LogPrintf("CServiceNodeMan::AskForDN -- Asking same peer %s for missing ServiceNode entry again: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
        } else {
            // we already asked for this outpoint but not this node
            LogPrintf("CServiceNodeMan::AskForDN -- Asking new peer %s for missing ServiceNode entry: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
        }
    } else {
        // we never asked any node for this outpoint
        LogPrintf("CServiceNodeMan::AskForDN -- Asking peer %s for missing ServiceNode entry for the first time: %s\n", addrSquashed.ToString(), outpoint.ToStringShort());
    }
    mWeAskedForServiceNodeListEntry[outpoint][addrSquashed] = GetTime() + PSEG_UPDATE_SECONDS;

    if (pnode->GetSendVersion() == 70900) {
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::PSEG, CTxIn(outpoint)));
    } else {
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::PSEG, outpoint));
    }
}

bool CServiceNodeMan::AllowMixing(const COutPoint& outpoint)
{
    LOCK(cs);
    CServiceNode* pdn = Find(outpoint);
    if (!pdn) {
        return false;
    }
    nPsqCount++;
    pdn->nLastPsq = nPsqCount;
    pdn->fAllowMixingTx = true;

    return true;
}

bool CServiceNodeMan::DisallowMixing(const COutPoint& outpoint)
{
    LOCK(cs);
    CServiceNode* pdn = Find(outpoint);
    if (!pdn) {
        return false;
    }
    pdn->fAllowMixingTx = false;

    return true;
}

bool CServiceNodeMan::PoSeBan(const COutPoint& outpoint)
{
    LOCK(cs);
    CServiceNode* pdn = Find(outpoint);
    if (!pdn) {
        return false;
    }
    pdn->PoSeBan();

    return true;
}

void CServiceNodeMan::Check()
{
    LOCK2(cs_main, cs);

    LogPrint("servicenode", "CServiceNodeMan::Check -- nLastSentinelPingTime=%d, IsSentinelPingActive()=%d\n", nLastSentinelPingTime, IsSentinelPingActive());

    for (auto& dnpair : mapServiceNodes) {
        // NOTE: internally it checks only every SERVICENODE_CHECK_SECONDS seconds
        // since the last time, so expect some DNs to skip this
        dnpair.second.Check();
    }
}


void CServiceNodeMan::CheckAndRemove(CConnman& connman)
{
    if (!servicenodeSync.IsServiceNodeListSynced())
        return;

    LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove\n");

    {
        // Need LOCK2 here to ensure consistent locking order because code below locks cs_main
        // in CheckDnbAndUpdateServiceNodeList()
        LOCK2(cs_main, cs);

        Check();

        // Remove spent ServiceNodes, prepare structures and make requests to reasure the state of inactive ones
        rank_pair_vec_t vecServiceNodeRanks;
        // ask for up to DNB_RECOVERY_MAX_ASK_ENTRIES ServiceNode entries at a time
        int nAskForDnbRecovery = DNB_RECOVERY_MAX_ASK_ENTRIES;
        std::map<COutPoint, CServiceNode>::iterator it = mapServiceNodes.begin();
        while (it != mapServiceNodes.end()) {
            CServiceNodeBroadcast dnb = CServiceNodeBroadcast(it->second);
            uint256 hash = dnb.GetHash();
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
                bool fAsk = (nAskForDnbRecovery > 0) &&
                            servicenodeSync.IsSynced() &&
                            it->second.IsNewStartRequired() &&
                            !IsDnbRecoveryRequested(hash) &&
                            !IsArgSet("-connect");
                if (fAsk) {
                    // this DN is in a non-recoverable state and we haven't asked other nodes yet
                    std::set<CService> setRequested;
                    // calulate only once and only when it's needed
                    if (vecServiceNodeRanks.empty()) {
                        int nRandomBlockHeight = GetRandInt(nCachedBlockHeight);
                        GetServiceNodeRanks(vecServiceNodeRanks, nRandomBlockHeight);
                    }
                    bool fAskedForDnbRecovery = false;
                    // ask first DNB_RECOVERY_QUORUM_TOTAL ServiceNodes we can connect to and we haven't asked recently
                    for (int i = 0; setRequested.size() < DNB_RECOVERY_QUORUM_TOTAL && i < (int)vecServiceNodeRanks.size(); i++) {
                        // avoid banning
                        if (mWeAskedForServiceNodeListEntry.count(it->first) && mWeAskedForServiceNodeListEntry[it->first].count(vecServiceNodeRanks[i].second.addr))
                            continue;
                        // didn't ask recently, ok to ask now
                        CService addr = vecServiceNodeRanks[i].second.addr;
                        setRequested.insert(addr);
                        listScheduledDnbRequestConnections.push_back(std::make_pair(addr, hash));
                        fAskedForDnbRecovery = true;
                    }
                    if (fAskedForDnbRecovery) {
                        LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove -- Recovery initiated, servicenode=%s\n", it->first.ToStringShort());
                        nAskForDnbRecovery--;
                    }
                    // wait for dnb recovery replies for DNB_RECOVERY_WAIT_SECONDS seconds
                    mDnbRecoveryRequests[hash] = std::make_pair(GetTime() + DNB_RECOVERY_WAIT_SECONDS, setRequested);
                }
                ++it;
            }
        }
        // proces replies for SERVICENODE_NEW_START_REQUIRED ServiceNodes
        LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove -- mDnbRecoveryGoodReplies size=%d\n", (int)mDnbRecoveryGoodReplies.size());
        std::map<uint256, std::vector<CServiceNodeBroadcast> >::iterator itDnbReplies = mDnbRecoveryGoodReplies.begin();
        while (itDnbReplies != mDnbRecoveryGoodReplies.end()) {
            if (mDnbRecoveryRequests[itDnbReplies->first].first < GetTime()) {
                // all nodes we asked should have replied now
                if (itDnbReplies->second.size() >= DNB_RECOVERY_QUORUM_REQUIRED) {
                    // majority of nodes we asked agrees that this DN doesn't require new dnb, reprocess one of new dnbs
                    LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove -- reprocessing dnb, ServiceNode=%s\n", itDnbReplies->second[0].outpoint.ToStringShort());
                    // mapSeenServiceNodeBroadcast.erase(itDnbReplies->first);
                    int nDos;
                    itDnbReplies->second[0].fRecovery = true;
                    CheckDnbAndUpdateServiceNodeList(nullptr, itDnbReplies->second[0], nDos, connman);
                }
                LogPrint("servicenode", "CServiceNodeMan::CheckAndRemove -- removing dnb recovery reply, ServiceNode=%s, size=%d\n", itDnbReplies->second[0].outpoint.ToStringShort(), (int)itDnbReplies->second.size());
                mDnbRecoveryGoodReplies.erase(itDnbReplies++);
            } else {
                ++itDnbReplies;
            }
        }
    }
    {
        // no need for cm_main below
        LOCK(cs);

        auto itDnbRequest = mDnbRecoveryRequests.begin();
        while (itDnbRequest != mDnbRecoveryRequests.end()) {
            // Allow this dnb to be re-verified again after DNB_RECOVERY_RETRY_SECONDS seconds
            // if DN is still in SERVICENODE_NEW_START_REQUIRED state.
            if (GetTime() - itDnbRequest->second.first > DNB_RECOVERY_RETRY_SECONDS) {
                mDnbRecoveryRequests.erase(itDnbRequest++);
            } else {
                ++itDnbRequest;
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

        // NOTE: do not expire mapSeenServiceNodeBroadcast entries here, clean them on dnb updates!

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

    for (auto& dnpair : mapServiceNodes) {
        if (dnpair.second.nProtocolVersion < nProtocolVersion)
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

    for (auto& dnpair : mapServiceNodes) {
        if (dnpair.second.nProtocolVersion < nProtocolVersion || !dnpair.second.IsEnabled())
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

    for (auto& dnpair : mapServiceNodes)
        if ((nNetworkType == NET_IPV4 && dnpair.second.addr.IsIPv4()) ||
            (nNetworkType == NET_TOR  && dnpair.second.addr.IsTor())  ||
            (nNetworkType == NET_IPV6 && dnpair.second.addr.IsIPv6())) {
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

    if (pnode->GetSendVersion() == 70900) {
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

bool CServiceNodeMan::GetServiceNodeInfo(const COutPoint& outpoint, servicenode_info_t& dnInfoRet)
{
    LOCK(cs);
    auto it = mapServiceNodes.find(outpoint);
    if (it == mapServiceNodes.end()) {
        return false;
    }
    dnInfoRet = it->second.GetInfo();
    return true;
}

bool CServiceNodeMan::GetServiceNodeInfo(const CPubKey& pubKeyServiceNode, servicenode_info_t& dnInfoRet)
{
    LOCK(cs);
    for (auto& dnpair : mapServiceNodes) {
        if (dnpair.second.pubKeyServiceNode == pubKeyServiceNode) {
            dnInfoRet = dnpair.second.GetInfo();
            return true;
        }
    }
    return false;
}

bool CServiceNodeMan::GetServiceNodeInfo(const CScript& payee, servicenode_info_t& dnInfoRet)
{
    LOCK(cs);
    for (const auto& dnpair : mapServiceNodes) {
        CScript scriptCollateralAddress = GetScriptForDestination(dnpair.second.pubKeyCollateralAddress.GetID());
        if (scriptCollateralAddress == payee) {
            dnInfoRet = dnpair.second.GetInfo();
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
bool CServiceNodeMan::GetNextServiceNodeInQueueForPayment(bool fFilterSigTime, int& nCountRet, servicenode_info_t& dnInfoRet)
{
    return GetNextServiceNodeInQueueForPayment(nCachedBlockHeight, fFilterSigTime, nCountRet, dnInfoRet);
}

bool CServiceNodeMan::GetNextServiceNodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCountRet, servicenode_info_t& dnInfoRet)
{
    dnInfoRet = servicenode_info_t();
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

    int nDnCount = CountServiceNodes();

    for (const auto& dnpair : mapServiceNodes) {
        if (!dnpair.second.IsValidForPayment())
            continue;

        // //check protocol version
        if (dnpair.second.nProtocolVersion < snpayments.GetMinServiceNodePaymentsProto())
            continue;

        //it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
        if (snpayments.IsScheduled(dnpair.second, nBlockHeight))
            continue;

        //it's too new, wait for a cycle
        if (fFilterSigTime && dnpair.second.sigTime + (nDnCount * 2.6 * 60) > GetAdjustedTime())
            continue;

        //make sure it has at least as many confirmations as there are ServiceNodes
        if (GetUTXOConfirmations(dnpair.first) < nDnCount)
            continue;

        vecServiceNodeLastPaid.push_back(std::make_pair(dnpair.second.GetLastPaidBlock(), &dnpair.second));
    }

    nCountRet = (int)vecServiceNodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if (fFilterSigTime && nCountRet < nDnCount / 3)
        return GetNextServiceNodeInQueueForPayment(nBlockHeight, false, nCountRet, dnInfoRet);

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
    int nTenthNetwork = nDnCount / 10;
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
        dnInfoRet = pBestServiceNode->GetInfo();
    }
    return dnInfoRet.fInfoValid;
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
    for (const auto& dnpair : mapServiceNodes) {
        vpServiceNodesShuffled.push_back(&dnpair.second);
    }

    FastRandomContext insecure_rand;
    // shuffle pointers
    std::random_shuffle(vpServiceNodesShuffled.begin(), vpServiceNodesShuffled.end(), insecure_rand);
    bool fExclude;

    // loop through
    for (const auto& pdn : vpServiceNodesShuffled) {
        if (pdn->nProtocolVersion < nProtocolVersion || !pdn->IsEnabled())
            continue;
        fExclude = false;
        for (const auto& outpointToExclude : vecToExclude) {
            if (pdn->outpoint == outpointToExclude) {
                fExclude = true;
                break;
            }
        }
        if (fExclude)
            continue;
        // found the one not in vecToExclude
        LogPrint("servicenode", "CServiceNodeMan::FindRandomNotInVec -- found, ServiceNode=%s\n", pdn->outpoint.ToStringShort());
        return pdn->GetInfo();
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
    for (const auto& dnpair : mapServiceNodes) {
        if (dnpair.second.nProtocolVersion >= nMinProtocol) {
            vecServiceNodeScoresRet.push_back(std::make_pair(dnpair.second.CalculateScore(nBlockHash), &dnpair.second));
        }
    }

    sort(vecServiceNodeScoresRet.rbegin(), vecServiceNodeScoresRet.rend(), CompareScoreDN());
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

    std::vector<servicenode_info_t> vecDnInfo; // will be empty when no wallet
#ifdef ENABLE_WALLET
    privateSendClient.GetMixingServiceNodesInfo(vecDnInfo);
#endif // ENABLE_WALLET

    connman.ForEachNode(CConnman::AllNodes, [&vecDnInfo](CNode* pnode) {
        if (pnode->fServiceNode) {
#ifdef ENABLE_WALLET
            bool fFound = false;
            for (const auto& dnInfo : vecDnInfo) {
                if (pnode->addr == dnInfo.addr) {
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


std::pair<CService, std::set<uint256> > CServiceNodeMan::PopScheduledDnbRequestConnection()
{
    LOCK(cs);
    if (listScheduledDnbRequestConnections.empty()) {
        return std::make_pair(CService(), std::set<uint256>());
    }

    std::set<uint256> setResult;

    listScheduledDnbRequestConnections.sort();
    std::pair<CService, uint256> pairFront = listScheduledDnbRequestConnections.front();

    // squash hashes from requests with the same CService as the first one into setResult
    std::list<std::pair<CService, uint256> >::iterator it = listScheduledDnbRequestConnections.begin();
    while (it != listScheduledDnbRequestConnections.end()) {
        if (pairFront.first == it->first) {
            setResult.insert(it->second);
            it = listScheduledDnbRequestConnections.erase(it);
        } else {
            // since list is sorted now, we can be sure that there is no more hashes left
            // to ask for from this addr
            break;
        }
    }
    return std::make_pair(pairFront.first, setResult);
}

void CServiceNodeMan::ProcessPendingDnbRequests(CConnman& connman)
{
    std::pair<CService, std::set<uint256> > p = PopScheduledDnbRequestConnection();
    if (!(p.first == CService() || p.second.empty())) {
        if (connman.IsServiceNodeOrDisconnectRequested(p.first))
            return;
        mapPendingDNB.insert(std::make_pair(p.first, std::make_pair(GetTime(), p.second)));
        connman.AddPendingServiceNode(p.first);
    }

    std::map<CService, std::pair<int64_t, std::set<uint256> > >::iterator itPendingDNB = mapPendingDNB.begin();
    while (itPendingDNB != mapPendingDNB.end()) {
        bool fDone = connman.ForNode(itPendingDNB->first, [&](CNode* pnode) {
            // compile request vector
            std::vector<CInv> vToFetch;
            for (auto& nHash : itPendingDNB->second.second) {
                if (nHash != uint256()) {
                    vToFetch.push_back(CInv(MSG_SERVICENODE_ANNOUNCE, nHash));
                    LogPrint("servicenode", "-- asking for dnb %s from addr=%s\n", nHash.ToString(), pnode->addr.ToString());
                }
            }

            // ask for data
            CNetMsgMaker msgMaker(pnode->GetSendVersion());
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::GETDATA, vToFetch));
            return true;
        });

        int64_t nTimeAdded = itPendingDNB->second.first;
        if (fDone || (GetTime() - nTimeAdded > 15)) {
            if (!fDone) {
                LogPrint("servicenode", "CServiceNodeMan::%s -- failed to connect to %s\n", __func__, itPendingDNB->first.ToString());
            }
            mapPendingDNB.erase(itPendingDNB++);
        } else {
            ++itPendingDNB;
        }
    }
}

void CServiceNodeMan::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if (fLiteMode)
        return; // disable all Credit specific functionality

    if (strCommand == NetMsgType::DNANNOUNCE) { //ServiceNode Broadcast

        CServiceNodeBroadcast dnb;
        vRecv >> dnb;

        pfrom->setAskFor.erase(dnb.GetHash());

        if (!servicenodeSync.IsBlockchainSynced())
            return;

        LogPrint("servicenode", "DNANNOUNCE -- ServiceNode announce, ServiceNode=%s\n", dnb.outpoint.ToStringShort());

        int nDos = 0;

        if (CheckDnbAndUpdateServiceNodeList(pfrom, dnb, nDos, connman)) {
            // use announced ServiceNode as a peer
            connman.AddNewAddress(CAddress(dnb.addr, NODE_NETWORK), pfrom->addr, 2 * 60 * 60);
        } else if (nDos > 0) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), nDos);
        }

        if (fServiceNodesAdded) {
            NotifyServiceNodeUpdates(connman);
        }
    } else if (strCommand == NetMsgType::DNPING) { //ServiceNode Ping

        CServiceNodePing dnp;
        vRecv >> dnp;

        uint256 nHash = dnp.GetHash();

        pfrom->setAskFor.erase(nHash);

        if (!servicenodeSync.IsBlockchainSynced())
            return;

        LogPrint("servicenode", "DNPING -- ServiceNode ping, ServiceNode=%s\n", dnp.servicenodeOutpoint.ToStringShort());

        // Need LOCK2 here to ensure consistent locking order because the CheckAndUpdate call below locks cs_main
        LOCK2(cs_main, cs);

        if (mapSeenServiceNodePing.count(nHash))
            return; //seen
        mapSeenServiceNodePing.insert(std::make_pair(nHash, dnp));

        LogPrint("servicenode", "DNPING -- ServiceNode ping, ServiceNode=%s new\n", dnp.servicenodeOutpoint.ToStringShort());

        // see if we have this ServiceNode
        CServiceNode* pdn = Find(dnp.servicenodeOutpoint);

        if (pdn && dnp.fSentinelIsCurrent)
            UpdateLastSentinelPingTime();

        // too late, new DNANNOUNCE is required
        if (pdn && pdn->IsNewStartRequired())
            return;

        int nDos = 0;
        if (dnp.CheckAndUpdate(pdn, false, nDos, connman))
            return;

        if (nDos > 0) {
            // if anything significant failed, mark that node
            Misbehaving(pfrom->GetId(), nDos);
        } else if (pdn != NULL) {
            // nothing significant failed, dn is a known one too
            return;
        }

        // something significant is broken or dn is unknown,
        // we might have to ask for a ServiceNode entry once
        AskForDN(pfrom, dnp.servicenodeOutpoint, connman);

    } else if (strCommand == NetMsgType::PSEG) { //Get ServiceNode list or specific entry
        // Ignore such requests until we are fully synced.
        // We could start processing this after ServiceNode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!servicenodeSync.IsSynced())
            return;

        COutPoint servicenodeOutpoint;

        if (pfrom->nVersion == 70900) {
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

    } else if (strCommand == NetMsgType::DNVERIFY) { // ServiceNode Verify

        // Need LOCK2 here to ensure consistent locking order because the all functions below call GetBlockHash which locks cs_main
        LOCK2(cs_main, cs);

        CServiceNodeVerification dnv;
        vRecv >> dnv;

        pfrom->setAskFor.erase(dnv.GetHash());

        if (!servicenodeSync.IsServiceNodeListSynced())
            return;

        if (dnv.vchSig1.empty()) {
            // CASE 1: someone asked me to verify myself /IP we are using/
            SendVerifyReply(pfrom, dnv, connman);
        } else if (dnv.vchSig2.empty()) {
            // CASE 2: we _probably_ got verification we requested from some ServiceNode
            ProcessVerifyReply(pfrom, dnv);
        } else {
            // CASE 3: we _probably_ got verification broadcast signed by some ServiceNode which verified another one
            ProcessVerifyBroadcast(pfrom, dnv);
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

    for (const auto& dnpair : mapServiceNodes) {
        if (Params().RequireRoutableExternalIP() &&
            (dnpair.second.addr.IsRFC1918() || dnpair.second.addr.IsLocal()))
            continue; // do not send local network ServiceNode
        // NOTE: send ServiceNode regardless of its current state, the other node will need it to verify old votes.
        LogPrint("servicenode", "CServiceNodeMan::%s -- Sending ServiceNode entry: ServiceNode=%s  addr=%s\n", __func__, dnpair.first.ToStringShort(), dnpair.second.addr.ToString());
        PushPsegInvs(pnode, dnpair.second);
        nInvCount++;
    }

    connman.PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::SYNCSTATUSCOUNT, SERVICENODE_SYNC_LIST, nInvCount));
    LogPrintf("CServiceNodeMan::%s -- Sent %d ServiceNode invs to peer=%d\n", __func__, nInvCount, pnode->id);
}

void CServiceNodeMan::PushPsegInvs(CNode* pnode, const CServiceNode& dn)
{
    AssertLockHeld(cs);

    CServiceNodeBroadcast dnb(dn);
    CServiceNodePing dnp = dnb.lastPing;
    uint256 hashDNB = dnb.GetHash();
    uint256 hashDNP = dnp.GetHash();
    pnode->PushInventory(CInv(MSG_SERVICENODE_ANNOUNCE, hashDNB));
    pnode->PushInventory(CInv(MSG_SERVICENODE_PING, hashDNP));
    mapSeenServiceNodeBroadcast.insert(std::make_pair(hashDNB, std::make_pair(GetTime(), dnb)));
    mapSeenServiceNodePing.insert(std::make_pair(hashDNP, dnp));
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
    for (const auto& dnpair : mapServiceNodes) {
        vSortedByAddr.push_back(&dnpair.second);
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

        for (auto& dnpair : mapServiceNodes) {
            vSortedByAddr.push_back(&dnpair.second);
        }

        sort(vSortedByAddr.begin(), vSortedByAddr.end(), CompareByAddr());

        for (const auto& pdn : vSortedByAddr) {
            // check only (pre)enabled ServiceNodes
            if (!pdn->IsEnabled() && !pdn->IsPreEnabled())
                continue;
            // initial step
            if (!pprevServiceNode) {
                pprevServiceNode = pdn;
                pverifiedServiceNode = pdn->IsPoSeVerified() ? pdn : nullptr;
                continue;
            }
            // second+ step
            if (pdn->addr == pprevServiceNode->addr) {
                if (pverifiedServiceNode) {
                    // another ServiceNode with the same ip is verified, ban this one
                    vBan.push_back(pdn);
                } else if (pdn->IsPoSeVerified()) {
                    // this ServiceNode with the same ip is verified, ban previous one
                    vBan.push_back(pprevServiceNode);
                    // and keep a reference to be able to ban following ServiceNodes with the same ip
                    pverifiedServiceNode = pdn;
                }
            } else {
                pverifiedServiceNode = pdn->IsPoSeVerified() ? pdn : nullptr;
            }
            pprevServiceNode = pdn;
        }
    }

    // ban duplicates
    for (auto& pdn : vBan) {
        LogPrintf("CServiceNodeMan::CheckSameAddr -- increasing PoSe ban score for ServiceNode %s\n", pdn->outpoint.ToStringShort());
        pdn->IncreasePoSeBanScore();
    }
}

bool CServiceNodeMan::SendVerifyRequest(const CAddress& addr, const std::vector<const CServiceNode*>& vSortedByAddr, CConnman& connman)
{
    if (netfulfilledman.HasFulfilledRequest(addr, strprintf("%s", NetMsgType::DNVERIFY) + "-request")) {
        // we already asked for verification, not a good idea to do this too often, skip it
        LogPrint("servicenode", "CServiceNodeMan::SendVerifyRequest -- too many requests, skipping... addr=%s\n", addr.ToString());
        return false;
    }

    if (connman.IsServiceNodeOrDisconnectRequested(addr))
        return false;

    connman.AddPendingServiceNode(addr);
    // use random nonce, store it and require node to reply with correct one later
    CServiceNodeVerification dnv(addr, GetRandInt(999999), nCachedBlockHeight - 1);
    LOCK(cs_mapPendingDNV);
    mapPendingDNV.insert(std::make_pair(addr, std::make_pair(GetTime(), dnv)));
    LogPrintf("CServiceNodeMan::SendVerifyRequest -- verifying node using nonce %d addr=%s\n", dnv.nonce, addr.ToString());
    return true;
}

void CServiceNodeMan::ProcessPendingDnvRequests(CConnman& connman)
{
    LOCK(cs_mapPendingDNV);

    std::map<CService, std::pair<int64_t, CServiceNodeVerification> >::iterator itPendingDNV = mapPendingDNV.begin();

    while (itPendingDNV != mapPendingDNV.end()) {
        bool fDone = connman.ForNode(itPendingDNV->first, [&](CNode* pnode) {
            netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::DNVERIFY) + "-request");
            // use random nonce, store it and require node to reply with correct one later
            mWeAskedForVerification[pnode->addr] = itPendingDNV->second.second;
            LogPrint("servicenode", "-- verifying node using nonce %d addr=%s\n", itPendingDNV->second.second.nonce, pnode->addr.ToString());
            CNetMsgMaker msgMaker(pnode->GetSendVersion()); // TODO this gives a warning about version not being set (we should wait for VERSION exchange)
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DNVERIFY, itPendingDNV->second.second));
            return true;
        });

        int64_t nTimeAdded = itPendingDNV->second.first;
        if (fDone || (GetTime() - nTimeAdded > 15)) {
            if (!fDone) {
                LogPrint("servicenode", "CServiceNodeMan::%s -- failed to connect to %s\n", __func__, itPendingDNV->first.ToString());
            }
            mapPendingDNV.erase(itPendingDNV++);
        } else {
            ++itPendingDNV;
        }
    }
}

void CServiceNodeMan::SendVerifyReply(CNode* pnode, CServiceNodeVerification& dnv, CConnman& connman)
{
    AssertLockHeld(cs_main);

    // only ServiceNodes can sign this, why would someone ask regular node?
    if (!fServiceNodeMode) {
        // do not ban, malicious node might be using my IP
        // and trying to confuse the node which tries to verify it
        return;
    }

    if (netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::DNVERIFY) + "-reply")) {
        // peer should not ask us that often
        LogPrintf("CServiceNodeMan::SendVerifyReply -- ERROR: peer already asked me recently, peer=%d\n", pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if (!GetBlockHash(blockHash, dnv.nBlockHeight)) {
        LogPrintf("CServiceNodeMan::SendVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", dnv.nBlockHeight, pnode->id);
        return;
    }

    std::string strError;

    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
        uint256 hash = dnv.GetSignatureHash1(blockHash);

        if (!CHashSigner::SignHash(hash, activeServiceNode.keyServiceNode, dnv.vchSig1)) {
            LogPrintf("CServiceNodeMan::SendVerifyReply -- SignHash() failed\n");
            return;
        }

        if (!CHashSigner::VerifyHash(hash, activeServiceNode.pubKeyServiceNode, dnv.vchSig1, strError)) {
            LogPrintf("CServiceNodeMan::SendVerifyReply -- VerifyHash() failed, error: %s\n", strError);
            return;
        }
    } else {
        std::string strMessage = strprintf("%s%d%s", activeServiceNode.service.ToString(false), dnv.nonce, blockHash.ToString());

        if (!CMessageSigner::SignMessage(strMessage, dnv.vchSig1, activeServiceNode.keyServiceNode)) {
            LogPrintf("CServiceNodeMan::SendVerifyReply -- SignMessage() failed\n");
            return;
        }

        if (!CMessageSigner::VerifyMessage(activeServiceNode.pubKeyServiceNode, dnv.vchSig1, strMessage, strError)) {
            LogPrintf("CServiceNodeMan::SendVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
            return;
        }
    }

    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::DNVERIFY, dnv));
    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::DNVERIFY) + "-reply");
}

void CServiceNodeMan::ProcessVerifyReply(CNode* pnode, CServiceNodeVerification& dnv)
{
    AssertLockHeld(cs_main);

    std::string strError;

    // did we even ask for it? if that's the case we should have matching fulfilled request
    if (!netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::DNVERIFY) + "-request")) {
        LogPrintf("CServiceNodeMan::ProcessVerifyReply -- ERROR: we didn't ask for verification of %s, peer=%d\n", pnode->addr.ToString(), pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nonce for a known address must match the one we sent
    if (mWeAskedForVerification[pnode->addr].nonce != dnv.nonce) {
        LogPrintf("CServiceNodeMan::ProcessVerifyReply -- ERROR: wrong nounce: requested=%d, received=%d, peer=%d\n",
            mWeAskedForVerification[pnode->addr].nonce, dnv.nonce, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    // Received nBlockHeight for a known address must match the one we sent
    if (mWeAskedForVerification[pnode->addr].nBlockHeight != dnv.nBlockHeight) {
        LogPrintf("CServiceNodeMan::ProcessVerifyReply -- ERROR: wrong nBlockHeight: requested=%d, received=%d, peer=%d\n",
            mWeAskedForVerification[pnode->addr].nBlockHeight, dnv.nBlockHeight, pnode->id);
        Misbehaving(pnode->id, 20);
        return;
    }

    uint256 blockHash;
    if (!GetBlockHash(blockHash, dnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("ServiceNodeMan::ProcessVerifyReply -- can't get block hash for unknown block height %d, peer=%d\n", dnv.nBlockHeight, pnode->id);
        return;
    }

    // we already verified this address, why node is spamming?
    if (netfulfilledman.HasFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::DNVERIFY) + "-done")) {
        LogPrintf("CServiceNodeMan::ProcessVerifyReply -- ERROR: already verified %s recently\n", pnode->addr.ToString());
        Misbehaving(pnode->id, 20);
        return;
    }

    {
        LOCK(cs);

        CServiceNode* prealServiceNode = nullptr;
        std::vector<CServiceNode*> vpServiceNodesToBan;

        uint256 hash1 = dnv.GetSignatureHash1(blockHash);
        std::string strMessage1 = strprintf("%s%d%s", pnode->addr.ToString(false), dnv.nonce, blockHash.ToString());

        for (auto& dnpair : mapServiceNodes) {
            if (CAddress(dnpair.second.addr, NODE_NETWORK) == pnode->addr) {
                bool fFound = false;
                if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
                    fFound = CHashSigner::VerifyHash(hash1, dnpair.second.pubKeyServiceNode, dnv.vchSig1, strError);
                    // we don't care about dnv with signature in old format
                } else {
                    fFound = CMessageSigner::VerifyMessage(dnpair.second.pubKeyServiceNode, dnv.vchSig1, strMessage1, strError);
                }
                if (fFound) {
                    // found it!
                    prealServiceNode = &dnpair.second;
                    if (!dnpair.second.IsPoSeVerified()) {
                        dnpair.second.DecreasePoSeBanScore();
                    }
                    netfulfilledman.AddFulfilledRequest(pnode->addr, strprintf("%s", NetMsgType::DNVERIFY) + "-done");

                    // we can only broadcast it if we are an activated servicenode
                    if (activeServiceNode.outpoint.IsNull())
                        continue;
                    // update ...
                    dnv.addr = dnpair.second.addr;
                    dnv.servicenodeOutpoint1 = dnpair.second.outpoint;
                    dnv.servicenodeOutpoint2 = activeServiceNode.outpoint;
                    // ... and sign it
                    std::string strError;

                    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
                        uint256 hash2 = dnv.GetSignatureHash2(blockHash);

                        if (!CHashSigner::SignHash(hash2, activeServiceNode.keyServiceNode, dnv.vchSig2)) {
                            LogPrintf("ServiceNodeMan::ProcessVerifyReply -- SignHash() failed\n");
                            return;
                        }

                        if (!CHashSigner::VerifyHash(hash2, activeServiceNode.pubKeyServiceNode, dnv.vchSig2, strError)) {
                            LogPrintf("ServiceNodeMan::ProcessVerifyReply -- VerifyHash() failed, error: %s\n", strError);
                            return;
                        }
                    } else {
                        std::string strMessage2 = strprintf("%s%d%s%s%s", dnv.addr.ToString(false), dnv.nonce, blockHash.ToString(),
                            dnv.servicenodeOutpoint1.ToStringShort(), dnv.servicenodeOutpoint2.ToStringShort());

                        if (!CMessageSigner::SignMessage(strMessage2, dnv.vchSig2, activeServiceNode.keyServiceNode)) {
                            LogPrintf("ServiceNodeMan::ProcessVerifyReply -- SignMessage() failed\n");
                            return;
                        }

                        if (!CMessageSigner::VerifyMessage(activeServiceNode.pubKeyServiceNode, dnv.vchSig2, strMessage2, strError)) {
                            LogPrintf("ServiceNodeMan::ProcessVerifyReply -- VerifyMessage() failed, error: %s\n", strError);
                            return;
                        }
                    }

                    mWeAskedForVerification[pnode->addr] = dnv;
                    mapSeenServiceNodeVerification.insert(std::make_pair(dnv.GetHash(), dnv));
                    dnv.Relay();

                } else {
                    vpServiceNodesToBan.push_back(&dnpair.second);
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
        for (const auto& pdn : vpServiceNodesToBan) {
            pdn->IncreasePoSeBanScore();
            LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyReply -- increased PoSe ban score for %s addr %s, new score %d\n",
                prealServiceNode->outpoint.ToStringShort(), pnode->addr.ToString(), pdn->nPoSeBanScore);
        }
        if (!vpServiceNodesToBan.empty())
            LogPrintf("CServiceNodeMan::ProcessVerifyReply -- PoSe score increased for %d fake ServiceNodes, addr %s\n",
                (int)vpServiceNodesToBan.size(), pnode->addr.ToString());
    }
}

void CServiceNodeMan::ProcessVerifyBroadcast(CNode* pnode, const CServiceNodeVerification& dnv)
{
    AssertLockHeld(cs_main);

    std::string strError;

    if (mapSeenServiceNodeVerification.find(dnv.GetHash()) != mapSeenServiceNodeVerification.end()) {
        // we already have one
        return;
    }
    mapSeenServiceNodeVerification[dnv.GetHash()] = dnv;

    // we don't care about history
    if (dnv.nBlockHeight < nCachedBlockHeight - MAX_POSE_BLOCKS) {
        LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyBroadcast -- Outdated: current block %d, verification block %d, peer=%d\n",
            nCachedBlockHeight, dnv.nBlockHeight, pnode->id);
        return;
    }

    if (dnv.servicenodeOutpoint1 == dnv.servicenodeOutpoint2) {
        LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyBroadcast -- ERROR: same outpoints %s, peer=%d\n",
            dnv.servicenodeOutpoint1.ToStringShort(), pnode->id);
        // that was NOT a good idea to cheat and verify itself,
        // ban the node we received such message from
        Misbehaving(pnode->id, 100);
        return;
    }

    uint256 blockHash;
    if (!GetBlockHash(blockHash, dnv.nBlockHeight)) {
        // this shouldn't happen...
        LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- Can't get block hash for unknown block height %d, peer=%d\n", dnv.nBlockHeight, pnode->id);
        return;
    }

    int nRank;

    if (!GetServiceNodeRank(dnv.servicenodeOutpoint2, nRank, dnv.nBlockHeight, MIN_POSE_PROTO_VERSION)) {
        LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyBroadcast -- Can't calculate rank for ServiceNode %s\n",
            dnv.servicenodeOutpoint2.ToStringShort());
        return;
    }

    if (nRank > MAX_POSE_RANK) {
        LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyBroadcast -- ServiceNode %s is not in top %d, current rank %d, peer=%d\n",
            dnv.servicenodeOutpoint2.ToStringShort(), (int)MAX_POSE_RANK, nRank, pnode->id);
        return;
    }

    {
        LOCK(cs);

        CServiceNode* pdn1 = Find(dnv.servicenodeOutpoint1);
        if (!pdn1) {
            LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- can't find ServiceNode1 %s\n", dnv.servicenodeOutpoint1.ToStringShort());
            return;
        }

        CServiceNode* pdn2 = Find(dnv.servicenodeOutpoint2);
        if (!pdn2) {
            LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- can't find ServiceNode %s\n", dnv.servicenodeOutpoint2.ToStringShort());
            return;
        }

        if (pdn1->addr != dnv.addr) {
            LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- addr %s does not match %s\n", dnv.addr.ToString(), pdn1->addr.ToString());
            return;
        }

        if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
            uint256 hash1 = dnv.GetSignatureHash1(blockHash);
            uint256 hash2 = dnv.GetSignatureHash2(blockHash);

            if (!CHashSigner::VerifyHash(hash1, pdn1->pubKeyServiceNode, dnv.vchSig1, strError)) {
                LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- VerifyHash() failed, error: %s\n", strError);
                return;
            }

            if (!CHashSigner::VerifyHash(hash2, pdn2->pubKeyServiceNode, dnv.vchSig2, strError)) {
                LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- VerifyHash() failed, error: %s\n", strError);
                return;
            }
        } else {
            std::string strMessage1 = strprintf("%s%d%s", dnv.addr.ToString(false), dnv.nonce, blockHash.ToString());
            std::string strMessage2 = strprintf("%s%d%s%s%s", dnv.addr.ToString(false), dnv.nonce, blockHash.ToString(),
                dnv.servicenodeOutpoint1.ToStringShort(), dnv.servicenodeOutpoint2.ToStringShort());

            if (!CMessageSigner::VerifyMessage(pdn1->pubKeyServiceNode, dnv.vchSig1, strMessage1, strError)) {
                LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- VerifyMessage() for servicenode1 failed, error: %s\n", strError);
                return;
            }

            if (!CMessageSigner::VerifyMessage(pdn2->pubKeyServiceNode, dnv.vchSig2, strMessage2, strError)) {
                LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- VerifyMessage() for servicenode2 failed, error: %s\n", strError);
                return;
            }
        }

        if (!pdn1->IsPoSeVerified()) {
            pdn1->DecreasePoSeBanScore();
        }
        dnv.Relay();

        LogPrintf("CServiceNodeMan::ProcessVerifyBroadcast -- verified ServiceNode %s for addr %s\n",
            pdn1->outpoint.ToStringShort(), pdn1->addr.ToString());

        // increase ban score for everyone else with the same addr
        int nCount = 0;
        for (auto& dnpair : mapServiceNodes) {
            if (dnpair.second.addr != dnv.addr || dnpair.first == dnv.servicenodeOutpoint1)
                continue;
            dnpair.second.IncreasePoSeBanScore();
            nCount++;
            LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyBroadcast -- increased PoSe ban score for %s addr %s, new score %d\n",
                dnpair.first.ToStringShort(), dnpair.second.addr.ToString(), dnpair.second.nPoSeBanScore);
        }
        if (nCount)
            LogPrint("servicenode", "CServiceNodeMan::ProcessVerifyBroadcast -- PoSe score increased for %d fake ServiceNodes, addr %s\n",
                nCount, pdn1->addr.ToString());
    }
}

std::string CServiceNodeMan::ToString() const
{
    std::ostringstream info;

    info << "ServiceNodes: " << (int)mapServiceNodes.size() << ", peers who asked us for ServiceNode list: " << (int)mAskedUsForServiceNodeList.size() << ", peers we asked for ServiceNode list: " << (int)mWeAskedForServiceNodeList.size() << ", entries in ServiceNode list we asked for: " << (int)mWeAskedForServiceNodeListEntry.size() << ", nPsqCount: " << (int)nPsqCount;

    return info.str();
}

bool CServiceNodeMan::CheckDnbAndUpdateServiceNodeList(CNode* pfrom, CServiceNodeBroadcast dnb, int& nDos, CConnman& connman)
{
    // Need to lock cs_main here to ensure consistent locking order because the SimpleCheck call below locks cs_main
    LOCK(cs_main);

    {
        LOCK(cs);
        nDos = 0;
        LogPrint("servicenode", "CServiceNodeMan::CheckDnbAndUpdateServiceNodeList -- servicenode=%s\n", dnb.outpoint.ToStringShort());

        uint256 hash = dnb.GetHash();
        if (mapSeenServiceNodeBroadcast.count(hash) && !dnb.fRecovery) { //seen
            LogPrint("servicenode", "CServiceNodeMan::CheckDnbAndUpdateServiceNodeList -- servicenode=%s seen\n", dnb.outpoint.ToStringShort());
            // less then 2 pings left before this DN goes into non-recoverable state, bump sync timeout
            if (GetTime() - mapSeenServiceNodeBroadcast[hash].first > SERVICENODE_NEW_START_REQUIRED_SECONDS - SERVICENODE_MIN_DNP_SECONDS * 2) {
                LogPrint("servicenode", "CServiceNodeMan::CheckDnbAndUpdateServiceNodeList -- servicenode=%s seen update\n", dnb.outpoint.ToStringShort());
                mapSeenServiceNodeBroadcast[hash].first = GetTime();
                servicenodeSync.BumpAssetLastTime("CServiceNodeMan::CheckDnbAndUpdateServiceNodeList - seen");
            }
            // did we ask this node for it?
            if (pfrom && IsDnbRecoveryRequested(hash) && GetTime() < mDnbRecoveryRequests[hash].first) {
                LogPrint("servicenode", "CServiceNodeMan::CheckDnbAndUpdateServiceNodeList -- dnb=%s seen request\n", hash.ToString());
                if (mDnbRecoveryRequests[hash].second.count(pfrom->addr)) {
                    LogPrint("servicenode", "CServiceNodeMan::CheckDnbAndUpdateServiceNodeList -- dnb=%s seen request, addr=%s\n", hash.ToString(), pfrom->addr.ToString());
                    // do not allow node to send same dnb multiple times in recovery mode
                    mDnbRecoveryRequests[hash].second.erase(pfrom->addr);
                    // does it have newer lastPing?
                    if (dnb.lastPing.sigTime > mapSeenServiceNodeBroadcast[hash].second.lastPing.sigTime) {
                        // simulate Check
                        CServiceNode dnTemp = CServiceNode(dnb);
                        dnTemp.Check();
                        LogPrint("servicenode", "CServiceNodeMan::CheckDnbAndUpdateServiceNodeList -- dnb=%s seen request, addr=%s, better lastPing: %d min ago, projected dn state: %s\n", hash.ToString(), pfrom->addr.ToString(), (GetAdjustedTime() - dnb.lastPing.sigTime) / 60, dnTemp.GetStateString());
                        if (dnTemp.IsValidStateForAutoStart(dnTemp.nActiveState)) {
                            // this node thinks it's a good one
                            LogPrint("servicenode", "CServiceNodeMan::CheckDnbAndUpdateServiceNodeList -- servicenode=%s seen good\n", dnb.outpoint.ToStringShort());
                            mDnbRecoveryGoodReplies[hash].push_back(dnb);
                        }
                    }
                }
            }
            return true;
        }
        mapSeenServiceNodeBroadcast.insert(std::make_pair(hash, std::make_pair(GetTime(), dnb)));

        LogPrint("servicenode", "CServiceNodeMan::CheckDnbAndUpdateServiceNodeList -- servicenode=%s new\n", dnb.outpoint.ToStringShort());

        if (!dnb.SimpleCheck(nDos)) {
            LogPrint("servicenode", "CServiceNodeMan::CheckDnbAndUpdateServiceNodeList -- SimpleCheck() failed, servicenode=%s\n", dnb.outpoint.ToStringShort());
            return false;
        }

        // search ServiceNode list
        CServiceNode* pdn = Find(dnb.outpoint);
        if (pdn) {
            CServiceNodeBroadcast dnbOld = mapSeenServiceNodeBroadcast[CServiceNodeBroadcast(*pdn).GetHash()].second;
            if (!dnb.Update(pdn, nDos, connman)) {
                LogPrint("servicenode", "CServiceNodeMan::CheckDnbAndUpdateServiceNodeList -- Update() failed, servicenode=%s\n", dnb.outpoint.ToStringShort());
                return false;
            }
            if (hash != dnbOld.GetHash()) {
                mapSeenServiceNodeBroadcast.erase(dnbOld.GetHash());
            }
            return true;
        }
    }

    if (dnb.CheckOutpoint(nDos)) {
        Add(dnb);
        servicenodeSync.BumpAssetLastTime("CServiceNodeMan::CheckDnbAndUpdateServiceNodeList - new");
        // if it matches our ServiceNode privkey...
        if (fServiceNodeMode && dnb.pubKeyServiceNode == activeServiceNode.pubKeyServiceNode) {
            dnb.nPoSeBanScore = -SERVICENODE_POSE_BAN_MAX_SCORE;
            if (dnb.nProtocolVersion == PROTOCOL_VERSION) {
                // ... and PROTOCOL_VERSION, then we've been remotely activated ...
                LogPrintf("CServiceNodeMan::CheckDnbAndUpdateServiceNodeList -- Got NEW ServiceNode entry: servicenode=%s  sigTime=%lld  addr=%s\n",
                    dnb.outpoint.ToStringShort(), dnb.sigTime, dnb.addr.ToString());
                activeServiceNode.ManageState(connman);
            } else {
                // ... otherwise we need to reactivate our node, do not add it to the list and do not relay
                // but also do not ban the node we get this message from
                LogPrintf("CServiceNodeMan::CheckDnbAndUpdateServiceNodeList -- wrong PROTOCOL_VERSION, re-activate your DN: message nProtocolVersion=%d  PROTOCOL_VERSION=%d\n", dnb.nProtocolVersion, PROTOCOL_VERSION);
                return false;
            }
        }
        dnb.Relay(connman);
    } else {
        LogPrintf("CServiceNodeMan::CheckDnbAndUpdateServiceNodeList -- Rejected ServiceNode entry: %s  addr=%s\n", dnb.outpoint.ToStringShort(), dnb.addr.ToString());
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

    for (auto& dnpair : mapServiceNodes) {
        dnpair.second.UpdateLastPaid(pindex, nMaxBlocksToScanBack);
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
    CServiceNode* pdn = Find(outpoint);
    if (!pdn) {
        return false;
    }
    pdn->AddGovernanceVote(nGovernanceObjectHash);
    return true;
}

void CServiceNodeMan::RemoveGovernanceObject(uint256 nGovernanceObjectHash)
{
    LOCK(cs);
    for (auto& dnpair : mapServiceNodes) {
        dnpair.second.RemoveGovernanceObject(nGovernanceObjectHash);
    }
}

void CServiceNodeMan::CheckServiceNode(const CPubKey& pubKeyServiceNode, bool fForce)
{
    LOCK(cs);
    for (auto& dnpair : mapServiceNodes) {
        if (dnpair.second.pubKeyServiceNode == pubKeyServiceNode) {
            dnpair.second.Check(fForce);
            return;
        }
    }
}

bool CServiceNodeMan::IsServiceNodePingedWithin(const COutPoint& outpoint, int nSeconds, int64_t nTimeToCheckAt)
{
    LOCK(cs);
    CServiceNode* pdn = Find(outpoint);
    return pdn ? pdn->IsPingedWithin(nSeconds, nTimeToCheckAt) : false;
}

void CServiceNodeMan::SetServiceNodeLastPing(const COutPoint& outpoint, const CServiceNodePing& dnp)
{
    LOCK(cs);
    CServiceNode* pdn = Find(outpoint);
    if (!pdn) {
        return;
    }
    pdn->lastPing = dnp;
    if (dnp.fSentinelIsCurrent) {
        UpdateLastSentinelPingTime();
    }
    mapSeenServiceNodePing.insert(std::make_pair(dnp.GetHash(), dnp));

    CServiceNodeBroadcast dnb(*pdn);
    uint256 hash = dnb.GetHash();
    if (mapSeenServiceNodeBroadcast.count(hash)) {
        mapSeenServiceNodeBroadcast[hash].second.lastPing = dnp;
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

    for (const auto& dnpair : mapServiceNodes) {
        if (dnpair.second.lastPing.nDaemonVersion > CLIENT_VERSION) {
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
    dnodeman.Check();

    dnodeman.ProcessPendingDnbRequests(connman);
    dnodeman.ProcessPendingDnvRequests(connman);

    if (nTick % 60 == 0) {
        dnodeman.ProcessServiceNodeConnections(connman);
        dnodeman.CheckAndRemove(connman);
        dnodeman.WarnServiceNodeDaemonUpdates();
    }

    if (fServiceNodeMode && (nTick % (60 * 5) == 0)) {
        dnodeman.DoFullVerificationStep(connman);
    }
}
