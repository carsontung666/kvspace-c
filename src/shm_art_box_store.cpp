#include "shm_art_box_store.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace kvspace::detail {
namespace {

constexpr std::size_t kPrefixRefOffset = 0;
constexpr std::size_t kValueRefOffset = 8;
constexpr std::size_t kPrefixLengthOffset = 16;
constexpr std::size_t kChildCountOffset = 20;
constexpr std::size_t kKindOffset = 22;
constexpr std::size_t kFlagsOffset = 23;
constexpr std::size_t kNodeBodyOffset = 24;
constexpr std::uint8_t kHasValueFlag = 1;
constexpr std::uint32_t kLocalIdMask = UINT32_C(0x3fffffff);

[[noreturn]] void RaiseArtBoxStore(
    AllocatorErrorCode code,
    const std::string& message) {
    throw AllocatorError(code, "kvspace ArtBox node store: " + message);
}

bool IsValidKind(ArtBoxNodeKind kind) noexcept {
    switch (kind) {
    case ArtBoxNodeKind::Node4:
    case ArtBoxNodeKind::Node16:
    case ArtBoxNodeKind::Node48:
    case ArtBoxNodeKind::Node256:
        return true;
    }
    return false;
}

std::size_t KindIndex(
    ArtBoxNodeKind kind,
    AllocatorErrorCode failure_code) {
    if (!IsValidKind(kind)) {
        RaiseArtBoxStore(failure_code, "invalid ART node kind");
    }
    return static_cast<std::size_t>(kind) - 1U;
}

ArtBoxNodeKind KindFromByte(
    std::uint8_t raw,
    AllocatorErrorCode failure_code) {
    const auto kind = static_cast<ArtBoxNodeKind>(raw);
    static_cast<void>(KindIndex(kind, failure_code));
    return kind;
}

struct DecodedReference {
    ArtBoxNodeKind kind;
    std::uint32_t local_id;
};

DecodedReference DecodeReference(
    std::uint32_t reference,
    AllocatorErrorCode failure_code) {
    if (reference == ArtBoxNodeRefCodec::kEmpty) {
        RaiseArtBoxStore(failure_code, "empty node reference has no target");
    }
    const auto local_id = reference & kLocalIdMask;
    if (local_id > ArtBoxNodeRefCodec::kMaximumLocalId) {
        RaiseArtBoxStore(failure_code, "node reference local ID is reserved");
    }
    const auto tag = static_cast<std::uint8_t>(reference >> 30U);
    return DecodedReference{
        KindFromByte(static_cast<std::uint8_t>(tag + 1U), failure_code),
        local_id};
}

std::uint16_t Load16(const std::uint8_t* data) noexcept {
    return static_cast<std::uint16_t>(data[0]) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(data[1]) << 8U);
}

std::uint32_t Load32(const std::uint8_t* data) noexcept {
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < sizeof(result); ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        result |= static_cast<std::uint32_t>(data[index]) << shift;
    }
    return result;
}

std::uint64_t Load64(const std::uint8_t* data) noexcept {
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < sizeof(result); ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        result |= static_cast<std::uint64_t>(data[index]) << shift;
    }
    return result;
}

void Store16(std::uint8_t* data, std::uint16_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        data[index] = static_cast<std::uint8_t>(value >> shift);
    }
}

void Store32(std::uint8_t* data, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        data[index] = static_cast<std::uint8_t>(value >> shift);
    }
}

void Store64(std::uint8_t* data, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        data[index] = static_cast<std::uint8_t>(value >> shift);
    }
}

bool AllBytesEqual(
    const std::uint8_t* begin,
    const std::uint8_t* end,
    std::uint8_t expected) noexcept {
    return std::all_of(
        begin, end,
        [expected](std::uint8_t value) { return value == expected; });
}

void ValidateChildSyntax(
    std::uint32_t child,
    AllocatorErrorCode failure_code) {
    if (child == ArtBoxNodeRecord::kEmptyChild) return;
    static_cast<void>(DecodeReference(child, failure_code));
}

