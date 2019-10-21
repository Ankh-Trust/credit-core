// Copyright (c) 2019 Duality Blockchain Solutions Developers

#ifndef FLUID_SERVICENODE_H
#define FLUID_SERVICENODE_H

#include "amount.h"
#include "dbwrapper.h"
#include "serialize.h"

#include "sync.h"
#include "uint256.h"

class CScript;
class CTransaction;

class CFluidServiceNode
{
public:
    static const int CURRENT_VERSION = 1;
    int nVersion;
    std::vector<unsigned char> FluidScript;
    CAmount ServiceNodeReward;
    int64_t nTimeStamp;
    std::vector<std::vector<unsigned char> > SovereignAddresses;
    uint256 txHash;
    unsigned int nHeight;

    CFluidServiceNode()
    {
        SetNull();
    }

    CFluidServiceNode(const CTransaction& tx)
    {
        SetNull();
        UnserializeFromTx(tx);
    }

    CFluidServiceNode(const CScript& fluidScript)
    {
        SetNull();
        UnserializeFromScript(fluidScript);
    }

    inline void SetNull()
    {
        nVersion = CFluidServiceNode::CURRENT_VERSION;
        FluidScript.clear();
        ServiceNodeReward = -1;
        nTimeStamp = 0;
        SovereignAddresses.clear();
        txHash.SetNull();
        nHeight = 0;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(this->nVersion);
        READWRITE(FluidScript);
        READWRITE(ServiceNodeReward);
        READWRITE(VARINT(nTimeStamp));
        READWRITE(SovereignAddresses);
        READWRITE(txHash);
        READWRITE(VARINT(nHeight));
    }

    inline friend bool operator==(const CFluidServiceNode& a, const CFluidServiceNode& b)
    {
        return (a.FluidScript == b.FluidScript && a.ServiceNodeReward == b.ServiceNodeReward && a.nTimeStamp == b.nTimeStamp);
    }

    inline friend bool operator!=(const CFluidServiceNode& a, const CFluidServiceNode& b)
    {
        return !(a == b);
    }

    friend bool operator<(const CFluidServiceNode& a, const CFluidServiceNode& b)
    {
        return (a.nTimeStamp < b.nTimeStamp);
    }

    friend bool operator>(const CFluidServiceNode& a, const CFluidServiceNode& b)
    {
        return (a.nTimeStamp > b.nTimeStamp);
    }

    inline CFluidServiceNode operator=(const CFluidServiceNode& b)
    {
        FluidScript = b.FluidScript;
        ServiceNodeReward = b.ServiceNodeReward;
        nTimeStamp = b.nTimeStamp;
        SovereignAddresses.clear(); //clear out previous entries
        for (const std::vector<unsigned char>& vchAddress : b.SovereignAddresses) {
            SovereignAddresses.push_back(vchAddress);
        }
        txHash = b.txHash;
        nHeight = b.nHeight;
        return *this;
    }

    inline bool IsNull() const { return (nTimeStamp == 0); }
    bool UnserializeFromTx(const CTransaction& tx);
    bool UnserializeFromScript(const CScript& fluidScript);
    void Serialize(std::vector<unsigned char>& vchData);
};

static CCriticalSection cs_fluid_servicenode;

class CFluidServiceNodeDB : public CDBWrapper
{
public:
    CFluidServiceNodeDB(size_t nCacheSize, bool fMemory, bool fWipe, bool obfuscate);
    bool AddFluidServiceNodeEntry(const CFluidServiceNode& entry, const int op);
    bool GetLastFluidServiceNodeRecord(CFluidServiceNode& returnEntry, const int nHeight);
    bool GetAllFluidServiceNodeRecords(std::vector<CFluidServiceNode>& entries);
    bool IsEmpty();
    bool RecordExists(const std::vector<unsigned char>& vchFluidScript);
};

bool GetFluidServiceNodeData(const CScript& scriptPubKey, CFluidServiceNode& entry);
bool GetFluidServiceNodeData(const CTransaction& tx, CFluidServiceNode& entry, int& nOut);
bool CheckFluidServiceNodeDB();

extern CFluidServiceNodeDB* pFluidServiceNodeDB;

#endif // FLUID_SERVICENODE_H
