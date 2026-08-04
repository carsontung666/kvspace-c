#include "shm_art_bump_store.h"

#include "shm_lifetime.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

namespace kvspace::detail {
namespace {

constexpr std::array<std::uint8_t, 8> kEngineMagic = {
    'K', 'V', 'A', 'B', 'U', 'M', 'P', '1'};
constexpr std::array<std::uint8_t, 8> kSlabMagic = {
    'K', 'V', 'A', 'S', 'L', 'A', 'B', '1'};
constexpr std::array<std::uint8_t, 8> kLayoutDomain = {
    'A', 'R', 'T', 'B', 'U', 'M', 'P', '4'};
constexpr std::array<std::uint8_t, 8> kCopyDomain = {
    'A', 'B', 'J', 'C', 'O', 'P', 'Y', '1'};
constexpr std::array<std::uint8_t, 8> kReadyDomain = {
    'A', 'B', 'J', 'R', 'E', 'A', 'D', 'Y'};

constexpr std::uint32_t kEngineVersion = 1;
constexpr std::uint32_t kSlabVersion = 1;
constexpr std::uint8_t kHasValueFlag = 1;
constexpr std::uint64_t kFnvOffset = UINT64_C(14695981039346656037);
constexpr std::uint64_t kFnvPrime = UINT64_C(1099511628211);

constexpr std::size_t kCommittedRootOffset = 16;
constexpr std::size_t kActiveZoneOffset = 24;
constexpr std::size_t kHeaderReservedAOffset = 28;
constexpr std::size_t kSlabsOffset = 32;
constexpr std::size_t kRawZonesOffset = 160;
constexpr std::size_t kGeometryHashOffset = 224;
constexpr std::size_t kNodeCapacityOffset = 232;
constexpr std::size_t kPayloadBytesOffset = 236;
constexpr std::size_t kHeaderReservedBOffset = 252;
constexpr std::size_t kJournalOffset = 256;

constexpr std::size_t kJournalOldRootOffset = 0;
constexpr std::size_t kJournalSourceTopOffset = 8;
constexpr std::size_t kJournalGenerationOffset = 16;
constexpr std::size_t kJournalBaseChecksumOffset = 24;
constexpr std::size_t kJournalNewRootOffset = 32;
constexpr std::size_t kJournalTargetTopOffset = 40;
constexpr std::size_t kJournalNodeCountOffset = 48;
constexpr std::size_t kJournalEntryCountOffset = 56;
constexpr std::size_t kJournalLiveBytesOffset = 64;
constexpr std::size_t kJournalReadyChecksumOffset = 72;
constexpr std::size_t kJournalStateOffset = 80;
constexpr std::size_t kJournalSourceZoneOffset = 84;
constexpr std::size_t kJournalTargetZoneOffset = 88;
constexpr std::size_t kJournalSourceEpochOffset = 92;
constexpr std::size_t kJournalTargetEpochOffset = 96;
constexpr std::size_t kJournalReservedOffset = 100;

constexpr std::size_t kNodePrefixOffset = 0;
constexpr std::size_t kNodeValueOffset = 8;
constexpr std::size_t kNodePrefixLengthOffset = 16;
constexpr std::size_t kNodeChildCountOffset = 20;
constexpr std::size_t kNodeKindOffset = 22;
constexpr std::size_t kNodeFlagsOffset = 23;
constexpr std::size_t kNodeBodyOffset = 24;

constexpr std::size_t kSlabPayloadOffset = 12;
constexpr std::size_t kSlabZoneOffsetOffset = 16;
constexpr std::size_t kSlabZoneBytesOffset = 24;
constexpr std::size_t kSlabCapacityOffset = 32;
constexpr std::size_t kSlabHighWaterOffset = 36;
constexpr std::size_t kSlabUsedCountOffset = 40;
constexpr std::size_t kSlabReservedOffset = 44;
constexpr std::size_t kSlabFreeHeadOffset = 48;
constexpr std::size_t kSlabHashOffset = 56;

static_assert(kSlabsOffset + 4U * 32U == kRawZonesOffset);
static_assert(kRawZonesOffset + 2U * 32U == kGeometryHashOffset);
static_assert(kHeaderReservedBOffset + 4U == kJournalOffset);
static_assert(
    kJournalOffset + ArtBumpHeaderCodec::kJournalBytes ==
    ArtBumpHeaderCodec::kHeaderBytes);
static_assert(kJournalReservedOffset + 28U ==
              ArtBumpHeaderCodec::kJournalBytes);
static_assert(kSlabHashOffset + 8U ==
              ArtBumpGeometry::kSlabMetadataBytes);

[[noreturn]] void RaiseArtBump(
    AllocatorErrorCode code,
    const std::string& message) {
    throw AllocatorError(code, "kvspace ArtBump store: " + message);
}

std::uint16_t Load16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t Load32(const std::uint8_t* bytes) noexcept {
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < sizeof(result); ++index) {
        result |= static_cast<std::uint32_t>(bytes[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return result;
}

std::uint64_t Load64(const std::uint8_t* bytes) noexcept {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < sizeof(result); ++index) {
        result |= static_cast<std::uint64_t>(bytes[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return result;
}

void Store16(std::uint8_t* bytes, std::uint16_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
}

void Store32(std::uint8_t* bytes, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
}

void Store64(std::uint8_t* bytes, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
}

struct alignas(4) AtomicLe32 final {
    std::uint8_t bytes[4];
};

struct alignas(8) AtomicLe64 final {
    std::uint8_t bytes[8];
};

static_assert(sizeof(AtomicLe32) == sizeof(std::uint32_t));
static_assert(alignof(AtomicLe32) == alignof(std::uint32_t));
static_assert(sizeof(AtomicLe64) == sizeof(std::uint64_t));
static_assert(alignof(AtomicLe64) == alignof(std::uint64_t));
static_assert(std::is_trivial_v<AtomicLe32>);
static_assert(std::is_standard_layout_v<AtomicLe32>);
static_assert(std::is_trivially_copyable_v<AtomicLe32>);
static_assert(std::is_trivially_destructible_v<AtomicLe32>);
static_assert(std::is_trivial_v<AtomicLe64>);
static_assert(std::is_standard_layout_v<AtomicLe64>);
static_assert(std::is_trivially_copyable_v<AtomicLe64>);
static_assert(std::is_trivially_destructible_v<AtomicLe64>);
static_assert(
    __atomic_always_lock_free(sizeof(AtomicLe32), nullptr),
    "persistent uint32 publication must be lock-free");
static_assert(
    __atomic_always_lock_free(sizeof(AtomicLe64), nullptr),
    "persistent uint64 publication must be lock-free");

std::uint32_t Acquire32(const std::uint8_t* bytes) noexcept {
    AtomicLe32 encoded{};
    __atomic_load(
        reinterpret_cast<const AtomicLe32*>(bytes),
        &encoded,
        __ATOMIC_ACQUIRE);
    return Load32(encoded.bytes);
}

std::uint64_t Acquire64(const std::uint8_t* bytes) noexcept {
    AtomicLe64 encoded{};
    __atomic_load(
        reinterpret_cast<const AtomicLe64*>(bytes),
        &encoded,
        __ATOMIC_ACQUIRE);
    return Load64(encoded.bytes);
}

void Release32(std::uint8_t* bytes, std::uint32_t value) noexcept {
    AtomicLe32 encoded{};
    Store32(encoded.bytes, value);
    __atomic_store(
        reinterpret_cast<AtomicLe32*>(bytes),
        &encoded,
        __ATOMIC_RELEASE);
}

void Release64(std::uint8_t* bytes, std::uint64_t value) noexcept {
    AtomicLe64 encoded{};
    Store64(encoded.bytes, value);
    __atomic_store(
        reinterpret_cast<AtomicLe64*>(bytes),
        &encoded,
        __ATOMIC_RELEASE);
}

bool AllBytes(
    const std::uint8_t* begin,
    const std::uint8_t* end,
    std::uint8_t expected) noexcept {
    return std::all_of(
        begin, end,
        [expected](std::uint8_t value) { return value == expected; });
}

void HashByte(std::uint64_t* hash, std::uint8_t value) noexcept {
    *hash ^= value;
    *hash *= kFnvPrime;
}

template <std::size_t Size>
void HashBytes(
    std::uint64_t* hash,
    const std::array<std::uint8_t, Size>& bytes) noexcept {
    for (const auto value : bytes) HashByte(hash, value);
}

void Hash32(std::uint64_t* hash, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        HashByte(
            hash,
            static_cast<std::uint8_t>(
                value >> static_cast<unsigned>(index * 8U)));
    }
}

void Hash64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        HashByte(
            hash,
            static_cast<std::uint8_t>(
                value >> static_cast<unsigned>(index * 8U)));
    }
}

std::uint64_t CheckedAdd(
    std::uint64_t left,
    std::uint64_t right,
    AllocatorErrorCode code,
    const char* message) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        RaiseArtBump(code, message);
    }
    return left + right;
}

std::uint64_t CheckedMultiply(
    std::uint64_t left,
    std::uint64_t right,
    AllocatorErrorCode code,
    const char* message) {
    if (left != 0 &&
        right > std::numeric_limits<std::uint64_t>::max() / left) {
        RaiseArtBump(code, message);
    }
    return left * right;
}

std::uint64_t AlignUp(
    std::uint64_t value,
    std::uint64_t alignment,
    AllocatorErrorCode code,
    const char* message) {
    if (alignment == 0 || (alignment & (alignment - 1U)) != 0) {
        RaiseArtBump(code, "alignment is not a nonzero power of two");
    }
    const auto added = CheckedAdd(value, alignment - 1U, code, message);
    return added & ~(alignment - 1U);
}

std::size_t KindIndex(
    ArtBumpNodeKind kind,
    AllocatorErrorCode code) {
    switch (kind) {
    case ArtBumpNodeKind::Node4:
    case ArtBumpNodeKind::Node16:
    case ArtBumpNodeKind::Node48:
    case ArtBumpNodeKind::Node256:
        return static_cast<std::size_t>(kind) - 1U;
    }
    RaiseArtBump(code, "invalid ART node kind");
}

ArtBumpNodeKind DecodeKind(
    std::uint8_t raw,
    AllocatorErrorCode code) {
    const auto kind = static_cast<ArtBumpNodeKind>(raw);
    static_cast<void>(KindIndex(kind, code));
    return kind;
}

bool ValidJournalState(ArtBumpJournalState state) noexcept {
    return state == ArtBumpJournalState::Idle ||
        state == ArtBumpJournalState::Copying ||
        state == ArtBumpJournalState::Ready;
}

void ValidateJournal(
    const ArtBumpCompactJournal& journal,
    AllocatorErrorCode code) {
    if (!ValidJournalState(journal.state)) {
        RaiseArtBump(code, "invalid compact-journal state");
    }
    if (journal.state == ArtBumpJournalState::Idle) return;
    if (journal.source_zone > 1 || journal.target_zone > 1 ||
        journal.source_zone == journal.target_zone ||
        journal.source_epoch == 0 || journal.target_epoch == 0) {
        RaiseArtBump(code, "invalid active compact-journal zones");
    }
    if (journal.base_checksum !=
        ArtBumpHeaderCodec::BaseChecksum(journal)) {
        RaiseArtBump(code, "compact-journal base checksum mismatch");
    }
    if (journal.state == ArtBumpJournalState::Ready &&
        journal.ready_checksum !=
            ArtBumpHeaderCodec::ReadyChecksum(journal)) {
        RaiseArtBump(code, "compact-journal ready checksum mismatch");
    }
}

void ValidateSlabDescriptor(
    const ArtBumpSlabDescriptor& descriptor,
    std::uint32_t payload_bytes,
    std::uint32_t capacity,
    std::uint64_t region_bytes,
    AllocatorErrorCode code) {
    if (descriptor.metadata_bytes !=
            ArtBumpGeometry::kSlabMetadataBytes ||
        descriptor.metadata_offset % 64U != 0 ||
        descriptor.zone_offset != CheckedAdd(
            descriptor.metadata_offset,
            ArtBumpGeometry::kSlabMetadataBytes,
            code,
            "slab metadata end overflows") ||
        descriptor.zone_offset == 0) {
        RaiseArtBump(code, "invalid ArtBump slab descriptor");
    }
    const auto expected_zone_bytes = CheckedMultiply(
        capacity, payload_bytes, code, "slab zone size overflows");
    if (descriptor.zone_bytes != expected_zone_bytes) {
        RaiseArtBump(code, "slab zone is not exact-capacity geometry");
    }
    const auto end = CheckedAdd(
        descriptor.zone_offset,
        descriptor.zone_bytes,
        code,
        "slab end overflows");
    if (end > region_bytes) {
        RaiseArtBump(code, "slab lies outside the mapped region");
    }
}

bool BitmapBit(
    const std::vector<std::uint8_t>& bitmap,
    std::size_t bit_count,
    std::uint32_t id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= bit_count) return false;
    return (bitmap[index / 8U] &
            static_cast<std::uint8_t>(1U <<
                static_cast<unsigned>(index % 8U))) != 0;
}

