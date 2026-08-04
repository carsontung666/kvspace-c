#include "shm_trie_node_store.h"

#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace kvspace::detail {
namespace {

constexpr std::size_t kValueWordOffset = 0;
constexpr std::size_t kChildrenOffset = sizeof(std::uint64_t);

[[noreturn]] void RaiseTrie(
    AllocatorErrorCode code,
    const std::string& message) {
    throw AllocatorError(code, "kvspace trie node store: " + message);
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

bool IsEmptyTrieNode(const TrieNodeRecord& node) noexcept {
    if (node.has_value) return false;
    for (const auto child : node.children) {
        if (child != TrieNodeRecord::kEmptyChild) return false;
    }
    return true;
}

std::uint64_t MaximumCapacityForHeader(std::size_t header_bytes) noexcept {
    if (header_bytes == 2) return UINT64_C(1) << 14U;
    if (header_bytes == 4) return UINT64_C(1) << 30U;
    if (header_bytes == 8) return UINT32_MAX;
    return 0;
}

std::uint64_t CapacityForHeader(
    std::size_t node_zone_bytes,
    std::size_t header_bytes) noexcept {
    const auto stride = TrieNodeStore::kPersistentNodeBytes + header_bytes;
    const auto capacity = static_cast<std::uint64_t>(node_zone_bytes / stride);
    return capacity != 0 &&
            capacity <= MaximumCapacityForHeader(header_bytes)
        ? capacity
        : 0;
}

std::uint64_t SelectedCapacity(
    std::size_t node_zone_bytes,
    FixedBlockHeaderWidth requested) noexcept {
    if (requested != FixedBlockHeaderWidth::Automatic) {
        return CapacityForHeader(
            node_zone_bytes, static_cast<std::size_t>(requested));
    }
    for (const std::size_t width : {2U, 4U, 8U}) {
        const auto capacity = CapacityForHeader(node_zone_bytes, width);
        if (capacity != 0) return capacity;
    }
    return 0;
}

} // namespace

TrieNodeRecord::TrieNodeRecord() noexcept {
    children.fill(kEmptyChild);
}

std::array<std::uint8_t, TrieNodeCodec::kEncodedBytes>
TrieNodeCodec::Encode(const TrieNodeRecord& record) {
    if (record.value_ref > kMaxValueRef) {
        RaiseTrie(
            AllocatorErrorCode::InvalidArgument,
            "value reference exceeds 63 bits");
    }
    if (!record.has_value && record.value_ref != 0) {
        RaiseTrie(
            AllocatorErrorCode::InvalidArgument,
            "missing value must use canonical zero reference");
    }

    std::array<std::uint8_t, kEncodedBytes> encoded{};
    const auto packed = (record.value_ref << 1U) |
        static_cast<std::uint64_t>(record.has_value ? 1U : 0U);
    Store64(encoded.data() + kValueWordOffset, packed);
    for (std::size_t index = 0; index < record.children.size(); ++index) {
        const auto child = record.children[index];
        if (child < TrieNodeRecord::kEmptyChild) {
            RaiseTrie(
                AllocatorErrorCode::InvalidArgument,
                "child ID is below the empty sentinel");
        }
        const auto raw = child == TrieNodeRecord::kEmptyChild
            ? UINT32_MAX
            : static_cast<std::uint32_t>(child);
        Store32(
            encoded.data() + kChildrenOffset +
                index * sizeof(std::uint32_t),
            raw);
    }
    return encoded;
}

TrieNodeRecord TrieNodeCodec::Decode(
    const void* encoded,
    std::size_t encoded_bytes) {
    if (encoded == nullptr || encoded_bytes != kEncodedBytes) {
        RaiseTrie(
            AllocatorErrorCode::InvalidArgument,
            "codec input must be exactly 1032 bytes");
    }
    const auto* bytes = static_cast<const std::uint8_t*>(encoded);
    const auto packed = Load64(bytes + kValueWordOffset);
    TrieNodeRecord record;
    record.has_value = (packed & 1U) != 0;
    record.value_ref = packed >> 1U;
    if (!record.has_value && record.value_ref != 0) {
        RaiseTrie(
            AllocatorErrorCode::Corrupt,
            "noncanonical missing value reference");
    }
    for (std::size_t index = 0; index < record.children.size(); ++index) {
        const auto raw = Load32(
            bytes + kChildrenOffset + index * sizeof(std::uint32_t));
        if (raw == UINT32_MAX) {
            record.children[index] = TrieNodeRecord::kEmptyChild;
        } else if (raw <=
                   static_cast<std::uint32_t>(
                       std::numeric_limits<std::int32_t>::max())) {
            record.children[index] = static_cast<std::int32_t>(raw);
        } else {
            RaiseTrie(
                AllocatorErrorCode::Corrupt,
                "child ID is not -1 or a nonnegative int32");
        }
    }
    return record;
}

TrieNodeStore::TrieNodeStore(
    FixedBlockAllocator blocks,
    bool recovery_access) noexcept
    : blocks_(std::move(blocks)), recovery_access_(recovery_access) {}

TrieNodeStore::TrieNodeStore(TrieNodeStore&& other) noexcept
    : blocks_(std::move(other.blocks_)),
      recovery_access_(other.recovery_access_) {
    other.recovery_access_ = false;
}

TrieNodeStore& TrieNodeStore::operator=(TrieNodeStore&& other) noexcept {
    if (this == &other) return *this;
    blocks_ = std::move(other.blocks_);
    recovery_access_ = other.recovery_access_;
    other.recovery_access_ = false;
    return *this;
}

std::uint32_t TrieNodeStore::CanonicalCapacityForZone(
    std::uint64_t node_zone_bytes) {
    if (node_zone_bytes > std::numeric_limits<std::size_t>::max()) {
        RaiseTrie(
            AllocatorErrorCode::InvalidArgument,
            "node zone is not addressable");
    }
    const auto capacity = SelectedCapacity(
        static_cast<std::size_t>(node_zone_bytes),
        FixedBlockHeaderWidth::Automatic);
    if (capacity == 0) {
        RaiseTrie(
            AllocatorErrorCode::InvalidArgument,
            "node zone cannot hold a canonical fixed block");
    }
    if (capacity > kMaximumNodeCapacity) {
        RaiseTrie(
            AllocatorErrorCode::InvalidArgument,
            "node capacity exceeds the int32 ID space");
    }
    return static_cast<std::uint32_t>(capacity);
}

TrieNodeStore TrieNodeStore::Initialize(
    void* metadata,
    std::size_t metadata_bytes,
    void* node_zone,
    std::size_t node_zone_bytes,
    FixedBlockHeaderWidth header_width) {
    // FixedBlockAllocator supports a wider uint32 ID space. Reject Trie IDs
    // that cannot be encoded as nonnegative int32 before Initialize writes its
    // persistent metadata.
    if (header_width == FixedBlockHeaderWidth::Automatic) {
        static_cast<void>(CanonicalCapacityForZone(node_zone_bytes));
    } else {
        const auto capacity = SelectedCapacity(node_zone_bytes, header_width);
        if (capacity == 0 || capacity > kMaximumNodeCapacity) {
            RaiseTrie(
                AllocatorErrorCode::InvalidArgument,
                "invalid explicit node block geometry");
        }
    }
    auto blocks = FixedBlockAllocator::Initialize(
        metadata,
        metadata_bytes,
        node_zone,
        node_zone_bytes,
        kPersistentNodeBytes,
        header_width);
    TrieNodeStore store(std::move(blocks), false);
    store.ValidateGeometry(AllocatorErrorCode::InvalidArgument);
    return store;
}

TrieNodeStore TrieNodeStore::Attach(
    void* metadata,
    std::size_t metadata_bytes,
    void* node_zone,
    std::size_t node_zone_bytes) {
    auto blocks = FixedBlockAllocator::Attach(
        metadata, metadata_bytes, node_zone, node_zone_bytes);
    TrieNodeStore store(std::move(blocks), false);
    store.ValidateGeometry(AllocatorErrorCode::Corrupt);
    store.Validate();
    return store;
}

TrieNodeStore TrieNodeStore::AttachForRecovery(
    void* metadata,
    std::size_t metadata_bytes,
    void* node_zone,
    std::size_t node_zone_bytes) {
    auto blocks = FixedBlockAllocator::AttachForRecovery(
        metadata, metadata_bytes, node_zone, node_zone_bytes);
    TrieNodeStore store(std::move(blocks), true);
    store.ValidateGeometry(AllocatorErrorCode::Corrupt);
    return store;
}

void TrieNodeStore::ValidateGeometry(AllocatorErrorCode failure_code) const {
    if (blocks_.PayloadBytes() != kPersistentNodeBytes) {
        RaiseTrie(failure_code, "fixed block payload is not 1032 bytes");
    }
    if (static_cast<std::uint64_t>(blocks_.Capacity()) >
        kMaximumNodeCapacity) {
        RaiseTrie(failure_code, "node capacity exceeds the int32 ID space");
    }
}

void TrieNodeStore::ValidateRecord(
    const TrieNodeRecord& record,
    bool require_allocated_children,
    AllocatorErrorCode failure_code) const {
    for (const auto child : record.children) {
        if (child == TrieNodeRecord::kEmptyChild) continue;
        if (child < 0 ||
            static_cast<std::uint64_t>(child) >= blocks_.Capacity()) {
            RaiseTrie(failure_code, "child ID is outside the node store");
        }
        if (require_allocated_children &&
            !blocks_.IsAllocated(static_cast<std::uint32_t>(child))) {
            RaiseTrie(failure_code, "child ID does not name an allocated node");
        }
    }
}

std::uint32_t TrieNodeStore::Allocate() {
    return Allocate(TrieNodeRecord{});
}

std::uint32_t TrieNodeStore::Allocate(const TrieNodeRecord& record) {
    const auto encoded = TrieNodeCodec::Encode(record);
    ValidateRecord(record, true, AllocatorErrorCode::InvalidArgument);
    const auto id = blocks_.Allocate();
    std::memcpy(blocks_.BlockData(id), encoded.data(), encoded.size());
    return id;
}

std::uint32_t TrieNodeStore::Clone(std::uint32_t source_id) {
    // Read fully validates the source before Allocate changes any byte. The
    // returned record is then revalidated against the current serialized
    // allocation state by Allocate(record).
    return Allocate(Read(source_id));
}

void TrieNodeStore::DiscardUnpublished(std::uint32_t id) {
    blocks_.Free(id);
}

void TrieNodeStore::ReclaimPublished(std::uint32_t id) {
    blocks_.Free(id);
}

void TrieNodeStore::ReclaimPublished(
    const std::vector<std::uint32_t>& ids) {
    for (const auto id : ids) ReclaimPublished(id);
}

TrieNodeRecord TrieNodeStore::Read(std::uint32_t id) const {
    const auto record = TrieNodeCodec::Decode(
        blocks_.BlockData(id), kPersistentNodeBytes);
    ValidateRecord(record, true, AllocatorErrorCode::Corrupt);
    return record;
}

TrieNodeRecord TrieNodeStore::ReadForRecovery(std::uint32_t id) const {
    if (!recovery_access_) {
        RaiseTrie(
            AllocatorErrorCode::InvalidArgument,
            "recovery read requires recovery attach");
    }
    const auto record = TrieNodeCodec::Decode(
        blocks_.RecoveryBlockData(id), kPersistentNodeBytes);
    ValidateRecord(record, false, AllocatorErrorCode::Corrupt);
    return record;
}

TrieNodeStore::PreparedRecovery
TrieNodeStore::PrepareRecoveryFromCommittedRoot(
    std::uint32_t committed_root_id) && {
    if (!recovery_access_) {
        RaiseTrie(
            AllocatorErrorCode::InvalidArgument,
            "committed-root recovery requires recovery attach");
    }
    const auto capacity = blocks_.Capacity();
    if (committed_root_id >= capacity) {
        RaiseTrie(AllocatorErrorCode::Corrupt, "committed root is out of range");
    }

    // All root-derived graph traversal, codec validation, and heap allocation
    // happens before FixedBlockAllocator::Rebuild changes a persistent byte.
    // Marking an already seen child rejects cycles, duplicate edges, and DAGs.
    const auto bitmap_bytes =
        (static_cast<std::size_t>(capacity) + 7U) / 8U;
    std::vector<std::uint8_t> live_bits(bitmap_bytes, 0);
    std::vector<std::uint32_t> stack;
    std::vector<std::uint64_t> value_references;
    SetBitmapBit(&live_bits, committed_root_id);
    stack.push_back(committed_root_id);
    std::uint32_t max_live_plus_one = committed_root_id + 1U;
    std::uint64_t reachable_node_count = 0;
    while (!stack.empty()) {
        const auto id = stack.back();
        stack.pop_back();
        const auto record = ReadForRecovery(id);
        if (id != committed_root_id && IsEmptyTrieNode(record)) {
            RaiseTrie(
                AllocatorErrorCode::Corrupt,
                "committed trie contains a noncanonical empty non-root leaf");
        }
        ++reachable_node_count;
        if (record.has_value) {
            value_references.push_back(record.value_ref);
        }
        for (const auto child : record.children) {
            if (child == TrieNodeRecord::kEmptyChild) continue;
            const auto child_id = static_cast<std::uint32_t>(child);
            if (BitmapBit(live_bits, child_id)) {
                RaiseTrie(
                    AllocatorErrorCode::Corrupt,
                    "committed trie contains a cycle or shared node");
            }
            SetBitmapBit(&live_bits, child_id);
            stack.push_back(child_id);
            if (child_id >= max_live_plus_one) {
                max_live_plus_one = child_id + 1U;
            }
        }
    }

    const auto live_bit_count = static_cast<std::size_t>(max_live_plus_one);
    live_bits.resize((live_bit_count + 7U) / 8U);
    const auto scratch_bytes = blocks_.RebuildScratchBytes(
        live_bits.data(), live_bit_count);
    std::vector<std::uint8_t> rebuild_scratch(scratch_bytes, 0);
    return PreparedRecovery(
        std::move(*this),
        std::move(live_bits),
        live_bit_count,
        reachable_node_count,
        std::move(value_references),
        std::move(rebuild_scratch));
}

TrieNodeStore TrieNodeStore::RecoverFromCommittedRoot(
    std::uint32_t committed_root_id) && {
    auto prepared = std::move(*this).PrepareRecoveryFromCommittedRoot(
        committed_root_id);
    return std::move(prepared).Apply();
}

TrieNodeStore::PreparedRecovery::PreparedRecovery(
    TrieNodeStore store,
    std::vector<std::uint8_t> live_bits,
    std::size_t live_bit_count,
    std::uint64_t reachable_node_count,
    std::vector<std::uint64_t> value_references,
    std::vector<std::uint8_t> rebuild_scratch) noexcept
    : store_(std::move(store)),
      live_bits_(std::move(live_bits)),
      live_bit_count_(live_bit_count),
      reachable_node_count_(reachable_node_count),
      value_references_(std::move(value_references)),
      rebuild_scratch_(std::move(rebuild_scratch)) {}

TrieNodeStore::PreparedRecovery::PreparedRecovery(
    PreparedRecovery&& other) noexcept
    : store_(std::move(other.store_)),
      live_bits_(std::move(other.live_bits_)),
      live_bit_count_(other.live_bit_count_),
      reachable_node_count_(other.reachable_node_count_),
      value_references_(std::move(other.value_references_)),
      rebuild_scratch_(std::move(other.rebuild_scratch_)),
      ready_(other.ready_) {
    other.live_bit_count_ = 0;
    other.reachable_node_count_ = 0;
    other.ready_ = false;
}

TrieNodeStore::PreparedRecovery&
TrieNodeStore::PreparedRecovery::operator=(
    PreparedRecovery&& other) noexcept {
    if (this == &other) return *this;
    store_ = std::move(other.store_);
    live_bits_ = std::move(other.live_bits_);
    live_bit_count_ = other.live_bit_count_;
    reachable_node_count_ = other.reachable_node_count_;
    value_references_ = std::move(other.value_references_);
    rebuild_scratch_ = std::move(other.rebuild_scratch_);
    ready_ = other.ready_;
    other.live_bit_count_ = 0;
    other.reachable_node_count_ = 0;
    other.ready_ = false;
    return *this;
}

TrieNodeStore TrieNodeStore::PreparedRecovery::Apply() && {
    if (!ready_) {
        RaiseTrie(
            AllocatorErrorCode::InvalidArgument,
            "prepared recovery was already consumed");
    }
    store_.blocks_.RebuildPrepared(
        live_bits_.data(),
        live_bit_count_,
        rebuild_scratch_.data(),
        rebuild_scratch_.size());
    store_.recovery_access_ = false;
    ready_ = false;
    return std::move(store_);
}

void TrieNodeStore::Validate() const {
    ValidateGeometry(AllocatorErrorCode::Corrupt);
    blocks_.Validate();
    const auto high_water = blocks_.HighWater();
    for (std::uint32_t id = 0; id < high_water; ++id) {
        if (!blocks_.IsAllocated(id)) continue;
        static_cast<void>(Read(id));
    }
}

} // namespace kvspace::detail
