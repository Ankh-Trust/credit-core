
// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2017 The Dash Core Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CREDIT_SERVICENODE_H
#define CREDIT_SERVICENODE_H

#include "key.h"
#include "spork.h"
#include "validation.h"

class CServiceNode;
class CServiceNodeBroadcast;

static const int SERVICENODE_CHECK_SECONDS = 5;
static const int SERVICENODE_MIN_DNB_SECONDS = 5 * 60;
static const int SERVICENODE_MIN_DNP_SECONDS = 10 * 60;
static const int SERVICENODE_EXPIRATION_SECONDS = 65 * 60;
static const int SERVICENODE_SENTINEL_PING_MAX_SECONDS = 120 * 60;
static const int SERVICENODE_NEW_START_REQUIRED_SECONDS = 180 * 60;

static const int SERVICENODE_POSE_BAN_MAX_SCORE = 5;

//
// The ServiceNode Ping Class : Contains a different serialize method for sending pings from ServiceNodes throughout the network
//

// sentinel version before sentinel ping implementation
#define DEFAULT_SENTINEL_VERSION 0x010001
// daemon version before implementation of nDaemonVersion in CServiceNodePing
#define DEFAULT_DAEMON_VERSION 203050

class CServiceNodePing
{
public:
    COutPoint servicenodeOutpoint{};
    uint256 blockHash{};
    int64_t sigTime{}; //dnb message times
    std::vector<unsigned char> vchSig{};
    bool fSentinelIsCurrent = false; // true if last sentinel ping was actual
    // DSB is always 0, other 3 bits corresponds to x.x.x version scheme
    uint32_t nSentinelVersion{DEFAULT_SENTINEL_VERSION};
    uint32_t nDaemonVersion{DEFAULT_DAEMON_VERSION};

    CServiceNodePing() = default;

    CServiceNodePing(const COutPoint& outpoint);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        int nVersion = s.GetVersion();
        if (nVersion == 70900 && (s.GetType() & SER_NETWORK)) {
            // converting from/to old format
            CTxIn txin{};
            if (ser_action.ForRead()) {
                READWRITE(txin);
                servicenodeOutpoint = txin.prevout;
            } else {
                txin = CTxIn(servicenodeOutpoint);
                READWRITE(txin);
            }
        } else {
            // using new format directly
            READWRITE(servicenodeOutpoint);
        }
        READWRITE(blockHash);
        READWRITE(sigTime);
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(vchSig);
        }
        if (ser_action.ForRead() && s.size() == 0) {
            // TODO: drop this after migration to 71000
            fSentinelIsCurrent = false;
            nSentinelVersion = DEFAULT_SENTINEL_VERSION;
            nDaemonVersion = DEFAULT_DAEMON_VERSION;
            return;
        }
        READWRITE(fSentinelIsCurrent);
        READWRITE(nSentinelVersion);
        if (ser_action.ForRead() && s.size() == 0) {
            // TODO: drop this after migration to 71000
            nDaemonVersion = DEFAULT_DAEMON_VERSION;
            return;
        }
        if (!(nVersion == 70900 && (s.GetType() & SER_NETWORK))) {
            READWRITE(nDaemonVersion);
        }
    }

    uint256 GetHash() const;
    uint256 GetSignatureHash() const;

    bool IsExpired() const { return GetAdjustedTime() - sigTime > SERVICENODE_NEW_START_REQUIRED_SECONDS; }

    bool Sign(const CKey& keyServiceNode, const CPubKey& pubKeyServiceNode);
    bool CheckSignature(const CPubKey& pubKeyServiceNode, int& nDos) const;
    bool SimpleCheck(int& nDos);
    bool CheckAndUpdate(CServiceNode* pdn, bool fFromNewBroadcast, int& nDos, CConnman& connman);
    void Relay(CConnman& connman);

    std::string GetSentinelString() const;
    std::string GetDaemonString() const;

    explicit operator bool() const;
};

inline bool operator==(const CServiceNodePing& a, const CServiceNodePing& b)
{
    return a.servicenodeOutpoint == b.servicenodeOutpoint && a.blockHash == b.blockHash;
}
inline bool operator!=(const CServiceNodePing& a, const CServiceNodePing& b)
{
    return !(a == b);
}
inline CServiceNodePing::operator bool() const
{
    return *this != CServiceNodePing();
}

struct servicenode_info_t {
    // Note: all these constructors can be removed once C++14 is enabled.
    // (in C++11 the member initializers wrongly disqualify this as an aggregate)
    servicenode_info_t() = default;
    servicenode_info_t(servicenode_info_t const&) = default;

    servicenode_info_t(int activeState, int protoVer, int64_t sTime) : nActiveState{activeState}, nProtocolVersion{protoVer}, sigTime{sTime} {}

