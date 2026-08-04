#include "test_support.h"

#include "shm_trie_index.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using kvspace::detail::AllocatorError;
using kvspace::detail::AllocatorErrorCode;
using kvspace::detail::ByteTrieIndex;
using kvspace::detail::FixedBlockAllocator;
using kvspace::detail::FixedBlockHeaderWidth;
using kvspace::detail::TrieNodeCodec;
using kvspace::detail::TrieNodeRecord;
using kvspace::detail::TrieNodeStore;

constexpr std::size_t kBlockHeaderBytes = 2;
constexpr std::size_t kNodeStride =
    kBlockHeaderBytes + TrieNodeStore::kPersistentNodeBytes;

template <typename Function>
void expectAllocatorError(AllocatorErrorCode expected_code, Function&& function) {
    bool caught = false;
    try {
        function();
    } catch (const AllocatorError& error) {
        CHECK(error.Code() == expected_code);
        caught = true;
    }
    CHECK(caught);
}

class TrieFixture final {
public:
    explicit TrieFixture(std::uint32_t capacity)
        : metadata_(FixedBlockAllocator::kPersistentMetadataBytes),
          node_zone_(
              kNodeStride * static_cast<std::size_t>(capacity),
              static_cast<std::uint8_t>(0xa5)),
          store_(TrieNodeStore::Initialize(
              metadata_.data(),
              metadata_.size(),
              node_zone_.data(),
              node_zone_.size(),
              FixedBlockHeaderWidth::Bytes2)),
          index_(store_),
          initial_root_(index_.CreateInitialRoot()) {
        CHECK(store_.Capacity() == capacity);
    }

    TrieNodeStore& Store() noexcept { return store_; }
    ByteTrieIndex& Index() noexcept { return index_; }
    ByteTrieIndex::RootId InitialRoot() const noexcept {
        return initial_root_;
    }

private:
    std::vector<std::uint8_t> metadata_;
    std::vector<std::uint8_t> node_zone_;
    TrieNodeStore store_;
    ByteTrieIndex index_;
    ByteTrieIndex::RootId initial_root_;
};

std::string byteString(std::initializer_list<unsigned int> byte_values) {
    std::string result;
    result.reserve(byte_values.size());
    for (const auto byte_value : byte_values) {
        CHECK(byte_value <= 0xffU);
        result.push_back(
            static_cast<char>(static_cast<unsigned char>(byte_value)));
    }
    return result;
}

struct UnsignedByteLess final {
    bool operator()(const std::string& left, const std::string& right) const
        noexcept {
        return std::lexicographical_compare(
            left.begin(),
            left.end(),
            right.begin(),
            right.end(),
            [](char lhs_byte, char rhs_byte) noexcept {
                return static_cast<unsigned char>(lhs_byte) <
                    static_cast<unsigned char>(rhs_byte);
            });
    }
};

using Model = std::map<
    std::string,
    ByteTrieIndex::ValueRef,
    UnsignedByteLess>;

void testCommittedRootUsesLittleEndianAtomicBytes() {
    alignas(std::uint32_t) std::array<std::uint8_t, 4> storage{};
    auto committed = ByteTrieIndex::CommittedRootSlot::Initialize(
        storage.data(), storage.size(), UINT32_C(0x01020304));
    const std::array<std::uint8_t, 4> initial_bytes = {
        0x04, 0x03, 0x02, 0x01};
    CHECK(storage == initial_bytes);
    CHECK(committed.LoadAcquire() == UINT32_C(0x01020304));

    TrieFixture fixture(8);
    committed = ByteTrieIndex::CommittedRootSlot::Initialize(
        storage.data(), storage.size(), fixture.InitialRoot());
    auto update = fixture.Index().Put(fixture.InitialRoot(), "key", 1);
    const auto new_root = update.NewRoot();
    CHECK(std::move(update).Publish(committed));
    CHECK(committed.LoadAcquire() == new_root);
    for (std::size_t index = 0; index < storage.size(); ++index) {
        CHECK(storage[index] == static_cast<std::uint8_t>(
            new_root >> static_cast<unsigned>(index * 8U)));
    }
}

