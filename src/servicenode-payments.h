// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2017 The Dash Core Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CREDIT_SERVICENODE_PAYMENTS_H
#define CREDIT_SERVICENODE_PAYMENTS_H

#include "core_io.h"
#include "servicenode.h"
#include "key.h"
#include "net_processing.h"
#include "util.h"
#include "utilstrencodings.h"

class CServiceNodeBlockPayees;
class CServiceNodePayments;
class CServiceNodePaymentVote;

static const int SNPAYMENTS_SIGNATURES_REQUIRED = 10;
static const int SNPAYMENTS_SIGNATURES_TOTAL = 20;

//! minimum peer version that can receive and send servicenode payment messages,
//  vote for servicenode and be elected as a payment winner
// V1 - Last protocol version before update
// V2 - Newest protocol version
static const int MIN_SERVICENODE_PAYMENT_PROTO_VERSION_1 = 71100;
static const int MIN_SERVICENODE_PAYMENT_PROTO_VERSION_2 = 71110;  // Only Service Nodes >= v1.1.0.0 will get paid after Spork 10 activation

extern CCriticalSection cs_vecPayees;
extern CCriticalSection cs_mapServiceNodeBlocks;
extern CCriticalSection cs_mapServiceNodePayeeVotes;

extern CServiceNodePayments snpayments;

/// TODO: all 4 functions do not belong here really, they should be refactored/moved somewhere (main.cpp ?)
bool IsBlockValueValid(const CBlock& block, int nBlockHeight, CAmount blockReward, std::string& strErrorRet);
bool IsBlockPayeeValid(const CTransaction& txNew, int nBlockHeight, CAmount blockReward);
void FillBlockPayments(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutServiceNodeRet, std::vector<CTxOut>& voutSuperblockRet);
std::string GetRequiredPaymentsString(int nBlockHeight);

class CServiceNodePayee
{
private:
    CScript scriptPubKey;
    std::vector<uint256> vecVoteHashes;

public:
    CServiceNodePayee() : scriptPubKey(),
                     vecVoteHashes()
    {
    }

    CServiceNodePayee(CScript payee, uint256 hashIn) : scriptPubKey(payee),
                                                  vecVoteHashes()
    {
        vecVoteHashes.push_back(hashIn);
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(*(CScriptBase*)(&scriptPubKey));
        READWRITE(vecVoteHashes);
    }

    CScript GetPayee() const { return scriptPubKey; }

    void AddVoteHash(uint256 hashIn) { vecVoteHashes.push_back(hashIn); }
    std::vector<uint256> GetVoteHashes() const { return vecVoteHashes; }
    int GetVoteCount() const { return vecVoteHashes.size(); }
};

// Keep track of votes for payees from ServiceNodes
class CServiceNodeBlockPayees
{
public:
    int nBlockHeight;
    std::vector<CServiceNodePayee> vecPayees;

    CServiceNodeBlockPayees() : nBlockHeight(0),
                           vecPayees()
    {
    }
    CServiceNodeBlockPayees(int nBlockHeightIn) : nBlockHeight(nBlockHeightIn),
                                             vecPayees()
    {
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(nBlockHeight);
        READWRITE(vecPayees);
    }

    void AddPayee(const CServiceNodePaymentVote& vote);
    bool GetBestPayee(CScript& payeeRet) const;
    bool HasPayeeWithVotes(const CScript& payeeIn, int nVotesReq) const;

    bool IsTransactionValid(const CTransaction& txNew, int nHeight) const;

    std::string GetRequiredPaymentsString() const;
};

// vote for the winning payment
class CServiceNodePaymentVote
{
public:
    COutPoint servicenodeOutpoint;

    int nBlockHeight;
    CScript payee;
    std::vector<unsigned char> vchSig;

    CServiceNodePaymentVote() : servicenodeOutpoint(),
                           nBlockHeight(0),
                           payee(),
                           vchSig()
    {
    }