    servicenode_info_t(int activeState, int protoVer, int64_t sTime, COutPoint const& outpnt, CService const& addr, CPubKey const& pkCollAddr, CPubKey const& pkDN) : nActiveState{activeState}, nProtocolVersion{protoVer}, sigTime{sTime},
                                                                                                                                                                 outpoint{outpnt}, addr{addr},
                                                                                                                                                                 pubKeyCollateralAddress{pkCollAddr}, pubKeyServiceNode{pkDN} {}

    int nActiveState = 0;
    int nProtocolVersion = 0;
    int64_t sigTime = 0; //dnb message time

    COutPoint outpoint{};
    CService addr{};
    CPubKey pubKeyCollateralAddress{};
    CPubKey pubKeyServiceNode{};

    int64_t nLastPsq = 0; //the psq count from the last psq broadcast of this node
    int64_t nTimeLastChecked = 0;
    int64_t nTimeLastPaid = 0;
    int64_t nTimeLastPing = 0; //* not in CDN
    bool fInfoValid = false;   //* not in CDN
};

//
// The ServiceNode Class. For managing the PrivateSend process. It contains the input of the 1000DYN, signature to prove
// it's the one who own that ip address and code for calculating the payment election.
//
class CServiceNode : public servicenode_info_t
{
private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

public:
    enum state {
        SERVICENODE_PRE_ENABLED,
        SERVICENODE_ENABLED,
        SERVICENODE_EXPIRED,
        SERVICENODE_OUTPOINT_SPENT,
        SERVICENODE_UPDATE_REQUIRED,
        SERVICENODE_SENTINEL_PING_EXPIRED,
        SERVICENODE_NEW_START_REQUIRED,
        SERVICENODE_POSE_BAN
    };

    enum CollateralStatus {
        COLLATERAL_OK,
        COLLATERAL_UTXO_NOT_FOUND,
        COLLATERAL_INVALID_AMOUNT,
        COLLATERAL_INVALID_PUBKEY
    };


    CServiceNodePing lastPing{};
    std::vector<unsigned char> vchSig{};

    uint256 nCollateralMinConfBlockHash{};
    int nBlockLastPaid{};
    int nPoSeBanScore{};
    int nPoSeBanHeight{};
    bool fAllowMixingTx{};
    bool fUnitTest = false;

    // KEEP TRACK OF GOVERNANCE ITEMS EACH SERVICENODE HAS VOTE UPON FOR RECALCULATION
    std::map<uint256, int> mapGovernanceObjectsVotedOn;

