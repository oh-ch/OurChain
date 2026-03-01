// Copyright (c) 2025 The OurChain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SHARDING_SHARD_H
#define BITCOIN_SHARDING_SHARD_H

#if defined(HAVE_CONFIG_H)
#include "config/bitcoin-config.h"
#endif

#if ENABLE_SHARDING

#include <deque>
#include <map>
#include <set>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations
class uint256;
class CBlock;
class CBlockIndex;
class CTransaction;
class CCoinsViewCache;
class CTxUndo;
class CChain;
class CDiskBlockPos;
class CChainParams;
struct COutPoint;

// Need to include coins.h for hasher types and protocol.h for MessageStartChars
#include "coins.h"
#include "primitives/transaction.h"
#include "protocol.h"

// Forward declaration (full definition in validation.h)
struct CBlockIndexWorkComparator;

/** Transaction metadata for merge/conflict resolution */
struct TransactionInfo {
    uint256 blockhash;
    std::vector<CTxIn> vin;
    std::vector<CTxOut> vout;
};

// Default sharding configuration values
static const uint32_t DEFAULT_TOTAL_SHARD_COUNT = 1;
static const uint32_t DEFAULT_MY_SHARD_ID = 0;


// Type aliases for cleaner naming
typedef std::unordered_map<COutPoint, uint256, SaltedOutpointHasher> BufferedOutpointsMap;
typedef std::unordered_map<uint256, TransactionInfo, SaltedHasher> TransactionInfoMap;

/**
 * Merge status enum - tracks the state of transaction merging across shards
 */
enum MergeStatus {
    MERGE_STATUS_NONE,        // No merge in progress
    MERGE_STATUS_IN_PROGRESS, // Merge is currently being processed
    MERGE_STATUS_COMPLETED    // Merge has completed
};

/**
 * ShardManager - Centralized management of all sharding functionality
 *
 * This class encapsulates all sharding-related state and operations,
 * providing a clean API without redundant "shard" prefixes in method names.
 */
class ShardManager
{
public:
    // Singleton access
    static ShardManager& GetInstance();

    // Configuration
    void Initialize(uint32_t totalCount, uint32_t myId);
    uint32_t GetTotalCount() const { return m_totalCount; }
    uint32_t GetMyId() const { return m_myId; }
    bool IsInitialized() const { return m_initialized; }

    // Shard determination
    uint32_t GetShardForHash(const uint256& hash) const;
    bool IsTxCrossShard(const uint256& hash) const; // For transactions only

    // Helper for logging shard information
    std::string FormatShardInfo(const uint256& hash) const;

    // Chain management
    CChain& GetChain(uint32_t shardId);
    const CChain& GetChain(uint32_t shardId) const;
    CBlockIndex* GetpindexBestHeader(uint32_t shardId) const;
    void SetpindexBestHeader(uint32_t shardId, CBlockIndex* pindex);

    // Best invalid block accessors
    CBlockIndex*& GetBestInvalid(uint32_t shardId);
    CBlockIndex* GetBestInvalid(uint32_t shardId) const;

    // Block index candidates accessors
    std::set<CBlockIndex*, CBlockIndexWorkComparator>& GetBlockIndexCandidates(uint32_t shardId);
    const std::set<CBlockIndex*, CBlockIndexWorkComparator>& GetBlockIndexCandidates(uint32_t shardId) const;

    // Clear all sharding state (for cleanup)
    void ClearAll();

    // Cross-shard transaction relay (relay without mempool; protected by cs_main at call sites)
    void AddCrossShardTransactionToRelay(const CTransactionRef& tx);
    bool HaveCrossShardTransaction(const uint256& hash) const;
    CTransactionRef GetCrossShardTransaction(const uint256& hash) const;
    void ExpireCrossShardRelay(int64_t nNow);

    // Transaction processing
    void MergeTransaction(CBlockIndex* pindex, const CTransaction& tx,
                          CCoinsViewCache& inputs, CTxUndo& txundo);
    void UpdateCoins(CCoinsViewCache& inputs, int nHeight);

    // Merge status management
    MergeStatus GetMergeStatus() const { return m_mergeStatus; }
    void SetMergeStatus(MergeStatus status) { m_mergeStatus = status; }
    int GetBestChainHeight() const { return m_bestChainHeight; }

    // Utilities
    uint256 HashInvalidList(const std::vector<uint256>& vInvalidTxHashes) const;

    // Accessors for tests
    BufferedOutpointsMap& GetBufferedOutpoints() { return m_mapOutpointToTxHash; }
    TransactionInfoMap& GetTransactionInfo() { return m_mapTxInfo; }

private:
    ShardManager() = default;
    ~ShardManager() = default;
    ShardManager(const ShardManager&) = delete;
    ShardManager& operator=(const ShardManager&) = delete;

    bool m_initialized = false;
    uint32_t m_totalCount = 1;
    uint32_t m_myId = 0;
    MergeStatus m_mergeStatus = MERGE_STATUS_NONE;
    int m_bestChainHeight = 0;


    std::map<uint32_t, CChain> m_chains;
    std::map<uint32_t, CBlockIndex*> m_bestHeaders;
    std::map<uint32_t, CBlockIndex*> m_bestInvalid;
    std::map<uint32_t, std::set<CBlockIndex*, CBlockIndexWorkComparator>> m_blockIndexCandidates;

    // Merge/buffer maps
    BufferedOutpointsMap m_mapOutpointToTxHash;
    TransactionInfoMap m_mapTxInfo;

    // Cross-shard relay: transactions we relay but don't keep in mempool
    std::map<uint256, CTransactionRef> m_crossShardRelay;
    std::deque<std::pair<int64_t, std::map<uint256, CTransactionRef>::iterator>> m_crossShardRelayExpiration;
};

#endif // ENABLE_SHARDING
#endif // BITCOIN_SHARDING_SHARD_H