void SetBitmapBit(
    std::vector<std::uint8_t>* bitmap,
    std::uint32_t id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    (*bitmap)[index / 8U] |= static_cast<std::uint8_t>(
        1U << static_cast<unsigned>(index % 8U));
}

void ValidateBitmap(
    const std::vector<std::uint8_t>& bitmap,
    std::size_t bit_count,
    std::uint32_t capacity) {
    if (bit_count > static_cast<std::size_t>(capacity)) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "live bitmap count exceeds slab capacity");
    }
    const auto required_bytes = (bit_count + 7U) / 8U;
    if (bitmap.size() < required_bytes) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "live bitmap storage is too small");
    }
    if (required_bytes != 0 && bit_count % 8U != 0) {
        const auto valid = static_cast<unsigned>(bit_count % 8U);
        const auto invalid_mask = static_cast<std::uint8_t>(
            static_cast<unsigned>(UINT8_MAX) << valid);
        if ((bitmap[required_bytes - 1U] & invalid_mask) != 0) {
            RaiseArtBump(
                AllocatorErrorCode::InvalidArgument,
                "live bitmap has set bits beyond its count");
        }
    }
    for (std::size_t index = required_bytes; index < bitmap.size(); ++index) {
        if (bitmap[index] != 0) {
            RaiseArtBump(
                AllocatorErrorCode::InvalidArgument,
                "live bitmap has hidden set bits");
        }
    }
}

} // namespace

std::uint64_t ArtBumpGeometry::QueueCapacity(std::uint64_t queue_limit) {
    if (queue_limit == 0) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "queue limit must be nonzero");
    }
    const auto ten_q = CheckedMultiply(
        queue_limit, 10, AllocatorErrorCode::Capacity,
        "queue-capacity numerator overflows");
    const auto required = CheckedAdd(
        ten_q, 6, AllocatorErrorCode::Capacity,
        "queue-capacity ceiling overflows") / 7U;
    std::uint64_t capacity = 1;
    while (capacity < required) {
        if (capacity > std::numeric_limits<std::uint64_t>::max() / 2U) {
            RaiseArtBump(
                AllocatorErrorCode::Capacity,
                "queue capacity has no representable power of two");
        }
        capacity *= 2U;
    }
    return capacity;
}

ArtBumpGeometry ArtBumpGeometry::Compute(
    std::uint64_t region_size_value,
    std::uint64_t entry_limit_value,
    std::uint64_t queue_capacity_value,
    std::uint64_t page_bytes,
    std::uint64_t common_header_bytes,
    std::uint64_t queue_slot_bytes) {
    if (region_size_value == 0 || entry_limit_value == 0 ||
        queue_capacity_value == 0) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "region, entry limit, and queue capacity must be nonzero");
    }
    if ((queue_capacity_value & (queue_capacity_value - 1U)) != 0) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "queue capacity must be a power of two");
    }
    if (page_bytes != kPageBytes ||
        common_header_bytes != kCommonHeaderBytes ||
        queue_slot_bytes != kQueueSlotBytes) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "ArtBump ABI 4 requires the exact supported common geometry");
    }
    const auto node_capacity64 = CheckedAdd(
        CheckedMultiply(
            entry_limit_value, 4, AllocatorErrorCode::Capacity,
            "ArtBump node capacity overflows"),
        1, AllocatorErrorCode::Capacity,
        "ArtBump node capacity overflows");
    if (node_capacity64 > std::numeric_limits<std::uint32_t>::max()) {
        RaiseArtBump(
            AllocatorErrorCode::Capacity,
            "ArtBump node capacity exceeds uint32");
    }

    ArtBumpGeometry result;
    result.region_size = region_size_value;
    result.entry_limit = entry_limit_value;
    result.queue_capacity = queue_capacity_value;
    result.node_capacity = static_cast<std::uint32_t>(node_capacity64);
    result.queue_offset = AlignUp(
        common_header_bytes, page_bytes, AllocatorErrorCode::Capacity,
        "common-header page alignment overflows");
    result.queue_table_bytes = CheckedMultiply(
        queue_capacity_value, queue_slot_bytes, AllocatorErrorCode::Capacity,
        "queue table size overflows");
    result.heap_offset = AlignUp(
        CheckedAdd(
            result.queue_offset, result.queue_table_bytes,
            AllocatorErrorCode::Capacity, "queue table end overflows"),
        page_bytes, AllocatorErrorCode::Capacity,
        "queue-table page alignment overflows");
    if (region_size_value < result.heap_offset) {
        RaiseArtBump(
            AllocatorErrorCode::Capacity,
            "region ends before the common heap");
    }
    const auto usable = region_size_value - result.heap_offset;
    result.heap_bytes = (usable / 8U) & ~(page_bytes - 1U);
    if (result.heap_bytes < page_bytes) {
        RaiseArtBump(
            AllocatorErrorCode::Capacity,
            "common heap has less than one page");
    }
    result.engine_offset = CheckedAdd(
        result.heap_offset, result.heap_bytes,
        AllocatorErrorCode::Capacity, "engine offset overflows");
    result.engine_bytes = region_size_value - result.engine_offset;

    std::uint64_t cursor = AlignUp(
        CheckedAdd(
            result.engine_offset, kEngineHeaderBytes,
            AllocatorErrorCode::Capacity, "engine header end overflows"),
        64, AllocatorErrorCode::Capacity,
        "first slab alignment overflows");
    for (std::size_t index = 0; index < result.slabs.size(); ++index) {
        auto& slab = result.slabs[index];
        slab.metadata_offset = cursor;
        slab.metadata_bytes = kSlabMetadataBytes;
        slab.zone_offset = CheckedAdd(
            cursor, kSlabMetadataBytes,
            AllocatorErrorCode::Capacity, "slab metadata end overflows");
        slab.zone_bytes = CheckedMultiply(
            result.node_capacity,
            ArtBumpNodeCodec::kPayloadBytes[index],
            AllocatorErrorCode::Capacity,
            "slab zone size overflows");
        cursor = AlignUp(
            CheckedAdd(
                slab.zone_offset, slab.zone_bytes,
                AllocatorErrorCode::Capacity, "slab zone end overflows"),
            64, AllocatorErrorCode::Capacity,
            "slab alignment overflows");
    }
    if (cursor > region_size_value) {
        RaiseArtBump(
            AllocatorErrorCode::Capacity,
            "fixed node slabs exceed the engine tail");
    }
    const auto raw_budget = region_size_value - cursor;
    const auto zone_bytes = raw_budget / 2U;
    if (zone_bytes == 0) {
        RaiseArtBump(
            AllocatorErrorCode::Capacity,
            "engine tail has no nonempty raw zone");
    }
    result.raw_zones[0] = ArtBumpRawZoneDescriptor{
        cursor, zone_bytes, cursor, 1};
    const auto zone1_begin = CheckedAdd(
        cursor, zone_bytes, AllocatorErrorCode::Capacity,
        "second raw zone offset overflows");
    result.raw_zones[1] = ArtBumpRawZoneDescriptor{
        zone1_begin, zone_bytes, zone1_begin, 1};
    const auto owned_raw = CheckedMultiply(
        zone_bytes, 2, AllocatorErrorCode::Capacity,
        "raw-zone size overflows");
    result.terminal_tail_bytes = raw_budget - owned_raw;
    result.geometry_hash = ArtBumpHeaderCodec::GeometryHash(
        result.entry_limit,
        result.node_capacity,
        result.slabs,
        result.raw_zones);
    return result;
}

ArtBumpGeometry ArtBumpGeometry::DefaultProfile() {
    return Compute(
        UINT64_C(1073741824),
        UINT64_C(65536),
        QueueCapacity(UINT64_C(4096)));
}

ArtBumpEngineHeader ArtBumpGeometry::InitialHeader() const {
    if (geometry_hash != ArtBumpHeaderCodec::GeometryHash(
            entry_limit, node_capacity, slabs, raw_zones)) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "geometry object has a stale hash");
    }
    ArtBumpEngineHeader result;
    result.slabs = slabs;
    result.raw_zones = raw_zones;
    result.geometry_hash = geometry_hash;
    result.node_capacity = node_capacity;
    return result;
}

std::uint64_t ArtBumpHeaderCodec::BaseChecksum(
    const ArtBumpCompactJournal& journal) {
    std::uint64_t hash = kFnvOffset;
    HashBytes(&hash, kCopyDomain);
    Hash64(&hash, journal.old_root);
    Hash64(&hash, journal.source_top);
    Hash64(&hash, journal.operation_generation);
    Hash32(&hash, journal.source_zone);
    Hash32(&hash, journal.target_zone);
    Hash32(&hash, journal.source_epoch);
    Hash32(&hash, journal.target_epoch);
    return hash;
}

std::uint64_t ArtBumpHeaderCodec::ReadyChecksum(
    const ArtBumpCompactJournal& journal) {
    std::uint64_t hash = kFnvOffset;
    HashBytes(&hash, kReadyDomain);
    Hash64(&hash, journal.base_checksum);
    Hash64(&hash, journal.new_root);
    Hash64(&hash, journal.target_top);
    Hash64(&hash, journal.node_count);
    Hash64(&hash, journal.entry_count);
    Hash64(&hash, journal.engine_live_bytes);
    return hash;
}

std::uint64_t ArtBumpHeaderCodec::EngineLayoutHash(
    std::uint64_t common_header_bytes,
    std::uint64_t queue_slot_bytes,
    std::uint64_t blob_header_bytes,
    std::uint64_t mutex_bytes,
    std::uint64_t condition_bytes) {
    std::uint64_t hash = kFnvOffset;
    HashBytes(&hash, kLayoutDomain);
    for (const auto value : {
             common_header_bytes,
             UINT64_C(384),
             UINT64_C(128),
             UINT64_C(64),
             UINT64_C(64),
             UINT64_C(168),
             UINT64_C(664),
             UINT64_C(2072),
             queue_slot_bytes,
             blob_header_bytes,
             mutex_bytes,
             condition_bytes}) {
        Hash64(&hash, value);
    }
    return hash;
}

