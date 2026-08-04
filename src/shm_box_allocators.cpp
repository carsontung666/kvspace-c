#include "shm_box_allocators.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>
#include <utility>

// The allocation geometry was independently implemented from the public
// algorithm descriptions at these source snapshots:
//   blockmalloc 6bb8e16d2a9a5f755ad4ae2e711405feacc452ba
//   https://github.com/array2d/blockmalloc/tree/6bb8e16d2a9a5f755ad4ae2e711405feacc452ba
//   boxmalloc 49c343477c11244fc0a8e53f6f0e3da12def7581
//   https://github.com/array2d/boxmalloc/tree/49c343477c11244fc0a8e53f6f0e3da12def7581
// No source code or persistent C struct layout is copied. This file defines a
// KVSpace-owned, versioned byte format designed for robust-mutex recovery.

namespace kvspace::detail {
namespace {

constexpr std::array<std::uint8_t, 8> kBlockMagic = {
    'K', 'V', 'B', 'L', 'O', 'C', 'K', '1'};
constexpr std::array<std::uint8_t, 8> kBoxMagic = {
    'K', 'V', 'B', 'O', 'X', 'A', '0', '1'};
constexpr std::uint32_t kBlockVersion = 1;
constexpr std::uint32_t kBoxVersion = 1;

constexpr std::size_t kBlockVersionOffset = 8;
constexpr std::size_t kBlockMetadataBytesOffset = 12;
constexpr std::size_t kBlockZoneBytesOffset = 16;
constexpr std::size_t kBlockPayloadBytesOffset = 24;
constexpr std::size_t kBlockCapacityOffset = 32;
constexpr std::size_t kBlockHighWaterOffset = 36;
constexpr std::size_t kBlockUsedCountOffset = 40;
constexpr std::size_t kBlockFreeHeadOffset = 44;
constexpr std::size_t kBlockImmutableHashOffset = 48;
constexpr std::size_t kBlockHeaderWidthOffset = 56;
constexpr std::size_t kBlockReservedOffset = 57;

constexpr std::size_t kBoxVersionOffset = 8;
constexpr std::size_t kBoxHeaderBytesOffset = 12;
constexpr std::size_t kBoxMetadataBytesOffset = 16;
constexpr std::size_t kBoxDataBytesOffset = 24;
constexpr std::size_t kBoxBlockMetadataOffsetOffset = 32;
constexpr std::size_t kBoxBlockZoneOffsetOffset = 40;
constexpr std::size_t kBoxBlockZoneBytesOffset = 48;
constexpr std::size_t kBoxNodeBytesOffset = 56;
constexpr std::size_t kBoxRootIdOffset = 60;
constexpr std::size_t kBoxRootLevelOffset = 64;
constexpr std::size_t kBoxRootSlotsOffset = 65;
constexpr std::size_t kBoxReservedAOffset = 66;
constexpr std::size_t kBoxImmutableHashOffset = 72;
constexpr std::size_t kBoxReservedBOffset = 80;

constexpr std::size_t kNodeParentOffset = 0;
constexpr std::size_t kNodeParentSlotOffset = 4;
constexpr std::size_t kNodeLevelOffset = 5;
constexpr std::size_t kNodeSlotCountOffset = 6;
constexpr std::size_t kNodeReservedOffset = 7;
constexpr std::size_t kNodeStatesOffset = 8;
constexpr std::size_t kNodeChildrenOffset = 24;

constexpr std::size_t kBoxBlockMetadataOffset =
    BoxAllocator::kPersistentHeaderBytes;
constexpr std::size_t kBoxBlockZoneOffset =
    kBoxBlockMetadataOffset + FixedBlockAllocator::kPersistentMetadataBytes;

[[noreturn]] void Raise(
    AllocatorErrorCode code,
    const std::string& message) {
    throw AllocatorError(code, "kvspace allocator: " + message);
}

std::uint16_t Load16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(data[0]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint32_t Load32(const std::uint8_t* data) noexcept {
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        result |= static_cast<std::uint32_t>(data[index]) << shift;
    }
    return result;
}

std::uint64_t Load64(const std::uint8_t* data) noexcept {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        result |= static_cast<std::uint64_t>(data[index]) << shift;
    }
    return result;
}

void Store16(std::uint8_t* data, std::uint16_t value) noexcept {
    for (std::size_t index = 0; index < 2; ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        data[index] = static_cast<std::uint8_t>(value >> shift);
    }
}

void Store32(std::uint8_t* data, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        data[index] = static_cast<std::uint8_t>(value >> shift);
    }
}

void Store64(std::uint8_t* data, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        data[index] = static_cast<std::uint8_t>(value >> shift);
    }
}

void HashByte(std::uint64_t* hash, std::uint8_t value) noexcept {
    *hash ^= value;
    *hash *= 1099511628211ULL;
}

void Hash32(std::uint64_t* hash, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        HashByte(hash, static_cast<std::uint8_t>(value >> shift));
    }
}

void Hash64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        HashByte(hash, static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint64_t BlockImmutableHash(
    std::uint32_t metadata_bytes,
    std::uint64_t block_zone_bytes,
    std::uint64_t payload_bytes,
    std::uint32_t capacity,
    std::uint8_t header_bytes) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto value : kBlockMagic) HashByte(&hash, value);
    Hash32(&hash, kBlockVersion);
    Hash32(&hash, metadata_bytes);
    Hash64(&hash, block_zone_bytes);
    Hash64(&hash, payload_bytes);
    Hash32(&hash, capacity);
    HashByte(&hash, header_bytes);
    return hash;
}

std::uint64_t BoxImmutableHash(
    std::uint64_t metadata_bytes,
    std::uint64_t data_bytes,
    std::uint64_t block_zone_bytes,
    std::uint8_t root_level,
    std::uint8_t root_slots) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto value : kBoxMagic) HashByte(&hash, value);
    Hash32(&hash, kBoxVersion);
    Hash32(
        &hash,
        static_cast<std::uint32_t>(BoxAllocator::kPersistentHeaderBytes));
    Hash64(&hash, metadata_bytes);
    Hash64(&hash, data_bytes);
    Hash64(&hash, static_cast<std::uint64_t>(kBoxBlockMetadataOffset));
    Hash64(&hash, static_cast<std::uint64_t>(kBoxBlockZoneOffset));
    Hash64(&hash, block_zone_bytes);
    Hash32(
        &hash,
        static_cast<std::uint32_t>(BoxAllocator::kNodePayloadBytes));
    Hash32(&hash, 0);
    HashByte(&hash, root_level);
    HashByte(&hash, root_slots);
    return hash;
}

std::uint64_t CheckedAdd(
    std::uint64_t left,
    std::uint64_t right,
    AllocatorErrorCode code,
    const char* message) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        Raise(code, message);
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
        Raise(code, message);
    }
    return left * right;
}

std::uint64_t MaxCapacityForHeader(std::size_t header_bytes) {
    switch (header_bytes) {
    case 2:
        return 1ULL << 14U;
    case 4:
        return 1ULL << 30U;
    case 8:
        return std::numeric_limits<std::uint32_t>::max();
    default:
        Raise(AllocatorErrorCode::InvalidArgument, "invalid block header width");
    }
}

std::size_t SelectHeaderBytes(
    std::size_t block_zone_bytes,
    std::size_t payload_bytes,
    FixedBlockHeaderWidth requested) {
    const auto fits = [&](std::size_t width) {
        if (payload_bytes > std::numeric_limits<std::size_t>::max() - width) {
            return false;
        }
        const auto stride = payload_bytes + width;
        const auto raw_capacity = block_zone_bytes / stride;
        return raw_capacity != 0 &&
            static_cast<std::uint64_t>(raw_capacity) <=
                MaxCapacityForHeader(width);
    };

    if (requested != FixedBlockHeaderWidth::Automatic) {
        const auto width = static_cast<std::size_t>(requested);
        if (width != 2 && width != 4 && width != 8) {
            Raise(
                AllocatorErrorCode::InvalidArgument,
                "invalid requested block header width");
        }
    }

    for (const std::size_t width : {2U, 4U, 8U}) {
        if (!fits(width)) continue;
        if (requested != FixedBlockHeaderWidth::Automatic &&
            static_cast<std::size_t>(requested) != width) {
            Raise(
                AllocatorErrorCode::InvalidArgument,
                "requested block header width is not canonical");
        }
        return width;
    }
    Raise(
        AllocatorErrorCode::InvalidArgument,
        "block zone cannot be represented by a local uint32 ID");
}

bool BitmapBit(
    const std::uint8_t* bitmap,
    std::size_t bit_count,
    std::uint32_t id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= bit_count) return false;
    const auto byte = bitmap[index / 8U];
    const auto shift = static_cast<unsigned>(index % 8U);
    return (byte & static_cast<std::uint8_t>(1U << shift)) != 0;
}

bool GetBit(
    const std::uint8_t* bitmap,
    std::size_t bit_count,
    std::uint32_t id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    if (index >= bit_count) return false;
    const auto shift = static_cast<unsigned>(index % 8U);
    return (bitmap[index / 8U] &
            static_cast<std::uint8_t>(1U << shift)) != 0;
}

void SetBit(std::uint8_t* bitmap, std::uint32_t id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    const auto shift = static_cast<unsigned>(index % 8U);
    bitmap[index / 8U] |= static_cast<std::uint8_t>(1U << shift);
}

} // namespace

AllocatorError::AllocatorError(
    AllocatorErrorCode code,
    const std::string& message)
    : std::runtime_error(message), code_(code) {}

