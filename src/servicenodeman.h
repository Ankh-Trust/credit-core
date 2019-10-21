// Copyright (c) 2019-2019 The Ankh Core Developers
// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2017 The Dash Core Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CREDIT_SERVICENODEMAN_H
#define CREDIT_SERVICENODEMAN_H

#include "servicenode.h"
#include "sync.h"

class CServiceNodeMan;
class CConnman;

extern CServiceNodeMan dnodeman;

class CServiceNodeMan
{
public:
    typedef std::pair<arith_uint256, const CServiceNode*> score_pair_t;
    typedef std::vector<score_pair_t> score_pair_vec_t;
    typedef std::pair<int, const CServiceNode> rank_pair_t;
    typedef std::vector<rank_pair_t> rank_pair_vec_t;

private:
    static const std::string SERIALIZATION_VERSION_STRING;

    static const int PSEG_UPDATE_SECONDS = 3 * 60 * 60;

    static const int LAST_PAID_SCAN_BLOCKS;

    static const int MIN_POSE_PROTO_VERSION = 70600;
    static const int MAX_POSE_CONNECTIONS = 10;
    static const int MAX_POSE_RANK = 10;
    static const int MAX_POSE_BLOCKS = 10;

    static const int DNB_RECOVERY_QUORUM_TOTAL = 10;
    static const int DNB_RECOVERY_QUORUM_REQUIRED = 10;
    static const int DNB_RECOVERY_MAX_ASK_ENTRIES = 10;
    static const int DNB_RECOVERY_WAIT_SECONDS = 60;
    static const int DNB_RECOVERY_RETRY_SECONDS = 3 * 60 * 60;
    // the minimun active ServiceNodes before using InstandSend
    static const int INSTANTSEND_MIN_ACTIVE_SERVICENODE_COUNT = 25;
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    // Keep track of current block height
    int nCachedBlockHeight;

    // map to hold all DNs
    std::map<COutPoint, CServiceNode> mapServiceNodes;
    // who's asked for the ServiceNode list and the last time
    std::map<CService, int64_t> mAskedUsForServiceNodeList;
    // who we asked for the ServiceNode list and the last time
    std::map<CService, int64_t> mWeAskedForServiceNodeList;
    // which ServiceNodes we've asked for
    std::map<COutPoint, std::map<CService, int64_t> > mWeAskedForServiceNodeListEntry;

    // who we asked for the servicenode verification
    std::map<CService, CServiceNodeVerification> mWeAskedForVerification;

    // these maps are used for ServiceNode recovery from SERVICENODE_NEW_START_REQUIRED state
    std::map<uint256, std::pair<int64_t, std::set<CService> > > mDnbRecoveryRequests;
    std::map<uint256, std::vector<CServiceNodeBroadcast> > mDnbRecoveryGoodReplies;
    std::list<std::pair<CService, uint256> > listScheduledDnbRequestConnections;
    std::map<CService, std::pair<int64_t, std::set<uint256> > > mapPendingDNB;
    std::map<CService, std::pair<int64_t, CServiceNodeVerification> > mapPendingDNV;
    CCriticalSection cs_mapPendingDNV;

    /// Set when ServiceNodes are added, cleared when CGovernanceManager is notified
    bool fServiceNodesAdded;

    /// Set when ServiceNodes are removed, cleared when CGovernanceManager is notified
    bool fServiceNodesRemoved;

    std::vector<uint256> vecDirtyGovernanceObjectHashes;

    int64_t nLastSentinelPingTime;

    friend class CServiceNodeSync;
    /// Find an entry
    CServiceNode* Find(const COutPoint& outpoint);

    bool GetServiceNodeScores(const uint256& nBlockHash, score_pair_vec_t& vecServiceNodeScoresRet, int nMinProtocol = 0);

    void SyncSingle(CNode* pnode, const COutPoint& outpoint, CConnman& connman);
    void SyncAll(CNode* pnode, CConnman& connman);

    void PushPsegInvs(CNode* pnode, const CServiceNode& dn);

public:
    // Keep track of all broadcasts I've seen
    std::map<uint256, std::pair<int64_t, CServiceNodeBroadcast> > mapSeenServiceNodeBroadcast;
    // Keep track of all pings I've seen
    std::map<uint256, CServiceNodePing> mapSeenServiceNodePing;
    // Keep track of all verifications I've seen
    std::map<uint256, CServiceNodeVerification> mapSeenServiceNodeVerification;
    // keep track of psq count to prevent ServiceNodes from gaming privatesend queue
    int64_t nPsqCount;


    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        LOCK(cs);
        std::string strVersion;
        if (ser_action.ForRead()) {
            READWRITE(strVersion);
        } else {
            strVersion = SERIALIZATION_VERSION_STRING;
            READWRITE(strVersion);
        }

        READWRITE(mapServiceNodes);
        READWRITE(mAskedUsForServiceNodeList);
        READWRITE(mWeAskedForServiceNodeList);
        READWRITE(mWeAskedForServiceNodeListEntry);
        READWRITE(mDnbRecoveryRequests);
        READWRITE(mDnbRecoveryGoodReplies);
        READWRITE(nLastSentinelPingTime);
        READWRITE(nPsqCount);