std::uint64_t ArtBumpHeaderCodec::GeometryHash(
    std::uint64_t entry_limit,
    std::uint32_t node_capacity,
    const std::array<ArtBumpSlabDescriptor, 4>& slabs,
    const std::array<ArtBumpRawZoneDescriptor, 2>& raw_zones) {
    std::uint64_t hash = kFnvOffset;
    HashBytes(&hash, kEngineMagic);
    Hash32(&hash, kEngineVersion);
    Hash32(&hash, static_cast<std::uint32_t>(kHeaderBytes));
    Hash64(&hash, entry_limit);
    Hash32(&hash, node_capacity);
    for (const auto payload : ArtBumpNodeCodec::kPayloadBytes) {
        Hash32(&hash, static_cast<std::uint32_t>(payload));
    }
    for (const auto& slab : slabs) {
        Hash64(&hash, slab.metadata_offset);
        Hash64(&hash, slab.metadata_bytes);
        Hash64(&hash, slab.zone_offset);
        Hash64(&hash, slab.zone_bytes);
    }
    for (const auto& zone : raw_zones) {
        Hash64(&hash, zone.begin);
        Hash64(&hash, zone.bytes);
    }
    return hash;
}

std::uint64_t ArtBumpHeaderCodec::SlabGeometryHash(
    std::uint32_t payload_bytes,
    std::uint64_t zone_offset,
    std::uint64_t zone_bytes,
    std::uint32_t capacity) {
    std::uint64_t hash = kFnvOffset;
    HashBytes(&hash, kSlabMagic);
    Hash32(&hash, kSlabVersion);
    Hash32(&hash, payload_bytes);
    Hash64(&hash, zone_offset);
    Hash64(&hash, zone_bytes);
    Hash32(&hash, capacity);
    return hash;
}

ArtBumpHeaderCodec::JournalBytes ArtBumpHeaderCodec::EncodeJournal(
    const ArtBumpCompactJournal& journal) {
    ValidateJournal(journal, AllocatorErrorCode::InvalidArgument);
    JournalBytes encoded{};
    Store64(encoded.data() + kJournalOldRootOffset, journal.old_root);
    Store64(encoded.data() + kJournalSourceTopOffset, journal.source_top);
    Store64(
        encoded.data() + kJournalGenerationOffset,
        journal.operation_generation);
    Store64(
        encoded.data() + kJournalBaseChecksumOffset,
        journal.base_checksum);
    Store64(encoded.data() + kJournalNewRootOffset, journal.new_root);
    Store64(encoded.data() + kJournalTargetTopOffset, journal.target_top);
    Store64(encoded.data() + kJournalNodeCountOffset, journal.node_count);
    Store64(encoded.data() + kJournalEntryCountOffset, journal.entry_count);
    Store64(
        encoded.data() + kJournalLiveBytesOffset,
        journal.engine_live_bytes);
    Store64(
        encoded.data() + kJournalReadyChecksumOffset,
        journal.ready_checksum);
    Store32(
        encoded.data() + kJournalStateOffset,
        static_cast<std::uint32_t>(journal.state));
    Store32(
        encoded.data() + kJournalSourceZoneOffset,
        journal.source_zone);
    Store32(
        encoded.data() + kJournalTargetZoneOffset,
        journal.target_zone);
    Store32(
        encoded.data() + kJournalSourceEpochOffset,
        journal.source_epoch);
    Store32(
        encoded.data() + kJournalTargetEpochOffset,
        journal.target_epoch);
    return encoded;
}

ArtBumpCompactJournal ArtBumpHeaderCodec::DecodeJournal(
    const void* encoded,
    std::size_t encoded_bytes) {
    if (encoded == nullptr || encoded_bytes != kJournalBytes) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "compact journal does not have exactly 128 bytes");
    }
    const auto* bytes = static_cast<const std::uint8_t*>(encoded);
    if (!AllBytes(
            bytes + kJournalReservedOffset,
            bytes + kJournalBytes,
            0)) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "compact journal has nonzero reserved bytes");
    }
    ArtBumpCompactJournal result;
    result.old_root = Load64(bytes + kJournalOldRootOffset);
    result.source_top = Load64(bytes + kJournalSourceTopOffset);
    result.operation_generation = Load64(bytes + kJournalGenerationOffset);
    result.base_checksum = Load64(bytes + kJournalBaseChecksumOffset);
    result.new_root = Load64(bytes + kJournalNewRootOffset);
    result.target_top = Load64(bytes + kJournalTargetTopOffset);
    result.node_count = Load64(bytes + kJournalNodeCountOffset);
    result.entry_count = Load64(bytes + kJournalEntryCountOffset);
    result.engine_live_bytes = Load64(bytes + kJournalLiveBytesOffset);
    result.ready_checksum = Load64(bytes + kJournalReadyChecksumOffset);
    result.state = static_cast<ArtBumpJournalState>(
        Load32(bytes + kJournalStateOffset));
    result.source_zone = Load32(bytes + kJournalSourceZoneOffset);
    result.target_zone = Load32(bytes + kJournalTargetZoneOffset);
    result.source_epoch = Load32(bytes + kJournalSourceEpochOffset);
    result.target_epoch = Load32(bytes + kJournalTargetEpochOffset);
    ValidateJournal(result, AllocatorErrorCode::Corrupt);
    return result;
}

ArtBumpHeaderCodec::HeaderBytes ArtBumpHeaderCodec::Encode(
    const ArtBumpEngineHeader& header) {
    if (header.active_zone > 1 || header.node_capacity == 0 ||
        header.payload_bytes !=
            std::array<std::uint32_t, 4>{64, 168, 664, 2072}) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "invalid ArtBump engine header fields");
    }
    for (std::size_t index = 0; index < header.slabs.size(); ++index) {
        ValidateSlabDescriptor(
            header.slabs[index],
            header.payload_bytes[index],
            header.node_capacity,
            std::numeric_limits<std::uint64_t>::max(),
            AllocatorErrorCode::InvalidArgument);
        if (index != 0) {
            const auto previous_end = CheckedAdd(
                header.slabs[index - 1U].zone_offset,
                header.slabs[index - 1U].zone_bytes,
                AllocatorErrorCode::InvalidArgument,
                "slab end overflows");
            if (header.slabs[index].metadata_offset != AlignUp(
                    previous_end, 64,
                    AllocatorErrorCode::InvalidArgument,
                    "slab alignment overflows")) {
                RaiseArtBump(
                    AllocatorErrorCode::InvalidArgument,
                    "slab descriptors are not canonical and consecutive");
            }
        }
    }
    for (const auto& zone : header.raw_zones) {
        const auto end = CheckedAdd(
            zone.begin, zone.bytes,
            AllocatorErrorCode::InvalidArgument,
            "raw-zone end overflows");
        if (zone.bytes == 0 || zone.epoch == 0 ||
            zone.top < zone.begin || zone.top > end) {
            RaiseArtBump(
                AllocatorErrorCode::InvalidArgument,
                "invalid raw-zone descriptor");
        }
    }
    if (header.raw_zones[1].begin != CheckedAdd(
            header.raw_zones[0].begin,
            header.raw_zones[0].bytes,
            AllocatorErrorCode::InvalidArgument,
            "raw-zone adjacency overflows") ||
        header.raw_zones[1].bytes != header.raw_zones[0].bytes) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "raw zones are not equal consecutive halves");
    }
    ValidateJournal(header.journal, AllocatorErrorCode::InvalidArgument);

    HeaderBytes encoded{};
    std::copy(kEngineMagic.begin(), kEngineMagic.end(), encoded.begin());
    Store32(encoded.data() + 8, kEngineVersion);
    Store32(encoded.data() + 12, static_cast<std::uint32_t>(kHeaderBytes));
    Store64(
        encoded.data() + kCommittedRootOffset,
        header.committed_root);
    Store32(encoded.data() + kActiveZoneOffset, header.active_zone);
    for (std::size_t index = 0; index < header.slabs.size(); ++index) {
        const auto offset = kSlabsOffset + index * 32U;
        const auto& slab = header.slabs[index];
        Store64(encoded.data() + offset, slab.metadata_offset);
        Store64(encoded.data() + offset + 8U, slab.metadata_bytes);
        Store64(encoded.data() + offset + 16U, slab.zone_offset);
        Store64(encoded.data() + offset + 24U, slab.zone_bytes);
    }
    for (std::size_t index = 0; index < header.raw_zones.size(); ++index) {
        const auto offset = kRawZonesOffset + index * 32U;
        const auto& zone = header.raw_zones[index];
        Store64(encoded.data() + offset, zone.begin);
        Store64(encoded.data() + offset + 8U, zone.bytes);
        Store64(encoded.data() + offset + 16U, zone.top);
        Store32(encoded.data() + offset + 24U, zone.epoch);
    }
    Store64(encoded.data() + kGeometryHashOffset, header.geometry_hash);
    Store32(encoded.data() + kNodeCapacityOffset, header.node_capacity);
    for (std::size_t index = 0; index < header.payload_bytes.size(); ++index) {
        Store32(
            encoded.data() + kPayloadBytesOffset + index * 4U,
            header.payload_bytes[index]);
    }
    const auto journal = EncodeJournal(header.journal);
    std::copy(
        journal.begin(), journal.end(), encoded.begin() + kJournalOffset);
    return encoded;
}

ArtBumpEngineHeader ArtBumpHeaderCodec::Decode(
    const void* encoded,
    std::size_t encoded_bytes) {
    if (encoded == nullptr || encoded_bytes != kHeaderBytes) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "engine header does not have exactly 384 bytes");
    }
    const auto* bytes = static_cast<const std::uint8_t*>(encoded);
    if (!std::equal(kEngineMagic.begin(), kEngineMagic.end(), bytes) ||
        Load32(bytes + 8) != kEngineVersion ||
        Load32(bytes + 12) != kHeaderBytes) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "bad ArtBump engine magic, version, or header size");
    }
    if (Load32(bytes + kHeaderReservedAOffset) != 0 ||
        !AllBytes(
            bytes + kHeaderReservedBOffset,
            bytes + kJournalOffset,
            0)) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "ArtBump engine header has nonzero reserved bytes");
    }

    ArtBumpEngineHeader result;
    result.committed_root = Load64(bytes + kCommittedRootOffset);
    result.active_zone = Load32(bytes + kActiveZoneOffset);
    for (std::size_t index = 0; index < result.slabs.size(); ++index) {
        const auto offset = kSlabsOffset + index * 32U;
        result.slabs[index] = ArtBumpSlabDescriptor{
            Load64(bytes + offset),
            Load64(bytes + offset + 8U),
            Load64(bytes + offset + 16U),
            Load64(bytes + offset + 24U)};
    }
    for (std::size_t index = 0; index < result.raw_zones.size(); ++index) {
        const auto offset = kRawZonesOffset + index * 32U;
        if (Load32(bytes + offset + 28U) != 0) {
            RaiseArtBump(
                AllocatorErrorCode::Corrupt,
                "raw-zone descriptor reserved word is nonzero");
        }
        result.raw_zones[index] = ArtBumpRawZoneDescriptor{
            Load64(bytes + offset),
            Load64(bytes + offset + 8U),
            Load64(bytes + offset + 16U),
            Load32(bytes + offset + 24U)};
    }
    result.geometry_hash = Load64(bytes + kGeometryHashOffset);
    result.node_capacity = Load32(bytes + kNodeCapacityOffset);
    for (std::size_t index = 0; index < result.payload_bytes.size(); ++index) {
        result.payload_bytes[index] = Load32(
            bytes + kPayloadBytesOffset + index * 4U);
    }
    result.journal = DecodeJournal(
        bytes + kJournalOffset, kJournalBytes);

    // Reuse the complete encode-side structural validation without accepting
    // its InvalidArgument classification for persistent bytes.
    try {
        static_cast<void>(Encode(result));
    } catch (const AllocatorError&) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "invalid ArtBump engine-header geometry");
    }
    if (result.journal.state != ArtBumpJournalState::Idle) {
        const auto& source = result.raw_zones[result.journal.source_zone];
        const auto source_end = CheckedAdd(
            source.begin, source.bytes,
            AllocatorErrorCode::Corrupt,
            "source raw-zone end overflows");
        if (result.journal.source_top < source.begin ||
            result.journal.source_top > source_end) {
            RaiseArtBump(
                AllocatorErrorCode::Corrupt,
                "journal source top is outside its raw zone");
        }
        if (result.journal.state == ArtBumpJournalState::Ready) {
            const auto& target = result.raw_zones[result.journal.target_zone];
            const auto target_end = CheckedAdd(
                target.begin, target.bytes,
                AllocatorErrorCode::Corrupt,
                "target raw-zone end overflows");
            if (result.journal.target_top < target.begin ||
                result.journal.target_top > target_end) {
                RaiseArtBump(
                    AllocatorErrorCode::Corrupt,
                    "journal target top is outside its raw zone");
            }
        }
    }
    return result;
}

