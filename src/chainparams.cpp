
// Copyright (c) 2009-2019 Satoshi Nakamoto
// Copyright (c) 2009-2019 The Bitcoin Developers
// Copyright (c) 2014-2019 The Dash Core Developers
// Copyright (c) 2016-2019 Duality Blockchain Solutions Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"

#include "arith_uint256.h"
#include "consensus/merkle.h"
#include "hash.h"
#include "streams.h"

#include "tinyformat.h"
#include "util.h"
#include "utilstrencodings.h"

#include "uint256.h"

#include <assert.h>

#include <boost/assign/list_of.hpp>

#include "chainparamsseeds.h"

const arith_uint256 maxUint = UintToArith256(uint256S("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, const uint32_t nTime, const uint32_t nNonce, const uint32_t nBits, const int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.nVersion = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 1591833600 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime = nTime;
    genesis.nBits = nBits;
    genesis.nNonce = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

static void MineGenesis(CBlockHeader& genesisBlock, const uint256& powLimit, bool noProduction)
{
    if (noProduction)
        genesisBlock.nTime = std::time(0);
    genesisBlock.nNonce = 0;

    printf("NOTE: Genesis nTime = %u \n", genesisBlock.nTime);
    printf("WARN: Genesis nNonce (BLANK!) = %u \n", genesisBlock.nNonce);

    arith_uint256 besthash = maxUint;
    arith_uint256 hashTarget = UintToArith256(powLimit);
    printf("Target: %s\n", hashTarget.GetHex().c_str());
    arith_uint256 newhash = UintToArith256(genesisBlock.GetHash());
    while (newhash > hashTarget) {
        genesisBlock.nNonce++;
        if (genesisBlock.nNonce == 0) {
            printf("NONCE WRAPPED, incrementing time\n");
            ++genesisBlock.nTime;
        }
        // If nothing found after trying for a while, print status
        if ((genesisBlock.nNonce & 0xfff) == 0)
            printf("nonce %08X: hash = %s (target = %s)\n",
                genesisBlock.nNonce, newhash.ToString().c_str(),
                hashTarget.ToString().c_str());

        if (newhash < besthash) {
            besthash = newhash;
            printf("New best: %s\n", newhash.GetHex().c_str());
        }
        newhash = UintToArith256(genesisBlock.GetHash());
    }
    printf("Genesis nTime = %u \n", genesisBlock.nTime);
    printf("Genesis nNonce = %u \n", genesisBlock.nNonce);
    printf("Genesis nBits: %08x\n", genesisBlock.nBits);
    printf("Genesis Hash = %s\n", newhash.ToString().c_str());
    printf("Genesis Hash Merkle Root = %s\n", genesisBlock.hashMerkleRoot.ToString().c_str());
}

/**
 * Build the genesis block. Note that the output of its generation
 * transaction cannot be spent since it did not originally exist in the
 * database.
 *
 * CBlock(hash=00000ffd590b14, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=e0028e, nTime=1390095618, nBits=1e0ffff0, nNonce=28917698, vtx=1)
 *   CTransaction(hash=e0028e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d01044c5957697265642030392f4a616e2f3230313420546865204772616e64204578706572696d656e7420476f6573204c6976653a204f76657273746f636b2e636f6d204973204e6f7720416363657074696e6720426974636f696e73)
 *     CTxOut(nValue=50.00000000, scriptPubKey=0xA9037BAC7050C479B121CF)
 *   vMerkleTree: e0028e
 */
static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "Put on the full armor of God, so that you can take your stand against the devil's schemes";
    const CScript genesisOutputScript = CScript() << ParseHex("") << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

/**
 * Main network
 */

class CMainParams : public CChainParams
{
public:
    CMainParams()
    {
        strNetworkID = "main";
        consensus.nRewardsStart = 5;               // PoW Rewards begin on block 5
        consensus.nServiceNodePaymentsStartBlock = 32000;  // ServiceNode Payments begin on block 32000
        consensus.nMinCountServiceNodesPaymentStart = 1;  // ServiceNode Payments begin once 1 ServiceNodes exist or more.

        consensus.nInstantSendConfirmationsRequired = 11;
        consensus.nInstantSendKeepLock = 24;

        consensus.nBudgetPaymentsStartBlock = 10800;   // actual historical value
        consensus.nBudgetPaymentsCycleBlocks = 87660; //Blocks per month
        consensus.nBudgetPaymentsWindowBlocks = 100;
        consensus.nBudgetProposalEstablishingTime = 24 * 60 * 60;

        consensus.nSuperblockStartBlock = 10800;
        consensus.nSuperblockStartHash = uint256S("");
        consensus.nSuperblockCycle = 87660; // 2880 (Blocks per day) x 365.25 (Days per Year) / 12 = 87660

        consensus.nGovernanceMinQuorum = 10;
        consensus.nGovernanceFilterElements = 20000;

        consensus.nServiceNodeMinimumConfirmations = 15;

        consensus.nMajorityEnforceBlockUpgrade = 750;
        consensus.nMajorityRejectBlockOutdated = 950;
        consensus.nMajorityWindow = 1000;

        consensus.powLimit = uint256S("00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowTargetTimespan = 30 * 64; // Credit: 1920 seconds
        consensus.nPowTargetSpacing = DEFAULT_AVERAGE_POW_BLOCK_TIME;
        consensus.nUpdateDiffAlgoHeight = 10; // Credit: Algorithm fork block
        consensus.nPowAveragingWindow = 5;
        consensus.nPowMaxAdjustUp = 32;
        consensus.nPowMaxAdjustDown = 48;
        assert(maxUint / UintToArith256(consensus.powLimit) >= consensus.nPowAveragingWindow);
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        consensus.nRuleChangeActivationThreshold = 321; // 95% of nMinerConfirmationWindow
        consensus.nMinerConfirmationWindow = 30;        // nPowTargetTimespan / nPowTargetSpacing

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999;   // December 31, 2008

        // Deployment of BIP68, BIP112, and BIP113.
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 1513591800; // Dec 18th 2017 10:10:00
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 1545134400;   // Dec 18th 2018 12:00:00

        // Deployment of BIP147
        consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nStartTime = 1533945600; // Aug 11th, 2018
        consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nTimeout = 1565481600; // Aug 11th, 2019
        consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nWindowSize = 4032;
        consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nThreshold = 3226; // 80% of 4032

        // Deployment of InstantSend autolocks
        consensus.vDeployments[Consensus::DEPLOYMENT_ISAUTOLOCKS].bit = 4;
        consensus.vDeployments[Consensus::DEPLOYMENT_ISAUTOLOCKS].nStartTime = 1533945600; // Aug 11th, 2018
        consensus.vDeployments[Consensus::DEPLOYMENT_ISAUTOLOCKS].nTimeout = 1565481600; // Aug 11th, 2019
        consensus.vDeployments[Consensus::DEPLOYMENT_ISAUTOLOCKS].nWindowSize = 4032;
        consensus.vDeployments[Consensus::DEPLOYMENT_ISAUTOLOCKS].nThreshold = 3226; // 80% of 4032

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = 0;

        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0x"); // 500

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0x65;
        pchMessageStart[1] = 0x67;
        pchMessageStart[2] = 0x7a;
        pchMessageStart[3] = 0x85;
        vAlertPubKey = ParseHex("045d786a68d9da26e8edb3f770639e409d200a9aaab9a051dea55f8ff7886ee91df21d4e3ee96f8bdbe38558bf641995dcecfa37d2343cf112bf15c2c03ee322e1");
        nDefaultPort = DEFAULT_P2P_PORT;
        nPruneAfterHeight = 87660;
        startNewChain = false;

        genesis = CreateGenesisBlock(1591830336, 1102454, UintToArith256(consensus.powLimit).GetCompact(), 1, (1 * COIN));
        if (startNewChain == true) {
            MineGenesis(genesis, consensus.powLimit, true);
        }

        consensus.hashGenesisBlock = genesis.GetHash();

        if (!startNewChain) {
            assert(consensus.hashGenesisBlock == uint256S("0x0000049d099254d27f72451dd68377969aaef8b6cb6d673859ee9c9e8ec437a6"));
            assert(genesis.hashMerkleRoot == uint256S("0xcf42d6a146359c277a6620e802ff3511ec0877c588fd5e26d996e431befb966c"));
        }

        vSeeds.push_back(CDNSSeedData("", ""));

        // Credit addresses start with 'C'
        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 28);
        // Credit script addresses start with '5'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 10);
        // Credit private keys start with 'y'
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 140);
        // Credit BIP32 pubkeys start with 'xpub' (Bitcoin defaults)
        base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x04)(0x88)(0xB2)(0x1E).convert_to_container<std::vector<unsigned char> >();
        // Credit BIP32 prvkeys start with 'xprv' (Bitcoin defaults)
        base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x04)(0x88)(0xAD)(0xE4).convert_to_container<std::vector<unsigned char> >();
        // Credit Stealth Address start with 'L'
        base58Prefixes[STEALTH_ADDRESS] = {0x0F};
        // Credit BIP44 coin type is '5'
        nExtCoinType = 5;

        vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_main, pnSeed6_main + ARRAYLEN(pnSeed6_main));

        fMiningRequiresPeers = true;
        fDefaultConsistencyChecks = false;
        fRequireStandard = true;
        fRequireRoutableExternalIP = true;
        fMineBlocksOnDemand = false;
        fAllowMultipleAddressesFromGroup = false;
        fAllowMultiplePorts = false;

        nPoolMaxTransactions = 3;
        nFulfilledRequestExpireTime = 60 * 60; // fulfilled requests expire in 1 hour

        vSporkAddresses = {"CJRBs6AwoRqYsfoyMKMCaVziDdHVFrCKE1"};
        nMinSporkKeys = 1;

        checkpointData = (CCheckpointData){
            boost::assign::map_list_of
	    (0, uint256S("0x0000049d099254d27f72451dd68377969aaef8b6cb6d673859ee9c9e8ec437a6"))
	};

        chainTxData = ChainTxData{
            0,		// * UNIX timestamp of last known number of transactions
            0,  	// * total number of transactions between genesis and that timestamp
                	//   (the tx=... number in the SetBestChain debug.log lines)
            0.1 	// * estimated number of transactions per second after that timestamp
        };
    }
};
static CMainParams mainParams;

