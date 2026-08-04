#include "test_support.h"

#include "shm_trie_index.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using kvspace::detail::AllocatorError;
using kvspace::detail::AllocatorErrorCode;
using kvspace::detail::ByteTrieIndex;
using kvspace::detail::FixedBlockAllocator;
using kvspace::detail::FixedBlockHeaderWidth;
using kvspace::detail::TrieNodeRecord;
using kvspace::detail::TrieNodeStore;

constexpr std::size_t kBlockHeaderBytes = 2;
constexpr std::size_t kNodeStride =
    kBlockHeaderBytes + TrieNodeStore::kPersistentNodeBytes;
constexpr std::size_t kHighWaterOffset = 36;
constexpr std::size_t kUsedCountOffset = 40;
constexpr std::size_t kFreeHeadOffset = 44;

bool recordsEqual(
    const TrieNodeRecord& left,
    const TrieNodeRecord& right) {
    return left.has_value == right.has_value &&
        left.value_ref == right.value_ref &&
        left.children == right.children;
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

class SharedMapping final {
public:
    explicit SharedMapping(std::size_t bytes)
        : bytes_(bytes),
          address_(::mmap(
              nullptr,
              bytes,
              PROT_READ | PROT_WRITE,
              MAP_SHARED | MAP_ANONYMOUS,
              -1,
              0)) {
        CHECK(address_ != MAP_FAILED);
    }

    ~SharedMapping() {
        if (address_ != MAP_FAILED) {
            static_cast<void>(::munmap(address_, bytes_));
        }
    }

    SharedMapping(const SharedMapping&) = delete;
    SharedMapping& operator=(const SharedMapping&) = delete;

    void* Data() noexcept { return address_; }

private:
    std::size_t bytes_;
    void* address_;
};

std::uint8_t* nodePayload(void* zone, std::uint32_t id) noexcept {
    return static_cast<std::uint8_t*>(zone) +
        static_cast<std::size_t>(id) * kNodeStride + kBlockHeaderBytes;
}

std::size_t reachableCount(
    const TrieNodeStore& store,
    std::uint32_t root) {
    std::vector<std::uint32_t> pending{root};
    std::vector<std::uint8_t> seen(store.Capacity(), 0);
    seen[root] = 1;
    std::size_t count = 0;
    while (!pending.empty()) {
        const auto id = pending.back();
        pending.pop_back();
        ++count;
        const auto node = store.Read(id);
        for (const auto child : node.children) {
            if (child == TrieNodeRecord::kEmptyChild) continue;
            const auto child_id = static_cast<std::uint32_t>(child);
            CHECK(seen[child_id] == 0);
            seen[child_id] = 1;
            pending.push_back(child_id);
        }
    }
    return count;
}

std::uint32_t directChild(
    const TrieNodeStore& store,
    std::uint32_t root,
    unsigned char key_byte) {
    const auto child = store.Read(root).children[key_byte];
    CHECK(child != TrieNodeRecord::kEmptyChild);
    return static_cast<std::uint32_t>(child);
}

void waitForCleanExit(pid_t child) {
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

struct PersistentByte {
    bool metadata;
    std::size_t offset;
};

void applyWritePrefix(
    std::vector<std::uint8_t>* metadata,
    std::vector<std::uint8_t>* zone,
    const std::vector<std::uint8_t>& final_metadata,
    const std::vector<std::uint8_t>& final_zone,
    const std::vector<PersistentByte>& writes,
    std::size_t write_count) {
    CHECK(write_count <= writes.size());
    for (std::size_t index = 0; index < write_count; ++index) {
        const auto write = writes[index];
        if (write.metadata) {
            (*metadata)[write.offset] = final_metadata[write.offset];
        } else {
            (*zone)[write.offset] = final_zone[write.offset];
        }
    }
}

void testEveryAllocateByteCutRecoversTheOldRoot() {
    constexpr std::size_t capacity = 8;
    std::vector<std::uint8_t> base_metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> base_zone(kNodeStride * capacity, 0xa5);
    auto base = TrieNodeStore::Initialize(
        base_metadata.data(), base_metadata.size(),
        base_zone.data(), base_zone.size(),
        FixedBlockHeaderWidth::Bytes2);
    CHECK(base.Allocate() == 0);

    auto allocated_metadata = base_metadata;
    auto allocated_zone = base_zone;
    auto allocated = TrieNodeStore::Attach(
        allocated_metadata.data(), allocated_metadata.size(),
        allocated_zone.data(), allocated_zone.size());
    TrieNodeRecord staged;
    staged.has_value = true;
    staged.value_ref = 99;
    CHECK(allocated.Allocate(staged) == 1);

    // FixedBlock high-water allocation writes the new block header, high-water
    // and used counters, then TrieNodeStore copies the complete payload. Sweep
    // every byte boundary of that persistent sequence with root 0 unchanged.
    std::vector<PersistentByte> writes;
    for (std::size_t byte = 0; byte < kBlockHeaderBytes; ++byte) {
        writes.push_back(PersistentByte{false, kNodeStride + byte});
    }
    for (std::size_t byte = 0; byte < sizeof(std::uint32_t); ++byte) {
        writes.push_back(PersistentByte{true, kHighWaterOffset + byte});
    }
    for (std::size_t byte = 0; byte < sizeof(std::uint32_t); ++byte) {
        writes.push_back(PersistentByte{true, kUsedCountOffset + byte});
    }
    for (std::size_t byte = 0;
         byte < TrieNodeStore::kPersistentNodeBytes;
         ++byte) {
        writes.push_back(PersistentByte{
            false, kNodeStride + kBlockHeaderBytes + byte});
    }

    for (std::size_t cut = 0; cut <= writes.size(); ++cut) {
        auto metadata = base_metadata;
        auto zone = base_zone;
        applyWritePrefix(
            &metadata, &zone,
            allocated_metadata, allocated_zone,
            writes, cut);
        auto recovery = TrieNodeStore::AttachForRecovery(
            metadata.data(), metadata.size(), zone.data(), zone.size());
        auto normal = std::move(recovery).RecoverFromCommittedRoot(0);
        CHECK(metadata == base_metadata);
        CHECK(std::equal(
            base_zone.begin(),
            base_zone.begin() + static_cast<std::ptrdiff_t>(kNodeStride),
            zone.begin()));
        CHECK(normal.UsedCount() == 1);
        CHECK(normal.HighWater() == 1);
        CHECK(normal.Read(0).children == TrieNodeRecord{}.children);
        expectAllocatorError(AllocatorErrorCode::InvalidId, [&] {
            static_cast<void>(normal.Read(1));
        });
        CHECK(normal.Allocate(staged) == 1);
        CHECK(normal.Read(1).value_ref == 99);
        normal.Validate();
    }
}

void testEveryPathCopyStageBeforePublicationIsReclaimed() {
    constexpr std::size_t capacity = 16;
    for (std::size_t completed_allocations = 1;
         completed_allocations <= 4;
         ++completed_allocations) {
        std::vector<std::uint8_t> metadata(
            FixedBlockAllocator::kPersistentMetadataBytes);
        std::vector<std::uint8_t> zone(kNodeStride * capacity, 0xa5);
        auto store = TrieNodeStore::Initialize(
            metadata.data(), metadata.size(), zone.data(), zone.size(),
            FixedBlockHeaderWidth::Bytes2);
        CHECK(store.Allocate() == 0); // still-committed empty root

        TrieNodeRecord leaf;
        leaf.has_value = true;
        leaf.value_ref = 3;
        auto child = store.Allocate(leaf);
        if (completed_allocations >= 2U) {
            TrieNodeRecord parent_c;
            parent_c.children[static_cast<unsigned char>('c')] =
                static_cast<std::int32_t>(child);
            child = store.Allocate(parent_c);
        }
        if (completed_allocations >= 3U) {
            TrieNodeRecord parent_b;
            parent_b.children[static_cast<unsigned char>('b')] =
                static_cast<std::int32_t>(child);
            child = store.Allocate(parent_b);
        }
        if (completed_allocations >= 4U) {
            TrieNodeRecord copied_root;
            copied_root.children[static_cast<unsigned char>('a')] =
                static_cast<std::int32_t>(child);
            child = store.Allocate(copied_root);
        }
        static_cast<void>(child);

        auto recovery = TrieNodeStore::AttachForRecovery(
            metadata.data(), metadata.size(), zone.data(), zone.size());
        auto normal = std::move(recovery).RecoverFromCommittedRoot(0);
        CHECK(normal.UsedCount() == 1);
        CHECK(normal.HighWater() == 1);
        CHECK(recordsEqual(normal.Read(0), TrieNodeRecord{}));
        CHECK(normal.Allocate() == 1);
        normal.Validate();
    }
}

void testEveryRebuildWritePrefixReplaysToOneCanonicalState() {
    constexpr std::size_t capacity = 32;
    std::vector<std::uint8_t> initial_metadata(
        FixedBlockAllocator::kPersistentMetadataBytes);
    std::vector<std::uint8_t> initial_zone(kNodeStride * capacity, 0xa5);
    auto store = TrieNodeStore::Initialize(
        initial_metadata.data(), initial_metadata.size(),
        initial_zone.data(), initial_zone.size(),
        FixedBlockHeaderWidth::Bytes2);
    ByteTrieIndex index(store);
    auto root = index.CreateInitialRoot();
    std::uint32_t root_storage = root;
    auto committed = ByteTrieIndex::CommittedRootSlot::Initialize(
        &root_storage, sizeof(root_storage), root);
    auto first = index.Put(root, "a", 1);
    CHECK(std::move(first).Publish(committed));
    root = committed.LoadAcquire();
    auto second = index.Put(root, "b", 2);
    CHECK(std::move(second).Publish(committed));
    root = committed.LoadAcquire();

    const auto before_metadata = initial_metadata;
    const auto before_zone = initial_zone;
    auto canonical_metadata = before_metadata;
    auto canonical_zone = before_zone;
    auto canonical_recovery = TrieNodeStore::AttachForRecovery(
        canonical_metadata.data(), canonical_metadata.size(),
        canonical_zone.data(), canonical_zone.size());
    auto canonical =
        std::move(canonical_recovery).RecoverFromCommittedRoot(root);
    const auto canonical_high_water = canonical.HighWater();
    CHECK(canonical.UsedCount() == reachableCount(canonical, root));

    // Rebuild writes all block headers from high ID down, then high-water,
    // used-count, and free-head. Replay owner-death recovery after every byte
    // prefix of that exact sequence and require one byte-identical result.
    std::vector<PersistentByte> writes;
    for (std::uint32_t cursor = canonical_high_water; cursor > 0; --cursor) {
        const auto id = cursor - 1U;
        for (std::size_t byte = 0; byte < kBlockHeaderBytes; ++byte) {
            writes.push_back(PersistentByte{
                false,
                static_cast<std::size_t>(id) * kNodeStride + byte});
        }
    }
    for (const std::size_t field : {
             kHighWaterOffset, kUsedCountOffset, kFreeHeadOffset}) {
        for (std::size_t byte = 0; byte < sizeof(std::uint32_t); ++byte) {
            writes.push_back(PersistentByte{true, field + byte});
        }
    }

    for (std::size_t cut = 0; cut <= writes.size(); ++cut) {
        auto metadata = before_metadata;
        auto zone = before_zone;
        applyWritePrefix(
            &metadata, &zone,
            canonical_metadata, canonical_zone,
            writes, cut);
        auto recovery = TrieNodeStore::AttachForRecovery(
            metadata.data(), metadata.size(), zone.data(), zone.size());
        auto normal = std::move(recovery).RecoverFromCommittedRoot(root);
        CHECK(metadata == canonical_metadata);
        CHECK(zone == canonical_zone);
        CHECK(*ByteTrieIndex(normal).Get(root, "a") == 1);
        CHECK(*ByteTrieIndex(normal).Get(root, "b") == 2);
        normal.Validate();
    }
}

void testCrashBeforeAndAfterCommittedRootPublication() {
    constexpr std::uint32_t capacity = 128;
    constexpr std::size_t metadata_bytes =
        FixedBlockAllocator::kPersistentMetadataBytes;
    constexpr std::size_t zone_bytes =
        kNodeStride * static_cast<std::size_t>(capacity);
    SharedMapping metadata_mapping(metadata_bytes);
    SharedMapping zone_mapping(zone_bytes);
    SharedMapping root_mapping(sizeof(std::uint32_t));
    auto* metadata = metadata_mapping.Data();
    auto* zone = zone_mapping.Data();
    auto* committed_root =
        static_cast<std::uint32_t*>(root_mapping.Data());

    auto store = TrieNodeStore::Initialize(
        metadata,
        metadata_bytes,
        zone,
        zone_bytes,
        FixedBlockHeaderWidth::Bytes2);
    ByteTrieIndex index(store);
    auto root = index.CreateInitialRoot();
    auto committed = ByteTrieIndex::CommittedRootSlot::Initialize(
        committed_root, sizeof(*committed_root), root);

    auto add_aa = index.Put(root, "aa", 1);
    CHECK(std::move(add_aa).Publish(committed));
    root = committed.LoadAcquire();
    auto add_z = index.Put(root, "z", 2);
    CHECK(std::move(add_z).Publish(committed));
    root = committed.LoadAcquire();

    // Canonicalize away initialization/update history so the next used-count
    // deltas describe only the simulated crash residue.
    auto recovery = TrieNodeStore::AttachForRecovery(
        metadata, metadata_bytes, zone, zone_bytes);
    store = std::move(recovery).RecoverFromCommittedRoot(root);
    CHECK(store.UsedCount() == reachableCount(store, root));
    CHECK(*index.Get(root, "aa") == 1);
    CHECK(*index.Get(root, "z") == 2);

    // The child exits with a complete path copy still armed and never stores
    // its candidate root. _exit intentionally bypasses the RAII rollback just
    // as process death would.
    const auto child_before_publish = ::fork();
    CHECK(child_before_publish >= 0);
    if (child_before_publish == 0) {
        try {
            auto child_store = TrieNodeStore::Attach(
                metadata, metadata_bytes, zone, zone_bytes);
            ByteTrieIndex child_index(child_store);
            auto staged = child_index.Put(
                committed.LoadAcquire(), "ab", 3);
            static_cast<void>(staged.NewRoot());
            ::_exit(0);
        } catch (...) {
            ::_exit(2);
        }
    }
    waitForCleanExit(child_before_publish);
    CHECK(committed.LoadAcquire() == root);

    auto old_root_recovery = TrieNodeStore::AttachForRecovery(
        metadata, metadata_bytes, zone, zone_bytes);
    store = std::move(old_root_recovery).RecoverFromCommittedRoot(root);
    CHECK(store.UsedCount() == reachableCount(store, root));
    CHECK(!index.Get(root, "ab").has_value());
    CHECK(*index.Get(root, "aa") == 1);
    CHECK(*index.Get(root, "z") == 2);

    const auto old_root = root;
    const auto shared_z = directChild(store, old_root, 'z');
    std::array<std::uint8_t, TrieNodeStore::kPersistentNodeBytes>
        shared_z_payload{};
    std::memcpy(
        shared_z_payload.data(),
        nodePayload(zone, shared_z),
        shared_z_payload.size());

    // This child release-publishes the new root and dies before any optional
    // old-version reclamation. Recovery must keep the new tree and its shared
    // off-path child while reclaiming old-only path nodes.
    const auto child_after_publish = ::fork();
    CHECK(child_after_publish >= 0);
    if (child_after_publish == 0) {
        try {
            auto child_store = TrieNodeStore::Attach(
                metadata, metadata_bytes, zone, zone_bytes);
            ByteTrieIndex child_index(child_store);
            auto child_committed =
                ByteTrieIndex::CommittedRootSlot::Attach(
                    committed_root, sizeof(*committed_root));
            auto update = child_index.Put(
                child_committed.LoadAcquire(), "ab", 3);
            if (!std::move(update).Publish(child_committed)) ::_exit(3);
            ::_exit(0);
        } catch (...) {
            ::_exit(2);
        }
    }
    waitForCleanExit(child_after_publish);
    root = committed.LoadAcquire();
    CHECK(root != old_root);

    auto new_root_recovery = TrieNodeStore::AttachForRecovery(
        metadata, metadata_bytes, zone, zone_bytes);
    store = std::move(new_root_recovery).RecoverFromCommittedRoot(root);
    index.ValidateRoot(root);
    CHECK(store.UsedCount() == reachableCount(store, root));
    CHECK(*index.Get(root, "aa") == 1);
    CHECK(*index.Get(root, "ab") == 3);
    CHECK(*index.Get(root, "z") == 2);
    CHECK(directChild(store, root, 'z') == shared_z);
    CHECK(std::memcmp(
              shared_z_payload.data(),
              nodePayload(zone, shared_z),
              shared_z_payload.size()) == 0);
    expectAllocatorError(AllocatorErrorCode::NotAllocated, [&] {
        static_cast<void>(store.Read(old_root));
    });
    store.Validate();
}

void runTrieCowRecoveryTests() {
    testEveryAllocateByteCutRecoversTheOldRoot();
    testEveryPathCopyStageBeforePublicationIsReclaimed();
    testEveryRebuildWritePrefixReplaysToOneCanonicalState();
    testCrashBeforeAndAfterCommittedRootPublication();
}

} // namespace

int main() {
    return runTest(runTrieCowRecoveryTests);
}
