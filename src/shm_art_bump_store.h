#pragma once

#include "shm_box_allocators.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <pthread.h>
#include <vector>

namespace kvspace::detail {

enum class ArtBumpNodeKind : std::uint8_t {
    Node4 = 1,
    Node16 = 2,
    Node48 = 3,
    Node256 = 4,
};

enum class ArtBumpJournalState : std::uint32_t {
    Idle = 0,
    Copying = 1,
    Ready = 2,
};

enum class ArtBumpReadyField : std::uint8_t {
    NewRoot = 0,
    TargetTop = 1,
    NodeCount = 2,
    EntryCount = 3,
    EngineLiveBytes = 4,
    ReadyChecksum = 5,
};

enum class ArtBumpRebuildCut : std::uint8_t {
    First = 0,
    Middle = 1,
    Final = 2,
};

struct ArtBumpSlabDescriptor {
    std::uint64_t metadata_offset = 0;
    std::uint64_t metadata_bytes = 0;
    std::uint64_t zone_offset = 0;
    std::uint64_t zone_bytes = 0;
};

struct ArtBumpRawZoneDescriptor {
    std::uint64_t begin = 0;
    std::uint64_t bytes = 0;
    std::uint64_t top = 0;
    std::uint32_t epoch = 0;
};

struct ArtBumpCompactJournal {
    std::uint64_t old_root = 0;
    std::uint64_t source_top = 0;
    std::uint64_t operation_generation = 0;
    std::uint64_t base_checksum = 0;
    std::uint64_t new_root = 0;
    std::uint64_t target_top = 0;
    std::uint64_t node_count = 0;
    std::uint64_t entry_count = 0;
    std::uint64_t engine_live_bytes = 0;
    std::uint64_t ready_checksum = 0;
    ArtBumpJournalState state = ArtBumpJournalState::Idle;
    std::uint32_t source_zone = 0;
    std::uint32_t target_zone = 0;
    std::uint32_t source_epoch = 0;
    std::uint32_t target_epoch = 0;
};

struct ArtBumpEngineHeader {
    std::uint64_t committed_root = 0;
    std::uint32_t active_zone = 0;
    std::array<ArtBumpSlabDescriptor, 4> slabs{};
    std::array<ArtBumpRawZoneDescriptor, 2> raw_zones{};
    std::uint64_t geometry_hash = 0;
    std::uint32_t node_capacity = 0;
    std::array<std::uint32_t, 4> payload_bytes = {
        64, 168, 664, 2072};
    ArtBumpCompactJournal journal{};
};

struct ArtBumpGeometry {
    static constexpr std::uint64_t kPageBytes = 4096;
    static constexpr std::uint64_t kCommonHeaderBytes = 1472;
    static constexpr std::uint64_t kQueueSlotBytes = 32;
    static constexpr std::uint64_t kEngineHeaderBytes = 384;
    static constexpr std::uint64_t kJournalBytes = 128;
    static constexpr std::uint64_t kSlabMetadataBytes = 64;

    std::uint64_t region_size = 0;
    std::uint64_t queue_offset = 0;
    std::uint64_t queue_table_bytes = 0;
    std::uint64_t heap_offset = 0;
    std::uint64_t heap_bytes = 0;
    std::uint64_t engine_offset = 0;
    std::uint64_t engine_bytes = 0;
    std::uint64_t terminal_tail_bytes = 0;
    std::uint64_t entry_limit = 0;
    std::uint64_t queue_capacity = 0;
    std::uint32_t node_capacity = 0;
    std::array<ArtBumpSlabDescriptor, 4> slabs{};
    std::array<ArtBumpRawZoneDescriptor, 2> raw_zones{};
    std::uint64_t geometry_hash = 0;

    static std::uint64_t QueueCapacity(std::uint64_t queue_limit);
    static ArtBumpGeometry Compute(
        std::uint64_t region_size,
        std::uint64_t entry_limit,
        std::uint64_t queue_capacity,
        std::uint64_t page_bytes = kPageBytes,
        std::uint64_t common_header_bytes = kCommonHeaderBytes,
        std::uint64_t queue_slot_bytes = kQueueSlotBytes);
    static ArtBumpGeometry DefaultProfile();
    ArtBumpEngineHeader InitialHeader() const;
};

class ArtBumpHeaderCodec final {
public:
    static constexpr std::size_t kHeaderBytes = 384;
    static constexpr std::size_t kJournalBytes = 128;
    static constexpr std::size_t kRawZonesOffset = 160;
    static constexpr std::size_t kRawZoneDescriptorBytes = 32;
    using HeaderBytes = std::array<std::uint8_t, kHeaderBytes>;
    using JournalBytes = std::array<std::uint8_t, kJournalBytes>;