FixedBlockAllocator::FixedBlockAllocator(
    std::uint8_t* metadata,
    std::size_t metadata_bytes,
    std::uint8_t* block_zone,
    std::size_t block_zone_bytes,
    std::size_t payload_bytes,
    std::size_t header_bytes,
    std::uint32_t capacity,
    bool recovery_access) noexcept
    : metadata_(metadata),
      metadata_bytes_(metadata_bytes),
      block_zone_(block_zone),
      block_zone_bytes_(block_zone_bytes),
      payload_bytes_(payload_bytes),
      header_bytes_(header_bytes),
      capacity_(capacity),
      recovery_access_(recovery_access) {}

FixedBlockAllocator::FixedBlockAllocator(
    FixedBlockAllocator&& other) noexcept
    : metadata_(other.metadata_),
      metadata_bytes_(other.metadata_bytes_),
      block_zone_(other.block_zone_),
      block_zone_bytes_(other.block_zone_bytes_),
      payload_bytes_(other.payload_bytes_),
      header_bytes_(other.header_bytes_),
      capacity_(other.capacity_),
      recovery_access_(other.recovery_access_) {
    other.metadata_ = nullptr;
    other.metadata_bytes_ = 0;
    other.block_zone_ = nullptr;
    other.block_zone_bytes_ = 0;
    other.payload_bytes_ = 0;
    other.header_bytes_ = 0;
    other.capacity_ = 0;
    other.recovery_access_ = false;
}

FixedBlockAllocator& FixedBlockAllocator::operator=(
    FixedBlockAllocator&& other) noexcept {
    if (this == &other) return *this;
    metadata_ = other.metadata_;
    metadata_bytes_ = other.metadata_bytes_;
    block_zone_ = other.block_zone_;
    block_zone_bytes_ = other.block_zone_bytes_;
    payload_bytes_ = other.payload_bytes_;
    header_bytes_ = other.header_bytes_;
    capacity_ = other.capacity_;
    recovery_access_ = other.recovery_access_;
    other.metadata_ = nullptr;
    other.metadata_bytes_ = 0;
    other.block_zone_ = nullptr;
    other.block_zone_bytes_ = 0;
    other.payload_bytes_ = 0;
    other.header_bytes_ = 0;
    other.capacity_ = 0;
    other.recovery_access_ = false;
    return *this;
}

FixedBlockAllocator FixedBlockAllocator::Initialize(
    void* metadata,
    std::size_t metadata_bytes,
    void* block_zone,
    std::size_t block_zone_bytes,
    std::size_t payload_bytes,
    FixedBlockHeaderWidth requested_header_width) {
    if (metadata == nullptr || block_zone == nullptr) {
        Raise(AllocatorErrorCode::InvalidArgument, "null block allocator region");
    }
    if (metadata_bytes != kPersistentMetadataBytes) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "invalid block metadata region size");
    }
    if (payload_bytes == 0) {
        Raise(AllocatorErrorCode::InvalidArgument, "zero block payload size");
    }

    const auto header_bytes = SelectHeaderBytes(
        block_zone_bytes, payload_bytes, requested_header_width);
    if (payload_bytes >
        std::numeric_limits<std::size_t>::max() - header_bytes) {
        Raise(AllocatorErrorCode::InvalidArgument, "block stride overflow");
    }
    const auto stride = payload_bytes + header_bytes;
    const auto raw_capacity = block_zone_bytes / stride;
    if (raw_capacity == 0 ||
        static_cast<std::uint64_t>(raw_capacity) >
            std::numeric_limits<std::uint32_t>::max()) {
        Raise(AllocatorErrorCode::InvalidArgument, "invalid block capacity");
    }
    const auto capacity = static_cast<std::uint32_t>(raw_capacity);
    auto* metadata_bytes_ptr = static_cast<std::uint8_t*>(metadata);
    auto* block_bytes_ptr = static_cast<std::uint8_t*>(block_zone);
    std::memset(metadata_bytes_ptr, 0, metadata_bytes);
    std::copy(kBlockMagic.begin(), kBlockMagic.end(), metadata_bytes_ptr);
    Store32(metadata_bytes_ptr + kBlockVersionOffset, kBlockVersion);
    Store32(
        metadata_bytes_ptr + kBlockMetadataBytesOffset,
        static_cast<std::uint32_t>(metadata_bytes));
    Store64(
        metadata_bytes_ptr + kBlockZoneBytesOffset,
        static_cast<std::uint64_t>(block_zone_bytes));
    Store64(
        metadata_bytes_ptr + kBlockPayloadBytesOffset,
        static_cast<std::uint64_t>(payload_bytes));
    Store32(metadata_bytes_ptr + kBlockCapacityOffset, capacity);
    Store32(metadata_bytes_ptr + kBlockHighWaterOffset, 0);
    Store32(metadata_bytes_ptr + kBlockUsedCountOffset, 0);
    Store32(metadata_bytes_ptr + kBlockFreeHeadOffset, kInvalidId);
    metadata_bytes_ptr[kBlockHeaderWidthOffset] =
        static_cast<std::uint8_t>(header_bytes);
    Store64(
        metadata_bytes_ptr + kBlockImmutableHashOffset,
        BlockImmutableHash(
            static_cast<std::uint32_t>(metadata_bytes),
            static_cast<std::uint64_t>(block_zone_bytes),
            static_cast<std::uint64_t>(payload_bytes),
            capacity,
            static_cast<std::uint8_t>(header_bytes)));

    FixedBlockAllocator allocator(
        metadata_bytes_ptr,
        metadata_bytes,
        block_bytes_ptr,
        block_zone_bytes,
        payload_bytes,
        header_bytes,
        capacity,
        false);
    // A fresh allocator has high_water == 0, so validation needs no heap
    // scratch. Keep every possibly allocating operation before persistent
    // initialization writes.
    allocator.ValidateWithScratch(nullptr, 0);
    return allocator;
}

FixedBlockAllocator FixedBlockAllocator::AttachForRecovery(
    void* metadata,
    std::size_t metadata_bytes,
    void* block_zone,
    std::size_t block_zone_bytes) {
    if (metadata == nullptr || block_zone == nullptr ||
        metadata_bytes != kPersistentMetadataBytes) {
        Raise(AllocatorErrorCode::InvalidArgument, "invalid block attach region");
    }
    auto* metadata_bytes_ptr = static_cast<std::uint8_t*>(metadata);
    auto* block_bytes_ptr = static_cast<std::uint8_t*>(block_zone);
    const auto payload_bytes64 =
        Load64(metadata_bytes_ptr + kBlockPayloadBytesOffset);
    if (payload_bytes64 > std::numeric_limits<std::size_t>::max()) {
        Raise(AllocatorErrorCode::Corrupt, "block payload size is not addressable");
    }
    const auto header_bytes = static_cast<std::size_t>(
        metadata_bytes_ptr[kBlockHeaderWidthOffset]);
    const auto capacity = Load32(metadata_bytes_ptr + kBlockCapacityOffset);
    FixedBlockAllocator allocator(
        metadata_bytes_ptr,
        metadata_bytes,
        block_bytes_ptr,
        block_zone_bytes,
        static_cast<std::size_t>(payload_bytes64),
        header_bytes,
        capacity,
        true);
    allocator.ValidateImmutable();
    return allocator;
}

FixedBlockAllocator FixedBlockAllocator::Attach(
    void* metadata,
    std::size_t metadata_bytes,
    void* block_zone,
    std::size_t block_zone_bytes) {
    auto allocator = AttachForRecovery(
        metadata, metadata_bytes, block_zone, block_zone_bytes);
    allocator.recovery_access_ = false;
    allocator.Validate();
    return allocator;
}

FixedBlockHeaderWidth FixedBlockAllocator::MinimumHeaderWidth(
    std::uint64_t capacity) {
    if (capacity == 0 ||
        capacity > std::numeric_limits<std::uint32_t>::max()) {
        Raise(AllocatorErrorCode::InvalidArgument, "invalid local block capacity");
    }
    if (capacity <= (1ULL << 14U)) {
        return FixedBlockHeaderWidth::Bytes2;
    }
    if (capacity <= (1ULL << 30U)) {
        return FixedBlockHeaderWidth::Bytes4;
    }
    return FixedBlockHeaderWidth::Bytes8;
}