        READWRITE(mapSeenServiceNodeBroadcast);
        READWRITE(mapSeenServiceNodePing);
        if (ser_action.ForRead() && (strVersion != SERIALIZATION_VERSION_STRING)) {
            Clear();
        }
    }

    CServiceNodeMan();

    /// Add an entry
    bool Add(CServiceNode& dn);

    /// Ask (source) node for dnb
    void AskForDN(CNode* pnode, const COutPoint& outpoint, CConnman& connman);
    void AskForDnb(CNode* pnode, const uint256& hash);

    bool PoSeBan(const COutPoint& outpoint);
    bool AllowMixing(const COutPoint& outpoint);
    bool DisallowMixing(const COutPoint& outpoint);

    /// Check all ServiceNodes
    void Check();

    /// Check all ServiceNode and remove inactive
    void CheckAndRemove(CConnman& connman);
    /// This is dummy overload to be used for dumping/loading dncache.dat
    void CheckAndRemove() {}

    /// Clear ServiceNode vector
    void Clear();

    /// Count ServiceNodes filtered by nProtocolVersion.
    /// ServiceNode nProtocolVersion should match or be above the one specified in param here.
    int CountServiceNodes(int nProtocolVersion = -1);
    /// Count enabled ServiceNodes filtered by nProtocolVersion.
    /// ServiceNode nProtocolVersion should match or be above the one specified in param here.
    int CountEnabled(int nProtocolVersion = -1);

    /// Count ServiceNodes by network type - NET_IPV4, NET_IPV6, NET_TOR
    // int CountByIP(int nNetworkType);

    void PsegUpdate(CNode* pnode, CConnman& connman);

    /// Versions of Find that are safe to use from outside the class
    bool Get(const COutPoint& outpoint, CServiceNode& servicenodeRet);
    bool Has(const COutPoint& outpoint);

    bool GetServiceNodeInfo(const COutPoint& outpoint, servicenode_info_t& dnInfoRet);
    bool GetServiceNodeInfo(const CPubKey& pubKeyServiceNode, servicenode_info_t& dnInfoRet);
    bool GetServiceNodeInfo(const CScript& payee, servicenode_info_t& dnInfoRet);

    /// Find an entry in the ServiceNode list that is next to be paid
    bool GetNextServiceNodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCountRet, servicenode_info_t& dnInfoRet);
    /// Same as above but use current block height
    bool GetNextServiceNodeInQueueForPayment(bool fFilterSigTime, int& nCountRet, servicenode_info_t& dnInfoRet);

    /// Find a random entry
    servicenode_info_t FindRandomNotInVec(const std::vector<COutPoint>& vecToExclude, int nProtocolVersion = -1);

    std::map<COutPoint, CServiceNode> GetFullServiceNodeMap() { return mapServiceNodes; }

    bool GetServiceNodeRanks(rank_pair_vec_t& vecServiceNodeRanksRet, int nBlockHeight = -1, int nMinProtocol = 0);
    bool GetServiceNodeRank(const COutPoint& outpoint, int& nRankRet, int nBlockHeight = -1, int nMinProtocol = 0);

    void ProcessServiceNodeConnections(CConnman& connman);
    std::pair<CService, std::set<uint256> > PopScheduledDnbRequestConnection();
    void ProcessPendingDnbRequests(CConnman& connman);

    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman);

    void DoFullVerificationStep(CConnman& connman);
    void CheckSameAddr();
    bool SendVerifyRequest(const CAddress& addr, const std::vector<const CServiceNode*>& vSortedByAddr, CConnman& connman);
    void ProcessPendingDnvRequests(CConnman& connman);
    void SendVerifyReply(CNode* pnode, CServiceNodeVerification& dnv, CConnman& connman);
    void ProcessVerifyReply(CNode* pnode, CServiceNodeVerification& dnv);
    void ProcessVerifyBroadcast(CNode* pnode, const CServiceNodeVerification& dnv);

    /// Return the number of (unique) ServiceNodes
    int size() { return mapServiceNodes.size(); }

    std::string ToString() const;

    /// Perform complete check and only then update list and maps
    bool CheckDnbAndUpdateServiceNodeList(CNode* pfrom, CServiceNodeBroadcast dnb, int& nDos, CConnman& connman);
    bool IsDnbRecoveryRequested(const uint256& hash) { return mDnbRecoveryRequests.count(hash); }

    void UpdateLastPaid(const CBlockIndex* pindex);

    void AddDirtyGovernanceObjectHash(const uint256& nHash)
    {
        LOCK(cs);
        vecDirtyGovernanceObjectHashes.push_back(nHash);
    }

    std::vector<uint256> GetAndClearDirtyGovernanceObjectHashes()
    {
        LOCK(cs);
        std::vector<uint256> vecTmp = vecDirtyGovernanceObjectHashes;
        vecDirtyGovernanceObjectHashes.clear();
        return vecTmp;
    }

    bool IsSentinelPingActive();
    void UpdateLastSentinelPingTime();
    bool AddGovernanceVote(const COutPoint& outpoint, uint256 nGovernanceObjectHash);
    void RemoveGovernanceObject(uint256 nGovernanceObjectHash);

    void CheckServiceNode(const CPubKey& pubKeyServiceNode, bool fForce);

    bool IsServiceNodePingedWithin(const COutPoint& outpoint, int nSeconds, int64_t nTimeToCheckAt = -1);
    void SetServiceNodeLastPing(const COutPoint& outpoint, const CServiceNodePing& dnp);

    void UpdatedBlockTip(const CBlockIndex* pindex);

    void WarnServiceNodeDaemonUpdates();

    /**
     * Called to notify CGovernanceManager that the ServiceNode index has been updated.
     * Must be called while not holding the CServiceNodeMan::cs mutex
     */
    void NotifyServiceNodeUpdates(CConnman& connman);

    void DoMaintenance(CConnman& connman);

    bool EnoughActiveForInstandSend() { return (CountEnabled() >= INSTANTSEND_MIN_ACTIVE_SERVICENODE_COUNT); }

};

#endif // CREDIT_SERVICENODEMAN_H
