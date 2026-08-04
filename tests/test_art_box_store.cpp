#include "test_support.h"

#include "shm_art_box_store.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {

using kvspace::detail::AllocatorError;
using kvspace::detail::AllocatorErrorCode;
using kvspace::detail::ArtBoxNodeCodec;
using kvspace::detail::ArtBoxNodeKind;
using kvspace::detail::ArtBoxNodeLiveBitCounts;
using kvspace::detail::ArtBoxNodeLiveBitmaps;
using kvspace::detail::ArtBoxNodeRecord;
using kvspace::detail::ArtBoxNodeRefCodec;
using kvspace::detail::ArtBoxNodeStore;
using kvspace::detail::ArtBoxNodeStoreRegion;
using kvspace::detail::ArtBoxNodeStoreRegions;
using kvspace::detail::FixedBlockAllocator;
using kvspace::detail::FixedBlockHeaderWidth;

constexpr std::array<ArtBoxNodeKind, 4> kKinds = {
    ArtBoxNodeKind::Node4,
    ArtBoxNodeKind::Node16,
    ArtBoxNodeKind::Node48,
    ArtBoxNodeKind::Node256};

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

std::size_t kindIndex(ArtBoxNodeKind kind) {
    return static_cast<std::size_t>(kind) - 1U;
}

bool recordsEqual(
    const ArtBoxNodeRecord& left,
    const ArtBoxNodeRecord& right) {
    return left.prefix_ref == right.prefix_ref &&
        left.value_ref == right.value_ref &&
        left.prefix_len == right.prefix_len &&
        left.child_count == right.child_count &&
        left.kind == right.kind &&
        left.has_value == right.has_value &&
        left.keys == right.keys &&
        left.index == right.index &&
        left.children == right.children;
}

void store16(std::uint8_t* bytes, std::uint16_t value) {
    bytes[0] = static_cast<std::uint8_t>(value);
    bytes[1] = static_cast<std::uint8_t>(value >> 8U);
}

void store32(std::uint8_t* bytes, std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        bytes[index] = static_cast<std::uint8_t>(value >> shift);
    }
}

class StoreFixture final {
public:
    explicit StoreFixture(std::uint32_t capacity)
        : capacity_(capacity),
          header_bytes_(static_cast<std::size_t>(
              FixedBlockAllocator::MinimumHeaderWidth(capacity))) {
        for (std::size_t index = 0; index < kKinds.size(); ++index) {
            metadata_[index].resize(
                FixedBlockAllocator::kPersistentMetadataBytes, 0xa5);
            const auto stride =
                header_bytes_ + ArtBoxNodeCodec::kPayloadBytes[index];
            zones_[index].resize(
                static_cast<std::size_t>(capacity_) * stride, 0xa5);
        }
    }

    ArtBoxNodeStoreRegions Regions() {
        ArtBoxNodeStoreRegions result{};
        for (std::size_t index = 0; index < result.size(); ++index) {
            result[index] = ArtBoxNodeStoreRegion{
                metadata_[index].data(),
                metadata_[index].size(),
                zones_[index].data(),
                zones_[index].size()};
        }
        return result;
    }

    ArtBoxNodeStore Initialize() {
        return ArtBoxNodeStore::Initialize(Regions(), capacity_);
    }

    std::uint8_t* Payload(ArtBoxNodeKind kind, std::uint32_t local_id) {
        const auto index = kindIndex(kind);
        const auto stride =
            header_bytes_ + ArtBoxNodeCodec::kPayloadBytes[index];
        return zones_[index].data() +
            static_cast<std::size_t>(local_id) * stride + header_bytes_;
    }

    std::uint8_t* BlockHeader(ArtBoxNodeKind kind, std::uint32_t local_id) {
        const auto index = kindIndex(kind);
        const auto stride =
            header_bytes_ + ArtBoxNodeCodec::kPayloadBytes[index];
        return zones_[index].data() +
            static_cast<std::size_t>(local_id) * stride;
    }