void FixedBlockAllocator::ValidateImmutable() const {
    if (metadata_ == nullptr || block_zone_ == nullptr ||
        metadata_bytes_ != kPersistentMetadataBytes) {
        Raise(AllocatorErrorCode::Corrupt, "missing block allocator region");
    }
    if (!std::equal(kBlockMagic.begin(), kBlockMagic.end(), metadata_)) {
        Raise(AllocatorErrorCode::Corrupt, "bad block allocator magic");
    }
    if (Load32(metadata_ + kBlockVersionOffset) != kBlockVersion) {
        Raise(AllocatorErrorCode::Corrupt, "unsupported block allocator version");
    }
    if (Load32(metadata_ + kBlockMetadataBytesOffset) !=
        kPersistentMetadataBytes) {
        Raise(AllocatorErrorCode::Corrupt, "block metadata size mismatch");
    }
    if (Load64(metadata_ + kBlockZoneBytesOffset) != block_zone_bytes_ ||
        Load64(metadata_ + kBlockPayloadBytesOffset) != payload_bytes_ ||
        Load32(metadata_ + kBlockCapacityOffset) != capacity_ ||
        metadata_[kBlockHeaderWidthOffset] != header_bytes_) {
        Raise(AllocatorErrorCode::Corrupt, "block allocator geometry mismatch");
    }
    if (payload_bytes_ == 0 ||
        (header_bytes_ != 2 && header_bytes_ != 4 && header_bytes_ != 8) ||
        capacity_ == 0) {
        Raise(AllocatorErrorCode::Corrupt, "invalid block allocator geometry");
    }
    if (payload_bytes_ >
        std::numeric_limits<std::size_t>::max() - header_bytes_) {
        Raise(AllocatorErrorCode::Corrupt, "block stride overflow");
    }
    const auto stride = payload_bytes_ + header_bytes_;
    if (block_zone_bytes_ / stride != capacity_ ||
        capacity_ > MaxCapacityForHeader(header_bytes_)) {
        Raise(AllocatorErrorCode::Corrupt, "invalid encoded block capacity");
    }
    const auto canonical_header_bytes = SelectHeaderBytes(
        block_zone_bytes_,
        payload_bytes_,
        FixedBlockHeaderWidth::Automatic);
    const auto canonical_capacity = block_zone_bytes_ /
        (payload_bytes_ + canonical_header_bytes);
    if (header_bytes_ != canonical_header_bytes ||
        static_cast<std::size_t>(capacity_) != canonical_capacity) {
        Raise(
            AllocatorErrorCode::Corrupt,
            "noncanonical block allocator geometry");
    }
    const auto expected_hash = BlockImmutableHash(
        static_cast<std::uint32_t>(metadata_bytes_),
        static_cast<std::uint64_t>(block_zone_bytes_),
        static_cast<std::uint64_t>(payload_bytes_),
        capacity_,
        static_cast<std::uint8_t>(header_bytes_));
    if (Load64(metadata_ + kBlockImmutableHashOffset) != expected_hash) {
        Raise(AllocatorErrorCode::Corrupt, "block immutable checksum mismatch");
    }
    for (std::size_t offset = kBlockReservedOffset;
         offset < kPersistentMetadataBytes;
         ++offset) {
        if (metadata_[offset] != 0) {
            Raise(AllocatorErrorCode::Corrupt, "nonzero block reserved bytes");
        }
    }
}

void FixedBlockAllocator::RequireNormalAccess() const {
    if (metadata_ == nullptr || block_zone_ == nullptr || capacity_ == 0) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "allocator handle is empty or moved from");
    }
    if (recovery_access_) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "ordinary access requires a normal attach or successful rebuild");
    }
}

std::uint32_t FixedBlockAllocator::StoredHighWater() const noexcept {
    return Load32(metadata_ + kBlockHighWaterOffset);
}

std::uint32_t FixedBlockAllocator::StoredUsedCount() const noexcept {
    return Load32(metadata_ + kBlockUsedCountOffset);
}

std::uint8_t* FixedBlockAllocator::BlockHeaderAddress(
    std::uint32_t id) const {
    if (id >= capacity_) {
        Raise(AllocatorErrorCode::InvalidId, "block ID is outside this allocator");
    }
    const auto stride = payload_bytes_ + header_bytes_;
    return block_zone_ + static_cast<std::size_t>(id) * stride;
}

FixedBlockAllocator::BlockHeader FixedBlockAllocator::ReadBlockHeader(
    std::uint32_t id) const {
    const auto* address = BlockHeaderAddress(id);
    std::uint64_t raw = 0;
    switch (header_bytes_) {
    case 2:
        raw = Load16(address);
        break;
    case 4:
        raw = Load32(address);
        break;
    case 8:
        raw = Load64(address);
        if ((raw >> 34U) != 0) {
            Raise(AllocatorErrorCode::Corrupt, "block header reserved bits are set");
        }
        break;
    default:
        Raise(AllocatorErrorCode::Corrupt, "invalid block header width");
    }

    BlockHeader result;
    result.used = (raw & 1U) != 0;
    result.has_next = (raw & 2U) != 0;
    result.next = static_cast<std::uint32_t>((raw >> 2U) & UINT32_MAX);
    if (result.used && result.has_next) {
        Raise(AllocatorErrorCode::Corrupt, "allocated block links into free list");
    }
    if (!result.has_next && result.next != 0) {
        Raise(AllocatorErrorCode::Corrupt, "block tail contains a next ID");
    }
    if (result.used && result.next != 0) {
        Raise(AllocatorErrorCode::Corrupt, "allocated block contains free metadata");
    }
    return result;
}

void FixedBlockAllocator::WriteBlockHeader(
    std::uint32_t id,
    bool used,
    std::optional<std::uint32_t> next) {
    if (used && next.has_value()) {
        Raise(AllocatorErrorCode::InvalidArgument, "allocated block cannot link free ID");
    }
    if (next.has_value() && *next >= capacity_) {
        Raise(AllocatorErrorCode::InvalidId, "free-list next ID is outside allocator");
    }
    std::uint64_t raw = used ? 1U : 0U;
    if (next.has_value()) {
        raw |= 2U;
        raw |= static_cast<std::uint64_t>(*next) << 2U;
    }
    auto* address = BlockHeaderAddress(id);
    switch (header_bytes_) {
    case 2:
        Store16(address, static_cast<std::uint16_t>(raw));
        break;
    case 4:
        Store32(address, static_cast<std::uint32_t>(raw));
        break;
    case 8:
        Store64(address, raw);
        break;
    default:
        Raise(AllocatorErrorCode::Corrupt, "invalid block header width");
    }
}

void FixedBlockAllocator::Validate() const {
    RequireNormalAccess();
    ValidateImmutable();
    const auto high_water = StoredHighWater();
    if (high_water > capacity_) {
        Raise(AllocatorErrorCode::Corrupt, "invalid block allocator counters");
    }
    std::vector<std::uint8_t> scratch(
        (static_cast<std::size_t>(high_water) + 7U) / 8U, 0);
    ValidateWithScratch(scratch.data(), scratch.size());
}

void FixedBlockAllocator::ValidateWithScratch(
    std::uint8_t* scratch,
    std::size_t scratch_bytes) const {
    ValidateImmutable();
    const auto high_water = StoredHighWater();
    const auto used_count = StoredUsedCount();
    const auto free_head = Load32(metadata_ + kBlockFreeHeadOffset);
    if (high_water > capacity_ || used_count > high_water ||
        (free_head != kInvalidId && free_head >= high_water)) {
        Raise(AllocatorErrorCode::Corrupt, "invalid block allocator counters");
    }
    const auto required_scratch =
        (static_cast<std::size_t>(high_water) + 7U) / 8U;
    if (required_scratch > scratch_bytes ||
        (required_scratch != 0 && scratch == nullptr)) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "fixed block validation scratch is too small");
    }
    if (required_scratch != 0) {
        std::memset(scratch, 0, required_scratch);
    }
    std::uint32_t observed_used = 0;
    for (std::uint32_t id = 0; id < high_water; ++id) {
        if (ReadBlockHeader(id).used) ++observed_used;
    }
    if (observed_used != used_count) {
        Raise(AllocatorErrorCode::Corrupt, "block used count mismatch");
    }

    auto cursor = free_head;
    std::uint32_t observed_free = 0;
    while (cursor != kInvalidId) {
        if (cursor >= high_water || GetBit(scratch, high_water, cursor)) {
            Raise(AllocatorErrorCode::Corrupt, "block free-list cycle or bad ID");
        }
        const auto header = ReadBlockHeader(cursor);
        if (header.used) {
            Raise(AllocatorErrorCode::Corrupt, "free list contains allocated block");
        }
        SetBit(scratch, cursor);
        ++observed_free;
        cursor = header.has_next ? header.next : kInvalidId;
    }
    if (observed_free != high_water - used_count) {
        Raise(AllocatorErrorCode::Corrupt, "block free-list length mismatch");
    }
    for (std::uint32_t id = 0; id < high_water; ++id) {
        const auto header = ReadBlockHeader(id);
        if (!header.used && !GetBit(scratch, high_water, id)) {
            Raise(AllocatorErrorCode::Corrupt, "free block is unreachable");
        }
    }
}

std::uint32_t FixedBlockAllocator::Allocate() {
    RequireNormalAccess();
    const auto high_water = HighWater();
    const auto used_count = UsedCount();
    auto free_head = Load32(metadata_ + kBlockFreeHeadOffset);
    if (high_water > capacity_ || used_count > high_water ||
        (free_head != kInvalidId && free_head >= high_water)) {
        Raise(AllocatorErrorCode::Corrupt, "invalid block allocator state");
    }

    if (free_head != kInvalidId) {
        const auto header = ReadBlockHeader(free_head);
        if (header.used) {
            Raise(AllocatorErrorCode::Corrupt, "free head is allocated");
        }
        const auto id = free_head;
        free_head = header.has_next ? header.next : kInvalidId;
        if (free_head != kInvalidId && free_head >= high_water) {
            Raise(AllocatorErrorCode::Corrupt, "free head next ID is invalid");
        }
        WriteBlockHeader(id, true, std::nullopt);
        Store32(metadata_ + kBlockFreeHeadOffset, free_head);
        Store32(metadata_ + kBlockUsedCountOffset, used_count + 1U);
        return id;
    }

    if (high_water >= capacity_) {
        Raise(AllocatorErrorCode::Capacity, "fixed block allocator is full");
    }
    WriteBlockHeader(high_water, true, std::nullopt);
    Store32(metadata_ + kBlockHighWaterOffset, high_water + 1U);
    Store32(metadata_ + kBlockUsedCountOffset, used_count + 1U);
    return high_water;
}

void FixedBlockAllocator::Free(std::uint32_t id) {
    RequireNormalAccess();
    const auto high_water = HighWater();
    const auto used_count = UsedCount();
    const auto free_head = Load32(metadata_ + kBlockFreeHeadOffset);
    if (id >= high_water) {
        Raise(AllocatorErrorCode::InvalidId, "block ID was never allocated here");
    }
    if (used_count == 0 ||
        (free_head != kInvalidId && free_head >= high_water)) {
        Raise(AllocatorErrorCode::Corrupt, "invalid block allocator free state");
    }
    if (!ReadBlockHeader(id).used) {
        Raise(AllocatorErrorCode::NotAllocated, "double free of fixed block");
    }
    if (free_head == kInvalidId) {
        WriteBlockHeader(id, false, std::nullopt);
    } else {
        WriteBlockHeader(id, false, free_head);
    }
    Store32(metadata_ + kBlockFreeHeadOffset, id);
    Store32(metadata_ + kBlockUsedCountOffset, used_count - 1U);
}

