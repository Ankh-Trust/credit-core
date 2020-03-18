
// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Copyright (c) 2014-2017 The Dash Core Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "servicenode-payments.h"

#include "activeservicenode.h"
#include "chain.h"
#include "consensus/validation.h"
#include "servicenode-sync.h"
#include "servicenodeman.h"
#include "fluid/fluiddb.h"
#include "governance-classes.h"
#include "init.h"
#include "messagesigner.h"
#include "netfulfilledman.h"
#include "netmessagemaker.h"
#include "policy/fees.h"
#include "spork.h"
#include "util.h"
#include "utilmoneystr.h"

#include <boost/lexical_cast.hpp>

/** Object for who's going to get paid on which blocks */
CServiceNodePayments snpayments;

CCriticalSection cs_vecPayees;
CCriticalSection cs_mapServiceNodeBlocks;
CCriticalSection cs_mapServiceNodePaymentVotes;

/**
* IsBlockValueValid
*
*   Determine if coinbase outgoing created money is the correct value
*
*   Why is this needed?
*   - In Credit some blocks are superblocks, which output much higher amounts of coins
*   - Otherblocks are 10% lower in outgoing value, so in total, no extra coins are created
*   - When non-superblocks are detected, the normal schedule should be maintained
*/

bool IsBlockValueValid(const CBlock& block, int nBlockHeight, CAmount blockReward, std::string& strErrorRet)
{
    strErrorRet = "";

    bool isBlockRewardValueMet = (block.vtx[0]->GetValueOut() <= blockReward);
    LogPrint("gobject", "block.vtx[0].GetValueOut() %lld <= blockReward %lld\n", block.vtx[0]->GetValueOut(), blockReward);

    // we are still using budgets, but we have no data about them anymore,
    // all we know is predefined budget cycle and window

    const Consensus::Params& consensusParams = Params().GetConsensus();
    if (nBlockHeight < consensusParams.nSuperblockStartBlock) {
        int nOffset = nBlockHeight % consensusParams.nBudgetPaymentsCycleBlocks;
        if (nBlockHeight >= consensusParams.nBudgetPaymentsStartBlock &&
            nOffset < consensusParams.nBudgetPaymentsWindowBlocks) {
            if (servicenodeSync.IsSynced() && !sporkManager.IsSporkActive(SPORK_13_OLD_SUPERBLOCK_FLAG)) {
                LogPrint("gobject", "IsBlockValueValid -- Client synced but budget spork is disabled, checking block value against block reward\n");
                if (!isBlockRewardValueMet) {
                    strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, budgets are disabled",
                        nBlockHeight, block.vtx[0]->GetValueOut(), blockReward);
                }
                return isBlockRewardValueMet;
            }
            LogPrint("gobject", "IsBlockValueValid -- WARNING: Skipping budget block value checks, accepting block\n");
            // TODO: reprocess blocks to make sure they are legit?
            return true;
        }
        // LogPrint("gobject", "IsBlockValueValid -- Block is not in budget cycle window, checking block value against block reward\n");
        if (!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, block is not in budget cycle window",
                nBlockHeight, block.vtx[0]->GetValueOut(), blockReward);
        }
        return isBlockRewardValueMet;
    }

    // superblocks started

    CAmount nSuperblockMaxValue = blockReward + CSuperblock::GetPaymentsLimit(nBlockHeight);
    bool isSuperblockMaxValueMet = (block.vtx[0]->GetValueOut() <= nSuperblockMaxValue);

    LogPrint("gobject", "block.vtx[0].GetValueOut() %lld <= nSuperblockMaxValue %lld\n", block.vtx[0]->GetValueOut(), nSuperblockMaxValue);

    if (!servicenodeSync.IsSynced()) {
        // not enough data but at least it must NOT exceed superblock max value
        if (CSuperblock::IsValidBlockHeight(nBlockHeight)) {
            LogPrint("gobject", "IsBlockPayeeValid -- WARNING: Client not synced, checking superblock max bounds only\n");
            if (!isSuperblockMaxValueMet) {
                strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded superblock max value",
                    nBlockHeight, block.vtx[0]->GetValueOut(), nSuperblockMaxValue);
            }
            return isSuperblockMaxValueMet;
        }
        if (!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, only regular blocks are allowed at this height",
                nBlockHeight, block.vtx[0]->GetValueOut(), blockReward);
        }
        // it MUST be a regular block otherwise
        return isBlockRewardValueMet;
    }

    // we are synced, let's try to check as much data as we can

    if (sporkManager.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED)) {
        if (CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
            if (CSuperblockManager::IsValid(*block.vtx[0], nBlockHeight, blockReward)) {
                LogPrint("gobject", "IsBlockValueValid -- Valid superblock at height %d: %s", nBlockHeight, block.vtx[0]->ToString());
                // all checks are done in CSuperblock::IsValid, nothing to do here
                return true;
            }

            // triggered but invalid? that's weird
            LogPrintf("IsBlockValueValid -- ERROR: Invalid superblock detected at height %d: %s", nBlockHeight, block.vtx[0]->ToString());
            // should NOT allow invalid superblocks, when superblocks are enabled
            strErrorRet = strprintf("invalid superblock detected at height %d", nBlockHeight);
            return false;
        }
        LogPrint("gobject", "IsBlockValueValid -- No triggered superblock detected at height %d\n", nBlockHeight);
        if (!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, no triggered superblock detected",
                nBlockHeight, block.vtx[0]->GetValueOut(), blockReward);
        }
    } else {
        // should NOT allow superblocks at all, when superblocks are disabled
        LogPrint("gobject", "IsBlockValueValid -- Superblocks are disabled, no superblocks allowed\n");
        if (!isBlockRewardValueMet) {
            strErrorRet = strprintf("coinbase pays too much at height %d (actual=%d vs limit=%d), exceeded block reward, superblocks are disabled",
                nBlockHeight, block.vtx[0]->GetValueOut(), blockReward);
        }
    }

    // it MUST be a regular block
    return isBlockRewardValueMet;
}

