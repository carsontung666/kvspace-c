#include "test_support.h"

#include "shm_box_allocators.h"
#include "shm_art_box_store.h"
#include "shm_trie_node_store.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <random>
#include <sys/mman.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

namespace allocator_failure_injection {

thread_local std::ptrdiff_t countdown = -1;

bool ShouldFail() noexcept {
    if (countdown < 0) return false;
    if (countdown == 0) {
        countdown = -1;
        return true;
    }
    --countdown;
    return false;
}

void FailAfter(std::size_t successful_allocations) noexcept {
    countdown = static_cast<std::ptrdiff_t>(successful_allocations);
}

void Disable() noexcept { countdown = -1; }

void* AllocateBytes(std::size_t size) {
    if (ShouldFail()) throw std::bad_alloc();
    if (void* result = std::malloc(size == 0 ? 1 : size); result != nullptr) {
        return result;
    }
    throw std::bad_alloc();
}

} // namespace allocator_failure_injection

void* operator new(std::size_t size) {
    return allocator_failure_injection::AllocateBytes(size);
}

void* operator new[](std::size_t size) {
    return allocator_failure_injection::AllocateBytes(size);
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept {
    std::free(pointer);
}

namespace {

using kvspace::detail::AllocatorError;
using kvspace::detail::AllocatorErrorCode;
using kvspace::detail::ArtBoxNodeKind;
using kvspace::detail::ArtBoxNodeLiveBitCounts;
using kvspace::detail::ArtBoxNodeLiveBitmaps;
using kvspace::detail::ArtBoxNodeRecord;
using kvspace::detail::ArtBoxNodeRefCodec;
using kvspace::detail::ArtBoxNodeStore;
using kvspace::detail::ArtBoxNodeStoreRegion;
using kvspace::detail::ArtBoxNodeStoreRegions;
using kvspace::detail::BoxAllocator;
using kvspace::detail::BoxLiveInterval;
using kvspace::detail::FixedBlockAllocator;
using kvspace::detail::FixedBlockHeaderWidth;
using kvspace::detail::TrieNodeRecord;
using kvspace::detail::TrieNodeStore;

static_assert(!std::is_copy_constructible<FixedBlockAllocator>::value);
static_assert(!std::is_copy_assignable<FixedBlockAllocator>::value);
static_assert(std::is_nothrow_move_constructible<FixedBlockAllocator>::value);
static_assert(std::is_nothrow_move_assignable<FixedBlockAllocator>::value);
static_assert(!std::is_copy_constructible<BoxAllocator>::value);
static_assert(std::is_nothrow_move_constructible<BoxAllocator>::value);
static_assert(
    !std::is_copy_constructible<BoxAllocator::PreparedRebuild>::value);
static_assert(
    std::is_nothrow_move_constructible<
        BoxAllocator::PreparedRebuild>::value);
static_assert(
    !std::is_copy_constructible<
        ArtBoxNodeStore::PreparedRecovery>::value);
static_assert(
    std::is_nothrow_move_constructible<
        ArtBoxNodeStore::PreparedRecovery>::value);
static_assert(
    !std::is_copy_constructible<
        TrieNodeStore::PreparedRecovery>::value);
static_assert(
    std::is_nothrow_move_constructible<
        TrieNodeStore::PreparedRecovery>::value);

constexpr std::size_t kBoxBlockMetadataOffset =
    BoxAllocator::kPersistentHeaderBytes;
constexpr std::size_t kBoxBlockZoneOffset =
    kBoxBlockMetadataOffset + FixedBlockAllocator::kPersistentMetadataBytes;
constexpr std::size_t kLargeBoxMetadataBytes = 64U * 1024U;
constexpr std::uint64_t kLargeBoxDataBytes = 32U * 1024U;

void testPreparedArtBoxAndBoxRecoveryAllocationOrdinals() {
    constexpr std::uint32_t capacity = 4;
    const auto header_bytes = static_cast<std::size_t>(
        FixedBlockAllocator::MinimumHeaderWidth(capacity));
    std::array<std::vector<std::uint8_t>, 4> baseline_node_metadata;
    std::array<std::vector<std::uint8_t>, 4> baseline_node_zones;
    for (std::size_t index = 0; index < baseline_node_metadata.size(); ++index) {
        baseline_node_metadata[index].resize(
            FixedBlockAllocator::kPersistentMetadataBytes);
        baseline_node_zones[index].resize(
            static_cast<std::size_t>(capacity) *
            (header_bytes +
             kvspace::detail::ArtBoxNodeCodec::kPayloadBytes[index]));
    }
    const auto make_regions = [](
        std::array<std::vector<std::uint8_t>, 4>* metadata,
        std::array<std::vector<std::uint8_t>, 4>* zones) {
        ArtBoxNodeStoreRegions regions{};
        for (std::size_t index = 0; index < regions.size(); ++index) {
            regions[index] = ArtBoxNodeStoreRegion{
                (*metadata)[index].data(),
                (*metadata)[index].size(),
                (*zones)[index].data(),
                (*zones)[index].size()};
        }
        return regions;
    };
    auto baseline_regions = make_regions(
        &baseline_node_metadata, &baseline_node_zones);
    auto baseline_nodes = ArtBoxNodeStore::Initialize(
        baseline_regions, capacity);
    for (std::size_t index = 0; index < 4; ++index) {
        ArtBoxNodeRecord record;
        record.kind = static_cast<ArtBoxNodeKind>(index + 1U);
        record.has_value = true;
        CHECK(ArtBoxNodeRefCodec::LocalId(
                  baseline_nodes.Allocate(record)) == 0);
    }
    ArtBoxNodeLiveBitmaps bitmaps;
    for (auto& bitmap : bitmaps) bitmap = {1};
    const ArtBoxNodeLiveBitCounts bit_counts = {1, 1, 1, 1};

    std::vector<std::uint8_t> baseline_box_metadata(kLargeBoxMetadataBytes);
    auto baseline_box = BoxAllocator::Initialize(
        baseline_box_metadata.data(),
        baseline_box_metadata.size(),
        kLargeBoxDataBytes);
    CHECK(baseline_box.Allocate(128) == 0);
    const std::vector<BoxLiveInterval> intervals = {
        {4096, 1}, {8192, 17}, {16384, 129}};

    const auto require_unchanged = [&baseline_node_metadata,
                                    &baseline_node_zones,
                                    &baseline_box_metadata](
        const std::array<std::vector<std::uint8_t>, 4>& metadata,
        const std::array<std::vector<std::uint8_t>, 4>& zones,
        const std::vector<std::uint8_t>& box_metadata) {
        CHECK(metadata == baseline_node_metadata);
        CHECK(zones == baseline_node_zones);
        CHECK(box_metadata == baseline_box_metadata);
    };

    std::size_t node_failures = 0;
    bool node_prepare_completed = false;
    for (std::size_t ordinal = 0; ordinal < 128; ++ordinal) {
        auto metadata = baseline_node_metadata;
        auto zones = baseline_node_zones;
        auto box_metadata = baseline_box_metadata;
        auto regions = make_regions(&metadata, &zones);
        auto recovery = ArtBoxNodeStore::AttachForRecovery(
            regions, capacity);
        bool failed = false;
        allocator_failure_injection::FailAfter(ordinal);
        try {
            static_cast<void>(
                std::move(recovery).PrepareRecovery(bitmaps, bit_counts));
        } catch (const std::bad_alloc&) {
            failed = true;
        } catch (...) {
            allocator_failure_injection::Disable();
            throw;
        }
        allocator_failure_injection::Disable();
        require_unchanged(metadata, zones, box_metadata);
        if (failed) {
            ++node_failures;
            continue;
        }
        node_prepare_completed = true;
        break;
    }
    CHECK(node_prepare_completed);
    CHECK(node_failures >= 8);

    std::size_t box_failures = 0;
    bool box_prepare_completed = false;
    for (std::size_t ordinal = 0; ordinal < 128; ++ordinal) {
        auto metadata = baseline_node_metadata;
        auto zones = baseline_node_zones;
        auto box_metadata = baseline_box_metadata;
        auto recovery = BoxAllocator::AttachForRecovery(
            box_metadata.data(), box_metadata.size(), kLargeBoxDataBytes);
        bool failed = false;
        allocator_failure_injection::FailAfter(ordinal);
        try {
            static_cast<void>(
                std::move(recovery).PrepareRebuild(intervals));
        } catch (const std::bad_alloc&) {
            failed = true;
        } catch (...) {
            allocator_failure_injection::Disable();
            throw;
        }
        allocator_failure_injection::Disable();
        require_unchanged(metadata, zones, box_metadata);
        if (failed) {
            ++box_failures;
            continue;
        }
        box_prepare_completed = true;
        break;
    }
    CHECK(box_prepare_completed);
    CHECK(box_failures >= 3);

    std::size_t combined_failures = 0;
    bool combined_completed = false;
    using RecoveryPlans = std::pair<
        ArtBoxNodeStore::PreparedRecovery,
        BoxAllocator::PreparedRebuild>;
    for (std::size_t ordinal = 0; ordinal < 256; ++ordinal) {
        auto metadata = baseline_node_metadata;
        auto zones = baseline_node_zones;
        auto box_metadata = baseline_box_metadata;
        auto regions = make_regions(&metadata, &zones);
        auto recovering_nodes = ArtBoxNodeStore::AttachForRecovery(
            regions, capacity);
        auto recovering_box = BoxAllocator::AttachForRecovery(
            box_metadata.data(), box_metadata.size(), kLargeBoxDataBytes);
        std::optional<RecoveryPlans> plans;
        bool failed = false;
        allocator_failure_injection::FailAfter(ordinal);
        try {
            auto node_plan = std::move(recovering_nodes).PrepareRecovery(
                bitmaps, bit_counts);
            auto box_plan =
                std::move(recovering_box).PrepareRebuild(intervals);
            plans.emplace(
                std::move(node_plan), std::move(box_plan));
        } catch (const std::bad_alloc&) {
            failed = true;
        } catch (...) {
            allocator_failure_injection::Disable();
            throw;
        }
        allocator_failure_injection::Disable();
        require_unchanged(metadata, zones, box_metadata);
        if (failed) {
            ++combined_failures;
            continue;
        }

        CHECK(plans.has_value());
        allocator_failure_injection::FailAfter(0);
        try {
            auto normal_nodes = std::move(plans->first).Apply();
            auto normal_box = std::move(plans->second).Apply();
            allocator_failure_injection::Disable();
            normal_nodes.Validate();
            normal_box.Validate();
            for (std::size_t index = 0; index < 4; ++index) {
                CHECK(normal_nodes.UsedCount(
                          static_cast<ArtBoxNodeKind>(index + 1U)) == 1);
            }
            CHECK(normal_box.AllocatedSize(4096) == 8);
            CHECK(normal_box.AllocatedSize(8192) == 24);
            CHECK(normal_box.AllocatedSize(16384) == 256);
        } catch (...) {
            allocator_failure_injection::Disable();
            throw;
        }
        combined_completed = true;
        break;
    }
    CHECK(combined_completed);
    CHECK(combined_failures >= node_failures + box_failures);
}

void store32(std::uint8_t* bytes, std::uint32_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        bytes[index] = static_cast<std::uint8_t>(value >> shift);
    }
}

void store64(std::uint8_t* bytes, std::uint64_t value) {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        const auto shift = static_cast<unsigned>(index * 8U);
        bytes[index] = static_cast<std::uint8_t>(value >> shift);
    }
}

