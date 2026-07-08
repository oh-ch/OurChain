// Copyright (c) 2025 The OurChain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SHARDING_SHARD_H
#define BITCOIN_SHARDING_SHARD_H

#if defined(HAVE_CONFIG_H)
#include "config/bitcoin-config.h"
#endif

#if ENABLE_SHARDING

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
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
#include "chain.h"
#include "coins.h"
#include "primitives/transaction.h"
#include "protocol.h"
#include "undo.h"

// Forward declaration (full definition in validation.h)
struct CBlockIndexWorkComparator;

/** Transaction metadata for merge/conflict resolution */
struct TransactionInfo {
    uint256 blockhash;
    CDiskBlockPos blockPos;
    unsigned int nTxOffset;
    CTxUndo txundo;

    TransactionInfo() : nTxOffset(0) {}
    TransactionInfo(const uint256& blockhash_in, const CDiskBlockPos& blockPos_in, unsigned int nTxOffset_in,
                    const CTxUndo& txundo_in)
        : blockhash(blockhash_in), blockPos(blockPos_in), nTxOffset(nTxOffset_in), txundo(txundo_in) {}
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
    /**
     * Update the best-known header for a shard. pindex must be non-null; writing
     * a null pointer is forbidden so the post-genesis invariant
     * (GetpindexBestHeader never returns null) cannot be violated by callers.
     */
    void SetpindexBestHeader(uint32_t shardId, CBlockIndex* pindex);
    /**
     * Bitcoin-style invariant for sharding: after genesis is available, every
     * shard has a non-null best header (tip if the shard chain is already set,
     * otherwise the shared genesis). Idempotent; safe to call from LoadGenesisBlock
     * and LoadChainTip. Call sites may then treat GetpindexBestHeader as non-null.
     */
    void EnsureBestHeadersSeeded(CBlockIndex* pGenesis);
    /** Fail fast if any shard is missing a best header (call after seeding). */
    void AssertBestHeadersSeeded() const;

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
    bool IsConflictTransaction(const CTransaction& tx) const;
    /** Returns false if the incoming tx lost the conflict and was marked invalid. */
    bool ResolveConflictTransaction(CBlockIndex* pindex, const CTransaction& tx, CCoinsViewCache& view);
    void StoreTxInfo(const CTransaction& tx, const uint256& blockhash, const CDiskBlockPos& blockPos,
                     unsigned int nTxOffset, const CTxUndo& txundo);
    /** Clear merge buffers after all shards have connected (UTXO already updated in ConnectBlock). */
    void ClearMergeState();

    // Merge status management
    MergeStatus GetMergeStatus() const { return m_mergeStatus; }
    void SetMergeStatus(MergeStatus status) { m_mergeStatus = status; }
    int GetBestChainHeight() const { return m_bestChainHeight; }
    uint32_t GetCurrentMergeHeight() const { return m_currentMergeHeight; }
    int GetLastMergedHeight() const { return m_lastMergedHeight; }

    /**
     * Record that shard `shardId` has connected the block at `height` for the
     * current merge frontier (height == lastMergedHeight + 1). When all shards
     * have arrived at that height the frontier advances and any miner blocked in
     * WaitForMerge() is woken. Heights at or below lastMergedHeight are ignored
     * (already merged); heights above the frontier indicate the node is out of
     * sync with the network frontier and are treated as a fatal error for now.
     */
    void CheckMergeCompleted(int height, uint32_t shardId);

    /** Block the caller (miner) until the merge frontier has caught up to this
     *  shard's own tip, i.e. lastMergedHeight >= myTipHeight. */
    void WaitForMerge();

    /** Record the highest block height connected on our own shard's chain. */
    void OnOwnBlockConnected(int height);

    // ---- Merge frontier stall recovery -------------------------------------
    // Called from net_processing's SendMessages loop. When arrivals at the
    // current frontier (lastMergedHeight+1) stay incomplete for longer than
    // MERGE_FRONTIER_STALL_TIMEOUT_US while a merge is IN_PROGRESS, peers
    // actively re-pull the missing shards' headers/blocks so a lost block
    // announcement cannot deadlock the frontier forever.

    /** True (per-peer throttled) if this peer should send recovery getheaders now. */
    bool ShouldRecoverMergeFrontierFromPeer(int64_t nNow, int64_t peerId);
    /** Fill missingShards with the shard ids that have not yet reached the frontier. */
    void GetMissingMergeFrontierShards(std::vector<uint32_t>& missingShards);
    /** The height currently being merged (lastMergedHeight + 1). */
    int GetMergeFrontierHeight();

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
    uint32_t m_currentMergeHeight = 0;

    // Merge frontier coordination (replaces the old single-slot m_mergeCount).
    // The network is assumed to share a single merge frontier at a time, so we
    // only track arrivals for the height currently being merged
    // (m_lastMergedHeight + 1).
    int m_lastMergedHeight = 0;          // highest fully merged height
    int m_myTipHeight = 0;               // highest height connected on our own shard
    std::set<uint32_t> m_mergeArrivals;  // shards that reached the current frontier height
    std::mutex m_mergeMutex;
    std::condition_variable m_mergeCv;

    // Merge frontier stall recovery state (guarded by m_mergeMutex).
    static const int64_t MERGE_FRONTIER_STALL_TIMEOUT_US = 3 * 1000000;      // 3s before recovery kicks in
    static const int64_t MERGE_FRONTIER_RECOVERY_INTERVAL_US = 2 * 1000000;  // re-pull at most every 2s per peer
    int64_t m_mergeFrontierStallSince = 0;                 // when the current frontier first looked stalled
    std::map<int64_t, int64_t> m_peerLastMergeRecovery;    // peerId -> last recovery attempt time (us)

    /** Requires m_mergeMutex held. Updates m_mergeFrontierStallSince and reports whether
     *  an IN_PROGRESS frontier merge has been stalled past the timeout. */
    bool IsMergeFrontierStalledUnlocked(int64_t nNow);


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