ArtBumpEngineHeader ArtBumpHeaderCodec::DecodeForRecovery(
    const void* encoded,
    std::size_t encoded_bytes) {
    if (encoded == nullptr || encoded_bytes != kHeaderBytes) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "recovery engine header does not have exactly 384 bytes");
    }
    const auto* original = static_cast<const std::uint8_t*>(encoded);
    const auto journal = DecodeJournal(
        original + kJournalOffset, kJournalBytes);
    if (journal.state == ArtBumpJournalState::Idle) {
        return Decode(encoded, encoded_bytes);
    }

    HeaderBytes canonical_mutable{};
    std::copy_n(original, canonical_mutable.size(), canonical_mutable.begin());
    Store32(canonical_mutable.data() + kActiveZoneOffset, 0);
    for (std::size_t index = 0; index < 2; ++index) {
        const auto zone_offset = kRawZonesOffset + index * 32U;
        const auto begin = Load64(original + zone_offset);
        Store64(canonical_mutable.data() + zone_offset + 16U, begin);
        Store32(canonical_mutable.data() + zone_offset + 24U, 1);
    }
    auto result = Decode(canonical_mutable.data(), canonical_mutable.size());
    result.active_zone = Load32(original + kActiveZoneOffset);
    for (std::size_t index = 0; index < 2; ++index) {
        const auto zone_offset = kRawZonesOffset + index * 32U;
        result.raw_zones[index].top = Load64(original + zone_offset + 16U);
        result.raw_zones[index].epoch = Load32(original + zone_offset + 24U);
    }
    return result;
}

ArtBumpHeaderView ArtBumpHeaderView::Initialize(
    void* encoded,
    std::size_t encoded_bytes,
    const ArtBumpEngineHeader& header) {
    if (encoded == nullptr || encoded_bytes != ArtBumpHeaderCodec::kHeaderBytes ||
        reinterpret_cast<std::uintptr_t>(encoded) % 8U != 0) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "invalid or unaligned engine-header region");
    }
    const auto bytes = ArtBumpHeaderCodec::Encode(header);
    std::memcpy(encoded, bytes.data(), bytes.size());
    StartImplicitLifetimes(encoded, encoded_bytes);
    return ArtBumpHeaderView(static_cast<std::uint8_t*>(encoded));
}

ArtBumpHeaderView ArtBumpHeaderView::Attach(
    void* encoded,
    std::size_t encoded_bytes) {
    if (encoded == nullptr || encoded_bytes != ArtBumpHeaderCodec::kHeaderBytes ||
        reinterpret_cast<std::uintptr_t>(encoded) % 8U != 0) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "invalid or unaligned engine-header attach region");
    }
    StartImplicitLifetimes(encoded, encoded_bytes);
    static_cast<void>(ArtBumpHeaderCodec::Decode(encoded, encoded_bytes));
    return ArtBumpHeaderView(static_cast<std::uint8_t*>(encoded));
}

ArtBumpHeaderView ArtBumpHeaderView::AttachForRecovery(
    void* encoded,
    std::size_t encoded_bytes) {
    if (encoded == nullptr || encoded_bytes != ArtBumpHeaderCodec::kHeaderBytes ||
        reinterpret_cast<std::uintptr_t>(encoded) % 8U != 0) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "invalid or unaligned recovery engine-header region");
    }
    StartImplicitLifetimes(encoded, encoded_bytes);
    static_cast<void>(
        ArtBumpHeaderCodec::DecodeForRecovery(encoded, encoded_bytes));
    return ArtBumpHeaderView(static_cast<std::uint8_t*>(encoded));
}

ArtBumpEngineHeader ArtBumpHeaderView::Decode() const {
    return ArtBumpHeaderCodec::Decode(
        bytes_, ArtBumpHeaderCodec::kHeaderBytes);
}

ArtBumpEngineHeader ArtBumpHeaderView::DecodeForRecovery() const {
    return ArtBumpHeaderCodec::DecodeForRecovery(
        bytes_, ArtBumpHeaderCodec::kHeaderBytes);
}

std::uint64_t ArtBumpHeaderView::CommittedRootAcquire() const noexcept {
    return Acquire64(bytes_ + kCommittedRootOffset);
}

void ArtBumpHeaderView::StoreCommittedRootRelease(
    std::uint64_t root) noexcept {
    Release64(bytes_ + kCommittedRootOffset, root);
}

std::uint32_t ArtBumpHeaderView::ActiveZone() const noexcept {
    return Load32(bytes_ + kActiveZoneOffset);
}

void ArtBumpHeaderView::StoreActiveZone(std::uint32_t zone) noexcept {
    Store32(bytes_ + kActiveZoneOffset, zone);
}

ArtBumpCompactJournal ArtBumpHeaderView::JournalAcquire() const {
    const auto state = Acquire32(
        bytes_ + kJournalOffset + kJournalStateOffset);
    ArtBumpHeaderCodec::JournalBytes snapshot{};
    std::copy_n(
        bytes_ + kJournalOffset,
        snapshot.size(),
        snapshot.begin());
    Store32(snapshot.data() + kJournalStateOffset, state);
    return ArtBumpHeaderCodec::DecodeJournal(
        snapshot.data(), snapshot.size());
}

void ArtBumpHeaderView::StoreJournalPayload(
    const ArtBumpCompactJournal& journal) {
    ValidateJournal(journal, AllocatorErrorCode::InvalidArgument);
    auto* bytes = bytes_ + kJournalOffset;
    Store64(bytes + kJournalOldRootOffset, journal.old_root);
    Store64(bytes + kJournalSourceTopOffset, journal.source_top);
    Store64(bytes + kJournalGenerationOffset, journal.operation_generation);
    Store64(bytes + kJournalBaseChecksumOffset, journal.base_checksum);
    Store64(bytes + kJournalNewRootOffset, journal.new_root);
    Store64(bytes + kJournalTargetTopOffset, journal.target_top);
    Store64(bytes + kJournalNodeCountOffset, journal.node_count);
    Store64(bytes + kJournalEntryCountOffset, journal.entry_count);
    Store64(bytes + kJournalLiveBytesOffset, journal.engine_live_bytes);
    Store64(bytes + kJournalReadyChecksumOffset, journal.ready_checksum);
    Store32(bytes + kJournalSourceZoneOffset, journal.source_zone);
    Store32(bytes + kJournalTargetZoneOffset, journal.target_zone);
    Store32(bytes + kJournalSourceEpochOffset, journal.source_epoch);
    Store32(bytes + kJournalTargetEpochOffset, journal.target_epoch);
    std::fill(
        bytes + kJournalReservedOffset,
        bytes + ArtBumpHeaderCodec::kJournalBytes,
        0);
}

void ArtBumpHeaderView::StoreJournalReadyFields(
    const ArtBumpCompactJournal& journal) noexcept {
    StoreJournalReadyField(journal, ArtBumpReadyField::NewRoot);
    StoreJournalReadyField(journal, ArtBumpReadyField::TargetTop);
    StoreJournalReadyField(journal, ArtBumpReadyField::NodeCount);
    StoreJournalReadyField(journal, ArtBumpReadyField::EntryCount);
    StoreJournalReadyField(journal, ArtBumpReadyField::EngineLiveBytes);
    StoreJournalReadyField(journal, ArtBumpReadyField::ReadyChecksum);
}

void ArtBumpHeaderView::StoreJournalReadyField(
    const ArtBumpCompactJournal& journal,
    ArtBumpReadyField field) noexcept {
    auto* bytes = bytes_ + kJournalOffset;
    switch (field) {
    case ArtBumpReadyField::NewRoot:
        Store64(bytes + kJournalNewRootOffset, journal.new_root);
        return;
    case ArtBumpReadyField::TargetTop:
        Store64(bytes + kJournalTargetTopOffset, journal.target_top);
        return;
    case ArtBumpReadyField::NodeCount:
        Store64(bytes + kJournalNodeCountOffset, journal.node_count);
        return;
    case ArtBumpReadyField::EntryCount:
        Store64(bytes + kJournalEntryCountOffset, journal.entry_count);
        return;
    case ArtBumpReadyField::EngineLiveBytes:
        Store64(bytes + kJournalLiveBytesOffset, journal.engine_live_bytes);
        return;
    case ArtBumpReadyField::ReadyChecksum:
        Store64(bytes + kJournalReadyChecksumOffset, journal.ready_checksum);
        return;
    }
}

void ArtBumpHeaderView::StoreJournalStateRelease(
    ArtBumpJournalState state) noexcept {
    Release32(
        bytes_ + kJournalOffset + kJournalStateOffset,
        static_cast<std::uint32_t>(state));
}

