// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2017 The Dash Core Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activeservicenode.h"
#include "base58.h"
#include "clientversion.h"
#include "servicenode-payments.h"
#include "servicenode-sync.h"
#include "servicenodeconfig.h"
#include "servicenodeman.h"
#include "init.h"
#include "netbase.h"
#include "validation.h"
#ifdef ENABLE_WALLET
#include "privatesend-client.h"
#endif // ENABLE_WALLET
#include "privatesend-server.h"
#include "rpc/server.h"
#include "util.h"
#include "utilmoneystr.h"

#include <univalue.h>

#include <fstream>
#include <iomanip>

UniValue servicenodelist(const JSONRPCRequest& request);

bool EnsureWalletIsAvailable(bool avoidException);

#ifdef ENABLE_WALLET
void EnsureWalletIsUnlocked();

UniValue privatesend(const JSONRPCRequest& request)
{
    if (!EnsureWalletIsAvailable(request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "privatesend \"command\"\n"
            "\nArguments:\n"
            "1. \"command\"        (string or set of strings, required) The command to execute\n"
            "\nAvailable commands:\n"
            "  start       - Start mixing\n"
            "  stop        - Stop mixing\n"
            "  reset       - Reset mixing\n");

    if (fServiceNodeMode)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Client-side mixing is not supported on servicenodes");

    if (request.params[0].get_str() == "start") {
        {
            LOCK(pwalletMain->cs_wallet);
            if (pwalletMain->IsLocked(true))
                throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please unlock wallet for mixing with walletpassphrase first.");
        }

        privateSendClient.fEnablePrivateSend = true;
        bool result = privateSendClient.DoAutomaticDenominating(*g_connman);
        return "Mixing " + (result ? "started successfully" : ("start failed: " + privateSendClient.GetStatuses() + ", will retry"));
    }

    if (request.params[0].get_str() == "stop") {
        privateSendClient.fEnablePrivateSend = false;
        return "Mixing was stopped";
    }

    if (request.params[0].get_str() == "reset") {
        privateSendClient.ResetPool();
        return "Mixing was reset";
    }

    return "Unknown command, please see \"help privatesend\"";
}
#endif // ENABLE_WALLET

UniValue getpoolinfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getpoolinfo\n"
            "Returns an object containing mixing pool related information.\n");

#ifdef ENABLE_WALLET
    CPrivateSendBaseManager* pprivateSendBaseManager = fServiceNodeMode ? (CPrivateSendBaseManager*)&privateSendServer : (CPrivateSendBaseManager*)&privateSendClient;

    UniValue obj(UniValue::VOBJ);
    // TODO:
    // obj.push_back(Pair("state",              pprivateSendBase->GetStateString()));
    obj.push_back(Pair("queue", pprivateSendBaseManager->GetQueueSize()));
    // obj.push_back(Pair("entries",            pprivateSendBase->GetEntriesCount()));
    obj.push_back(Pair("status", privateSendClient.GetStatuses()));

    std::vector<servicenode_info_t> vecSnInfo;
    if (privateSendClient.GetMixingServiceNodesInfo(vecSnInfo)) {
        UniValue pools(UniValue::VARR);
        for (const auto& snInfo : vecSnInfo) {
            UniValue pool(UniValue::VOBJ);
            pool.push_back(Pair("outpoint", snInfo.outpoint.ToStringShort()));
            pool.push_back(Pair("addr", snInfo.addr.ToString()));
            pools.push_back(pool);
        }
        obj.push_back(Pair("pools", pools));
    }

    if (pwalletMain) {
        obj.push_back(Pair("keys_left", pwalletMain->nKeysLeftSinceAutoBackup));
        obj.push_back(Pair("warnings", pwalletMain->nKeysLeftSinceAutoBackup < PRIVATESEND_KEYS_THRESHOLD_WARNING ? "WARNING: keypool is almost depleted!" : ""));
    }
#else  // ENABLE_WALLET
    UniValue obj(UniValue::VOBJ);
    obj.push_back(Pair("state", privateSendServer.GetStateString()));
    obj.push_back(Pair("queue", privateSendServer.GetQueueSize()));
    obj.push_back(Pair("entries", privateSendServer.GetEntriesCount()));
#endif // ENABLE_WALLET

    return obj;
}


UniValue servicenode(const JSONRPCRequest& request)
{
    std::string strCommand;
    if (request.params.size() >= 1) {
        strCommand = request.params[0].get_str();
    }

#ifdef ENABLE_WALLET
    if (strCommand == "start-many")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "DEPRECATED, please use start-all instead");
