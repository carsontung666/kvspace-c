#include "test_support.h"

#include "shm_trie_node_store.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using kvspace::detail::AllocatorError;
using kvspace::detail::AllocatorErrorCode;
using kvspace::detail::FixedBlockAllocator;
using kvspace::detail::FixedBlockHeaderWidth;
using kvspace::detail::TrieNodeCodec;
using kvspace::detail::TrieNodeRecord;
using kvspace::detail::TrieNodeStore;

constexpr std::size_t kHeaderBytes = 2;
constexpr std::size_t kStride =
    kHeaderBytes + TrieNodeStore::kPersistentNodeBytes;

template <typename Function>
void expectAllocatorError(AllocatorErrorCode code, Function&& function) {
    bool caught = false;
    try {
        function();
    } catch (const AllocatorError& error) {
        CHECK(error.Code() == code);
        caught = true;
    }
    CHECK(caught);
}

class ScopedMapping final {
public:
    ScopedMapping(void* address, std::size_t bytes)
        : address_(address), bytes_(bytes) {}
    ~ScopedMapping() {
        if (address_ != MAP_FAILED) {
            static_cast<void>(::munmap(address_, bytes_));
        }
    }
    ScopedMapping(const ScopedMapping&) = delete;
    ScopedMapping& operator=(const ScopedMapping&) = delete;

private:
    void* address_;
    std::size_t bytes_;
};

bool recordsEqual(
    const TrieNodeRecord& left,
    const TrieNodeRecord& right) {
    return left.has_value == right.has_value &&
        left.value_ref == right.value_ref &&
        left.children == right.children;
}

std::uint8_t* payloadAt(
    std::vector<std::uint8_t>* zone,
    std::uint32_t id) {
    return zone->data() + static_cast<std::size_t>(id) * kStride +
        kHeaderBytes;
}

const std::uint8_t* payloadAt(
    const std::vector<std::uint8_t>* zone,
    std::uint32_t id) {
    return zone->data() + static_cast<std::size_t>(id) * kStride +
        kHeaderBytes;
}

void store32(std::uint8_t* bytes, std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        bytes[index] = static_cast<std::uint8_t>(value >> shift);
    }
}

void storeChild(
    std::vector<std::uint8_t>* zone,
    std::uint32_t id,
    std::size_t slot,
    std::uint32_t child) {
    store32(
        payloadAt(zone, id) + 8U + slot * sizeof(std::uint32_t),
        child);
}

void expectRecoveryFailureWithoutWrites(
    AllocatorErrorCode code,
    std::vector<std::uint8_t>* metadata,
    std::vector<std::uint8_t>* zone,
    std::uint32_t root) {
    const auto metadata_before = *metadata;
    const auto zone_before = *zone;
    auto recovery = TrieNodeStore::AttachForRecovery(
        metadata->data(), metadata->size(), zone->data(), zone->size());
    expectAllocatorError(code, [&] {
        static_cast<void>(
            std::move(recovery).RecoverFromCommittedRoot(root));
    });
    CHECK(*metadata == metadata_before);
    CHECK(*zone == zone_before);
}

