#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace kvspace::detail {

enum class AllocatorErrorCode {
    InvalidArgument,
    Corrupt,
    Capacity,
    InvalidId,
    NotAllocated,
    Overlap,
};

class AllocatorError final : public std::runtime_error {
public:
    AllocatorError(AllocatorErrorCode code, const std::string& message);

    AllocatorErrorCode Code() const noexcept { return code_; }

private:
    AllocatorErrorCode code_;
};

enum class FixedBlockHeaderWidth : std::uint8_t {
    Automatic = 0,
    Bytes2 = 2,
    Bytes4 = 4,
    Bytes8 = 8,
};

// FixedBlockAllocator is a non-owning view over two caller-owned persistent
// regions. All integers are encoded little-endian; no C/C++ struct, bitfield,
// atomic, pointer, or lock representation is persisted.
//
// Mutations are deliberately serialized by the caller's robust mutex. They are
// not individually crash-atomic: after owner death the caller supplies the
// committed live-ID bitmap to Rebuild().
class FixedBlockAllocator final {
public:
    static constexpr std::size_t kPersistentMetadataBytes = 64;
    static constexpr std::uint32_t kInvalidId = UINT32_MAX;

    FixedBlockAllocator(const FixedBlockAllocator&) = delete;
    FixedBlockAllocator& operator=(const FixedBlockAllocator&) = delete;
    FixedBlockAllocator(FixedBlockAllocator&& other) noexcept;
    FixedBlockAllocator& operator=(FixedBlockAllocator&& other) noexcept;

    static FixedBlockAllocator Initialize(
        void* metadata,
        std::size_t metadata_bytes,
        void* block_zone,
        std::size_t block_zone_bytes,
        std::size_t payload_bytes,
        // An explicit width is a canonical-geometry assertion, not an
        // override; a different automatic minimum is rejected.
        FixedBlockHeaderWidth header_width =
            FixedBlockHeaderWidth::Automatic);

    // Attach validates both immutable geometry and the complete allocation
    // state, including free-list reachability.
    static FixedBlockAllocator Attach(
        void* metadata,
        std::size_t metadata_bytes,
        void* block_zone,
        std::size_t block_zone_bytes);

    // Recovery attach validates only immutable geometry. Until a successful
    // Rebuild, ordinary dynamic-state APIs are disabled and the only payload
    // view is the const RecoveryBlockData capability. A successful Rebuild
    // consumes that capability and turns this handle into a normal view.
    static FixedBlockAllocator AttachForRecovery(
        void* metadata,
        std::size_t metadata_bytes,
        void* block_zone,
        std::size_t block_zone_bytes);

    static FixedBlockHeaderWidth MinimumHeaderWidth(
        std::uint64_t capacity);

    void Validate() const;

    std::uint32_t Allocate();
    void Free(std::uint32_t id);

    // live_bits is a bit-packed bitmap: bit i is one when block i is live.
    // IDs >= live_bit_count are rebuilt as never allocated. Payload bytes are
    // never modified. Repeating Rebuild with the same bitmap is byte-stable.
    void Rebuild(const std::uint8_t* live_bits, std::size_t live_bit_count);

    // Multi-allocator recovery can validate every bitmap and allocate every
    // scratch buffer before the first persistent write. The sizing call is
    // read-only; RebuildPrepared performs no dynamic allocation and consumes
    // a recovery capability on success. live_bits and scratch must remain
    // stable during apply and must not overlap either persistent region.
    std::size_t RebuildScratchBytes(
        const std::uint8_t* live_bits,
        std::size_t live_bit_count) const;
    void RebuildPrepared(
        const std::uint8_t* live_bits,
        std::size_t live_bit_count,
        std::uint8_t* scratch,
        std::size_t scratch_bytes);

    // BlockData rejects free and never-allocated IDs.
    void* BlockData(std::uint32_t id);
    const void* BlockData(std::uint32_t id) const;

    // Owner-death recovery only: immutable geometry is sufficient to locate a
    // payload even when high-water/free-list block headers are torn. The
    // caller must derive authoritative live IDs elsewhere, then call Rebuild.
    const void* RecoveryBlockData(std::uint32_t id) const;
    bool IsAllocated(std::uint32_t id) const;

    std::uint32_t Capacity() const noexcept { return capacity_; }
    std::uint32_t HighWater() const;
    std::uint32_t UsedCount() const;
    std::size_t PayloadBytes() const noexcept { return payload_bytes_; }
    std::size_t HeaderBytes() const noexcept { return header_bytes_; }

private:
    friend class BoxAllocator;

    struct BlockHeader {
        bool used = false;
        bool has_next = false;
        std::uint32_t next = 0;
    };

