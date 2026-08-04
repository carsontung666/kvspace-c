#pragma once

#include "shm_box_allocators.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace kvspace::detail {

enum class ArtBoxNodeKind : std::uint8_t {
    Node4 = 1,
    Node16 = 2,
    Node48 = 3,
    Node256 = 4,
};

// In-memory representation only. Prefix and value references deliberately
// remain opaque: this storage component neither accesses Box data nor parses
// TLV values. A zero value_ref is therefore meaningful whenever has_value is
// true, while a missing value must use the canonical pair false/zero.
struct ArtBoxNodeRecord {
    static constexpr std::uint32_t kEmptyChild = UINT32_MAX;

    std::uint64_t prefix_ref = 0;
    std::uint64_t value_ref = 0;
    std::uint32_t prefix_len = 0;
    std::uint16_t child_count = 0;
    ArtBoxNodeKind kind = ArtBoxNodeKind::Node4;
    bool has_value = false;

    // Node4/16 use the first 4/16 key and child entries. Node48 uses index
    // plus the first 48 child slots. Node256 uses all child entries.
    std::array<std::uint8_t, 16> keys{};
    std::array<std::uint8_t, 256> index{};
    std::array<std::uint32_t, 256> children{};

    ArtBoxNodeRecord() noexcept;
};

class ArtBoxNodeRefCodec final {
public:
    static constexpr std::uint32_t kEmpty = UINT32_MAX;
    static constexpr std::uint32_t kMaximumLocalId = UINT32_C(0x3ffffffe);
    static constexpr std::uint32_t kMaximumCapacity =
        kMaximumLocalId + 1U;

    static std::uint32_t Encode(
        ArtBoxNodeKind kind,
        std::uint32_t local_id);
    static ArtBoxNodeKind Kind(std::uint32_t reference);
    static std::uint32_t LocalId(std::uint32_t reference);
};

// Stable manual little-endian codecs for the exact ArtBox payload sizes.
// Encode rejects noncanonical in-memory state; Decode additionally rejects
// nonzero padding, unused bytes, invalid tagged children, and a non-bijective
// Node48 index/child-slot relation.
class ArtBoxNodeCodec final {
public:
    static constexpr std::array<std::size_t, 4> kPayloadBytes = {
        48, 104, 472, 1048};

    static std::size_t PayloadBytes(ArtBoxNodeKind kind);
    static std::vector<std::uint8_t> Encode(
        const ArtBoxNodeRecord& record);
    static ArtBoxNodeRecord Decode(
        const void* encoded,
        std::size_t encoded_bytes);
};

struct ArtBoxNodeStoreRegion {
    void* metadata = nullptr;
    std::size_t metadata_bytes = 0;
    void* node_zone = nullptr;
    std::size_t node_zone_bytes = 0;
};

using ArtBoxNodeStoreRegions = std::array<ArtBoxNodeStoreRegion, 4>;
using ArtBoxNodeLiveBitmaps =
    std::array<std::vector<std::uint8_t>, 4>;
using ArtBoxNodeLiveBitCounts = std::array<std::size_t, 4>;

// Non-owning store over exactly four FixedBlockAllocator instances in
// Node4/16/48/256 order. All four regions must have the same explicit
// capacity, the same minimum FixedBlock header width, and no spare zone tail.
// Callers serialize access and own root publication plus Box lifetime policy.
class ArtBoxNodeStore final {
public:
    class PreparedRecovery;

    ArtBoxNodeStore(const ArtBoxNodeStore&) = delete;
    ArtBoxNodeStore& operator=(const ArtBoxNodeStore&) = delete;
    ArtBoxNodeStore(ArtBoxNodeStore&& other) noexcept;
    ArtBoxNodeStore& operator=(ArtBoxNodeStore&& other) noexcept;

    static ArtBoxNodeStore Initialize(
        const ArtBoxNodeStoreRegions& regions,
        std::uint32_t node_capacity);
    static ArtBoxNodeStore Attach(
        const ArtBoxNodeStoreRegions& regions,
        std::uint32_t node_capacity);
    static ArtBoxNodeStore AttachForRecovery(
        const ArtBoxNodeStoreRegions& regions,
        std::uint32_t node_capacity);

    std::uint32_t Allocate(const ArtBoxNodeRecord& record);
    std::uint32_t Clone(std::uint32_t source_ref);
    void DiscardUnpublished(std::uint32_t reference);
    void ReclaimPublished(std::uint32_t reference);
    void ReclaimPublished(const std::vector<std::uint32_t>& references);