void testCodecGoldenBytesAndCanonicalValues() {
    CHECK(TrieNodeCodec::kEncodedBytes == 1032);
    CHECK(TrieNodeStore::kPersistentNodeBytes == 1032);
    CHECK(TrieNodeStore::kMaximumNodeCapacity ==
          static_cast<std::uint64_t>(INT32_MAX) + 1U);
    CHECK(TrieNodeStore::kMaximumNodeCapacity - 1U ==
          static_cast<std::uint64_t>(INT32_MAX));
    CHECK(FixedBlockAllocator::MinimumHeaderWidth(
              TrieNodeStore::kMaximumNodeCapacity) ==
          FixedBlockHeaderWidth::Bytes8);

    const TrieNodeRecord empty;
    const auto empty_bytes = TrieNodeCodec::Encode(empty);
    CHECK(std::all_of(
        empty_bytes.begin(), empty_bytes.begin() + 8,
        [](std::uint8_t value) { return value == 0; }));
    CHECK(std::all_of(
        empty_bytes.begin() + 8, empty_bytes.end(),
        [](std::uint8_t value) { return value == 0xff; }));
    CHECK(recordsEqual(
        TrieNodeCodec::Decode(empty_bytes.data(), empty_bytes.size()), empty));

    // A present opaque reference of zero is valid. This codec intentionally
    // assigns it no Box-offset, None, or value-length meaning.
    TrieNodeRecord present_zero;
    present_zero.has_value = true;
    const auto present_zero_bytes = TrieNodeCodec::Encode(present_zero);
    CHECK(present_zero_bytes[0] == 1);
    CHECK(std::all_of(
        present_zero_bytes.begin() + 1, present_zero_bytes.begin() + 8,
        [](std::uint8_t value) { return value == 0; }));
    CHECK(recordsEqual(
        TrieNodeCodec::Decode(
            present_zero_bytes.data(), present_zero_bytes.size()),
        present_zero));

    TrieNodeRecord one;
    one.has_value = true;
    one.value_ref = 1;
    const auto one_bytes = TrieNodeCodec::Encode(one);
    CHECK(one_bytes[0] == 3);
    CHECK(std::all_of(
        one_bytes.begin() + 1, one_bytes.begin() + 8,
        [](std::uint8_t value) { return value == 0; }));

    TrieNodeRecord maximum;
    maximum.has_value = true;
    maximum.value_ref = TrieNodeCodec::kMaxValueRef;
    const auto maximum_bytes = TrieNodeCodec::Encode(maximum);
    CHECK(std::all_of(
        maximum_bytes.begin(), maximum_bytes.end(),
        [](std::uint8_t value) { return value == 0xff; }));
    CHECK(recordsEqual(
        TrieNodeCodec::Decode(maximum_bytes.data(), maximum_bytes.size()),
        maximum));

    TrieNodeRecord children;
    children.children[0] = 0;
    children.children[1] = 0x12345678;
    children.children[255] = INT32_MAX;
    const auto child_bytes = TrieNodeCodec::Encode(children);
    CHECK(child_bytes[8] == 0 && child_bytes[9] == 0 &&
          child_bytes[10] == 0 && child_bytes[11] == 0);
    CHECK(child_bytes[12] == 0x78 && child_bytes[13] == 0x56 &&
          child_bytes[14] == 0x34 && child_bytes[15] == 0x12);
    const auto last = 8U + 255U * sizeof(std::uint32_t);
    CHECK(child_bytes[last] == 0xff && child_bytes[last + 1U] == 0xff &&
          child_bytes[last + 2U] == 0xff && child_bytes[last + 3U] == 0x7f);
    CHECK(recordsEqual(
        TrieNodeCodec::Decode(child_bytes.data(), child_bytes.size()),
        children));
    CHECK(TrieNodeCodec::Encode(children) == child_bytes);

    TrieNodeRecord noncanonical_missing;
    noncanonical_missing.value_ref = 1;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(TrieNodeCodec::Encode(noncanonical_missing));
    });
    TrieNodeRecord oversized_ref;
    oversized_ref.has_value = true;
    oversized_ref.value_ref = UINT64_C(1) << 63U;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(TrieNodeCodec::Encode(oversized_ref));
    });
    TrieNodeRecord negative_child;
    negative_child.children[7] = -2;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(TrieNodeCodec::Encode(negative_child));
    });

    auto corrupt = empty_bytes;
    corrupt[0] = 2;
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(TrieNodeCodec::Decode(corrupt.data(), corrupt.size()));
    });
    corrupt = empty_bytes;
    store32(corrupt.data() + 8, UINT32_MAX - 1U);
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(TrieNodeCodec::Decode(corrupt.data(), corrupt.size()));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(TrieNodeCodec::Decode(nullptr, corrupt.size()));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(
            TrieNodeCodec::Decode(corrupt.data(), corrupt.size() - 1U));
    });
}