    FixedBlockAllocator(
        std::uint8_t* metadata,
        std::size_t metadata_bytes,
        std::uint8_t* block_zone,
        std::size_t block_zone_bytes,
        std::size_t payload_bytes,
        std::size_t header_bytes,
        std::uint32_t capacity,
        bool recovery_access) noexcept;

    void ValidateImmutable() const;
    void RequireNormalAccess() const;
    std::uint32_t StoredHighWater() const noexcept;
    std::uint32_t StoredUsedCount() const noexcept;
    void ValidateWithScratch(
        std::uint8_t* scratch,
        std::size_t scratch_bytes) const;
    void RebuildWithScratch(
        const std::uint8_t* live_bits,
        std::size_t live_bit_count,
        std::uint8_t* scratch,
        std::size_t scratch_bytes);
    BlockHeader ReadBlockHeader(std::uint32_t id) const;
    void WriteBlockHeader(
        std::uint32_t id,
        bool used,
        std::optional<std::uint32_t> next);
    std::uint8_t* BlockHeaderAddress(std::uint32_t id) const;

    std::uint8_t* metadata_;
    std::size_t metadata_bytes_;
    std::uint8_t* block_zone_;
    std::size_t block_zone_bytes_;
    std::size_t payload_bytes_;
    std::size_t header_bytes_;
    std::uint32_t capacity_;
    bool recovery_access_;
};

struct BoxLiveInterval {
    std::uint64_t offset;
    std::uint64_t size;
};

// BoxAllocator is a 16-way allocator over a fixed data-size address space.
// Only metadata is touched; the caller-owned data region contains no inline
// allocator headers. Offset zero is a valid allocation. Requested size zero is
// rejected and must be represented by the caller's separate None/null state.
class BoxAllocator final {
public:
    class PreparedRebuild;

    static constexpr std::size_t kPersistentHeaderBytes = 128;
    static constexpr std::size_t kNodePayloadBytes = 88;

    BoxAllocator(const BoxAllocator&) = delete;
    BoxAllocator& operator=(const BoxAllocator&) = delete;
    BoxAllocator(BoxAllocator&& other) noexcept = default;
    BoxAllocator& operator=(BoxAllocator&& other) noexcept = default;

    static BoxAllocator Initialize(
        void* metadata,
        std::size_t metadata_bytes,
        std::uint64_t data_bytes);
    static BoxAllocator Attach(
        void* metadata,
        std::size_t metadata_bytes,
        std::uint64_t data_bytes);
    static BoxAllocator AttachForRecovery(
        void* metadata,
        std::size_t metadata_bytes,
        std::uint64_t data_bytes);

    static std::uint64_t RoundSize(std::uint64_t requested_size);
    static std::uint64_t LargestCanonicalDataSize(
        std::uint64_t upper_bound);

    // For canonical D = 8*m*16^L (1 <= m <= 15), full expansion has
    // 1 + m*(16^L - 1)/15 metadata nodes. Counts that cannot be addressed by
    // the uint32 local-ID space are rejected.
    static std::uint64_t FullExpansionNodeCount(
        std::uint64_t canonical_data_bytes);

    // Exact bytes accepted by Initialize, including the Box header, embedded
    // FixedBlock metadata, node payloads, and the minimum 2/4/8-byte block
    // headers selected at the FixedBlock capacity thresholds.
    static std::uint64_t MinimumMetadataBytesForFullExpansion(
        std::uint64_t canonical_data_bytes);

    // Returns the largest fully expandable canonical data region for an exact
    // metadata+data budget. Page/alignment padding is intentionally external.
    static std::uint64_t LargestFullyRepresentableDataSize(
        std::uint64_t total_budget_bytes);

    void Validate() const;
    std::uint64_t Allocate(std::uint64_t requested_size);
    void Free(std::uint64_t offset);
    std::uint64_t AllocatedSize(std::uint64_t offset) const;

    // ReserveAt requires offset alignment to the rounded allocation unit and
    // rejects any overlap. BoxLiveInterval::size is the caller's logical size;
    // Rebuild rounds it exactly as Allocate does, preflights every interval,
    // resets metadata, and reserves in deterministic order; it is idempotent.
    void ReserveAt(std::uint64_t offset, std::uint64_t requested_size);
    void ValidateRebuildIntervals(
        const std::vector<BoxLiveInterval>& live_intervals) const;
    // The move-only plan owns this allocator handle, normalized intervals,
    // and all FixedBlock/Box validation scratch. Prepare is read-only and
    // Apply performs no dynamic allocation before returning a normal handle.
    PreparedRebuild PrepareRebuild(
        const std::vector<BoxLiveInterval>& live_intervals) &&;
    void Rebuild(const std::vector<BoxLiveInterval>& live_intervals);