#endif // ENABLE_WALLET

    if (request.fHelp ||
        (
#ifdef ENABLE_WALLET
            strCommand != "start-alias" && strCommand != "start-all" && strCommand != "start-missing" &&
            strCommand != "start-disabled" && strCommand != "outputs" &&
#endif // ENABLE_WALLET
            strCommand != "list" && strCommand != "list-conf" && strCommand != "count" &&
            strCommand != "debug" && strCommand != "current" && strCommand != "winner" && strCommand != "winners" && strCommand != "genkey" &&
            strCommand != "connect" && strCommand != "status"))
        throw std::runtime_error(
            "servicenode \"command\"...\n"
            "Set of commands to execute servicenode related actions\n"
            "\nArguments:\n"
            "1. \"command\"        (string or set of strings, required) The command to execute\n"
            "\nAvailable commands:\n"
            "  count        - Get information about number of servicenodes (DEPRECATED options: 'total', 'ps', 'enabled', 'qualify', 'all')\n"
            "  current      - Print info on current servicenode winner to be paid the next block (calculated locally)\n"
            "  genkey       - Generate new servicenodepairingkey\n"
#ifdef ENABLE_WALLET
            "  outputs      - Print servicenode compatible outputs\n"
            "  start-alias  - Start single remote servicenode by assigned alias configured in servicenode.conf\n"
            "  start-<mode> - Start remote servicenodes configured in servicenode.conf (<mode>: 'all', 'missing', 'disabled')\n"
#endif // ENABLE_WALLET
            "  status       - Print servicenode status information\n"
            "  list         - Print list of all known servicenodes (see servicenodelist for more info)\n"
            "  list-conf    - Print servicenode.conf in JSON format\n"
            "  winner       - Print info on next servicenode winner to vote for\n"
            "  winners      - Print list of servicenode winners\n");

    if (strCommand == "list") {
        JSONRPCRequest newRequest = request;
        newRequest.params.setArray();
        // forward params but skip "list"
        for (unsigned int i = 1; i < request.params.size(); i++) {
            newRequest.params.push_back(request.params[i]);
        }
        return servicenodelist(newRequest);
    }

    if (strCommand == "connect") {
        if (request.params.size() < 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "ServiceNode address required");

        std::string strAddress = request.params[1].get_str();

        CService addr;
        if (!Lookup(strAddress.c_str(), addr, 0, false))
            throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Incorrect servicenode address %s", strAddress));

        // TODO: Pass CConnman instance somehow and don't use global variable.
        g_connman->OpenServiceNodeConnection(CAddress(addr, NODE_NETWORK));
        if (!g_connman->IsConnected(CAddress(addr, NODE_NETWORK), CConnman::AllNodes))
            throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("Couldn't connect to servicenode %s", strAddress));

        return "successfully connected";
    }

    if (strCommand == "count") {
        if (request.params.size() > 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Too many parameters");

        int nCount;
        servicenode_info_t snInfo;
        snodeman.GetNextServiceNodeInQueueForPayment(true, nCount, snInfo);

        int total = snodeman.size();
        int ps = snodeman.CountEnabled(MIN_PRIVATESEND_PEER_PROTO_VERSION);
        int enabled = snodeman.CountEnabled();

        if (request.params.size() == 1) {
            UniValue obj(UniValue::VOBJ);

            obj.push_back(Pair("total", total));
            obj.push_back(Pair("ps_compatible", ps));
            obj.push_back(Pair("enabled", enabled));
            obj.push_back(Pair("qualify", nCount));

            return obj;
        }

        std::string strMode = request.params[1].get_str();

        if (strMode == "total")
            return total;

        if (strMode == "ps")
            return ps;

        if (strMode == "enabled")
            return enabled;

        if (strMode == "qualify")
            return nCount;

        if (strMode == "all")
            return strprintf("Total: %d (PS Compatible: %d / Enabled: %d / Qualify: %d)",
                total, ps, enabled, nCount);
    }

    if (strCommand == "current" || strCommand == "winner") {
        int nCount;
        int nHeight;
        servicenode_info_t snInfo;
        CBlockIndex* pindex = NULL;
        {
            LOCK(cs_main);
            pindex = chainActive.Tip();
        }
        nHeight = pindex->nHeight + (strCommand == "current" ? 1 : 10);
        snodeman.UpdateLastPaid(pindex);

        if (!snodeman.GetNextServiceNodeInQueueForPayment(nHeight, true, nCount, snInfo))
            return "unknown";

        UniValue obj(UniValue::VOBJ);

        obj.push_back(Pair("height", nHeight));
        obj.push_back(Pair("IP:port", snInfo.addr.ToString()));
        obj.push_back(Pair("protocol", snInfo.nProtocolVersion));
        obj.push_back(Pair("outpoint", snInfo.outpoint.ToStringShort()));
        obj.push_back(Pair("payee", CCreditAddress(snInfo.pubKeyCollateralAddress.GetID()).ToString()));
        obj.push_back(Pair("lastseen", snInfo.nTimeLastPing));
        obj.push_back(Pair("activeseconds", snInfo.nTimeLastPing - snInfo.sigTime));
        return obj;
    }

#ifdef ENABLE_WALLET
    if (strCommand == "start-alias") {
        if (!EnsureWalletIsAvailable(request.fHelp))
            return NullUniValue;

        if (request.params.size() < 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Please specify an alias");

        {
            LOCK(pwalletMain->cs_wallet);
            EnsureWalletIsUnlocked();
        }

        std::string strAlias = request.params[1].get_str();

        bool fFound = false;

        UniValue statusObj(UniValue::VOBJ);
        statusObj.push_back(Pair("alias", strAlias));

        for (const auto& sne : servicenodeConfig.getEntries()) {
            if (sne.getAlias() == strAlias) {
                fFound = true;
                std::string strError;
                CServiceNodeBroadcast snb;

                bool fResult = CServiceNodeBroadcast::Create(sne.getIp(), sne.getPrivKey(), sne.getTxHash(), sne.getOutputIndex(), strError, snb);

                int nDoS;
                if (fResult && !snodeman.CheckSnbAndUpdateServiceNodeList(NULL, snb, nDoS, *g_connman)) {
                    strError = "Failed to verify SNB";
                    fResult = false;
                }

                statusObj.push_back(Pair("result", fResult ? "successful" : "failed"));
                if (!fResult) {
                    statusObj.push_back(Pair("errorMessage", strError));
                }
                snodeman.NotifyServiceNodeUpdates(*g_connman);
                break;
            }
        }

        if (!fFound) {
            statusObj.push_back(Pair("result", "failed"));
            statusObj.push_back(Pair("errorMessage", "Could not find alias in config. Verify with list-conf."));
        }

        return statusObj;
    }

    if (strCommand == "start-all" || strCommand == "start-missing" || strCommand == "start-disabled") {
        if (!EnsureWalletIsAvailable(request.fHelp))
            return NullUniValue;

        {
            LOCK(pwalletMain->cs_wallet);
            EnsureWalletIsUnlocked();
        }

        if ((strCommand == "start-missing" || strCommand == "start-disabled") && !servicenodeSync.IsServiceNodeListSynced()) {
            throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "You can't use this command until servicenode list is synced");
        }

        int nSuccessful = 0;
        int nFailed = 0;

        UniValue resultsObj(UniValue::VOBJ);

        for (const auto& sne : servicenodeConfig.getEntries()) {
            std::string strError;

            COutPoint outpoint = COutPoint(uint256S(sne.getTxHash()), (uint32_t)atoi(sne.getOutputIndex()));
            CServiceNode sn;
            bool fFound = snodeman.Get(outpoint, sn);
            CServiceNodeBroadcast snb;

            if (strCommand == "start-missing" && fFound)
                continue;
            if (strCommand == "start-disabled" && fFound && sn.IsEnabled())
                continue;

            bool fResult = CServiceNodeBroadcast::Create(sne.getIp(), sne.getPrivKey(), sne.getTxHash(), sne.getOutputIndex(), strError, snb);

            int nDoS;
            if (fResult && !snodeman.CheckSnbAndUpdateServiceNodeList(NULL, snb, nDoS, *g_connman)) {
                strError = "Failed to verify SNB";
                fResult = false;
            }

            UniValue statusObj(UniValue::VOBJ);
            statusObj.push_back(Pair("alias", sne.getAlias()));
            statusObj.push_back(Pair("result", fResult ? "successful" : "failed"));

            if (fResult) {
                nSuccessful++;
            } else {
                nFailed++;
                statusObj.push_back(Pair("errorMessage", strError));
            }

            resultsObj.push_back(Pair("status", statusObj));
        }
        snodeman.NotifyServiceNodeUpdates(*g_connman);

        UniValue returnObj(UniValue::VOBJ);
        returnObj.push_back(Pair("overall", strprintf("Successfully started %d servicenodes, failed to start %d, total %d", nSuccessful, nFailed, nSuccessful + nFailed)));
        returnObj.push_back(Pair("detail", resultsObj));

        return returnObj;
    }
#endif // ENABLE_WALLET

    if (strCommand == "genkey") {
        CKey secret;
        secret.MakeNewKey(false);

        return CCreditSecret(secret).ToString();
    }

    if (strCommand == "list-conf") {
        UniValue resultObj(UniValue::VOBJ);

        for (const auto& sne : servicenodeConfig.getEntries()) {
            COutPoint outpoint = COutPoint(uint256S(sne.getTxHash()), (uint32_t)atoi(sne.getOutputIndex()));
            CServiceNode sn;
            bool fFound = snodeman.Get(outpoint, sn);

            std::string strStatus = fFound ? sn.GetStatus() : "MISSING";

            UniValue snObj(UniValue::VOBJ);
            snObj.push_back(Pair("address", sne.getIp()));
            snObj.push_back(Pair("privateKey", sne.getPrivKey()));
            snObj.push_back(Pair("txHash", sne.getTxHash()));
            snObj.push_back(Pair("outputIndex", sne.getOutputIndex()));
            snObj.push_back(Pair("status", strStatus));
            resultObj.push_back(Pair(sne.getAlias(), snObj));
        }

        return resultObj;
    }

#ifdef ENABLE_WALLET
    if (strCommand == "outputs") {
        if (!EnsureWalletIsAvailable(request.fHelp))
            return NullUniValue;

        // Find possible candidates
        std::vector<COutput> vPossibleCoins;
        pwalletMain->AvailableCoins(vPossibleCoins, true, NULL, false, ONLY_50000);

        UniValue obj(UniValue::VOBJ);
        for (const auto& out : vPossibleCoins) {
            obj.push_back(Pair(out.tx->GetHash().ToString(), strprintf("%d", out.i)));
        }

        return obj;
    }
#endif // ENABLE_WALLET

    if (strCommand == "status") {
        if (!fServiceNodeMode)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "This is not a servicenode");

        UniValue snObj(UniValue::VOBJ);

        snObj.push_back(Pair("outpoint", activeServiceNode.outpoint.ToStringShort()));
        snObj.push_back(Pair("service", activeServiceNode.service.ToString()));

        CServiceNode sn;
        if (snodeman.Get(activeServiceNode.outpoint, sn)) {
            snObj.push_back(Pair("payee", CCreditAddress(sn.pubKeyCollateralAddress.GetID()).ToString()));
        }

        snObj.push_back(Pair("status", activeServiceNode.GetStatus()));
        return snObj;
    }

    if (strCommand == "winners") {
        int nHeight;
        {
            LOCK(cs_main);
            CBlockIndex* pindex = chainActive.Tip();
            if (!pindex)
                return NullUniValue;

            nHeight = pindex->nHeight;
        }

        int nLast = 10;
        std::string strFilter = "";

        if (request.params.size() >= 2) {
            nLast = atoi(request.params[1].get_str());
        }

        if (request.params.size() == 3) {
            strFilter = request.params[2].get_str();
        }

        if (request.params.size() > 3)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'servicenode winners ( \"count\" \"filter\" )'");

        UniValue obj(UniValue::VOBJ);

        for (int i = nHeight - nLast; i < nHeight + 20; i++) {
            std::string strPayment = GetRequiredPaymentsString(i);
            if (strFilter != "" && strPayment.find(strFilter) == std::string::npos)
                continue;
            obj.push_back(Pair(strprintf("%d", i), strPayment));
        }

        return obj;
    }

    return NullUniValue;
}