void testRejectsNoncanonicalEmptyNonRootLeaf() {
    TrieFixture fixture(4);
    const auto empty_child = fixture.Store().Allocate();
    TrieNodeRecord root;
    root.children[0] = static_cast<std::int32_t>(empty_child);
    const auto malformed_root = fixture.Store().Allocate(root);
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        fixture.Index().ValidateRoot(malformed_root);
    });
}

ByteTrieIndex::RootId publishUpdate(
    ByteTrieIndex::PreparedUpdate&& update) {
    std::uint32_t root_storage = update.OldRoot();
    auto committed = ByteTrieIndex::CommittedRootSlot::Initialize(
        &root_storage, sizeof(root_storage), update.OldRoot());
    CHECK(std::move(update).Publish(committed));
    CHECK(!update.Changed());
    return committed.LoadAcquire();
}

void checkModel(
    const ByteTrieIndex& index,
    ByteTrieIndex::RootId root,
    const Model& expected) {
    // Entries validates the complete reachable graph before enumerating it.
    const auto entries = index.Entries(root);
    CHECK(entries.size() == expected.size());

    auto expected_cursor = expected.begin();
    for (const auto& entry : entries) {
        CHECK(expected_cursor != expected.end());
        CHECK(entry.key == expected_cursor->first);
        CHECK(entry.value_ref == expected_cursor->second);
        const auto found = index.Get(root, entry.key);
        CHECK(found.has_value());
        CHECK(*found == entry.value_ref);
        CHECK(index.Exists(root, entry.key));
        ++expected_cursor;
    }
    CHECK(expected_cursor == expected.end());

    // Random differential keys below are at most seven bytes long.
    const auto absent_probe =
        byteString({0xdeU, 0xadU, 0xbeU, 0xefU, 0x80U, 0U, 0xffU, 1U});
    CHECK(!index.Get(root, absent_probe).has_value());
    CHECK(!index.Exists(root, absent_probe));
}

std::size_t checkReachableShape(
    const TrieNodeStore& store,
    ByteTrieIndex::RootId root) {
    struct PendingNode {
        ByteTrieIndex::RootId id;
        bool is_root;
    };

    std::unordered_set<ByteTrieIndex::RootId> seen;
    std::vector<PendingNode> pending;
    seen.insert(root);
    pending.push_back(PendingNode{root, true});
    while (!pending.empty()) {
        const auto current = pending.back();
        pending.pop_back();
        const auto record = store.Read(current.id);
        const bool has_child = std::any_of(
            record.children.begin(),
            record.children.end(),
            [](std::int32_t child_id) noexcept {
                return child_id != TrieNodeRecord::kEmptyChild;
            });
        if (!current.is_root) {
            CHECK(record.has_value || has_child);
        }
        for (const auto child_id : record.children) {
            if (child_id == TrieNodeRecord::kEmptyChild) continue;
            const auto child =
                static_cast<ByteTrieIndex::RootId>(child_id);
            CHECK(seen.insert(child).second);
            pending.push_back(PendingNode{child, false});
        }
    }
    return seen.size();
}