bool IsBlockPayeeValid(const CTransaction& txNew, int nBlockHeight, CAmount blockReward)
{
    if (!servicenodeSync.IsSynced()) {
        //there is no budget data to use to check anything, let's just accept the longest chain
        LogPrint("snpayments", "IsBlockPayeeValid -- WARNING: Client not synced, skipping block payee checks\n");
        return true;
    }

    // we are still using budgets, but we have no data about them anymore,
    // we can only check servicenode payments

    const Consensus::Params& consensusParams = Params().GetConsensus();

    if (nBlockHeight < consensusParams.nSuperblockStartBlock) {
        LogPrint("gobject", "IsBlockPayeeValid -- WARNING: Client synced but old budget system is disabled, accepting any payee\n");
        return true;
    }

    // superblocks started
    // SEE IF THIS IS A VALID SUPERBLOCK

    if (sporkManager.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED)) {
        if (CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
            if (CSuperblockManager::IsValid(txNew, nBlockHeight, blockReward)) {
                LogPrint("gobject", "IsBlockPayeeValid -- Valid superblock at height %d: %s", nBlockHeight, txNew.ToString());
                return true;
            }

            LogPrintf("IsBlockPayeeValid -- ERROR: Invalid superblock detected at height %d: %s", nBlockHeight, txNew.ToString());
            // should NOT allow such superblocks, when superblocks are enabled
            return false;
        }
        // continue validation, should pay DN
        LogPrint("gobject", "IsBlockPayeeValid -- No triggered superblock detected at height %d\n", nBlockHeight);
    } else {
        // should NOT allow superblocks at all, when superblocks are disabled
        LogPrint("gobject", "IsBlockPayeeValid -- Superblocks are disabled, no superblocks allowed\n");
    }

    // IF THIS ISN'T A SUPERBLOCK OR SUPERBLOCK IS INVALID, IT SHOULD PAY A SERVICENODE DIRECTLY
    if (snpayments.IsTransactionValid(txNew, nBlockHeight)) {
        LogPrint("snpayments", "IsBlockPayeeValid -- Valid servicenode payment at height %d: %s", nBlockHeight, txNew.ToString());
        return true;
    }

    if (sporkManager.IsSporkActive(SPORK_8_SERVICENODE_PAYMENT_ENFORCEMENT)) {
        LogPrintf("IsBlockPayeeValid -- ERROR: Invalid servicenode payment detected at height %d: %s", nBlockHeight, txNew.ToString());
        return false;
    }

    LogPrintf("IsBlockPayeeValid -- WARNING: ServiceNode payment enforcement is disabled, accepting any payee\n");
    return true;
}

void FillBlockPayments(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutServiceNodeRet, std::vector<CTxOut>& voutSuperblockRet)
{
    // only create superblocks if spork is enabled AND if superblock is actually triggered
    // (height should be validated inside)
    if (sporkManager.IsSporkActive(SPORK_9_SUPERBLOCKS_ENABLED) &&
        CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
        LogPrint("gobject", "FillBlockPayments -- triggered superblock creation at height %d\n", nBlockHeight);
        CSuperblockManager::CreateSuperblock(txNew, nBlockHeight, voutSuperblockRet);
        return;
    }


    // FILL BLOCK PAYEE WITH SERVICENODE PAYMENT OTHERWISE
    snpayments.FillBlockPayee(txNew, nBlockHeight, blockReward, txoutServiceNodeRet);
    LogPrint("snpayments", "FillBlockPayments -- nBlockHeight %d blockReward %lld txoutServiceNodeRet %s txNew %s",
        nBlockHeight, blockReward, txoutServiceNodeRet.ToString(), txNew.ToString());
}

std::string GetRequiredPaymentsString(int nBlockHeight)
{
    // IF WE HAVE A ACTIVATED TRIGGER FOR THIS HEIGHT - IT IS A SUPERBLOCK, GET THE REQUIRED PAYEES
    if (CSuperblockManager::IsSuperblockTriggered(nBlockHeight)) {
        return CSuperblockManager::GetRequiredPaymentsString(nBlockHeight);
    }

    // OTHERWISE, PAY SERVICENODE
    return snpayments.GetRequiredPaymentsString(nBlockHeight);
}

void CServiceNodePayments::Clear()
{
    LOCK2(cs_mapServiceNodeBlocks, cs_mapServiceNodePaymentVotes);
    mapServiceNodeBlocks.clear();
    mapServiceNodePaymentVotes.clear();
}

bool CServiceNodePayments::UpdateLastVote(const CServiceNodePaymentVote& vote)
{
    LOCK(cs_mapServiceNodePaymentVotes);

    const auto it = mapServiceNodesLastVote.find(vote.servicenodeOutpoint);
    if (it != mapServiceNodesLastVote.end()) {
        if (it->second == vote.nBlockHeight)
            return false;
        it->second = vote.nBlockHeight;
        return true;
    }

    //record this servicenode voted
    mapServiceNodesLastVote.emplace(vote.servicenodeOutpoint, vote.nBlockHeight);
    return true;
}

/**
*   FillBlockPayee
*
*   Fill ServiceNode ONLY payment block
*/

