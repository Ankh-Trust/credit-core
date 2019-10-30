
// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2019 The Dash Core Developers
// Copyright (c) 2009-2019 The Bitcoin Developers
// Copyright (c) 2009-2019 Satoshi Nakamoto
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeservicenode.h"

#include "servicenode-sync.h"
#include "servicenode.h"
#include "servicenodeman.h"
#include "init.h"
#include "netbase.h"
#include "protocol.h"

#ifdef ENABLE_WALLET
extern CWallet* pwalletMain;
#endif //ENABLE_WALLET

// Keep track of the active ServiceNode
CActiveServiceNode activeServiceNode;

void CActiveServiceNode::DoMaintenance(CConnman &connman)
{
    if (ShutdownRequested()) return;
     ManageState(connman);
}

void CActiveServiceNode::ManageState(CConnman& connman)
{
    LogPrint("servicenode", "CActiveServiceNode::ManageState -- Start\n");
    if (!fServiceNodeMode) {
        LogPrint("servicenode", "CActiveServiceNode::ManageState -- Not a ServiceNode, returning\n");
        return;
    }

    if (Params().NetworkIDString() != CBaseChainParams::REGTEST && !servicenodeSync.IsBlockchainSynced()) {
        nState = ACTIVE_SERVICENODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveServiceNode::ManageState -- %s: %s\n", GetStateString(), GetStatus());
        return;
    }

    if (nState == ACTIVE_SERVICENODE_SYNC_IN_PROCESS) {
        nState = ACTIVE_SERVICENODE_INITIAL;
    }

    LogPrint("servicenode", "CActiveServiceNode::ManageState -- status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);

    if (eType == SERVICENODE_UNKNOWN) {
        ManageStateInitial(connman);
    }

    if (eType == SERVICENODE_REMOTE) {
        ManageStateRemote();
    }

    SendServiceNodePing(connman);
}

std::string CActiveServiceNode::GetStateString() const
{
    switch (nState) {
    case ACTIVE_SERVICENODE_INITIAL:
        return "INITIAL";
    case ACTIVE_SERVICENODE_SYNC_IN_PROCESS:
        return "SYNC_IN_PROCESS";
    case ACTIVE_SERVICENODE_INPUT_TOO_NEW:
        return "INPUT_TOO_NEW";
    case ACTIVE_SERVICENODE_NOT_CAPABLE:
        return "NOT_CAPABLE";
    case ACTIVE_SERVICENODE_STARTED:
        return "STARTED";
    default:
        return "UNKNOWN";
    }
}

std::string CActiveServiceNode::GetStatus() const
{
    switch (nState) {
    case ACTIVE_SERVICENODE_INITIAL:
        return "Node just started, not yet activated";
    case ACTIVE_SERVICENODE_SYNC_IN_PROCESS:
        return "Sync in progress. Must wait until sync is complete to start ServiceNode";
    case ACTIVE_SERVICENODE_INPUT_TOO_NEW:
        return strprintf("ServiceNode input must have at least %d confirmations", Params().GetConsensus().nServiceNodeMinimumConfirmations);
    case ACTIVE_SERVICENODE_NOT_CAPABLE:
        return "Not capable ServiceNode: " + strNotCapableReason;
    case ACTIVE_SERVICENODE_STARTED:
        return "ServiceNode successfully started";
    default:
        return "Unknown";
    }
}

std::string CActiveServiceNode::GetTypeString() const
{
    std::string strType;
    switch (eType) {
    case SERVICENODE_REMOTE:
        strType = "REMOTE";
        break;
    default:
        strType = "UNKNOWN";
        break;
    }
    return strType;
}

bool CActiveServiceNode::SendServiceNodePing(CConnman& connman)
{
    if (!fPingerEnabled) {
        LogPrint("servicenode", "CActiveServiceNode::SendServiceNodePing -- %s: ServiceNode ping service is disabled, skipping...\n", GetStateString());
        return false;
    }

    if (!dnodeman.Has(outpoint)) {
        strNotCapableReason = "ServiceNode not in ServiceNode list";
        nState = ACTIVE_SERVICENODE_NOT_CAPABLE;
        LogPrintf("CActiveServiceNode::SendServiceNodePing -- %s: %s\n", GetStateString(), strNotCapableReason);
        return false;
    }

    CServiceNodePing dnp(outpoint);
    dnp.nSentinelVersion = nSentinelVersion;
    dnp.fSentinelIsCurrent =
        (llabs(GetAdjustedTime() - nSentinelPingTime) < SERVICENODE_SENTINEL_PING_MAX_SECONDS);
    if (!dnp.Sign(keyServiceNode, pubKeyServiceNode)) {
        LogPrintf("CActiveServiceNode::SendServiceNodePing -- ERROR: Couldn't sign ServiceNode Ping\n");
        return false;
    }

    // Update lastPing for our ServiceNode in ServiceNode list
    if (dnodeman.IsServiceNodePingedWithin(outpoint, SERVICENODE_MIN_DNP_SECONDS, dnp.sigTime)) {
        LogPrintf("CActiveServiceNode::SendServiceNodePing -- Too early to send ServiceNode Ping\n");
        return false;
    }

    dnodeman.SetServiceNodeLastPing(outpoint, dnp);

    LogPrintf("CActiveServiceNode::SendServiceNodePing -- Relaying ping, collateral=%s\n", outpoint.ToStringShort());
    dnp.Relay(connman);

    return true;
}

bool CActiveServiceNode::UpdateSentinelPing(int version)
{
    nSentinelVersion = version;
    nSentinelPingTime = GetAdjustedTime();

    return true;
}

void CActiveServiceNode::ManageStateInitial(CConnman& connman)
{
    LogPrint("servicenode", "CActiveServiceNode::ManageStateInitial -- status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);
    // Check that our local network configuration is correct
    if (!fListen) {
        // listen option is probably overwritten by smth else, no good
        nState = ACTIVE_SERVICENODE_NOT_CAPABLE;
        strNotCapableReason = "ServiceNode must accept connections from outside. Make sure listen configuration option is not overwritten by some another parameter.";
        LogPrintf("CActiveServiceNode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // First try to find whatever local address is specified by externalip option
    bool fFoundLocal = GetLocal(service) && CServiceNode::IsValidNetAddr(service);
    if (!fFoundLocal) {
        bool empty = true;
        // If we have some peers, let's try to find our local address from one of them
        connman.ForEachNodeContinueIf(CConnman::AllNodes, [&fFoundLocal, &empty, this](CNode* pnode) {
            empty = false;
            if (pnode->addr.IsIPv4())
                fFoundLocal = GetLocal(service, &pnode->addr) && CServiceNode::IsValidNetAddr(service);
            return !fFoundLocal;
        });
        // nothing and no live connections, can't do anything for now
        if (empty) {
            nState = ACTIVE_SERVICENODE_NOT_CAPABLE;
            strNotCapableReason = "Can't detect valid external address. Will retry when there are some connections available.";
            LogPrintf("CActiveServiceNode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    }

    if (!fFoundLocal) {
        nState = ACTIVE_SERVICENODE_NOT_CAPABLE;
        strNotCapableReason = "Can't detect valid external address. Please consider using the externalip configuration option if problem persists. Make sure to use IPv4 address only.";
        LogPrintf("CActiveServiceNode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    int mainnetDefaultPort = Params(CBaseChainParams::MAIN).GetDefaultPort();

    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (service.GetPort() != mainnetDefaultPort) {
            nState = ACTIVE_SERVICENODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("Invalid port: %u - only %d is supported on mainnet.", service.GetPort(), mainnetDefaultPort);
            LogPrintf("CActiveServiceNode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
    } else if (Params().NetworkIDString() != CBaseChainParams::MAIN && service.GetPort() == mainnetDefaultPort) {
        nState = ACTIVE_SERVICENODE_NOT_CAPABLE;
        strNotCapableReason = strprintf("Invalid port: %u - %d is only supported on mainnet.", service.GetPort(), mainnetDefaultPort);
        LogPrintf("CActiveServiceNode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // Check socket connectivity
    LogPrintf("CActiveServiceNode::ManageStateInitial -- Checking inbound connection to '%s'\n", service.ToString());
    SOCKET hSocket;
    bool fConnected = ConnectSocket(service, hSocket, nConnectTimeout) && IsSelectableSocket(hSocket);
    CloseSocket(hSocket);

    if (!fConnected) {
        nState = ACTIVE_SERVICENODE_NOT_CAPABLE;
        strNotCapableReason = "Could not connect to " + service.ToString();
        LogPrintf("CActiveServiceNode::ManageStateInitial -- %s: %s\n", GetStateString(), strNotCapableReason);
        return;
    }

    // Default to REMOTE
    eType = SERVICENODE_REMOTE;

    LogPrint("servicenode", "CActiveServiceNode::ManageStateInitial -- End status = %s, type = %s, pinger enabled = %d\n", GetStatus(), GetTypeString(), fPingerEnabled);
}

void CActiveServiceNode::ManageStateRemote()
{
    LogPrint("servicenode", "CActiveServiceNode::ManageStateRemote -- Start status = %s, type = %s, pinger enabled = %d, pubKeyServiceNode.GetID() = %s\n",
        GetStatus(), fPingerEnabled, GetTypeString(), pubKeyServiceNode.GetID().ToString());

    dnodeman.CheckServiceNode(pubKeyServiceNode, true);
    servicenode_info_t infoDn;
    if (dnodeman.GetServiceNodeInfo(pubKeyServiceNode, infoDn)) {
        if (infoDn.nProtocolVersion != PROTOCOL_VERSION) {
            nState = ACTIVE_SERVICENODE_NOT_CAPABLE;
            strNotCapableReason = "Invalid protocol version";
            LogPrintf("CActiveServiceNode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (service != infoDn.addr) {
            nState = ACTIVE_SERVICENODE_NOT_CAPABLE;
            strNotCapableReason = "Broadcasted IP doesn't match our external address. Make sure you issued a new broadcast if IP of this ServiceNode changed recently.";
            LogPrintf("CActiveServiceNode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (!CServiceNode::IsValidStateForAutoStart(infoDn.nActiveState)) {
            nState = ACTIVE_SERVICENODE_NOT_CAPABLE;
            strNotCapableReason = strprintf("ServiceNode in %s state", CServiceNode::StateToString(infoDn.nActiveState));
            LogPrintf("CActiveServiceNode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
            return;
        }
        if (nState != ACTIVE_SERVICENODE_STARTED) {
            LogPrintf("CActiveServiceNode::ManageStateRemote -- STARTED!\n");
            outpoint = infoDn.outpoint;
            service = infoDn.addr;
            fPingerEnabled = true;
            nState = ACTIVE_SERVICENODE_STARTED;
        }
    } else {
        nState = ACTIVE_SERVICENODE_NOT_CAPABLE;
        strNotCapableReason = "ServiceNode not in ServiceNode list";
        LogPrintf("CActiveServiceNode::ManageStateRemote -- %s: %s\n", GetStateString(), strNotCapableReason);
    }
}