    CServiceNodePaymentVote(COutPoint outpoint, int nBlockHeight, CScript payee) : servicenodeOutpoint(outpoint),
                                                                              nBlockHeight(nBlockHeight),
                                                                              payee(payee),
                                                                              vchSig()
    {
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        int nVersion = s.GetVersion();
        if (nVersion == 70900 && (s.GetType() & SER_NETWORK)) {
            // converting from/to old format
            CTxIn vinServiceNode{};
            if (ser_action.ForRead()) {
                READWRITE(vinServiceNode);
                servicenodeOutpoint = vinServiceNode.prevout;
            } else {
                vinServiceNode = CTxIn(servicenodeOutpoint);
                READWRITE(vinServiceNode);
            }
        } else {
            // using new format directly
            READWRITE(servicenodeOutpoint);
        }
        READWRITE(nBlockHeight);
        READWRITE(*(CScriptBase*)(&payee));
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(vchSig);
        }
    }

    uint256 GetHash() const;
    uint256 GetSignatureHash() const;

    bool Sign();
    bool CheckSignature(const CPubKey& pubKeyServiceNode, int nValidationHeight, int& nDos) const;

    bool IsValid(CNode* pnode, int nValidationHeight, std::string& strError, CConnman& connman) const;
    void Relay(CConnman& connman) const;

    bool IsVerified() const { return !vchSig.empty(); }
    void MarkAsNotVerified() { vchSig.clear(); }

    std::string ToString() const;
};

//
// ServiceNode Payments Class
// Keeps track of who should get paid for which blocks
//

class CServiceNodePayments
{
private:
    // ServiceNode count times nStorageCoeff payments blocks should be stored ...
    const float nStorageCoeff;
    // ... but at least nMinBlocksToStore (payments blocks)
    const int nMinBlocksToStore;

    // Keep track of current block height
    int nCachedBlockHeight;

public:
    std::map<uint256, CServiceNodePaymentVote> mapServiceNodePaymentVotes;
    std::map<int, CServiceNodeBlockPayees> mapServiceNodeBlocks;
    std::map<COutPoint, int> mapServiceNodesLastVote;
    std::map<COutPoint, int> mapServiceNodesDidNotVote;

    CServiceNodePayments() : nStorageCoeff(1.25), nMinBlocksToStore(5000) {}

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(mapServiceNodePaymentVotes);
        READWRITE(mapServiceNodeBlocks);
    }

    void Clear();

    bool AddOrUpdatePaymentVote(const CServiceNodePaymentVote& vote);
    bool HasVerifiedPaymentVote(const uint256& hashIn) const;
    bool ProcessBlock(int nBlockHeight, CConnman& connman);
    void CheckBlockVotes(int nBlockHeight);

    void Sync(CNode* node, CConnman& connman) const;
    void RequestLowDataPaymentBlocks(CNode* pnode, CConnman& connman) const;
    void CheckAndRemove();

    bool GetBlockPayee(int nBlockHeight, CScript& payeeRet) const;
    bool IsTransactionValid(const CTransaction& txNew, int nBlockHeight) const;
    bool IsScheduled(const servicenode_info_t& dnInfo, int nNotBlockHeight) const;

    bool UpdateLastVote(const CServiceNodePaymentVote& vote);

    int GetMinServiceNodePaymentsProto() const;
    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman);
    std::string GetRequiredPaymentsString(int nBlockHeight) const;
    void FillBlockPayee(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutServiceNodeRet) const;
    std::string ToString() const;

    int GetBlockCount() const { return mapServiceNodeBlocks.size(); }
    int GetVoteCount() const { return mapServiceNodePaymentVotes.size(); }

    bool IsEnoughData() const;
    int GetStorageLimit() const;

    void UpdatedBlockTip(const CBlockIndex* pindex, CConnman& connman);

    void DoMaintenance();
};

#endif // CREDIT_SERVICENODE_PAYMENTS_H