void testStoreExactLayoutIdZeroReuseAndValidation() {
    constexpr std::size_t capacity = 4;
    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes, 0xa5);
    std::vector<std::uint8_t> zone(kStride * capacity, 0xa5);
    auto store = TrieNodeStore::Initialize(
        metadata.data(),
        metadata.size(),
        zone.data(),
        zone.size(),
        FixedBlockHeaderWidth::Bytes2);
    CHECK(metadata[0] == 'K' && metadata[1] == 'V' &&
          metadata[2] == 'B' && metadata[3] == 'L');
    CHECK(store.Capacity() == capacity);
    CHECK(store.Allocate() == 0);
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(store.ReadForRecovery(0));
    });
    const auto canonical_empty = TrieNodeCodec::Encode(TrieNodeRecord{});
    CHECK(std::equal(
        canonical_empty.begin(), canonical_empty.end(), payloadAt(&zone, 0)));

    TrieNodeRecord root;
    root.has_value = true;
    root.value_ref = 0;
    root.children[0] = 0;
    root.children[255] = 0;
    CHECK(store.Allocate(root) == 1);
    CHECK(recordsEqual(store.Read(1), root));
    CHECK(payloadAt(&zone, 1)[0] == 1);
    CHECK(payloadAt(&zone, 1)[8] == 0);
    CHECK(payloadAt(&zone, 1)[9] == 0);
    const auto metadata_before_normal_rebuild = metadata;
    const auto zone_before_normal_rebuild = zone;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(
            std::move(store).RecoverFromCommittedRoot(0));
    });
    const std::uint8_t no_live_nodes = 0;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(
            std::move(store).RecoverFromCommittedRoot(no_live_nodes));
    });
    CHECK(metadata == metadata_before_normal_rebuild);
    CHECK(zone == zone_before_normal_rebuild);

    TrieNodeRecord dangling;
    dangling.children[0] = 2;
    TrieNodeRecord out_of_range;
    out_of_range.children[0] = static_cast<std::int32_t>(capacity);
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(store.Allocate(out_of_range));
    });
    TrieNodeRecord noncanonical;
    noncanonical.value_ref = 9;
    CHECK(recordsEqual(store.Read(1), root));

    const auto metadata_before_invalid_allocate = metadata;
    const auto zone_before_invalid_allocate = zone;
    const auto used_before_invalid_allocate = store.UsedCount();
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(store.Allocate(dangling));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(store.Allocate(noncanonical));
    });
    TrieNodeRecord negative_allocate;
    negative_allocate.children[0] = -2;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(store.Allocate(negative_allocate));
    });
    CHECK(store.UsedCount() == used_before_invalid_allocate);
    CHECK(metadata == metadata_before_invalid_allocate);
    CHECK(zone == zone_before_invalid_allocate);

    TrieNodeRecord old_contents;
    old_contents.has_value = true;
    old_contents.value_ref = TrieNodeCodec::kMaxValueRef;
    old_contents.children.fill(0);
    CHECK(store.Allocate(old_contents) == 2);
    store.DiscardUnpublished(2);
    std::memset(payloadAt(&zone, 2), 0xa5, TrieNodeStore::kPersistentNodeBytes);
    CHECK(store.Allocate() == 2);
    CHECK(std::equal(
        canonical_empty.begin(), canonical_empty.end(), payloadAt(&zone, 2)));
    CHECK(recordsEqual(store.Read(2), TrieNodeRecord{}));

    // A normal read/attach rejects syntactically invalid and dangling edges.
    store32(payloadAt(&zone, 1) + 8, UINT32_MAX - 1U);
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(store.Read(1));
    });
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(TrieNodeStore::Attach(
            metadata.data(), metadata.size(), zone.data(), zone.size()));
    });
    store32(payloadAt(&zone, 1) + 8, 3);
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        store.Validate();
    });
    store32(payloadAt(&zone, 1) + 8, 0);
    store.Validate();
    auto attached = TrieNodeStore::Attach(
        metadata.data(), metadata.size(), zone.data(), zone.size());
    CHECK(recordsEqual(attached.Read(1), root));

    // The format contains no process address, so a byte copy attaches at a
    // different mapping address without fixups.
    auto copied_metadata = metadata;
    auto copied_zone = zone;
    auto copied = TrieNodeStore::Attach(
        copied_metadata.data(),
        copied_metadata.size(),
        copied_zone.data(),
        copied_zone.size());
    CHECK(recordsEqual(copied.Read(1), root));

    auto corrupt_immutable = metadata;
    corrupt_immutable[0] ^= 0x01;
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(TrieNodeStore::Attach(
            corrupt_immutable.data(),
            corrupt_immutable.size(),
            zone.data(),
            zone.size()));
    });
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(TrieNodeStore::AttachForRecovery(
            corrupt_immutable.data(),
            corrupt_immutable.size(),
            zone.data(),
            zone.size()));
    });

    attached.DiscardUnpublished(2);
    expectAllocatorError(AllocatorErrorCode::NotAllocated, [&] {
        static_cast<void>(attached.Read(2));
    });

    std::vector<std::uint8_t> wrong_metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    constexpr std::size_t wrong_payload =
        TrieNodeStore::kPersistentNodeBytes - 1U;
    std::vector<std::uint8_t> wrong_zone((wrong_payload + 2U) * 2U);
    static_cast<void>(FixedBlockAllocator::Initialize(
        wrong_metadata.data(),
        wrong_metadata.size(),
        wrong_zone.data(),
        wrong_zone.size(),
        wrong_payload,
        FixedBlockHeaderWidth::Bytes2));
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(TrieNodeStore::Attach(
            wrong_metadata.data(),
            wrong_metadata.size(),
            wrong_zone.data(),
            wrong_zone.size()));
    });
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(TrieNodeStore::AttachForRecovery(
            wrong_metadata.data(),
            wrong_metadata.size(),
            wrong_zone.data(),
            wrong_zone.size()));
    });
}