void testFreshRootEmptyKeyAndBoundaryReferences() {
    TrieFixture fixture(128);
    auto& store = fixture.Store();
    auto& index = fixture.Index();
    const auto empty_root = fixture.InitialRoot();

    CHECK(empty_root == 0);
    CHECK(store.HighWater() == 1);
    CHECK(store.UsedCount() == 1);
    CHECK(!index.Get(empty_root, std::string_view{}).has_value());
    CHECK(!index.Exists(empty_root, std::string_view{}));
    CHECK(index.Entries(empty_root).empty());
    CHECK(checkReachableShape(store, empty_root) == 1U);

    const auto used_before_reinitialize = store.UsedCount();
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&index] {
        static_cast<void>(index.CreateInitialRoot());
    });
    CHECK(store.UsedCount() == used_before_reinitialize);

    auto put_zero = index.Put(empty_root, std::string_view{}, 0);
    CHECK(put_zero.Changed());
    CHECK(put_zero.OldRoot() == empty_root);
    CHECK(put_zero.NewRoot() != empty_root);
    CHECK(!index.Get(empty_root, std::string_view{}).has_value());
    const auto pending_zero = index.Get(put_zero.NewRoot(), std::string_view{});
    CHECK(pending_zero.has_value());
    CHECK(*pending_zero == 0);
    const auto zero_root = publishUpdate(std::move(put_zero));
    const auto zero_value = index.Get(zero_root, std::string_view{});
    CHECK(zero_value.has_value());
    CHECK(*zero_value == 0);

    const auto used_before_noop = store.UsedCount();
    auto same_zero = index.Put(zero_root, std::string_view{}, 0);
    CHECK(!same_zero.Changed());
    CHECK(same_zero.OldRoot() == zero_root);
    CHECK(same_zero.NewRoot() == zero_root);
    CHECK(publishUpdate(std::move(same_zero)) == zero_root);
    CHECK(store.UsedCount() == used_before_noop);

    auto put_maximum = index.Put(
        zero_root,
        std::string_view{},
        TrieNodeCodec::kMaxValueRef);
    CHECK(put_maximum.Changed());
    const auto maximum_root = publishUpdate(std::move(put_maximum));
    const auto maximum_value = index.Get(maximum_root, std::string_view{});
    CHECK(maximum_value.has_value());
    CHECK(*maximum_value == TrieNodeCodec::kMaxValueRef);
    CHECK(index.Get(zero_root, std::string_view{}).has_value());
    CHECK(*index.Get(zero_root, std::string_view{}) == 0);

    auto child_update = index.Put(maximum_root, byteString({0U}), 7);
    const auto root_with_child = publishUpdate(std::move(child_update));
    auto erase_empty = index.Erase(root_with_child, std::string_view{});
    CHECK(erase_empty.Changed());
    const auto no_empty_value_root = publishUpdate(std::move(erase_empty));
    CHECK(!index.Get(no_empty_value_root, std::string_view{}).has_value());
    CHECK(index.Get(no_empty_value_root, byteString({0U})).has_value());
    CHECK(*index.Get(no_empty_value_root, byteString({0U})) == 7);
    CHECK(*index.Get(root_with_child, std::string_view{}) ==
          TrieNodeCodec::kMaxValueRef);

    const auto before_absent_erase = store.UsedCount();
    auto erase_empty_again =
        index.Erase(no_empty_value_root, std::string_view{});
    CHECK(!erase_empty_again.Changed());
    CHECK(publishUpdate(std::move(erase_empty_again)) == no_empty_value_root);
    CHECK(store.UsedCount() == before_absent_erase);

    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(index.Put(
            no_empty_value_root,
            std::string_view{},
            TrieNodeCodec::kMaxValueRef + 1U));
    });
    CHECK(store.UsedCount() == before_absent_erase);
    CHECK(*index.Get(no_empty_value_root, byteString({0U})) == 7);
    store.Validate();
}