void CServiceNodePayments::FillBlockPayee(CMutableTransaction& txNew, int nBlockHeight, CAmount blockReward, CTxOut& txoutServiceNodeRet) const
{
    // make sure it's not filled yet
    txoutServiceNodeRet = CTxOut();

    CScript payee;

    bool hasPayment = true;

    if (hasPayment && !snpayments.GetBlockPayee(nBlockHeight, payee)) {
        // no servicenode detected...
        int nCount = 0;
        servicenode_info_t dnInfo;
        if (!snodeman.GetNextServiceNodeInQueueForPayment(nBlockHeight, true, nCount, dnInfo)) {
            hasPayment = false;
            LogPrintf("CServiceNodePayments::FillBlockPayee: Failed to detect ServiceNode to pay\n");
            return;
        }
        // fill payee with locally calculated winner and hope for the best
        payee = GetScriptForDestination(dnInfo.pubKeyCollateralAddress.GetID());
    }

    // make sure it's not filled yet
    txoutServiceNodeRet = CTxOut();
    CAmount servicenodePayment = GetFluidServiceNodeReward(nBlockHeight);

    // split reward between miner ...
    txoutServiceNodeRet = CTxOut(servicenodePayment, payee);
    txNew.vout.push_back(txoutServiceNodeRet);
    // ... and servicenode
    CTxDestination address1;
    ExtractDestination(payee, address1);
    CCreditAddress address2(address1);

    LogPrintf("CServiceNodePayments::FillBlockPayee -- ServiceNode payment %ld to %s\n", FormatMoney(servicenodePayment), address2.ToString());
}

int CServiceNodePayments::GetMinServiceNodePaymentsProto() const
{
    return sporkManager.IsSporkActive(SPORK_10_SERVICENODE_PAY_UPDATED_NODES) ? MIN_SERVICENODE_PAYMENT_PROTO_VERSION_2 : MIN_SERVICENODE_PAYMENT_PROTO_VERSION_1;
}

void CServiceNodePayments::ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv, CConnman& connman)
{
    if (fLiteMode)
        return; // disable all Credit specific functionality

    if (strCommand == NetMsgType::SERVICENODEPAYMENTSYNC) { //ServiceNode Payments Request Sync

        if (pfrom->nVersion < GetMinServiceNodePaymentsProto()) {
            LogPrint("snpayments", "SERVICENODEPAYMENTSYNC -- peer=%d using obsolete version %i\n", pfrom->id, pfrom->nVersion);
            connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE, strprintf("Version must be %d or greater", GetMinServiceNodePaymentsProto())));
            return;
        }

        // Ignore such requests until we are fully synced.
        // We could start processing this after ServiceNode list is synced
        // but this is a heavy one so it's better to finish sync first.
        if (!servicenodeSync.IsSynced())
            return;

        // DEPRECATED, should be removed on next protocol bump
        //if (pfrom->nVersion == 71110) {
        //    int nCountNeeded;
        //    vRecv >> nCountNeeded;
        //}


        if (netfulfilledman.HasFulfilledRequest(pfrom->addr, NetMsgType::SERVICENODEPAYMENTSYNC)) {
            // Asking for the payments list multiple times in a short period of time is no good
            LogPrintf("SERVICENODEPAYMENTSYNC -- peer already asked me for the list, peer=%d\n", pfrom->id);
            Misbehaving(pfrom->GetId(), 20);
            return;
        }

        netfulfilledman.AddFulfilledRequest(pfrom->addr, NetMsgType::SERVICENODEPAYMENTSYNC);

        Sync(pfrom, connman);
        LogPrintf("SERVICENODEPAYMENTSYNC -- Sent ServiceNode payment votes to peer %d\n", pfrom->id);

    } else if (strCommand == NetMsgType::SERVICENODEPAYMENTVOTE) { // ServiceNode Payments Vote for the Winner

        CServiceNodePaymentVote vote;
        vRecv >> vote;

        if (pfrom->nVersion < GetMinServiceNodePaymentsProto()) {
            LogPrint("snpayments", "SERVICENODEPAYMENTVOTE -- peer=%d using obsolete version %i\n", pfrom->id, pfrom->nVersion);
            connman.PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::REJECT, strCommand, REJECT_OBSOLETE, strprintf("Version must be %d or greater", GetMinServiceNodePaymentsProto())));
            return;
        }

        uint256 nHash = vote.GetHash();

        pfrom->setAskFor.erase(nHash);

        // TODO: clear setAskFor for MSG_SERVICENODE_PAYMENT_BLOCK too

        // Ignore any payments messages until servicenode list is synced
        if (!servicenodeSync.IsServiceNodeListSynced())
            return;

        {
            LOCK(cs_mapServiceNodePaymentVotes);

            auto res = mapServiceNodePaymentVotes.emplace(nHash, vote);

            // Avoid processing same vote multiple times if it was already verified earlier
            if (!res.second && res.first->second.IsVerified()) {
                LogPrint("snpayments", "SERVICENODEPAYMENTVOTE -- hash=%s, nBlockHeight=%d/%d seen\n",
                    nHash.ToString(), vote.nBlockHeight, nCachedBlockHeight);
                return;
            }

            // Mark vote as non-verified when it's seen for the first time,
            // AddOrUpdatePaymentVote() below should take care of it if vote is actually ok
            res.first->second.MarkAsNotVerified();
        }

        int nFirstBlock = nCachedBlockHeight - GetStorageLimit();
        if (vote.nBlockHeight < nFirstBlock || vote.nBlockHeight > nCachedBlockHeight + 20) {
            LogPrint("snpayments", "SERVICENODEPAYMENTVOTE -- vote out of range: nFirstBlock=%d, nBlockHeight=%d, nHeight=%d\n", nFirstBlock, vote.nBlockHeight, nCachedBlockHeight);
            return;
        }

        std::string strError = "";
        if (!vote.IsValid(pfrom, nCachedBlockHeight, strError, connman)) {
            LogPrint("snpayments", "SERVICENODEPAYMENTVOTE -- invalid message, error: %s\n", strError);
            return;
        }

        servicenode_info_t dnInfo;
        if (!snodeman.GetServiceNodeInfo(vote.servicenodeOutpoint, dnInfo)) {
            // dn was not found, so we can't check vote, some info is probably missing
            LogPrintf("SERVICENODEPAYMENTVOTE -- ServiceNode is missing %s\n", vote.servicenodeOutpoint.ToStringShort());
            snodeman.AskForDN(pfrom, vote.servicenodeOutpoint, connman);
            return;
        }

        int nDos = 0;
        if (!vote.CheckSignature(dnInfo.pubKeyServiceNode, nCachedBlockHeight, nDos)) {
            if (nDos) {
                LOCK(cs_main);
                LogPrintf("SERVICENODEPAYMENTVOTE -- ERROR: invalid signature\n");
                Misbehaving(pfrom->GetId(), nDos);
            } else {
                // only warn about anything non-critical (i.e. nDos == 0) in debug mode
                LogPrint("snpayments", "SERVICENODEPAYMENTVOTE -- WARNING: invalid signature\n");
            }
            // Either our info or vote info could be outdated.
            // In case our info is outdated, ask for an update,
            snodeman.AskForDN(pfrom, vote.servicenodeOutpoint, connman);
            // but there is nothing we can do if vote info itself is outdated
            // (i.e. it was signed by a DN which changed its key),
            // so just quit here.
            return;
        }

        if (!UpdateLastVote(vote)) {
            LogPrintf("SERVICENODEPAYMENTVOTE -- servicenode already voted, servicenode=%s\n", vote.servicenodeOutpoint.ToStringShort());
            return;
        }

        CTxDestination address1;
        ExtractDestination(vote.payee, address1);
        CCreditAddress address2(address1);

        LogPrint("snpayments", "SERVICENODEPAYMENTVOTE -- vote: address=%s, nBlockHeight=%d, nHeight=%d, prevout=%s, hash=%s new\n",
            address2.ToString(), vote.nBlockHeight, nCachedBlockHeight, vote.servicenodeOutpoint.ToStringShort(), nHash.ToString());

        if (AddOrUpdatePaymentVote(vote)) {
            vote.Relay(connman);
            servicenodeSync.BumpAssetLastTime("SERVICENODEPAYMENTVOTE");
        }
    }
}