void testRecoveryIgnoresDynamicStateAndPreflightsEdges() {
    constexpr std::size_t capacity = 4;
    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> zone(kStride * capacity, 0xa5);
    auto store = TrieNodeStore::Initialize(
        metadata.data(),
        metadata.size(),
        zone.data(),
        zone.size(),
        FixedBlockHeaderWidth::Bytes2);
    TrieNodeRecord child;
    child.has_value = true;
    child.value_ref = 0;
    CHECK(store.Allocate(child) == 0);
    TrieNodeRecord root;
    root.has_value = true;
    root.value_ref = 17;
    root.children[42] = 0;
    CHECK(store.Allocate(root) == 1);
    CHECK(store.Allocate() == 2);

    // Corrupt mutable counters/free head and the root's live block header.
    std::fill(metadata.begin() + 36, metadata.begin() + 48, 0xff);
    zone[kStride] = 3;
    zone[kStride + 1U] = 0;
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(TrieNodeStore::Attach(
            metadata.data(), metadata.size(), zone.data(), zone.size()));
    });
    auto recovery = TrieNodeStore::AttachForRecovery(
        metadata.data(), metadata.size(), zone.data(), zone.size());
    CHECK(recordsEqual(recovery.ReadForRecovery(1), root));
    CHECK(recordsEqual(recovery.ReadForRecovery(0), child));
    expectAllocatorError(AllocatorErrorCode::InvalidId, [&] {
        static_cast<void>(recovery.ReadForRecovery(recovery.Capacity()));
    });
    const auto metadata_before_recovery_gate = metadata;
    const auto zone_before_recovery_gate = zone;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(recovery.Read(1));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(recovery.Allocate());
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        recovery.Validate();
    });
    CHECK(metadata == metadata_before_recovery_gate);
    CHECK(zone == zone_before_recovery_gate);

    const auto metadata_before_bad_root = metadata;
    const auto zone_before_bad_root = zone;
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(std::move(recovery).RecoverFromCommittedRoot(
            recovery.Capacity()));
    });
    CHECK(metadata == metadata_before_bad_root);
    CHECK(zone == zone_before_bad_root);
    CHECK(recordsEqual(recovery.ReadForRecovery(1), root));

    auto normal = std::move(recovery).RecoverFromCommittedRoot(1);
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(recovery.ReadForRecovery(1));
    });
    normal.Validate();
    CHECK(normal.UsedCount() == 2);
    CHECK(recordsEqual(normal.Read(1), root));
    CHECK(recordsEqual(normal.Read(0), child));
    const auto stable_metadata = metadata;
    const auto stable_zone = zone;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(
            std::move(normal).RecoverFromCommittedRoot(1));
    });
    CHECK(metadata == stable_metadata);
    CHECK(zone == stable_zone);
    auto second_recovery = TrieNodeStore::AttachForRecovery(
        metadata.data(), metadata.size(), zone.data(), zone.size());
    auto second_normal =
        std::move(second_recovery).RecoverFromCommittedRoot(1);
    CHECK(metadata == stable_metadata);
    CHECK(zone == stable_zone);
    second_normal.Validate();
    auto normal_after_recovery = TrieNodeStore::Attach(
        metadata.data(), metadata.size(), zone.data(), zone.size());
    CHECK(recordsEqual(normal_after_recovery.Read(1), root));
    CHECK(recordsEqual(normal_after_recovery.Read(0), child));
    CHECK(normal.Allocate() == 2);
    CHECK(recordsEqual(normal.Read(2), TrieNodeRecord{}));

    // Malformed authoritative payload also fails before metadata mutation.
    store32(payloadAt(&zone, 1), 2);
    auto malformed_recovery = TrieNodeStore::AttachForRecovery(
        metadata.data(), metadata.size(), zone.data(), zone.size());
    const auto metadata_before_bad_payload = metadata;
    const auto zone_before_bad_payload = zone;
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(
            std::move(malformed_recovery).RecoverFromCommittedRoot(1));
    });
    CHECK(metadata == metadata_before_bad_payload);
    CHECK(zone == zone_before_bad_payload);
}