void testBinaryKeysAndUnsignedLexicographicEntries() {
    TrieFixture fixture(512);
    auto& index = fixture.Index();
    auto root = fixture.InitialRoot();

    const std::vector<std::pair<std::string, ByteTrieIndex::ValueRef>> expected{
        {byteString({}), 0},
        {byteString({0U}), 10},
        {byteString({0U, 0U}), 11},
        {byteString({0U, 0xffU}), 12},
        {byteString({1U}), 13},
        {byteString({0x7fU}), 14},
        {byteString({0x80U}), 15},
        {byteString({0x80U, 0U}), 16},
        {byteString({0xffU}), TrieNodeCodec::kMaxValueRef},
        {byteString({0xffU, 0U}), 18},
    };
    const std::array<std::size_t, 10> insertion_order{
        {8U, 3U, 6U, 1U, 9U, 0U, 5U, 2U, 7U, 4U}};
    for (const auto position : insertion_order) {
        auto update = index.Put(
            root,
            expected[position].first,
            expected[position].second);
        CHECK(update.Changed());
        root = publishUpdate(std::move(update));
    }

    const auto entries = index.Entries(root);
    CHECK(entries.size() == expected.size());
    for (std::size_t position = 0; position < expected.size(); ++position) {
        CHECK(entries[position].key == expected[position].first);
        CHECK(entries[position].value_ref == expected[position].second);
        CHECK(index.Exists(root, expected[position].first));
        const auto value = index.Get(root, expected[position].first);
        CHECK(value.has_value());
        CHECK(*value == expected[position].second);
    }
    CHECK(!index.Exists(root, byteString({0U, 0x80U})));
    CHECK(!index.Get(root, byteString({0xfeU})).has_value());
    CHECK(checkReachableShape(fixture.Store(), root) == 10U);
    fixture.Store().Validate();
}

void testPutEraseNoopsAndImmutableRoots() {
    TrieFixture fixture(512);
    auto& store = fixture.Store();
    auto& index = fixture.Index();
    const auto root0 = fixture.InitialRoot();
    const Model model0;

    auto put_cat = index.Put(root0, "cat", 10);
    const auto root1 = publishUpdate(std::move(put_cat));
    Model model1{{"cat", 10}};
    checkModel(index, root0, model0);
    checkModel(index, root1, model1);

    auto put_car = index.Put(root1, "car", 20);
    const auto root2 = publishUpdate(std::move(put_car));
    auto model2 = model1;
    model2["car"] = 20;
    checkModel(index, root1, model1);
    checkModel(index, root2, model2);

    auto put_prefix = index.Put(root2, "ca", 30);
    const auto root3 = publishUpdate(std::move(put_prefix));
    auto model3 = model2;
    model3["ca"] = 30;
    checkModel(index, root2, model2);
    checkModel(index, root3, model3);

    const auto used_before_noops = store.UsedCount();
    auto same_cat = index.Put(root3, "cat", 10);
    CHECK(!same_cat.Changed());
    CHECK(same_cat.OldRoot() == root3);
    CHECK(same_cat.NewRoot() == root3);
    CHECK(publishUpdate(std::move(same_cat)) == root3);
    auto erase_missing_branch = index.Erase(root3, "cab");
    CHECK(!erase_missing_branch.Changed());
    CHECK(publishUpdate(std::move(erase_missing_branch)) == root3);
    auto erase_non_value_prefix = index.Erase(root3, "c");
    CHECK(!erase_non_value_prefix.Changed());
    CHECK(publishUpdate(std::move(erase_non_value_prefix)) == root3);
    CHECK(store.UsedCount() == used_before_noops);

    auto replace_cat = index.Put(root3, "cat", 11);
    const auto root4 = publishUpdate(std::move(replace_cat));
    auto model4 = model3;
    model4["cat"] = 11;
    checkModel(index, root3, model3);
    checkModel(index, root4, model4);

    auto erase_cat = index.Erase(root4, "cat");
    const auto root5 = publishUpdate(std::move(erase_cat));
    auto model5 = model4;
    CHECK(model5.erase("cat") == 1U);
    checkModel(index, root4, model4);
    checkModel(index, root5, model5);

    const auto used_before_repeat_erase = store.UsedCount();
    auto erase_cat_again = index.Erase(root5, "cat");
    CHECK(!erase_cat_again.Changed());
    CHECK(publishUpdate(std::move(erase_cat_again)) == root5);
    CHECK(store.UsedCount() == used_before_repeat_erase);

    checkModel(index, root0, model0);
    checkModel(index, root1, model1);
    checkModel(index, root2, model2);
    checkModel(index, root3, model3);
    checkModel(index, root4, model4);
    checkModel(index, root5, model5);
    store.Validate();
}