namespace {

void ValidateCanonicalNode(
    const ArtBumpNodeRecord& record,
    AllocatorErrorCode code) {
    static_cast<void>(KindIndex(record.kind, code));
    if ((record.prefix_len == 0) != (record.prefix_offset == 0)) {
        RaiseArtBump(
            code, "node prefix length/offset pair is not canonical");
    }
    if (!record.has_value && record.value_offset != 0) {
        RaiseArtBump(
            code, "missing node value must have zero offset");
    }
    const auto require_default_index = [&] {
        if (!AllBytes(
                record.index.data(),
                record.index.data() + record.index.size(),
                UINT8_MAX)) {
            RaiseArtBump(code, "node contains unused index bytes");
        }
    };
    const auto require_zero_keys = [&] {
        if (!AllBytes(
                record.keys.data(),
                record.keys.data() + record.keys.size(),
                0)) {
            RaiseArtBump(code, "node contains unused key bytes");
        }
    };

    switch (record.kind) {
    case ArtBumpNodeKind::Node4:
    case ArtBumpNodeKind::Node16: {
        const auto capacity = record.kind == ArtBumpNodeKind::Node4
            ? std::size_t{4}
            : std::size_t{16};
        if (record.child_count > capacity) {
            RaiseArtBump(code, "Node4/16 child count exceeds capacity");
        }
        require_default_index();
        for (std::size_t slot = 0; slot < capacity; ++slot) {
            if (slot < record.child_count) {
                if (record.children[slot] == 0) {
                    RaiseArtBump(code, "used Node4/16 child is empty");
                }
                if (slot != 0 &&
                    record.keys[slot - 1U] >= record.keys[slot]) {
                    RaiseArtBump(
                        code, "Node4/16 keys are not strictly increasing");
                }
            } else if (record.keys[slot] != 0 ||
                       record.children[slot] != 0) {
                RaiseArtBump(
                    code, "unused Node4/16 slot is not zero");
            }
        }
        for (std::size_t slot = capacity; slot < record.keys.size(); ++slot) {
            if (record.keys[slot] != 0) {
                RaiseArtBump(code, "out-of-class key byte is nonzero");
            }
        }
        for (std::size_t slot = capacity;
             slot < record.children.size();
             ++slot) {
            if (record.children[slot] != 0) {
                RaiseArtBump(code, "out-of-class child is nonzero");
            }
        }
        break;
    }
    case ArtBumpNodeKind::Node48: {
        require_zero_keys();
        if (record.child_count > 48U) {
            RaiseArtBump(code, "Node48 child count exceeds capacity");
        }
        std::array<bool, 48> referenced{};
        std::size_t indexed = 0;
        for (const auto slot : record.index) {
            if (slot == UINT8_MAX) continue;
            if (slot >= record.child_count || referenced[slot]) {
                RaiseArtBump(code, "Node48 index is not a dense bijection");
            }
            referenced[slot] = true;
            ++indexed;
        }
        if (indexed != record.child_count) {
            RaiseArtBump(code, "Node48 index count disagrees with header");
        }
        for (std::size_t slot = 0; slot < 48; ++slot) {
            if (slot < record.child_count) {
                if (!referenced[slot] || record.children[slot] == 0) {
                    RaiseArtBump(code, "Node48 live child is not indexed");
                }
            } else if (referenced[slot] || record.children[slot] != 0) {
                RaiseArtBump(code, "Node48 unused child is nonzero");
            }
        }
        for (std::size_t slot = 48; slot < record.children.size(); ++slot) {
            if (record.children[slot] != 0) {
                RaiseArtBump(code, "Node48 out-of-class child is nonzero");
            }
        }
        break;
    }
    case ArtBumpNodeKind::Node256: {
        require_zero_keys();
        require_default_index();
        std::size_t actual = 0;
        for (const auto child : record.children) {
            if (child != 0) ++actual;
        }
        if (actual != record.child_count) {
            RaiseArtBump(code, "Node256 child count disagrees with payload");
        }
        break;
    }
    }
}

} // namespace

ArtBumpNodeRecord::ArtBumpNodeRecord() noexcept {
    index.fill(UINT8_MAX);
}

std::size_t ArtBumpNodeCodec::PayloadBytes(ArtBumpNodeKind kind) {
    return kPayloadBytes[KindIndex(kind, AllocatorErrorCode::InvalidArgument)];
}

std::vector<std::uint8_t> ArtBumpNodeCodec::Encode(
    const ArtBumpNodeRecord& record) {
    std::vector<std::uint8_t> encoded(PayloadBytes(record.kind));
    static_cast<void>(EncodeTo(record, encoded.data(), encoded.size()));
    return encoded;
}

std::size_t ArtBumpNodeCodec::EncodeTo(
    const ArtBumpNodeRecord& record,
    void* output,
    std::size_t output_bytes) {
    ValidateCanonicalNode(record, AllocatorErrorCode::InvalidArgument);
    const auto payload_bytes = PayloadBytes(record.kind);
    if (output == nullptr || output_bytes < payload_bytes) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "node encode buffer is too small");
    }
    auto* encoded = static_cast<std::uint8_t*>(output);
    std::fill(encoded, encoded + payload_bytes, 0);
    Store64(encoded + kNodePrefixOffset, record.prefix_offset);
    Store64(encoded + kNodeValueOffset, record.value_offset);
    Store32(encoded + kNodePrefixLengthOffset, record.prefix_len);
    Store16(encoded + kNodeChildCountOffset, record.child_count);
    encoded[kNodeKindOffset] = static_cast<std::uint8_t>(record.kind);
    encoded[kNodeFlagsOffset] = record.has_value ? kHasValueFlag : 0;

    switch (record.kind) {
    case ArtBumpNodeKind::Node4:
        std::copy_n(record.keys.begin(), 4, encoded + kNodeBodyOffset);
        for (std::size_t slot = 0; slot < 4; ++slot) {
            Store64(encoded + 32U + slot * 8U, record.children[slot]);
        }
        break;
    case ArtBumpNodeKind::Node16:
        std::copy(record.keys.begin(), record.keys.end(), encoded + 24U);
        for (std::size_t slot = 0; slot < 16; ++slot) {
            Store64(encoded + 40U + slot * 8U, record.children[slot]);
        }
        break;
    case ArtBumpNodeKind::Node48:
        std::copy(record.index.begin(), record.index.end(), encoded + 24U);
        for (std::size_t slot = 0; slot < 48; ++slot) {
            Store64(encoded + 280U + slot * 8U, record.children[slot]);
        }
        break;
    case ArtBumpNodeKind::Node256:
        for (std::size_t slot = 0; slot < 256; ++slot) {
            Store64(encoded + 24U + slot * 8U, record.children[slot]);
        }
        break;
    }
    return payload_bytes;
}

ArtBumpNodeRecord ArtBumpNodeCodec::Decode(
    const void* encoded,
    std::size_t encoded_bytes) {
    if (encoded == nullptr ||
        std::find(kPayloadBytes.begin(), kPayloadBytes.end(), encoded_bytes) ==
            kPayloadBytes.end()) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "node input does not have an exact ABI-4 payload size");
    }
    const auto* bytes = static_cast<const std::uint8_t*>(encoded);
    const auto kind = DecodeKind(
        bytes[kNodeKindOffset], AllocatorErrorCode::Corrupt);
    if (kPayloadBytes[KindIndex(kind, AllocatorErrorCode::Corrupt)] !=
        encoded_bytes) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "node kind disagrees with its slab payload size");
    }
    if ((bytes[kNodeFlagsOffset] &
         static_cast<std::uint8_t>(~kHasValueFlag)) != 0) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "node has unknown flag bits");
    }

    ArtBumpNodeRecord result;
    result.prefix_offset = Load64(bytes + kNodePrefixOffset);
    result.value_offset = Load64(bytes + kNodeValueOffset);
    result.prefix_len = Load32(bytes + kNodePrefixLengthOffset);
    result.child_count = Load16(bytes + kNodeChildCountOffset);
    result.kind = kind;
    result.has_value = (bytes[kNodeFlagsOffset] & kHasValueFlag) != 0;
    switch (kind) {
    case ArtBumpNodeKind::Node4:
        std::copy_n(bytes + 24U, 4, result.keys.begin());
        if (!AllBytes(bytes + 28U, bytes + 32U, 0)) {
            RaiseArtBump(
                AllocatorErrorCode::Corrupt,
                "Node4 padding is nonzero");
        }
        for (std::size_t slot = 0; slot < 4; ++slot) {
            result.children[slot] = Load64(bytes + 32U + slot * 8U);
        }
        break;
    case ArtBumpNodeKind::Node16:
        std::copy_n(bytes + 24U, 16, result.keys.begin());
        for (std::size_t slot = 0; slot < 16; ++slot) {
            result.children[slot] = Load64(bytes + 40U + slot * 8U);
        }
        break;
    case ArtBumpNodeKind::Node48:
        std::copy_n(bytes + 24U, 256, result.index.begin());
        for (std::size_t slot = 0; slot < 48; ++slot) {
            result.children[slot] = Load64(bytes + 280U + slot * 8U);
        }
        break;
    case ArtBumpNodeKind::Node256:
        for (std::size_t slot = 0; slot < 256; ++slot) {
            result.children[slot] = Load64(bytes + 24U + slot * 8U);
        }
        break;
    }
    ValidateCanonicalNode(result, AllocatorErrorCode::Corrupt);
    return result;
}

namespace {

void PreflightNodeStoreRegions(
    void* region_base,
    std::size_t region_bytes,
    const std::array<ArtBumpSlabDescriptor, 4>& descriptors,
    std::uint32_t node_capacity) {
    if (region_base == nullptr || region_bytes == 0 || node_capacity == 0) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "invalid ArtBump node-store region");
    }
    const auto region_bytes64 = static_cast<std::uint64_t>(region_bytes);
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
        ValidateSlabDescriptor(
            descriptors[index],
            static_cast<std::uint32_t>(
                ArtBumpNodeCodec::kPayloadBytes[index]),
            node_capacity,
            region_bytes64,
            AllocatorErrorCode::InvalidArgument);
        if (index != 0) {
            const auto prior_end = CheckedAdd(
                descriptors[index - 1U].zone_offset,
                descriptors[index - 1U].zone_bytes,
                AllocatorErrorCode::InvalidArgument,
                "prior slab end overflows");
            if (descriptors[index].metadata_offset != AlignUp(
                    prior_end, 64,
                    AllocatorErrorCode::InvalidArgument,
                    "slab alignment overflows")) {
                RaiseArtBump(
                    AllocatorErrorCode::InvalidArgument,
                    "node-store slabs are not canonical and consecutive");
            }
        }
    }
}

} // namespace

ArtBumpNodeStore::ArtBumpNodeStore(
    std::uint8_t* region_base,
    std::size_t region_bytes,
    const std::array<ArtBumpSlabDescriptor, 4>& descriptors,
    std::uint32_t node_capacity,
    bool recovery_access) noexcept
    : region_base_(region_base),
      region_bytes_(region_bytes),
      descriptors_(descriptors),
      node_capacity_(node_capacity),
      recovery_access_(recovery_access) {
    for (std::size_t index = 0; index < slabs_.size(); ++index) {
        slabs_[index].metadata = region_base_ +
            static_cast<std::size_t>(descriptors_[index].metadata_offset);
        slabs_[index].zone = region_base_ +
            static_cast<std::size_t>(descriptors_[index].zone_offset);
        slabs_[index].descriptor = descriptors_[index];
        slabs_[index].capacity = node_capacity_;
        slabs_[index].payload_bytes = static_cast<std::uint32_t>(
            ArtBumpNodeCodec::kPayloadBytes[index]);
    }
}

ArtBumpNodeStore::ArtBumpNodeStore(ArtBumpNodeStore&& other) noexcept
    : region_base_(other.region_base_),
      region_bytes_(other.region_bytes_),
      descriptors_(other.descriptors_),
      slabs_(other.slabs_),
      node_capacity_(other.node_capacity_),
      recovery_access_(other.recovery_access_) {
    other.region_base_ = nullptr;
    other.region_bytes_ = 0;
    other.descriptors_ = {};
    other.slabs_ = {};
    other.node_capacity_ = 0;
    other.recovery_access_ = false;
}

ArtBumpNodeStore& ArtBumpNodeStore::operator=(
    ArtBumpNodeStore&& other) noexcept {
    if (this == &other) return *this;
    region_base_ = other.region_base_;
    region_bytes_ = other.region_bytes_;
    descriptors_ = other.descriptors_;
    slabs_ = other.slabs_;
    node_capacity_ = other.node_capacity_;
    recovery_access_ = other.recovery_access_;
    other.region_base_ = nullptr;
    other.region_bytes_ = 0;
    other.descriptors_ = {};
    other.slabs_ = {};
    other.node_capacity_ = 0;
    other.recovery_access_ = false;
    return *this;
}