void recomputeBlockImmutableHash(std::uint8_t* metadata) {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    const auto hash_byte = [&](std::uint8_t value) {
        hash ^= value;
        hash *= UINT64_C(1099511628211);
    };
    for (std::size_t offset = 0; offset < 36; ++offset) {
        hash_byte(metadata[offset]);
    }
    hash_byte(metadata[56]);
    store64(metadata + 48, hash);
}

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

bool overlaps(
    std::uint64_t left_offset,
    std::uint64_t left_size,
    std::uint64_t right_offset,
    std::uint64_t right_size) {
    return left_offset < right_offset + right_size &&
        right_offset < left_offset + left_size;
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

void testFixedStableFormatAndHeaderWidths() {
    CHECK(FixedBlockAllocator::kPersistentMetadataBytes == 64);
    CHECK(FixedBlockAllocator::MinimumHeaderWidth(1) ==
          FixedBlockHeaderWidth::Bytes2);
    CHECK(FixedBlockAllocator::MinimumHeaderWidth(1ULL << 14U) ==
          FixedBlockHeaderWidth::Bytes2);
    CHECK(FixedBlockAllocator::MinimumHeaderWidth((1ULL << 14U) + 1U) ==
          FixedBlockHeaderWidth::Bytes4);
    CHECK(FixedBlockAllocator::MinimumHeaderWidth(1ULL << 30U) ==
          FixedBlockHeaderWidth::Bytes4);
    CHECK(FixedBlockAllocator::MinimumHeaderWidth((1ULL << 30U) + 1U) ==
          FixedBlockHeaderWidth::Bytes8);
    CHECK(FixedBlockAllocator::MinimumHeaderWidth(UINT32_MAX) ==
          FixedBlockHeaderWidth::Bytes8);
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [] {
        static_cast<void>(FixedBlockAllocator::MinimumHeaderWidth(0));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [] {
        static_cast<void>(FixedBlockAllocator::MinimumHeaderWidth(
            static_cast<std::uint64_t>(UINT32_MAX) + 1U));
    });

    std::array<std::uint8_t, 65> oversized_metadata{};
    std::array<std::uint8_t, 10> one_block{};
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(FixedBlockAllocator::Initialize(
            oversized_metadata.data(),
            oversized_metadata.size(),
            one_block.data(),
            one_block.size(),
            8,
            FixedBlockHeaderWidth::Bytes2));
    });

    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes, 0xa5);
    std::vector<std::uint8_t> blocks((2U + 8U) * 3U, 0x5a);
    auto allocator = FixedBlockAllocator::Initialize(
        metadata.data(),
        metadata.size(),
        blocks.data(),
        blocks.size(),
        8,
        FixedBlockHeaderWidth::Bytes2);
    CHECK(allocator.HeaderBytes() == 2);
    CHECK(allocator.Capacity() == 3);
    CHECK(metadata[0] == 'K' && metadata[1] == 'V');
    CHECK(metadata[8] == 1 && metadata[9] == 0);
    CHECK(metadata[12] == 64 && metadata[13] == 0);
    CHECK(metadata[56] == 2);
    CHECK(blocks[0] == 0x5a);
    CHECK(blocks[2] == 0x5a);
    CHECK(allocator.Allocate() == 0);
    CHECK(allocator.Allocate() == 1);
    CHECK(allocator.Allocate() == 2);
    expectAllocatorError(
        AllocatorErrorCode::Capacity,
        [&] { static_cast<void>(allocator.Allocate()); });
    allocator.Validate();

    // Explicit widths assert the same minimum that Automatic would select;
    // they cannot create a persistent image that Attach would later reject.
    for (const auto noncanonical : {
             FixedBlockHeaderWidth::Bytes4,
             FixedBlockHeaderWidth::Bytes8}) {
        std::vector<std::uint8_t> rejected_metadata(
            FixedBlockAllocator::kPersistentMetadataBytes, 0xa5);
        const auto rejected_metadata_before = rejected_metadata;
        const auto blocks_before = blocks;
        expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
            static_cast<void>(FixedBlockAllocator::Initialize(
                rejected_metadata.data(),
                rejected_metadata.size(),
                blocks.data(),
                blocks.size(),
                8,
                noncanonical));
        });
        CHECK(rejected_metadata == rejected_metadata_before);
        CHECK(blocks == blocks_before);
    }

    constexpr std::size_t two_byte_capacity = 1U << 14U;
    std::vector<std::uint8_t> metadata2(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> exact_two((1U + 2U) * two_byte_capacity);
    auto auto_two = FixedBlockAllocator::Initialize(
        metadata2.data(),
        metadata2.size(),
        exact_two.data(),
        exact_two.size(),
        1);
    CHECK(auto_two.HeaderBytes() == 2);
    CHECK(auto_two.Capacity() == two_byte_capacity);

    std::vector<std::uint8_t> metadata4(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> just_over_two(
        (1U + 2U) * (two_byte_capacity + 1U));
    auto auto_four = FixedBlockAllocator::Initialize(
        metadata4.data(),
        metadata4.size(),
        just_over_two.data(),
        just_over_two.size(),
        1);
    CHECK(auto_four.HeaderBytes() == 4);
    CHECK(auto_four.Capacity() == 9831);
    CHECK(FixedBlockAllocator::MinimumHeaderWidth(auto_four.Capacity()) ==
          FixedBlockHeaderWidth::Bytes2);
    auto attached_four = FixedBlockAllocator::Attach(
        metadata4.data(),
        metadata4.size(),
        just_over_two.data(),
        just_over_two.size());
    CHECK(attached_four.HeaderBytes() == 4);
    CHECK(attached_four.Capacity() == 9831);
    auto recovery_four = FixedBlockAllocator::AttachForRecovery(
        metadata4.data(),
        metadata4.size(),
        just_over_two.data(),
        just_over_two.size());
    CHECK(recovery_four.HeaderBytes() == 4);
    CHECK(recovery_four.Capacity() == 9831);
}

void testFixedTailReuseAndInvalidFree() {
    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> blocks((2U + 16U) * 4U);
    auto allocator = FixedBlockAllocator::Initialize(
        metadata.data(),
        metadata.size(),
        blocks.data(),
        blocks.size(),
        16,
        FixedBlockHeaderWidth::Bytes2);

    CHECK(allocator.Allocate() == 0);
    allocator.Free(0);
    expectAllocatorError(
        AllocatorErrorCode::NotAllocated,
        [&] { static_cast<void>(allocator.BlockData(0)); });
    CHECK(allocator.Allocate() == 0);
    // The final free-list pop must install head=none. The historical upstream
    // bug left head=0 here and returned live ID 0 a second time.
    CHECK(allocator.Allocate() == 1);
    allocator.Validate();

    allocator.Free(0);
    expectAllocatorError(
        AllocatorErrorCode::NotAllocated,
        [&] { allocator.Free(0); });
    expectAllocatorError(
        AllocatorErrorCode::InvalidId,
        [&] { allocator.Free(allocator.Capacity()); });
    expectAllocatorError(
        AllocatorErrorCode::InvalidId,
        [&] { static_cast<void>(allocator.BlockData(3)); });
    allocator.Validate();
}

void testFixedAndBoxAttachRejectNoncanonicalAutomaticGeometry() {
    constexpr std::size_t block_zone_bytes = 180;
    constexpr std::size_t payload_bytes = BoxAllocator::kNodePayloadBytes;
    std::vector<std::uint8_t> fixed_metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> fixed_zone(block_zone_bytes);
    auto fixed = FixedBlockAllocator::Initialize(
        fixed_metadata.data(),
        fixed_metadata.size(),
        fixed_zone.data(),
        fixed_zone.size(),
        payload_bytes);
    CHECK(fixed.HeaderBytes() == 2);
    CHECK(fixed.Capacity() == 2);

    // width=4/capacity=1 is internally self-consistent for this same zone,
    // but Automatic must choose the minimum width=2 and capacity=2. Rehash
    // the complete mutated immutable geometry so rejection is not caused by
    // the checksum alone.
    store32(fixed_metadata.data() + 32, 1);
    fixed_metadata[56] = 4;
    recomputeBlockImmutableHash(fixed_metadata.data());
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(FixedBlockAllocator::Attach(
            fixed_metadata.data(),
            fixed_metadata.size(),
            fixed_zone.data(),
            fixed_zone.size()));
    });
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(FixedBlockAllocator::AttachForRecovery(
            fixed_metadata.data(),
            fixed_metadata.size(),
            fixed_zone.data(),
            fixed_zone.size()));
    });

    constexpr std::uint64_t data_bytes = 128;
    constexpr std::size_t metadata_bytes = 372;
    std::vector<std::uint8_t> box_metadata(metadata_bytes);
    static_cast<void>(BoxAllocator::Initialize(
        box_metadata.data(), box_metadata.size(), data_bytes));
    auto* embedded_metadata = box_metadata.data() + kBoxBlockMetadataOffset;
    auto* embedded_zone = box_metadata.data() + kBoxBlockZoneOffset;
    CHECK(embedded_metadata[56] == 2);

    // Preserve a valid allocated root under the forged four-byte layout so a
    // normal Box Attach would otherwise accept it, not merely recovery Attach.
    std::memmove(
        embedded_zone + 4,
        embedded_zone + 2,
        BoxAllocator::kNodePayloadBytes);
    store32(embedded_zone, 1);
    store32(embedded_metadata + 32, 1);
    embedded_metadata[56] = 4;
    recomputeBlockImmutableHash(embedded_metadata);
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(BoxAllocator::Attach(
            box_metadata.data(), box_metadata.size(), data_bytes));
    });
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(BoxAllocator::AttachForRecovery(
            box_metadata.data(), box_metadata.size(), data_bytes));
    });

    std::vector<std::uint8_t> payload_box_metadata(metadata_bytes);
    static_cast<void>(BoxAllocator::Initialize(
        payload_box_metadata.data(),
        payload_box_metadata.size(),
        data_bytes));
    auto* payload_embedded =
        payload_box_metadata.data() + kBoxBlockMetadataOffset;
    // Z=180 also has a self-consistent canonical inner geometry with
    // payload=178, width=2, and capacity=1. The outer Box contract fixes the
    // payload at 88, so neither attach mode may accept this rehashed bridge
    // mismatch even though the existing root remains readable at zone+2.
    store64(payload_embedded + 24, 178);
    store32(payload_embedded + 32, 1);
    recomputeBlockImmutableHash(payload_embedded);
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(BoxAllocator::Attach(
            payload_box_metadata.data(),
            payload_box_metadata.size(),
            data_bytes));
    });
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(BoxAllocator::AttachForRecovery(
            payload_box_metadata.data(),
            payload_box_metadata.size(),
            data_bytes));
    });
}