    std::array<std::vector<std::uint8_t>, 4>& Metadata() {
        return metadata_;
    }
    std::array<std::vector<std::uint8_t>, 4>& Zones() { return zones_; }
    std::uint32_t Capacity() const noexcept { return capacity_; }
    std::size_t HeaderBytes() const noexcept { return header_bytes_; }

private:
    std::uint32_t capacity_;
    std::size_t header_bytes_;
    std::array<std::vector<std::uint8_t>, 4> metadata_;
    std::array<std::vector<std::uint8_t>, 4> zones_;
};

ArtBoxNodeRecord nodeOfKind(ArtBoxNodeKind kind) {
    ArtBoxNodeRecord result;
    result.kind = kind;
    return result;
}

void expectDecodeCorrupt(const std::vector<std::uint8_t>& encoded) {
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(
            ArtBoxNodeCodec::Decode(encoded.data(), encoded.size()));
    });
}

void testTaggedReferencesAndGoldenCodecs() {
    CHECK(ArtBoxNodeRefCodec::kEmpty == UINT32_MAX);
    CHECK(ArtBoxNodeRefCodec::kMaximumLocalId == UINT32_C(0x3ffffffe));
    CHECK(ArtBoxNodeRefCodec::kMaximumCapacity == UINT32_C(0x3fffffff));
    CHECK(ArtBoxNodeRefCodec::Encode(ArtBoxNodeKind::Node4, 0) == 0);
    CHECK(ArtBoxNodeRefCodec::Encode(ArtBoxNodeKind::Node16, 0) ==
          UINT32_C(0x40000000));
    CHECK(ArtBoxNodeRefCodec::Encode(ArtBoxNodeKind::Node48, 0) ==
          UINT32_C(0x80000000));
    CHECK(ArtBoxNodeRefCodec::Encode(ArtBoxNodeKind::Node256, 0) ==
          UINT32_C(0xc0000000));
    CHECK(ArtBoxNodeRefCodec::Encode(
              ArtBoxNodeKind::Node256,
              ArtBoxNodeRefCodec::kMaximumLocalId) == UINT32_C(0xfffffffe));
    for (const auto kind : kKinds) {
        const auto reference = ArtBoxNodeRefCodec::Encode(kind, 17);
        CHECK(ArtBoxNodeRefCodec::Kind(reference) == kind);
        CHECK(ArtBoxNodeRefCodec::LocalId(reference) == 17);
    }
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [] {
        static_cast<void>(ArtBoxNodeRefCodec::Encode(
            ArtBoxNodeKind::Node4, UINT32_C(0x3fffffff)));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [] {
        static_cast<void>(
            ArtBoxNodeRefCodec::Kind(ArtBoxNodeRefCodec::kEmpty));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [] {
        static_cast<void>(ArtBoxNodeRefCodec::LocalId(UINT32_C(0x3fffffff)));
    });

    ArtBoxNodeRecord node4;
    node4.kind = ArtBoxNodeKind::Node4;
    node4.prefix_ref = UINT64_C(0x0102030405060708);
    node4.prefix_len = UINT32_C(0x11223344);
    node4.has_value = true;
    node4.value_ref = UINT64_C(0x8877665544332211);
    node4.child_count = 2;
    node4.keys[0] = 0x10;
    node4.keys[1] = 0xf0;
    node4.children[0] =
        ArtBoxNodeRefCodec::Encode(ArtBoxNodeKind::Node16, 0);
    node4.children[1] =
        ArtBoxNodeRefCodec::Encode(ArtBoxNodeKind::Node256, 0);
    const auto bytes4 = ArtBoxNodeCodec::Encode(node4);
    CHECK(bytes4.size() == 48);
    const std::array<std::uint8_t, 8> prefix_golden = {
        0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    CHECK(std::equal(prefix_golden.begin(), prefix_golden.end(), bytes4.begin()));
    const std::array<std::uint8_t, 8> value_golden = {
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    CHECK(std::equal(
        value_golden.begin(), value_golden.end(), bytes4.begin() + 8));
    CHECK(bytes4[16] == 0x44 && bytes4[17] == 0x33 &&
          bytes4[18] == 0x22 && bytes4[19] == 0x11);
    CHECK(bytes4[20] == 2 && bytes4[21] == 0);
    CHECK(bytes4[22] == 1 && bytes4[23] == 1);
    CHECK(bytes4[24] == 0x10 && bytes4[25] == 0xf0 &&
          bytes4[26] == 0 && bytes4[27] == 0);
    CHECK(bytes4[28] == 0 && bytes4[31] == 0x40);
    CHECK(bytes4[32] == 0 && bytes4[35] == 0xc0);
    CHECK(std::all_of(
        bytes4.begin() + 44, bytes4.end(),
        [](std::uint8_t value) { return value == 0; }));
    CHECK(recordsEqual(
        ArtBoxNodeCodec::Decode(bytes4.data(), bytes4.size()), node4));
    CHECK(ArtBoxNodeCodec::Encode(
              ArtBoxNodeCodec::Decode(bytes4.data(), bytes4.size())) ==
          bytes4);

    ArtBoxNodeRecord node16;
    node16.kind = ArtBoxNodeKind::Node16;
    node16.child_count = 2;
    node16.keys[0] = 1;
    node16.keys[1] = 2;
    node16.children[0] =
        ArtBoxNodeRefCodec::Encode(ArtBoxNodeKind::Node4, 7);
    node16.children[1] =
        ArtBoxNodeRefCodec::Encode(ArtBoxNodeKind::Node48, 9);
    const auto bytes16 = ArtBoxNodeCodec::Encode(node16);
    CHECK(bytes16.size() == 104);
    CHECK(bytes16[24] == 1 && bytes16[25] == 2);
    CHECK(bytes16[40] == 7 && bytes16[43] == 0);
    CHECK(bytes16[44] == 9 && bytes16[47] == 0x80);
    CHECK(recordsEqual(
        ArtBoxNodeCodec::Decode(bytes16.data(), bytes16.size()), node16));

    ArtBoxNodeRecord node48;
    node48.kind = ArtBoxNodeKind::Node48;
    node48.child_count = 2;
    node48.index[5] = 1;
    node48.index[200] = 0;
    node48.children[0] =
        ArtBoxNodeRefCodec::Encode(ArtBoxNodeKind::Node4, 1);
    node48.children[1] =
        ArtBoxNodeRefCodec::Encode(ArtBoxNodeKind::Node16, 2);
    const auto bytes48 = ArtBoxNodeCodec::Encode(node48);
    CHECK(bytes48.size() == 472);
    CHECK(bytes48[24 + 5] == 1 && bytes48[24 + 200] == 0);
    CHECK(bytes48[280] == 1 && bytes48[283] == 0);
    CHECK(bytes48[284] == 2 && bytes48[287] == 0x40);
    CHECK(recordsEqual(
        ArtBoxNodeCodec::Decode(bytes48.data(), bytes48.size()), node48));

    ArtBoxNodeRecord node256;
    node256.kind = ArtBoxNodeKind::Node256;
    node256.has_value = true;  // Stored None: present with zero opaque ref.
    node256.child_count = 2;
    node256.children[0] =
        ArtBoxNodeRefCodec::Encode(ArtBoxNodeKind::Node4, 0);
    node256.children[255] =
        ArtBoxNodeRefCodec::Encode(ArtBoxNodeKind::Node256, 5);
    const auto bytes256 = ArtBoxNodeCodec::Encode(node256);
    CHECK(bytes256.size() == 1048);
    CHECK(bytes256[23] == 1);
    CHECK(bytes256[24] == 0 && bytes256[27] == 0);
    CHECK(bytes256[24 + 255 * 4] == 5 &&
          bytes256[24 + 255 * 4 + 3] == 0xc0);
    CHECK(recordsEqual(
        ArtBoxNodeCodec::Decode(bytes256.data(), bytes256.size()), node256));
}

void testCodecRejectsEveryNoncanonicalForm() {
    ArtBoxNodeRecord missing_prefix;
    missing_prefix.prefix_ref = 1;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(ArtBoxNodeCodec::Encode(missing_prefix));
    });
    ArtBoxNodeRecord absent_prefix_object;
    absent_prefix_object.prefix_len = 1;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(ArtBoxNodeCodec::Encode(absent_prefix_object));
    });
    ArtBoxNodeRecord noncanonical_missing;
    noncanonical_missing.value_ref = 1;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(ArtBoxNodeCodec::Encode(noncanonical_missing));
    });
    ArtBoxNodeRecord unsorted;
    unsorted.child_count = 2;
    unsorted.keys[0] = 9;
    unsorted.keys[1] = 8;
    unsorted.children[0] = 0;
    unsorted.children[1] = 1;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(ArtBoxNodeCodec::Encode(unsorted));
    });

    auto node4 = ArtBoxNodeCodec::Encode(nodeOfKind(ArtBoxNodeKind::Node4));
    auto corrupt = node4;
    corrupt[23] = 2;
    expectDecodeCorrupt(corrupt);
    corrupt = node4;
    corrupt[8] = 1;
    expectDecodeCorrupt(corrupt);
    corrupt = node4;
    corrupt[0] = 1;
    expectDecodeCorrupt(corrupt);
    corrupt = node4;
    corrupt[44] = 1;
    expectDecodeCorrupt(corrupt);
    corrupt = node4;
    store16(corrupt.data() + 20, 1);
    store32(corrupt.data() + 28, UINT32_C(0x3fffffff));
    expectDecodeCorrupt(corrupt);
    corrupt = node4;
    store16(corrupt.data() + 20, 2);
    corrupt[24] = 2;
    corrupt[25] = 1;
    store32(corrupt.data() + 28, 0);
    store32(corrupt.data() + 32, 1);
    expectDecodeCorrupt(corrupt);
    corrupt = node4;
    store32(corrupt.data() + 28, 0);
    expectDecodeCorrupt(corrupt);

    ArtBoxNodeRecord valid48;
    valid48.kind = ArtBoxNodeKind::Node48;
    valid48.child_count = 2;
    valid48.index[10] = 0;
    valid48.index[20] = 1;
    valid48.children[0] = 0;
    valid48.children[1] = UINT32_C(0x40000000);
    const auto node48 = ArtBoxNodeCodec::Encode(valid48);
    corrupt = node48;
    corrupt[24 + 20] = 0;
    expectDecodeCorrupt(corrupt);
    corrupt = node48;
    corrupt[24 + 20] = 48;
    expectDecodeCorrupt(corrupt);
    corrupt = node48;
    corrupt[24 + 20] = UINT8_MAX;
    expectDecodeCorrupt(corrupt);
    corrupt = node48;
    store32(corrupt.data() + 280 + 4, UINT32_MAX);
    expectDecodeCorrupt(corrupt);
    corrupt = node48;
    store16(corrupt.data() + 20, 1);
    expectDecodeCorrupt(corrupt);

    auto node256 =
        ArtBoxNodeCodec::Encode(nodeOfKind(ArtBoxNodeKind::Node256));
    corrupt = node256;
    store32(corrupt.data() + 24, 0);
    expectDecodeCorrupt(corrupt);
    corrupt = node256;
    corrupt[22] = static_cast<std::uint8_t>(ArtBoxNodeKind::Node48);
    expectDecodeCorrupt(corrupt);

    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(ArtBoxNodeCodec::Decode(nullptr, node4.size()));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(
            ArtBoxNodeCodec::Decode(node4.data(), node4.size() - 1U));
    });
}