ArtBumpNodeStore ArtBumpNodeStore::Initialize(
    void* region_base,
    std::size_t region_bytes,
    const std::array<ArtBumpSlabDescriptor, 4>& descriptors,
    std::uint32_t node_capacity) {
    PreflightNodeStoreRegions(
        region_base, region_bytes, descriptors, node_capacity);
    auto* base = static_cast<std::uint8_t*>(region_base);
    for (std::size_t index = 0; index < descriptors.size(); ++index) {
        auto* metadata = base +
            static_cast<std::size_t>(descriptors[index].metadata_offset);
        std::fill(
            metadata,
            metadata + ArtBumpGeometry::kSlabMetadataBytes,
            0);
        std::copy(kSlabMagic.begin(), kSlabMagic.end(), metadata);
        Store32(metadata + 8, kSlabVersion);
        Store32(
            metadata + kSlabPayloadOffset,
            static_cast<std::uint32_t>(
                ArtBumpNodeCodec::kPayloadBytes[index]));
        Store64(
            metadata + kSlabZoneOffsetOffset,
            descriptors[index].zone_offset);
        Store64(
            metadata + kSlabZoneBytesOffset,
            descriptors[index].zone_bytes);
        Store32(metadata + kSlabCapacityOffset, node_capacity);
        Store32(metadata + kSlabHighWaterOffset, 0);
        Store32(metadata + kSlabUsedCountOffset, 0);
        Store64(metadata + kSlabFreeHeadOffset, 0);
        Store64(
            metadata + kSlabHashOffset,
            ArtBumpHeaderCodec::SlabGeometryHash(
                static_cast<std::uint32_t>(
                    ArtBumpNodeCodec::kPayloadBytes[index]),
                descriptors[index].zone_offset,
                descriptors[index].zone_bytes,
                node_capacity));
    }
    ArtBumpNodeStore store(
        base, region_bytes, descriptors, node_capacity, false);
    store.ValidateGeometry(AllocatorErrorCode::InvalidArgument);
    return store;
}

ArtBumpNodeStore ArtBumpNodeStore::Attach(
    void* region_base,
    std::size_t region_bytes,
    const std::array<ArtBumpSlabDescriptor, 4>& descriptors,
    std::uint32_t node_capacity) {
    PreflightNodeStoreRegions(
        region_base, region_bytes, descriptors, node_capacity);
    ArtBumpNodeStore store(
        static_cast<std::uint8_t*>(region_base),
        region_bytes,
        descriptors,
        node_capacity,
        false);
    store.Validate();
    return store;
}

ArtBumpNodeStore ArtBumpNodeStore::AttachForRecovery(
    void* region_base,
    std::size_t region_bytes,
    const std::array<ArtBumpSlabDescriptor, 4>& descriptors,
    std::uint32_t node_capacity) {
    PreflightNodeStoreRegions(
        region_base, region_bytes, descriptors, node_capacity);
    ArtBumpNodeStore store(
        static_cast<std::uint8_t*>(region_base),
        region_bytes,
        descriptors,
        node_capacity,
        true);
    store.ValidateGeometry(AllocatorErrorCode::Corrupt);
    return store;
}

ArtBumpNodeStore::Slab& ArtBumpNodeStore::SlabFor(
    ArtBumpNodeKind kind) {
    return slabs_[KindIndex(kind, AllocatorErrorCode::InvalidArgument)];
}

const ArtBumpNodeStore::Slab& ArtBumpNodeStore::SlabFor(
    ArtBumpNodeKind kind) const {
    return slabs_[KindIndex(kind, AllocatorErrorCode::InvalidArgument)];
}

void ArtBumpNodeStore::ValidateGeometry(AllocatorErrorCode code) const {
    if (region_base_ == nullptr || region_bytes_ == 0 ||
        node_capacity_ == 0) {
        RaiseArtBump(code, "node-store handle is empty or moved from");
    }
    for (std::size_t index = 0; index < slabs_.size(); ++index) {
        const auto& slab = slabs_[index];
        ValidateSlabDescriptor(
            slab.descriptor,
            slab.payload_bytes,
            slab.capacity,
            static_cast<std::uint64_t>(region_bytes_),
            code);
        const auto* metadata = slab.metadata;
        if (!std::equal(kSlabMagic.begin(), kSlabMagic.end(), metadata) ||
            Load32(metadata + 8) != kSlabVersion ||
            Load32(metadata + kSlabPayloadOffset) != slab.payload_bytes ||
            Load64(metadata + kSlabZoneOffsetOffset) !=
                slab.descriptor.zone_offset ||
            Load64(metadata + kSlabZoneBytesOffset) !=
                slab.descriptor.zone_bytes ||
            Load32(metadata + kSlabCapacityOffset) != slab.capacity ||
            Load32(metadata + kSlabReservedOffset) != 0 ||
            Load64(metadata + kSlabHashOffset) !=
                ArtBumpHeaderCodec::SlabGeometryHash(
                    slab.payload_bytes,
                    slab.descriptor.zone_offset,
                    slab.descriptor.zone_bytes,
                    slab.capacity)) {
            RaiseArtBump(code, "headerless slab immutable geometry mismatch");
        }
    }
}

ArtBumpNodeKind ArtBumpNodeStore::ReferenceKind(
    std::uint64_t reference) const {
    if (reference == 0) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidId,
            "empty node reference has no target");
    }
    for (std::size_t index = 0; index < slabs_.size(); ++index) {
        const auto& slab = slabs_[index];
        const auto begin = slab.descriptor.zone_offset;
        const auto end = begin + slab.descriptor.zone_bytes;
        if (reference < begin || reference >= end) continue;
        const auto delta = reference - begin;
        if (delta % slab.payload_bytes != 0 ||
            delta / slab.payload_bytes >= slab.capacity) {
            RaiseArtBump(
                AllocatorErrorCode::InvalidId,
                "node reference is not an exact slab-slot start");
        }
        return static_cast<ArtBumpNodeKind>(index + 1U);
    }
    RaiseArtBump(
        AllocatorErrorCode::InvalidId,
        "node reference lies outside every slab");
}

std::uint32_t ArtBumpNodeStore::SlotIndex(
    std::uint64_t reference) const {
    const auto kind = ReferenceKind(reference);
    const auto& slab = SlabFor(kind);
    return static_cast<std::uint32_t>(
        (reference - slab.descriptor.zone_offset) / slab.payload_bytes);
}

bool ArtBumpNodeStore::IsAllocated(std::uint64_t reference) const {
    if (recovery_access_) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "recovery handle has no authoritative allocation metadata");
    }
    const auto kind = ReferenceKind(reference);
    const auto& slab = SlabFor(kind);
    const auto id = SlotIndex(reference);
    if (id >= Load32(slab.metadata + kSlabHighWaterOffset)) return false;
    const auto* slot = slab.zone +
        static_cast<std::size_t>(id) * slab.payload_bytes;
    if (slot[kNodeKindOffset] == 0) return false;
    const auto record = ArtBumpNodeCodec::Decode(slot, slab.payload_bytes);
    if (record.kind != kind) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "allocated node kind disagrees with its slab");
    }
    return true;
}

void ArtBumpNodeStore::ValidateReferences(
    const ArtBumpNodeRecord& record,
    bool require_allocated,
    AllocatorErrorCode code) const {
    ValidateCanonicalNode(record, code);
    for (const auto child : record.children) {
        if (child == 0) continue;
        try {
            static_cast<void>(ReferenceKind(child));
            if (require_allocated && !IsAllocated(child)) {
                RaiseArtBump(code, "node child refers to a free slab slot");
            }
        } catch (const AllocatorError&) {
            RaiseArtBump(code, "node child reference is outside slab geometry");
        }
    }
}

std::uint64_t ArtBumpNodeStore::Allocate(
    const ArtBumpNodeRecord& record) {
    if (recovery_access_) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "cannot allocate through a recovery handle");
    }
    std::array<std::uint8_t, ArtBumpNodeCodec::kMaximumPayloadBytes> encoded{};
    const auto encoded_bytes = ArtBumpNodeCodec::EncodeTo(
        record, encoded.data(), encoded.size());
    ValidateReferences(
        record, true, AllocatorErrorCode::InvalidArgument);
    auto& slab = SlabFor(record.kind);
    auto high_water = Load32(slab.metadata + kSlabHighWaterOffset);
    auto used_count = Load32(slab.metadata + kSlabUsedCountOffset);
    if (high_water > slab.capacity || used_count > high_water) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "slab mutable counters are invalid");
    }
    if (used_count == std::numeric_limits<std::uint32_t>::max()) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "slab used count cannot be incremented");
    }
    std::uint64_t reference = Load64(
        slab.metadata + kSlabFreeHeadOffset);
    std::uint8_t* slot = nullptr;
    if (reference != 0) {
        if (used_count >= high_water) {
            RaiseArtBump(
                AllocatorErrorCode::Corrupt,
                "nonempty slab free list disagrees with used count");
        }
        const auto head_kind = ReferenceKind(reference);
        if (head_kind != record.kind) {
            RaiseArtBump(
                AllocatorErrorCode::Corrupt,
                "slab free head names a different class");
        }
        const auto id = SlotIndex(reference);
        if (id >= high_water) {
            RaiseArtBump(
                AllocatorErrorCode::Corrupt,
                "slab free head is above high water");
        }
        slot = slab.zone + static_cast<std::size_t>(id) * slab.payload_bytes;
        if (!AllBytes(slot + 8U, slot + slab.payload_bytes, 0)) {
            RaiseArtBump(
                AllocatorErrorCode::Corrupt,
                "free slab slot is not canonical");
        }
        const auto next = Load64(slot);
        if (next != 0) {
            try {
                if (ReferenceKind(next) != record.kind ||
                    SlotIndex(next) >= high_water || next == reference) {
                    RaiseArtBump(
                        AllocatorErrorCode::Corrupt,
                        "slab free-head link is not another free-class slot");
                }
            } catch (const AllocatorError&) {
                RaiseArtBump(
                    AllocatorErrorCode::Corrupt,
                    "slab free-head link is invalid");
            }
        }
        std::memcpy(slot, encoded.data(), encoded_bytes);
        Store64(slab.metadata + kSlabFreeHeadOffset, next);
    } else {
        if (used_count != high_water) {
            RaiseArtBump(
                AllocatorErrorCode::Corrupt,
                "empty slab free list omits free slots below high water");
        }
        if (high_water == slab.capacity) {
            RaiseArtBump(
                AllocatorErrorCode::Capacity,
                "ArtBump node slab is full");
        }
        reference = slab.descriptor.zone_offset +
            static_cast<std::uint64_t>(high_water) * slab.payload_bytes;
        slot = slab.zone +
            static_cast<std::size_t>(high_water) * slab.payload_bytes;
        std::memcpy(slot, encoded.data(), encoded_bytes);
        ++high_water;
        Store32(slab.metadata + kSlabHighWaterOffset, high_water);
    }
    Store32(slab.metadata + kSlabUsedCountOffset, used_count + 1U);
    return reference;
}

std::uint64_t ArtBumpNodeStore::Clone(
    std::uint64_t source_reference) {
    return Allocate(Read(source_reference));
}

void ArtBumpNodeStore::DiscardUnpublished(std::uint64_t reference) {
    ReclaimPublished(reference);
}