/**
 * Testnet (v3)
 */
class CTestNetParams : public CChainParams
{
public:
    CTestNetParams()
    {
        strNetworkID = "test";

        consensus.nRewardsStart = 0; // Rewards starts on block 0
        consensus.nServiceNodePaymentsStartBlock = 0;
        consensus.nMinCountServiceNodesPaymentStart = 1; // ServiceNode Payments begin once 1 ServiceNode exists or more.

        consensus.nInstantSendConfirmationsRequired = 11;
        consensus.nInstantSendKeepLock = 24;

        consensus.nBudgetPaymentsStartBlock = 200;
        consensus.nBudgetPaymentsCycleBlocks = 50;
        consensus.nBudgetPaymentsWindowBlocks = 10;
        consensus.nBudgetProposalEstablishingTime = 60 * 20;

        consensus.nSuperblockStartBlock = 0;
        consensus.nSuperblockStartHash = uint256(); // do not check this on testnet
        consensus.nSuperblockCycle = 24;            // Superblocks can be issued hourly on testnet

        consensus.nGovernanceMinQuorum = 1;
        consensus.nGovernanceFilterElements = 500;

        consensus.nServiceNodeMinimumConfirmations = 1;

        consensus.nMajorityEnforceBlockUpgrade = 510;
        consensus.nMajorityRejectBlockOutdated = 750;
        consensus.nMajorityWindow = 1000;

        consensus.powLimit = uint256S("00ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowAveragingWindow = 5;
        consensus.nPowMaxAdjustUp = 32;
        consensus.nPowMaxAdjustDown = 48;
        consensus.nPowTargetTimespan = 30 * 64; // Credit: 1920 seconds
        consensus.nPowTargetSpacing = DEFAULT_AVERAGE_POW_BLOCK_TIME;
        consensus.nUpdateDiffAlgoHeight = 10; // Credit: Algorithm fork block
        assert(maxUint / UintToArith256(consensus.powLimit) >= consensus.nPowAveragingWindow);
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = false;
        consensus.nRuleChangeActivationThreshold = 254; // 75% of nMinerConfirmationWindow
        consensus.nMinerConfirmationWindow = 30;        // nPowTargetTimespan / nPowTargetSpacing

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999;   // December 31, 2008

        // Deployment of BIP68, BIP112, and BIP113.
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 1513591800; // Dec 18th 2017 10:10:00
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 1545134400;   // Dec 18th 2018 12:00:00

        // Deployment of BIP147
        consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nStartTime = 1517792400; // Feb 5th, 2018
        consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nTimeout = 1549328400; // Feb 5th, 2019
        consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nWindowSize = 100;
        consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nThreshold = 50; // 50% of 100

        // Deployment of InstantSend autolocks
        consensus.vDeployments[Consensus::DEPLOYMENT_ISAUTOLOCKS].bit = 4;
        consensus.vDeployments[Consensus::DEPLOYMENT_ISAUTOLOCKS].nStartTime = 1532476800; // Jul 25th, 2018
        consensus.vDeployments[Consensus::DEPLOYMENT_ISAUTOLOCKS].nTimeout = 1564012800; // Jul 25th, 2019
        consensus.vDeployments[Consensus::DEPLOYMENT_ISAUTOLOCKS].nWindowSize = 100;
        consensus.vDeployments[Consensus::DEPLOYMENT_ISAUTOLOCKS].nThreshold = 50; // 50% of 100

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = 0; // 0

        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0x"); // 50

        pchMessageStart[0] = 0x35;
        pchMessageStart[1] = 0x38;
        pchMessageStart[2] = 0x1b;
        pchMessageStart[3] = 0x45;
        vAlertPubKey = ParseHex("04e6a381e79100342992ff72e825962b8dcfee89e7586aa77760f4ec4a1a7c222e40fefbccd33403d7febbfe9c99a5e4553c2615e9cdc29612ed56836f50a7a2f4");
        nDefaultPort = DEFAULT_P2P_PORT + 100;
        nPruneAfterHeight = 100;
        startNewChain = false;

        genesis = CreateGenesisBlock(1591831730, 22, UintToArith256(consensus.powLimit).GetCompact(), 1, (1 * COIN));
        if (startNewChain == true) {
            MineGenesis(genesis, consensus.powLimit, true);
        }

        consensus.hashGenesisBlock = genesis.GetHash();

        if (!startNewChain) {
            assert(consensus.hashGenesisBlock == uint256S("0x001e293da952831f70af634c8bfb8c580a1d1e0d0ba1151a74f7e5d727c29262"));
            assert(genesis.hashMerkleRoot == uint256S("0xcf42d6a146359c277a6620e802ff3511ec0877c588fd5e26d996e431befb966c"));
        }
        vFixedSeeds.clear();
        vSeeds.clear();
        //vSeeds.push_back(CDNSSeedData("",  ""));
        //vSeeds.push_back(CDNSSeedData("", ""));

        // Testnet Credit addresses start with 'c'
        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 87);
        // Testnet Credit script addresses start with '8' or '9'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 10);
        // Testnet private keys start with '9' or 'c' (Bitcoin defaults)
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 158);
        // Testnet Credit BIP32 pubkeys start with 'tpub' (Bitcoin defaults)
        base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x04)(0x35)(0x87)(0xCF).convert_to_container<std::vector<unsigned char> >();
        // Testnet Credit BIP32 prvkeys start with 'tprv' (Bitcoin defaults)
        base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x04)(0x35)(0x83)(0x94).convert_to_container<std::vector<unsigned char> >();
        // Credit Stealth Address start with 'T'
        base58Prefixes[STEALTH_ADDRESS] = {0x15};
        // Testnet Credit BIP44 coin type is '1' (All coin's testnet default)
        nExtCoinType = 1;

        vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_test, pnSeed6_test + ARRAYLEN(pnSeed6_test));

        fMiningRequiresPeers = false;
        fDefaultConsistencyChecks = false;
        fRequireStandard = false;
        fRequireRoutableExternalIP = true;
        fMineBlocksOnDemand = false;
        fAllowMultipleAddressesFromGroup = false;
        fAllowMultiplePorts = false;

        nPoolMaxTransactions = 3;
        nFulfilledRequestExpireTime = 5 * 60; // fulfilled requests expire in 5 minutes
        vSporkAddresses = {"cE9XsKfm3Y9t7jTJ9SzAToiD8CDHZFAd2o"};
        nMinSporkKeys = 1;

        checkpointData = (CCheckpointData){
            boost::assign::map_list_of
            (0, uint256S("0x"))
	};

        chainTxData = ChainTxData{
            0,		// * UNIX timestamp of last known number of transactions
            0,  	// * total number of transactions between genesis and that timestamp
                	//   (the tx=... number in the SetBestChain debug.log lines)
            0.1 	// * estimated number of transactions per second after that timestamp
        };
    }
};
static CTestNetParams testNetParams;

