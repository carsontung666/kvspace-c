#include "test_support.h"

#include "shm_art_bump_store.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace {
bool fail_allocations = false;
}

void* operator new(std::size_t bytes) {
    if (fail_allocations) throw std::bad_alloc();
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t bytes) {
    return ::operator new(bytes);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

using kvspace::detail::AllocatorError;
using kvspace::detail::AllocatorErrorCode;
using kvspace::detail::ArtBumpCompactJournal;
using kvspace::detail::ArtBumpEngineHeader;
using kvspace::detail::ArtBumpGeometry;
using kvspace::detail::ArtBumpHeaderCodec;
using kvspace::detail::ArtBumpHeaderView;
using kvspace::detail::ArtBumpJournalState;
using kvspace::detail::ArtBumpNodeCodec;
using kvspace::detail::ArtBumpNodeKind;
using kvspace::detail::ArtBumpNodeLiveBitCounts;
using kvspace::detail::ArtBumpNodeLiveBitmaps;
using kvspace::detail::ArtBumpNodeRecord;
using kvspace::detail::ArtBumpNodeStore;
using kvspace::detail::ArtBumpRawZone;
using kvspace::detail::ArtBumpReadyField;
using kvspace::detail::ArtBumpRebuildCut;
using kvspace::detail::ArtBumpSlabDescriptor;

constexpr std::array<ArtBumpNodeKind, 4> kKinds = {
    ArtBumpNodeKind::Node4,
    ArtBumpNodeKind::Node16,
    ArtBumpNodeKind::Node48,
    ArtBumpNodeKind::Node256};

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

std::uint32_t load32(const std::uint8_t* bytes) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

std::uint64_t load64(const std::uint8_t* bytes) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

void store32(std::uint8_t* bytes, std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
}

void store64(std::uint8_t* bytes, std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
}

std::uint64_t align64(std::uint64_t value) {
    return (value + 63U) & ~UINT64_C(63);
}

bool recordsEqual(
    const ArtBumpNodeRecord& left,
    const ArtBumpNodeRecord& right) {
    return left.prefix_offset == right.prefix_offset &&
        left.value_offset == right.value_offset &&
        left.prefix_len == right.prefix_len &&
        left.child_count == right.child_count &&
        left.kind == right.kind &&
        left.has_value == right.has_value &&
        left.keys == right.keys && left.index == right.index &&
        left.children == right.children;
}

void setLive(
    ArtBumpNodeLiveBitmaps* bitmaps,
    ArtBumpNodeLiveBitCounts* bit_counts,
    const ArtBumpNodeStore& store,
    std::uint64_t reference) {
    const auto kind_index = static_cast<std::size_t>(
        store.ReferenceKind(reference)) - 1U;
    const auto id = store.SlotIndex(reference);
    (*bitmaps)[kind_index][static_cast<std::size_t>(id) / 8U] |=
        static_cast<std::uint8_t>(
            1U << static_cast<unsigned>(id % 8U));
    (*bit_counts)[kind_index] = std::max(
        (*bit_counts)[kind_index],
        static_cast<std::size_t>(id) + 1U);
}

void testDefaultAndGeneralGeometryGoldens() {
    CHECK(ArtBumpGeometry::QueueCapacity(1) == 2);
    CHECK(ArtBumpGeometry::QueueCapacity(4096) == 8192);
    const auto geometry = ArtBumpGeometry::DefaultProfile();
    CHECK(geometry.region_size == UINT64_C(1073741824));
    CHECK(geometry.queue_capacity == 8192);
    CHECK(geometry.queue_offset == 4096);
    CHECK(geometry.queue_table_bytes == 262144);
    CHECK(geometry.heap_offset == 266240);
    CHECK(geometry.heap_bytes == 134180864);
    CHECK(geometry.engine_offset == 134447104);
    CHECK(geometry.engine_bytes == 939294720);
    CHECK(geometry.node_capacity == 262145);
    CHECK(geometry.slabs[0].metadata_offset == 134447488);
    CHECK(geometry.slabs[0].zone_offset == 134447552);
    CHECK(geometry.slabs[0].zone_bytes == 16777280);
    CHECK(geometry.slabs[1].metadata_offset == 151224832);
    CHECK(geometry.slabs[1].zone_offset == 151224896);
    CHECK(geometry.slabs[1].zone_bytes == 44040360);
    CHECK(geometry.slabs[2].metadata_offset == 195265280);
    CHECK(geometry.slabs[2].zone_offset == 195265344);
    CHECK(geometry.slabs[2].zone_bytes == 174064280);
    CHECK(geometry.slabs[3].metadata_offset == 369329664);
    CHECK(geometry.slabs[3].zone_offset == 369329728);
    CHECK(geometry.slabs[3].zone_bytes == 543164440);
    CHECK(geometry.raw_zones[0].begin == 912494208);
    CHECK(geometry.raw_zones[0].bytes == 80623808);
    CHECK(geometry.raw_zones[1].begin == 993118016);
    CHECK(geometry.raw_zones[1].bytes == 80623808);
    CHECK(geometry.terminal_tail_bytes == 0);
    CHECK(ArtBumpHeaderCodec::EngineLayoutHash() ==
          UINT64_C(0xb3d5c1220e5bba30));
    CHECK(geometry.geometry_hash == UINT64_C(0xb246b66f5ac6da9d));
    const std::array<std::uint64_t, 4> slab_hashes = {
        UINT64_C(0x63a9f2b507e0dee1),
        UINT64_C(0x7e5221948b3c88b6),
        UINT64_C(0xb63348d0c06bd177),
        UINT64_C(0x833588ef1c55f705)};
    for (std::size_t index = 0; index < slab_hashes.size(); ++index) {
        CHECK(ArtBumpHeaderCodec::SlabGeometryHash(
                  static_cast<std::uint32_t>(
                      ArtBumpNodeCodec::kPayloadBytes[index]),
                  geometry.slabs[index].zone_offset,
                  geometry.slabs[index].zone_bytes,
                  geometry.node_capacity) == slab_hashes[index]);
    }

    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [] {
        static_cast<void>(ArtBumpGeometry::QueueCapacity(0));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [] {
        static_cast<void>(ArtBumpGeometry::Compute(1U << 20U, 0, 2));
    });
    expectAllocatorError(AllocatorErrorCode::Capacity, [] {
        static_cast<void>(ArtBumpGeometry::Compute(
            1U << 20U,
            static_cast<std::uint64_t>(UINT32_MAX) / 4U + 1U,
            2));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [] {
        static_cast<void>(ArtBumpGeometry::Compute(
            1U << 20U, 1, 2, 8192));
    });
}

void testHeaderAndJournalGoldenCodecs() {
    const auto geometry = ArtBumpGeometry::DefaultProfile();
    auto header = geometry.InitialHeader();
    const auto encoded = ArtBumpHeaderCodec::Encode(header);
    CHECK(encoded.size() == 384);
    const std::array<std::uint8_t, 8> magic = {
        'K', 'V', 'A', 'B', 'U', 'M', 'P', '1'};
    CHECK(std::equal(magic.begin(), magic.end(), encoded.begin()));
    CHECK(load32(encoded.data() + 8U) == 1);
    CHECK(load32(encoded.data() + 12U) == 384);
    CHECK(load64(encoded.data() + 16U) == 0);
    CHECK(load32(encoded.data() + 24U) == 0);
    CHECK(load64(encoded.data() + 32U) ==
          geometry.slabs[0].metadata_offset);
    CHECK(load64(encoded.data() + 48U) == geometry.slabs[0].zone_offset);
    CHECK(load64(encoded.data() + 160U) == geometry.raw_zones[0].begin);
    CHECK(load64(encoded.data() + 176U) == geometry.raw_zones[0].begin);
    CHECK(load32(encoded.data() + 184U) == 1);
    CHECK(load64(encoded.data() + 224U) == geometry.geometry_hash);
    CHECK(load32(encoded.data() + 232U) == geometry.node_capacity);
    CHECK(load32(encoded.data() + 236U) == 64);
    CHECK(load32(encoded.data() + 248U) == 2072);
    CHECK(std::all_of(
        encoded.begin() + 252U,
        encoded.end(),
        [](std::uint8_t value) { return value == 0; }));
    const auto decoded = ArtBumpHeaderCodec::Decode(
        encoded.data(), encoded.size());
    CHECK(decoded.geometry_hash == header.geometry_hash);
    CHECK(decoded.slabs[3].zone_bytes == header.slabs[3].zone_bytes);

    ArtBumpCompactJournal journal;
    journal.old_root = UINT64_C(0x0102030405060708);
    journal.source_top = geometry.raw_zones[0].begin + 9U;
    journal.operation_generation = UINT64_C(0x1122334455667788);
    journal.source_zone = 0;
    journal.target_zone = 1;
    journal.source_epoch = 7;
    journal.target_epoch = 9;
    journal.state = ArtBumpJournalState::Copying;
    journal.base_checksum = ArtBumpHeaderCodec::BaseChecksum(journal);
    auto journal_bytes = ArtBumpHeaderCodec::EncodeJournal(journal);
    CHECK(load64(journal_bytes.data()) == journal.old_root);
    CHECK(load64(journal_bytes.data() + 8U) == journal.source_top);
    CHECK(load64(journal_bytes.data() + 24U) == journal.base_checksum);
    CHECK(load32(journal_bytes.data() + 80U) == 1);
    CHECK(load32(journal_bytes.data() + 84U) == 0);
    CHECK(load32(journal_bytes.data() + 88U) == 1);
    CHECK(std::all_of(
        journal_bytes.begin() + 100U,
        journal_bytes.end(),
        [](std::uint8_t value) { return value == 0; }));
    CHECK(ArtBumpHeaderCodec::DecodeJournal(
              journal_bytes.data(), journal_bytes.size()).base_checksum ==
          journal.base_checksum);

    alignas(8) std::array<std::uint8_t, 384> copying_persistent{};
    auto copying_view = ArtBumpHeaderView::Initialize(
        copying_persistent.data(), copying_persistent.size(), header);
    copying_view.StoreJournalPayload(journal);
    copying_view.StoreJournalStateRelease(ArtBumpJournalState::Copying);
    store32(copying_persistent.data() + 24U, UINT32_MAX);
    store64(copying_persistent.data() + 208U, UINT64_MAX);
    store32(copying_persistent.data() + 216U, 0);
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(ArtBumpHeaderCodec::Decode(
            copying_persistent.data(), copying_persistent.size()));
    });
    const auto copying_recovery = ArtBumpHeaderCodec::DecodeForRecovery(
        copying_persistent.data(), copying_persistent.size());
    CHECK(copying_recovery.journal.state == ArtBumpJournalState::Copying);
    CHECK(copying_recovery.journal.source_top == journal.source_top);
    CHECK(copying_recovery.active_zone == UINT32_MAX);
    CHECK(copying_recovery.raw_zones[1].top == UINT64_MAX);
    CHECK(copying_recovery.raw_zones[1].epoch == 0);

    journal.new_root = UINT64_C(0x8899aabbccddeeff);
    journal.target_top = geometry.raw_zones[1].begin + 17U;
    journal.node_count = 4;
    journal.entry_count = 2;
    journal.engine_live_bytes = 9001;
    journal.state = ArtBumpJournalState::Ready;
    journal.ready_checksum = ArtBumpHeaderCodec::ReadyChecksum(journal);
    journal_bytes = ArtBumpHeaderCodec::EncodeJournal(journal);
    CHECK(load32(journal_bytes.data() + 80U) == 2);
    CHECK(load64(journal_bytes.data() + 72U) == journal.ready_checksum);

    auto corrupt = journal_bytes;
    corrupt[104] = 1;
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(ArtBumpHeaderCodec::DecodeJournal(
            corrupt.data(), corrupt.size()));
    });
    corrupt = journal_bytes;
    corrupt[24] ^= 1U;
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(ArtBumpHeaderCodec::DecodeJournal(
            corrupt.data(), corrupt.size()));
    });

    alignas(8) std::array<std::uint8_t, 384> persistent{};
    auto view = ArtBumpHeaderView::Initialize(
        persistent.data(), persistent.size(), header);
    CHECK(view.CommittedRootAcquire() == 0);
    view.StoreCommittedRootRelease(UINT64_C(0x1020304050607080));
    CHECK(view.CommittedRootAcquire() == UINT64_C(0x1020304050607080));
    view.StoreJournalPayload(journal);
    view.StoreJournalStateRelease(ArtBumpJournalState::Ready);
    CHECK(view.JournalAcquire().ready_checksum == journal.ready_checksum);

    auto copying = journal;
    copying.new_root = 0;
    copying.target_top = 0;
    copying.node_count = 0;
    copying.entry_count = 0;
    copying.engine_live_bytes = 0;
    copying.ready_checksum = 0;
    copying.state = ArtBumpJournalState::Copying;
    auto partial_view = ArtBumpHeaderView::Initialize(
        persistent.data(), persistent.size(), header);
    partial_view.StoreJournalPayload(copying);
    partial_view.StoreJournalStateRelease(ArtBumpJournalState::Copying);
    const std::array<ArtBumpReadyField, 6> ready_fields = {
        ArtBumpReadyField::NewRoot,
        ArtBumpReadyField::TargetTop,
        ArtBumpReadyField::NodeCount,
        ArtBumpReadyField::EntryCount,
        ArtBumpReadyField::EngineLiveBytes,
        ArtBumpReadyField::ReadyChecksum};
    for (const auto field : ready_fields) {
        partial_view.StoreJournalReadyField(journal, field);
        CHECK(partial_view.JournalAcquire().state ==
              ArtBumpJournalState::Copying);
    }
    CHECK(load64(persistent.data() + 288U) == journal.new_root);
    CHECK(load64(persistent.data() + 296U) == journal.target_top);
    CHECK(load64(persistent.data() + 304U) == journal.node_count);
    CHECK(load64(persistent.data() + 312U) == journal.entry_count);
    CHECK(load64(persistent.data() + 320U) == journal.engine_live_bytes);
    CHECK(load64(persistent.data() + 328U) == journal.ready_checksum);
    partial_view.StoreJournalStateRelease(ArtBumpJournalState::Ready);

    store32(persistent.data() + 24U, UINT32_MAX);
    store64(persistent.data() + 176U, UINT64_MAX);
    store32(persistent.data() + 184U, 0);
    store64(persistent.data() + 208U, UINT64_MAX);
    store32(persistent.data() + 216U, 0);
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(ArtBumpHeaderCodec::Decode(
            persistent.data(), persistent.size()));
    });
    const auto recovery_header = ArtBumpHeaderCodec::DecodeForRecovery(
        persistent.data(), persistent.size());
    CHECK(recovery_header.active_zone == UINT32_MAX);
    CHECK(recovery_header.raw_zones[0].top == UINT64_MAX);
    CHECK(recovery_header.raw_zones[0].epoch == 0);
    auto recovery_view = ArtBumpHeaderView::AttachForRecovery(
        persistent.data(), persistent.size());
    CHECK(recovery_view.DecodeForRecovery().journal.state ==
          ArtBumpJournalState::Ready);
}