void testFourAllocatorStoreAndNormalValidation() {
    StoreFixture fixture(8);
    auto store = fixture.Initialize();
    CHECK(store.HeaderBytes() == 2);
    for (const auto kind : kKinds) CHECK(store.Capacity(kind) == 8);

    ArtBoxNodeRecord leaf;
    leaf.kind = ArtBoxNodeKind::Node4;
    leaf.has_value = true;
    CHECK(store.Allocate(leaf) == 0);  // Tagged reference zero is valid.

    ArtBoxNodeRecord node16;
    node16.kind = ArtBoxNodeKind::Node16;
    node16.child_count = 1;
    node16.keys[0] = 10;
    node16.children[0] = 0;
    const auto ref16 = store.Allocate(node16);
    CHECK(ref16 == UINT32_C(0x40000000));

    ArtBoxNodeRecord node48;
    node48.kind = ArtBoxNodeKind::Node48;
    node48.child_count = 1;
    node48.index[20] = 0;
    node48.children[0] = ref16;
    const auto ref48 = store.Allocate(node48);
    CHECK(ref48 == UINT32_C(0x80000000));

    ArtBoxNodeRecord node256;
    node256.kind = ArtBoxNodeKind::Node256;
    node256.child_count = 1;
    node256.children[30] = ref48;
    const auto root = store.Allocate(node256);
    CHECK(root == UINT32_C(0xc0000000));
    CHECK(recordsEqual(store.Read(0), leaf));
    CHECK(recordsEqual(store.Read(ref16), node16));
    CHECK(recordsEqual(store.Read(ref48), node48));
    CHECK(recordsEqual(store.Read(root), node256));
    store.Validate();

    const auto clone = store.Clone(root);
    CHECK(clone == UINT32_C(0xc0000001));
    CHECK(std::equal(
        fixture.Payload(ArtBoxNodeKind::Node256, 0),
        fixture.Payload(ArtBoxNodeKind::Node256, 0) + 1048,
        fixture.Payload(ArtBoxNodeKind::Node256, 1)));
    store.DiscardUnpublished(clone);
    CHECK(store.UsedCount(ArtBoxNodeKind::Node256) == 1);

    const auto reclaim4 = store.Allocate(nodeOfKind(ArtBoxNodeKind::Node4));
    const auto reclaim16 = store.Allocate(nodeOfKind(ArtBoxNodeKind::Node16));
    store.ReclaimPublished({reclaim4, reclaim16});
    CHECK(store.UsedCount(ArtBoxNodeKind::Node4) == 1);
    CHECK(store.UsedCount(ArtBoxNodeKind::Node16) == 1);
    expectAllocatorError(AllocatorErrorCode::NotAllocated, [&] {
        static_cast<void>(store.Read(reclaim4));
    });

    ArtBoxNodeRecord dangling;
    dangling.child_count = 1;
    dangling.keys[0] = 1;
    dangling.children[0] = ArtBoxNodeRefCodec::Encode(
        ArtBoxNodeKind::Node4, fixture.Capacity() - 1U);
    const auto metadata_before = fixture.Metadata();
    const auto zones_before = fixture.Zones();
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(store.Allocate(dangling));
    });
    CHECK(fixture.Metadata() == metadata_before);
    CHECK(fixture.Zones() == zones_before);

    auto attached = ArtBoxNodeStore::Attach(fixture.Regions(), 8);
    CHECK(recordsEqual(attached.Read(root), node256));
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(attached.ReadForRecovery(root));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidId, [&] {
        static_cast<void>(attached.Read(ArtBoxNodeRefCodec::kEmpty));
    });

    // No persisted address is process-local: copied bytes attach through a
    // different set of backing addresses without any fixup.
    auto copied_metadata = fixture.Metadata();
    auto copied_zones = fixture.Zones();
    ArtBoxNodeStoreRegions copied_regions{};
    for (std::size_t index = 0; index < copied_regions.size(); ++index) {
        copied_regions[index] = ArtBoxNodeStoreRegion{
            copied_metadata[index].data(),
            copied_metadata[index].size(),
            copied_zones[index].data(),
            copied_zones[index].size()};
    }
    auto copied = ArtBoxNodeStore::Attach(copied_regions, 8);
    CHECK(recordsEqual(copied.Read(root), node256));

    fixture.Payload(ArtBoxNodeKind::Node256, 0)[22] =
        static_cast<std::uint8_t>(ArtBoxNodeKind::Node48);
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(attached.Read(root));
    });
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(ArtBoxNodeStore::Attach(fixture.Regions(), 8));
    });

    StoreFixture tight_fixture(1);
    auto tight = tight_fixture.Initialize();
    CHECK(tight.Allocate(nodeOfKind(ArtBoxNodeKind::Node4)) == 0);
    const auto tight_metadata_before = tight_fixture.Metadata();
    const auto tight_zones_before = tight_fixture.Zones();
    expectAllocatorError(AllocatorErrorCode::Capacity, [&] {
        static_cast<void>(tight.Allocate(nodeOfKind(ArtBoxNodeKind::Node4)));
    });
    CHECK(tight_fixture.Metadata() == tight_metadata_before);
    CHECK(tight_fixture.Zones() == tight_zones_before);
}