uint256 CServiceNodePaymentVote::GetHash() const
{
    // Note: doesn't match serialization

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << *(CScriptBase*)(&payee);
    ss << nBlockHeight;
    ss << servicenodeOutpoint;
    return ss.GetHash();
}

uint256 CServiceNodePaymentVote::GetSignatureHash() const
{
    return SerializeHash(*this);
}

bool CServiceNodePaymentVote::Sign()
{
    std::string strError;

    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
        uint256 hash = GetSignatureHash();

        if (!CHashSigner::SignHash(hash, activeServiceNode.keyServiceNode, vchSig)) {
            LogPrintf("CServiceNodePaymentVote::Sign -- SignHash() failed\n");
            return false;
        }

        if (!CHashSigner::VerifyHash(hash, activeServiceNode.pubKeyServiceNode, vchSig, strError)) {
            LogPrintf("CServiceNodePaymentVote::Sign -- VerifyHash() failed, error: %s\n", strError);
            return false;
        }
    } else {
        std::string strMessage = servicenodeOutpoint.ToStringShort() +
                                 std::to_string(nBlockHeight) +
                                 ScriptToAsmStr(payee);

        if (!CMessageSigner::SignMessage(strMessage, vchSig, activeServiceNode.keyServiceNode)) {
            LogPrintf("CServiceNodePaymentVote::Sign -- SignMessage() failed\n");
            return false;
        }

        if (!CMessageSigner::VerifyMessage(activeServiceNode.pubKeyServiceNode, vchSig, strMessage, strError)) {
            LogPrintf("CServiceNodePaymentVote::Sign -- VerifyMessage() failed, error: %s\n", strError);
            return false;
        }
    }

    return true;
}

bool CServiceNodePayments::GetBlockPayee(int nBlockHeight, CScript& payeeRet) const
{
    LOCK(cs_mapServiceNodeBlocks);

    auto it = mapServiceNodeBlocks.find(nBlockHeight);
    return it != mapServiceNodeBlocks.end() && it->second.GetBestPayee(payeeRet);
}

// Is this ServiceNode scheduled to get paid soon?
// -- Only look ahead up to 8 blocks to allow for propagation of the latest 2 blocks of votes
bool CServiceNodePayments::IsScheduled(const servicenode_info_t& dnInfo, int nNotBlockHeight) const
{
    LOCK(cs_mapServiceNodeBlocks);

    if (!servicenodeSync.IsServiceNodeListSynced())
        return false;

    CScript dnpayee;
    dnpayee = GetScriptForDestination(dnInfo.pubKeyCollateralAddress.GetID());

    CScript payee;
    for (int64_t h = nCachedBlockHeight; h <= nCachedBlockHeight + 8; h++) {
        if (h == nNotBlockHeight)
            continue;
        if (GetBlockPayee(h, payee) && dnpayee == payee) {
            return true;
        }
    }

    return false;
}

bool CServiceNodePayments::AddOrUpdatePaymentVote(const CServiceNodePaymentVote& vote)
{
    uint256 blockHash = uint256();
    if (!GetBlockHash(blockHash, vote.nBlockHeight - 101))
        return false;

    uint256 nVoteHash = vote.GetHash();

    if (HasVerifiedPaymentVote(nVoteHash))
        return false;

    LOCK2(cs_mapServiceNodeBlocks, cs_mapServiceNodePaymentVotes);

    mapServiceNodePaymentVotes[nVoteHash] = vote;

    auto it = mapServiceNodeBlocks.emplace(vote.nBlockHeight, CServiceNodeBlockPayees(vote.nBlockHeight)).first;
    it->second.AddPayee(vote);

    LogPrint("snpayments", "CServiceNodePayments::AddOrUpdatePaymentVote -- added, hash=%s\n", nVoteHash.ToString());

    return true;
}

bool CServiceNodePayments::HasVerifiedPaymentVote(const uint256& hashIn) const
{
    LOCK(cs_mapServiceNodePaymentVotes);
    const auto it = mapServiceNodePaymentVotes.find(hashIn);
    return it != mapServiceNodePaymentVotes.end() && it->second.IsVerified();
}

