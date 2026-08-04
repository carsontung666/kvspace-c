#include "test_support.h"

#include "shm_trie_node_store.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <utility>
#include <vector>

namespace {

std::ptrdiff_t allocations_until_failure = -1;

bool shouldFailAllocation() noexcept {
    if (allocations_until_failure < 0) return false;
    if (allocations_until_failure == 0) {
        allocations_until_failure = -1;
        return true;
    }
    --allocations_until_failure;
    return false;
}

void* allocateBytes(std::size_t bytes) {
    if (shouldFailAllocation()) throw std::bad_alloc();
    if (void* memory = std::malloc(bytes == 0 ? 1 : bytes)) return memory;
    throw std::bad_alloc();
}

} // namespace

void* operator new(std::size_t bytes) {
    return allocateBytes(bytes);
}

void* operator new[](std::size_t bytes) {
    return allocateBytes(bytes);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

using kvspace::detail::FixedBlockAllocator;
using kvspace::detail::FixedBlockHeaderWidth;
using kvspace::detail::TrieNodeRecord;
using kvspace::detail::TrieNodeStore;

constexpr std::size_t kHeaderBytes = 2;
constexpr std::size_t kStride =
    kHeaderBytes + TrieNodeStore::kPersistentNodeBytes;

bool recordsEqual(
    const TrieNodeRecord& left,
    const TrieNodeRecord& right) {
    return left.has_value == right.has_value &&
        left.value_ref == right.value_ref &&
        left.children == right.children;
}

void testEveryRecoveryScratchAllocationFailsBeforeWrites() {
    constexpr std::size_t fanout = 64;
    constexpr std::size_t capacity = fanout + 16U;
    std::vector<std::uint8_t> initial_metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> initial_zone(kStride * capacity, 0xa5);
    auto initial = TrieNodeStore::Initialize(
        initial_metadata.data(),
        initial_metadata.size(),
        initial_zone.data(),
        initial_zone.size(),
        FixedBlockHeaderWidth::Bytes2);
    TrieNodeRecord leaf;
    leaf.has_value = true;
    leaf.value_ref = 7;
    for (std::uint32_t id = 0; id < fanout; ++id) {
        CHECK(initial.Allocate(leaf) == id);
    }
    TrieNodeRecord root;
    for (std::size_t slot = 0; slot < fanout; ++slot) {
        root.children[slot] = static_cast<std::int32_t>(slot);
    }
    constexpr std::uint32_t root_id = fanout;
    CHECK(initial.Allocate(root) == root_id);
    CHECK(initial.Allocate() == root_id + 1U); // unreachable crash residue

    // Sweep every allocation ordinal until recovery first succeeds. The wide
    // root forces several DFS-stack growth allocations in addition to the live
    // bitmap and FixedBlock rebuild scratch; no count is coupled to the current
    // vector growth strategy.
    bool saw_success = false;
    std::size_t failed_ordinals = 0;
    for (std::ptrdiff_t fail_after = 0; fail_after < 64; ++fail_after) {
        auto metadata = initial_metadata;
        auto zone = initial_zone;
        const auto metadata_before = metadata;
        const auto zone_before = zone;
        auto recovery = TrieNodeStore::AttachForRecovery(
            metadata.data(), metadata.size(), zone.data(), zone.size());

        allocations_until_failure = fail_after;
        bool caught = false;
        try {
            auto normal = std::move(recovery).RecoverFromCommittedRoot(root_id);
            allocations_until_failure = -1;
            CHECK(normal.UsedCount() == fanout + 1U);
            normal.Validate();
            saw_success = true;
        } catch (const std::bad_alloc&) {
            caught = true;
        }
        allocations_until_failure = -1;
        if (!caught) break;
        ++failed_ordinals;
        CHECK(metadata == metadata_before);
        CHECK(zone == zone_before);
        CHECK(recordsEqual(recovery.ReadForRecovery(root_id), root));
        CHECK(recordsEqual(recovery.ReadForRecovery(0), leaf));
        CHECK(recordsEqual(
            recovery.ReadForRecovery(static_cast<std::uint32_t>(fanout - 1U)),
            leaf));

        auto normal =
            std::move(recovery).RecoverFromCommittedRoot(root_id);
        CHECK(normal.UsedCount() == fanout + 1U);
        normal.Validate();
    }
    CHECK(saw_success);
    CHECK(failed_ordinals > 3U);
}

void runTrieRecoveryFaultTests() {
    testEveryRecoveryScratchAllocationFailsBeforeWrites();
}

} // namespace

int main() {
    return runTest(runTrieRecoveryFaultTests);
}