void FixedBlockAllocator::Rebuild(
    const std::uint8_t* live_bits,
    std::size_t live_bit_count) {
    const auto required_scratch =
        RebuildScratchBytes(live_bits, live_bit_count);
    std::vector<std::uint8_t> scratch(required_scratch, 0);
    RebuildPrepared(
        live_bits,
        live_bit_count,
        scratch.data(),
        scratch.size());
}

std::size_t FixedBlockAllocator::RebuildScratchBytes(
    const std::uint8_t* live_bits,
    std::size_t live_bit_count) const {
    ValidateImmutable();
    if (live_bit_count > capacity_ ||
        (live_bit_count != 0 && live_bits == nullptr)) {
        Raise(AllocatorErrorCode::InvalidArgument, "invalid live block bitmap");
    }

    std::uint32_t high_water = 0;
    for (std::size_t index = 0; index < live_bit_count; ++index) {
        const auto id = static_cast<std::uint32_t>(index);
        if (BitmapBit(live_bits, live_bit_count, id)) {
            high_water = id + 1U;
        }
    }
    return (static_cast<std::size_t>(high_water) + 7U) / 8U;
}

void FixedBlockAllocator::RebuildPrepared(
    const std::uint8_t* live_bits,
    std::size_t live_bit_count,
    std::uint8_t* scratch,
    std::size_t scratch_bytes) {
    const auto required_scratch =
        RebuildScratchBytes(live_bits, live_bit_count);
    if (required_scratch > scratch_bytes ||
        (required_scratch != 0 && scratch == nullptr)) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "fixed block rebuild scratch is too small");
    }
    RebuildWithScratch(
        live_bits,
        live_bit_count,
        scratch,
        scratch_bytes);
    recovery_access_ = false;
}

void FixedBlockAllocator::RebuildWithScratch(
    const std::uint8_t* live_bits,
    std::size_t live_bit_count,
    std::uint8_t* scratch,
    std::size_t scratch_bytes) {
    ValidateImmutable();
    if (live_bit_count > capacity_ ||
        (live_bit_count != 0 && live_bits == nullptr)) {
        Raise(AllocatorErrorCode::InvalidArgument, "invalid live block bitmap");
    }

    std::uint32_t high_water = 0;
    for (std::size_t index = 0; index < live_bit_count; ++index) {
        const auto id = static_cast<std::uint32_t>(index);
        if (BitmapBit(live_bits, live_bit_count, id)) {
            high_water = id + 1U;
        }
    }
    const auto required_scratch =
        (static_cast<std::size_t>(high_water) + 7U) / 8U;
    if (required_scratch > scratch_bytes ||
        (required_scratch != 0 && scratch == nullptr)) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "fixed block rebuild scratch is too small");
    }

    std::uint32_t used_count = 0;
    std::uint32_t free_head = kInvalidId;
    for (std::uint32_t id = 0; id < high_water; ++id) {
        if (BitmapBit(live_bits, live_bit_count, id)) {
            WriteBlockHeader(id, true, std::nullopt);
            ++used_count;
        } else {
            if (free_head == kInvalidId) {
                WriteBlockHeader(id, false, std::nullopt);
            } else {
                WriteBlockHeader(id, false, free_head);
            }
            free_head = id;
        }
    }
    Store32(metadata_ + kBlockHighWaterOffset, high_water);
    Store32(metadata_ + kBlockUsedCountOffset, used_count);
    Store32(metadata_ + kBlockFreeHeadOffset, free_head);
    ValidateWithScratch(scratch, scratch_bytes);
}

void* FixedBlockAllocator::BlockData(std::uint32_t id) {
    RequireNormalAccess();
    if (id >= HighWater()) {
        Raise(AllocatorErrorCode::InvalidId, "block ID was never allocated here");
    }
    if (!ReadBlockHeader(id).used) {
        Raise(AllocatorErrorCode::NotAllocated, "block ID is currently free");
    }
    return BlockHeaderAddress(id) + header_bytes_;
}

const void* FixedBlockAllocator::BlockData(std::uint32_t id) const {
    RequireNormalAccess();
    if (id >= HighWater()) {
        Raise(AllocatorErrorCode::InvalidId, "block ID was never allocated here");
    }
    if (!ReadBlockHeader(id).used) {
        Raise(AllocatorErrorCode::NotAllocated, "block ID is currently free");
    }
    return BlockHeaderAddress(id) + header_bytes_;
}

const void* FixedBlockAllocator::RecoveryBlockData(std::uint32_t id) const {
    if (!recovery_access_) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "recovery payload access requires recovery attach");
    }
    if (id >= capacity_) {
        Raise(
            AllocatorErrorCode::InvalidId,
            "recovery block ID is outside this allocator");
    }
    return BlockHeaderAddress(id) + header_bytes_;
}

bool FixedBlockAllocator::IsAllocated(std::uint32_t id) const {
    RequireNormalAccess();
    if (id >= HighWater()) return false;
    return ReadBlockHeader(id).used;
}

std::uint32_t FixedBlockAllocator::HighWater() const {
    RequireNormalAccess();
    return StoredHighWater();
}

std::uint32_t FixedBlockAllocator::UsedCount() const {
    RequireNormalAccess();
    return StoredUsedCount();
}

BoxAllocator::BoxAllocator(
    std::uint8_t* metadata,
    std::size_t metadata_bytes,
    std::uint64_t data_bytes,
    std::uint8_t root_level,
    std::uint8_t root_slots,
    FixedBlockAllocator blocks) noexcept
    : metadata_(metadata),
      metadata_bytes_(metadata_bytes),
      data_bytes_(data_bytes),
      root_level_(root_level),
      root_slots_(root_slots),
      blocks_(std::move(blocks)) {}

std::uint64_t BoxAllocator::UnitBytes(std::uint8_t level) {
    std::uint64_t result = 8;
    for (std::uint8_t current = 0; current < level; ++current) {
        result = CheckedMultiply(
            result,
            16,
            AllocatorErrorCode::Capacity,
            "box allocation unit overflows uint64");
    }
    return result;
}

BoxAllocator::Shape BoxAllocator::ShapeForRequest(
    std::uint64_t requested_size) {
    if (requested_size == 0) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "zero-size box allocation must use caller None state");
    }
    auto units = requested_size / 8U;
    if (requested_size % 8U != 0) ++units;
    std::uint8_t level = 0;
    while (units > 15U) {
        units = units / 16U + (units % 16U == 0 ? 0U : 1U);
        if (level == std::numeric_limits<std::uint8_t>::max()) {
            Raise(AllocatorErrorCode::Capacity, "box allocation level overflow");
        }
        ++level;
    }
    const auto unit_bytes = UnitBytes(level);
    const auto rounded_bytes = CheckedMultiply(
        unit_bytes,
        units,
        AllocatorErrorCode::Capacity,
        "rounded box allocation overflows uint64");
    return Shape{
        unit_bytes,
        rounded_bytes,
        level,
        static_cast<std::uint8_t>(units)};
}

BoxAllocator::Shape BoxAllocator::ShapeForExact(std::uint64_t exact_size) {
    if (exact_size == 0 || exact_size % 8U != 0) {
        Raise(AllocatorErrorCode::InvalidArgument, "noncanonical box data size");
    }
    auto units = exact_size / 8U;
    std::uint8_t level = 0;
    while (units > 15U && units % 16U == 0) {
        units /= 16U;
        if (level == std::numeric_limits<std::uint8_t>::max()) {
            Raise(AllocatorErrorCode::InvalidArgument, "box data level overflow");
        }
        ++level;
    }
    if (units == 0 || units > 15U) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "box data size must equal 8*multiple*16^level");
    }
    const auto unit_bytes = UnitBytes(level);
    const auto rounded_bytes = CheckedMultiply(
        unit_bytes,
        units,
        AllocatorErrorCode::InvalidArgument,
        "box data size overflows uint64");
    if (rounded_bytes != exact_size) {
        Raise(AllocatorErrorCode::InvalidArgument, "noncanonical box data size");
    }
    return Shape{
        unit_bytes,
        rounded_bytes,
        level,
        static_cast<std::uint8_t>(units)};
}

std::uint64_t BoxAllocator::RoundSize(std::uint64_t requested_size) {
    return ShapeForRequest(requested_size).rounded_bytes;
}

std::uint64_t BoxAllocator::LargestCanonicalDataSize(
    std::uint64_t upper_bound) {
    if (upper_bound < 8) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "box data upper bound is smaller than eight bytes");
    }
    const auto upper_units = upper_bound / 8U;
    std::uint64_t best_units = 0;
    std::uint64_t level_units = 1;
    while (level_units <= upper_units) {
        const auto multiple = std::min<std::uint64_t>(
            15, upper_units / level_units);
        best_units = std::max(best_units, multiple * level_units);
        if (level_units >
            std::numeric_limits<std::uint64_t>::max() / 16U) {
            break;
        }
        level_units *= 16U;
    }
    if (best_units == 0) {
        Raise(AllocatorErrorCode::InvalidArgument, "no canonical box data size");
    }
    return best_units * 8U;
}