void testRecoveryDerivesOneTreeAndIgnoresUnreachablePayloads() {
    constexpr std::size_t capacity = 8;

    // Both ends of the ID range are ordinary roots; zero is never a null
    // sentinel. Other allocated records become unreachable free blocks.
    for (const std::uint32_t root : {
             UINT32_C(0), static_cast<std::uint32_t>(capacity - 1U)}) {
        std::vector<std::uint8_t> metadata(
            FixedBlockAllocator::kPersistentMetadataBytes);
        std::vector<std::uint8_t> zone(kStride * capacity, 0xa5);
        auto store = TrieNodeStore::Initialize(
            metadata.data(), metadata.size(), zone.data(), zone.size(),
            FixedBlockHeaderWidth::Bytes2);
        for (std::uint32_t id = 0; id < capacity; ++id) {
            CHECK(store.Allocate() == id);
        }
        auto recovery = TrieNodeStore::AttachForRecovery(
            metadata.data(), metadata.size(), zone.data(), zone.size());
        auto normal =
            std::move(recovery).RecoverFromCommittedRoot(root);
        CHECK(normal.UsedCount() == 1);
        CHECK(recordsEqual(normal.Read(root), TrieNodeRecord{}));
        normal.Validate();
    }

    {
        std::vector<std::uint8_t> metadata(
            FixedBlockAllocator::kPersistentMetadataBytes);
        std::vector<std::uint8_t> zone(kStride * capacity, 0xa5);
        auto store = TrieNodeStore::Initialize(
            metadata.data(), metadata.size(), zone.data(), zone.size(),
            FixedBlockHeaderWidth::Bytes2);
        CHECK(store.Allocate() == 0);
        expectRecoveryFailureWithoutWrites(
            AllocatorErrorCode::Corrupt, &metadata, &zone, UINT32_MAX);
    }

    // ID zero is a real root. A self edge must be rejected before the first
    // allocator metadata write.
    {
        std::vector<std::uint8_t> metadata(
            FixedBlockAllocator::kPersistentMetadataBytes);
        std::vector<std::uint8_t> zone(kStride * capacity, 0xa5);
        auto store = TrieNodeStore::Initialize(
            metadata.data(), metadata.size(), zone.data(), zone.size(),
            FixedBlockHeaderWidth::Bytes2);
        CHECK(store.Allocate() == 0);
        storeChild(&zone, 0, 7, 0);
        expectRecoveryFailureWithoutWrites(
            AllocatorErrorCode::Corrupt, &metadata, &zone, 0);
    }

    // A back edge is a cycle even though each individual child ID and payload
    // is syntactically valid.
    {
        std::vector<std::uint8_t> metadata(
            FixedBlockAllocator::kPersistentMetadataBytes);
        std::vector<std::uint8_t> zone(kStride * capacity, 0xa5);
        auto store = TrieNodeStore::Initialize(
            metadata.data(), metadata.size(), zone.data(), zone.size(),
            FixedBlockHeaderWidth::Bytes2);
        CHECK(store.Allocate() == 0);
        TrieNodeRecord second;
        second.children[2] = 0;
        CHECK(store.Allocate(second) == 1);
        storeChild(&zone, 0, 1, 1);
        expectRecoveryFailureWithoutWrites(
            AllocatorErrorCode::Corrupt, &metadata, &zone, 0);
    }

    // Two slots naming one child and a diamond naming one leaf are both DAGs,
    // not valid immutable trie trees.
    {
        std::vector<std::uint8_t> metadata(
            FixedBlockAllocator::kPersistentMetadataBytes);
        std::vector<std::uint8_t> zone(kStride * capacity, 0xa5);
        auto store = TrieNodeStore::Initialize(
            metadata.data(), metadata.size(), zone.data(), zone.size(),
            FixedBlockHeaderWidth::Bytes2);
        CHECK(store.Allocate() == 0);
        TrieNodeRecord duplicate;
        duplicate.children[10] = 0;
        duplicate.children[11] = 0;
        CHECK(store.Allocate(duplicate) == 1);
        expectRecoveryFailureWithoutWrites(
            AllocatorErrorCode::Corrupt, &metadata, &zone, 1);
    }
    {
        std::vector<std::uint8_t> metadata(
            FixedBlockAllocator::kPersistentMetadataBytes);
        std::vector<std::uint8_t> zone(kStride * capacity, 0xa5);
        auto store = TrieNodeStore::Initialize(
            metadata.data(), metadata.size(), zone.data(), zone.size(),
            FixedBlockHeaderWidth::Bytes2);
        CHECK(store.Allocate() == 0);
        TrieNodeRecord left;
        left.children[1] = 0;
        CHECK(store.Allocate(left) == 1);
        TrieNodeRecord right;
        right.children[2] = 0;
        CHECK(store.Allocate(right) == 2);
        TrieNodeRecord diamond;
        diamond.children[3] = 1;
        diamond.children[4] = 2;
        CHECK(store.Allocate(diamond) == 3);
        expectRecoveryFailureWithoutWrites(
            AllocatorErrorCode::Corrupt, &metadata, &zone, 3);
    }

    // Reachable malformed edges fail, but unreachable bytes are deliberately
    // not decoded. They are reclaimed and overwritten on the next allocation.
    {
        std::vector<std::uint8_t> metadata(
            FixedBlockAllocator::kPersistentMetadataBytes);
        std::vector<std::uint8_t> zone(kStride * capacity, 0xa5);
        auto store = TrieNodeStore::Initialize(
            metadata.data(), metadata.size(), zone.data(), zone.size(),
            FixedBlockHeaderWidth::Bytes2);
        TrieNodeRecord leaf;
        leaf.has_value = true;
        CHECK(store.Allocate(leaf) == 0);
        TrieNodeRecord root;
        root.children[5] = 0;
        CHECK(store.Allocate(root) == 1);
        CHECK(store.Allocate() == 2);

        storeChild(
            &zone, 1, 5, static_cast<std::uint32_t>(capacity));
        expectRecoveryFailureWithoutWrites(
            AllocatorErrorCode::Corrupt, &metadata, &zone, 1);
        storeChild(&zone, 1, 5, 0);

        payloadAt(&zone, 2)[0] = 2;
        const auto payloads_before = zone;
        auto recovery = TrieNodeStore::AttachForRecovery(
            metadata.data(), metadata.size(), zone.data(), zone.size());
        auto normal =
            std::move(recovery).RecoverFromCommittedRoot(1);
        CHECK(normal.UsedCount() == 2);
        CHECK(std::equal(
            payloadAt(&payloads_before, 0),
            payloadAt(&payloads_before, 0) +
                TrieNodeStore::kPersistentNodeBytes,
            payloadAt(&zone, 0)));
        CHECK(std::equal(
            payloadAt(&payloads_before, 1),
            payloadAt(&payloads_before, 1) +
                TrieNodeStore::kPersistentNodeBytes,
            payloadAt(&zone, 1)));
        CHECK(payloadAt(&zone, 2)[0] == 2);
        CHECK(normal.Allocate() == 2);
        CHECK(recordsEqual(normal.Read(2), TrieNodeRecord{}));
        normal.Validate();
    }
}