void allocateUntilIdThree(
    ArtBoxNodeStore* store,
    ArtBoxNodeKind kind,
    const ArtBoxNodeRecord& live,
    std::uint32_t* live_ref) {
    for (std::uint32_t id = 0; id < 3; ++id) {
        auto dummy = nodeOfKind(kind);
        const auto reference = store->Allocate(dummy);
        CHECK(ArtBoxNodeRefCodec::LocalId(reference) == id);
    }
    *live_ref = store->Allocate(live);
    CHECK(ArtBoxNodeRefCodec::LocalId(*live_ref) == 3);
}

void testRecoveryFourBitmapsTornStateAndDescendingFreeChains() {
    StoreFixture fixture(8);
    auto store = fixture.Initialize();

    ArtBoxNodeRecord live4;
    live4.kind = ArtBoxNodeKind::Node4;
    live4.has_value = true;
    std::uint32_t ref4 = 0;
    allocateUntilIdThree(&store, ArtBoxNodeKind::Node4, live4, &ref4);

    ArtBoxNodeRecord live16;
    live16.kind = ArtBoxNodeKind::Node16;
    live16.child_count = 1;
    live16.keys[0] = 1;
    live16.children[0] = ref4;
    std::uint32_t ref16 = 0;
    allocateUntilIdThree(&store, ArtBoxNodeKind::Node16, live16, &ref16);

    ArtBoxNodeRecord live48;
    live48.kind = ArtBoxNodeKind::Node48;
    live48.child_count = 1;
    live48.index[2] = 0;
    live48.children[0] = ref16;
    std::uint32_t ref48 = 0;
    allocateUntilIdThree(&store, ArtBoxNodeKind::Node48, live48, &ref48);

    ArtBoxNodeRecord live256;
    live256.kind = ArtBoxNodeKind::Node256;
    live256.child_count = 1;
    live256.children[3] = ref48;
    std::uint32_t root = 0;
    allocateUntilIdThree(
        &store, ArtBoxNodeKind::Node256, live256, &root);

    for (std::size_t index = 0; index < kKinds.size(); ++index) {
        std::fill(
            fixture.Metadata()[index].begin() + 36,
            fixture.Metadata()[index].begin() + 48,
            UINT8_MAX);
        std::memset(
            fixture.BlockHeader(kKinds[index], 3),
            0,
            fixture.HeaderBytes());
    }
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(ArtBoxNodeStore::Attach(fixture.Regions(), 8));
    });

    auto recovery = ArtBoxNodeStore::AttachForRecovery(fixture.Regions(), 8);
    CHECK(recordsEqual(recovery.ReadForRecovery(root), live256));
    CHECK(recordsEqual(recovery.ReadForRecovery(ref48), live48));
    CHECK(recordsEqual(recovery.ReadForRecovery(ref16), live16));
    CHECK(recordsEqual(recovery.ReadForRecovery(ref4), live4));
    ArtBoxNodeLiveBitmaps live_bitmaps;
    for (auto& bitmap : live_bitmaps) bitmap.resize(1, 0x08);
    const ArtBoxNodeLiveBitCounts live_counts = {4, 4, 4, 4};
    const auto normal =
        std::move(recovery).Rebuild(live_bitmaps, live_counts);
    for (const auto kind : kKinds) {
        CHECK(normal.UsedCount(kind) == 1);
        CHECK(normal.HighWater(kind) == 4);
    }
    CHECK(recordsEqual(normal.Read(root), live256));
    normal.Validate();

    auto mutable_normal = ArtBoxNodeStore::Attach(fixture.Regions(), 8);
    for (const auto kind : kKinds) {
        const auto reference = mutable_normal.Allocate(nodeOfKind(kind));
        CHECK(ArtBoxNodeRefCodec::LocalId(reference) == 2);
        mutable_normal.DiscardUnpublished(reference);
    }
    const auto stable_metadata = fixture.Metadata();
    const auto stable_zones = fixture.Zones();
    auto second = ArtBoxNodeStore::AttachForRecovery(fixture.Regions(), 8);
    auto second_normal =
        std::move(second).RecoverFromCommittedRoot(root);
    CHECK(fixture.Metadata() == stable_metadata);
    CHECK(fixture.Zones() == stable_zones);
    second_normal.Validate();
}

