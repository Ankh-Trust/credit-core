
// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2019 The Dash Core Developers
// Copyright (c) 2009-2019 The Bitcoin Developers
// Copyright (c) 2009-2019 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef CREDIT_ACTIVESERVICENODE_H
#define CREDIT_ACTIVESERVICENODE_H

#include "chainparams.h"
#include "key.h"
#include "net.h"
#include "primitives/transaction.h"

#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif //ENABLE_WALLET

class CActiveServiceNode;

static const int ACTIVE_SERVICENODE_INITIAL = 0; // initial state
static const int ACTIVE_SERVICENODE_SYNC_IN_PROCESS = 1;
static const int ACTIVE_SERVICENODE_INPUT_TOO_NEW = 2;
static const int ACTIVE_SERVICENODE_NOT_CAPABLE = 3;
static const int ACTIVE_SERVICENODE_STARTED = 4;

extern CActiveServiceNode activeServiceNode;

// Responsible for activating the ServiceNode and pinging the network
class CActiveServiceNode
{
public:
    enum servicenode_type_enum_t {
        SERVICENODE_UNKNOWN = 0,
        SERVICENODE_REMOTE = 1
    };

private:
    // critical section to protect the inner data structures
    mutable CCriticalSection cs;

    servicenode_type_enum_t eType;

    bool fPingerEnabled;

    /// Ping ServiceNode
    bool SendServiceNodePing(CConnman& connman);

    //  sentinel ping data
    int64_t nSentinelPingTime;
    uint32_t nSentinelVersion;

public:
    // Keys for the active ServiceNode
    CPubKey pubKeyServiceNode;
    CKey keyServiceNode;

    // Initialized while registering ServiceNode
    COutPoint outpoint;
    CService service;

    int nState; // should be one of ACTIVE_SERVICENODE_XXXX
    std::string strNotCapableReason;

    CActiveServiceNode()
        : eType(SERVICENODE_UNKNOWN),
          fPingerEnabled(false),
          pubKeyServiceNode(),
          keyServiceNode(),
          outpoint(),
          service(),
          nState(ACTIVE_SERVICENODE_INITIAL)
    {
    }

    /// Manage state of active ServiceNode
    void ManageState(CConnman& connman);

    std::string GetStateString() const;
    std::string GetStatus() const;
    std::string GetTypeString() const;

    bool UpdateSentinelPing(int version);

    void DoMaintenance(CConnman &connman);

private:
    void ManageStateInitial(CConnman& connman);
    void ManageStateRemote();
};

#endif // CREDIT_ACTIVESERVICENODE_H