void ValidateCanonicalRecord(
    const ArtBoxNodeRecord& record,
    AllocatorErrorCode failure_code) {
    static_cast<void>(KindIndex(record.kind, failure_code));
    if ((record.prefix_len == 0) != (record.prefix_ref == 0)) {
        RaiseArtBoxStore(
            failure_code,
            "prefix length/reference pair is not canonical");
    }
    if (!record.has_value && record.value_ref != 0) {
        RaiseArtBoxStore(
            failure_code,
            "missing value must use a zero reference");
    }

    const auto require_default_index = [&] {
        if (!AllBytesEqual(
                record.index.data(),
                record.index.data() + record.index.size(),
                UINT8_MAX)) {
            RaiseArtBoxStore(
                failure_code,
                "node kind contains noncanonical unused index bytes");
        }
    };
    const auto require_default_keys = [&] {
        if (!AllBytesEqual(
                record.keys.data(),
                record.keys.data() + record.keys.size(),
                0)) {
            RaiseArtBoxStore(
                failure_code,
                "node kind contains noncanonical unused key bytes");
        }
    };

    switch (record.kind) {
    case ArtBoxNodeKind::Node4:
    case ArtBoxNodeKind::Node16: {
        const auto capacity = record.kind == ArtBoxNodeKind::Node4
            ? std::size_t{4}
            : std::size_t{16};
        if (record.child_count > capacity) {
            RaiseArtBoxStore(failure_code, "node child count exceeds capacity");
        }
        require_default_index();
        for (std::size_t slot = 0; slot < capacity; ++slot) {
            if (slot < record.child_count) {
                ValidateChildSyntax(record.children[slot], failure_code);
                if (record.children[slot] == ArtBoxNodeRecord::kEmptyChild) {
                    RaiseArtBoxStore(
                        failure_code, "used child slot is empty");
                }
                if (slot != 0 &&
                    record.keys[slot - 1U] >= record.keys[slot]) {
                    RaiseArtBoxStore(
                        failure_code, "Node4/16 keys are not strictly sorted");
                }
            } else if (record.keys[slot] != 0 ||
                       record.children[slot] !=
                           ArtBoxNodeRecord::kEmptyChild) {
                RaiseArtBoxStore(
                    failure_code, "unused Node4/16 slot is not canonical");
            }
        }
        for (std::size_t slot = capacity; slot < record.keys.size(); ++slot) {
            if (record.keys[slot] != 0) {
                RaiseArtBoxStore(
                    failure_code, "out-of-class key bytes are not zero");
            }
        }
        for (std::size_t slot = capacity;
             slot < record.children.size();
             ++slot) {
            if (record.children[slot] != ArtBoxNodeRecord::kEmptyChild) {
                RaiseArtBoxStore(
                    failure_code, "out-of-class child slot is not empty");
            }
        }
        break;
    }
    case ArtBoxNodeKind::Node48: {
        require_default_keys();
        if (record.child_count > 48U) {
            RaiseArtBoxStore(failure_code, "Node48 child count exceeds capacity");
        }
        std::array<bool, 48> referenced{};
        std::size_t indexed_children = 0;
        for (const auto slot : record.index) {
            if (slot == UINT8_MAX) continue;
            if (slot >= record.child_count) {
                RaiseArtBoxStore(
                    failure_code,
                    "Node48 index is outside its dense child slots");
            }
            if (referenced[slot]) {
                RaiseArtBoxStore(
                    failure_code, "Node48 index is not a bijection");
            }
            referenced[slot] = true;
            ++indexed_children;
        }
        if (indexed_children != record.child_count) {
            RaiseArtBoxStore(
                failure_code, "Node48 index count disagrees with header");
        }
        for (std::size_t slot = 0; slot < referenced.size(); ++slot) {
            const auto child = record.children[slot];
            if (slot < record.child_count) {
                if (!referenced[slot]) {
                    RaiseArtBoxStore(
                        failure_code,
                        "Node48 dense child slot is not indexed");
                }
                if (child == ArtBoxNodeRecord::kEmptyChild) {
                    RaiseArtBoxStore(
                        failure_code, "Node48 index names an empty slot");
                }
                ValidateChildSyntax(child, failure_code);
            } else if (referenced[slot] ||
                       child != ArtBoxNodeRecord::kEmptyChild) {
                RaiseArtBoxStore(
                    failure_code, "Node48 unindexed slot is not empty");
            }
        }
        for (std::size_t slot = referenced.size();
             slot < record.children.size();
             ++slot) {
            if (record.children[slot] != ArtBoxNodeRecord::kEmptyChild) {
                RaiseArtBoxStore(
                    failure_code, "Node48 unused child bytes are not empty");
            }
        }
        break;
    }
    case ArtBoxNodeKind::Node256: {
        require_default_keys();
        require_default_index();
        std::size_t actual_children = 0;
        for (const auto child : record.children) {
            if (child == ArtBoxNodeRecord::kEmptyChild) continue;
            ValidateChildSyntax(child, failure_code);
            ++actual_children;
        }
        if (actual_children != record.child_count) {
            RaiseArtBoxStore(
                failure_code, "Node256 child count disagrees with payload");
        }
        break;
    }
    }
}

struct StoreGeometry {
    std::size_t header_bytes;
};