    CServiceNode();
    CServiceNode(const CServiceNode& other);
    CServiceNode(const CServiceNodeBroadcast& dnb);
    CServiceNode(CService addrNew, COutPoint outpointNew, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyServiceNodeNew, int nProtocolVersionIn);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        LOCK(cs);
        int nVersion = s.GetVersion();
        if (nVersion == 70900 && (s.GetType() & SER_NETWORK)) {
            // converting from/to old format
            CTxIn txin{};
            if (ser_action.ForRead()) {
                READWRITE(txin);
                outpoint = txin.prevout;
            } else {
                txin = CTxIn(outpoint);
                READWRITE(txin);
            }
        } else {
            // using new format directly
            READWRITE(outpoint);
        }
        READWRITE(addr);
        READWRITE(pubKeyCollateralAddress);
        READWRITE(pubKeyServiceNode);
        READWRITE(lastPing);
        READWRITE(vchSig);
        READWRITE(sigTime);
        READWRITE(nLastPsq);
        READWRITE(nTimeLastChecked);
        READWRITE(nTimeLastPaid);
        READWRITE(nActiveState);
        READWRITE(nCollateralMinConfBlockHash);
        READWRITE(nBlockLastPaid);
        READWRITE(nProtocolVersion);
        READWRITE(nPoSeBanScore);
        READWRITE(nPoSeBanHeight);
        READWRITE(fAllowMixingTx);
        READWRITE(fUnitTest);
        READWRITE(mapGovernanceObjectsVotedOn);
    }

    // CALCULATE A RANK AGAINST OF GIVEN BLOCK
    arith_uint256 CalculateScore(const uint256& blockHash) const;

    bool UpdateFromNewBroadcast(CServiceNodeBroadcast& dnb, CConnman& connman);

    static CollateralStatus CheckCollateral(const COutPoint& outpoint, const CPubKey& pubkey);
    static CollateralStatus CheckCollateral(const COutPoint& outpoint, const CPubKey& pubkey, int& nHeightRet);
    void Check(bool fForce = false);

    bool IsBroadcastedWithin(int nSeconds) { return GetAdjustedTime() - sigTime < nSeconds; }

    bool IsPingedWithin(int nSeconds, int64_t nTimeToCheckAt = -1)
    {
        if (!lastPing)
            return false;

        if (nTimeToCheckAt == -1) {
            nTimeToCheckAt = GetAdjustedTime();
        }
        return nTimeToCheckAt - lastPing.sigTime < nSeconds;
    }

    bool IsEnabled() const { return nActiveState == SERVICENODE_ENABLED; }
    bool IsPreEnabled() const { return nActiveState == SERVICENODE_PRE_ENABLED; }
    bool IsPoSeBanned() const { return nActiveState == SERVICENODE_POSE_BAN; }
    // NOTE: this one relies on nPoSeBanScore, not on nActiveState as everything else here
    bool IsPoSeVerified() const { return nPoSeBanScore <= -SERVICENODE_POSE_BAN_MAX_SCORE; }
    bool IsExpired() const { return nActiveState == SERVICENODE_EXPIRED; }
    bool IsOutpointSpent() const { return nActiveState == SERVICENODE_OUTPOINT_SPENT; }
    bool IsUpdateRequired() const { return nActiveState == SERVICENODE_UPDATE_REQUIRED; }
    bool IsSentinelPingExpired() const { return nActiveState == SERVICENODE_SENTINEL_PING_EXPIRED; }
    bool IsNewStartRequired() const { return nActiveState == SERVICENODE_NEW_START_REQUIRED; }

    static bool IsValidStateForAutoStart(int nActiveStateIn)
    {
        return nActiveStateIn == SERVICENODE_ENABLED ||
               nActiveStateIn == SERVICENODE_PRE_ENABLED ||
               nActiveStateIn == SERVICENODE_EXPIRED ||
               nActiveStateIn == SERVICENODE_SENTINEL_PING_EXPIRED;
    }

    bool IsValidForPayment() const
    {
        if (nActiveState == SERVICENODE_ENABLED) {
            return true;
        }
        if (!sporkManager.IsSporkActive(SPORK_14_REQUIRE_SENTINEL_FLAG) &&
            (nActiveState == SERVICENODE_SENTINEL_PING_EXPIRED)) {
            return true;
        }

        return false;
    }

    bool IsValidNetAddr();
    static bool IsValidNetAddr(CService addrIn);

    void IncreasePoSeBanScore()
    {
        if (nPoSeBanScore < SERVICENODE_POSE_BAN_MAX_SCORE)
            nPoSeBanScore++;
    }
    void DecreasePoSeBanScore()
    {
        if (nPoSeBanScore > -SERVICENODE_POSE_BAN_MAX_SCORE)
            nPoSeBanScore--;
    }
    void PoSeBan() { nPoSeBanScore = SERVICENODE_POSE_BAN_MAX_SCORE; }

    servicenode_info_t GetInfo() const;

    static std::string StateToString(int nStateIn);
    std::string GetStateString() const;
    std::string GetStatus() const;

    int GetCollateralAge();

    int GetLastPaidTime() const { return nTimeLastPaid; }
    int GetLastPaidBlock() const { return nBlockLastPaid; }
    void UpdateLastPaid(const CBlockIndex* pindex, int nMaxBlocksToScanBack);

    // KEEP TRACK OF EACH GOVERNANCE ITEM INCASE THIS NODE GOES OFFLINE, SO WE CAN RECALC THEIR STATUS
    void AddGovernanceVote(uint256 nGovernanceObjectHash);
    // RECALCULATE CACHED STATUS FLAGS FOR ALL AFFECTED OBJECTS
    void FlagGovernanceItemsAsDirty();

    void RemoveGovernanceObject(uint256 nGovernanceObjectHash);

    CServiceNode& operator=(CServiceNode const& from)
    {
        static_cast<servicenode_info_t&>(*this) = from;
        lastPing = from.lastPing;
        vchSig = from.vchSig;
        nCollateralMinConfBlockHash = from.nCollateralMinConfBlockHash;
        nBlockLastPaid = from.nBlockLastPaid;
        nPoSeBanScore = from.nPoSeBanScore;
        nPoSeBanHeight = from.nPoSeBanHeight;
        fAllowMixingTx = from.fAllowMixingTx;
        fUnitTest = from.fUnitTest;
        mapGovernanceObjectsVotedOn = from.mapGovernanceObjectsVotedOn;
        return *this;
    }
};

inline bool operator==(const CServiceNode& a, const CServiceNode& b)
{
    return a.outpoint == b.outpoint;
}
inline bool operator!=(const CServiceNode& a, const CServiceNode& b)
{
    return !(a.outpoint == b.outpoint);
}

//
// The ServiceNode Broadcast Class : Contains a different serialize method for sending ServiceNodes through the network
//