void testRecoveryPreflightAndCorruptionNoWrites() {
    {
        StoreFixture fixture(8);
        auto store = fixture.Initialize();
        // Allocate cannot create a forward/self edge; publish a canonical
        // empty node, then simulate a corrupt committed self edge in bytes.
        const auto root = store.Allocate(nodeOfKind(ArtBoxNodeKind::Node4));
        store16(fixture.Payload(ArtBoxNodeKind::Node4, 0) + 20, 1);
        fixture.Payload(ArtBoxNodeKind::Node4, 0)[24] = 1;
        store32(fixture.Payload(ArtBoxNodeKind::Node4, 0) + 28, root);
        const auto metadata_before = fixture.Metadata();
        const auto zones_before = fixture.Zones();
        auto recovery =
            ArtBoxNodeStore::AttachForRecovery(fixture.Regions(), 8);
        expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
            static_cast<void>(
                std::move(recovery).RecoverFromCommittedRoot(root));
        });
        CHECK(fixture.Metadata() == metadata_before);
        CHECK(fixture.Zones() == zones_before);
    }

    {
        StoreFixture fixture(8);
        auto store = fixture.Initialize();
        const auto root = store.Allocate(nodeOfKind(ArtBoxNodeKind::Node4));
        store16(fixture.Payload(ArtBoxNodeKind::Node4, 0) + 20, 1);
        fixture.Payload(ArtBoxNodeKind::Node4, 0)[24] = 1;
        store32(
            fixture.Payload(ArtBoxNodeKind::Node4, 0) + 28,
            ArtBoxNodeRefCodec::Encode(ArtBoxNodeKind::Node16, 8));
        const auto metadata_before = fixture.Metadata();
        const auto zones_before = fixture.Zones();
        auto recovery =
            ArtBoxNodeStore::AttachForRecovery(fixture.Regions(), 8);
        expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
            static_cast<void>(
                std::move(recovery).RecoverFromCommittedRoot(root));
        });
        CHECK(fixture.Metadata() == metadata_before);
        CHECK(fixture.Zones() == zones_before);
    }

    // A bad fourth bitmap is rejected before the first allocator metadata
    // write, demonstrating the explicit two-phase apply boundary.
    {
        StoreFixture fixture(8);
        auto store = fixture.Initialize();
        static_cast<void>(store.Allocate(nodeOfKind(ArtBoxNodeKind::Node4)));
        const auto metadata_before = fixture.Metadata();
        const auto zones_before = fixture.Zones();
        auto recovery =
            ArtBoxNodeStore::AttachForRecovery(fixture.Regions(), 8);
        ArtBoxNodeLiveBitmaps bitmaps;
        for (auto& bitmap : bitmaps) bitmap.resize(1, 0);
        bitmaps[0][0] = 1;
        ArtBoxNodeLiveBitCounts counts = {1, 0, 0, 9};
        expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
            static_cast<void>(std::move(recovery).Rebuild(bitmaps, counts));
        });
        CHECK(fixture.Metadata() == metadata_before);
        CHECK(fixture.Zones() == zones_before);
    }

    // Empty-root recovery authoritatively clears every allocator.
    {
        StoreFixture fixture(8);
        auto store = fixture.Initialize();
        for (const auto kind : kKinds) {
            static_cast<void>(store.Allocate(nodeOfKind(kind)));
        }
        auto recovery =
            ArtBoxNodeStore::AttachForRecovery(fixture.Regions(), 8);
        auto normal = std::move(recovery).RecoverFromCommittedRoot(
            ArtBoxNodeRefCodec::kEmpty);
        for (const auto kind : kKinds) {
            CHECK(normal.UsedCount(kind) == 0);
            CHECK(normal.HighWater(kind) == 0);
        }
        normal.Validate();
    }
}