std::uint64_t BoxAllocator::FullExpansionNodeCount(
    std::uint64_t canonical_data_bytes) {
    const auto shape = ShapeForExact(canonical_data_bytes);
    std::uint64_t nodes = 1;
    std::uint64_t nodes_at_depth = shape.multiple;
    for (std::uint8_t depth = 0; depth < shape.level; ++depth) {
        if (nodes_at_depth >
            static_cast<std::uint64_t>(UINT32_MAX) - nodes) {
            Raise(
                AllocatorErrorCode::Capacity,
                "fully expanded box exceeds uint32 metadata node IDs");
        }
        nodes += nodes_at_depth;
        if (static_cast<unsigned int>(depth) + 1U < shape.level) {
            if (nodes_at_depth >
                static_cast<std::uint64_t>(UINT32_MAX) / 16U) {
                Raise(
                    AllocatorErrorCode::Capacity,
                    "fully expanded box exceeds uint32 metadata node IDs");
            }
            nodes_at_depth *= 16U;
        }
    }
    return nodes;
}

std::uint64_t BoxAllocator::MinimumMetadataBytesForFullExpansion(
    std::uint64_t canonical_data_bytes) {
    const auto node_count = FullExpansionNodeCount(canonical_data_bytes);
    const auto header_width = static_cast<std::uint64_t>(
        FixedBlockAllocator::MinimumHeaderWidth(node_count));
    const auto stride = CheckedAdd(
        static_cast<std::uint64_t>(kNodePayloadBytes),
        header_width,
        AllocatorErrorCode::Capacity,
        "box metadata node stride overflows uint64");
    return CheckedAdd(
        static_cast<std::uint64_t>(kBoxBlockZoneOffset),
        CheckedMultiply(
            node_count,
            stride,
            AllocatorErrorCode::Capacity,
            "box metadata size overflows uint64"),
        AllocatorErrorCode::Capacity,
        "box metadata size overflows uint64");
}

std::uint64_t BoxAllocator::LargestFullyRepresentableDataSize(
    std::uint64_t total_budget_bytes) {
    std::uint64_t best = 0;
    std::uint64_t unit_bytes = 8;
    for (;;) {
        bool level_is_representable = false;
        for (std::uint64_t multiple = 1; multiple <= 15; ++multiple) {
            if (unit_bytes >
                std::numeric_limits<std::uint64_t>::max() / multiple) {
                break;
            }
            const auto data_bytes = unit_bytes * multiple;
            std::uint64_t metadata_bytes = 0;
            try {
                metadata_bytes =
                    MinimumMetadataBytesForFullExpansion(data_bytes);
            } catch (const AllocatorError& error) {
                if (error.Code() != AllocatorErrorCode::Capacity) throw;
                break;
            }
            level_is_representable = true;
            if (metadata_bytes <= total_budget_bytes &&
                data_bytes <= total_budget_bytes - metadata_bytes) {
                best = std::max(best, data_bytes);
            }
        }
        if (!level_is_representable ||
            unit_bytes > std::numeric_limits<std::uint64_t>::max() / 16U) {
            break;
        }
        unit_bytes *= 16U;
    }
    if (best == 0) {
        Raise(
            AllocatorErrorCode::Capacity,
            "budget cannot hold a fully representable box");
    }
    return best;
}

BoxAllocator BoxAllocator::Initialize(
    void* metadata,
    std::size_t metadata_bytes,
    std::uint64_t data_bytes) {
    if (metadata == nullptr ||
        metadata_bytes <= kBoxBlockZoneOffset) {
        Raise(AllocatorErrorCode::InvalidArgument, "invalid box metadata region");
    }
    const auto data_shape = ShapeForExact(data_bytes);
    auto* bytes = static_cast<std::uint8_t*>(metadata);
    const auto block_zone_bytes = metadata_bytes - kBoxBlockZoneOffset;

    // Preflight the exact embedded FixedBlock geometry before changing the
    // caller's persistent bytes. FixedBlockAllocator::Initialize performs the
    // same selection, but calling it only after this check preserves the
    // outer header when even one root node cannot fit.
    const auto block_header_bytes = SelectHeaderBytes(
        block_zone_bytes,
        kNodePayloadBytes,
        FixedBlockHeaderWidth::Automatic);
    const auto block_stride = kNodePayloadBytes + block_header_bytes;
    const auto block_capacity = block_zone_bytes / block_stride;
    if (block_capacity == 0 ||
        static_cast<std::uint64_t>(block_capacity) >
            MaxCapacityForHeader(block_header_bytes)) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "box metadata cannot hold its root node");
    }

    std::memset(bytes, 0, kPersistentHeaderBytes);
    std::copy(kBoxMagic.begin(), kBoxMagic.end(), bytes);
    Store32(bytes + kBoxVersionOffset, kBoxVersion);
    Store32(
        bytes + kBoxHeaderBytesOffset,
        static_cast<std::uint32_t>(kPersistentHeaderBytes));
    Store64(
        bytes + kBoxMetadataBytesOffset,
        static_cast<std::uint64_t>(metadata_bytes));
    Store64(bytes + kBoxDataBytesOffset, data_bytes);
    Store64(
        bytes + kBoxBlockMetadataOffsetOffset,
        static_cast<std::uint64_t>(kBoxBlockMetadataOffset));
    Store64(
        bytes + kBoxBlockZoneOffsetOffset,
        static_cast<std::uint64_t>(kBoxBlockZoneOffset));
    Store64(
        bytes + kBoxBlockZoneBytesOffset,
        static_cast<std::uint64_t>(block_zone_bytes));
    Store32(
        bytes + kBoxNodeBytesOffset,
        static_cast<std::uint32_t>(kNodePayloadBytes));
    Store32(bytes + kBoxRootIdOffset, 0);
    bytes[kBoxRootLevelOffset] = data_shape.level;
    bytes[kBoxRootSlotsOffset] = data_shape.multiple;
    Store64(
        bytes + kBoxImmutableHashOffset,
        BoxImmutableHash(
            static_cast<std::uint64_t>(metadata_bytes),
            data_bytes,
            static_cast<std::uint64_t>(block_zone_bytes),
            data_shape.level,
            data_shape.multiple));

    auto blocks = FixedBlockAllocator::Initialize(
        bytes + kBoxBlockMetadataOffset,
        FixedBlockAllocator::kPersistentMetadataBytes,
        bytes + kBoxBlockZoneOffset,
        block_zone_bytes,
        kNodePayloadBytes);
    const auto root_id = blocks.Allocate();
    if (root_id != 0) {
        Raise(AllocatorErrorCode::Corrupt, "box root did not receive block ID zero");
    }
    BoxAllocator allocator(
        bytes,
        metadata_bytes,
        data_bytes,
        data_shape.level,
        data_shape.multiple,
        std::move(blocks));
    allocator.InitializeNode(
        0,
        FixedBlockAllocator::kInvalidId,
        UINT8_MAX,
        data_shape.level,
        data_shape.multiple);
    // A newly initialized box has exactly one metadata node. Its complete
    // validation uses one stack byte, so no bad_alloc can occur after the
    // first persistent write.
    std::uint8_t validation_scratch = 0;
    allocator.ValidateWithScratch(&validation_scratch, 1);
    return allocator;
}

BoxAllocator BoxAllocator::AttachForRecovery(
    void* metadata,
    std::size_t metadata_bytes,
    std::uint64_t data_bytes) {
    if (metadata == nullptr || metadata_bytes < kBoxBlockZoneOffset) {
        Raise(AllocatorErrorCode::InvalidArgument, "invalid box attach region");
    }
    auto* bytes = static_cast<std::uint8_t*>(metadata);
    const auto root_level = bytes[kBoxRootLevelOffset];
    const auto root_slots = bytes[kBoxRootSlotsOffset];
    if (metadata_bytes < kBoxBlockZoneOffset) {
        Raise(AllocatorErrorCode::InvalidArgument, "box metadata is truncated");
    }
    auto blocks = FixedBlockAllocator::AttachForRecovery(
        bytes + kBoxBlockMetadataOffset,
        FixedBlockAllocator::kPersistentMetadataBytes,
        bytes + kBoxBlockZoneOffset,
        metadata_bytes - kBoxBlockZoneOffset);
    BoxAllocator allocator(
        bytes,
        metadata_bytes,
        data_bytes,
        root_level,
        root_slots,
        std::move(blocks));
    allocator.ValidateImmutable();
    return allocator;
}

BoxAllocator BoxAllocator::Attach(
    void* metadata,
    std::size_t metadata_bytes,
    std::uint64_t data_bytes) {
    auto allocator = AttachForRecovery(metadata, metadata_bytes, data_bytes);
    allocator.blocks_.recovery_access_ = false;
    allocator.Validate();
    return allocator;
}

