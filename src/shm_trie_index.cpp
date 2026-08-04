#include "shm_trie_index.h"

#include "shm_lifetime.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace kvspace::detail {
namespace {

[[noreturn]] void RaiseIndex(
    AllocatorErrorCode code,
    const std::string& message) {
    throw AllocatorError(code, "kvspace byte trie index: " + message);
}

std::uint8_t KeyByte(std::string_view key, std::size_t index) noexcept {
    return static_cast<std::uint8_t>(
        static_cast<unsigned char>(key[index]));
}

bool IsEmptyNode(const TrieNodeRecord& node) noexcept {
    return !node.has_value && std::all_of(
        node.children.begin(),
        node.children.end(),
        [](std::int32_t child) {
            return child == TrieNodeRecord::kEmptyChild;
        });
}

template <typename Encoded>
Encoded EncodeLeAtomicWord(std::uint32_t value) noexcept {
    Encoded encoded{};
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        encoded.bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
    return encoded;
}

template <typename Encoded>
std::uint32_t DecodeLeAtomicWord(const Encoded& encoded) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(encoded.bytes[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

struct PathNode {
    std::uint32_t id;
    TrieNodeRecord record;
};

void RollbackAllocationsOrTerminate(
    TrieNodeStore* store,
    std::vector<std::uint32_t>* allocations) noexcept {
    if (store == nullptr) return;
    try {
        for (auto cursor = allocations->rbegin();
             cursor != allocations->rend();
             ++cursor) {
            store->DiscardUnpublished(*cursor);
        }
        allocations->clear();
    } catch (...) {
        // A rollback failure means allocator state can no longer be described
        // safely. Continuing could later publish or reuse an ambiguous node.
        std::terminate();
    }
}

} // namespace

ByteTrieIndex::CommittedRootSlot
ByteTrieIndex::CommittedRootSlot::Initialize(
    void* storage,
    std::size_t storage_bytes,
    RootId initial_root) {
    auto slot = Attach(storage, storage_bytes);
    slot.StoreRelease(initial_root);
    return slot;
}

ByteTrieIndex::CommittedRootSlot
ByteTrieIndex::CommittedRootSlot::Attach(
    void* storage,
    std::size_t storage_bytes) {
    if (storage == nullptr || storage_bytes < sizeof(std::uint32_t) ||
        reinterpret_cast<std::uintptr_t>(storage) %
                alignof(AtomicLe32) !=
            0) {
        RaiseIndex(
            AllocatorErrorCode::InvalidArgument,
            "committed root requires aligned uint32 storage");
    }
    static_assert(std::is_trivial_v<AtomicLe32>);
    static_assert(std::is_standard_layout_v<AtomicLe32>);
    static_assert(std::is_trivially_copyable_v<AtomicLe32>);
    static_assert(std::is_trivially_destructible_v<AtomicLe32>);
    static_assert(
        sizeof(AtomicLe32) == sizeof(std::uint32_t) &&
            alignof(AtomicLe32) == alignof(std::uint32_t),
        "persistent root byte codec must occupy one aligned uint32 word");
    static_assert(
        __atomic_always_lock_free(sizeof(AtomicLe32), nullptr),
        "persistent uint32 root publication must be lock-free");
    // Initialize delegates here, so both fresh and attached slots cross the
    // same implicit-lifetime boundary before any typed atomic operation.
    StartImplicitLifetimes(storage, sizeof(AtomicLe32));
    return CommittedRootSlot(static_cast<AtomicLe32*>(storage));
}

ByteTrieIndex::RootId
ByteTrieIndex::CommittedRootSlot::LoadAcquire() const noexcept {
    AtomicLe32 encoded{};
    __atomic_load(encoded_storage_, &encoded, __ATOMIC_ACQUIRE);
    return DecodeLeAtomicWord(encoded);
}

void ByteTrieIndex::CommittedRootSlot::StoreRelease(
    RootId root) noexcept {
    auto encoded = EncodeLeAtomicWord<AtomicLe32>(root);
    __atomic_store(encoded_storage_, &encoded, __ATOMIC_RELEASE);
}

bool ByteTrieIndex::CommittedRootSlot::CompareExchangeRelease(
    RootId expected_root,
    RootId new_root) noexcept {
    auto expected_encoded = EncodeLeAtomicWord<AtomicLe32>(expected_root);
    auto new_encoded = EncodeLeAtomicWord<AtomicLe32>(new_root);
    return __atomic_compare_exchange(
        encoded_storage_,
        &expected_encoded,
        &new_encoded,
        false,
        __ATOMIC_RELEASE,
        __ATOMIC_ACQUIRE);
}

ByteTrieIndex::PreparedUpdate::PreparedUpdate(
    TrieNodeStore* store,
    RootId old_root,
    RootId new_root,
    bool changed,
    std::vector<RootId>&& allocations,
    std::vector<RootId>&& retired) noexcept
    : store_(store),
      old_root_(old_root),
      new_root_(new_root),
      changed_(changed),
      active_(true),
      allocations_(std::move(allocations)),
      retired_(std::move(retired)) {}

ByteTrieIndex::PreparedUpdate::PreparedUpdate(
    PreparedUpdate&& other) noexcept
    : store_(other.store_),
      old_root_(other.old_root_),
      new_root_(other.new_root_),
      changed_(other.changed_),
      active_(other.active_),
      allocations_(std::move(other.allocations_)),
      retired_(std::move(other.retired_)) {
    other.store_ = nullptr;
    other.changed_ = false;
    other.active_ = false;
}

ByteTrieIndex::PreparedUpdate&
ByteTrieIndex::PreparedUpdate::operator=(PreparedUpdate&& other) noexcept {
    if (this == &other) return *this;
    RollbackOrTerminate();
    store_ = other.store_;
    old_root_ = other.old_root_;
    new_root_ = other.new_root_;
    changed_ = other.changed_;
    active_ = other.active_;
    allocations_ = std::move(other.allocations_);
    retired_ = std::move(other.retired_);
    other.store_ = nullptr;
    other.changed_ = false;
    other.active_ = false;
    return *this;
}

ByteTrieIndex::PreparedUpdate::~PreparedUpdate() noexcept {
    RollbackOrTerminate();
}

void ByteTrieIndex::PreparedUpdate::Abort() noexcept {
    RollbackOrTerminate();
}

bool ByteTrieIndex::PreparedUpdate::Publish(
    CommittedRootSlot& committed_root) && noexcept {
    if (!active_) return false;
    if (!changed_) {
        active_ = false;
        return committed_root.LoadAcquire() == old_root_;
    }
    if (!committed_root.CompareExchangeRelease(old_root_, new_root_)) {
        RollbackOrTerminate();
        return false;
    }
    store_ = nullptr;
    allocations_.clear();
    retired_.clear();
    changed_ = false;
    active_ = false;
    return true;
}

ByteTrieIndex::StagedDelta
ByteTrieIndex::PreparedUpdate::DetachForBatch() && noexcept {
    StagedDelta delta;
    if (!active_) return delta;
    delta.old_root = old_root_;
    delta.new_root = new_root_;
    delta.changed = changed_;
    delta.allocations = std::move(allocations_);
    delta.retired = std::move(retired_);
    store_ = nullptr;
    changed_ = false;
    active_ = false;
    return delta;
}

void ByteTrieIndex::PreparedUpdate::RollbackOrTerminate() noexcept {
    RollbackAllocationsOrTerminate(store_, &allocations_);
    store_ = nullptr;
    changed_ = false;
    active_ = false;
    retired_.clear();
}

ByteTrieIndex::RootId ByteTrieIndex::CreateInitialRoot() {
    if (store_.HighWater() != 0 || store_.UsedCount() != 0) {
        RaiseIndex(
            AllocatorErrorCode::InvalidArgument,
            "initial root requires a fresh node store");
    }
    return store_.Allocate();
}

std::optional<ByteTrieIndex::ValueRef> ByteTrieIndex::Get(
    RootId root,
    std::string_view key) const {
    auto node = store_.Read(root);
    for (std::size_t index = 0; index < key.size(); ++index) {
        const auto child = node.children[KeyByte(key, index)];
        if (child == TrieNodeRecord::kEmptyChild) return std::nullopt;
        node = store_.Read(static_cast<RootId>(child));
    }
    if (!node.has_value) return std::nullopt;
    return node.value_ref;
}

bool ByteTrieIndex::Exists(RootId root, std::string_view key) const {
    return Get(root, key).has_value();
}

void ByteTrieIndex::ValidateRoot(RootId root) const {
    std::unordered_set<RootId> seen;
    std::vector<RootId> stack;
    seen.insert(root);
    stack.push_back(root);
    while (!stack.empty()) {
        const auto id = stack.back();
        stack.pop_back();
        const auto node = store_.Read(id);
        if (id != root && IsEmptyNode(node)) {
            RaiseIndex(
                AllocatorErrorCode::Corrupt,
                "root contains a noncanonical empty non-root leaf");
        }
        for (const auto child : node.children) {
            if (child == TrieNodeRecord::kEmptyChild) continue;
            const auto child_id = static_cast<RootId>(child);
            if (!seen.insert(child_id).second) {
                RaiseIndex(
                    AllocatorErrorCode::Corrupt,
                    "root contains a cycle or shared node");
            }
            stack.push_back(child_id);
        }
    }
}

std::vector<ByteTrieIndex::Entry> ByteTrieIndex::Entries(RootId root) const {
    ValidateRoot(root);
    return EntriesFromValidatedRoot(root);
}

std::vector<ByteTrieIndex::Entry>
ByteTrieIndex::EntriesFromValidatedRoot(RootId root) const {

    struct Frame {
        TrieNodeRecord node;
        std::uint16_t next_child = 0;
        bool value_emitted = false;
    };

    std::vector<Entry> entries;
    std::vector<Frame> stack;
    std::string key;
    stack.push_back(Frame{store_.Read(root), 0, false});
    while (!stack.empty()) {
        auto& frame = stack.back();
        if (!frame.value_emitted) {
            frame.value_emitted = true;
            if (frame.node.has_value) {
                entries.push_back(Entry{key, frame.node.value_ref});
            }
            continue;
        }

        while (frame.next_child < frame.node.children.size() &&
               frame.node.children[frame.next_child] ==
                   TrieNodeRecord::kEmptyChild) {
            ++frame.next_child;
        }
        if (frame.next_child == frame.node.children.size()) {
            stack.pop_back();
            if (!stack.empty()) key.pop_back();
            continue;
        }

        const auto slot = frame.next_child;
        const auto child = frame.node.children[slot];
        ++frame.next_child;
        key.push_back(static_cast<char>(static_cast<unsigned char>(slot)));
        stack.push_back(Frame{
            store_.Read(static_cast<RootId>(child)), 0, false});
    }
    return entries;
}

ByteTrieIndex::PreparedUpdate ByteTrieIndex::Put(
    RootId old_root,
    std::string_view key,
    ValueRef value_ref) {
    ValidateRoot(old_root);
    return PutFromValidatedRoot(old_root, key, value_ref);
}

ByteTrieIndex::PreparedUpdate ByteTrieIndex::PutFromValidatedRoot(
    RootId old_root,
    std::string_view key,
    ValueRef value_ref) {
    if (value_ref > TrieNodeCodec::kMaxValueRef) {
        RaiseIndex(
            AllocatorErrorCode::InvalidArgument,
            "value reference exceeds 63 bits");
    }
    if (key.size() == std::numeric_limits<std::size_t>::max()) {
        RaiseIndex(AllocatorErrorCode::InvalidArgument, "key is too large");
    }
    std::vector<PathNode> path;
    std::vector<RootId> allocations;
    std::vector<RootId> retired;
    path.reserve(key.size() + 1U);
    allocations.reserve(key.size() + 1U);

    path.push_back(PathNode{old_root, store_.Read(old_root)});
    std::size_t matched = 0;
    while (matched < key.size()) {
        const auto child = path.back().record.children[KeyByte(key, matched)];
        if (child == TrieNodeRecord::kEmptyChild) break;
        path.push_back(PathNode{
            static_cast<RootId>(child),
            store_.Read(static_cast<RootId>(child))});
        ++matched;
    }

    if (matched == key.size() &&
        path.back().record.has_value &&
        path.back().record.value_ref == value_ref) {
        return PreparedUpdate(
            nullptr,
            old_root,
            old_root,
            false,
            std::move(allocations),
            std::move(retired));
    }

    const auto allocate = [this, &allocations](
                              const TrieNodeRecord& record) {
        const auto id = store_.Allocate(record);
        allocations.push_back(id);
        return id;
    };

    try {
        RootId new_child = 0;
        if (matched == key.size()) {
            auto terminal = path.back().record;
            terminal.has_value = true;
            terminal.value_ref = value_ref;
            new_child = allocate(terminal);
        } else {
            TrieNodeRecord leaf;
            leaf.has_value = true;
            leaf.value_ref = value_ref;
            new_child = allocate(leaf);
            for (std::size_t depth = key.size() - 1U;
                 depth > matched;
                 --depth) {
                TrieNodeRecord parent;
                parent.children[KeyByte(key, depth)] =
                    static_cast<std::int32_t>(new_child);
                new_child = allocate(parent);
            }

            auto existing_parent = path.back().record;
            existing_parent.children[KeyByte(key, matched)] =
                static_cast<std::int32_t>(new_child);
            new_child = allocate(existing_parent);
        }

        for (std::size_t depth = matched; depth > 0; --depth) {
            auto parent = path[depth - 1U].record;
            parent.children[KeyByte(key, depth - 1U)] =
                static_cast<std::int32_t>(new_child);
            new_child = allocate(parent);
        }
        retired.reserve(path.size());
        for (const auto& item : path) retired.push_back(item.id);
        return PreparedUpdate(
            &store_,
            old_root,
            new_child,
            true,
            std::move(allocations),
            std::move(retired));
    } catch (...) {
        RollbackAllocationsOrTerminate(&store_, &allocations);
        throw;
    }
}

ByteTrieIndex::PreparedUpdate ByteTrieIndex::Erase(
    RootId old_root,
    std::string_view key) {
    ValidateRoot(old_root);
    return EraseFromValidatedRoot(old_root, key);
}

ByteTrieIndex::PreparedUpdate ByteTrieIndex::EraseFromValidatedRoot(
    RootId old_root,
    std::string_view key) {
    if (key.size() == std::numeric_limits<std::size_t>::max()) {
        RaiseIndex(AllocatorErrorCode::InvalidArgument, "key is too large");
    }
    std::vector<PathNode> path;
    std::vector<RootId> allocations;
    std::vector<RootId> retired;
    path.reserve(key.size() + 1U);
    allocations.reserve(key.size() + 1U);
    path.push_back(PathNode{old_root, store_.Read(old_root)});
    for (std::size_t depth = 0; depth < key.size(); ++depth) {
        const auto child = path.back().record.children[KeyByte(key, depth)];
        if (child == TrieNodeRecord::kEmptyChild) {
            return PreparedUpdate(
                nullptr,
                old_root,
                old_root,
                false,
                std::move(allocations),
                std::move(retired));
        }
        path.push_back(PathNode{
            static_cast<RootId>(child),
            store_.Read(static_cast<RootId>(child))});
    }
    if (!path.back().record.has_value) {
        return PreparedUpdate(
            nullptr,
            old_root,
            old_root,
            false,
            std::move(allocations),
            std::move(retired));
    }

    const auto allocate = [this, &allocations](
                              const TrieNodeRecord& record) {
        const auto id = store_.Allocate(record);
        allocations.push_back(id);
        return id;
    };

    try {
        auto terminal = path.back().record;
        terminal.has_value = false;
        terminal.value_ref = 0;
        bool child_present = key.empty() || !IsEmptyNode(terminal);
        RootId new_child = 0;
        if (child_present) new_child = allocate(terminal);

        for (std::size_t depth = key.size(); depth > 0; --depth) {
            auto parent = path[depth - 1U].record;
            parent.children[KeyByte(key, depth - 1U)] = child_present
                ? static_cast<std::int32_t>(new_child)
                : TrieNodeRecord::kEmptyChild;
            child_present = depth == 1U || !IsEmptyNode(parent);
            if (child_present) new_child = allocate(parent);
        }
        retired.reserve(path.size());
        for (const auto& item : path) retired.push_back(item.id);
        return PreparedUpdate(
            &store_,
            old_root,
            new_child,
            true,
            std::move(allocations),
            std::move(retired));
    } catch (...) {
        RollbackAllocationsOrTerminate(&store_, &allocations);
        throw;
    }
}

} // namespace kvspace::detail