void testPreparedUpdateLifecycleAndPublishOrdering() {
    TrieFixture fixture(512);
    auto& store = fixture.Store();
    auto& index = fixture.Index();
    auto committed_root = fixture.InitialRoot();
    const auto base_used = store.UsedCount();

    auto explicitly_aborted = index.Put(committed_root, "abort", 1);
    const auto aborted_root = explicitly_aborted.NewRoot();
    CHECK(explicitly_aborted.Changed());
    CHECK(store.UsedCount() > base_used);
    CHECK(index.Get(aborted_root, "abort").has_value());
    CHECK(!index.Get(committed_root, "abort").has_value());
    explicitly_aborted.Abort();
    CHECK(!explicitly_aborted.Changed());
    CHECK(store.UsedCount() == base_used);
    expectAllocatorError(AllocatorErrorCode::NotAllocated, [&] {
        static_cast<void>(index.Get(aborted_root, "abort"));
    });
    explicitly_aborted.Abort();
    CHECK(store.UsedCount() == base_used);

    ByteTrieIndex::RootId destructor_root = 0;
    {
        auto destructor_rollback = index.Put(committed_root, "destructor", 2);
        destructor_root = destructor_rollback.NewRoot();
        CHECK(store.UsedCount() > base_used);
        CHECK(index.Get(destructor_root, "destructor").has_value());
    }
    CHECK(store.UsedCount() == base_used);
    expectAllocatorError(AllocatorErrorCode::NotAllocated, [&] {
        static_cast<void>(index.Get(destructor_root, "destructor"));
    });

    auto move_source = index.Put(committed_root, "moved", 3);
    const auto moved_root = move_source.NewRoot();
    const auto used_while_moved_update_exists = store.UsedCount();
    auto move_owner = std::move(move_source);
    CHECK(!move_source.Changed());
    move_source.Abort();
    CHECK(store.UsedCount() == used_while_moved_update_exists);
    CHECK(*index.Get(moved_root, "moved") == 3);
    committed_root = publishUpdate(std::move(move_owner));
    CHECK(committed_root == moved_root);
    const auto used_after_move_publish = store.UsedCount();
    move_owner.Abort();
    CHECK(store.UsedCount() == used_after_move_publish);

    const auto before_move_assignment = store.UsedCount();
    auto move_target = index.Put(committed_root, "discarded target", 4);
    const auto discarded_target_root = move_target.NewRoot();
    const auto used_after_target = store.UsedCount();
    auto move_incoming = index.Put(committed_root, "incoming", 5);
    const auto incoming_root = move_incoming.NewRoot();
    const auto used_with_both = store.UsedCount();
    const auto incoming_node_count = used_with_both - used_after_target;
    CHECK(incoming_node_count > 0);
    move_target = std::move(move_incoming);
    CHECK(!move_incoming.Changed());
    CHECK(store.UsedCount() == before_move_assignment + incoming_node_count);
    expectAllocatorError(AllocatorErrorCode::NotAllocated, [&] {
        static_cast<void>(index.Get(discarded_target_root, "discarded target"));
    });
    CHECK(*index.Get(incoming_root, "incoming") == 5);
    move_incoming.Abort();
    CHECK(store.UsedCount() == before_move_assignment + incoming_node_count);
    move_target.Abort();
    CHECK(store.UsedCount() == before_move_assignment);
    expectAllocatorError(AllocatorErrorCode::NotAllocated, [&] {
        static_cast<void>(index.Get(incoming_root, "incoming"));
    });

    std::uint32_t root_storage = committed_root;
    auto committed = ByteTrieIndex::CommittedRootSlot::Initialize(
        &root_storage, sizeof(root_storage), committed_root);
    auto ordered_publish = index.Put(committed_root, "published", 6);
    const auto candidate_root = ordered_publish.NewRoot();
    const auto used_before_publish = store.UsedCount();
    CHECK(std::move(ordered_publish).Publish(committed));
    CHECK(!ordered_publish.Changed());
    CHECK(committed.LoadAcquire() == candidate_root);
    CHECK(*index.Get(candidate_root, "published") == 6);
    CHECK(!index.Get(committed_root, "published").has_value());
    CHECK(store.UsedCount() == used_before_publish);
    ordered_publish.Abort();
    CHECK(store.UsedCount() == used_before_publish);
    CHECK(!std::move(ordered_publish).Publish(committed));
    CHECK(committed.LoadAcquire() == candidate_root);
    committed_root = committed.LoadAcquire();
    CHECK(*index.Get(committed_root, "published") == 6);

    auto no_change = index.Put(committed_root, "published", 6);
    CHECK(!no_change.Changed());
    CHECK(std::move(no_change).Publish(committed));
    CHECK(!no_change.Changed());
    CHECK(!std::move(no_change).Publish(committed));
    CHECK(store.UsedCount() == used_before_publish);

    // Two prepared updates from one root cannot overwrite each other. The
    // stale CAS fails, rolls back its nodes, and is permanently terminal.
    const auto used_before_race = store.UsedCount();
    auto stale = index.Put(committed_root, "stale", 7);
    const auto stale_root = stale.NewRoot();
    auto winner = index.Put(committed_root, "winner", 8);
    const auto winner_root = winner.NewRoot();
    CHECK(std::move(winner).Publish(committed));
    CHECK(committed.LoadAcquire() == winner_root);
    CHECK(!std::move(stale).Publish(committed));
    CHECK(!stale.Changed());
    CHECK(!std::move(stale).Publish(committed));
    CHECK(committed.LoadAcquire() == winner_root);
    expectAllocatorError(AllocatorErrorCode::NotAllocated, [&] {
        static_cast<void>(index.Get(stale_root, "stale"));
    });
    CHECK(store.UsedCount() > used_before_race);
    CHECK(*index.Get(winner_root, "winner") == 8);

    alignas(std::uint32_t) std::array<std::uint8_t, 8> raw_root{};
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(ByteTrieIndex::CommittedRootSlot::Attach(
            raw_root.data() + 1U, sizeof(std::uint32_t)));
    });
    expectAllocatorError(AllocatorErrorCode::InvalidArgument, [&] {
        static_cast<void>(ByteTrieIndex::CommittedRootSlot::Attach(
            nullptr, sizeof(std::uint32_t)));
    });
    store.Validate();
}