StoreGeometry PreflightRegions(
    const ArtBoxNodeStoreRegions& regions,
    std::uint32_t node_capacity) {
    if (node_capacity == 0 ||
        node_capacity > ArtBoxNodeRefCodec::kMaximumCapacity) {
        RaiseArtBoxStore(
            AllocatorErrorCode::InvalidArgument,
            "node capacity exceeds the tagged local-ID space");
    }
    const auto width = FixedBlockAllocator::MinimumHeaderWidth(node_capacity);
    const auto header_bytes = static_cast<std::size_t>(width);
    StoreGeometry result{header_bytes};
    for (std::size_t index = 0; index < regions.size(); ++index) {
        const auto& region = regions[index];
        if (region.metadata == nullptr || region.node_zone == nullptr ||
            region.metadata_bytes !=
                FixedBlockAllocator::kPersistentMetadataBytes) {
            RaiseArtBoxStore(
                AllocatorErrorCode::InvalidArgument,
                "invalid fixed-block region");
        }
        const auto payload = ArtBoxNodeCodec::kPayloadBytes[index];
        if (payload > std::numeric_limits<std::size_t>::max() - header_bytes) {
            RaiseArtBoxStore(
                AllocatorErrorCode::InvalidArgument,
                "node stride overflows address space");
        }
        const auto stride = payload + header_bytes;
        if (node_capacity >
            std::numeric_limits<std::size_t>::max() / stride) {
            RaiseArtBoxStore(
                AllocatorErrorCode::InvalidArgument,
                "node zone size overflows address space");
        }
        const auto exact_bytes =
            static_cast<std::size_t>(node_capacity) * stride;
        if (region.node_zone_bytes != exact_bytes) {
            RaiseArtBoxStore(
                AllocatorErrorCode::InvalidArgument,
                "node zone is not the exact capacity geometry");
        }
    }
    return result;
}

bool BitmapBit(
    const std::vector<std::uint8_t>& bitmap,
    std::uint32_t id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    const auto shift = static_cast<unsigned>(index % 8U);
    return (bitmap[index / 8U] &
            static_cast<std::uint8_t>(1U << shift)) != 0;
}

void SetBitmapBit(
    std::vector<std::uint8_t>* bitmap,
    std::uint32_t id) noexcept {
    const auto index = static_cast<std::size_t>(id);
    const auto shift = static_cast<unsigned>(index % 8U);
    (*bitmap)[index / 8U] |= static_cast<std::uint8_t>(1U << shift);
}

void ValidateLiveBitmap(
    const std::vector<std::uint8_t>& bitmap,
    std::size_t bit_count,
    std::uint32_t capacity) {
    if (bit_count > capacity) {
        RaiseArtBoxStore(
            AllocatorErrorCode::InvalidArgument,
            "live bitmap count exceeds allocator capacity");
    }
    const auto required_bytes = (bit_count + 7U) / 8U;
    if (bitmap.size() < required_bytes) {
        RaiseArtBoxStore(
            AllocatorErrorCode::InvalidArgument,
            "live bitmap storage is too small");
    }
    if (required_bytes != 0 && bit_count % 8U != 0) {
        const auto valid_bits = static_cast<unsigned>(bit_count % 8U);
        const auto invalid_mask = static_cast<std::uint8_t>(
            static_cast<unsigned>(UINT8_MAX) << valid_bits);
        if ((bitmap[required_bytes - 1U] & invalid_mask) != 0) {
            RaiseArtBoxStore(
                AllocatorErrorCode::InvalidArgument,
                "live bitmap has set bits beyond its count");
        }
    }
    for (std::size_t index = required_bytes;
         index < bitmap.size();
         ++index) {
        if (bitmap[index] != 0) {
            RaiseArtBoxStore(
                AllocatorErrorCode::InvalidArgument,
                "live bitmap has hidden set bits beyond its count");
        }
    }
}

} // namespace

ArtBoxNodeRecord::ArtBoxNodeRecord() noexcept {
    index.fill(UINT8_MAX);
    children.fill(kEmptyChild);
}

std::uint32_t ArtBoxNodeRefCodec::Encode(
    ArtBoxNodeKind kind,
    std::uint32_t local_id) {
    const auto kind_index = KindIndex(
        kind, AllocatorErrorCode::InvalidArgument);
    if (local_id > kMaximumLocalId) {
        RaiseArtBoxStore(
            AllocatorErrorCode::InvalidArgument,
            "local node ID exceeds the tagged-reference limit");
    }
    return (static_cast<std::uint32_t>(kind_index) << 30U) | local_id;
}

ArtBoxNodeKind ArtBoxNodeRefCodec::Kind(std::uint32_t reference) {
    return DecodeReference(
        reference, AllocatorErrorCode::InvalidArgument).kind;
}

std::uint32_t ArtBoxNodeRefCodec::LocalId(std::uint32_t reference) {
    return DecodeReference(
        reference, AllocatorErrorCode::InvalidArgument).local_id;
}

std::size_t ArtBoxNodeCodec::PayloadBytes(ArtBoxNodeKind kind) {
    return kPayloadBytes[KindIndex(kind, AllocatorErrorCode::InvalidArgument)];
}