    ArtBoxNodeRecord Read(std::uint32_t reference) const;
    ArtBoxNodeRecord ReadForRecovery(std::uint32_t reference) const;

    // The owning engine first uses ReadForRecovery to preflight its complete
    // node graph and Box intervals without writes. PrepareRecovery validates
    // and owns all four bitmaps plus every FixedBlock scratch allocation; it
    // moves this recovery handle only after that complete read-only phase.
    PreparedRecovery PrepareRecovery(
        ArtBoxNodeLiveBitmaps live_bitmaps,
        ArtBoxNodeLiveBitCounts live_bit_counts) &&;

    // Single-store convenience wrapper over PrepareRecovery + Apply.
    ArtBoxNodeStore Rebuild(
        const ArtBoxNodeLiveBitmaps& live_bitmaps,
        const ArtBoxNodeLiveBitCounts& live_bit_counts) &&;

    // Storage-only convenience wrapper. Production recovery must use the
    // explicit ReadForRecovery/preflight/PrepareRecovery split above so it can
    // also prepare all Box intervals before changing allocator metadata.
    ArtBoxNodeStore RecoverFromCommittedRoot(
        std::uint32_t committed_root) &&;
    void Validate() const;

    std::uint32_t Capacity(ArtBoxNodeKind kind) const;
    std::size_t HeaderBytes() const noexcept;
    std::uint32_t HighWater(ArtBoxNodeKind kind) const;
    std::uint32_t UsedCount(ArtBoxNodeKind kind) const;

private:
    ArtBoxNodeStore(
        FixedBlockAllocator node4,
        FixedBlockAllocator node16,
        FixedBlockAllocator node48,
        FixedBlockAllocator node256,
        std::uint32_t node_capacity,
        std::size_t header_bytes,
        bool recovery_access) noexcept;

    FixedBlockAllocator& Blocks(ArtBoxNodeKind kind);
    const FixedBlockAllocator& Blocks(ArtBoxNodeKind kind) const;
    void ValidateGeometry(AllocatorErrorCode failure_code) const;
    void ValidateRecordReferences(
        const ArtBoxNodeRecord& record,
        bool require_allocated_children,
        AllocatorErrorCode failure_code) const;

    FixedBlockAllocator node4_;
    FixedBlockAllocator node16_;
    FixedBlockAllocator node48_;
    FixedBlockAllocator node256_;
    std::uint32_t node_capacity_;
    std::size_t header_bytes_;
    bool recovery_access_;
};

class ArtBoxNodeStore::PreparedRecovery final {
public:
    PreparedRecovery(const PreparedRecovery&) = delete;
    PreparedRecovery& operator=(const PreparedRecovery&) = delete;
    PreparedRecovery(PreparedRecovery&& other) noexcept;
    PreparedRecovery& operator=(PreparedRecovery&& other) noexcept;

    // Calls exactly four allocation-free FixedBlock RebuildPrepared applies
    // and returns a normal ArtBoxNodeStore handle.
    ArtBoxNodeStore Apply() &&;

private:
    friend class ArtBoxNodeStore;

    PreparedRecovery(
        ArtBoxNodeStore store,
        ArtBoxNodeLiveBitmaps live_bitmaps,
        ArtBoxNodeLiveBitCounts live_bit_counts,
        std::array<std::vector<std::uint8_t>, 4> scratch) noexcept;

    ArtBoxNodeStore store_;
    ArtBoxNodeLiveBitmaps live_bitmaps_;
    ArtBoxNodeLiveBitCounts live_bit_counts_{};
    std::array<std::vector<std::uint8_t>, 4> scratch_;
    bool ready_ = true;
};

static_assert(
    ArtBoxNodeCodec::kPayloadBytes[0] == 24U + 4U + 4U * 4U + 4U,
    "ArtBox Node4 persistent size changed");
static_assert(
    ArtBoxNodeCodec::kPayloadBytes[1] == 24U + 16U + 16U * 4U,
    "ArtBox Node16 persistent size changed");
static_assert(
    ArtBoxNodeCodec::kPayloadBytes[2] == 24U + 256U + 48U * 4U,
    "ArtBox Node48 persistent size changed");
static_assert(
    ArtBoxNodeCodec::kPayloadBytes[3] == 24U + 256U * 4U,
    "ArtBox Node256 persistent size changed");

} // namespace kvspace::detail