void testAllNodeGoldenCodecsAndRejections() {
    ArtBumpNodeRecord node4;
    node4.prefix_offset = UINT64_C(0x0102030405060708);
    node4.prefix_len = UINT32_C(0x11223344);
    node4.value_offset = UINT64_C(0x8877665544332211);
    node4.has_value = true;
    node4.child_count = 2;
    node4.keys[0] = 0x10;
    node4.keys[1] = 0xf0;
    node4.children[0] = UINT64_C(0x1112131415161718);
    node4.children[1] = UINT64_C(0xa1a2a3a4a5a6a7a8);
    const auto bytes4 = ArtBumpNodeCodec::Encode(node4);
    CHECK(bytes4.size() == 64);
    CHECK(load64(bytes4.data()) == node4.prefix_offset);
    CHECK(load64(bytes4.data() + 8U) == node4.value_offset);
    CHECK(load32(bytes4.data() + 16U) == node4.prefix_len);
    CHECK(bytes4[20] == 2 && bytes4[22] == 1 && bytes4[23] == 1);
    CHECK(bytes4[24] == 0x10 && bytes4[25] == 0xf0);
    CHECK(std::all_of(
        bytes4.begin() + 28U,
        bytes4.begin() + 32U,
        [](std::uint8_t value) { return value == 0; }));
    CHECK(load64(bytes4.data() + 32U) == node4.children[0]);
    CHECK(recordsEqual(
        ArtBumpNodeCodec::Decode(bytes4.data(), bytes4.size()), node4));

    ArtBumpNodeRecord node16;
    node16.kind = ArtBumpNodeKind::Node16;
    node16.child_count = 1;
    node16.keys[0] = 7;
    node16.children[0] = 64;
    const auto bytes16 = ArtBumpNodeCodec::Encode(node16);
    CHECK(bytes16.size() == 168);
    CHECK(bytes16[24] == 7 && load64(bytes16.data() + 40U) == 64);

    ArtBumpNodeRecord node48;
    node48.kind = ArtBumpNodeKind::Node48;
    node48.child_count = 2;
    node48.index[3] = 1;
    node48.index[250] = 0;
    node48.children[0] = 64;
    node48.children[1] = 128;
    const auto bytes48 = ArtBumpNodeCodec::Encode(node48);
    CHECK(bytes48.size() == 664);
    CHECK(bytes48[27] == 1 && bytes48[274] == 0);
    CHECK(load64(bytes48.data() + 280U) == 64);

    ArtBumpNodeRecord node256;
    node256.kind = ArtBumpNodeKind::Node256;
    node256.has_value = true; // Stored None.
    node256.child_count = 2;
    node256.children[0] = 64;
    node256.children[255] = 128;
    const auto bytes256 = ArtBumpNodeCodec::Encode(node256);
    CHECK(bytes256.size() == 2072);
    CHECK(load64(bytes256.data() + 24U) == 64);
    CHECK(load64(bytes256.data() + 24U + 255U * 8U) == 128);

    auto corrupt = bytes4;
    corrupt[28] = 1;
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(ArtBumpNodeCodec::Decode(
            corrupt.data(), corrupt.size()));
    });
    corrupt = bytes4;
    corrupt[23] = 2;
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(ArtBumpNodeCodec::Decode(
            corrupt.data(), corrupt.size()));
    });
    auto corrupt48 = bytes48;
    corrupt48[27] = 0;
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(ArtBumpNodeCodec::Decode(
            corrupt48.data(), corrupt48.size()));
    });
}