std::vector<std::uint8_t> ArtBoxNodeCodec::Encode(
    const ArtBoxNodeRecord& record) {
    ValidateCanonicalRecord(record, AllocatorErrorCode::InvalidArgument);
    std::vector<std::uint8_t> encoded(PayloadBytes(record.kind), 0);
    Store64(encoded.data() + kPrefixRefOffset, record.prefix_ref);
    Store64(encoded.data() + kValueRefOffset, record.value_ref);
    Store32(encoded.data() + kPrefixLengthOffset, record.prefix_len);
    Store16(encoded.data() + kChildCountOffset, record.child_count);
    encoded[kKindOffset] = static_cast<std::uint8_t>(record.kind);
    encoded[kFlagsOffset] = record.has_value ? kHasValueFlag : 0;

    switch (record.kind) {
    case ArtBoxNodeKind::Node4:
        std::copy_n(record.keys.begin(), 4, encoded.begin() + kNodeBodyOffset);
        for (std::size_t slot = 0; slot < 4; ++slot) {
            Store32(encoded.data() + 28U + slot * 4U, record.children[slot]);
        }
        break;
    case ArtBoxNodeKind::Node16:
        std::copy(
            record.keys.begin(),
            record.keys.end(),
            encoded.begin() + kNodeBodyOffset);
        for (std::size_t slot = 0; slot < 16; ++slot) {
            Store32(encoded.data() + 40U + slot * 4U, record.children[slot]);
        }
        break;
    case ArtBoxNodeKind::Node48:
        std::copy(
            record.index.begin(),
            record.index.end(),
            encoded.begin() + kNodeBodyOffset);
        for (std::size_t slot = 0; slot < 48; ++slot) {
            Store32(encoded.data() + 280U + slot * 4U, record.children[slot]);
        }
        break;
    case ArtBoxNodeKind::Node256:
        for (std::size_t slot = 0; slot < 256; ++slot) {
            Store32(encoded.data() + 24U + slot * 4U, record.children[slot]);
        }
        break;
    }
    return encoded;
}

ArtBoxNodeRecord ArtBoxNodeCodec::Decode(
    const void* encoded,
    std::size_t encoded_bytes) {
    if (encoded == nullptr ||
        std::find(kPayloadBytes.begin(), kPayloadBytes.end(), encoded_bytes) ==
            kPayloadBytes.end()) {
        RaiseArtBoxStore(
            AllocatorErrorCode::InvalidArgument,
            "codec input has no exact ArtBox payload size");
    }
    const auto* bytes = static_cast<const std::uint8_t*>(encoded);
    const auto kind = KindFromByte(
        bytes[kKindOffset], AllocatorErrorCode::Corrupt);
    if (kPayloadBytes[KindIndex(kind, AllocatorErrorCode::Corrupt)] !=
        encoded_bytes) {
        RaiseArtBoxStore(
            AllocatorErrorCode::Corrupt,
            "node kind does not match its payload class");
    }
    if ((bytes[kFlagsOffset] &
         static_cast<std::uint8_t>(~kHasValueFlag)) != 0) {
        RaiseArtBoxStore(
            AllocatorErrorCode::Corrupt, "node has unknown flag bits");
    }

    ArtBoxNodeRecord record;
    record.prefix_ref = Load64(bytes + kPrefixRefOffset);
    record.value_ref = Load64(bytes + kValueRefOffset);
    record.prefix_len = Load32(bytes + kPrefixLengthOffset);
    record.child_count = Load16(bytes + kChildCountOffset);
    record.kind = kind;
    record.has_value = (bytes[kFlagsOffset] & kHasValueFlag) != 0;

    switch (kind) {
    case ArtBoxNodeKind::Node4:
        std::copy_n(bytes + kNodeBodyOffset, 4, record.keys.begin());
        for (std::size_t slot = 0; slot < 4; ++slot) {
            record.children[slot] = Load32(bytes + 28U + slot * 4U);
        }
        if (!AllBytesEqual(bytes + 44U, bytes + 48U, 0)) {
            RaiseArtBoxStore(
                AllocatorErrorCode::Corrupt,
                "Node4 padding bytes are not zero");
        }
        break;
    case ArtBoxNodeKind::Node16:
        std::copy_n(bytes + kNodeBodyOffset, 16, record.keys.begin());
        for (std::size_t slot = 0; slot < 16; ++slot) {
            record.children[slot] = Load32(bytes + 40U + slot * 4U);
        }
        break;
    case ArtBoxNodeKind::Node48:
        std::copy_n(bytes + kNodeBodyOffset, 256, record.index.begin());
        for (std::size_t slot = 0; slot < 48; ++slot) {
            record.children[slot] = Load32(bytes + 280U + slot * 4U);
        }
        break;
    case ArtBoxNodeKind::Node256:
        for (std::size_t slot = 0; slot < 256; ++slot) {
            record.children[slot] = Load32(bytes + 24U + slot * 4U);
        }
        break;
    }
    ValidateCanonicalRecord(record, AllocatorErrorCode::Corrupt);
    return record;
}