/**
 * Regression test
 */
class CRegTestParams : public CChainParams
{
public:
    CRegTestParams()
    {
        strNetworkID = "regtest";
        consensus.nRewardsStart = 0; // Rewards starts on block 0
        consensus.nServiceNodePaymentsStartBlock = 0;
        consensus.nMinCountServiceNodesPaymentStart = 1; // ServiceNode Payments begin once 1 ServiceNode exists or more.

        consensus.nInstantSendConfirmationsRequired = 11;
        consensus.nInstantSendKeepLock = 24;

        consensus.nBudgetPaymentsStartBlock = 1000;
        consensus.nBudgetPaymentsCycleBlocks = 50;
        consensus.nBudgetPaymentsWindowBlocks = 10;
        consensus.nBudgetProposalEstablishingTime = 60 * 20;

        consensus.nSuperblockStartBlock = 0;
        consensus.nSuperblockStartHash = uint256(); // do not check this on regtest
        consensus.nSuperblockCycle = 10;

        consensus.nGovernanceMinQuorum = 1;
        consensus.nGovernanceFilterElements = 100;

        consensus.nServiceNodeMinimumConfirmations = 1;

        consensus.nMajorityEnforceBlockUpgrade = 750;
        consensus.nMajorityRejectBlockOutdated = 950;
        consensus.nMajorityWindow = 1000;

        consensus.powLimit = uint256S("000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowAveragingWindow = 5;
        consensus.nPowMaxAdjustUp = 32;
        consensus.nPowMaxAdjustDown = 48;
        consensus.nPowTargetTimespan = 30 * 64; // Credit: 1920 seconds
        consensus.nPowTargetSpacing = DEFAULT_AVERAGE_POW_BLOCK_TIME;
        consensus.nUpdateDiffAlgoHeight = 10; // Credit: Algorithm fork block
        assert(maxUint / UintToArith256(consensus.powLimit) >= consensus.nPowAveragingWindow);
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = true;
        consensus.nRuleChangeActivationThreshold = 254; // 75% of nMinerConfirmationWindow
        consensus.nMinerConfirmationWindow = 30;        // nPowTargetTimespan / nPowTargetSpacing

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 999999999999ULL;

        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 999999999999ULL;

        consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].bit = 2;
        consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_BIP147].nTimeout = 999999999999ULL;