class StoreFixture final {
public:
    explicit StoreFixture(std::uint32_t capacity) : capacity_(capacity) {
        std::uint64_t cursor = 64;
        for (std::size_t index = 0; index < descriptors_.size(); ++index) {
            auto& descriptor = descriptors_[index];
            descriptor.metadata_offset = cursor;
            descriptor.metadata_bytes = 64;
            descriptor.zone_offset = cursor + 64U;
            descriptor.zone_bytes =
                static_cast<std::uint64_t>(capacity_) *
                ArtBumpNodeCodec::kPayloadBytes[index];
            cursor = align64(descriptor.zone_offset + descriptor.zone_bytes);
        }
        bytes_.resize(static_cast<std::size_t>(cursor), 0xa5);
    }

    ArtBumpNodeStore Initialize() {
        return ArtBumpNodeStore::Initialize(
            bytes_.data(), bytes_.size(), descriptors_, capacity_);
    }
    ArtBumpNodeStore Attach() {
        return ArtBumpNodeStore::Attach(
            bytes_.data(), bytes_.size(), descriptors_, capacity_);
    }
    ArtBumpNodeStore AttachForRecovery() {
        return ArtBumpNodeStore::AttachForRecovery(
            bytes_.data(), bytes_.size(), descriptors_, capacity_);
    }
    std::uint8_t* Metadata(std::size_t kind_index) {
        return bytes_.data() + static_cast<std::size_t>(
            descriptors_[kind_index].metadata_offset);
    }
    std::uint8_t* Slot(std::size_t kind_index, std::uint32_t id) {
        return bytes_.data() + static_cast<std::size_t>(
            descriptors_[kind_index].zone_offset) +
            static_cast<std::size_t>(id) *
                ArtBumpNodeCodec::kPayloadBytes[kind_index];
    }
    std::uint32_t Capacity() const noexcept { return capacity_; }
    const std::vector<std::uint8_t>& Bytes() const noexcept { return bytes_; }

private:
    std::uint32_t capacity_;
    std::array<ArtBumpSlabDescriptor, 4> descriptors_{};
    std::vector<std::uint8_t> bytes_;
};

