// Copyright (c) 2025 The OurChain developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sharding/shard.h"

#include "arith_uint256.h"
#include "chain.h"
#include "chainparams.h"
#include "coins.h"
#include "hash.h"
#include "primitives/transaction.h"
#include "uint256.h"
#include "undo.h"
#include "util.h"
#include "utilstrencodings.h" // For strprintf
#include "validation.h"

#if ENABLE_SHARDING

ShardManager& ShardManager::GetInstance()
{
    static ShardManager instance;
    return instance;
}

void ShardManager::Initialize(uint32_t totalCount, uint32_t myId)
{
    m_totalCount = totalCount;
    m_myId = myId;
    m_initialized = true;
}

uint32_t ShardManager::GetShardForHash(const uint256& hash) const
{
    // Full txid % shard_count: interpret entire 256-bit hash as integer, then mod.
    arith_uint256 h = UintToArith256(hash);
    arith_uint256 count_val(m_totalCount);
    arith_uint256 remainder = h - (h / count_val) * count_val;
    return (uint32_t)remainder.GetLow64();
}

bool ShardManager::IsTxCrossShard(const uint256& hash) const
{
    return GetShardForHash(hash) != m_myId;
}

std::string ShardManager::FormatShardInfo(const uint256& hash) const
{
    uint32_t txShard = GetShardForHash(hash);
    return strprintf("(shard %u, we are shard %u)", txShard, m_myId);
}

uint256 ShardManager::HashInvalidList(const std::vector<uint256>& vInvalidTxHashes) const
{
    return SerializeHash(vInvalidTxHashes);
}

CChain& ShardManager::GetChain(uint32_t shardId)
{
    return m_chains[shardId];
}

const CChain& ShardManager::GetChain(uint32_t shardId) const
{
    // For const access, we need to use find() to avoid creating entries
    // But to match the original behavior where map[key] creates entries,
    // we'll use const_cast to allow modification (caller must ensure thread safety)
    return const_cast<ShardManager*>(this)->m_chains[shardId];
}

CBlockIndex* ShardManager::GetpindexBestHeader(uint32_t shardId) const
{
    auto it = m_bestHeaders.find(shardId);
    return (it != m_bestHeaders.end()) ? it->second : nullptr;
}

void ShardManager::SetpindexBestHeader(uint32_t shardId, CBlockIndex* pindex)
{
    m_bestHeaders[shardId] = pindex;
    if (pindex->nHeight > m_bestChainHeight) {
        m_bestChainHeight = pindex->nHeight;
    }
}

CBlockIndex*& ShardManager::GetBestInvalid(uint32_t shardId)
{
    return m_bestInvalid[shardId];
}

CBlockIndex* ShardManager::GetBestInvalid(uint32_t shardId) const
{
    auto it = m_bestInvalid.find(shardId);
    return (it != m_bestInvalid.end()) ? it->second : nullptr;
}

std::set<CBlockIndex*, CBlockIndexWorkComparator>& ShardManager::GetBlockIndexCandidates(uint32_t shardId)
{
    return m_blockIndexCandidates[shardId];
}

const std::set<CBlockIndex*, CBlockIndexWorkComparator>& ShardManager::GetBlockIndexCandidates(uint32_t shardId) const
{
    auto it = m_blockIndexCandidates.find(shardId);
    if (it != m_blockIndexCandidates.end()) {
        return it->second;
    }
    // Return a reference to an empty set if not found
    static const std::set<CBlockIndex*, CBlockIndexWorkComparator> emptySet;
    return emptySet;
}

void ShardManager::ClearAll()
{
    m_chains.clear();
    m_bestHeaders.clear();
    m_bestInvalid.clear();
    m_blockIndexCandidates.clear();
    m_mapOutpointToTxHash.clear();
    m_mapTxInfo.clear();
    m_crossShardRelay.clear();
    m_crossShardRelayExpiration.clear();
}

void ShardManager::AddCrossShardTransactionToRelay(const CTransactionRef& tx)
{
    uint256 hash = tx->GetHash();
    int64_t nNow = GetTimeMicros();
    auto ret = m_crossShardRelay.insert(std::make_pair(hash, tx));
    if (ret.second) {
        m_crossShardRelayExpiration.push_back(std::make_pair(nNow + 15 * 60 * 1000000, ret.first));
    }
}

bool ShardManager::HaveCrossShardTransaction(const uint256& hash) const
{
    return m_crossShardRelay.count(hash) != 0;
}

CTransactionRef ShardManager::GetCrossShardTransaction(const uint256& hash) const
{
    auto it = m_crossShardRelay.find(hash);
    return (it != m_crossShardRelay.end()) ? it->second : CTransactionRef();
}

void ShardManager::ExpireCrossShardRelay(int64_t nNow)
{
    while (!m_crossShardRelayExpiration.empty() && m_crossShardRelayExpiration.front().first < nNow) {
        m_crossShardRelay.erase(m_crossShardRelayExpiration.front().second);
        m_crossShardRelayExpiration.pop_front();
    }
}

void ShardManager::MergeTransaction(CBlockIndex* pindex, const CTransaction& tx,
                                    CCoinsViewCache& inputs, CTxUndo& txundo)
{
    // Mark inputs spent
    if (!tx.IsCoinBase()) {
        txundo.vprevout.reserve(tx.vin.size());
        for (const CTxIn& txin : tx.vin) {
            txundo.vprevout.emplace_back();
            txundo.vprevout.back() = inputs.AccessCoin(txin.prevout);
            // Check if there is conflict transaction using same outpoint
            auto it = m_mapOutpointToTxHash.find(txin.prevout);
            if (it == m_mapOutpointToTxHash.end()) {
                m_mapOutpointToTxHash[txin.prevout] = tx.GetHash();
                // Only store transaction info if it doesn't already exist
                m_mapTxInfo.try_emplace(tx.GetHash(), pindex->GetBlockHash(), tx.vin, tx.vout);
            } else {
                uint256 blockhashConflict = m_mapTxInfo[it->second].blockhash;
                extern BlockMap mapBlockIndex;
                CBlockIndex* pindexConflict = mapBlockIndex[blockhashConflict];
                // Compare each shard's block's priority index (smaller is higher priority)
                if (pindexConflict->nShardId >= pindex->nShardId) {
                    pindex->vInvalidList.push_back(tx.GetHash());
                } else {
                    // Erase the conflict transaction from both maps
                    std::vector<CTxIn> vinConflict = std::move(m_mapTxInfo[it->second].vin);
                    // Erase all outpoints in vinConflict from m_mapOutpointToTxHash
                    for (const CTxIn& conflictTxin : vinConflict) {
                        COutPoint outpoint = conflictTxin.prevout;
                        m_mapOutpointToTxHash.erase(outpoint);
                    }
                    pindexConflict->vInvalidList.push_back(it->second);
                    m_mapTxInfo.erase(it->second);
                    m_mapOutpointToTxHash[txin.prevout] = tx.GetHash();
                }
            }
        }
    } else {
        // add outputs
        AddCoins(inputs, tx, pindex->nHeight);
    }
}

void ShardManager::UpdateCoins(CCoinsViewCache& inputs, int nHeight)
{
    for (auto const& [outpoint, txid] : m_mapOutpointToTxHash) {
        bool is_spent = inputs.SpendShardCoin(outpoint);
        assert(is_spent);
    }
    for (auto const& [txid, transactionInfo] : m_mapTxInfo) {
        AddShardCoins(inputs, txid, transactionInfo, nHeight);
    }
    m_mapOutpointToTxHash.clear();
    m_mapTxInfo.clear();
}

#endif // ENABLE_SHARDING