ArtBoxNodeStore::ArtBoxNodeStore(
    FixedBlockAllocator node4,
    FixedBlockAllocator node16,
    FixedBlockAllocator node48,
    FixedBlockAllocator node256,
    std::uint32_t node_capacity,
    std::size_t header_bytes,
    bool recovery_access) noexcept
    : node4_(std::move(node4)),
      node16_(std::move(node16)),
      node48_(std::move(node48)),
      node256_(std::move(node256)),
      node_capacity_(node_capacity),
      header_bytes_(header_bytes),
      recovery_access_(recovery_access) {}

ArtBoxNodeStore::ArtBoxNodeStore(ArtBoxNodeStore&& other) noexcept
    : node4_(std::move(other.node4_)),
      node16_(std::move(other.node16_)),
      node48_(std::move(other.node48_)),
      node256_(std::move(other.node256_)),
      node_capacity_(other.node_capacity_),
      header_bytes_(other.header_bytes_),
      recovery_access_(other.recovery_access_) {
    other.node_capacity_ = 0;
    other.header_bytes_ = 0;
    other.recovery_access_ = false;
}

ArtBoxNodeStore& ArtBoxNodeStore::operator=(ArtBoxNodeStore&& other) noexcept {
    if (this == &other) return *this;
    node4_ = std::move(other.node4_);
    node16_ = std::move(other.node16_);
    node48_ = std::move(other.node48_);
    node256_ = std::move(other.node256_);
    node_capacity_ = other.node_capacity_;
    header_bytes_ = other.header_bytes_;
    recovery_access_ = other.recovery_access_;
    other.node_capacity_ = 0;
    other.header_bytes_ = 0;
    other.recovery_access_ = false;
    return *this;
}

ArtBoxNodeStore ArtBoxNodeStore::Initialize(
    const ArtBoxNodeStoreRegions& regions,
    std::uint32_t node_capacity) {
    const auto geometry = PreflightRegions(regions, node_capacity);
    const auto width = static_cast<FixedBlockHeaderWidth>(geometry.header_bytes);
    ArtBoxNodeStore store(
        FixedBlockAllocator::Initialize(
            regions[0].metadata,
            regions[0].metadata_bytes,
            regions[0].node_zone,
            regions[0].node_zone_bytes,
            ArtBoxNodeCodec::kPayloadBytes[0],
            width),
        FixedBlockAllocator::Initialize(
            regions[1].metadata,
            regions[1].metadata_bytes,
            regions[1].node_zone,
            regions[1].node_zone_bytes,
            ArtBoxNodeCodec::kPayloadBytes[1],
            width),
        FixedBlockAllocator::Initialize(
            regions[2].metadata,
            regions[2].metadata_bytes,
            regions[2].node_zone,
            regions[2].node_zone_bytes,
            ArtBoxNodeCodec::kPayloadBytes[2],
            width),
        FixedBlockAllocator::Initialize(
            regions[3].metadata,
            regions[3].metadata_bytes,
            regions[3].node_zone,
            regions[3].node_zone_bytes,
            ArtBoxNodeCodec::kPayloadBytes[3],
            width),
        node_capacity,
        geometry.header_bytes,
        false);
    store.ValidateGeometry(AllocatorErrorCode::InvalidArgument);
    return store;
}

ArtBoxNodeStore ArtBoxNodeStore::Attach(
    const ArtBoxNodeStoreRegions& regions,
    std::uint32_t node_capacity) {
    const auto geometry = PreflightRegions(regions, node_capacity);
    ArtBoxNodeStore store(
        FixedBlockAllocator::Attach(
            regions[0].metadata,
            regions[0].metadata_bytes,
            regions[0].node_zone,
            regions[0].node_zone_bytes),
        FixedBlockAllocator::Attach(
            regions[1].metadata,
            regions[1].metadata_bytes,
            regions[1].node_zone,
            regions[1].node_zone_bytes),
        FixedBlockAllocator::Attach(
            regions[2].metadata,
            regions[2].metadata_bytes,
            regions[2].node_zone,
            regions[2].node_zone_bytes),
        FixedBlockAllocator::Attach(
            regions[3].metadata,
            regions[3].metadata_bytes,
            regions[3].node_zone,
            regions[3].node_zone_bytes),
        node_capacity,
        geometry.header_bytes,
        false);
    store.ValidateGeometry(AllocatorErrorCode::Corrupt);
    store.Validate();
    return store;
}