void testRecoveryRejectsEmptyNonRootLeafWithoutWrites() {
    constexpr std::size_t capacity = 3;
    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> zone(kStride * capacity, 0xa5);
    auto store = TrieNodeStore::Initialize(
        metadata.data(),
        metadata.size(),
        zone.data(),
        zone.size(),
        FixedBlockHeaderWidth::Bytes2);
    const auto empty_child = store.Allocate();
    TrieNodeRecord root;
    root.children[7] = static_cast<std::int32_t>(empty_child);
    const auto malformed_root = store.Allocate(root);

    expectRecoveryFailureWithoutWrites(
        AllocatorErrorCode::Corrupt,
        &metadata,
        &zone,
        malformed_root);
}

void testRecoveryDocumentsInRangeCanonicalPayloadTrustBoundary() {
    constexpr std::size_t capacity = 8;
    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> zone(kStride * capacity, 0xa5);
    auto store = TrieNodeStore::Initialize(
        metadata.data(), metadata.size(), zone.data(), zone.size(),
        FixedBlockHeaderWidth::Bytes2);
    CHECK(store.Allocate() == 0);

    // Simulate arbitrary corruption that creates a syntactically canonical
    // node in a slot which allocator metadata says was never allocated, then
    // points the committed root at it. Recovery deliberately cannot consult
    // torn allocation headers, and the exact 1032-byte format has no generation
    // or checksum. Consequently this in-range canonical payload is accepted;
    // immutable publish-last COW is the stated provenance boundary.
    constexpr std::uint32_t fabricated_id = 7;
    TrieNodeRecord fabricated;
    fabricated.has_value = true;
    fabricated.value_ref = 23;
    const auto canonical = TrieNodeCodec::Encode(fabricated);
    std::copy(
        canonical.begin(), canonical.end(), payloadAt(&zone, fabricated_id));
    storeChild(&zone, 0, 99, fabricated_id);

    auto recovery = TrieNodeStore::AttachForRecovery(
        metadata.data(), metadata.size(), zone.data(), zone.size());
    auto normal = std::move(recovery).RecoverFromCommittedRoot(0);
    CHECK(normal.UsedCount() == 2);
    CHECK(normal.HighWater() == fabricated_id + 1U);
    CHECK(recordsEqual(normal.Read(fabricated_id), fabricated));
    CHECK(normal.Read(0).children[99] ==
          static_cast<std::int32_t>(fabricated_id));
    normal.Validate();
}