        consensus.vDeployments[Consensus::DEPLOYMENT_ISAUTOLOCKS].bit = 4;
        consensus.vDeployments[Consensus::DEPLOYMENT_ISAUTOLOCKS].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_ISAUTOLOCKS].nTimeout = 999999999999ULL;

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = 0;

        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0x");

        pchMessageStart[0] = 0x37;
        pchMessageStart[1] = 0x3a;
        pchMessageStart[2] = 0x1d;
        pchMessageStart[3] = 0x47;
        vAlertPubKey = ParseHex("043ec9f8941d64bcb9abe169aad9400ca3bc713d0b89ccd5dd6f54fac98831cec17fc87f4665fc83ace24e7081f5b6f0ee6cf946195174ba1bdf1eda650573c632");
        nDefaultPort = DEFAULT_P2P_PORT + 200;
        nPruneAfterHeight = 100;
        startNewChain = false;

        genesis = CreateGenesisBlock(1591831730, 744, UintToArith256(consensus.powLimit).GetCompact(), 1, (1 * COIN));
        if (startNewChain == true) {
            MineGenesis(genesis, consensus.powLimit, true);
        }

        consensus.hashGenesisBlock = genesis.GetHash();

        if (!startNewChain) {
            assert(consensus.hashGenesisBlock == uint256S("0x000dd558f4324c4df1740bcab666ee9f7b61aa863b1de62bc864d780dbab64cd"));
            assert(genesis.hashMerkleRoot == uint256S("0xcf42d6a146359c277a6620e802ff3511ec0877c588fd5e26d996e431befb966c"));
        }

        vFixedSeeds.clear(); //! Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();      //! Regtest mode doesn't have any DNS seeds.

        fMiningRequiresPeers = false;
        fDefaultConsistencyChecks = true;
        fRequireStandard = false;
        fRequireRoutableExternalIP = true;
        fMineBlocksOnDemand = true;
        fAllowMultipleAddressesFromGroup = false;
        fAllowMultiplePorts = false;

        nFulfilledRequestExpireTime = 5 * 60; // fulfilled requests expire in 5 minutes

        vSporkAddresses = {"yjDHexSC7vHmf7KYwDaB5ybiARup1Wj88m"}; //private key: cUJ3aeHjikcaixh8k6gjwNESazyMqXfjKXeXzpgUVXe3UBMd2A2A
        nMinSporkKeys = 1;

        checkpointData = (CCheckpointData){
            boost::assign::map_list_of(0, uint256S("0x"))};

        chainTxData = ChainTxData{
            0, 		// * UNIX timestamp of last known number of transactions
            0,  	// * total number of transactions between genesis and that timestamp
                	//   (the tx=... number in the SetBestChain debug.log lines)
            0.1 	// * estimated number of transactions per second after that timestamp
        };

        // Regtest Credit addresses start with 'y'
        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 140);
        // Regtest Credit script addresses start with '8' or '9'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 19);
        // Regtest private keys start with '9' or 'c' (Bitcoin defaults)
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 239);
        // Regtest Credit BIP32 pubkeys start with 'tpub' (Bitcoin defaults)
        base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x04)(0x35)(0x87)(0xCF).convert_to_container<std::vector<unsigned char> >();
        // Regtest Credit BIP32 prvkeys start with 'tprv' (Bitcoin defaults)
        base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x04)(0x35)(0x83)(0x94).convert_to_container<std::vector<unsigned char> >();
        // Credit Stealth Address start with 'R'
        base58Prefixes[STEALTH_ADDRESS] = {0x13};
        // Regtest Credit BIP44 coin type is '1' (All coin's testnet default)
        nExtCoinType = 1;
    }
    void UpdateBIP9Parameters(Consensus::DeploymentPos d, int64_t nStartTime, int64_t nTimeout)
    {
        consensus.vDeployments[d].nStartTime = nStartTime;
        consensus.vDeployments[d].nTimeout = nTimeout;
    }
};
static CRegTestParams regTestParams;