void testFixedRecoveryRebuildAndCorruption() {
    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> blocks((2U + 8U) * 5U);
    auto allocator = FixedBlockAllocator::Initialize(
        metadata.data(),
        metadata.size(),
        blocks.data(),
        blocks.size(),
        8,
        FixedBlockHeaderWidth::Bytes2);
    CHECK(allocator.Allocate() == 0);
    CHECK(allocator.Allocate() == 1);
    allocator.Free(0);

    // Corrupt only dynamic free-list state: used and has-next simultaneously.
    blocks[0] = 3;
    blocks[1] = 0;
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(FixedBlockAllocator::Attach(
            metadata.data(),
            metadata.size(),
            blocks.data(),
            blocks.size()));
    });

    auto recovery = FixedBlockAllocator::AttachForRecovery(
        metadata.data(),
        metadata.size(),
        blocks.data(),
        blocks.size());
    const std::uint8_t live_bits = 0x05; // IDs 0 and 2.
    const auto metadata_before_bad_alloc = metadata;
    const auto blocks_before_bad_alloc = blocks;
    bool allocation_failed = false;
    allocator_failure_injection::FailAfter(0);
    try {
        recovery.Rebuild(&live_bits, 3);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    } catch (...) {
        allocator_failure_injection::Disable();
        throw;
    }
    allocator_failure_injection::Disable();
    CHECK(allocation_failed);
    CHECK(metadata == metadata_before_bad_alloc);
    CHECK(blocks == blocks_before_bad_alloc);
    CHECK(recovery.RecoveryBlockData(0) != nullptr);

    const auto required_scratch =
        recovery.RebuildScratchBytes(&live_bits, 3);
    CHECK(required_scratch == 1);
    const auto metadata_before_short_scratch = metadata;
    const auto blocks_before_short_scratch = blocks;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        recovery.RebuildPrepared(&live_bits, 3, nullptr, 0);
    });
    CHECK(metadata == metadata_before_short_scratch);
    CHECK(blocks == blocks_before_short_scratch);
    CHECK(recovery.RecoveryBlockData(0) != nullptr);

    std::vector<std::uint8_t> scratch(required_scratch, 0);
    allocator_failure_injection::FailAfter(0);
    recovery.RebuildPrepared(
        &live_bits, 3, scratch.data(), scratch.size());
    allocator_failure_injection::Disable();
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(recovery.RecoveryBlockData(0));
    });
    CHECK(recovery.UsedCount() == 2);
    CHECK(recovery.IsAllocated(0));
    CHECK(!recovery.IsAllocated(1));
    CHECK(recovery.IsAllocated(2));
    recovery.Validate();

    const auto metadata_snapshot = metadata;
    const auto blocks_snapshot = blocks;
    recovery.Rebuild(&live_bits, 3);
    CHECK(metadata == metadata_snapshot);
    CHECK(blocks == blocks_snapshot);
    CHECK(recovery.Allocate() == 1);
    recovery.Validate();

    const auto before_bad_bitmap = metadata;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        recovery.Rebuild(&live_bits, recovery.Capacity() + 1ULL);
    });
    CHECK(metadata == before_bad_bitmap);
    recovery.Validate();
}

void testFixedRecoveryCanonicalDescendingFreeChain() {
    constexpr std::size_t payload_bytes = 8;
    constexpr std::size_t header_bytes = 2;
    constexpr std::size_t stride = payload_bytes + header_bytes;
    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> blocks(stride * 5U);
    static_cast<void>(FixedBlockAllocator::Initialize(
        metadata.data(),
        metadata.size(),
        blocks.data(),
        blocks.size(),
        payload_bytes,
        FixedBlockHeaderWidth::Bytes2));

    auto recovery = FixedBlockAllocator::AttachForRecovery(
        metadata.data(), metadata.size(), blocks.data(), blocks.size());
    const std::uint8_t live_bits = 0x12; // IDs 1 and 4 are committed.
    recovery.Rebuild(&live_bits, 5);

    // The canonical recovery chain is 3 -> 2 -> 0. Two-byte headers encode
    // has_next in bit 1 and next_id starting at bit 2.
    CHECK(metadata[44] == 3 && metadata[45] == 0 &&
          metadata[46] == 0 && metadata[47] == 0);
    CHECK(blocks[0 * stride] == 0 && blocks[0 * stride + 1U] == 0);
    CHECK(blocks[2 * stride] == 2 && blocks[2 * stride + 1U] == 0);
    CHECK(blocks[3 * stride] == 10 && blocks[3 * stride + 1U] == 0);
    CHECK(recovery.Allocate() == 3);
    CHECK(recovery.Allocate() == 2);
    CHECK(recovery.Allocate() == 0);
    recovery.Validate();
}