void testFullFanoutCloneAndPathCopy() {
    constexpr std::size_t fanout_capacity = 257;
    std::vector<std::uint8_t> fanout_metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> fanout_zone(kStride * fanout_capacity, 0xa5);
    auto fanout_store = TrieNodeStore::Initialize(
        fanout_metadata.data(),
        fanout_metadata.size(),
        fanout_zone.data(),
        fanout_zone.size(),
        FixedBlockHeaderWidth::Bytes2);
    for (std::uint32_t id = 0; id < 256; ++id) {
        CHECK(fanout_store.Allocate() == id);
    }
    TrieNodeRecord fanout;
    for (std::size_t slot = 0; slot < fanout.children.size(); ++slot) {
        fanout.children[slot] = static_cast<std::int32_t>(slot);
    }
    CHECK(fanout_store.Allocate(fanout) == 256);
    CHECK(recordsEqual(fanout_store.Read(256), fanout));
    CHECK(fanout_store.Read(256).children[0] == 0);
    CHECK(fanout_store.Read(256).children[255] == 255);
    fanout_store.Validate();

    // Exercise a three-level storage-only path copy. Root publication and
    // reclamation are deliberately outside this component.
    constexpr std::size_t path_capacity = 6;
    std::vector<std::uint8_t> path_metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> path_zone(kStride * path_capacity, 0xa5);
    auto path_store = TrieNodeStore::Initialize(
        path_metadata.data(),
        path_metadata.size(),
        path_zone.data(),
        path_zone.size(),
        FixedBlockHeaderWidth::Bytes2);
    TrieNodeRecord original_leaf;
    original_leaf.has_value = true;
    original_leaf.value_ref = 10;
    CHECK(path_store.Allocate(original_leaf) == 0);
    TrieNodeRecord original_middle;
    original_middle.children[20] = 0;
    CHECK(path_store.Allocate(original_middle) == 1);
    TrieNodeRecord original_root;
    original_root.children[10] = 1;
    CHECK(path_store.Allocate(original_root) == 2);

    const auto exact_clone_id = path_store.Clone(0);
    CHECK(exact_clone_id == 3);
    CHECK(std::equal(
        payloadAt(&path_zone, 0),
        payloadAt(&path_zone, 0) + TrieNodeStore::kPersistentNodeBytes,
        payloadAt(&path_zone, exact_clone_id)));
    CHECK(recordsEqual(path_store.Read(exact_clone_id), original_leaf));
    path_store.DiscardUnpublished(exact_clone_id);

    auto new_leaf = path_store.Read(0);
    new_leaf.value_ref = 11;
    const auto new_leaf_id = path_store.Allocate(new_leaf);
    CHECK(new_leaf_id == 3);
    auto new_middle = path_store.Read(1);
    new_middle.children[20] = static_cast<std::int32_t>(new_leaf_id);
    const auto new_middle_id = path_store.Allocate(new_middle);
    CHECK(new_middle_id == 4);
    auto new_root = path_store.Read(2);
    new_root.children[10] = static_cast<std::int32_t>(new_middle_id);
    const auto new_root_id = path_store.Allocate(new_root);
    CHECK(new_root_id == 5);

    CHECK(recordsEqual(path_store.Read(0), original_leaf));
    CHECK(recordsEqual(path_store.Read(1), original_middle));
    CHECK(recordsEqual(path_store.Read(2), original_root));
    CHECK(recordsEqual(path_store.Read(3), new_leaf));
    CHECK(recordsEqual(path_store.Read(4), new_middle));
    CHECK(recordsEqual(path_store.Read(5), new_root));
    path_store.Validate();

    const auto metadata_before_failed_clone = path_metadata;
    const auto zone_before_failed_clone = path_zone;
    expectAllocatorError(AllocatorErrorCode::Capacity, [&] {
        static_cast<void>(path_store.Clone(2));
    });
    CHECK(path_metadata == metadata_before_failed_clone);
    CHECK(path_zone == zone_before_failed_clone);
    CHECK(recordsEqual(path_store.Read(2), original_root));
}

