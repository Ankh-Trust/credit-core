
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
    txNew.vin[0].scriptSig = CScript() << 1568397693 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
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
    const char* pszTimestamp = "BBC News 13 Sep - Macron pension reform: Paris paralysed by massive strike";
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
        consensus.nServiceNodePaymentsStartBlock = 64000;  // ServiceNode Payments begin on block 64000
        consensus.nMinCountServiceNodesPaymentStart = 21;  // ServiceNode Payments begin once 5 ServiceNodes exist or more.

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
        consensus.defaultAssumeValid = uint256S("0x00000864ccd2d6521902e633164afb3c10151648b7b107f63bb11c76abbfed0c"); // 500

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0x63;
        pchMessageStart[1] = 0x65;
        pchMessageStart[2] = 0x78;
        pchMessageStart[3] = 0x83;
        vAlertPubKey = ParseHex("0460f2c89c27c21504bbe2abe4e4719f13a200fd6e887779c783010a388c2c3cf2965b5b61e77e5319074e81cc3ca6126f20a17b39763cbcb681861e4891704bac");
        nDefaultPort = DEFAULT_P2P_PORT;
        nPruneAfterHeight = 87660;
        startNewChain = false;

        genesis = CreateGenesisBlock(1568397983, 716476, UintToArith256(consensus.powLimit).GetCompact(), 1, (1 * COIN));
        if (startNewChain == true) {
            MineGenesis(genesis, consensus.powLimit, true);
        }

        consensus.hashGenesisBlock = genesis.GetHash();

        if (!startNewChain) {
            assert(consensus.hashGenesisBlock == uint256S("0x00000cd870baeadf7b4c39a930a7f9fb8d7dc0348351ece15277a4053c241a4d"));
            assert(genesis.hashMerkleRoot == uint256S("0x986526382e16ade3266f5cae52998022f8eeb3a6d71d664f7a5681afa922eb85"));
        }

        vSeeds.push_back(CDNSSeedData("ankh-trust.com", "seeds-1.ankh-trust.com"));

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
	    (0, uint256S("0x00000cd870baeadf7b4c39a930a7f9fb8d7dc0348351ece15277a4053c241a4d"))
	    (250, uint256S("0x00000c0629fe8fd9f13afff39a8d98594b71064fb8da57857b5d99f6cc984bf3"))
	    (500, uint256S("0x00000864ccd2d6521902e633164afb3c10151648b7b107f63bb11c76abbfed0c"))
	    (1000, uint256S("0x00000ad8fc098dcf303d29c7f5b85630877122a9c9ee8835ba0f4f329731e05a"))
      (2500, uint256S("0x0000005d6f5e2096b533ec2d446023660a0b967f84079fbbcc6da426ae1f7178"))
      (5000, uint256S("0x00000d20749e14facec18e58fe561e6f8407dc04c43a0f6f9be4094a1553b60f"))
      (10000, uint256S("0x00000bd00a47a1c5410146082d8e0b67d2a51fe7ae154f45f256b81652dd9352"))
      (25000, uint256S("0x0000075c38c0d3db2cd85a796e8a4e43462a824b4e449885fbb62628eb215bbc"))
      (50000, uint256S("0x0000028af2cf5e493f25b66ca8fe14f743082c04c59d5e7a58c6566e1debf4cb"))
      (100000, uint256S("0x0000015fc0f82a15461acfe20682337e5040ead9b3ccc71da42649b518d4c9bd"))
      (250000, uint256S("0x000000be5cdeedd83ab01815a31f602fd289c64aa77664e0b19ac042a134561c"))
      (413920, uint256S("0x00000228003f0584bed9edcc1bb004c6bb450faf16189ff50c8f32d76045be2b"))
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
        consensus.defaultAssumeValid = uint256S("0x0013527412422032ebfbb8ae1334b392f19e58547936538d5e2df886e7446f97"); // 50

        pchMessageStart[0] = 0x33;
        pchMessageStart[1] = 0x36;
        pchMessageStart[2] = 0x19;
        pchMessageStart[3] = 0x43;
        vAlertPubKey = ParseHex("043ebea3223e70f2c8205ecf2c32f567fbca8bf8e74e1138f6090c4cd442589c69f719199b4a12a42bd8022eed0541e43744e387bff564dd28dba51da146170626");
        nDefaultPort = DEFAULT_P2P_PORT + 100;
        nPruneAfterHeight = 100;
        startNewChain = false;

        genesis = CreateGenesisBlock(1568565078, 75, UintToArith256(consensus.powLimit).GetCompact(), 1, (1 * COIN));
        if (startNewChain == true) {
            MineGenesis(genesis, consensus.powLimit, true);
        }

        consensus.hashGenesisBlock = genesis.GetHash();

        if (!startNewChain) {
            assert(consensus.hashGenesisBlock == uint256S("0x00ae30d90e90878b24a32fb54dcd73f7e3a012d068b6da6b209ff10b15a61a9c"));
            assert(genesis.hashMerkleRoot == uint256S("0x986526382e16ade3266f5cae52998022f8eeb3a6d71d664f7a5681afa922eb85"));
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
        vSporkAddresses = {"cNDvBW88KSRu1r9GFAB1yikFERjicX36fL"};
        nMinSporkKeys = 1;

        checkpointData = (CCheckpointData){
            boost::assign::map_list_of
            (0, uint256S("0x00ae30d90e90878b24a32fb54dcd73f7e3a012d068b6da6b209ff10b15a61a9c"))
            (25, uint256S("008fe407f7a568ce51c36b1d2c664c3019d610ba4ef6a2895a1042c844f867ee"))
            (50, uint256S("0x0013527412422032ebfbb8ae1334b392f19e58547936538d5e2df886e7446f97"))
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

        pchMessageStart[0] = 0x33;
        pchMessageStart[1] = 0x36;
        pchMessageStart[2] = 0x19;
        pchMessageStart[3] = 0x43;
        vAlertPubKey = ParseHex("04d62401026b035db2d25a2128f46c8ced5ef9843c1efd1873b7eeb7e272b22d124e2fbe88b8dfbc1036ee868b0337dfe9b911da9fa667e9e22fa7467d74cc2482");
        nDefaultPort = DEFAULT_P2P_PORT + 200;
        nPruneAfterHeight = 100;
        startNewChain = false;

        genesis = CreateGenesisBlock(1568566686, 4138, UintToArith256(consensus.powLimit).GetCompact(), 1, (1 * COIN));
        if (startNewChain == true) {
            MineGenesis(genesis, consensus.powLimit, true);
        }

        consensus.hashGenesisBlock = genesis.GetHash();

        if (!startNewChain) {
            assert(consensus.hashGenesisBlock == uint256S("0x0006115f9888a8625e4f59e2d381a0a10c2950b5a9c00d6d8105669eb30ec1b7"));
            assert(genesis.hashMerkleRoot == uint256S("0x986526382e16ade3266f5cae52998022f8eeb3a6d71d664f7a5681afa922eb85"));
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

        vSporkAddresses = {"yZGHUrpjL4C28ove4hw7cD6EGEvWpoYi5R"}; //private key: cUJ3aeHjikcaixh8k6gjwNESazyMqXfjKXeXzpgUVXe3UBMd2A2A
        nMinSporkKeys = 1;

        checkpointData = (CCheckpointData){
            boost::assign::map_list_of(0, uint256S("0x0006115f9888a8625e4f59e2d381a0a10c2950b5a9c00d6d8105669eb30ec1b7"))};

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
        consensus.defaultAssumeValid = uint256S("0x0000e533b41b39a92a939d56e3aea121c3cd99ad1ebcaade8d786a9d869a39d8"); //15

        pchMessageStart[0] = 0x33;
        pchMessageStart[1] = 0x36;
        pchMessageStart[2] = 0x19;
        pchMessageStart[3] = 0x43;
        // Alert key
        vAlertPubKey = ParseHex("0479fb17e7585dd085ad0937cd8ffe970a868b47a276d6ed07092ea5dbb592179cecdd2b4689f0ec16e49b00da72694b4c60328405b78439151b94d166aed89c4f");
        nDefaultPort = DEFAULT_P2P_PORT + 300; // 33900
        nPruneAfterHeight = 100;
        startNewChain = false;

        genesis = CreateGenesisBlock(1568568306, 224839, UintToArith256(consensus.powLimit).GetCompact(), 1, (1 * COIN));
        if (startNewChain == true) {
            MineGenesis(genesis, consensus.powLimit, true);
        }

        consensus.hashGenesisBlock = genesis.GetHash();

        if (!startNewChain) {
            assert(consensus.hashGenesisBlock == uint256S("0x0000f43d3725f549e85ab68da8dd9ac9ab9828597a125afe9fa987b69eb8ec30"));
            assert(genesis.hashMerkleRoot == uint256S("0x986526382e16ade3266f5cae52998022f8eeb3a6d71d664f7a5681afa922eb85"));
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
        vSporkAddresses = {"zYBJ2G93DEdFeKhGMhTEDhaobLprufbZYn"};
        nMinSporkKeys = 1;

        checkpointData = (CCheckpointData){
            boost::assign::map_list_of(0, uint256S("0x0000f43d3725f549e85ab68da8dd9ac9ab9828597a125afe9fa987b69eb8ec30"))};

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