    static HeaderBytes Encode(const ArtBumpEngineHeader& header);
    static ArtBumpEngineHeader Decode(
        const void* encoded,
        std::size_t encoded_bytes);
    // When journal state is active, mutable selectors and raw top/epoch fields
    // are nonauthoritative. This validates all immutable/reserved/journal bytes
    // while returning those mutable bytes without trusting their values.
    static ArtBumpEngineHeader DecodeForRecovery(
        const void* encoded,
        std::size_t encoded_bytes);
    static JournalBytes EncodeJournal(const ArtBumpCompactJournal& journal);
    static ArtBumpCompactJournal DecodeJournal(
        const void* encoded,
        std::size_t encoded_bytes);
    static std::uint64_t BaseChecksum(const ArtBumpCompactJournal& journal);
    static std::uint64_t ReadyChecksum(const ArtBumpCompactJournal& journal);
    static std::uint64_t EngineLayoutHash(
        std::uint64_t common_header_bytes = 1472,
        std::uint64_t queue_slot_bytes = 32,
        std::uint64_t blob_header_bytes = 48,
        std::uint64_t mutex_bytes = sizeof(pthread_mutex_t),
        std::uint64_t condition_bytes = sizeof(pthread_cond_t));
    static std::uint64_t GeometryHash(
        std::uint64_t entry_limit,
        std::uint32_t node_capacity,
        const std::array<ArtBumpSlabDescriptor, 4>& slabs,
        const std::array<ArtBumpRawZoneDescriptor, 2>& raw_zones);
    static std::uint64_t SlabGeometryHash(
        std::uint32_t payload_bytes,
        std::uint64_t zone_offset,
        std::uint64_t zone_bytes,
        std::uint32_t capacity);
};

// Non-owning access to the manually encoded mutable engine header fields.
// All access remains byte-codec based; the four publication fields use
// acquire/release compiler atomics on their naturally aligned LE scalar words.
class ArtBumpHeaderView final {
public:
    static ArtBumpHeaderView Initialize(
        void* encoded,
        std::size_t encoded_bytes,
        const ArtBumpEngineHeader& header);
    static ArtBumpHeaderView Attach(void* encoded, std::size_t encoded_bytes);
    static ArtBumpHeaderView AttachForRecovery(
        void* encoded,
        std::size_t encoded_bytes);

    ArtBumpEngineHeader Decode() const;
    ArtBumpEngineHeader DecodeForRecovery() const;
    std::uint64_t CommittedRootAcquire() const noexcept;
    void StoreCommittedRootRelease(std::uint64_t root) noexcept;
    std::uint32_t ActiveZone() const noexcept;
    void StoreActiveZone(std::uint32_t zone) noexcept;
    ArtBumpCompactJournal JournalAcquire() const;
    void StoreJournalPayload(const ArtBumpCompactJournal& journal);
    void StoreJournalReadyField(
        const ArtBumpCompactJournal& journal,
        ArtBumpReadyField field) noexcept;
    void StoreJournalReadyFields(const ArtBumpCompactJournal& journal) noexcept;
    void StoreJournalStateRelease(ArtBumpJournalState state) noexcept;

    std::uint8_t* Data() const noexcept { return bytes_; }

private:
    explicit ArtBumpHeaderView(std::uint8_t* bytes) noexcept : bytes_(bytes) {}
    std::uint8_t* bytes_ = nullptr;
};

struct ArtBumpNodeRecord {
    static constexpr std::uint64_t kEmptyChild = 0;

    std::uint64_t prefix_offset = 0;
    std::uint64_t value_offset = 0;
    std::uint32_t prefix_len = 0;
    std::uint16_t child_count = 0;
    ArtBumpNodeKind kind = ArtBumpNodeKind::Node4;
    bool has_value = false;
    std::array<std::uint8_t, 16> keys{};
    std::array<std::uint8_t, 256> index{};
    std::array<std::uint64_t, 256> children{};

    ArtBumpNodeRecord() noexcept;
};

class ArtBumpNodeCodec final {
public:
    static constexpr std::array<std::size_t, 4> kPayloadBytes = {
        64, 168, 664, 2072};
    static constexpr std::size_t kMaximumPayloadBytes = 2072;

