#pragma once

#include "shm_trie_node_store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kvspace::detail {

// A storage-neutral persistent byte trie. Keys are arbitrary byte strings and
// value_ref is an opaque 63-bit token. Published nodes are immutable: updates
// allocate a leaf-to-root path copy and never modify or reclaim an old root.
class ByteTrieIndex final {
public:
    using RootId = std::uint32_t;
    using ValueRef = std::uint64_t;

    class PreparedUpdate;

    struct StagedDelta {
        RootId old_root = 0;
        RootId new_root = 0;
        bool changed = false;
        std::vector<RootId> allocations;
        std::vector<RootId> retired;
    };

    // Non-owning capability for one naturally aligned persistent little-endian
    // uint32 root slot. Initialize/Attach validate the address once. Loads are
    // acquire. Normal copy-on-write publication uses a release CAS that also
    // verifies the update was prepared from the still-committed root. A caller
    // holding the enclosing region's exclusive lock may instead release-store
    // a root during initialization, clear, or recovery commit. Atomic
    // operations copy the encoded byte array as one word; numeric values are
    // decoded and encoded explicitly at this boundary.
    class CommittedRootSlot final {
    public:
        static CommittedRootSlot Initialize(
            void* storage,
            std::size_t storage_bytes,
            RootId initial_root);
        static CommittedRootSlot Attach(
            void* storage,
            std::size_t storage_bytes);

        RootId LoadAcquire() const noexcept;
        void StoreRelease(RootId root) noexcept;

    private:
        friend class PreparedUpdate;

        struct alignas(4) AtomicLe32 final {
            std::uint8_t bytes[4];
        };

        explicit CommittedRootSlot(AtomicLe32* encoded_storage) noexcept
            : encoded_storage_(encoded_storage) {}
        bool CompareExchangeRelease(
            RootId expected_root,
            RootId new_root) noexcept;

        AtomicLe32* encoded_storage_;
    };

    struct Entry {
        std::string key;
        ValueRef value_ref;
    };

    // Owns exactly the nodes allocated by one unpublished path copy. Dropping
    // or explicitly aborting it rolls those nodes back. Publish is one-shot:
    // it release-CASes one validated committed-root slot from OldRoot to
    // NewRoot. A stale CAS fails, rolls the update back, and becomes terminal.
    class PreparedUpdate final {
    public:
        PreparedUpdate(const PreparedUpdate&) = delete;
        PreparedUpdate& operator=(const PreparedUpdate&) = delete;
        PreparedUpdate(PreparedUpdate&& other) noexcept;
        PreparedUpdate& operator=(PreparedUpdate&& other) noexcept;
        ~PreparedUpdate() noexcept;

        RootId OldRoot() const noexcept { return old_root_; }
        RootId NewRoot() const noexcept { return new_root_; }
        bool Changed() const noexcept { return changed_; }

        void Abort() noexcept;
        bool Publish(CommittedRootSlot& committed_root) && noexcept;

        // Transfers ownership to a transaction that will publish one final
        // persistent root for a batch. The caller must either discard every
        // allocation on rollback, or publish NewRoot and then reclaim Retired.
        StagedDelta DetachForBatch() && noexcept;

    private:
        friend class ByteTrieIndex;

        PreparedUpdate(
            TrieNodeStore* store,
            RootId old_root,
            RootId new_root,
            bool changed,
            std::vector<RootId>&& allocations,
            std::vector<RootId>&& retired) noexcept;

        void RollbackOrTerminate() noexcept;

        TrieNodeStore* store_ = nullptr;
        RootId old_root_ = 0;
        RootId new_root_ = 0;
        bool changed_ = false;
        bool active_ = false;
        std::vector<RootId> allocations_;
        std::vector<RootId> retired_;
    };

    explicit ByteTrieIndex(TrieNodeStore& store) noexcept : store_(store) {}

    // Initialization-only operation. The enclosing region must not become
    // externally attachable until this returned root has been stored.
    RootId CreateInitialRoot();

    std::optional<ValueRef> Get(
        RootId root,
        std::string_view key) const;
    bool Exists(RootId root, std::string_view key) const;
    std::vector<Entry> Entries(RootId root) const;
    std::vector<Entry> EntriesFromValidatedRoot(RootId root) const;

    PreparedUpdate Put(
        RootId old_root,
        std::string_view key,
        ValueRef value_ref);
    PreparedUpdate Erase(RootId old_root, std::string_view key);

    // Production batching fast path. The caller must have validated the first
    // root and may then pass only roots returned by this same index. The final
    // root must be fully validated before persistent publication.
    PreparedUpdate PutFromValidatedRoot(
        RootId old_root,
        std::string_view key,
        ValueRef value_ref);
    PreparedUpdate EraseFromValidatedRoot(
        RootId old_root,
        std::string_view key);

    void ValidateRoot(RootId root) const;

private:
    TrieNodeStore& store_;
};

} // namespace kvspace::detail