void testCapacityFailureRollsBackEveryAllocation() {
    // A four-byte missing key needs five copied/new nodes. Only four slots are
    // free, so the failure occurs after allocating every remaining slot.
    TrieFixture fixture(5);
    auto& store = fixture.Store();
    auto& index = fixture.Index();
    const auto root = fixture.InitialRoot();
    const auto used_before_failure = store.UsedCount();
    const auto too_long = byteString({1U, 2U, 3U, 4U});

    expectAllocatorError(AllocatorErrorCode::Capacity, [&] {
        static_cast<void>(index.Put(root, too_long, 9));
    });
    CHECK(store.UsedCount() == used_before_failure);
    CHECK(store.HighWater() == store.Capacity());
    CHECK(index.Entries(root).empty());
    CHECK(!index.Get(root, too_long).has_value());
    index.ValidateRoot(root);
    store.Validate();

    // All four slots must be reusable: this exact update consumes all four.
    const auto exact_fit_key = byteString({1U, 2U, 3U});
    auto exact_fit = index.Put(root, exact_fit_key, 10);
    CHECK(exact_fit.Changed());
    CHECK(store.UsedCount() == store.Capacity());
    const auto exact_fit_root = exact_fit.NewRoot();
    CHECK(*index.Get(exact_fit_root, exact_fit_key) == 10);
    CHECK(!index.Get(root, exact_fit_key).has_value());
    exact_fit.Abort();
    CHECK(store.UsedCount() == used_before_failure);
    expectAllocatorError(AllocatorErrorCode::NotAllocated, [&] {
        static_cast<void>(index.Get(exact_fit_root, exact_fit_key));
    });
    store.Validate();

    auto retry = index.Put(root, exact_fit_key, 10);
    const auto committed_root = publishUpdate(std::move(retry));
    CHECK(store.UsedCount() == store.Capacity());
    CHECK(*index.Get(committed_root, exact_fit_key) == 10);
    CHECK(index.Entries(root).empty());
    store.Validate();
}