ArtBoxNodeStore ArtBoxNodeStore::AttachForRecovery(
    const ArtBoxNodeStoreRegions& regions,
    std::uint32_t node_capacity) {
    const auto geometry = PreflightRegions(regions, node_capacity);
    ArtBoxNodeStore store(
        FixedBlockAllocator::AttachForRecovery(
            regions[0].metadata,
            regions[0].metadata_bytes,
            regions[0].node_zone,
            regions[0].node_zone_bytes),
        FixedBlockAllocator::AttachForRecovery(
            regions[1].metadata,
            regions[1].metadata_bytes,
            regions[1].node_zone,
            regions[1].node_zone_bytes),
        FixedBlockAllocator::AttachForRecovery(
            regions[2].metadata,
            regions[2].metadata_bytes,
            regions[2].node_zone,
            regions[2].node_zone_bytes),
        FixedBlockAllocator::AttachForRecovery(
            regions[3].metadata,
            regions[3].metadata_bytes,
            regions[3].node_zone,
            regions[3].node_zone_bytes),
        node_capacity,
        geometry.header_bytes,
        true);
    store.ValidateGeometry(AllocatorErrorCode::Corrupt);
    return store;
}

FixedBlockAllocator& ArtBoxNodeStore::Blocks(ArtBoxNodeKind kind) {
    switch (kind) {
    case ArtBoxNodeKind::Node4:
        return node4_;
    case ArtBoxNodeKind::Node16:
        return node16_;
    case ArtBoxNodeKind::Node48:
        return node48_;
    case ArtBoxNodeKind::Node256:
        return node256_;
    }
    RaiseArtBoxStore(AllocatorErrorCode::InvalidArgument, "invalid node kind");
}

const FixedBlockAllocator& ArtBoxNodeStore::Blocks(
    ArtBoxNodeKind kind) const {
    switch (kind) {
    case ArtBoxNodeKind::Node4:
        return node4_;
    case ArtBoxNodeKind::Node16:
        return node16_;
    case ArtBoxNodeKind::Node48:
        return node48_;
    case ArtBoxNodeKind::Node256:
        return node256_;
    }
    RaiseArtBoxStore(AllocatorErrorCode::InvalidArgument, "invalid node kind");
}

void ArtBoxNodeStore::ValidateGeometry(
    AllocatorErrorCode failure_code) const {
    if (node_capacity_ == 0 ||
        node_capacity_ > ArtBoxNodeRefCodec::kMaximumCapacity ||
        header_bytes_ != static_cast<std::size_t>(
            FixedBlockAllocator::MinimumHeaderWidth(node_capacity_))) {
        RaiseArtBoxStore(failure_code, "invalid ArtBox store geometry");
    }
    for (std::size_t index = 0;
         index < ArtBoxNodeCodec::kPayloadBytes.size();
         ++index) {
        const auto kind = static_cast<ArtBoxNodeKind>(index + 1U);
        const auto& blocks = Blocks(kind);
        if (blocks.Capacity() != node_capacity_ ||
            blocks.HeaderBytes() != header_bytes_ ||
            blocks.PayloadBytes() != ArtBoxNodeCodec::kPayloadBytes[index]) {
            RaiseArtBoxStore(
                failure_code, "fixed block allocator geometry mismatch");
        }
    }
}

void ArtBoxNodeStore::ValidateRecordReferences(
    const ArtBoxNodeRecord& record,
    bool require_allocated_children,
    AllocatorErrorCode failure_code) const {
    ValidateCanonicalRecord(record, failure_code);
    for (const auto child : record.children) {
        if (child == ArtBoxNodeRecord::kEmptyChild) continue;
        const auto decoded = DecodeReference(child, failure_code);
        if (decoded.local_id >= node_capacity_) {
            RaiseArtBoxStore(
                failure_code, "child reference exceeds allocator capacity");
        }
        if (require_allocated_children &&
            !Blocks(decoded.kind).IsAllocated(decoded.local_id)) {
            RaiseArtBoxStore(
                failure_code, "child reference is not allocated");
        }
    }
}

std::uint32_t ArtBoxNodeStore::Allocate(
    const ArtBoxNodeRecord& record) {
    const auto encoded = ArtBoxNodeCodec::Encode(record);
    ValidateRecordReferences(
        record, true, AllocatorErrorCode::InvalidArgument);
    auto& blocks = Blocks(record.kind);
    const auto local_id = blocks.Allocate();
    std::memcpy(blocks.BlockData(local_id), encoded.data(), encoded.size());
    return ArtBoxNodeRefCodec::Encode(record.kind, local_id);
}

std::uint32_t ArtBoxNodeStore::Clone(std::uint32_t source_ref) {
    return Allocate(Read(source_ref));
}

void ArtBoxNodeStore::DiscardUnpublished(std::uint32_t reference) {
    const auto decoded = DecodeReference(
        reference, AllocatorErrorCode::InvalidId);
    Blocks(decoded.kind).Free(decoded.local_id);
}

void ArtBoxNodeStore::ReclaimPublished(std::uint32_t reference) {
    const auto decoded = DecodeReference(
        reference, AllocatorErrorCode::InvalidId);
    Blocks(decoded.kind).Free(decoded.local_id);
}