void BoxAllocator::ValidateImmutable() const {
    if (metadata_ == nullptr || metadata_bytes_ < kBoxBlockZoneOffset) {
        Raise(AllocatorErrorCode::Corrupt, "missing box metadata region");
    }
    if (!std::equal(kBoxMagic.begin(), kBoxMagic.end(), metadata_)) {
        Raise(AllocatorErrorCode::Corrupt, "bad box allocator magic");
    }
    if (Load32(metadata_ + kBoxVersionOffset) != kBoxVersion ||
        Load32(metadata_ + kBoxHeaderBytesOffset) != kPersistentHeaderBytes) {
        Raise(AllocatorErrorCode::Corrupt, "unsupported box allocator version");
    }
    if (Load64(metadata_ + kBoxMetadataBytesOffset) != metadata_bytes_ ||
        Load64(metadata_ + kBoxDataBytesOffset) != data_bytes_ ||
        Load64(metadata_ + kBoxBlockMetadataOffsetOffset) !=
            kBoxBlockMetadataOffset ||
        Load64(metadata_ + kBoxBlockZoneOffsetOffset) != kBoxBlockZoneOffset ||
        Load64(metadata_ + kBoxBlockZoneBytesOffset) !=
            metadata_bytes_ - kBoxBlockZoneOffset ||
        Load32(metadata_ + kBoxNodeBytesOffset) != kNodePayloadBytes ||
        Load32(metadata_ + kBoxRootIdOffset) != 0 ||
        metadata_[kBoxRootLevelOffset] != root_level_ ||
        metadata_[kBoxRootSlotsOffset] != root_slots_) {
        Raise(AllocatorErrorCode::Corrupt, "box allocator geometry mismatch");
    }
    const auto exact = ShapeForExact(data_bytes_);
    if (exact.level != root_level_ || exact.multiple != root_slots_) {
        Raise(AllocatorErrorCode::Corrupt, "box root geometry is invalid");
    }
    const auto expected_hash = BoxImmutableHash(
        static_cast<std::uint64_t>(metadata_bytes_),
        data_bytes_,
        static_cast<std::uint64_t>(metadata_bytes_ - kBoxBlockZoneOffset),
        root_level_,
        root_slots_);
    if (Load64(metadata_ + kBoxImmutableHashOffset) != expected_hash) {
        Raise(AllocatorErrorCode::Corrupt, "box immutable checksum mismatch");
    }
    for (std::size_t offset = kBoxReservedAOffset;
         offset < kBoxImmutableHashOffset;
         ++offset) {
        if (metadata_[offset] != 0) {
            Raise(AllocatorErrorCode::Corrupt, "nonzero box reserved bytes");
        }
    }
    for (std::size_t offset = kBoxReservedBOffset;
         offset < kPersistentHeaderBytes;
         ++offset) {
        if (metadata_[offset] != 0) {
            Raise(AllocatorErrorCode::Corrupt, "nonzero box reserved bytes");
        }
    }
    blocks_.ValidateImmutable();
    if (blocks_.PayloadBytes() != kNodePayloadBytes) {
        Raise(
            AllocatorErrorCode::Corrupt,
            "box embedded block payload size mismatch");
    }
}

BoxAllocator::Node BoxAllocator::ReadNode(std::uint32_t id) const {
    const auto* bytes = static_cast<const std::uint8_t*>(blocks_.BlockData(id));
    Node node;
    node.parent = Load32(bytes + kNodeParentOffset);
    node.parent_slot = bytes[kNodeParentSlotOffset];
    node.level = bytes[kNodeLevelOffset];
    node.slot_count = bytes[kNodeSlotCountOffset];
    if (bytes[kNodeReservedOffset] != 0) {
        Raise(AllocatorErrorCode::Corrupt, "box node reserved byte is set");
    }
    for (std::size_t index = 0; index < node.states.size(); ++index) {
        const auto raw = bytes[kNodeStatesOffset + index];
        if (raw > static_cast<std::uint8_t>(SlotState::ObjectContinued)) {
            Raise(AllocatorErrorCode::Corrupt, "box node has invalid slot state");
        }
        node.states[index] = static_cast<SlotState>(raw);
        node.children[index] = Load32(
            bytes + kNodeChildrenOffset + index * sizeof(std::uint32_t));
    }
    return node;
}

void BoxAllocator::WriteNode(std::uint32_t id, const Node& node) {
    auto* bytes = static_cast<std::uint8_t*>(blocks_.BlockData(id));
    std::memset(bytes, 0, kNodePayloadBytes);
    Store32(bytes + kNodeParentOffset, node.parent);
    bytes[kNodeParentSlotOffset] = node.parent_slot;
    bytes[kNodeLevelOffset] = node.level;
    bytes[kNodeSlotCountOffset] = node.slot_count;
    for (std::size_t index = 0; index < node.states.size(); ++index) {
        bytes[kNodeStatesOffset + index] =
            static_cast<std::uint8_t>(node.states[index]);
        Store32(
            bytes + kNodeChildrenOffset + index * sizeof(std::uint32_t),
            node.children[index]);
    }
}

void BoxAllocator::InitializeNode(
    std::uint32_t id,
    std::uint32_t parent,
    std::uint8_t parent_slot,
    std::uint8_t level,
    std::uint8_t slot_count) {
    if (slot_count == 0 || slot_count > 16) {
        Raise(AllocatorErrorCode::Corrupt, "invalid box node slot count");
    }
    Node node;
    node.parent = parent;
    node.parent_slot = parent_slot;
    node.level = level;
    node.slot_count = slot_count;
    node.states.fill(SlotState::Unused);
    node.children.fill(FixedBlockAllocator::kInvalidId);
    WriteNode(id, node);
}

bool BoxAllocator::NodeIsEmpty(const Node& node) const {
    return std::all_of(
        node.states.begin(),
        node.states.begin() + node.slot_count,
        [](SlotState state) { return state == SlotState::Unused; });
}

BoxAllocator::AllocationResult BoxAllocator::AllocateInNode(
    std::uint32_t node_id,
    std::uint64_t node_base,
    const Shape& shape) {
    auto node = ReadNode(node_id);
    if (shape.level > node.level) {
        return AllocationResult{AllocationResultKind::NoSpace, 0};
    }
    const auto slot_bytes = UnitBytes(node.level);
    if (shape.level == node.level) {
        std::size_t run = 0;
        for (std::size_t index = 0; index < node.slot_count; ++index) {
            if (node.states[index] == SlotState::Unused) {
                ++run;
            } else {
                run = 0;
            }
            if (run == shape.multiple) {
                const auto first = index + 1U - run;
                node.states[first] = SlotState::ObjectStart;
                for (std::size_t continuation = first + 1U;
                     continuation <= index;
                     ++continuation) {
                    node.states[continuation] = SlotState::ObjectContinued;
                }
                WriteNode(node_id, node);
                const auto displacement = CheckedMultiply(
                    static_cast<std::uint64_t>(first),
                    slot_bytes,
                    AllocatorErrorCode::Corrupt,
                    "box node offset overflow");
                return AllocationResult{
                    AllocationResultKind::Success,
                    CheckedAdd(
                        node_base,
                        displacement,
                        AllocatorErrorCode::Corrupt,
                        "box node offset overflow")};
            }
        }
        return AllocationResult{AllocationResultKind::NoSpace, 0};
    }

    bool saw_metadata_capacity = false;
    for (std::size_t index = 0; index < node.slot_count; ++index) {
        if (node.states[index] != SlotState::Child) continue;
        const auto child_base = CheckedAdd(
            node_base,
            CheckedMultiply(
                static_cast<std::uint64_t>(index),
                slot_bytes,
                AllocatorErrorCode::Corrupt,
                "box child offset overflow"),
            AllocatorErrorCode::Corrupt,
            "box child offset overflow");
        const auto result = AllocateInNode(
            node.children[index], child_base, shape);
        if (result.kind == AllocationResultKind::Success) return result;
        if (result.kind == AllocationResultKind::MetadataCapacity) {
            saw_metadata_capacity = true;
        }
    }

    for (std::size_t index = 0; index < node.slot_count; ++index) {
        if (node.states[index] != SlotState::Unused) continue;
        std::uint32_t child_id = 0;
        try {
            child_id = blocks_.Allocate();
        } catch (const AllocatorError& error) {
            if (error.Code() == AllocatorErrorCode::Capacity) {
                return AllocationResult{
                    AllocationResultKind::MetadataCapacity, 0};
            }
            throw;
        }
        bool linked = false;
        try {
            InitializeNode(
                child_id,
                node_id,
                static_cast<std::uint8_t>(index),
                static_cast<std::uint8_t>(node.level - 1U),
                16);
            node.states[index] = SlotState::Child;
            node.children[index] = child_id;
            WriteNode(node_id, node);
            linked = true;
            const auto child_base = CheckedAdd(
                node_base,
                CheckedMultiply(
                    static_cast<std::uint64_t>(index),
                    slot_bytes,
                    AllocatorErrorCode::Corrupt,
                    "box child offset overflow"),
                AllocatorErrorCode::Corrupt,
                "box child offset overflow");
            const auto result = AllocateInNode(child_id, child_base, shape);
            if (result.kind == AllocationResultKind::Success) return result;
            node.states[index] = SlotState::Unused;
            node.children[index] = FixedBlockAllocator::kInvalidId;
            WriteNode(node_id, node);
            linked = false;
            blocks_.Free(child_id);
            if (result.kind == AllocationResultKind::MetadataCapacity) {
                return result;
            }
        } catch (...) {
            if (linked) {
                node.states[index] = SlotState::Unused;
                node.children[index] = FixedBlockAllocator::kInvalidId;
                WriteNode(node_id, node);
            }
            if (blocks_.IsAllocated(child_id)) blocks_.Free(child_id);
            throw;
        }
    }
    return AllocationResult{
        saw_metadata_capacity
            ? AllocationResultKind::MetadataCapacity
            : AllocationResultKind::NoSpace,
        0};
}

std::uint64_t BoxAllocator::Allocate(std::uint64_t requested_size) {
    const auto shape = ShapeForRequest(requested_size);
    if (shape.rounded_bytes > data_bytes_) {
        Raise(AllocatorErrorCode::Capacity, "box allocation exceeds data region");
    }
    const auto result = AllocateInNode(0, 0, shape);
    if (result.kind != AllocationResultKind::Success) {
        Raise(
            AllocatorErrorCode::Capacity,
            result.kind == AllocationResultKind::MetadataCapacity
                ? "box metadata node region is full"
                : "box data region is full or fragmented");
    }
    return result.offset;
}