    static std::size_t PayloadBytes(ArtBumpNodeKind kind);
    static std::vector<std::uint8_t> Encode(const ArtBumpNodeRecord& record);
    static std::size_t EncodeTo(
        const ArtBumpNodeRecord& record,
        void* output,
        std::size_t output_bytes);
    static ArtBumpNodeRecord Decode(
        const void* encoded,
        std::size_t encoded_bytes);
};

using ArtBumpNodeLiveBitmaps =
    std::array<std::vector<std::uint8_t>, 4>;
using ArtBumpNodeLiveBitCounts = std::array<std::size_t, 4>;

// Four ABI-4 headerless slabs over absolute region offsets. The enclosing
// engine owns publication and graph validation; this class owns slot format,
// arbitrary clean free-list validation, exact allocation, and rebuild.
class ArtBumpNodeStore final {
public:
    class PreparedRebuild;

    ArtBumpNodeStore(const ArtBumpNodeStore&) = delete;
    ArtBumpNodeStore& operator=(const ArtBumpNodeStore&) = delete;
    ArtBumpNodeStore(ArtBumpNodeStore&& other) noexcept;
    ArtBumpNodeStore& operator=(ArtBumpNodeStore&& other) noexcept;

    static ArtBumpNodeStore Initialize(
        void* region_base,
        std::size_t region_bytes,
        const std::array<ArtBumpSlabDescriptor, 4>& slabs,
        std::uint32_t node_capacity);
    static ArtBumpNodeStore Attach(
        void* region_base,
        std::size_t region_bytes,
        const std::array<ArtBumpSlabDescriptor, 4>& slabs,
        std::uint32_t node_capacity);
    static ArtBumpNodeStore AttachForRecovery(
        void* region_base,
        std::size_t region_bytes,
        const std::array<ArtBumpSlabDescriptor, 4>& slabs,
        std::uint32_t node_capacity);

    // Uses fixed stack storage for the largest node; neither call allocates.
    std::uint64_t Allocate(const ArtBumpNodeRecord& record);
    std::uint64_t Clone(std::uint64_t source_reference);
    void DiscardUnpublished(std::uint64_t reference);
    void ReclaimPublished(std::uint64_t reference);

    ArtBumpNodeRecord Read(std::uint64_t reference) const;
    ArtBumpNodeRecord ReadForRecovery(std::uint64_t reference) const;
    ArtBumpNodeKind ReferenceKind(std::uint64_t reference) const;
    std::uint32_t SlotIndex(std::uint64_t reference) const;
    bool IsAllocated(std::uint64_t reference) const;

    // Preparation owns every vector allocation. Apply is allocation-free,
    // valid for normal commit/Clear and recovery, and returns normal access.
    PreparedRebuild PrepareRebuild(
        ArtBumpNodeLiveBitmaps live_bitmaps,
        ArtBumpNodeLiveBitCounts live_bit_counts) &&;
    ArtBumpNodeStore Rebuild(
        ArtBumpNodeLiveBitmaps live_bitmaps,
        ArtBumpNodeLiveBitCounts live_bit_counts) &&;

    void Validate() const;
    std::uint32_t Capacity(ArtBumpNodeKind kind) const;
    std::uint32_t HighWater(ArtBumpNodeKind kind) const;
    std::uint32_t UsedCount(ArtBumpNodeKind kind) const;
    std::uint64_t FreeHead(ArtBumpNodeKind kind) const;

private:
    struct Slab {
        std::uint8_t* metadata = nullptr;
        std::uint8_t* zone = nullptr;
        ArtBumpSlabDescriptor descriptor{};
        std::uint32_t capacity = 0;
        std::uint32_t payload_bytes = 0;
    };
    ArtBumpNodeStore(
        std::uint8_t* region_base,
        std::size_t region_bytes,
        const std::array<ArtBumpSlabDescriptor, 4>& slabs,
        std::uint32_t node_capacity,
        bool recovery_access) noexcept;

    Slab& SlabFor(ArtBumpNodeKind kind);
    const Slab& SlabFor(ArtBumpNodeKind kind) const;
    void ValidateGeometry(AllocatorErrorCode code) const;
    void ValidateReferences(
        const ArtBumpNodeRecord& record,
        bool require_allocated,
        AllocatorErrorCode code) const;