/**
 * Privatenet
 */
class CPrivateNetParams : public CChainParams
{
public:
    CPrivateNetParams()
    {
        strNetworkID = "privatenet";

        consensus.nRewardsStart = 0; // Rewards starts on block 0
        consensus.nServiceNodePaymentsStartBlock = 0;
        consensus.nMinCountServiceNodesPaymentStart = 1; // ServiceNode Payments begin once 1 ServiceNode exists or more.

        consensus.nInstantSendConfirmationsRequired = 11;
        consensus.nInstantSendKeepLock = 24;

        consensus.nBudgetPaymentsStartBlock = 200;
        consensus.nBudgetPaymentsCycleBlocks = 50;
        consensus.nBudgetPaymentsWindowBlocks = 10;
        consensus.nBudgetProposalEstablishingTime = 60 * 20;

        consensus.nSuperblockStartBlock = 0;
        consensus.nSuperblockStartHash = uint256(); // do not check this on testnet
        consensus.nSuperblockCycle = 24;            // Superblocks can be issued hourly on testnet

        consensus.nGovernanceMinQuorum = 1;
        consensus.nGovernanceFilterElements = 500;

        consensus.nServiceNodeMinimumConfirmations = 1;

        consensus.nMajorityEnforceBlockUpgrade = 510;
        consensus.nMajorityRejectBlockOutdated = 750;
        consensus.nMajorityWindow = 1000;

        consensus.powLimit = uint256S("0000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nPowAveragingWindow = 5;
        consensus.nPowMaxAdjustUp = 32;
        consensus.nPowMaxAdjustDown = 48;
        consensus.nPowTargetTimespan = 30 * 64; // Credit: 1920 seconds
        consensus.nPowTargetSpacing = DEFAULT_AVERAGE_POW_BLOCK_TIME;
        consensus.nUpdateDiffAlgoHeight = 10; // Credit: Algorithm fork block
        assert(maxUint / UintToArith256(consensus.powLimit) >= consensus.nPowAveragingWindow);
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = false;
        consensus.nRuleChangeActivationThreshold = 254; // 75% of nMinerConfirmationWindow
        consensus.nMinerConfirmationWindow = 30;        // nPowTargetTimespan / nPowTargetSpacing

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 1199145601; // January 1, 2008
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = 1230767999;   // December 31, 2008

        // Deployment of BIP68, BIP112, and BIP113.
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].bit = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nStartTime = 1513591800; // Dec 18th 2017 10:10:00
        consensus.vDeployments[Consensus::DEPLOYMENT_CSV].nTimeout = 1545134400;   // Dec 18th 2018 12:00:00

        // The best chain should have at least this much work.
        consensus.nMinimumChainWork = 0; // 0

        // By default assume that the signatures in ancestors of this block are valid.
        consensus.defaultAssumeValid = uint256S("0x"); //15

        pchMessageStart[0] = 0x35;
        pchMessageStart[1] = 0x38;
        pchMessageStart[2] = 0x1b;
        pchMessageStart[3] = 0x45;
        // Alert key
        vAlertPubKey = ParseHex("04c1d08f17a0eedb3aa465676f39134e39f180fceefb793e85cfbe9066157a52cbfc7cfa63fc51f3a1ef001d60f10e0561dbbe032aa32f13b180039de60527cc86");
        nDefaultPort = DEFAULT_P2P_PORT + 300; // 33900
        nPruneAfterHeight = 100;
        startNewChain = false;

        genesis = CreateGenesisBlock(1591831731, 25040, UintToArith256(consensus.powLimit).GetCompact(), 1, (1 * COIN));
        if (startNewChain == true) {
            MineGenesis(genesis, consensus.powLimit, true);
        }

        consensus.hashGenesisBlock = genesis.GetHash();

        if (!startNewChain) {
            assert(consensus.hashGenesisBlock == uint256S("0x00002e435e476837b630c19c93c2e372480c3bc8209f523cd0d0c2d47ab48123"));
            assert(genesis.hashMerkleRoot == uint256S("0xcf42d6a146359c277a6620e802ff3511ec0877c588fd5e26d996e431befb966c"));
        }
        vFixedSeeds.clear();
        vSeeds.clear();
        //vSeeds.push_back(CDNSSeedData("",  ""));
        //vSeeds.push_back(CDNSSeedData("", ""));

        // Privatenet Credit addresses start with 'z'
        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 142);
        // Privatenet Credit script addresses start with '8' or '9'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 10);
        // Privatenet private keys start with '9' or 'c' (Bitcoin defaults)
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 158);
        // Privatenet Credit BIP32 pubkeys start with 'tpub' (Bitcoin defaults)
        base58Prefixes[EXT_PUBLIC_KEY] = boost::assign::list_of(0x04)(0x35)(0x87)(0xCF).convert_to_container<std::vector<unsigned char> >();
        // Privatenet Credit BIP32 prvkeys start with 'tprv' (Bitcoin defaults)
        base58Prefixes[EXT_SECRET_KEY] = boost::assign::list_of(0x04)(0x35)(0x83)(0x94).convert_to_container<std::vector<unsigned char> >();
        // Privatenet Stealth Address start with 'P'
        base58Prefixes[STEALTH_ADDRESS] = {0x12};
        // Privatenet Credit BIP44 coin type is '1' (All coin's testnet default)
        nExtCoinType = 1;

        vFixedSeeds = std::vector<SeedSpec6>(pnSeed6_privatenet, pnSeed6_privatenet + ARRAYLEN(pnSeed6_privatenet));

        fMiningRequiresPeers = false;
        fDefaultConsistencyChecks = false;
        fRequireStandard = false;
        fRequireRoutableExternalIP = true;
        fMineBlocksOnDemand = false;
        fAllowMultipleAddressesFromGroup = false;
        fAllowMultiplePorts = false;

        nPoolMaxTransactions = 3;
        nFulfilledRequestExpireTime = 5 * 60; // fulfilled requests expire in 5 minutes
        // To import spork key (D777Y4eMXrf1NgDSY1Q7kjoZuVso1ed7HL): importprivkey QWtd8MQx5kRgwmu4LGzSR5KRprf7RTCJc24yZ9BingoUMF1sBmcs
        vSporkAddresses = {"zHAf5Nt9y6XHCoB79obPLqhm5ioG4vwQPa"};
        nMinSporkKeys = 1;

        checkpointData = (CCheckpointData){
            boost::assign::map_list_of(0, uint256S("0x0000c5300560ed12db551d3dc80dbf12838ad92b260d5a8936f56844e96adb74"))};

        chainTxData = ChainTxData{
            0, 		// * UNIX timestamp of last known number of transactions
            0,  	// * total number of transactions between genesis and that timestamp
                	//   (the tx=... number in the SetBestChain debug.log lines)
            0.1 	// * estimated number of transactions per second after that timestamp
        };
    }
};
static CPrivateNetParams privateNetParams;

static CChainParams* pCurrentParams = 0;

const CChainParams& Params()
{
    assert(pCurrentParams);
    return *pCurrentParams;
}

CChainParams& Params(const std::string& chain)
{
    if (chain == CBaseChainParams::MAIN)
        return mainParams;
    else if (chain == CBaseChainParams::TESTNET)
        return testNetParams;
    else if (chain == CBaseChainParams::REGTEST)
        return regTestParams;
    else if (chain == CBaseChainParams::PRIVATENET)
        return privateNetParams;
    else
        throw std::runtime_error(strprintf("%s: Unknown chain %s.", __func__, chain));
}

void SelectParams(const std::string& network)
{
    SelectBaseParams(network);
    pCurrentParams = &Params(network);
}

void UpdateRegtestBIP9Parameters(Consensus::DeploymentPos d, int64_t nStartTime, int64_t nTimeout)
{
    regTestParams.UpdateBIP9Parameters(d, nStartTime, nTimeout);
}