ArtBumpNodeLiveBitmaps emptyBitmaps(std::uint32_t capacity) {
    ArtBumpNodeLiveBitmaps result;
    for (auto& bitmap : result) {
        bitmap.resize((static_cast<std::size_t>(capacity) + 7U) / 8U, 0);
    }
    return result;
}

void testHeaderlessSlabsNormalAndPreparedRebuild() {
    StoreFixture fixture(8);
    auto store = fixture.Initialize();
    for (std::size_t index = 0; index < kKinds.size(); ++index) {
        const auto* metadata = fixture.Metadata(index);
        CHECK(std::memcmp(metadata, "KVASLAB1", 8) == 0);
        CHECK(load32(metadata + 8U) == 1);
        CHECK(load32(metadata + 12U) ==
              ArtBumpNodeCodec::kPayloadBytes[index]);
        CHECK(load32(metadata + 32U) == fixture.Capacity());
        CHECK(load32(metadata + 36U) == 0);
        CHECK(load32(metadata + 40U) == 0);
        CHECK(load64(metadata + 48U) == 0);
    }

    ArtBumpNodeRecord leaf;
    leaf.has_value = true;
    const auto leaf_ref = store.Allocate(leaf);

    ArtBumpNodeRecord node16;
    node16.kind = ArtBumpNodeKind::Node16;
    node16.child_count = 1;
    node16.keys[0] = 10;
    node16.children[0] = leaf_ref;
    const auto node16_ref = store.Allocate(node16);

    ArtBumpNodeRecord node48;
    node48.kind = ArtBumpNodeKind::Node48;
    node48.child_count = 1;
    node48.index[20] = 0;
    node48.children[0] = node16_ref;
    const auto node48_ref = store.Allocate(node48);

    ArtBumpNodeRecord node256;
    node256.kind = ArtBumpNodeKind::Node256;
    node256.child_count = 1;
    node256.children[200] = node48_ref;
    const auto root_ref = store.Allocate(node256);
    CHECK(recordsEqual(store.Read(root_ref), node256));

    const auto extra1 = store.Allocate(leaf);
    const auto extra2 = store.Allocate(leaf);
    CHECK(store.SlotIndex(extra1) == 1);
    CHECK(store.SlotIndex(extra2) == 2);

    auto live = emptyBitmaps(fixture.Capacity());
    ArtBumpNodeLiveBitCounts bit_counts{};
    for (const auto reference : {
             leaf_ref, node16_ref, node48_ref, root_ref, extra2}) {
        setLive(&live, &bit_counts, store, reference);
    }
    fail_allocations = true;
    auto prepared = [&] {
        try {
            return std::move(store).PrepareRebuild(
                std::move(live), bit_counts);
        } catch (...) {
            fail_allocations = false;
            throw;
        }
    }();
    std::array<std::array<bool, 3>, 4> rebuild_cuts{};
    const auto cut_hook = +[](void* context,
                              std::size_t slab_index,
                              ArtBumpRebuildCut cut) noexcept {
        auto* cuts = static_cast<
            std::array<std::array<bool, 3>, 4>*>(context);
        (*cuts)[slab_index][static_cast<std::size_t>(cut)] = true;
    };
    ArtBumpNodeStore rebuilt = [&] {
        try {
            return std::move(prepared).Apply(cut_hook, &rebuild_cuts);
        } catch (...) {
            fail_allocations = false;
            throw;
        }
    }();
    fail_allocations = false;
    for (const auto& slab_cuts : rebuild_cuts) {
        CHECK(std::all_of(
            slab_cuts.begin(), slab_cuts.end(),
            [](bool seen) { return seen; }));
    }
    CHECK(rebuilt.HighWater(ArtBumpNodeKind::Node4) == 3);
    CHECK(rebuilt.UsedCount(ArtBumpNodeKind::Node4) == 2);
    CHECK(rebuilt.FreeHead(ArtBumpNodeKind::Node4) == extra1);
    CHECK(load64(fixture.Slot(0, 1)) == 0);
    CHECK(std::all_of(
        fixture.Slot(0, 1) + 8U,
        fixture.Slot(0, 1) + 64U,
        [](std::uint8_t value) { return value == 0; }));

    // Allocation consumes the rebuilt head; subsequent prepends produce a
    // legal non-descending clean chain which Attach must preserve bytewise.
    const auto reused = rebuilt.Allocate(leaf);
    CHECK(reused == extra1);
    rebuilt.ReclaimPublished(extra2);
    rebuilt.ReclaimPublished(reused);
    const auto before_attach = fixture.Bytes();
    auto attached = fixture.Attach();
    CHECK(fixture.Bytes() == before_attach);
    CHECK(attached.Read(root_ref).children[200] == node48_ref);

    // Mutable metadata may be torn. Recovery attach ignores it, and the
    // prepared exact live set restores deterministic canonical metadata.
    store32(fixture.Metadata(0) + 36U, UINT32_MAX);
    auto recovery = fixture.AttachForRecovery();
    CHECK(recovery.ReadForRecovery(root_ref).children[200] == node48_ref);
    auto recovery_live = emptyBitmaps(fixture.Capacity());
    ArtBumpNodeLiveBitCounts recovery_counts{};
    for (const auto reference : {
             leaf_ref, node16_ref, node48_ref, root_ref}) {
        setLive(&recovery_live, &recovery_counts, recovery, reference);
    }
    auto recovery_prepared = std::move(recovery).PrepareRebuild(
        std::move(recovery_live), recovery_counts);
    auto recovered = std::move(recovery_prepared).Apply();
    CHECK(recovered.HighWater(ArtBumpNodeKind::Node4) == 1);
    CHECK(recovered.UsedCount(ArtBumpNodeKind::Node4) == 1);
    CHECK(recovered.FreeHead(ArtBumpNodeKind::Node4) == 0);
    static_cast<void>(fixture.Attach());
}