void CServiceNodeBlockPayees::AddPayee(const CServiceNodePaymentVote& vote)
{
    LOCK(cs_vecPayees);

    uint256 nVoteHash = vote.GetHash();

    for (auto& payee : vecPayees) {
        if (payee.GetPayee() == vote.payee) {
            payee.AddVoteHash(nVoteHash);
            return;
        }
    }
    CServiceNodePayee payeeNew(vote.payee, nVoteHash);
    vecPayees.push_back(payeeNew);
}

bool CServiceNodeBlockPayees::GetBestPayee(CScript& payeeRet) const
{
    LOCK(cs_vecPayees);

    if (!vecPayees.size()) {
        LogPrint("snpayments", "CServiceNodeBlockPayees::GetBestPayee -- ERROR: couldn't find any payee\n");
        return false;
    }

    int nVotes = -1;
    for (const auto& payee : vecPayees) {
        if (payee.GetVoteCount() > nVotes) {
            payeeRet = payee.GetPayee();
            nVotes = payee.GetVoteCount();
        }
    }

    return (nVotes > -1);
}

bool CServiceNodeBlockPayees::HasPayeeWithVotes(const CScript& payeeIn, int nVotesReq) const
{
    LOCK(cs_vecPayees);

    for (const auto& payee : vecPayees) {
        if (payee.GetVoteCount() >= nVotesReq && payee.GetPayee() == payeeIn) {
            return true;
        }
    }

    //LogPrint("snpayments", "CServiceNodeBlockPayees::HasPayeeWithVotes -- ERROR: couldn't find any payee with %d+ votes\n", nVotesReq);
    return false;
}

bool CServiceNodeBlockPayees::IsTransactionValid(const CTransaction& txNew, const int nHeight) const
{
    LOCK(cs_vecPayees);

    int nMaxSignatures = 0;
    std::string strPayeesPossible = "";
    CAmount nServiceNodePayment = GetFluidServiceNodeReward(nHeight);

    //require at least SNPAYMENTS_SIGNATURES_REQUIRED signatures

    for (const auto& payee : vecPayees) {
        if (payee.GetVoteCount() >= nMaxSignatures) {
            nMaxSignatures = payee.GetVoteCount();
        }
    }

    // if we don't have at least SNPAYMENTS_SIGNATURES_REQUIRED signatures on a payee, approve whichever is the longest chain
    if (nMaxSignatures < SNPAYMENTS_SIGNATURES_REQUIRED)
        return true;

    for (const auto& payee : vecPayees) {
        if (payee.GetVoteCount() >= SNPAYMENTS_SIGNATURES_REQUIRED) {
            for (const auto& txout : txNew.vout) {
                if (payee.GetPayee() == txout.scriptPubKey && nServiceNodePayment == txout.nValue) {
                    LogPrint("snpayments", "CServiceNodeBlockPayees::IsTransactionValid -- Found required payment\n");
                    return true;
                }
            }

            CTxDestination address1;
            ExtractDestination(payee.GetPayee(), address1);
            CCreditAddress address2(address1);

            if (strPayeesPossible == "") {
                strPayeesPossible = address2.ToString();
            } else {
                strPayeesPossible += "," + address2.ToString();
            }
        }
    }

    LogPrintf("CServiceNodeBlockPayees::IsTransactionValid -- ERROR: Missing required payment, possible payees: '%s', amount: %f 0AC\n", strPayeesPossible, (float)nServiceNodePayment / COIN);
    return false;
}

std::string CServiceNodeBlockPayees::GetRequiredPaymentsString() const
{
    LOCK(cs_vecPayees);

    std::string strRequiredPayments = "";

    for (const auto& payee : vecPayees) {
        CTxDestination address1;
        ExtractDestination(payee.GetPayee(), address1);
        CCreditAddress address2(address1);

        if (!strRequiredPayments.empty())
            strRequiredPayments += ", ";

        strRequiredPayments += strprintf("%s:%d", address2.ToString(), payee.GetVoteCount());
    }

    if (strRequiredPayments.empty())
        return "Unknown";

    return strRequiredPayments;
}

std::string CServiceNodePayments::GetRequiredPaymentsString(int nBlockHeight) const
{
    LOCK(cs_mapServiceNodeBlocks);

    const auto it = mapServiceNodeBlocks.find(nBlockHeight);
    return it == mapServiceNodeBlocks.end() ? "Unknown" : it->second.GetRequiredPaymentsString();
}

bool CServiceNodePayments::IsTransactionValid(const CTransaction& txNew, int nBlockHeight) const
{
    LOCK(cs_mapServiceNodeBlocks);

    const auto it = mapServiceNodeBlocks.find(nBlockHeight);
    return it == mapServiceNodeBlocks.end() ? true : it->second.IsTransactionValid(txNew, nBlockHeight);
}

void CServiceNodePayments::CheckAndRemove()
{
    if (!servicenodeSync.IsBlockchainSynced())
        return;

    LOCK2(cs_mapServiceNodeBlocks, cs_mapServiceNodePaymentVotes);

    int nLimit = GetStorageLimit();

    std::map<uint256, CServiceNodePaymentVote>::iterator it = mapServiceNodePaymentVotes.begin();
    while (it != mapServiceNodePaymentVotes.end()) {
        CServiceNodePaymentVote vote = (*it).second;

        if (nCachedBlockHeight - vote.nBlockHeight > nLimit) {
            LogPrint("snpayments", "CServiceNodePayments::CheckAndRemove -- Removing old ServiceNode payment: nBlockHeight=%d\n", vote.nBlockHeight);
            mapServiceNodePaymentVotes.erase(it++);
            mapServiceNodeBlocks.erase(vote.nBlockHeight);
        } else {
            ++it;
        }
    }
    LogPrint("snpayments", "CServiceNodePayments::CheckAndRemove -- %s\n", ToString());
}