ArtBoxNodeStoreRegions syntheticRegions(
    std::array<std::array<std::uint8_t, 64>, 4>* metadata,
    std::uint32_t capacity) {
    const auto width = static_cast<std::size_t>(
        FixedBlockAllocator::MinimumHeaderWidth(capacity));
    ArtBoxNodeStoreRegions regions{};
    for (std::size_t index = 0; index < regions.size(); ++index) {
        const auto stride = width + ArtBoxNodeCodec::kPayloadBytes[index];
        CHECK(capacity <= std::numeric_limits<std::size_t>::max() / stride);
        // Initialize never touches a block zone while high-water is zero.
        // A stable non-null token lets this sparse geometry test avoid a
        // multi-terabyte virtual mapping at the upper local-ID boundary.
        regions[index] = ArtBoxNodeStoreRegion{
            (*metadata)[index].data(),
            (*metadata)[index].size(),
            reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x1000)),
            static_cast<std::size_t>(capacity) * stride};
    }
    return regions;
}

void testExactGeometryAndTwoFourEightHeaderBoundaries() {
    CHECK(FixedBlockAllocator::MinimumHeaderWidth(UINT32_C(1) << 14U) ==
          FixedBlockHeaderWidth::Bytes2);
    CHECK(FixedBlockAllocator::MinimumHeaderWidth(
              (UINT32_C(1) << 14U) + 1U) == FixedBlockHeaderWidth::Bytes4);
    CHECK(FixedBlockAllocator::MinimumHeaderWidth(UINT32_C(1) << 30U) ==
          FixedBlockHeaderWidth::Bytes4);
    CHECK(FixedBlockAllocator::MinimumHeaderWidth(
              (UINT32_C(1) << 30U) + 1U) == FixedBlockHeaderWidth::Bytes8);

    std::array<std::array<std::uint8_t, 64>, 4> metadata2{};
    auto regions2 = syntheticRegions(&metadata2, UINT32_C(1) << 14U);
    auto store2 = ArtBoxNodeStore::Initialize(
        regions2, UINT32_C(1) << 14U);
    CHECK(store2.HeaderBytes() == 2);

    std::array<std::array<std::uint8_t, 64>, 4> metadata4{};
    auto regions4 = syntheticRegions(
        &metadata4, (UINT32_C(1) << 14U) + 1U);
    auto store4 = ArtBoxNodeStore::Initialize(
        regions4, (UINT32_C(1) << 14U) + 1U);
    CHECK(store4.HeaderBytes() == 4);

    std::array<std::array<std::uint8_t, 64>, 4> metadata_max{};
    auto regions_max = syntheticRegions(
        &metadata_max, ArtBoxNodeRefCodec::kMaximumCapacity);
    auto store_max = ArtBoxNodeStore::Initialize(
        regions_max, ArtBoxNodeRefCodec::kMaximumCapacity);
    CHECK(store_max.HeaderBytes() == 4);

    std::array<std::array<std::uint8_t, 64>, 4> metadata8{};
    for (auto& item : metadata8) item.fill(0xa5);
    const auto metadata8_before = metadata8;
    const auto first_eight_byte_capacity =
        (UINT32_C(1) << 30U) + 1U;
    auto regions8 = syntheticRegions(&metadata8, first_eight_byte_capacity);
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(ArtBoxNodeStore::Initialize(
            regions8, first_eight_byte_capacity));
    });
    CHECK(metadata8 == metadata8_before);

    StoreFixture exact(3);
    auto bad_regions = exact.Regions();
    ++bad_regions[2].node_zone_bytes;
    const auto exact_metadata_before = exact.Metadata();
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(ArtBoxNodeStore::Initialize(bad_regions, 3));
    });
    CHECK(exact.Metadata() == exact_metadata_before);
}

void runArtBoxStoreTests() {
    testTaggedReferencesAndGoldenCodecs();
    testCodecRejectsEveryNoncanonicalForm();
    testFourAllocatorStoreAndNormalValidation();
    testRecoveryFourBitmapsTornStateAndDescendingFreeChains();
    testRecoveryPreflightAndCorruptionNoWrites();
    testExactGeometryAndTwoFourEightHeaderBoundaries();
}

} // namespace

int main() {
    return runTest(runArtBoxStoreTests);
}