void testErasePrunesOnlyDeadSuffixes() {
    TrieFixture fixture(512);
    auto& store = fixture.Store();
    auto& index = fixture.Index();
    const auto root0 = fixture.InitialRoot();

    auto add_ab = index.Put(root0, "ab", 1);
    const auto root1 = publishUpdate(std::move(add_ab));
    auto add_ac = index.Put(root1, "ac", 2);
    const auto root2 = publishUpdate(std::move(add_ac));
    auto add_a = index.Put(root2, "a", 3);
    const auto root3 = publishUpdate(std::move(add_a));
    const Model model3{{"a", 3}, {"ab", 1}, {"ac", 2}};
    checkModel(index, root3, model3);
    CHECK(checkReachableShape(store, root3) == 4U);

    auto remove_ab = index.Erase(root3, "ab");
    const auto root4 = publishUpdate(std::move(remove_ab));
    const Model model4{{"a", 3}, {"ac", 2}};
    checkModel(index, root4, model4);
    CHECK(checkReachableShape(store, root4) == 3U);
    checkModel(index, root3, model3);

    auto remove_ac = index.Erase(root4, "ac");
    const auto root5 = publishUpdate(std::move(remove_ac));
    const Model model5{{"a", 3}};
    checkModel(index, root5, model5);
    CHECK(checkReachableShape(store, root5) == 2U);
    checkModel(index, root4, model4);

    auto remove_a = index.Erase(root5, "a");
    const auto root6 = publishUpdate(std::move(remove_a));
    const Model model6;
    checkModel(index, root6, model6);
    CHECK(checkReachableShape(store, root6) == 1U);
    const auto final_record = store.Read(root6);
    CHECK(!final_record.has_value);
    CHECK(std::all_of(
        final_record.children.begin(),
        final_record.children.end(),
        [](std::int32_t child_id) noexcept {
            return child_id == TrieNodeRecord::kEmptyChild;
        }));
    checkModel(index, root5, model5);

    auto add_only_path = index.Put(root6, "xyz", 4);
    const auto path_root = publishUpdate(std::move(add_only_path));
    CHECK(checkReachableShape(store, path_root) == 4U);
    auto remove_only_path = index.Erase(path_root, "xyz");
    const auto pruned_root = publishUpdate(std::move(remove_only_path));
    CHECK(checkReachableShape(store, pruned_root) == 1U);
    CHECK(index.Entries(pruned_root).empty());
    CHECK(*index.Get(path_root, "xyz") == 4);
    store.Validate();
}

std::string randomKey(std::mt19937_64& generator) {
    const auto length = static_cast<std::size_t>(generator() % 8U);
    std::string key;
    key.reserve(length);
    for (std::size_t position = 0; position < length; ++position) {
        const auto byte_value = static_cast<unsigned char>(generator() & 0xffU);
        key.push_back(static_cast<char>(byte_value));
    }
    return key;
}

ByteTrieIndex::ValueRef randomValue(std::mt19937_64& generator) {
    const auto selector = generator() % 8U;
    if (selector == 0U) return 0;
    if (selector == 1U) return TrieNodeCodec::kMaxValueRef;
    return generator() & TrieNodeCodec::kMaxValueRef;
}