bool CServiceNodePaymentVote::IsValid(CNode* pnode, int nValidationHeight, std::string& strError, CConnman& connman) const
{
    servicenode_info_t dnInfo;

    if (!snodeman.GetServiceNodeInfo(servicenodeOutpoint, dnInfo)) {
        strError = strprintf("Unknown servicenode=%s", servicenodeOutpoint.ToStringShort());
        // Only ask if we are already synced and still have no idea about that ServiceNode
        if (servicenodeSync.IsServiceNodeListSynced()) {
            snodeman.AskForDN(pnode, servicenodeOutpoint, connman);
        }

        return false;
    }

    int nMinRequiredProtocol;
    if (nBlockHeight >= nValidationHeight) {
        // new votes must comply SPORK_10_SERVICENODE_PAY_UPDATED_NODES rules
        nMinRequiredProtocol = snpayments.GetMinServiceNodePaymentsProto();
    } else {
        // allow non-updated servicenodes for old blocks
        nMinRequiredProtocol = MIN_SERVICENODE_PAYMENT_PROTO_VERSION_1;
    }

    if (dnInfo.nProtocolVersion < nMinRequiredProtocol) {
        strError = strprintf("ServiceNode protocol is too old: nProtocolVersion=%d, nMinRequiredProtocol=%d", dnInfo.nProtocolVersion, nMinRequiredProtocol);
        return false;
    }

    // Only servicenodes should try to check servicenode rank for old votes - they need to pick the right winner for future blocks.
    // Regular clients (miners included) need to verify servicenode rank for future block votes only.
    if (!fServiceNodeMode && nBlockHeight < nValidationHeight)
        return true;

    int nRank;

    if (!snodeman.GetServiceNodeRank(servicenodeOutpoint, nRank, nBlockHeight - 101, nMinRequiredProtocol)) {
        LogPrint("snpayments", "CServiceNodePaymentVote::IsValid -- Can't calculate rank for servicenode %s\n",
            servicenodeOutpoint.ToStringShort());
        return false;
    }

    if (nRank > SNPAYMENTS_SIGNATURES_TOTAL) {
        // It's common to have servicenodes mistakenly think they are in the top 10
        // We don't want to print all of these messages in normal mode, debug mode should print though
        strError = strprintf("ServiceNode %s is not in the top %d (%d)", servicenodeOutpoint.ToStringShort(), SNPAYMENTS_SIGNATURES_TOTAL, nRank);
        // Only ban for new dnw which is out of bounds, for old dnw DN list itself might be way too much off
        if (nRank > SNPAYMENTS_SIGNATURES_TOTAL * 2 && nBlockHeight > nValidationHeight) {
            LOCK(cs_main);
            strError = strprintf("ServiceNode %s is not in the top %d (%d)", servicenodeOutpoint.ToStringShort(), SNPAYMENTS_SIGNATURES_TOTAL * 2, nRank);
            LogPrintf("CServiceNodePaymentVote::IsValid -- Error: %s\n", strError);
            Misbehaving(pnode->GetId(), 20);
        }
        // Still invalid however
        return false;
    }

    return true;
}

bool CServiceNodePayments::ProcessBlock(int nBlockHeight, CConnman& connman)
{
    // DETERMINE IF WE SHOULD BE VOTING FOR THE NEXT PAYEE

    if (fLiteMode || !fServiceNodeMode)
        return false;

    // We have little chances to pick the right winner if winners list is out of sync
    // but we have no choice, so we'll try. However it doesn't make sense to even try to do so
    // if we have not enough data about ServiceNodes.
    if (!servicenodeSync.IsServiceNodeListSynced())
        return false;

    int nRank;

    if (!snodeman.GetServiceNodeRank(activeServiceNode.outpoint, nRank, nBlockHeight - 101, GetMinServiceNodePaymentsProto())) {
        LogPrint("snpayments", "CServiceNodePayments::ProcessBlock -- Unknown ServiceNode\n");
        return false;
    }

    if (nRank > SNPAYMENTS_SIGNATURES_TOTAL) {
        LogPrint("snpayments", "CServiceNodePayments::ProcessBlock -- ServiceNode not in the top %d (%d)\n", SNPAYMENTS_SIGNATURES_TOTAL, nRank);
        return false;
    }


    // LOCATE THE NEXT SERVICENODE WHICH SHOULD BE PAID

    LogPrint("snpayments", "CServiceNodePayments::ProcessBlock -- Start: nBlockHeight=%d, servicenode=%s\n", nBlockHeight, activeServiceNode.outpoint.ToStringShort());

    // pay to the oldest DN that still had no payment but its input is old enough and it was active long enough
    int nCount = 0;
    servicenode_info_t dnInfo;

    if (!snodeman.GetNextServiceNodeInQueueForPayment(nBlockHeight, true, nCount, dnInfo)) {
        LogPrintf("CServiceNodePayments::ProcessBlock -- ERROR: Failed to find ServiceNode to pay\n");
        return false;
    }

    LogPrint("snpayments", "CServiceNodePayments::ProcessBlock -- ServiceNode found by GetNextServiceNodeInQueueForPayment(): %s\n", dnInfo.outpoint.ToStringShort());

    CScript payee = GetScriptForDestination(dnInfo.pubKeyCollateralAddress.GetID());

    CServiceNodePaymentVote voteNew(activeServiceNode.outpoint, nBlockHeight, payee);

    CTxDestination address1;
    ExtractDestination(payee, address1);
    CCreditAddress address2(address1);

    LogPrint("snpayments", "CServiceNodePayments::ProcessBlock -- vote: payee=%s, nBlockHeight=%d\n", address2.ToString(), nBlockHeight);

    // SIGN MESSAGE TO NETWORK WITH OUR SERVICENODE KEYS

    LogPrint("snpayments", "CServiceNodePayments::ProcessBlock -- Signing vote\n");
    if (voteNew.Sign()) {
        LogPrint("snpayments", "CServiceNodePayments::ProcessBlock -- AddPaymentVote()\n");

        if (AddOrUpdatePaymentVote(voteNew)) {
            voteNew.Relay(connman);
            return true;
        }
    }

    return false;
}