void ArtBoxNodeStore::ReclaimPublished(
    const std::vector<std::uint32_t>& references) {
    for (const auto reference : references) ReclaimPublished(reference);
}

ArtBoxNodeRecord ArtBoxNodeStore::Read(std::uint32_t reference) const {
    const auto decoded = DecodeReference(
        reference, AllocatorErrorCode::InvalidId);
    const auto& blocks = Blocks(decoded.kind);
    const auto record = ArtBoxNodeCodec::Decode(
        blocks.BlockData(decoded.local_id), blocks.PayloadBytes());
    if (record.kind != decoded.kind) {
        RaiseArtBoxStore(
            AllocatorErrorCode::Corrupt, "tag and node kind disagree");
    }
    ValidateRecordReferences(record, true, AllocatorErrorCode::Corrupt);
    return record;
}

ArtBoxNodeRecord ArtBoxNodeStore::ReadForRecovery(
    std::uint32_t reference) const {
    if (!recovery_access_) {
        RaiseArtBoxStore(
            AllocatorErrorCode::InvalidArgument,
            "recovery read requires recovery attach");
    }
    const auto decoded = DecodeReference(
        reference, AllocatorErrorCode::InvalidId);
    if (decoded.local_id >= node_capacity_) {
        RaiseArtBoxStore(
            AllocatorErrorCode::InvalidId,
            "recovery node reference exceeds allocator capacity");
    }
    const auto& blocks = Blocks(decoded.kind);
    const auto record = ArtBoxNodeCodec::Decode(
        blocks.RecoveryBlockData(decoded.local_id), blocks.PayloadBytes());
    if (record.kind != decoded.kind) {
        RaiseArtBoxStore(
            AllocatorErrorCode::Corrupt, "tag and node kind disagree");
    }
    ValidateRecordReferences(record, false, AllocatorErrorCode::Corrupt);
    return record;
}

ArtBoxNodeStore::PreparedRecovery ArtBoxNodeStore::PrepareRecovery(
    ArtBoxNodeLiveBitmaps live_bitmaps,
    ArtBoxNodeLiveBitCounts live_bit_counts) && {
    if (!recovery_access_) {
        RaiseArtBoxStore(
            AllocatorErrorCode::InvalidArgument,
            "allocator recovery preparation requires recovery attach");
    }
    for (std::size_t index = 0; index < live_bitmaps.size(); ++index) {
        ValidateLiveBitmap(
            live_bitmaps[index], live_bit_counts[index], node_capacity_);
    }

    // FixedBlockAllocator's recovery implementation is idempotent. Complete
    // every read-only sizing check and every scratch allocation before the
    // first allocation-free apply changes persistent bytes.
    std::array<std::size_t, 4> scratch_bytes{};
    for (std::size_t index = 0; index < scratch_bytes.size(); ++index) {
        const auto kind = static_cast<ArtBoxNodeKind>(index + 1U);
        scratch_bytes[index] = Blocks(kind).RebuildScratchBytes(
            live_bitmaps[index].data(), live_bit_counts[index]);
    }
    std::array<std::vector<std::uint8_t>, 4> scratch;
    for (std::size_t index = 0; index < scratch.size(); ++index) {
        scratch[index].resize(scratch_bytes[index]);
    }
    return PreparedRecovery(
        std::move(*this),
        std::move(live_bitmaps),
        live_bit_counts,
        std::move(scratch));
}

ArtBoxNodeStore ArtBoxNodeStore::Rebuild(
    const ArtBoxNodeLiveBitmaps& live_bitmaps,
    const ArtBoxNodeLiveBitCounts& live_bit_counts) && {
    auto prepared = std::move(*this).PrepareRecovery(
        live_bitmaps, live_bit_counts);
    return std::move(prepared).Apply();
}

ArtBoxNodeStore::PreparedRecovery::PreparedRecovery(
    ArtBoxNodeStore store,
    ArtBoxNodeLiveBitmaps live_bitmaps,
    ArtBoxNodeLiveBitCounts live_bit_counts,
    std::array<std::vector<std::uint8_t>, 4> scratch) noexcept
    : store_(std::move(store)),
      live_bitmaps_(std::move(live_bitmaps)),
      live_bit_counts_(live_bit_counts),
      scratch_(std::move(scratch)) {}

ArtBoxNodeStore::PreparedRecovery::PreparedRecovery(
    PreparedRecovery&& other) noexcept
    : store_(std::move(other.store_)),
      live_bitmaps_(std::move(other.live_bitmaps_)),
      live_bit_counts_(other.live_bit_counts_),
      scratch_(std::move(other.scratch_)),
      ready_(other.ready_) {
    other.live_bit_counts_.fill(0);
    other.ready_ = false;
}