void testRandomDifferentialPersistentModel() {
    TrieFixture fixture(10000);
    auto& store = fixture.Store();
    auto& index = fixture.Index();
    auto root = fixture.InitialRoot();
    Model model;
    std::mt19937_64 generator(UINT64_C(0x6279746574726965));

    std::vector<std::string> key_pool{
        byteString({}),
        byteString({0U}),
        byteString({0xffU}),
        byteString({0U, 0xffU}),
        byteString({0x80U, 0U}),
    };
    while (key_pool.size() < 96U) {
        auto candidate = randomKey(generator);
        if (std::find(key_pool.begin(), key_pool.end(), candidate) ==
            key_pool.end()) {
            key_pool.push_back(std::move(candidate));
        }
    }

    std::vector<std::pair<ByteTrieIndex::RootId, Model>> checkpoints;
    checkpoints.emplace_back(root, model);
    for (std::size_t step = 0; step < 900U; ++step) {
        const auto key_position = static_cast<std::size_t>(
            generator() % static_cast<std::uint64_t>(key_pool.size()));
        const auto& key = key_pool[key_position];
        const bool do_put = generator() % 100U < 63U;
        auto next_value = randomValue(generator);
        const auto existing = model.find(key);
        if (do_put && existing != model.end() && generator() % 4U == 0U) {
            next_value = existing->second;
        }

        const auto old_root = root;
        const auto old_model = model;
        auto expected = model;
        bool expected_changed = false;
        if (do_put) {
            const auto expected_existing = expected.find(key);
            expected_changed = expected_existing == expected.end() ||
                expected_existing->second != next_value;
            expected[key] = next_value;
        } else {
            expected_changed = expected.erase(key) != 0U;
        }

        const auto used_before = store.UsedCount();
        auto prepared = do_put
            ? index.Put(old_root, key, next_value)
            : index.Erase(old_root, key);
        CHECK(prepared.OldRoot() == old_root);
        CHECK(prepared.Changed() == expected_changed);
        if (expected_changed) {
            CHECK(prepared.NewRoot() != old_root);
            CHECK(store.UsedCount() > used_before);
        } else {
            CHECK(prepared.NewRoot() == old_root);
            CHECK(store.UsedCount() == used_before);
        }
        checkModel(index, prepared.NewRoot(), expected);

        const auto candidate_root = prepared.NewRoot();
        const bool abort_update = expected_changed && generator() % 11U == 0U;
        if (abort_update) {
            prepared.Abort();
            CHECK(store.UsedCount() == used_before);
            expectAllocatorError(AllocatorErrorCode::NotAllocated, [&] {
                static_cast<void>(index.Entries(candidate_root));
            });
            root = old_root;
            model = old_model;
            checkModel(index, root, model);
        } else {
            root = publishUpdate(std::move(prepared));
            model = std::move(expected);
            const auto used_after_publish = store.UsedCount();
            prepared.Abort();
            CHECK(store.UsedCount() == used_after_publish);
        }

        const auto expected_value = model.find(key);
        const auto observed_value = index.Get(root, key);
        CHECK(observed_value.has_value() == (expected_value != model.end()));
        if (expected_value != model.end()) {
            CHECK(*observed_value == expected_value->second);
        }
        if (step % 29U == 0U) {
            checkModel(index, root, model);
            CHECK(checkReachableShape(store, root) >= 1U);
        }
        if (step % 101U == 0U) store.Validate();
        if (step % 137U == 0U) checkpoints.emplace_back(root, model);
    }

    for (const auto& checkpoint : checkpoints) {
        checkModel(index, checkpoint.first, checkpoint.second);
    }
    checkModel(index, root, model);
    store.Validate();
}

void runTrieIndexTests() {
    testCommittedRootUsesLittleEndianAtomicBytes();
    testRejectsNoncanonicalEmptyNonRootLeaf();
    testFreshRootEmptyKeyAndBoundaryReferences();
    testBinaryKeysAndUnsignedLexicographicEntries();
    testPutEraseNoopsAndImmutableRoots();
    testPreparedUpdateLifecycleAndPublishOrdering();
    testCapacityFailureRollsBackEveryAllocation();
    testErasePrunesOnlyDeadSuffixes();
    testRandomDifferentialPersistentModel();
}

} // namespace

int main() {
    return runTest(runTrieIndexTests);
}