void testFixedPreparedEmptyRecovery() {
    constexpr std::size_t payload_bytes = 8;
    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> blocks((2U + payload_bytes) * 3U);
    auto initialized = FixedBlockAllocator::Initialize(
        metadata.data(),
        metadata.size(),
        blocks.data(),
        blocks.size(),
        payload_bytes,
        FixedBlockHeaderWidth::Bytes2);
    CHECK(initialized.Allocate() == 0);
    CHECK(initialized.Allocate() == 1);

    auto recovery = FixedBlockAllocator::AttachForRecovery(
        metadata.data(), metadata.size(), blocks.data(), blocks.size());
    CHECK(recovery.RebuildScratchBytes(nullptr, 0) == 0);
    recovery.RebuildPrepared(nullptr, 0, nullptr, 0);
    CHECK(recovery.HighWater() == 0);
    CHECK(recovery.UsedCount() == 0);
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(recovery.RecoveryBlockData(0));
    });

    const auto metadata_snapshot = metadata;
    const auto blocks_snapshot = blocks;
    recovery.RebuildPrepared(nullptr, 0, nullptr, 0);
    CHECK(metadata == metadata_snapshot);
    CHECK(blocks == blocks_snapshot);
    recovery.Validate();
}

void testFixedRecoveryPayloadAccessIgnoresDynamicMetadata() {
    constexpr std::size_t payload_bytes = 16;
    constexpr std::size_t capacity = 4;
    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> blocks(
        (2U + payload_bytes) * capacity, 0xa5);
    auto allocator = FixedBlockAllocator::Initialize(
        metadata.data(),
        metadata.size(),
        blocks.data(),
        blocks.size(),
        payload_bytes,
        FixedBlockHeaderWidth::Bytes2);
    CHECK(allocator.Allocate() == 0);
    CHECK(allocator.Allocate() == 1);
    const std::array<std::uint8_t, payload_bytes> authoritative = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f};
    std::memcpy(
        allocator.BlockData(1), authoritative.data(), authoritative.size());
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(allocator.RecoveryBlockData(1));
    });

    // A recovery view is a read-only capability even when the dynamic bytes
    // happen to be healthy. Moving it transfers and revokes that capability.
    auto healthy_recovery = FixedBlockAllocator::AttachForRecovery(
        metadata.data(), metadata.size(), blocks.data(), blocks.size());
    const auto healthy_metadata = metadata;
    const auto healthy_blocks = blocks;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(healthy_recovery.BlockData(1));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(healthy_recovery.Allocate());
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        healthy_recovery.Free(1);
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(healthy_recovery.HighWater());
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        healthy_recovery.Validate();
    });
    CHECK(metadata == healthy_metadata);
    CHECK(blocks == healthy_blocks);
    auto moved_recovery = std::move(healthy_recovery);
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(healthy_recovery.RecoveryBlockData(1));
    });
    CHECK(moved_recovery.RecoveryBlockData(1) != nullptr);

    // Corrupt only mutable state: impossible counters and a torn block header.
    std::fill(metadata.begin() + 36, metadata.begin() + 48, 0xff);
    const auto stride = payload_bytes + 2U;
    blocks[stride] = 3;
    blocks[stride + 1U] = 0;
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(FixedBlockAllocator::Attach(
            metadata.data(), metadata.size(), blocks.data(), blocks.size()));
    });

    auto recovery = FixedBlockAllocator::AttachForRecovery(
        metadata.data(), metadata.size(), blocks.data(), blocks.size());
    const auto* recovered = static_cast<const std::uint8_t*>(
        recovery.RecoveryBlockData(1));
    CHECK(std::equal(
        authoritative.begin(), authoritative.end(), recovered));
    const auto& const_recovery = recovery;
    const auto* const_recovered = static_cast<const std::uint8_t*>(
        const_recovery.RecoveryBlockData(1));
    CHECK(std::equal(
        authoritative.begin(), authoritative.end(), const_recovered));
    expectAllocatorError(AllocatorErrorCode::InvalidId, [&] {
        static_cast<void>(recovery.RecoveryBlockData(recovery.Capacity()));
    });

    const auto metadata_before_failed_rebuild = metadata;
    const auto blocks_before_failed_rebuild = blocks;
    const std::uint8_t invalid_live_bits = 0;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        recovery.Rebuild(
            &invalid_live_bits,
            static_cast<std::size_t>(recovery.Capacity()) + 1U);
    });
    CHECK(metadata == metadata_before_failed_rebuild);
    CHECK(blocks == blocks_before_failed_rebuild);
    CHECK(recovery.RecoveryBlockData(1) != nullptr);
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(recovery.BlockData(1));
    });

    const std::uint8_t live_bits = 0x02; // Only authoritative ID 1 is live.
    recovery.Rebuild(&live_bits, 2);
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(recovery.RecoveryBlockData(1));
    });
    CHECK(!recovery.IsAllocated(0));
    CHECK(recovery.IsAllocated(1));
    CHECK(std::equal(
        authoritative.begin(),
        authoritative.end(),
        static_cast<const std::uint8_t*>(recovery.BlockData(1))));
    recovery.Validate();
}

void testFixedSparseZoneDoesNotTouchCapacity() {
    const auto raw_page_size = ::sysconf(_SC_PAGESIZE);
    CHECK(raw_page_size > 2);
    const auto page_size = static_cast<std::size_t>(raw_page_size);
    constexpr std::size_t capacity = 1U << 14U;
    const auto mapping_bytes = page_size * capacity;
    void* mapping = ::mmap(
        nullptr,
        mapping_bytes,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    CHECK(mapping != MAP_FAILED);
    ScopedMapping scoped(mapping, mapping_bytes);
    CHECK(::mprotect(mapping, page_size, PROT_READ | PROT_WRITE) == 0);

    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    auto allocator = FixedBlockAllocator::Initialize(
        metadata.data(),
        metadata.size(),
        mapping,
        mapping_bytes,
        page_size - 2U,
        FixedBlockHeaderWidth::Bytes2);
    CHECK(allocator.Capacity() == capacity);
    const std::uint8_t root_live = 1;
    allocator.Rebuild(&root_live, 1);
    CHECK(allocator.HighWater() == 1);
    CHECK(allocator.BlockData(0) != nullptr);
    allocator.Validate();
}

void testBoxRoundingAndCanonicalDataShape() {
    CHECK(BoxAllocator::RoundSize(1) == 8);
    CHECK(BoxAllocator::RoundSize(8) == 8);
    CHECK(BoxAllocator::RoundSize(9) == 16);
    std::uint64_t unit = 8;
    for (std::size_t level = 0; level < 5; ++level) {
        const auto top = unit * 15U;
        CHECK(BoxAllocator::RoundSize(top) == top);
        CHECK(BoxAllocator::RoundSize(top + 1U) == unit * 16U);
        unit *= 16U;
    }
    CHECK(BoxAllocator::RoundSize(121) == 128);
    CHECK(BoxAllocator::RoundSize(129) == 256);
    CHECK(BoxAllocator::RoundSize(1920) == 1920);
    CHECK(BoxAllocator::RoundSize(1921) == 2048);
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [] {
        static_cast<void>(BoxAllocator::RoundSize(0));
    });
    expectAllocatorError(AllocatorErrorCode::Capacity, [] {
        static_cast<void>(BoxAllocator::RoundSize(UINT64_MAX));
    });

    const std::array<std::pair<std::uint64_t, std::uint64_t>, 6>
        canonical_bounds = {{{8, 8},
                             {120, 120},
                             {128, 128},
                             {129, 128},
                             {1000, 896},
                             {UINT64_MAX, 1ULL << 63U}}};
    for (const auto& item : canonical_bounds) {
        const auto canonical =
            BoxAllocator::LargestCanonicalDataSize(item.first);
        CHECK(canonical == item.second);
        CHECK(canonical <= item.first);
        std::vector<std::uint8_t> metadata(kLargeBoxMetadataBytes);
        auto box = BoxAllocator::Initialize(
            metadata.data(), metadata.size(), canonical);
        box.Validate();
    }
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [] {
        static_cast<void>(BoxAllocator::LargestCanonicalDataSize(7));
    });

    for (const std::uint64_t valid_size : {120U, 128U, 2048U}) {
        std::vector<std::uint8_t> metadata(kLargeBoxMetadataBytes);
        auto box = BoxAllocator::Initialize(
            metadata.data(), metadata.size(), valid_size);
        CHECK(box.DataBytes() == valid_size);
        box.Validate();
    }
    for (const std::uint64_t invalid_size : {0U, 129U, 136U, 1000U}) {
        std::vector<std::uint8_t> metadata(kLargeBoxMetadataBytes, 0x5a);
        expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
            static_cast<void>(BoxAllocator::Initialize(
                metadata.data(), metadata.size(), invalid_size));
        });
    }
}