UniValue servicenodelist(const JSONRPCRequest& request)
{
    std::string strMode = "json";
    std::string strFilter = "";

    if (request.params.size() >= 1)
        strMode = request.params[0].get_str();
    if (request.params.size() == 2)
        strFilter = request.params[1].get_str();

    if (request.fHelp || (strMode != "activeseconds" && strMode != "addr" && strMode != "daemon" && strMode != "full" && strMode != "info" && strMode != "json" &&
                             strMode != "lastseen" && strMode != "lastpaidtime" && strMode != "lastpaidblock" &&
                             strMode != "protocol" && strMode != "payee" && strMode != "pubkey" &&
                             strMode != "rank" && strMode != "sentinel" && strMode != "status")) {
        throw std::runtime_error(
            "servicenodelist ( \"mode\" \"filter\" )\n"
            "Get a list of servicenodes in different modes\n"
            "\nArguments:\n"
            "1. \"mode\"      (string, optional/required to use filter, defaults = json) The mode to run list in\n"
            "2. \"filter\"    (string, optional) Filter results. Partial match by outpoint by default in all modes,\n"
            "                                    additional matches in some modes are also available\n"
            "\nAvailable modes:\n"
            "  activeseconds  - Print number of seconds servicenode recognized by the network as enabled\n"
            "                   (since latest issued \"servicenode start/start-many/start-alias\")\n"
            "  addr           - Print ip address associated with a servicenode (can be additionally filtered, partial match)\n"
            "  daemon         - Print daemon version of a servicenode (can be additionally filtered, exact match)\n"
            "  full           - Print info in format 'status protocol payee lastseen activeseconds lastpaidtime lastpaidblock IP'\n"
            "                   (can be additionally filtered, partial match)\n"
            "  info           - Print info in format 'status protocol payee lastseen activeseconds sentinelversion sentinelstate IP'\n"
            "                   (can be additionally filtered, partial match)\n"
            "  json           - Print info in JSON format (can be additionally filtered, partial match)\n"
            "  lastpaidblock  - Print the last block height a node was paid on the network\n"
            "  lastpaidtime   - Print the last time a node was paid on the network\n"
            "  lastseen       - Print timestamp of when a servicenode was last seen on the network\n"
            "  payee          - Print Credit address associated with a servicenode (can be additionally filtered,\n"
            "                   partial match)\n"
            "  protocol       - Print protocol of a servicenode (can be additionally filtered, exact match)\n"
            "  pubkey         - Print the servicenode (not collateral) public key\n"
            "  rank           - Print rank of a servicenode based on current block\n"
            "  sentinel       - Print sentinel version of a servicenode (can be additionally filtered, exact match)\n"
            "  status         - Print servicenode status: PRE_ENABLED / ENABLED / EXPIRED / SENTINEL_PING_EXPIRED / NEW_START_REQUIRED /\n"
            "                   UPDATE_REQUIRED / POSE_BAN / OUTPOINT_SPENT (can be additionally filtered, partial match)\n");
    }

    if (strMode == "full" || strMode == "json" || strMode == "lastpaidtime" || strMode == "lastpaidblock") {
        CBlockIndex* pindex = NULL;
        {
            LOCK(cs_main);
            pindex = chainActive.Tip();
        }
        snodeman.UpdateLastPaid(pindex);
    }

    UniValue obj(UniValue::VOBJ);
    if (strMode == "rank") {
        CServiceNodeMan::rank_pair_vec_t vServiceNodeRanks;
        snodeman.GetServiceNodeRanks(vServiceNodeRanks);
        for (const auto& rankpair : vServiceNodeRanks) {
            std::string strOutpoint = rankpair.second.outpoint.ToStringShort();
            if (strFilter != "" && strOutpoint.find(strFilter) == std::string::npos)
                continue;
            obj.push_back(Pair(strOutpoint, rankpair.first));
        }
    } else {
        std::map<COutPoint, CServiceNode> mapServiceNodes = snodeman.GetFullServiceNodeMap();
        for (const auto& snpair : mapServiceNodes) {
            CServiceNode sn = snpair.second;
            std::string strOutpoint = snpair.first.ToStringShort();
            if (strMode == "activeseconds") {
                if (strFilter != "" && strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                obj.push_back(Pair(strOutpoint, (int64_t)(sn.lastPing.sigTime - sn.sigTime)));
            } else if (strMode == "addr") {
                std::string strAddress = sn.addr.ToString();
                if (strFilter != "" && strAddress.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                obj.push_back(Pair(strOutpoint, strAddress));
            } else if (strMode == "daemon") {
                std::string strDaemon = sn.lastPing.GetDaemonString();
                if (strFilter != "" && strDaemon.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                obj.push_back(Pair(strOutpoint, strDaemon));
            } else if (strMode == "sentinel") {
                std::string strSentinel = sn.lastPing.GetSentinelString();
                if (strFilter != "" && strSentinel.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                obj.push_back(Pair(strOutpoint, strSentinel));
            } else if (strMode == "full") {
                std::ostringstream streamFull;
                streamFull << std::setw(18) << sn.GetStatus() << " " << sn.nProtocolVersion << " " << CCreditAddress(sn.pubKeyCollateralAddress.GetID()).ToString() << " " << (int64_t)sn.lastPing.sigTime << " " << std::setw(8) << (int64_t)(sn.lastPing.sigTime - sn.sigTime) << " " << std::setw(10) << sn.GetLastPaidTime() << " " << std::setw(6) << sn.GetLastPaidBlock() << " " << sn.addr.ToString();
                std::string strFull = streamFull.str();
                if (strFilter != "" && strFull.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                obj.push_back(Pair(strOutpoint, strFull));
            } else if (strMode == "info") {
                std::ostringstream streamInfo;
                streamInfo << std::setw(18) << sn.GetStatus() << " " << sn.nProtocolVersion << " " << CCreditAddress(sn.pubKeyCollateralAddress.GetID()).ToString() << " " << (int64_t)sn.lastPing.sigTime << " " << std::setw(8) << (int64_t)(sn.lastPing.sigTime - sn.sigTime) << " " << sn.lastPing.GetSentinelString() << " " << (sn.lastPing.fSentinelIsCurrent ? "current" : "expired") << " " << sn.addr.ToString();
                std::string strInfo = streamInfo.str();
                if (strFilter != "" && strInfo.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                obj.push_back(Pair(strOutpoint, strInfo));
            } else if (strMode == "json") {
                std::ostringstream streamInfo;
                streamInfo << sn.addr.ToString() << " " << CCreditAddress(sn.pubKeyCollateralAddress.GetID()).ToString() << " " << sn.GetStatus() << " " << sn.nProtocolVersion << " " << sn.lastPing.nDaemonVersion << " " << sn.lastPing.GetSentinelString() << " " << (sn.lastPing.fSentinelIsCurrent ? "current" : "expired") << " " << (int64_t)sn.lastPing.sigTime << " " << (int64_t)(sn.lastPing.sigTime - sn.sigTime) << " " << sn.GetLastPaidTime() << " " << sn.GetLastPaidBlock();
                std::string strInfo = streamInfo.str();
                if (strFilter != "" && strInfo.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                UniValue objSN(UniValue::VOBJ);
                objSN.push_back(Pair("address", sn.addr.ToString()));
                objSN.push_back(Pair("payee", CCreditAddress(sn.pubKeyCollateralAddress.GetID()).ToString()));
                objSN.push_back(Pair("status", sn.GetStatus()));
                objSN.push_back(Pair("protocol", sn.nProtocolVersion));
                objSN.push_back(Pair("daemonversion", sn.lastPing.GetDaemonString()));
                objSN.push_back(Pair("sentinelversion", sn.lastPing.GetSentinelString()));
                objSN.push_back(Pair("sentinelstate", (sn.lastPing.fSentinelIsCurrent ? "current" : "expired")));
                objSN.push_back(Pair("lastseen", (int64_t)sn.lastPing.sigTime));
                objSN.push_back(Pair("activeseconds", (int64_t)(sn.lastPing.sigTime - sn.sigTime)));
                objSN.push_back(Pair("lastpaidtime", sn.GetLastPaidTime()));
                objSN.push_back(Pair("lastpaidblock", sn.GetLastPaidBlock()));
                obj.push_back(Pair(strOutpoint, objSN));
            } else if (strMode == "lastpaidblock") {
                if (strFilter != "" && strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                obj.push_back(Pair(strOutpoint, sn.GetLastPaidBlock()));
            } else if (strMode == "lastpaidtime") {
                if (strFilter != "" && strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                obj.push_back(Pair(strOutpoint, sn.GetLastPaidTime()));
            } else if (strMode == "lastseen") {
                if (strFilter != "" && strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                obj.push_back(Pair(strOutpoint, (int64_t)sn.lastPing.sigTime));
            } else if (strMode == "payee") {
                CCreditAddress address(sn.pubKeyCollateralAddress.GetID());
                std::string strPayee = address.ToString();
                if (strFilter != "" && strPayee.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                obj.push_back(Pair(strOutpoint, strPayee));
            } else if (strMode == "protocol") {
                if (strFilter != "" && strFilter != strprintf("%d", sn.nProtocolVersion) &&
                    strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                obj.push_back(Pair(strOutpoint, sn.nProtocolVersion));
            } else if (strMode == "pubkey") {
                if (strFilter != "" && strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                obj.push_back(Pair(strOutpoint, HexStr(sn.pubKeyServiceNode)));
            } else if (strMode == "status") {
                std::string strStatus = sn.GetStatus();
                if (strFilter != "" && strStatus.find(strFilter) == std::string::npos &&
                    strOutpoint.find(strFilter) == std::string::npos)
                    continue;
                obj.push_back(Pair(strOutpoint, strStatus));
            }
        }
    }
    return obj;
}

bool DecodeHexVecSnb(std::vector<CServiceNodeBroadcast>& vecSnb, std::string strHexSnb)
{
    if (!IsHex(strHexSnb))
        return false;

    std::vector<unsigned char> snbData(ParseHex(strHexSnb));
    CDataStream ssData(snbData, SER_NETWORK, PROTOCOL_VERSION);
    try {
        ssData >> vecSnb;
    } catch (const std::exception&) {
        return false;
    }

    return true;
}

UniValue servicenodebroadcast(const JSONRPCRequest& request)
{
    std::string strCommand;
    if (request.params.size() >= 1)
        strCommand = request.params[0].get_str();

    if (request.fHelp ||
        (
#ifdef ENABLE_WALLET
            strCommand != "create-alias" && strCommand != "create-all" &&
#endif // ENABLE_WALLET
            strCommand != "decode" && strCommand != "relay"))
        throw std::runtime_error(
            "servicenodebroadcast \"command\"...\n"
            "Set of commands to create and relay servicenode broadcast messages\n"
            "\nArguments:\n"
            "1. \"command\"        (string or set of strings, required) The command to execute\n"
            "\nAvailable commands:\n"
#ifdef ENABLE_WALLET
            "  create-alias  - Create single remote servicenode broadcast message by assigned alias configured in servicenode.conf\n"
            "  create-all    - Create remote servicenode broadcast messages for all servicenodes configured in servicenode.conf\n"
#endif // ENABLE_WALLET
            "  decode        - Decode servicenode broadcast message\n"
            "  relay         - Relay servicenode broadcast message to the network\n");

#ifdef ENABLE_WALLET
    if (strCommand == "create-alias") {
        if (!EnsureWalletIsAvailable(request.fHelp))
            return NullUniValue;

        // wait for reindex and/or import to finish
        if (fImporting || fReindex)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Wait for reindex and/or import to finish");

        if (request.params.size() < 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Please specify an alias");

        {
            LOCK(pwalletMain->cs_wallet);
            EnsureWalletIsUnlocked();
        }

        bool fFound = false;
        std::string strAlias = request.params[1].get_str();

        UniValue statusObj(UniValue::VOBJ);
        std::vector<CServiceNodeBroadcast> vecSnb;

        statusObj.push_back(Pair("alias", strAlias));

        for (const auto& sne : servicenodeConfig.getEntries()) {
            if (sne.getAlias() == strAlias) {
                fFound = true;
                std::string strError;
                CServiceNodeBroadcast snb;

                bool fResult = CServiceNodeBroadcast::Create(sne.getIp(), sne.getPrivKey(), sne.getTxHash(), sne.getOutputIndex(), strError, snb, true);

                statusObj.push_back(Pair("result", fResult ? "successful" : "failed"));
                if (fResult) {
                    vecSnb.push_back(snb);
                    CDataStream ssVecSnb(SER_NETWORK, PROTOCOL_VERSION);
                    ssVecSnb << vecSnb;
                    statusObj.push_back(Pair("hex", HexStr(ssVecSnb)));
                } else {
                    statusObj.push_back(Pair("errorMessage", strError));
                }
                break;
            }
        }

        if (!fFound) {
            statusObj.push_back(Pair("result", "not found"));
            statusObj.push_back(Pair("errorMessage", "Could not find alias in config. Verify with list-conf."));
        }

        return statusObj;
    }

    if (strCommand == "create-all") {
        if (!EnsureWalletIsAvailable(request.fHelp))
            return NullUniValue;

        // wait for reindex and/or import to finish
        if (fImporting || fReindex)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Wait for reindex and/or import to finish");

        {
            LOCK(pwalletMain->cs_wallet);
            EnsureWalletIsUnlocked();
        }

        int nSuccessful = 0;
        int nFailed = 0;

        UniValue resultsObj(UniValue::VOBJ);
        std::vector<CServiceNodeBroadcast> vecSnb;

        for (const auto& sne : servicenodeConfig.getEntries()) {
            std::string strError;
            CServiceNodeBroadcast snb;

            bool fResult = CServiceNodeBroadcast::Create(sne.getIp(), sne.getPrivKey(), sne.getTxHash(), sne.getOutputIndex(), strError, snb, true);

            UniValue statusObj(UniValue::VOBJ);
            statusObj.push_back(Pair("alias", sne.getAlias()));
            statusObj.push_back(Pair("result", fResult ? "successful" : "failed"));

            if (fResult) {
                nSuccessful++;
                vecSnb.push_back(snb);
            } else {
                nFailed++;
                statusObj.push_back(Pair("errorMessage", strError));
            }

            resultsObj.push_back(Pair("status", statusObj));
        }

        CDataStream ssVecSnb(SER_NETWORK, PROTOCOL_VERSION);
        ssVecSnb << vecSnb;
        UniValue returnObj(UniValue::VOBJ);
        returnObj.push_back(Pair("overall", strprintf("Successfully created broadcast messages for %d servicenodes, failed to create %d, total %d", nSuccessful, nFailed, nSuccessful + nFailed)));
        returnObj.push_back(Pair("detail", resultsObj));
        returnObj.push_back(Pair("hex", HexStr(ssVecSnb.begin(), ssVecSnb.end())));

        return returnObj;
    }
#endif // ENABLE_WALLET

    if (strCommand == "decode") {
        if (request.params.size() != 2)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Correct usage is 'servicenodebroadcast decode \"hexstring\"'");

        std::vector<CServiceNodeBroadcast> vecSnb;

        if (!DecodeHexVecSnb(vecSnb, request.params[1].get_str()))
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "ServiceNode broadcast message decode failed");

        int nSuccessful = 0;
        int nFailed = 0;
        int nDos = 0;
        UniValue returnObj(UniValue::VOBJ);

        for (const auto& snb : vecSnb) {
            UniValue resultObj(UniValue::VOBJ);

            if (snb.CheckSignature(nDos)) {
                nSuccessful++;
                resultObj.push_back(Pair("outpoint", snb.outpoint.ToStringShort()));
                resultObj.push_back(Pair("addr", snb.addr.ToString()));
                resultObj.push_back(Pair("pubKeyCollateralAddress", CCreditAddress(snb.pubKeyCollateralAddress.GetID()).ToString()));
                resultObj.push_back(Pair("pubKeyServiceNode", CCreditAddress(snb.pubKeyServiceNode.GetID()).ToString()));
                resultObj.push_back(Pair("vchSig", EncodeBase64(&snb.vchSig[0], snb.vchSig.size())));
                resultObj.push_back(Pair("sigTime", snb.sigTime));
                resultObj.push_back(Pair("protocolVersion", snb.nProtocolVersion));
                resultObj.push_back(Pair("nLastPsq", snb.nLastPsq));

                UniValue lastPingObj(UniValue::VOBJ);
                lastPingObj.push_back(Pair("outpoint", snb.lastPing.servicenodeOutpoint.ToStringShort()));
                lastPingObj.push_back(Pair("blockHash", snb.lastPing.blockHash.ToString()));
                lastPingObj.push_back(Pair("sigTime", snb.lastPing.sigTime));
                lastPingObj.push_back(Pair("vchSig", EncodeBase64(&snb.lastPing.vchSig[0], snb.lastPing.vchSig.size())));

                resultObj.push_back(Pair("lastPing", lastPingObj));
            } else {
                nFailed++;
                resultObj.push_back(Pair("errorMessage", "ServiceNode broadcast signature verification failed"));
            }

            returnObj.push_back(Pair(snb.GetHash().ToString(), resultObj));
        }

        returnObj.push_back(Pair("overall", strprintf("Successfully decoded broadcast messages for %d servicenodes, failed to decode %d, total %d", nSuccessful, nFailed, nSuccessful + nFailed)));

        return returnObj;
    }

    if (strCommand == "relay") {
        if (request.params.size() < 2 || request.params.size() > 3)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "servicenodebroadcast relay \"hexstring\"\n"
                                                      "\nArguments:\n"
                                                      "1. \"hex\"      (string, required) Broadcast messages hex string\n");

        std::vector<CServiceNodeBroadcast> vecSnb;

        if (!DecodeHexVecSnb(vecSnb, request.params[1].get_str()))
            throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "ServiceNode broadcast message decode failed");

        int nSuccessful = 0;
        int nFailed = 0;
        UniValue returnObj(UniValue::VOBJ);

        // verify all signatures first, bailout if any of them broken
        for (const auto& snb : vecSnb) {
            UniValue resultObj(UniValue::VOBJ);

            resultObj.push_back(Pair("outpoint", snb.outpoint.ToStringShort()));
            resultObj.push_back(Pair("addr", snb.addr.ToString()));

            int nDos = 0;
            bool fResult;
            if (snb.CheckSignature(nDos)) {
                fResult = snodeman.CheckSnbAndUpdateServiceNodeList(NULL, snb, nDos, *g_connman);
                snodeman.NotifyServiceNodeUpdates(*g_connman);
            } else
                fResult = false;

            if (fResult) {
                nSuccessful++;
                resultObj.push_back(Pair(snb.GetHash().ToString(), "successful"));
            } else {
                nFailed++;
                resultObj.push_back(Pair("errorMessage", "ServiceNode broadcast signature verification failed"));
            }

            returnObj.push_back(Pair(snb.GetHash().ToString(), resultObj));
        }

        returnObj.push_back(Pair("overall", strprintf("Successfully relayed broadcast messages for %d servicenodes, failed to relay %d, total %d", nSuccessful, nFailed, nSuccessful + nFailed)));

        return returnObj;
    }

    return NullUniValue;
}

UniValue sentinelping(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "sentinelping version\n"
            "\nSentinel ping.\n"
            "\nArguments:\n"
            "1. version           (string, required) Sentinel version in the form \"x.x.x\"\n"
            "\nResult:\n"
            "state                (boolean) Ping result\n"
            "\nExamples:\n" +
            HelpExampleCli("sentinelping", "1.0.2") + HelpExampleRpc("sentinelping", "1.0.2"));
    }

    activeServiceNode.UpdateSentinelPing(StringVersionToInt(request.params[0].get_str()));
    return true;
}

static const CRPCCommand commands[] =
    {
        //  category                 name                     actor (function)     okSafe argNames
        //  ---------------------    ----------------------   -------------------  ------ ----
        /* Credit features */
        {"credit", "servicenode", &servicenode, true, {}},
        {"credit", "servicenodelist", &servicenodelist, true, {}},
        {"credit", "servicenodebroadcast", &servicenodebroadcast, true, {}},
        {"credit", "getpoolinfo", &getpoolinfo, true, {}},
        {"credit", "sentinelping", &sentinelping, true, {}},
#ifdef ENABLE_WALLET
        {"credit", "privatesend", &privatesend, false, {}},
#endif // ENABLE_WALLET
};

void RegisterServiceNodeRPCCommands(CRPCTable& t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