ArtBoxNodeStore::PreparedRecovery&
ArtBoxNodeStore::PreparedRecovery::operator=(
    PreparedRecovery&& other) noexcept {
    if (this == &other) return *this;
    store_ = std::move(other.store_);
    live_bitmaps_ = std::move(other.live_bitmaps_);
    live_bit_counts_ = other.live_bit_counts_;
    scratch_ = std::move(other.scratch_);
    ready_ = other.ready_;
    other.live_bit_counts_.fill(0);
    other.ready_ = false;
    return *this;
}

ArtBoxNodeStore ArtBoxNodeStore::PreparedRecovery::Apply() && {
    if (!ready_) {
        RaiseArtBoxStore(
            AllocatorErrorCode::InvalidArgument,
            "prepared recovery was already consumed");
    }
    for (std::size_t index = 0; index < live_bitmaps_.size(); ++index) {
        const auto kind = static_cast<ArtBoxNodeKind>(index + 1U);
        auto& blocks = store_.Blocks(kind);
        blocks.RebuildPrepared(
            live_bitmaps_[index].data(),
            live_bit_counts_[index],
            scratch_[index].data(),
            scratch_[index].size());
    }
    store_.recovery_access_ = false;
    ready_ = false;
    return std::move(store_);
}

ArtBoxNodeStore ArtBoxNodeStore::RecoverFromCommittedRoot(
    std::uint32_t committed_root) && {
    if (!recovery_access_) {
        RaiseArtBoxStore(
            AllocatorErrorCode::InvalidArgument,
            "committed-root recovery requires recovery attach");
    }
    ArtBoxNodeLiveBitmaps live_bitmaps;
    const auto bitmap_bytes =
        (static_cast<std::size_t>(node_capacity_) + 7U) / 8U;
    for (auto& bitmap : live_bitmaps) bitmap.resize(bitmap_bytes, 0);
    ArtBoxNodeLiveBitCounts live_bit_counts{};
    std::vector<std::uint32_t> stack;

    if (committed_root != ArtBoxNodeRefCodec::kEmpty) {
        const auto decoded = DecodeReference(
            committed_root, AllocatorErrorCode::Corrupt);
        if (decoded.local_id >= node_capacity_) {
            RaiseArtBoxStore(
                AllocatorErrorCode::Corrupt,
                "committed root exceeds allocator capacity");
        }
        const auto kind_index = KindIndex(
            decoded.kind, AllocatorErrorCode::Corrupt);
        SetBitmapBit(&live_bitmaps[kind_index], decoded.local_id);
        live_bit_counts[kind_index] =
            static_cast<std::size_t>(decoded.local_id) + 1U;
        stack.push_back(committed_root);
    }

    while (!stack.empty()) {
        const auto reference = stack.back();
        stack.pop_back();
        const auto record = ReadForRecovery(reference);
        for (const auto child : record.children) {
            if (child == ArtBoxNodeRecord::kEmptyChild) continue;
            const auto decoded = DecodeReference(
                child, AllocatorErrorCode::Corrupt);
            const auto kind_index = KindIndex(
                decoded.kind, AllocatorErrorCode::Corrupt);
            if (BitmapBit(live_bitmaps[kind_index], decoded.local_id)) {
                RaiseArtBoxStore(
                    AllocatorErrorCode::Corrupt,
                    "committed ART contains a cycle or shared node");
            }
            SetBitmapBit(&live_bitmaps[kind_index], decoded.local_id);
            live_bit_counts[kind_index] = std::max(
                live_bit_counts[kind_index],
                static_cast<std::size_t>(decoded.local_id) + 1U);
            stack.push_back(child);
        }
    }
    auto prepared = std::move(*this).PrepareRecovery(
        std::move(live_bitmaps), live_bit_counts);
    return std::move(prepared).Apply();
}

void ArtBoxNodeStore::Validate() const {
    ValidateGeometry(AllocatorErrorCode::Corrupt);
    for (std::size_t index = 0;
         index < ArtBoxNodeCodec::kPayloadBytes.size();
         ++index) {
        const auto kind = static_cast<ArtBoxNodeKind>(index + 1U);
        const auto& blocks = Blocks(kind);
        blocks.Validate();
        const auto high_water = blocks.HighWater();
        for (std::uint32_t id = 0; id < high_water; ++id) {
            if (!blocks.IsAllocated(id)) continue;
            static_cast<void>(Read(ArtBoxNodeRefCodec::Encode(kind, id)));
        }
    }
}

std::uint32_t ArtBoxNodeStore::Capacity(ArtBoxNodeKind kind) const {
    return Blocks(kind).Capacity();
}

std::size_t ArtBoxNodeStore::HeaderBytes() const noexcept {
    return header_bytes_;
}

std::uint32_t ArtBoxNodeStore::HighWater(ArtBoxNodeKind kind) const {
    return Blocks(kind).HighWater();
}

std::uint32_t ArtBoxNodeStore::UsedCount(ArtBoxNodeKind kind) const {
    return Blocks(kind).UsedCount();
}

} // namespace kvspace::detail