void testHistoricalHighWaterAtCapacityIsLegal() {
    constexpr std::uint64_t entry_limit = 2;
    constexpr std::uint32_t capacity = 4U * entry_limit + 1U;
    constexpr std::uint32_t maximum_live = 4U * entry_limit - 2U;
    StoreFixture fixture(capacity);
    auto store = fixture.Initialize();
    ArtBumpNodeRecord leaf;
    leaf.has_value = true;
    std::array<std::uint64_t, capacity> references{};
    for (auto& reference : references) {
        reference = store.Allocate(leaf);
    }
    CHECK(store.HighWater(ArtBumpNodeKind::Node4) == capacity);
    for (std::size_t index = maximum_live; index < references.size(); ++index) {
        store.ReclaimPublished(references[index]);
    }
    CHECK(store.HighWater(ArtBumpNodeKind::Node4) == capacity);
    CHECK(store.UsedCount(ArtBumpNodeKind::Node4) == maximum_live);
    const auto before_attach = fixture.Bytes();
    auto attached = fixture.Attach();
    CHECK(fixture.Bytes() == before_attach);
    CHECK(attached.HighWater(ArtBumpNodeKind::Node4) == capacity);
    CHECK(attached.UsedCount(ArtBumpNodeKind::Node4) == maximum_live);
}