void BoxAllocator::ReserveInNode(
    std::uint32_t node_id,
    std::uint64_t node_base,
    std::uint64_t offset,
    const Shape& shape) {
    auto node = ReadNode(node_id);
    if (shape.level > node.level || offset < node_base) {
        Raise(AllocatorErrorCode::InvalidArgument, "invalid box reservation level");
    }
    const auto slot_bytes = UnitBytes(node.level);
    const auto delta = offset - node_base;
    const auto coverage = CheckedMultiply(
        slot_bytes,
        node.slot_count,
        AllocatorErrorCode::Corrupt,
        "box node coverage overflow");
    if (delta >= coverage) {
        Raise(AllocatorErrorCode::InvalidArgument, "box reservation is out of node");
    }

    if (shape.level == node.level) {
        if (delta % slot_bytes != 0) {
            Raise(AllocatorErrorCode::InvalidArgument, "misaligned box reservation");
        }
        const auto first64 = delta / slot_bytes;
        if (first64 > std::numeric_limits<std::size_t>::max()) {
            Raise(AllocatorErrorCode::InvalidArgument, "box slot is not addressable");
        }
        const auto first = static_cast<std::size_t>(first64);
        const auto end = first + shape.multiple;
        if (end > node.slot_count) {
            Raise(AllocatorErrorCode::InvalidArgument, "box reservation crosses node");
        }
        for (std::size_t index = first; index < end; ++index) {
            if (node.states[index] != SlotState::Unused) {
                Raise(AllocatorErrorCode::Overlap, "box reservation overlaps live data");
            }
        }
        node.states[first] = SlotState::ObjectStart;
        for (std::size_t index = first + 1U; index < end; ++index) {
            node.states[index] = SlotState::ObjectContinued;
        }
        WriteNode(node_id, node);
        return;
    }

    const auto slot64 = delta / slot_bytes;
    if (slot64 >= node.slot_count) {
        Raise(AllocatorErrorCode::InvalidArgument, "box reservation slot is invalid");
    }
    const auto slot = static_cast<std::size_t>(slot64);
    const auto child_base = CheckedAdd(
        node_base,
        CheckedMultiply(
            slot64,
            slot_bytes,
            AllocatorErrorCode::Corrupt,
            "box child offset overflow"),
        AllocatorErrorCode::Corrupt,
        "box child offset overflow");
    if (node.states[slot] == SlotState::Child) {
        ReserveInNode(node.children[slot], child_base, offset, shape);
        return;
    }
    if (node.states[slot] != SlotState::Unused) {
        Raise(AllocatorErrorCode::Overlap, "box reservation overlaps live data");
    }

    const auto child_id = blocks_.Allocate();
    bool linked = false;
    try {
        InitializeNode(
            child_id,
            node_id,
            static_cast<std::uint8_t>(slot),
            static_cast<std::uint8_t>(node.level - 1U),
            16);
        node.states[slot] = SlotState::Child;
        node.children[slot] = child_id;
        WriteNode(node_id, node);
        linked = true;
        ReserveInNode(child_id, child_base, offset, shape);
    } catch (...) {
        if (linked) {
            node.states[slot] = SlotState::Unused;
            node.children[slot] = FixedBlockAllocator::kInvalidId;
            WriteNode(node_id, node);
        }
        if (blocks_.IsAllocated(child_id)) blocks_.Free(child_id);
        throw;
    }
}

void BoxAllocator::ReserveAt(
    std::uint64_t offset,
    std::uint64_t requested_size) {
    const auto shape = ShapeForRequest(requested_size);
    if (offset % shape.unit_bytes != 0) {
        Raise(AllocatorErrorCode::InvalidArgument, "misaligned box reservation");
    }
    const auto end = CheckedAdd(
        offset,
        shape.rounded_bytes,
        AllocatorErrorCode::InvalidArgument,
        "box reservation end overflows");
    if (end > data_bytes_) {
        Raise(AllocatorErrorCode::InvalidArgument, "box reservation is out of bounds");
    }
    ReserveInNode(0, 0, offset, shape);
}

std::uint64_t BoxAllocator::FreeInNode(
    std::uint32_t node_id,
    std::uint64_t node_base,
    std::uint64_t offset,
    bool* became_empty) {
    auto node = ReadNode(node_id);
    const auto slot_bytes = UnitBytes(node.level);
    if (offset < node_base) {
        Raise(AllocatorErrorCode::NotAllocated, "box offset is not allocated");
    }
    const auto delta = offset - node_base;
    const auto slot64 = delta / slot_bytes;
    if (slot64 >= node.slot_count) {
        Raise(AllocatorErrorCode::NotAllocated, "box offset is not allocated");
    }
    const auto slot = static_cast<std::size_t>(slot64);
    if (node.states[slot] == SlotState::ObjectStart) {
        if (delta % slot_bytes != 0) {
            Raise(AllocatorErrorCode::NotAllocated, "box offset is not object start");
        }
        std::size_t count = 1;
        while (slot + count < node.slot_count &&
               node.states[slot + count] == SlotState::ObjectContinued) {
            ++count;
        }
        for (std::size_t index = slot; index < slot + count; ++index) {
            node.states[index] = SlotState::Unused;
            node.children[index] = FixedBlockAllocator::kInvalidId;
        }
        WriteNode(node_id, node);
        *became_empty = NodeIsEmpty(node);
        return CheckedMultiply(
            static_cast<std::uint64_t>(count),
            slot_bytes,
            AllocatorErrorCode::Corrupt,
            "box allocation size overflow");
    }
    if (node.states[slot] == SlotState::Child) {
        const auto child_base = CheckedAdd(
            node_base,
            CheckedMultiply(
                slot64,
                slot_bytes,
                AllocatorErrorCode::Corrupt,
                "box child offset overflow"),
            AllocatorErrorCode::Corrupt,
            "box child offset overflow");
        bool child_empty = false;
        const auto size = FreeInNode(
            node.children[slot], child_base, offset, &child_empty);
        if (child_empty) {
            const auto child_id = node.children[slot];
            node.states[slot] = SlotState::Unused;
            node.children[slot] = FixedBlockAllocator::kInvalidId;
            WriteNode(node_id, node);
            blocks_.Free(child_id);
        }
        *became_empty = NodeIsEmpty(node);
        return size;
    }
    Raise(AllocatorErrorCode::NotAllocated, "box offset is not object start");
}

void BoxAllocator::Free(std::uint64_t offset) {
    if (offset >= data_bytes_) {
        Raise(AllocatorErrorCode::InvalidArgument, "box free offset is out of bounds");
    }
    bool root_empty = false;
    static_cast<void>(FreeInNode(0, 0, offset, &root_empty));
}

std::uint64_t BoxAllocator::AllocatedSizeInNode(
    std::uint32_t node_id,
    std::uint64_t node_base,
    std::uint64_t offset) const {
    const auto node = ReadNode(node_id);
    const auto slot_bytes = UnitBytes(node.level);
    if (offset < node_base) {
        Raise(AllocatorErrorCode::NotAllocated, "box offset is not allocated");
    }
    const auto delta = offset - node_base;
    const auto slot64 = delta / slot_bytes;
    if (slot64 >= node.slot_count) {
        Raise(AllocatorErrorCode::NotAllocated, "box offset is not allocated");
    }
    const auto slot = static_cast<std::size_t>(slot64);
    if (node.states[slot] == SlotState::ObjectStart) {
        if (delta % slot_bytes != 0) {
            Raise(AllocatorErrorCode::NotAllocated, "box offset is not object start");
        }
        std::size_t count = 1;
        while (slot + count < node.slot_count &&
               node.states[slot + count] == SlotState::ObjectContinued) {
            ++count;
        }
        return CheckedMultiply(
            static_cast<std::uint64_t>(count),
            slot_bytes,
            AllocatorErrorCode::Corrupt,
            "box allocation size overflow");
    }
    if (node.states[slot] == SlotState::Child) {
        const auto child_base = CheckedAdd(
            node_base,
            CheckedMultiply(
                slot64,
                slot_bytes,
                AllocatorErrorCode::Corrupt,
                "box child offset overflow"),
            AllocatorErrorCode::Corrupt,
            "box child offset overflow");
        return AllocatedSizeInNode(
            node.children[slot], child_base, offset);
    }
    Raise(AllocatorErrorCode::NotAllocated, "box offset is not object start");
}

std::uint64_t BoxAllocator::AllocatedSize(std::uint64_t offset) const {
    if (offset >= data_bytes_) {
        Raise(AllocatorErrorCode::InvalidArgument, "box offset is out of bounds");
    }
    return AllocatedSizeInNode(0, 0, offset);
}

BoxAllocator::RebuildPlan BoxAllocator::MakeRebuildPlan(
    const std::vector<BoxLiveInterval>& live_intervals) const {
    ValidateImmutable();
    RebuildPlan plan;
    plan.intervals.reserve(live_intervals.size());
    for (const auto& interval : live_intervals) {
        const auto shape = ShapeForRequest(interval.size);
        if (interval.offset % shape.unit_bytes != 0) {
            Raise(
                AllocatorErrorCode::InvalidArgument,
                "rebuild interval offset is misaligned");
        }
        const auto end = CheckedAdd(
            interval.offset,
            shape.rounded_bytes,
            AllocatorErrorCode::InvalidArgument,
            "rebuild interval end overflows");
        if (end > data_bytes_) {
            Raise(
                AllocatorErrorCode::InvalidArgument,
                "rebuild interval is out of bounds");
        }
        plan.intervals.push_back(
            NormalizedRebuildInterval{interval.offset, shape});
    }
    std::sort(
        plan.intervals.begin(),
        plan.intervals.end(),
        [](const NormalizedRebuildInterval& left,
           const NormalizedRebuildInterval& right) {
            return left.offset < right.offset;
        });
    std::uint64_t previous_end = 0;
    bool have_previous = false;
    for (const auto& interval : plan.intervals) {
        if (have_previous && interval.offset < previous_end) {
            Raise(AllocatorErrorCode::Overlap, "rebuild intervals overlap");
        }
        previous_end = interval.offset + interval.shape.rounded_bytes;
        have_previous = true;
    }

    std::set<std::pair<std::uint8_t, std::uint64_t>> required_nodes;
    required_nodes.emplace(root_level_, 0);
    for (const auto& interval : plan.intervals) {
        auto level = root_level_;
        std::uint64_t base = 0;
        while (level > interval.shape.level) {
            const auto slot_bytes = UnitBytes(level);
            const auto slot = (interval.offset - base) / slot_bytes;
            base = CheckedAdd(
                base,
                CheckedMultiply(
                    slot,
                    slot_bytes,
                    AllocatorErrorCode::Corrupt,
                    "rebuild path offset overflow"),
                AllocatorErrorCode::Corrupt,
                "rebuild path offset overflow");
            --level;
            required_nodes.emplace(level, base);
        }
        const auto target_slot =
            (interval.offset - base) / interval.shape.unit_bytes;
        const auto target_slots = interval.shape.level == root_level_
            ? static_cast<std::uint64_t>(root_slots_)
            : 16U;
        if (target_slot + interval.shape.multiple > target_slots) {
            Raise(
                AllocatorErrorCode::InvalidArgument,
                "rebuild interval crosses a 16-way node boundary");
        }
    }
    if (required_nodes.size() > blocks_.Capacity()) {
        Raise(
            AllocatorErrorCode::Capacity,
            "box metadata region cannot represent live intervals");
    }
    plan.required_node_count = required_nodes.size();
    return plan;
}