class CServiceNodeBroadcast : public CServiceNode
{
public:
    bool fRecovery;
    CServiceNodeBroadcast() : CServiceNode(), fRecovery(false) {}
    CServiceNodeBroadcast(const CServiceNode& dn) : CServiceNode(dn), fRecovery(false) {}
    CServiceNodeBroadcast(CService addrNew, COutPoint outpointNew, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyServiceNodeNew, int nProtocolVersionIn) : CServiceNode(addrNew, outpointNew, pubKeyCollateralAddressNew, pubKeyServiceNodeNew, nProtocolVersionIn), fRecovery(false) {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        int nVersion = s.GetVersion();
        if (nVersion == 70900 && (s.GetType() & SER_NETWORK)) {
            // converting from/to old format
            CTxIn txin{};
            if (ser_action.ForRead()) {
                READWRITE(txin);
                outpoint = txin.prevout;
            } else {
                txin = CTxIn(outpoint);
                READWRITE(txin);
            }
        } else {
            // using new format directly
            READWRITE(outpoint);
        }
        READWRITE(addr);
        READWRITE(pubKeyCollateralAddress);
        READWRITE(pubKeyServiceNode);
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(vchSig);
        }
        READWRITE(sigTime);
        READWRITE(nProtocolVersion);
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(lastPing);
        }
    }

    uint256 GetHash() const;
    uint256 GetSignatureHash() const;

    /// Create ServiceNode broadcast, needs to be relayed manually after that
    static bool Create(const COutPoint& outpoint, const CService& service, const CKey& keyCollateralAddressNew, const CPubKey& pubKeyCollateralAddressNew, const CKey& keyServiceNodeNew, const CPubKey& pubKeyServiceNodeNew, std::string& strErrorRet, CServiceNodeBroadcast& dnbRet);
    static bool Create(const std::string strService, const std::string strKey, const std::string strTxHash, const std::string strOutputIndex, std::string& strErrorRet, CServiceNodeBroadcast& dnbRet, bool fOffline = false);

    bool SimpleCheck(int& nDos);
    bool Update(CServiceNode* pdn, int& nDos, CConnman& connman);
    bool CheckOutpoint(int& nDos);

    bool Sign(const CKey& keyCollateralAddress);
    bool CheckSignature(int& nDos) const;
    void Relay(CConnman& connman) const;
};

class CServiceNodeVerification
{
public:
    COutPoint servicenodeOutpoint1{};
    COutPoint servicenodeOutpoint2{};
    CService addr{};
    int nonce{};
    int nBlockHeight{};
    std::vector<unsigned char> vchSig1{};
    std::vector<unsigned char> vchSig2{};

    CServiceNodeVerification() = default;

    CServiceNodeVerification(CService addr, int nonce, int nBlockHeight) : addr(addr),
                                                                      nonce(nonce),
                                                                      nBlockHeight(nBlockHeight)
    {
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        int nVersion = s.GetVersion();
        if (nVersion == 70900 && (s.GetType() & SER_NETWORK)) {
            // converting from/to old format
            CTxIn txin1{};
            CTxIn txin2{};
            if (ser_action.ForRead()) {
                READWRITE(txin1);
                READWRITE(txin2);
                servicenodeOutpoint1 = txin1.prevout;
                servicenodeOutpoint2 = txin2.prevout;
            } else {
                txin1 = CTxIn(servicenodeOutpoint1);
                txin2 = CTxIn(servicenodeOutpoint2);
                READWRITE(txin1);
                READWRITE(txin2);
            }
        } else {
            // using new format directly
            READWRITE(servicenodeOutpoint1);
            READWRITE(servicenodeOutpoint2);
        }
        READWRITE(addr);
        READWRITE(nonce);
        READWRITE(nBlockHeight);
        READWRITE(vchSig1);
        READWRITE(vchSig2);
    }

    uint256 GetHash() const
    {
        // Note: doesn't match serialization

        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        // adding dummy values here to match old hashing format
        ss << servicenodeOutpoint1 << uint8_t{} << 0xffffffff;
        ss << servicenodeOutpoint2 << uint8_t{} << 0xffffffff;
        ss << addr;
        ss << nonce;
        ss << nBlockHeight;
        return ss.GetHash();
    }

    uint256 GetSignatureHash1(const uint256& blockHash) const
    {
        // Note: doesn't match serialization

        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << addr;
        ss << nonce;
        ss << blockHash;
        return ss.GetHash();
    }

    uint256 GetSignatureHash2(const uint256& blockHash) const
    {
        // Note: doesn't match serialization

        CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
        ss << servicenodeOutpoint1;
        ss << servicenodeOutpoint2;
        ss << addr;
        ss << nonce;
        ss << blockHash;
        return ss.GetHash();
    }

    void Relay() const
    {
        CInv inv(MSG_SERVICENODE_VERIFY, GetHash());
        g_connman->RelayInv(inv);
    }
};

#endif // CREDIT_SERVICENODE_H