void testRawZoneRangeBeforeReadAndPreparedState() {
    std::vector<std::uint8_t> region(256, 0);
    alignas(8) std::array<std::uint8_t, 32> descriptor{};
    store64(descriptor.data(), 64);
    store64(descriptor.data() + 8U, 32);
    store64(descriptor.data() + 16U, 64);
    store32(descriptor.data() + 24U, 1);
    auto zone = ArtBumpRawZone::Attach(
        region.data(), region.size(), descriptor.data(), descriptor.size(),
        64, 32);
    const std::array<std::uint8_t, 3> value = {4, 5, 6};
    const auto offset = zone.Allocate(value.data(), value.size());
    CHECK(offset == 64);
    CHECK(zone.TopAcquire() == 67);
    CHECK(zone.Remaining() == 29);
    const auto* read = zone.ReadBounded(offset, value.size(), 67);
    CHECK(std::equal(value.begin(), value.end(), read));
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(zone.ReadBounded(offset, 4, 67));
    });
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(zone.ReadBounded(63, 1, 67));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(zone.PrepareState(64, 0));
    });
    const auto reset = zone.PrepareState(64, 9);
    fail_allocations = true;
    reset.Apply();
    fail_allocations = false;
    CHECK(zone.TopAcquire() == 64);
    CHECK(zone.Epoch() == 9);

    // Active-journal recovery must attach using only immutable geometry even
    // when mutable descriptor fields are torn or stale.
    store64(descriptor.data() + 16U, UINT64_MAX);
    store32(descriptor.data() + 24U, 0);
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(ArtBumpRawZone::Attach(
            region.data(), region.size(), descriptor.data(), descriptor.size(),
            64, 32));
    });
    auto recovery = ArtBumpRawZone::AttachForRecovery(
        region.data(), region.size(), descriptor.data(), descriptor.size(),
        64, 32);
    CHECK(recovery.Begin() == 64);
    CHECK(recovery.Bytes() == 32);
    const auto repair = recovery.PrepareState(64, 3);
    repair.Apply();
    CHECK(recovery.TopAcquire() == 64);
    CHECK(recovery.Epoch() == 3);
}

void runArtBumpStoreTests() {
    testDefaultAndGeneralGeometryGoldens();
    testHeaderAndJournalGoldenCodecs();
    testAllNodeGoldenCodecsAndRejections();
        testHeaderlessSlabsNormalAndPreparedRebuild();
        testHistoricalHighWaterAtCapacityIsLegal();
        testRawZoneRangeBeforeReadAndPreparedState();
}

} // namespace

int main() {
    return runTest(runArtBumpStoreTests);
}