void ArtBumpNodeStore::ReclaimPublished(std::uint64_t reference) {
    if (recovery_access_) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "cannot free through a recovery handle");
    }
    const auto kind = ReferenceKind(reference);
    auto& slab = SlabFor(kind);
    const auto id = SlotIndex(reference);
    const auto high_water = Load32(slab.metadata + kSlabHighWaterOffset);
    auto used_count = Load32(slab.metadata + kSlabUsedCountOffset);
    if (id >= high_water || used_count == 0 || !IsAllocated(reference)) {
        RaiseArtBump(
            AllocatorErrorCode::NotAllocated,
            "node reference is not allocated");
    }
    auto* slot = slab.zone +
        static_cast<std::size_t>(id) * slab.payload_bytes;
    const auto old_head = Load64(slab.metadata + kSlabFreeHeadOffset);
    std::fill(slot, slot + slab.payload_bytes, 0);
    Store64(slot, old_head);
    Store64(slab.metadata + kSlabFreeHeadOffset, reference);
    Store32(slab.metadata + kSlabUsedCountOffset, used_count - 1U);
}

ArtBumpNodeRecord ArtBumpNodeStore::Read(
    std::uint64_t reference) const {
    if (recovery_access_) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "ordinary read requires a normal node-store handle");
    }
    const auto kind = ReferenceKind(reference);
    const auto& slab = SlabFor(kind);
    const auto id = SlotIndex(reference);
    if (!IsAllocated(reference)) {
        RaiseArtBump(
            AllocatorErrorCode::NotAllocated,
            "node reference names a free or never-used slot");
    }
    const auto* slot = slab.zone +
        static_cast<std::size_t>(id) * slab.payload_bytes;
    auto record = ArtBumpNodeCodec::Decode(slot, slab.payload_bytes);
    if (record.kind != kind) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "node kind disagrees with its slab");
    }
    ValidateReferences(record, true, AllocatorErrorCode::Corrupt);
    return record;
}

ArtBumpNodeRecord ArtBumpNodeStore::ReadForRecovery(
    std::uint64_t reference) const {
    if (!recovery_access_) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "recovery read requires a recovery node-store handle");
    }
    const auto kind = ReferenceKind(reference);
    const auto& slab = SlabFor(kind);
    const auto id = SlotIndex(reference);
    const auto* slot = slab.zone +
        static_cast<std::size_t>(id) * slab.payload_bytes;
    auto record = ArtBumpNodeCodec::Decode(slot, slab.payload_bytes);
    if (record.kind != kind) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "recovery node kind disagrees with its slab");
    }
    ValidateReferences(record, false, AllocatorErrorCode::Corrupt);
    return record;
}

void ArtBumpNodeStore::Validate() const {
    if (recovery_access_) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "clean validation requires a normal node-store handle");
    }
    ValidateGeometry(AllocatorErrorCode::Corrupt);
    ArtBumpNodeLiveBitmaps live;
    ArtBumpNodeLiveBitCounts bit_counts{};
    for (std::size_t index = 0; index < slabs_.size(); ++index) {
        const auto& slab = slabs_[index];
        const auto high_water = Load32(
            slab.metadata + kSlabHighWaterOffset);
        const auto used_count = Load32(
            slab.metadata + kSlabUsedCountOffset);
        if (high_water > slab.capacity || used_count > high_water) {
            RaiseArtBump(
                AllocatorErrorCode::Corrupt,
                "slab mutable counters exceed capacity");
        }
        bit_counts[index] = high_water;
        live[index].resize(
            (static_cast<std::size_t>(high_water) + 7U) / 8U,
            0);
        std::vector<std::uint8_t> free_seen(
            live[index].size(), 0);
        std::uint32_t free_count = 0;
        auto free_reference = Load64(
            slab.metadata + kSlabFreeHeadOffset);
        while (free_reference != 0) {
            ArtBumpNodeKind free_kind;
            std::uint32_t id;
            try {
                free_kind = ReferenceKind(free_reference);
                id = SlotIndex(free_reference);
            } catch (const AllocatorError&) {
                RaiseArtBump(
                    AllocatorErrorCode::Corrupt,
                    "slab free list contains an invalid reference");
            }
            if (KindIndex(free_kind, AllocatorErrorCode::Corrupt) != index ||
                id >= high_water ||
                BitmapBit(free_seen, high_water, id)) {
                RaiseArtBump(
                    AllocatorErrorCode::Corrupt,
                    "slab free list has wrong-class, high, or duplicate slot");
            }
            const auto* slot = slab.zone +
                static_cast<std::size_t>(id) * slab.payload_bytes;
            if (!AllBytes(slot + 8U, slot + slab.payload_bytes, 0)) {
                RaiseArtBump(
                    AllocatorErrorCode::Corrupt,
                    "free slab slot contains nonzero node bytes");
            }
            SetBitmapBit(&free_seen, id);
            ++free_count;
            free_reference = Load64(slot);
        }
        if (free_count != high_water - used_count) {
            RaiseArtBump(
                AllocatorErrorCode::Corrupt,
                "slab free-list count disagrees with used count");
        }
        std::uint32_t actual_used = 0;
        for (std::uint32_t id = 0; id < high_water; ++id) {
            if (BitmapBit(free_seen, high_water, id)) continue;
            const auto* slot = slab.zone +
                static_cast<std::size_t>(id) * slab.payload_bytes;
            const auto record = ArtBumpNodeCodec::Decode(
                slot, slab.payload_bytes);
            if (KindIndex(record.kind, AllocatorErrorCode::Corrupt) != index) {
                RaiseArtBump(
                    AllocatorErrorCode::Corrupt,
                    "allocated slot kind disagrees with slab");
            }
            SetBitmapBit(&live[index], id);
            ++actual_used;
        }
        if (actual_used != used_count) {
            RaiseArtBump(
                AllocatorErrorCode::Corrupt,
                "slab allocated-slot count disagrees with metadata");
        }
    }

    for (std::size_t index = 0; index < slabs_.size(); ++index) {
        const auto& slab = slabs_[index];
        for (std::uint32_t id = 0;
             id < static_cast<std::uint32_t>(bit_counts[index]);
             ++id) {
            if (!BitmapBit(live[index], bit_counts[index], id)) continue;
            const auto* slot = slab.zone +
                static_cast<std::size_t>(id) * slab.payload_bytes;
            const auto record = ArtBumpNodeCodec::Decode(
                slot, slab.payload_bytes);
            for (const auto child : record.children) {
                if (child == 0) continue;
                ArtBumpNodeKind child_kind;
                std::uint32_t child_id;
                try {
                    child_kind = ReferenceKind(child);
                    child_id = SlotIndex(child);
                } catch (const AllocatorError&) {
                    RaiseArtBump(
                        AllocatorErrorCode::Corrupt,
                        "allocated node has invalid child reference");
                }
                const auto child_index = KindIndex(
                    child_kind, AllocatorErrorCode::Corrupt);
                if (!BitmapBit(
                        live[child_index],
                        bit_counts[child_index],
                        child_id)) {
                    RaiseArtBump(
                        AllocatorErrorCode::Corrupt,
                        "allocated node refers to a free child slot");
                }
            }
        }
    }
}

std::uint32_t ArtBumpNodeStore::Capacity(
    ArtBumpNodeKind kind) const {
    return SlabFor(kind).capacity;
}

std::uint32_t ArtBumpNodeStore::HighWater(
    ArtBumpNodeKind kind) const {
    if (recovery_access_) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "recovery handle cannot trust slab high water");
    }
    return Load32(SlabFor(kind).metadata + kSlabHighWaterOffset);
}

std::uint32_t ArtBumpNodeStore::UsedCount(
    ArtBumpNodeKind kind) const {
    if (recovery_access_) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "recovery handle cannot trust slab used count");
    }
    return Load32(SlabFor(kind).metadata + kSlabUsedCountOffset);
}

std::uint64_t ArtBumpNodeStore::FreeHead(
    ArtBumpNodeKind kind) const {
    if (recovery_access_) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "recovery handle cannot trust slab free head");
    }
    return Load64(SlabFor(kind).metadata + kSlabFreeHeadOffset);
}

ArtBumpNodeStore::PreparedRebuild ArtBumpNodeStore::PrepareRebuild(
    ArtBumpNodeLiveBitmaps live_bitmaps,
    ArtBumpNodeLiveBitCounts live_bit_counts) && {
    ValidateGeometry(AllocatorErrorCode::Corrupt);
    std::array<std::uint32_t, 4> rebuilt_high_water{};
    std::array<std::uint32_t, 4> rebuilt_used_count{};
    for (std::size_t index = 0; index < live_bitmaps.size(); ++index) {
        ValidateBitmap(
            live_bitmaps[index], live_bit_counts[index], node_capacity_);
        for (std::uint32_t id = 0;
             id < static_cast<std::uint32_t>(live_bit_counts[index]);
             ++id) {
            if (!BitmapBit(
                    live_bitmaps[index], live_bit_counts[index], id)) {
                continue;
            }
            rebuilt_high_water[index] = id + 1U;
            if (rebuilt_used_count[index] ==
                std::numeric_limits<std::uint32_t>::max()) {
                RaiseArtBump(
                    AllocatorErrorCode::InvalidArgument,
                    "rebuild live count overflows uint32");
            }
            ++rebuilt_used_count[index];
            const auto& slab = slabs_[index];
            const auto* slot = slab.zone +
                static_cast<std::size_t>(id) * slab.payload_bytes;
            const auto record = ArtBumpNodeCodec::Decode(
                slot, slab.payload_bytes);
            if (KindIndex(
                    record.kind, AllocatorErrorCode::Corrupt) != index) {
                RaiseArtBump(
                    AllocatorErrorCode::Corrupt,
                    "rebuild live node kind disagrees with slab");
            }
        }
    }

    // The supplied live set is the complete authoritative node set. Validate
    // every reference against that same set before the first rebuild write.
    for (std::size_t index = 0; index < live_bitmaps.size(); ++index) {
        const auto& slab = slabs_[index];
        for (std::uint32_t id = 0;
             id < rebuilt_high_water[index];
             ++id) {
            if (!BitmapBit(
                    live_bitmaps[index], live_bit_counts[index], id)) {
                continue;
            }
            const auto* slot = slab.zone +
                static_cast<std::size_t>(id) * slab.payload_bytes;
            const auto record = ArtBumpNodeCodec::Decode(
                slot, slab.payload_bytes);
            for (const auto child : record.children) {
                if (child == 0) continue;
                ArtBumpNodeKind child_kind;
                std::uint32_t child_id;
                try {
                    child_kind = ReferenceKind(child);
                    child_id = SlotIndex(child);
                } catch (const AllocatorError&) {
                    RaiseArtBump(
                        AllocatorErrorCode::Corrupt,
                        "rebuild node has invalid child reference");
                }
                const auto child_index = KindIndex(
                    child_kind, AllocatorErrorCode::Corrupt);
                if (!BitmapBit(
                        live_bitmaps[child_index],
                        live_bit_counts[child_index],
                        child_id)) {
                    RaiseArtBump(
                        AllocatorErrorCode::Corrupt,
                        "rebuild live set omits a referenced child");
                }
            }
        }
    }
    return PreparedRebuild(
        std::move(*this),
        std::move(live_bitmaps),
        live_bit_counts,
        rebuilt_high_water,
        rebuilt_used_count);
}