void testMaximumCapacitySparseGeometry() {
    constexpr std::size_t wide_header_bytes = 8;
    constexpr std::uint64_t wide_stride =
        wide_header_bytes + TrieNodeStore::kPersistentNodeBytes;
    constexpr std::uint64_t exact_bytes64 =
        wide_stride * TrieNodeStore::kMaximumNodeCapacity;
    constexpr std::uint64_t over_bytes64 = exact_bytes64 + wide_stride;
    static_assert(
        over_bytes64 <= std::numeric_limits<std::size_t>::max(),
        "capacity geometry test requires a 64-bit address space");
    const auto mapping_bytes = static_cast<std::size_t>(over_bytes64);
    void* mapping = ::mmap(
        nullptr,
        mapping_bytes,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
        -1,
        0);
    CHECK(mapping != MAP_FAILED);
    ScopedMapping scoped(mapping, mapping_bytes);

    std::vector<std::uint8_t> exact_metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    auto exact = TrieNodeStore::Initialize(
        exact_metadata.data(),
        exact_metadata.size(),
        mapping,
        static_cast<std::size_t>(exact_bytes64));
    CHECK(exact.Capacity() == TrieNodeStore::kMaximumNodeCapacity);

    std::vector<std::uint8_t> over_metadata(
        FixedBlockAllocator::kPersistentMetadataBytes, 0xa5);
    const auto over_metadata_before = over_metadata;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(TrieNodeStore::Initialize(
            over_metadata.data(),
            over_metadata.size(),
            mapping,
            mapping_bytes));
    });
    CHECK(over_metadata == over_metadata_before);
}

void exerciseGuardedStore(bool place_at_end) {
    const auto raw_page_size = ::sysconf(_SC_PAGESIZE);
    CHECK(raw_page_size > 0);
    const auto page_size = static_cast<std::size_t>(raw_page_size);
    CHECK(kStride < page_size);
    const auto mapping_bytes = page_size * 3U;
    void* mapping = ::mmap(
        nullptr,
        mapping_bytes,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    CHECK(mapping != MAP_FAILED);
    ScopedMapping scoped(mapping, mapping_bytes);
    auto* middle = static_cast<std::uint8_t*>(mapping) + page_size;
    CHECK(::mprotect(middle, page_size, PROT_READ | PROT_WRITE) == 0);
    auto* zone = place_at_end ? middle + page_size - kStride : middle;
    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    auto store = TrieNodeStore::Initialize(
        metadata.data(),
        metadata.size(),
        zone,
        kStride,
        FixedBlockHeaderWidth::Bytes2);
    TrieNodeRecord record;
    record.has_value = true;
    record.value_ref = TrieNodeCodec::kMaxValueRef;
    CHECK(store.Allocate(record) == 0);
    CHECK(recordsEqual(store.Read(0), record));
    store.Validate();
}

void testGuardPagesAndRandomRoundTrips() {
    exerciseGuardedStore(false);
    exerciseGuardedStore(true);

    constexpr std::size_t capacity = 2000;
    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> zone(kStride * capacity, 0xa5);
    auto store = TrieNodeStore::Initialize(
        metadata.data(),
        metadata.size(),
        zone.data(),
        zone.size(),
        FixedBlockHeaderWidth::Bytes2);
    std::mt19937_64 random(0x747269656e6f6465ULL);
    for (std::uint32_t id = 0; id < capacity; ++id) {
        TrieNodeRecord expected;
        expected.has_value = (random() & 1U) != 0;
        expected.value_ref = expected.has_value
            ? random() & TrieNodeCodec::kMaxValueRef
            : 0;
        for (auto& child : expected.children) {
            if (id != 0 && random() % 8U == 0) {
                child = static_cast<std::int32_t>(random() % id);
            }
        }
        const auto encoded = TrieNodeCodec::Encode(expected);
        CHECK(TrieNodeCodec::Encode(
                  TrieNodeCodec::Decode(encoded.data(), encoded.size())) ==
              encoded);
        CHECK(store.Allocate(expected) == id);
        CHECK(recordsEqual(store.Read(id), expected));
        if (id % 97U == 0) store.Validate();
    }
    store.Validate();
}

void runTrieNodeStoreTests() {
    testCodecGoldenBytesAndCanonicalValues();
    testStoreExactLayoutIdZeroReuseAndValidation();
    testRecoveryIgnoresDynamicStateAndPreflightsEdges();
    testRecoveryDerivesOneTreeAndIgnoresUnreachablePayloads();
    testRecoveryRejectsEmptyNonRootLeafWithoutWrites();
    testRecoveryDocumentsInRangeCanonicalPayloadTrustBoundary();
    testFullFanoutCloneAndPathCopy();
    testMaximumCapacitySparseGeometry();
    testGuardPagesAndRandomRoundTrips();
}

} // namespace

int main() {
    return runTest(runTrieNodeStoreTests);
}