    std::uint64_t DataBytes() const noexcept { return data_bytes_; }
    std::uint32_t MetadataNodeCapacity() const noexcept {
        return blocks_.Capacity();
    }
    std::uint32_t MetadataNodesUsed() const { return blocks_.UsedCount(); }

private:
    enum class SlotState : std::uint8_t {
        Unused = 0,
        Child = 1,
        ObjectStart = 2,
        ObjectContinued = 3,
    };

    struct Shape {
        std::uint64_t unit_bytes;
        std::uint64_t rounded_bytes;
        std::uint8_t level;
        std::uint8_t multiple;
    };

    struct NormalizedRebuildInterval {
        std::uint64_t offset;
        Shape shape;
    };

    struct RebuildPlan {
        std::vector<NormalizedRebuildInterval> intervals;
        std::size_t required_node_count = 0;
    };

    struct Node {
        std::uint32_t parent = FixedBlockAllocator::kInvalidId;
        std::uint8_t parent_slot = UINT8_MAX;
        std::uint8_t level = 0;
        std::uint8_t slot_count = 0;
        std::array<SlotState, 16> states{};
        std::array<std::uint32_t, 16> children{};
    };

    enum class AllocationResultKind {
        Success,
        NoSpace,
        MetadataCapacity,
    };

    struct AllocationResult {
        AllocationResultKind kind = AllocationResultKind::NoSpace;
        std::uint64_t offset = 0;
    };

    BoxAllocator(
        std::uint8_t* metadata,
        std::size_t metadata_bytes,
        std::uint64_t data_bytes,
        std::uint8_t root_level,
        std::uint8_t root_slots,
        FixedBlockAllocator blocks) noexcept;

    static Shape ShapeForRequest(std::uint64_t requested_size);
    static Shape ShapeForExact(std::uint64_t exact_size);
    static std::uint64_t UnitBytes(std::uint8_t level);
    RebuildPlan MakeRebuildPlan(
        const std::vector<BoxLiveInterval>& live_intervals) const;

    void ValidateImmutable() const;
    Node ReadNode(std::uint32_t id) const;
    void WriteNode(std::uint32_t id, const Node& node);
    void InitializeNode(
        std::uint32_t id,
        std::uint32_t parent,
        std::uint8_t parent_slot,
        std::uint8_t level,
        std::uint8_t slot_count);
    bool NodeIsEmpty(const Node& node) const;

    AllocationResult AllocateInNode(
        std::uint32_t node_id,
        std::uint64_t node_base,
        const Shape& shape);
    void ReserveInNode(
        std::uint32_t node_id,
        std::uint64_t node_base,
        std::uint64_t offset,
        const Shape& shape);
    std::uint64_t FreeInNode(
        std::uint32_t node_id,
        std::uint64_t node_base,
        std::uint64_t offset,
        bool* became_empty);
    std::uint64_t AllocatedSizeInNode(
        std::uint32_t node_id,
        std::uint64_t node_base,
        std::uint64_t offset) const;

    void ValidateNode(
        std::uint32_t node_id,
        std::uint32_t expected_parent,
        std::uint8_t expected_parent_slot,
        std::uint8_t expected_level,
        std::uint8_t expected_slots,
        std::uint8_t* reachable,
        std::size_t reachable_bit_count) const;
    void ValidateWithScratch(
        std::uint8_t* scratch,
        std::size_t scratch_bytes) const;

    std::uint8_t* metadata_;
    std::size_t metadata_bytes_;
    std::uint64_t data_bytes_;
    std::uint8_t root_level_;
    std::uint8_t root_slots_;
    FixedBlockAllocator blocks_;
};

class BoxAllocator::PreparedRebuild final {
public:
    PreparedRebuild(const PreparedRebuild&) = delete;
    PreparedRebuild& operator=(const PreparedRebuild&) = delete;
    PreparedRebuild(PreparedRebuild&& other) noexcept;
    PreparedRebuild& operator=(PreparedRebuild&& other) noexcept;

    BoxAllocator Apply() &&;

private:
    friend class BoxAllocator;

    PreparedRebuild(
        BoxAllocator allocator,
        std::vector<NormalizedRebuildInterval> intervals,
        std::size_t required_node_count,
        std::vector<std::uint8_t> validation_scratch) noexcept;

    BoxAllocator allocator_;
    std::vector<NormalizedRebuildInterval> intervals_;
    std::size_t required_node_count_ = 0;
    std::vector<std::uint8_t> validation_scratch_;
    bool ready_ = true;
};

} // namespace kvspace::detail