ArtBumpNodeStore ArtBumpNodeStore::Rebuild(
    ArtBumpNodeLiveBitmaps live_bitmaps,
    ArtBumpNodeLiveBitCounts live_bit_counts) && {
    auto prepared = std::move(*this).PrepareRebuild(
        std::move(live_bitmaps), live_bit_counts);
    return std::move(prepared).Apply();
}

ArtBumpNodeStore::PreparedRebuild::PreparedRebuild(
    ArtBumpNodeStore store,
    ArtBumpNodeLiveBitmaps live_bitmaps,
    ArtBumpNodeLiveBitCounts live_bit_counts,
    std::array<std::uint32_t, 4> rebuilt_high_water,
    std::array<std::uint32_t, 4> rebuilt_used_count) noexcept
    : store_(std::move(store)),
      live_bitmaps_(std::move(live_bitmaps)),
      live_bit_counts_(live_bit_counts),
      rebuilt_high_water_(rebuilt_high_water),
      rebuilt_used_count_(rebuilt_used_count) {}

ArtBumpNodeStore::PreparedRebuild::PreparedRebuild(
    PreparedRebuild&& other) noexcept
    : store_(std::move(other.store_)),
      live_bitmaps_(std::move(other.live_bitmaps_)),
      live_bit_counts_(other.live_bit_counts_),
      rebuilt_high_water_(other.rebuilt_high_water_),
      rebuilt_used_count_(other.rebuilt_used_count_),
      ready_(other.ready_) {
    other.live_bit_counts_.fill(0);
    other.rebuilt_high_water_.fill(0);
    other.rebuilt_used_count_.fill(0);
    other.ready_ = false;
}

ArtBumpNodeStore::PreparedRebuild&
ArtBumpNodeStore::PreparedRebuild::operator=(
    PreparedRebuild&& other) noexcept {
    if (this == &other) return *this;
    store_ = std::move(other.store_);
    live_bitmaps_ = std::move(other.live_bitmaps_);
    live_bit_counts_ = other.live_bit_counts_;
    rebuilt_high_water_ = other.rebuilt_high_water_;
    rebuilt_used_count_ = other.rebuilt_used_count_;
    ready_ = other.ready_;
    other.live_bit_counts_.fill(0);
    other.rebuilt_high_water_.fill(0);
    other.rebuilt_used_count_.fill(0);
    other.ready_ = false;
    return *this;
}

ArtBumpNodeStore ArtBumpNodeStore::PreparedRebuild::Apply(
    WriteCutHook hook,
    void* context) && {
    if (!ready_) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "prepared slab rebuild was already consumed");
    }
    for (std::size_t index = 0; index < live_bitmaps_.size(); ++index) {
        auto& slab = store_.slabs_[index];
        std::uint64_t write_count = 3;
        for (std::uint32_t id = 0;
             id < rebuilt_high_water_[index];
             ++id) {
            if (!BitmapBit(
                    live_bitmaps_[index], live_bit_counts_[index], id)) {
                ++write_count;
            }
        }
        std::uint64_t write_ordinal = 0;
        const auto notify = [&]() noexcept {
            if (hook != nullptr) {
                if (write_ordinal == 0) {
                    hook(context, index, ArtBumpRebuildCut::First);
                }
                if (write_ordinal == write_count / 2U) {
                    hook(context, index, ArtBumpRebuildCut::Middle);
                }
                if (write_ordinal + 1U == write_count) {
                    hook(context, index, ArtBumpRebuildCut::Final);
                }
            }
            ++write_ordinal;
        };
        std::uint64_t free_head = 0;
        for (std::uint32_t id = 0;
             id < rebuilt_high_water_[index];
             ++id) {
            if (BitmapBit(
                    live_bitmaps_[index], live_bit_counts_[index], id)) {
                continue;
            }
            auto* slot = slab.zone +
                static_cast<std::size_t>(id) * slab.payload_bytes;
            std::fill(slot, slot + slab.payload_bytes, 0);
            Store64(slot, free_head);
            free_head = slab.descriptor.zone_offset +
                static_cast<std::uint64_t>(id) * slab.payload_bytes;
            notify();
        }
        Store32(
            slab.metadata + kSlabHighWaterOffset,
            rebuilt_high_water_[index]);
        notify();
        Store32(
            slab.metadata + kSlabUsedCountOffset,
            rebuilt_used_count_[index]);
        notify();
        Store64(slab.metadata + kSlabFreeHeadOffset, free_head);
        notify();
    }
    store_.recovery_access_ = false;
    ready_ = false;
    return std::move(store_);
}

ArtBumpNodeStore ArtBumpNodeStore::PreparedRebuild::Cancel() && noexcept {
    ready_ = false;
    return std::move(store_);
}

ArtBumpRawZone::ArtBumpRawZone(
    std::uint8_t* region_base,
    std::uint8_t* descriptor) noexcept
    : region_base_(region_base),
      descriptor_(descriptor) {}

ArtBumpRawZone ArtBumpRawZone::Attach(
    void* region_base,
    std::size_t region_bytes,
    void* encoded_zone_descriptor,
    std::size_t descriptor_bytes,
    std::uint64_t expected_begin,
    std::uint64_t expected_bytes) {
    if (region_base == nullptr || region_bytes == 0 ||
        encoded_zone_descriptor == nullptr || descriptor_bytes != 32 ||
        reinterpret_cast<std::uintptr_t>(encoded_zone_descriptor) % 8U != 0) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "invalid or unaligned raw-zone view");
    }
    StartImplicitLifetimes(encoded_zone_descriptor, descriptor_bytes);
    auto* descriptor = static_cast<std::uint8_t*>(encoded_zone_descriptor);
    const auto begin = Load64(descriptor);
    const auto bytes = Load64(descriptor + 8U);
    const auto top = Acquire64(descriptor + 16U);
    const auto epoch = Load32(descriptor + 24U);
    if (Load32(descriptor + 28U) != 0 ||
        begin != expected_begin || bytes != expected_bytes || bytes == 0 ||
        epoch == 0) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "raw-zone descriptor mismatch");
    }
    const auto end = CheckedAdd(
        begin, bytes, AllocatorErrorCode::Corrupt,
        "raw-zone end overflows");
    if (end > static_cast<std::uint64_t>(region_bytes) ||
        top < begin || top > end) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "raw-zone range lies outside the mapped region");
    }
    return ArtBumpRawZone(
        static_cast<std::uint8_t*>(region_base),
        descriptor);
}

ArtBumpRawZone ArtBumpRawZone::AttachForRecovery(
    void* region_base,
    std::size_t region_bytes,
    void* encoded_zone_descriptor,
    std::size_t descriptor_bytes,
    std::uint64_t expected_begin,
    std::uint64_t expected_bytes) {
    if (region_base == nullptr || region_bytes == 0 ||
        encoded_zone_descriptor == nullptr || descriptor_bytes != 32 ||
        reinterpret_cast<std::uintptr_t>(encoded_zone_descriptor) % 8U != 0) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "invalid or unaligned recovery raw-zone view");
    }
    StartImplicitLifetimes(encoded_zone_descriptor, descriptor_bytes);
    auto* descriptor = static_cast<std::uint8_t*>(encoded_zone_descriptor);
    const auto begin = Load64(descriptor);
    const auto bytes = Load64(descriptor + 8U);
    if (Load32(descriptor + 28U) != 0 ||
        begin != expected_begin || bytes != expected_bytes || bytes == 0) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "recovery raw-zone immutable descriptor mismatch");
    }
    const auto end = CheckedAdd(
        begin, bytes, AllocatorErrorCode::Corrupt,
        "recovery raw-zone end overflows");
    if (end > static_cast<std::uint64_t>(region_bytes)) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "recovery raw zone lies outside the mapped region");
    }
    return ArtBumpRawZone(
        static_cast<std::uint8_t*>(region_base),
        descriptor);
}

std::uint64_t ArtBumpRawZone::Begin() const noexcept {
    return Load64(descriptor_);
}

std::uint64_t ArtBumpRawZone::Bytes() const noexcept {
    return Load64(descriptor_ + 8U);
}

std::uint64_t ArtBumpRawZone::TopAcquire() const noexcept {
    return Acquire64(descriptor_ + 16U);
}

std::uint32_t ArtBumpRawZone::Epoch() const noexcept {
    return Load32(descriptor_ + 24U);
}

std::uint64_t ArtBumpRawZone::End() const {
    return CheckedAdd(
        Begin(), Bytes(), AllocatorErrorCode::Corrupt,
        "raw-zone end overflows");
}

std::uint64_t ArtBumpRawZone::Remaining() const {
    const auto top = TopAcquire();
    const auto end = End();
    if (top < Begin() || top > end) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "raw-zone top is outside its range");
    }
    return end - top;
}

std::uint64_t ArtBumpRawZone::Allocate(
    const void* bytes,
    std::size_t length) {
    if (bytes == nullptr || length == 0) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "raw allocation must be nonempty");
    }
    const auto top = TopAcquire();
    const auto length64 = static_cast<std::uint64_t>(length);
    const auto advanced = CheckedAdd(
        top, length64, AllocatorErrorCode::Capacity,
        "raw allocation end overflows");
    if (top < Begin() || advanced > End()) {
        RaiseArtBump(
            AllocatorErrorCode::Capacity,
            "raw zone has insufficient capacity");
    }
    std::memmove(
        region_base_ + static_cast<std::size_t>(top),
        bytes,
        length);
    StoreTopRelease(advanced);
    return top;
}

void ArtBumpRawZone::ValidateInterval(
    std::uint64_t offset,
    std::size_t length,
    std::uint64_t recorded_top) const {
    const auto begin = Begin();
    const auto zone_end = End();
    if (recorded_top < begin || recorded_top > zone_end || length == 0 ||
        offset < begin || offset >= recorded_top) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "raw interval start or recorded top is out of range");
    }
    const auto interval_end = CheckedAdd(
        offset,
        static_cast<std::uint64_t>(length),
        AllocatorErrorCode::Corrupt,
        "raw interval end overflows");
    if (interval_end > recorded_top) {
        RaiseArtBump(
            AllocatorErrorCode::Corrupt,
            "raw interval ends above the recorded top");
    }
}

const std::uint8_t* ArtBumpRawZone::ReadBounded(
    std::uint64_t offset,
    std::size_t length,
    std::uint64_t recorded_top) const {
    ValidateInterval(offset, length, recorded_top);
    return region_base_ + static_cast<std::size_t>(offset);
}

std::uint8_t* ArtBumpRawZone::MutableBounded(
    std::uint64_t offset,
    std::size_t length,
    std::uint64_t recorded_top) const {
    ValidateInterval(offset, length, recorded_top);
    return region_base_ + static_cast<std::size_t>(offset);
}

void ArtBumpRawZone::StoreTopRelease(std::uint64_t top) const noexcept {
    Release64(descriptor_ + 16U, top);
}

void ArtBumpRawZone::StoreEpoch(std::uint32_t epoch) const noexcept {
    Store32(descriptor_ + 24U, epoch);
}

ArtBumpRawZone::PreparedState ArtBumpRawZone::PrepareState(
    std::uint64_t top,
    std::uint32_t epoch) const {
    const auto begin = Begin();
    const auto end = End();
    if (epoch == 0 || top < begin || top > end) {
        RaiseArtBump(
            AllocatorErrorCode::InvalidArgument,
            "prepared raw-zone state is outside canonical range");
    }
    return PreparedState(*this, top, epoch);
}

void ArtBumpRawZone::PreparedState::Apply() const noexcept {
    zone_.StoreEpoch(epoch_);
    zone_.StoreTopRelease(top_);
}

} // namespace kvspace::detail