void BoxAllocator::ValidateRebuildIntervals(
    const std::vector<BoxLiveInterval>& live_intervals) const {
    static_cast<void>(MakeRebuildPlan(live_intervals));
}

BoxAllocator::PreparedRebuild BoxAllocator::PrepareRebuild(
    const std::vector<BoxLiveInterval>& live_intervals) && {
    auto plan = MakeRebuildPlan(live_intervals);
    std::vector<std::uint8_t> validation_scratch(
        (plan.required_node_count + 7U) / 8U, 0);
    return PreparedRebuild(
        std::move(*this),
        std::move(plan.intervals),
        plan.required_node_count,
        std::move(validation_scratch));
}

void BoxAllocator::Rebuild(
    const std::vector<BoxLiveInterval>& live_intervals) {
    auto prepared = std::move(*this).PrepareRebuild(live_intervals);
    *this = std::move(prepared).Apply();
}

BoxAllocator::PreparedRebuild::PreparedRebuild(
    BoxAllocator allocator,
    std::vector<NormalizedRebuildInterval> intervals,
    std::size_t required_node_count,
    std::vector<std::uint8_t> validation_scratch) noexcept
    : allocator_(std::move(allocator)),
      intervals_(std::move(intervals)),
      required_node_count_(required_node_count),
      validation_scratch_(std::move(validation_scratch)) {}

BoxAllocator::PreparedRebuild::PreparedRebuild(
    PreparedRebuild&& other) noexcept
    : allocator_(std::move(other.allocator_)),
      intervals_(std::move(other.intervals_)),
      required_node_count_(other.required_node_count_),
      validation_scratch_(std::move(other.validation_scratch_)),
      ready_(other.ready_) {
    other.required_node_count_ = 0;
    other.ready_ = false;
}

BoxAllocator::PreparedRebuild&
BoxAllocator::PreparedRebuild::operator=(
    PreparedRebuild&& other) noexcept {
    if (this == &other) return *this;
    allocator_ = std::move(other.allocator_);
    intervals_ = std::move(other.intervals_);
    required_node_count_ = other.required_node_count_;
    validation_scratch_ = std::move(other.validation_scratch_);
    ready_ = other.ready_;
    other.required_node_count_ = 0;
    other.ready_ = false;
    return *this;
}

BoxAllocator BoxAllocator::PreparedRebuild::Apply() && {
    if (!ready_) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "prepared box rebuild was already consumed");
    }
    if (required_node_count_ == 0 ||
        validation_scratch_.size() <
            (required_node_count_ + 7U) / 8U) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "prepared box rebuild scratch is invalid");
    }

    const std::uint8_t root_live = 1;
    allocator_.blocks_.RebuildWithScratch(
        &root_live,
        1,
        validation_scratch_.data(),
        validation_scratch_.size());

    // Every operation below uses only the normalized vector and fixed scratch
    // owned by this capability. ReserveInNode writes deterministic metadata
    // but performs no heap allocation.
    allocator_.blocks_.recovery_access_ = false;
    allocator_.InitializeNode(
        0,
        FixedBlockAllocator::kInvalidId,
        UINT8_MAX,
        allocator_.root_level_,
        allocator_.root_slots_);
    for (const auto& interval : intervals_) {
        allocator_.ReserveInNode(
            0, 0, interval.offset, interval.shape);
    }
    allocator_.ValidateWithScratch(
        validation_scratch_.data(), validation_scratch_.size());
    ready_ = false;
    return std::move(allocator_);
}

void BoxAllocator::ValidateNode(
    std::uint32_t node_id,
    std::uint32_t expected_parent,
    std::uint8_t expected_parent_slot,
    std::uint8_t expected_level,
    std::uint8_t expected_slots,
    std::uint8_t* reachable,
    std::size_t reachable_bit_count) const {
    if (node_id >= blocks_.Capacity() || !blocks_.IsAllocated(node_id)) {
        Raise(AllocatorErrorCode::Corrupt, "box tree references a free node");
    }
    if (GetBit(reachable, reachable_bit_count, node_id)) {
        Raise(AllocatorErrorCode::Corrupt, "box tree has a cycle or shared node");
    }
    SetBit(reachable, node_id);
    const auto node = ReadNode(node_id);
    if (node.parent != expected_parent ||
        node.parent_slot != expected_parent_slot ||
        node.level != expected_level ||
        node.slot_count != expected_slots ||
        node.slot_count == 0 || node.slot_count > 16) {
        Raise(AllocatorErrorCode::Corrupt, "box node geometry mismatch");
    }
    if (expected_parent != FixedBlockAllocator::kInvalidId &&
        NodeIsEmpty(node)) {
        Raise(AllocatorErrorCode::Corrupt, "box tree contains an empty child node");
    }

    bool continuation_allowed = false;
    std::size_t object_run = 0;
    for (std::size_t index = 0; index < node.states.size(); ++index) {
        if (index >= node.slot_count) {
            if (node.states[index] != SlotState::Unused ||
                node.children[index] != FixedBlockAllocator::kInvalidId) {
                Raise(AllocatorErrorCode::Corrupt, "unused box slot contains data");
            }
            continuation_allowed = false;
            object_run = 0;
            continue;
        }
        switch (node.states[index]) {
        case SlotState::Unused:
            if (node.children[index] != FixedBlockAllocator::kInvalidId) {
                Raise(AllocatorErrorCode::Corrupt, "unused box slot has child ID");
            }
            continuation_allowed = false;
            object_run = 0;
            break;
        case SlotState::Child:
            if (node.level == 0 ||
                node.children[index] == FixedBlockAllocator::kInvalidId) {
                Raise(AllocatorErrorCode::Corrupt, "invalid box child slot");
            }
            ValidateNode(
                node.children[index],
                node_id,
                static_cast<std::uint8_t>(index),
                static_cast<std::uint8_t>(node.level - 1U),
                16,
                reachable,
                reachable_bit_count);
            continuation_allowed = false;
            object_run = 0;
            break;
        case SlotState::ObjectStart:
            if (node.children[index] != FixedBlockAllocator::kInvalidId) {
                Raise(AllocatorErrorCode::Corrupt, "box object slot has child ID");
            }
            continuation_allowed = true;
            object_run = 1;
            break;
        case SlotState::ObjectContinued:
            if (!continuation_allowed ||
                node.children[index] != FixedBlockAllocator::kInvalidId) {
                Raise(AllocatorErrorCode::Corrupt, "orphan box continuation slot");
            }
            ++object_run;
            if (object_run > 15) {
                Raise(
                    AllocatorErrorCode::Corrupt,
                    "box object uses more than 15 slots at one level");
            }
            break;
        }
    }
}

void BoxAllocator::Validate() const {
    ValidateImmutable();
    const auto high_water = blocks_.HighWater();
    if (high_water > blocks_.Capacity()) {
        Raise(AllocatorErrorCode::Corrupt, "invalid box metadata high water");
    }
    std::vector<std::uint8_t> scratch(
        (static_cast<std::size_t>(high_water) + 7U) / 8U, 0);
    ValidateWithScratch(scratch.data(), scratch.size());
}

void BoxAllocator::ValidateWithScratch(
    std::uint8_t* scratch,
    std::size_t scratch_bytes) const {
    ValidateImmutable();
    const auto high_water = blocks_.HighWater();
    const auto required_scratch =
        (static_cast<std::size_t>(high_water) + 7U) / 8U;
    if (high_water > blocks_.Capacity() || required_scratch > scratch_bytes ||
        (required_scratch != 0 && scratch == nullptr)) {
        Raise(
            AllocatorErrorCode::InvalidArgument,
            "box validation scratch is too small");
    }
    blocks_.ValidateWithScratch(scratch, scratch_bytes);
    if (required_scratch != 0) {
        std::memset(scratch, 0, required_scratch);
    }
    ValidateNode(
        0,
        FixedBlockAllocator::kInvalidId,
        UINT8_MAX,
        root_level_,
        root_slots_,
        scratch,
        high_water);
    for (std::uint32_t id = 0; id < high_water; ++id) {
        if (blocks_.IsAllocated(id) != GetBit(scratch, high_water, id)) {
            Raise(
                AllocatorErrorCode::Corrupt,
                "box metadata block is unreachable or unexpectedly free");
        }
    }
}

} // namespace kvspace::detail