void testBoxProvableLayoutHelpers() {
    constexpr std::uint64_t box_fixed_metadata =
        BoxAllocator::kPersistentHeaderBytes +
        FixedBlockAllocator::kPersistentMetadataBytes;
    CHECK(box_fixed_metadata == 192);
    CHECK(BoxAllocator::FullExpansionNodeCount(8) == 1);
    CHECK(BoxAllocator::FullExpansionNodeCount(120) == 1);
    CHECK(BoxAllocator::FullExpansionNodeCount(128) == 2);
    CHECK(BoxAllocator::FullExpansionNodeCount(1920) == 16);
    CHECK(BoxAllocator::FullExpansionNodeCount(2048) == 18);
    CHECK(BoxAllocator::MinimumMetadataBytesForFullExpansion(8) ==
          box_fixed_metadata + 90U);

    // L=4 straddles the 2^14 FixedBlock header threshold: m=3 still uses
    // 2-byte headers, while m=4 must use 4-byte headers.
    constexpr std::uint64_t level4_unit = 8ULL * 65536ULL;
    constexpr std::uint64_t below_data = 3ULL * level4_unit;
    constexpr std::uint64_t above_data = 4ULL * level4_unit;
    constexpr std::uint64_t below_nodes = 13108;
    constexpr std::uint64_t above_nodes = 17477;
    CHECK(BoxAllocator::FullExpansionNodeCount(below_data) == below_nodes);
    CHECK(BoxAllocator::FullExpansionNodeCount(above_data) == above_nodes);
    const auto below_metadata =
        BoxAllocator::MinimumMetadataBytesForFullExpansion(below_data);
    const auto above_metadata =
        BoxAllocator::MinimumMetadataBytesForFullExpansion(above_data);
    CHECK(below_metadata == box_fixed_metadata + below_nodes * 90U);
    CHECK(above_metadata == box_fixed_metadata + above_nodes * 92U);
    std::vector<std::uint8_t> below_storage(
        static_cast<std::size_t>(below_metadata));
    auto below = BoxAllocator::Initialize(
        below_storage.data(), below_storage.size(), below_data);
    CHECK(below.MetadataNodeCapacity() == below_nodes);
    std::vector<std::uint8_t> above_storage(
        static_cast<std::size_t>(above_metadata));
    auto above = BoxAllocator::Initialize(
        above_storage.data(), above_storage.size(), above_data);
    CHECK(above.MetadataNodeCapacity() == above_nodes);

    // L=8 similarly crosses the 2^30 threshold between m=3 and m=4.
    constexpr std::uint64_t level8_unit = 8ULL * (1ULL << 32U);
    constexpr std::uint64_t level8_factor = 286331153ULL;
    constexpr std::uint64_t below_wide_nodes = 1ULL + 3ULL * level8_factor;
    constexpr std::uint64_t above_wide_nodes = 1ULL + 4ULL * level8_factor;
    CHECK(below_wide_nodes <= (1ULL << 30U));
    CHECK(above_wide_nodes > (1ULL << 30U));
    CHECK(BoxAllocator::MinimumMetadataBytesForFullExpansion(
              3ULL * level8_unit) ==
          box_fixed_metadata + below_wide_nodes * 92U);
    CHECK(BoxAllocator::MinimumMetadataBytesForFullExpansion(
              4ULL * level8_unit) ==
          box_fixed_metadata + above_wide_nodes * 96U);

    constexpr std::uint64_t max_full_data = 14ULL * level8_unit;
    constexpr std::uint64_t max_full_nodes =
        1ULL + 14ULL * level8_factor;
    CHECK(max_full_nodes <= UINT32_MAX);
    CHECK(BoxAllocator::FullExpansionNodeCount(max_full_data) ==
          max_full_nodes);
    expectAllocatorError(AllocatorErrorCode::Capacity, [] {
        static_cast<void>(BoxAllocator::FullExpansionNodeCount(
            15ULL * level8_unit));
    });
    expectAllocatorError(AllocatorErrorCode::Capacity, [] {
        static_cast<void>(BoxAllocator::FullExpansionNodeCount(1ULL << 63U));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [] {
        static_cast<void>(BoxAllocator::FullExpansionNodeCount(UINT64_MAX));
    });

    const auto smallest_total =
        BoxAllocator::MinimumMetadataBytesForFullExpansion(8) + 8U;
    expectAllocatorError(AllocatorErrorCode::Capacity, [&] {
        static_cast<void>(BoxAllocator::LargestFullyRepresentableDataSize(
            smallest_total - 1U));
    });
    CHECK(BoxAllocator::LargestFullyRepresentableDataSize(smallest_total) == 8);
    CHECK(BoxAllocator::LargestFullyRepresentableDataSize(UINT64_MAX) ==
          max_full_data);

    // Brute-force every small byte budget against an independently enumerated
    // canonical set. This checks both exact metadata cost and maximal choice.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> candidates;
    for (std::uint64_t unit = 8; unit <= 8192; unit *= 16U) {
        for (std::uint64_t multiple = 1; multiple <= 15; ++multiple) {
            const auto data = unit * multiple;
            const auto metadata =
                BoxAllocator::MinimumMetadataBytesForFullExpansion(data);
            candidates.emplace_back(metadata + data, data);
        }
    }
    for (std::uint64_t budget = 0; budget <= 8192; ++budget) {
        std::uint64_t expected = 0;
        for (const auto& candidate : candidates) {
            if (candidate.first <= budget) {
                expected = std::max(expected, candidate.second);
            }
        }
        if (expected == 0) {
            expectAllocatorError(AllocatorErrorCode::Capacity, [&] {
                static_cast<void>(
                    BoxAllocator::LargestFullyRepresentableDataSize(budget));
            });
        } else {
            CHECK(BoxAllocator::LargestFullyRepresentableDataSize(budget) ==
                  expected);
        }
    }
}

void testInitializeStrongGuaranteesAndNoCapabilityAliases() {
    // 192 bytes are fixed Box metadata; the root additionally needs one
    // 88-byte node plus the minimum two-byte FixedBlock header.
    for (std::size_t bytes = kBoxBlockZoneOffset + 1U;
         bytes < kBoxBlockZoneOffset + 90U;
         ++bytes) {
        std::vector<std::uint8_t> metadata(bytes, 0x5a);
        const auto before = metadata;
        expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
            static_cast<void>(
                BoxAllocator::Initialize(metadata.data(), metadata.size(), 8));
        });
        CHECK(metadata == before);
    }

    std::vector<std::uint8_t> too_small_fixed_metadata(
        FixedBlockAllocator::kPersistentMetadataBytes, 0x5a);
    std::vector<std::uint8_t> too_small_fixed_zone(89U, 0x5a);
    const auto fixed_metadata_before = too_small_fixed_metadata;
    const auto fixed_zone_before = too_small_fixed_zone;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(FixedBlockAllocator::Initialize(
            too_small_fixed_metadata.data(),
            too_small_fixed_metadata.size(),
            too_small_fixed_zone.data(),
            too_small_fixed_zone.size(),
            BoxAllocator::kNodePayloadBytes,
            FixedBlockHeaderWidth::Bytes2));
    });
    CHECK(too_small_fixed_metadata == fixed_metadata_before);
    CHECK(too_small_fixed_zone == fixed_zone_before);

    std::vector<std::uint8_t> exact_box(kBoxBlockZoneOffset + 90U, 0x5a);
    allocator_failure_injection::FailAfter(0);
    try {
        auto box = BoxAllocator::Initialize(
            exact_box.data(), exact_box.size(), 8);
        allocator_failure_injection::Disable();
        CHECK(box.MetadataNodeCapacity() == 1);
        box.Validate();
    } catch (...) {
        allocator_failure_injection::Disable();
        throw;
    }

    std::vector<std::uint8_t> fixed_metadata(
        FixedBlockAllocator::kPersistentMetadataBytes, 0x5a);
    std::vector<std::uint8_t> fixed_zone(90U, 0x5a);
    allocator_failure_injection::FailAfter(0);
    try {
        auto fixed = FixedBlockAllocator::Initialize(
            fixed_metadata.data(),
            fixed_metadata.size(),
            fixed_zone.data(),
            fixed_zone.size(),
            BoxAllocator::kNodePayloadBytes,
            FixedBlockHeaderWidth::Bytes2);
        allocator_failure_injection::Disable();
        CHECK(fixed.Capacity() == 1);
        fixed.Validate();
    } catch (...) {
        allocator_failure_injection::Disable();
        throw;
    }
}

void testBoxStableFormatOffsetZeroAndReuse() {
    std::vector<std::uint8_t> metadata(kLargeBoxMetadataBytes, 0xa5);
    auto box = BoxAllocator::Initialize(
        metadata.data(), metadata.size(), kLargeBoxDataBytes);
    CHECK(BoxAllocator::kPersistentHeaderBytes == 128);
    CHECK(BoxAllocator::kNodePayloadBytes == 88);
    CHECK(metadata[0] == 'K' && metadata[1] == 'V');
    CHECK(metadata[8] == 1 && metadata[9] == 0);
    CHECK(metadata[12] == 128 && metadata[13] == 0);
    CHECK(metadata[kBoxBlockMetadataOffset] == 'K');

    const auto first = box.Allocate(1);
    CHECK(first == 0);
    CHECK(box.AllocatedSize(0) == 8);
    const auto second = box.Allocate(8);
    CHECK(second == 8);
    box.Free(0);
    expectAllocatorError(
        AllocatorErrorCode::NotAllocated,
        [&] { static_cast<void>(box.AllocatedSize(0)); });
    CHECK(box.Allocate(8) == 0);
    box.Validate();
}