void CServiceNodePayments::CheckBlockVotes(int nBlockHeight)
{
    if (!servicenodeSync.IsWinnersListSynced())
        return;

    CServiceNodeMan::rank_pair_vec_t dns;
    if (!snodeman.GetServiceNodeRanks(dns, nBlockHeight - 101, GetMinServiceNodePaymentsProto())) {
        LogPrintf("CServiceNodePayments::CheckBlockVotes -- nBlockHeight=%d, GetServiceNodeRanks failed\n", nBlockHeight);
        return;
    }

    std::string debugStr;

    debugStr += strprintf("CServiceNodePayments::CheckBlockVotes -- nBlockHeight=%d,\n  Expected voting DNs:\n", nBlockHeight);

    LOCK2(cs_mapServiceNodeBlocks, cs_mapServiceNodePaymentVotes);

    int i{0};
    for (const auto& dn : dns) {
        CScript payee;
        bool found = false;

        const auto it = mapServiceNodeBlocks.find(nBlockHeight);
        if (it != mapServiceNodeBlocks.end()) {
            for (const auto& p : it->second.vecPayees) {
                for (const auto& voteHash : p.GetVoteHashes()) {
                    const auto itVote = mapServiceNodePaymentVotes.find(voteHash);
                    if (itVote == mapServiceNodePaymentVotes.end()) {
                        debugStr += strprintf("    - could not find vote %s\n",
                            voteHash.ToString());
                        continue;
                    }
                    if (itVote->second.servicenodeOutpoint == dn.second.outpoint) {
                        payee = itVote->second.payee;
                        found = true;
                        break;
                    }
                }
            }
        }

        if (found) {
            CTxDestination address1;
            ExtractDestination(payee, address1);
            CCreditAddress address2(address1);

            debugStr += strprintf("    - %s - voted for %s\n",
                dn.second.outpoint.ToStringShort(), address2.ToString());
        } else {
            mapServiceNodesDidNotVote.emplace(dn.second.outpoint, 0).first->second++;

            debugStr += strprintf("    - %s - no vote received\n",
                dn.second.outpoint.ToStringShort());
        }

        if (++i >= SNPAYMENTS_SIGNATURES_TOTAL)
            break;
    }

    if (mapServiceNodesDidNotVote.empty()) {
        LogPrint("snpayments", "%s", debugStr);
        return;
    }

    debugStr += "  ServiceNodes which missed a vote in the past:\n";
    for (const auto& item : mapServiceNodesDidNotVote) {
        debugStr += strprintf("    - %s: %d\n", item.first.ToStringShort(), item.second);
    }

    LogPrint("snpayments", "%s", debugStr);
}

void CServiceNodePaymentVote::Relay(CConnman& connman) const
{
    // Do not relay until fully synced
    if (!servicenodeSync.IsSynced()) {
        LogPrint("snpayments", "CServiceNodePayments::Relay -- won't relay until fully synced\n");
        return;
    }

    CInv inv(MSG_SERVICENODE_PAYMENT_VOTE, GetHash());
    connman.RelayInv(inv);
}

bool CServiceNodePaymentVote::CheckSignature(const CPubKey& pubKeyServiceNode, int nValidationHeight, int& nDos) const
{
    // do not ban by default
    nDos = 0;
    std::string strError = "";

    if (sporkManager.IsSporkActive(SPORK_6_NEW_SIGS)) {
        uint256 hash = GetSignatureHash();

        if (!CHashSigner::VerifyHash(hash, pubKeyServiceNode, vchSig, strError)) {
            // could be a signature in old format
            std::string strMessage = servicenodeOutpoint.ToStringShort() +
                                     boost::lexical_cast<std::string>(nBlockHeight) +
                                     ScriptToAsmStr(payee);
            if (!CMessageSigner::VerifyMessage(pubKeyServiceNode, vchSig, strMessage, strError)) {
                // nope, not in old format either
                // Only ban for future block vote when we are already synced.
                // Otherwise it could be the case when DN which signed this vote is using another key now
                // and we have no idea about the old one.
                if (servicenodeSync.IsServiceNodeListSynced() && nBlockHeight > nValidationHeight) {
                    nDos = 20;
                }
                return error("CServiceNodePaymentVote::CheckSignature -- Got bad ServiceNode payment signature, servicenode=%s, error: %s",
                    servicenodeOutpoint.ToStringShort(), strError);
            }
        }
    } else {
        std::string strMessage = servicenodeOutpoint.ToStringShort() +
                                 boost::lexical_cast<std::string>(nBlockHeight) +
                                 ScriptToAsmStr(payee);

        if (!CMessageSigner::VerifyMessage(pubKeyServiceNode, vchSig, strMessage, strError)) {
            // Only ban for future block vote when we are already synced.
            // Otherwise it could be the case when DN which signed this vote is using another key now
            // and we have no idea about the old one.
            if (servicenodeSync.IsServiceNodeListSynced() && nBlockHeight > nValidationHeight) {
                nDos = 20;
            }
            return error("CServiceNodePaymentVote::CheckSignature -- Got bad ServiceNode payment signature, servicenode=%s, error: %s",
                servicenodeOutpoint.ToStringShort(), strError);
        }
    }

    return true;
}

std::string CServiceNodePaymentVote::ToString() const
{
    std::ostringstream info;

    info << servicenodeOutpoint.ToStringShort() << ", " << nBlockHeight << ", " << ScriptToAsmStr(payee) << ", " << (int)vchSig.size();

    return info.str();
}