    std::uint8_t* region_base_ = nullptr;
    std::size_t region_bytes_ = 0;
    std::array<ArtBumpSlabDescriptor, 4> descriptors_{};
    std::array<Slab, 4> slabs_{};
    std::uint32_t node_capacity_ = 0;
    bool recovery_access_ = false;
};

class ArtBumpNodeStore::PreparedRebuild final {
public:
    using WriteCutHook = void (*)(
        void* context,
        std::size_t slab_index,
        ArtBumpRebuildCut cut) noexcept;

    PreparedRebuild(const PreparedRebuild&) = delete;
    PreparedRebuild& operator=(const PreparedRebuild&) = delete;
    PreparedRebuild(PreparedRebuild&& other) noexcept;
    PreparedRebuild& operator=(PreparedRebuild&& other) noexcept;

    ArtBumpNodeStore Apply(
        WriteCutHook hook = nullptr,
        void* context = nullptr) &&;
    ArtBumpNodeStore Cancel() && noexcept;

private:
    friend class ArtBumpNodeStore;
    PreparedRebuild(
        ArtBumpNodeStore store,
        ArtBumpNodeLiveBitmaps live_bitmaps,
        ArtBumpNodeLiveBitCounts live_bit_counts,
        std::array<std::uint32_t, 4> rebuilt_high_water,
        std::array<std::uint32_t, 4> rebuilt_used_count) noexcept;

    ArtBumpNodeStore store_;
    ArtBumpNodeLiveBitmaps live_bitmaps_;
    ArtBumpNodeLiveBitCounts live_bit_counts_{};
    std::array<std::uint32_t, 4> rebuilt_high_water_{};
    std::array<std::uint32_t, 4> rebuilt_used_count_{};
    bool ready_ = true;
};

// Headerless raw-zone view. The descriptor points directly into an accepted
// 384-byte header, so top/epoch updates remain persistent manual-LE writes.
class ArtBumpRawZone final {
public:
    class PreparedState;

    static ArtBumpRawZone Attach(
        void* region_base,
        std::size_t region_bytes,
        void* encoded_zone_descriptor,
        std::size_t descriptor_bytes,
        std::uint64_t expected_begin,
        std::uint64_t expected_bytes);
    // Active-journal recovery trusts only immutable begin/bytes. Mutable
    // top/epoch may be torn or stale and are deliberately not validated.
    static ArtBumpRawZone AttachForRecovery(
        void* region_base,
        std::size_t region_bytes,
        void* encoded_zone_descriptor,
        std::size_t descriptor_bytes,
        std::uint64_t expected_begin,
        std::uint64_t expected_bytes);

    std::uint64_t Begin() const noexcept;
    std::uint64_t Bytes() const noexcept;
    std::uint64_t TopAcquire() const noexcept;
    std::uint32_t Epoch() const noexcept;
    std::uint64_t End() const;
    std::uint64_t Remaining() const;

    std::uint64_t Allocate(const void* bytes, std::size_t length);
    const std::uint8_t* ReadBounded(
        std::uint64_t offset,
        std::size_t length,
        std::uint64_t recorded_top) const;
    std::uint8_t* MutableBounded(
        std::uint64_t offset,
        std::size_t length,
        std::uint64_t recorded_top) const;
    void ValidateInterval(
        std::uint64_t offset,
        std::size_t length,
        std::uint64_t recorded_top) const;

    void StoreTopRelease(std::uint64_t top) const noexcept;
    void StoreEpoch(std::uint32_t epoch) const noexcept;
    PreparedState PrepareState(std::uint64_t top, std::uint32_t epoch) const;

private:
    ArtBumpRawZone(
        std::uint8_t* region_base,
        std::uint8_t* descriptor) noexcept;

    std::uint8_t* region_base_ = nullptr;
    std::uint8_t* descriptor_ = nullptr;
};

class ArtBumpRawZone::PreparedState final {
public:
    void Apply() const noexcept;

private:
    friend class ArtBumpRawZone;
    PreparedState(ArtBumpRawZone zone, std::uint64_t top, std::uint32_t epoch)
        noexcept : zone_(zone), top_(top), epoch_(epoch) {}
    ArtBumpRawZone zone_;
    std::uint64_t top_ = 0;
    std::uint32_t epoch_ = 0;
};

static_assert(ArtBumpNodeCodec::kPayloadBytes[0] == 64);
static_assert(ArtBumpNodeCodec::kPayloadBytes[1] == 168);
static_assert(ArtBumpNodeCodec::kPayloadBytes[2] == 664);
static_assert(ArtBumpNodeCodec::kPayloadBytes[3] == 2072);

} // namespace kvspace::detail