void testBoxSparseMetadataDoesNotTouchCapacity() {
    const auto raw_page_size = ::sysconf(_SC_PAGESIZE);
    CHECK(raw_page_size > 0);
    const auto page_size = static_cast<std::size_t>(raw_page_size);
    constexpr std::size_t pages = 1U << 14U;
    const auto mapping_bytes = page_size * pages;
    void* mapping = ::mmap(
        nullptr,
        mapping_bytes,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0);
    CHECK(mapping != MAP_FAILED);
    ScopedMapping scoped(mapping, mapping_bytes);
    CHECK(::mprotect(mapping, page_size, PROT_READ | PROT_WRITE) == 0);

    auto box = BoxAllocator::Initialize(
        mapping, mapping_bytes, kLargeBoxDataBytes);
    CHECK(box.MetadataNodeCapacity() > 1000);
    box.Rebuild({{0, 1}});
    CHECK(box.AllocatedSize(0) == 8);
    box.Validate();
}

void testBoxReserveAndStrongFailures() {
    std::vector<std::uint8_t> metadata(kLargeBoxMetadataBytes);
    auto box = BoxAllocator::Initialize(
        metadata.data(), metadata.size(), kLargeBoxDataBytes);
    box.ReserveAt(0, 9);
    CHECK(box.AllocatedSize(0) == 16);
    const auto nodes_after_reserve = box.MetadataNodesUsed();
    expectAllocatorError(
        AllocatorErrorCode::Overlap,
        [&] { box.ReserveAt(8, 8); });
    CHECK(box.MetadataNodesUsed() == nodes_after_reserve);
    box.Validate();
    CHECK(box.Allocate(8) == 16);
    box.Validate();

    const auto nodes_before_bounds = box.MetadataNodesUsed();
    expectAllocatorError(
        AllocatorErrorCode::InvalidArgument,
        [&] { box.ReserveAt(kLargeBoxDataBytes - 8U, 9); });
    expectAllocatorError(
        AllocatorErrorCode::InvalidArgument,
        [&] { box.ReserveAt(1, 8); });
    CHECK(box.MetadataNodesUsed() == nodes_before_bounds);
    box.Validate();

    // A 15-slot level-0 allocation starting at slot 2 crosses a node boundary.
    std::vector<std::uint8_t> crossing_metadata(kLargeBoxMetadataBytes);
    auto crossing = BoxAllocator::Initialize(
        crossing_metadata.data(),
        crossing_metadata.size(),
        kLargeBoxDataBytes);
    expectAllocatorError(
        AllocatorErrorCode::InvalidArgument,
        [&] { crossing.ReserveAt(16, 120); });
    CHECK(crossing.MetadataNodesUsed() == 1);
    crossing.Validate();
    CHECK(crossing.Allocate(kLargeBoxDataBytes) == 0);

    // Capacity for root plus one child: a byte allocation needs three child
    // levels, so recursively created empty children must unwind completely.
    constexpr std::size_t two_node_metadata =
        kBoxBlockZoneOffset + (2U + BoxAllocator::kNodePayloadBytes) * 2U;
    std::vector<std::uint8_t> tiny_metadata(two_node_metadata);
    auto tiny = BoxAllocator::Initialize(
        tiny_metadata.data(), tiny_metadata.size(), kLargeBoxDataBytes);
    CHECK(tiny.MetadataNodeCapacity() == 2);
    expectAllocatorError(
        AllocatorErrorCode::Capacity,
        [&] { static_cast<void>(tiny.Allocate(1)); });
    CHECK(tiny.MetadataNodesUsed() == 1);
    tiny.Validate();
    CHECK(tiny.Allocate(kLargeBoxDataBytes) == 0);

    std::vector<std::uint8_t> reserve_tiny_metadata(two_node_metadata);
    auto reserve_tiny = BoxAllocator::Initialize(
        reserve_tiny_metadata.data(),
        reserve_tiny_metadata.size(),
        kLargeBoxDataBytes);
    expectAllocatorError(
        AllocatorErrorCode::Capacity,
        [&] { reserve_tiny.ReserveAt(0, 1); });
    CHECK(reserve_tiny.MetadataNodesUsed() == 1);
    reserve_tiny.Validate();
    reserve_tiny.ReserveAt(0, kLargeBoxDataBytes);
    CHECK(reserve_tiny.AllocatedSize(0) == kLargeBoxDataBytes);

    std::vector<std::uint8_t> data_full_metadata(kLargeBoxMetadataBytes);
    auto data_full = BoxAllocator::Initialize(
        data_full_metadata.data(), data_full_metadata.size(), 128);
    CHECK(data_full.Allocate(128) == 0);
    const auto full_nodes = data_full.MetadataNodesUsed();
    expectAllocatorError(
        AllocatorErrorCode::Capacity,
        [&] { static_cast<void>(data_full.Allocate(8)); });
    CHECK(data_full.MetadataNodesUsed() == full_nodes);
    data_full.Validate();
    data_full.Free(0);
    CHECK(data_full.Allocate(8) == 0);
}

void testBoxRebuildPreflightAndIdempotence() {
    std::vector<std::uint8_t> metadata(kLargeBoxMetadataBytes);
    auto box = BoxAllocator::Initialize(
        metadata.data(), metadata.size(), kLargeBoxDataBytes);
    static_cast<void>(box.Allocate(128));

    const std::vector<BoxLiveInterval> live = {
        {256, 17}, // rounds to 24
        {0, 1},    // rounds to 8
        {128, 9},  // rounds to 16
    };
    const auto before_validation = metadata;
    box.ValidateRebuildIntervals(live);
    CHECK(metadata == before_validation);
    box.Rebuild(live);
    CHECK(box.AllocatedSize(0) == 8);
    CHECK(box.AllocatedSize(128) == 16);
    CHECK(box.AllocatedSize(256) == 24);
    box.Validate();

    const auto snapshot = metadata;
    const std::vector<BoxLiveInterval> reordered = {
        {128, 9}, {0, 1}, {256, 17}};
    box.Rebuild(reordered);
    CHECK(metadata == snapshot);

    const auto next = box.Allocate(8);
    CHECK(!overlaps(next, 8, 0, 8));
    CHECK(!overlaps(next, 8, 128, 16));
    CHECK(!overlaps(next, 8, 256, 24));
    box.Free(next);
    box.Validate();

    const auto require_unchanged = [&](const std::vector<BoxLiveInterval>& bad,
                                       AllocatorErrorCode code) {
        const auto before = metadata;
        expectAllocatorError(
            code, [&] { box.ValidateRebuildIntervals(bad); });
        CHECK(metadata == before);
        expectAllocatorError(code, [&] { box.Rebuild(bad); });
        CHECK(metadata == before);
        box.Validate();
    };
    require_unchanged({{0, 9}, {8, 8}}, AllocatorErrorCode::Overlap);
    require_unchanged(
        {{kLargeBoxDataBytes - 8U, 9}},
        AllocatorErrorCode::InvalidArgument);
    require_unchanged({{1, 8}}, AllocatorErrorCode::InvalidArgument);
    require_unchanged({{0, 0}}, AllocatorErrorCode::InvalidArgument);
    require_unchanged({{16, 120}}, AllocatorErrorCode::InvalidArgument);
    require_unchanged({{0, UINT64_MAX}}, AllocatorErrorCode::Capacity);

    constexpr std::size_t root_only_metadata =
        kBoxBlockZoneOffset + 2U + BoxAllocator::kNodePayloadBytes;
    std::vector<std::uint8_t> root_metadata(root_only_metadata);
    auto root_only = BoxAllocator::Initialize(
        root_metadata.data(), root_metadata.size(), kLargeBoxDataBytes);
    CHECK(root_only.MetadataNodeCapacity() == 1);
    CHECK(root_only.Allocate(kLargeBoxDataBytes) == 0);
    const auto root_snapshot = root_metadata;
    expectAllocatorError(
        AllocatorErrorCode::Capacity,
        [&] { root_only.Rebuild({{0, 1}}); });
    CHECK(root_metadata == root_snapshot);
    root_only.Validate();
    CHECK(root_only.AllocatedSize(0) == kLargeBoxDataBytes);
}