// Send all votes up to nCountNeeded blocks (but not more than GetStorageLimit)
void CServiceNodePayments::Sync(CNode* pnode, CConnman& connman) const
{
    LOCK(cs_mapServiceNodeBlocks);

    if (!servicenodeSync.IsWinnersListSynced())
        return;

    int nInvCount = 0;

    for (int h = nCachedBlockHeight; h < nCachedBlockHeight + 20; h++) {
        const auto it = mapServiceNodeBlocks.find(h);
        if (it != mapServiceNodeBlocks.end()) {
            for (const auto& payee : it->second.vecPayees) {
                std::vector<uint256> vecVoteHashes = payee.GetVoteHashes();
                for (const auto& hash : vecVoteHashes) {
                    if (!HasVerifiedPaymentVote(hash))
                        continue;
                    pnode->PushInventory(CInv(MSG_SERVICENODE_PAYMENT_VOTE, hash));
                    nInvCount++;
                }
            }
        }
    }

    LogPrintf("CServiceNodePayments::Sync -- Sent %d votes to peer=%d\n", nInvCount, pnode->id);
    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    connman.PushMessage(pnode, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, SERVICENODE_SYNC_DNW, nInvCount));
}
// Request low data/unknown payment blocks in batches directly from some node instead of/after preliminary Sync.
void CServiceNodePayments::RequestLowDataPaymentBlocks(CNode* pnode, CConnman& connman) const
{
    if (!servicenodeSync.IsServiceNodeListSynced())
        return;

    CNetMsgMaker msgMaker(pnode->GetSendVersion());
    LOCK2(cs_main, cs_mapServiceNodeBlocks);

    std::vector<CInv> vToFetch;
    int nLimit = GetStorageLimit();

    const CBlockIndex* pindex = chainActive.Tip();

    while (nCachedBlockHeight - pindex->nHeight < nLimit) {
        if (!mapServiceNodeBlocks.count(pindex->nHeight)) {
            // We have no idea about this block height, let's ask
            vToFetch.push_back(CInv(MSG_SERVICENODE_PAYMENT_BLOCK, pindex->GetBlockHash()));
            // We should not violate GETDATA rules
            if (vToFetch.size() == MAX_INV_SZ) {
                LogPrint("snpayments", "CServiceNodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d blocks\n", pnode->id, MAX_INV_SZ);
                connman.PushMessage(pnode, msgMaker.Make(NetMsgType::GETDATA, vToFetch));
                // Start filling new batch
                vToFetch.clear();
            }
        }
        if (!pindex->pprev)
            break;
        pindex = pindex->pprev;
    }

    for (auto& dnBlockPayees : mapServiceNodeBlocks) {
        int nBlockHeight = dnBlockPayees.first;
        int nTotalVotes = 0;
        bool fFound = false;
        for (const auto& payee : dnBlockPayees.second.vecPayees) {
            if (payee.GetVoteCount() >= SNPAYMENTS_SIGNATURES_REQUIRED) {
                fFound = true;
                break;
            }
            nTotalVotes += payee.GetVoteCount();
        }
        // A clear winner (SNPAYMENTS_SIGNATURES_REQUIRED+ votes) was found
        // or no clear winner was found but there are at least avg number of votes
        if (fFound || nTotalVotes >= (SNPAYMENTS_SIGNATURES_TOTAL + SNPAYMENTS_SIGNATURES_REQUIRED) / 2) {
            // so just move to the next block
            continue;
        }
        // DEBUG
        DBG(
            // Let's see why this failed
            for (const auto& payee
                 : dnBlockPayees.second.vecPayees) {
                CTxDestination address1;
                ExtractDestination(payee.GetPayee(), address1);
                CCreditAddress address2(address1);
                printf("payee %s votes %d\n", address2.ToString().c_str(), payee.GetVoteCount());
            } printf("block %d votes total %d\n", it->first, nTotalVotes);)
        // END DEBUG
        // Low data block found, let's try to sync it
        uint256 hash;
        if (GetBlockHash(hash, nBlockHeight)) {
            vToFetch.push_back(CInv(MSG_SERVICENODE_PAYMENT_BLOCK, hash));
        }
        // We should not violate GETDATA rules
        if (vToFetch.size() == MAX_INV_SZ) {
            LogPrint("snpayments", "CServiceNodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d payment blocks\n", pnode->id, MAX_INV_SZ);
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::GETDATA, vToFetch));
            // Start filling new batch
            vToFetch.clear();
        }
    }
    // Ask for the rest of it
    if (!vToFetch.empty()) {
        LogPrint("snpayments", "CServiceNodePayments::SyncLowDataPaymentBlocks -- asking peer %d for %d payment blocks\n", pnode->id, vToFetch.size());
        connman.PushMessage(pnode, msgMaker.Make(NetMsgType::GETDATA, vToFetch));
    }
}

std::string CServiceNodePayments::ToString() const
{
    std::ostringstream info;

    info << "Votes: " << (int)mapServiceNodePaymentVotes.size() << ", Blocks: " << (int)mapServiceNodeBlocks.size();

    return info.str();
}

bool CServiceNodePayments::IsEnoughData() const
{
    float nAverageVotes = (SNPAYMENTS_SIGNATURES_TOTAL + SNPAYMENTS_SIGNATURES_REQUIRED) / 2;
    int nStorageLimit = GetStorageLimit();
    return GetBlockCount() > nStorageLimit && GetVoteCount() > nStorageLimit * nAverageVotes;
}

int CServiceNodePayments::GetStorageLimit() const
{
    return std::max(int(snodeman.size() * nStorageCoeff), nMinBlocksToStore);
}

void CServiceNodePayments::UpdatedBlockTip(const CBlockIndex* pindex, CConnman& connman)
{
    if (!pindex)
        return;

    nCachedBlockHeight = pindex->nHeight;
    LogPrint("snpayments", "CServiceNodePayments::UpdatedBlockTip -- nCachedBlockHeight=%d\n", nCachedBlockHeight);

    int nFutureBlock = nCachedBlockHeight + 10;

    CheckBlockVotes(nFutureBlock - 1);
    ProcessBlock(nFutureBlock, connman);
}

void CServiceNodePayments::DoMaintenance()
{
    if (ShutdownRequested()) return;
     CheckAndRemove();
}
