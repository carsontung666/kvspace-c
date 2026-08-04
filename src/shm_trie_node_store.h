#pragma once

#include "shm_box_allocators.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace kvspace::detail {

// In-memory representation only. The persistent representation is always
// encoded byte-by-byte by TrieNodeCodec; this native type is never persisted.
// value_ref is deliberately opaque here. In particular, this component does
// not define Box offsets, logical value lengths, or None semantics.
struct TrieNodeRecord {
    static constexpr std::int32_t kEmptyChild = -1;

    bool has_value = false;
    std::uint64_t value_ref = 0;
    std::array<std::int32_t, 256> children{};

    TrieNodeRecord() noexcept;
};

// Stable 1032-byte codec:
//   bytes 0..7:   LE ((value_ref << 1) | has_value)
//   bytes 8..1031: 256 LE int32 child IDs, with -1 meaning empty.
// A missing value is canonical only as has_value=false,value_ref=0.
class TrieNodeCodec final {
public:
    static constexpr std::size_t kEncodedBytes = 1032;
    static constexpr std::uint64_t kMaxValueRef = UINT64_MAX >> 1U;

    static std::array<std::uint8_t, kEncodedBytes> Encode(
        const TrieNodeRecord& record);
    static TrieNodeRecord Decode(
        const void* encoded,
        std::size_t encoded_bytes);
};

// Non-owning persistent node store backed by exactly one FixedBlockAllocator.
// There is no additional store header and every block payload is exactly 1032
// bytes. Callers serialize mutations and own root publication/recovery policy.
class TrieNodeStore final {
public:
    class PreparedRecovery;

    static constexpr std::size_t kPersistentNodeBytes =
        TrieNodeCodec::kEncodedBytes;
    static constexpr std::uint64_t kMaximumNodeCapacity =
        static_cast<std::uint64_t>(INT32_MAX) + 1U;

    // Pure preflight for the exact FixedBlock geometry selected for a node
    // zone. It performs the same canonical header-width selection used by
    // Initialize and rejects capacities outside the trie child-ID domain.
    static std::uint32_t CanonicalCapacityForZone(
        std::uint64_t node_zone_bytes);

    TrieNodeStore(const TrieNodeStore&) = delete;
    TrieNodeStore& operator=(const TrieNodeStore&) = delete;
    TrieNodeStore(TrieNodeStore&& other) noexcept;
    TrieNodeStore& operator=(TrieNodeStore&& other) noexcept;

    static TrieNodeStore Initialize(
        void* metadata,
        std::size_t metadata_bytes,
        void* node_zone,
        std::size_t node_zone_bytes,
        FixedBlockHeaderWidth header_width =
            FixedBlockHeaderWidth::Automatic);
    static TrieNodeStore Attach(
        void* metadata,
        std::size_t metadata_bytes,
        void* node_zone,
        std::size_t node_zone_bytes);
    static TrieNodeStore AttachForRecovery(
        void* metadata,
        std::size_t metadata_bytes,
        void* node_zone,
        std::size_t node_zone_bytes);

    // Allocate always validates and encodes the complete record before the
    // FixedBlock allocation mutates persistent metadata.
    std::uint32_t Allocate();
    std::uint32_t Allocate(const TrieNodeRecord& record);
    // Clone creates an unpublished byte-equivalent node; it does not publish
    // a root or alter the source.
    std::uint32_t Clone(std::uint32_t source_id);
    // This deliberately narrow operation is only for rolling back a node that
    // has never been published. Reachable-node reclamation requires an
    // external graph/root protocol and is not exposed as a generic Free.
    void DiscardUnpublished(std::uint32_t id);

    // The caller may reclaim these IDs only after it has published a new root
    // that it has proved cannot reach them. Keeping this operation distinct
    // from DiscardUnpublished makes the publication boundary explicit at the
    // production-engine layer.
    void ReclaimPublished(std::uint32_t id);
    void ReclaimPublished(const std::vector<std::uint32_t>& ids);

    TrieNodeRecord Read(std::uint32_t id) const;

    // Owner-death recovery only. This ignores mutable allocation headers and
    // validates only immutable geometry, the payload codec, and child ranges.
    TrieNodeRecord ReadForRecovery(std::uint32_t id) const;

    // Recovery-attached stores only. Prepare walks exactly the tree reachable
    // from the caller's single externally committed root, rejects cycles and
    // shared nodes, and prepares every scratch resource without changing a
    // persistent byte. The move-only result owns this recovery handle, the
    // live bitmap, all present value references, and the FixedBlock scratch.
    // Apply performs no dynamic allocation and returns a normal handle.
    PreparedRecovery PrepareRecoveryFromCommittedRoot(
        std::uint32_t committed_root_id) &&;

    // Convenience wrapper for single-allocator callers. A successful call
    // consumes this recovery handle and returns a normal handle. On any
    // prepare failure, persistent bytes are unchanged and this handle keeps
    // its recovery capability. Allocation headers are intentionally untrusted:
    // an in-range slot whose bytes happen to be a canonical node cannot be
    // distinguished from a formerly allocated node. Safety therefore relies
    // on immutable nodes plus publish-last roots; arbitrary-byte provenance
    // would require a different format with generations or integrity tags.
    TrieNodeStore RecoverFromCommittedRoot(
        std::uint32_t committed_root_id) &&;
    void Validate() const;

    std::uint32_t Capacity() const noexcept { return blocks_.Capacity(); }
    std::uint32_t HighWater() const { return blocks_.HighWater(); }
    std::uint32_t UsedCount() const { return blocks_.UsedCount(); }

private:
    TrieNodeStore(
        FixedBlockAllocator blocks,
        bool recovery_access) noexcept;

    void ValidateGeometry(AllocatorErrorCode failure_code) const;
    void ValidateRecord(
        const TrieNodeRecord& record,
        bool require_allocated_children,
        AllocatorErrorCode failure_code) const;

    FixedBlockAllocator blocks_;
    bool recovery_access_;
};

class TrieNodeStore::PreparedRecovery final {
public:
    PreparedRecovery(const PreparedRecovery&) = delete;
    PreparedRecovery& operator=(const PreparedRecovery&) = delete;
    PreparedRecovery(PreparedRecovery&& other) noexcept;
    PreparedRecovery& operator=(PreparedRecovery&& other) noexcept;

    const std::vector<std::uint8_t>& LiveBits() const noexcept {
        return live_bits_;
    }
    std::size_t LiveBitCount() const noexcept { return live_bit_count_; }
    std::uint64_t ReachableNodeCount() const noexcept {
        return reachable_node_count_;
    }
    const std::vector<std::uint64_t>& ValueReferences() const noexcept {
        return value_references_;
    }

    TrieNodeStore Apply() &&;

private:
    friend class TrieNodeStore;

    PreparedRecovery(
        TrieNodeStore store,
        std::vector<std::uint8_t> live_bits,
        std::size_t live_bit_count,
        std::uint64_t reachable_node_count,
        std::vector<std::uint64_t> value_references,
        std::vector<std::uint8_t> rebuild_scratch) noexcept;

    TrieNodeStore store_;
    std::vector<std::uint8_t> live_bits_;
    std::size_t live_bit_count_ = 0;
    std::uint64_t reachable_node_count_ = 0;
    std::vector<std::uint64_t> value_references_;
    std::vector<std::uint8_t> rebuild_scratch_;
    bool ready_ = true;
};

static_assert(
    TrieNodeCodec::kEncodedBytes == 8U + 256U * sizeof(std::uint32_t),
    "Trie node persistent size changed");

} // namespace kvspace::detail