void testBoxRebuildAllocationFailureIsStrong() {
    std::vector<std::uint8_t> metadata(kLargeBoxMetadataBytes);
    auto box = BoxAllocator::Initialize(
        metadata.data(), metadata.size(), kLargeBoxDataBytes);
    CHECK(box.Allocate(128) == 0);
    const auto baseline = metadata;
    const std::vector<BoxLiveInterval> replacement = {
        {4096, 1},
        {8192, 17},
        {16384, 129},
    };

    std::size_t injected_failures = 0;
    bool completed = false;
    for (std::size_t fail_after = 0; fail_after < 1024; ++fail_after) {
        metadata = baseline;
        auto candidate = BoxAllocator::Attach(
            metadata.data(), metadata.size(), kLargeBoxDataBytes);
        bool allocation_failed = false;
        allocator_failure_injection::FailAfter(fail_after);
        try {
            candidate.Rebuild(replacement);
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        } catch (...) {
            allocator_failure_injection::Disable();
            throw;
        }
        allocator_failure_injection::Disable();

        if (!allocation_failed) {
            completed = true;
            candidate.Validate();
            CHECK(candidate.AllocatedSize(4096) == 8);
            CHECK(candidate.AllocatedSize(8192) == 24);
            CHECK(candidate.AllocatedSize(16384) == 256);
            break;
        }

        ++injected_failures;
        CHECK(metadata == baseline);
        candidate.Validate();
        CHECK(candidate.AllocatedSize(0) == 128);
        const auto probe = candidate.Allocate(8);
        CHECK(candidate.AllocatedSize(probe) == 8);
        candidate.Free(probe);
        candidate.Validate();
    }
    CHECK(completed);
    CHECK(injected_failures >= 3);
}

void testMoveOnlyPreparedRecoveryCapabilities() {
    const std::vector<BoxLiveInterval> replacement = {
        {4096, 1},
        {8192, 17},
        {16384, 129},
    };
    std::vector<std::uint8_t> box_metadata(kLargeBoxMetadataBytes);
    auto original_box = BoxAllocator::Initialize(
        box_metadata.data(), box_metadata.size(), kLargeBoxDataBytes);
    CHECK(original_box.Allocate(128) == 0);
    auto box_recovery = BoxAllocator::AttachForRecovery(
        box_metadata.data(), box_metadata.size(), kLargeBoxDataBytes);

    const auto box_before_bad_alloc = box_metadata;
    bool box_allocation_failed = false;
    allocator_failure_injection::FailAfter(0);
    try {
        static_cast<void>(
            std::move(box_recovery).PrepareRebuild(replacement));
    } catch (const std::bad_alloc&) {
        box_allocation_failed = true;
    } catch (...) {
        allocator_failure_injection::Disable();
        throw;
    }
    allocator_failure_injection::Disable();
    CHECK(box_allocation_failed);
    CHECK(box_metadata == box_before_bad_alloc);
    box_recovery.ValidateRebuildIntervals(replacement);

    const auto box_before_invalid = box_metadata;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(std::move(box_recovery).PrepareRebuild({{1, 8}}));
    });
    CHECK(box_metadata == box_before_invalid);
    box_recovery.ValidateRebuildIntervals(replacement);

    auto box_plan =
        std::move(box_recovery).PrepareRebuild(replacement);
    allocator_failure_injection::FailAfter(0);
    auto rebuilt_box = std::move(box_plan).Apply();
    allocator_failure_injection::Disable();
    rebuilt_box.Validate();
    CHECK(rebuilt_box.AllocatedSize(4096) == 8);
    CHECK(rebuilt_box.AllocatedSize(8192) == 24);
    CHECK(rebuilt_box.AllocatedSize(16384) == 256);

    constexpr std::size_t trie_capacity = 4;
    constexpr std::size_t trie_stride =
        2U + TrieNodeStore::kPersistentNodeBytes;
    std::vector<std::uint8_t> trie_metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> trie_zone(trie_capacity * trie_stride);
    auto original_trie = TrieNodeStore::Initialize(
        trie_metadata.data(),
        trie_metadata.size(),
        trie_zone.data(),
        trie_zone.size(),
        FixedBlockHeaderWidth::Bytes2);
    TrieNodeRecord leaf;
    leaf.has_value = true;
    CHECK(original_trie.Allocate(leaf) == 0);
    TrieNodeRecord root;
    root.has_value = true;
    root.value_ref = 17;
    root.children[42] = 0;
    CHECK(original_trie.Allocate(root) == 1);
    CHECK(original_trie.Allocate() == 2);

    // Recovery attaches ignore only these mutable dynamic fields.
    std::fill(trie_metadata.begin() + 36, trie_metadata.begin() + 48, 0xff);
    auto trie_recovery = TrieNodeStore::AttachForRecovery(
        trie_metadata.data(),
        trie_metadata.size(),
        trie_zone.data(),
        trie_zone.size());
    const auto trie_metadata_before_bad_alloc = trie_metadata;
    const auto trie_zone_before_bad_alloc = trie_zone;
    bool trie_allocation_failed = false;
    allocator_failure_injection::FailAfter(0);
    try {
        static_cast<void>(
            std::move(trie_recovery)
                .PrepareRecoveryFromCommittedRoot(1));
    } catch (const std::bad_alloc&) {
        trie_allocation_failed = true;
    } catch (...) {
        allocator_failure_injection::Disable();
        throw;
    }
    allocator_failure_injection::Disable();
    CHECK(trie_allocation_failed);
    CHECK(trie_metadata == trie_metadata_before_bad_alloc);
    CHECK(trie_zone == trie_zone_before_bad_alloc);
    CHECK(trie_recovery.ReadForRecovery(1).value_ref == 17);

    const auto trie_metadata_before_invalid = trie_metadata;
    const auto trie_zone_before_invalid = trie_zone;
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(
            std::move(trie_recovery).PrepareRecoveryFromCommittedRoot(
                trie_recovery.Capacity()));
    });
    CHECK(trie_metadata == trie_metadata_before_invalid);
    CHECK(trie_zone == trie_zone_before_invalid);
    CHECK(trie_recovery.ReadForRecovery(1).value_ref == 17);

    auto trie_plan = std::move(trie_recovery)
        .PrepareRecoveryFromCommittedRoot(1);
    CHECK(trie_plan.LiveBitCount() == 2);
    CHECK(trie_plan.LiveBits().size() == 1);
    CHECK((trie_plan.LiveBits()[0] & 0x03U) == 0x03U);
    CHECK(trie_plan.ReachableNodeCount() == 2);
    CHECK(trie_plan.ValueReferences() ==
          std::vector<std::uint64_t>({17, 0}));
    allocator_failure_injection::FailAfter(0);
    auto rebuilt_trie = std::move(trie_plan).Apply();
    allocator_failure_injection::Disable();
    rebuilt_trie.Validate();
    CHECK(rebuilt_trie.UsedCount() == 2);
    CHECK(rebuilt_trie.Read(1).value_ref == 17);
    CHECK(rebuilt_trie.Read(0).has_value);
}

void testBoxRecoveryFromCorruptMetadata() {
    std::vector<std::uint8_t> metadata(kLargeBoxMetadataBytes);
    auto box = BoxAllocator::Initialize(
        metadata.data(), metadata.size(), kLargeBoxDataBytes);
    box.ReserveAt(0, 1);
    box.ReserveAt(16384, 1);
    box.Free(16384);
    box.Validate();

    auto blocks = FixedBlockAllocator::Attach(
        metadata.data() + kBoxBlockMetadataOffset,
        FixedBlockAllocator::kPersistentMetadataBytes,
        metadata.data() + kBoxBlockZoneOffset,
        metadata.size() - kBoxBlockZoneOffset);
    std::uint32_t free_id = FixedBlockAllocator::kInvalidId;
    for (std::uint32_t id = 1; id < blocks.HighWater(); ++id) {
        if (!blocks.IsAllocated(id)) {
            free_id = id;
            break;
        }
    }
    CHECK(free_id != FixedBlockAllocator::kInvalidId);
    auto* block_zone = metadata.data() + kBoxBlockZoneOffset;
    const auto stride = blocks.HeaderBytes() + BoxAllocator::kNodePayloadBytes;
    auto* corrupt_header =
        block_zone + static_cast<std::size_t>(free_id) * stride;
    std::memset(corrupt_header, 0, blocks.HeaderBytes());
    corrupt_header[0] = 3; // allocated and linked at the same time

    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(BoxAllocator::Attach(
            metadata.data(), metadata.size(), kLargeBoxDataBytes));
    });
    auto recovery = BoxAllocator::AttachForRecovery(
        metadata.data(), metadata.size(), kLargeBoxDataBytes);
    const auto metadata_before_recovery_gate = metadata;
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(recovery.Allocate(1));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(recovery.AllocatedSize(0));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(recovery.MetadataNodesUsed());
    });
    CHECK(metadata == metadata_before_recovery_gate);
    recovery.Rebuild({{0, 1}});
    CHECK(recovery.AllocatedSize(0) == 8);
    recovery.Validate();
    recovery.Rebuild({{0, 1}});
    CHECK(recovery.AllocatedSize(0) == 8);

    std::vector<std::uint8_t> node_metadata(kLargeBoxMetadataBytes);
    auto node_box = BoxAllocator::Initialize(
        node_metadata.data(), node_metadata.size(), kLargeBoxDataBytes);
    CHECK(node_box.Allocate(1) == 0);
    auto node_blocks = FixedBlockAllocator::Attach(
        node_metadata.data() + kBoxBlockMetadataOffset,
        FixedBlockAllocator::kPersistentMetadataBytes,
        node_metadata.data() + kBoxBlockZoneOffset,
        node_metadata.size() - kBoxBlockZoneOffset);
    std::uint8_t* leaf = nullptr;
    for (std::uint32_t id = 0; id < node_blocks.HighWater(); ++id) {
        if (!node_blocks.IsAllocated(id)) continue;
        auto* candidate =
            static_cast<std::uint8_t*>(node_blocks.BlockData(id));
        if (candidate[5] == 0 && candidate[6] == 16) {
            leaf = candidate;
            break;
        }
    }
    CHECK(leaf != nullptr);
    leaf[8] = 2;
    std::fill(leaf + 9, leaf + 24, static_cast<std::uint8_t>(3));
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(BoxAllocator::Attach(
            node_metadata.data(),
            node_metadata.size(),
            kLargeBoxDataBytes));
    });
    auto node_recovery = BoxAllocator::AttachForRecovery(
        node_metadata.data(), node_metadata.size(), kLargeBoxDataBytes);
    node_recovery.Rebuild({});
    node_recovery.Validate();
    CHECK(node_recovery.MetadataNodesUsed() == 1);

    CHECK(node_recovery.Allocate(1) == 0);
    auto rebuilt_blocks = FixedBlockAllocator::Attach(
        node_metadata.data() + kBoxBlockMetadataOffset,
        FixedBlockAllocator::kPersistentMetadataBytes,
        node_metadata.data() + kBoxBlockZoneOffset,
        node_metadata.size() - kBoxBlockZoneOffset);
    leaf = nullptr;
    for (std::uint32_t id = 1; id < rebuilt_blocks.HighWater(); ++id) {
        if (!rebuilt_blocks.IsAllocated(id)) continue;
        auto* candidate =
            static_cast<std::uint8_t*>(rebuilt_blocks.BlockData(id));
        if (candidate[5] == 0 && candidate[6] == 16) {
            leaf = candidate;
            break;
        }
    }
    CHECK(leaf != nullptr);
    std::fill(leaf + 8, leaf + 24, static_cast<std::uint8_t>(0));
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(BoxAllocator::Attach(
            node_metadata.data(),
            node_metadata.size(),
            kLargeBoxDataBytes));
    });
    auto empty_child_recovery = BoxAllocator::AttachForRecovery(
        node_metadata.data(), node_metadata.size(), kLargeBoxDataBytes);
    empty_child_recovery.Rebuild({});
    empty_child_recovery.Validate();
}

void testFixedModelStress() {
    constexpr std::uint32_t capacity = 257;
    constexpr std::size_t payload_bytes = 32;
    std::vector<std::uint8_t> metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> block_zone(
        (2U + payload_bytes) * capacity);
    auto allocator = FixedBlockAllocator::Initialize(
        metadata.data(),
        metadata.size(),
        block_zone.data(),
        block_zone.size(),
        payload_bytes,
        FixedBlockHeaderWidth::Bytes2);
    std::vector<bool> model(capacity, false);
    std::vector<std::uint32_t> live_ids;
    std::mt19937_64 random(0x6b76737061636531ULL);

    for (std::size_t operation = 0; operation < 5000; ++operation) {
        const bool should_allocate = live_ids.empty() || random() % 100U < 58U;
        if (should_allocate) {
            if (live_ids.size() == capacity) {
                expectAllocatorError(
                    AllocatorErrorCode::Capacity,
                    [&] { static_cast<void>(allocator.Allocate()); });
            } else {
                const auto id = allocator.Allocate();
                CHECK(id < capacity);
                CHECK(!model[id]);
                model[id] = true;
                live_ids.push_back(id);
                auto* payload = static_cast<std::uint8_t*>(
                    allocator.BlockData(id));
                payload[0] = static_cast<std::uint8_t>(operation);
            }
        } else {
            const auto random_index = static_cast<std::size_t>(
                random() % static_cast<std::uint64_t>(live_ids.size()));
            const auto id = live_ids[random_index];
            allocator.Free(id);
            model[id] = false;
            live_ids[random_index] = live_ids.back();
            live_ids.pop_back();
        }

        if (operation != 0 && operation % 401U == 0) {
            std::vector<std::uint8_t> bitmap((capacity + 7U) / 8U, 0);
            for (std::uint32_t id = 0; id < capacity; ++id) {
                if (!model[id]) continue;
                const auto byte = static_cast<std::size_t>(id / 8U);
                const auto shift = static_cast<unsigned>(id % 8U);
                bitmap[byte] |= static_cast<std::uint8_t>(1U << shift);
            }
            allocator.Rebuild(bitmap.data(), capacity);
        }
        CHECK(allocator.UsedCount() == live_ids.size());
        for (std::uint32_t id = 0; id < capacity; ++id) {
            CHECK(allocator.IsAllocated(id) == model[id]);
        }
        if (operation % 37U == 0) allocator.Validate();
    }
    allocator.Validate();
}

struct BoxModelAllocation {
    std::uint64_t offset;
    std::uint64_t requested_size;
    std::uint64_t rounded_size;
};

void verifyBoxModel(
    const BoxAllocator& allocator,
    const std::vector<BoxModelAllocation>& model) {
    auto sorted = model;
    std::sort(
        sorted.begin(),
        sorted.end(),
        [](const BoxModelAllocation& left, const BoxModelAllocation& right) {
            return left.offset < right.offset;
        });
    std::uint64_t previous_end = 0;
    bool have_previous = false;
    for (const auto& allocation : sorted) {
        CHECK(allocation.offset + allocation.rounded_size <=
              allocator.DataBytes());
        if (have_previous) CHECK(previous_end <= allocation.offset);
        CHECK(allocator.AllocatedSize(allocation.offset) ==
              allocation.rounded_size);
        previous_end = allocation.offset + allocation.rounded_size;
        have_previous = true;
    }
}

void testBoxModelStress() {
    std::vector<std::uint8_t> metadata(kLargeBoxMetadataBytes);
    auto allocator = BoxAllocator::Initialize(
        metadata.data(), metadata.size(), kLargeBoxDataBytes);
    const std::array<std::uint64_t, 18> request_sizes = {
        1, 7, 8, 9, 17, 63, 119, 120, 121,
        127, 128, 129, 255, 256, 257, 1023, 1921, 4096};
    std::vector<BoxModelAllocation> model;
    std::mt19937_64 random(0x6b76737061636532ULL);

    for (std::size_t operation = 0; operation < 5000; ++operation) {
        const bool should_allocate = model.empty() || random() % 100U < 57U;
        if (should_allocate) {
            const auto size_index = static_cast<std::size_t>(
                random() % static_cast<std::uint64_t>(request_sizes.size()));
            const auto requested_size = request_sizes[size_index];
            const auto rounded_size = BoxAllocator::RoundSize(requested_size);
            try {
                const auto offset = allocator.Allocate(requested_size);
                for (const auto& existing : model) {
                    CHECK(!overlaps(
                        offset,
                        rounded_size,
                        existing.offset,
                        existing.rounded_size));
                }
                model.push_back(BoxModelAllocation{
                    offset, requested_size, rounded_size});
            } catch (const AllocatorError& error) {
                CHECK(error.Code() == AllocatorErrorCode::Capacity);
                allocator.Validate();
            }
        } else {
            const auto random_index = static_cast<std::size_t>(
                random() % static_cast<std::uint64_t>(model.size()));
            allocator.Free(model[random_index].offset);
            model[random_index] = model.back();
            model.pop_back();
        }

        if (operation != 0 && operation % 503U == 0) {
            std::vector<BoxLiveInterval> intervals;
            intervals.reserve(model.size());
            for (const auto& allocation : model) {
                intervals.push_back(BoxLiveInterval{
                    allocation.offset, allocation.requested_size});
            }
            std::shuffle(intervals.begin(), intervals.end(), random);
            allocator.Rebuild(intervals);
        }
        verifyBoxModel(allocator, model);
        if (operation % 41U == 0) allocator.Validate();
    }
    allocator.Validate();
    verifyBoxModel(allocator, model);
}

void runAllocatorTests() {
    testPreparedArtBoxAndBoxRecoveryAllocationOrdinals();
    testFixedStableFormatAndHeaderWidths();
    testFixedTailReuseAndInvalidFree();
    testFixedAndBoxAttachRejectNoncanonicalAutomaticGeometry();
    testFixedRecoveryRebuildAndCorruption();
    testFixedRecoveryCanonicalDescendingFreeChain();
    testFixedPreparedEmptyRecovery();
    testFixedRecoveryPayloadAccessIgnoresDynamicMetadata();
    testFixedSparseZoneDoesNotTouchCapacity();
    testBoxRoundingAndCanonicalDataShape();
    testBoxProvableLayoutHelpers();
    testInitializeStrongGuaranteesAndNoCapabilityAliases();
    testBoxStableFormatOffsetZeroAndReuse();
    testBoxSparseMetadataDoesNotTouchCapacity();
    testBoxReserveAndStrongFailures();
    testBoxRebuildPreflightAndIdempotence();
    testBoxRebuildAllocationFailureIsStrong();
    testMoveOnlyPreparedRecoveryCapabilities();
    testBoxRecoveryFromCorruptMetadata();
    testFixedModelStress();
    testBoxModelStress();
}

} // namespace

int main() {
    return runTest(runAllocatorTests);
}
