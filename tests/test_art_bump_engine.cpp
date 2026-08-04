#include "test_support.h"

#include "kvspace/xvalue.h"
#include "shm_art_bump_index.h"
#include "shm_art_bump_store.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <map>
#include <new>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace allocation_failure {

bool enabled = false;
std::ptrdiff_t fail_after = -1;

} // namespace allocation_failure

void* operator new(std::size_t size) {
    if (allocation_failure::enabled) throw std::bad_alloc();
    if (allocation_failure::fail_after == 0) throw std::bad_alloc();
    if (allocation_failure::fail_after > 0) {
        --allocation_failure::fail_after;
    }
    if (void* allocation = std::malloc(size == 0 ? 1 : size)) {
        return allocation;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* allocation) noexcept {
    std::free(allocation);
}

void operator delete[](void* allocation) noexcept {
    std::free(allocation);
}

void operator delete(void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

void operator delete[](void* allocation, std::size_t) noexcept {
    std::free(allocation);
}

namespace {

using kvspace::detail::ArtBumpCompactJournal;
using kvspace::detail::ArtBumpDerivedState;
using kvspace::detail::ArtBumpGeometry;
using kvspace::detail::ArtBumpHeaderView;
using kvspace::detail::ArtBumpIndex;
using kvspace::detail::ArtBumpIndexState;
using kvspace::detail::ArtBumpJournalState;
using kvspace::detail::ArtBumpApplyStep;
using kvspace::detail::ArtBumpNodeKind;
using kvspace::detail::ArtBumpNodeStore;
using kvspace::detail::ArtBumpRawZone;
using kvspace::detail::ArtBumpReadyField;
using kvspace::detail::ArtBumpStateSnapshot;
using kvspace::detail::AllocatorError;
using kvspace::detail::AllocatorErrorCode;

constexpr std::size_t kRegionBytes = 4U * 1024U * 1024U;
constexpr std::uint64_t kEntryLimit = 64;

class FakeState final : public ArtBumpIndexState {
public:
    struct Event {
        ArtBumpApplyStep step = ArtBumpApplyStep::MutationRawTopPublished;
        std::uint64_t ordinal = 0;
    };

    FakeState(
        ArtBumpHeaderView header,
        std::uint64_t entry_limit,
        const std::uint8_t* region_base,
        const ArtBumpGeometry& geometry) noexcept
        : header_(header),
          entry_limit_(entry_limit),
          region_base_(region_base) {
        for (std::size_t index = 0; index < slab_metadata_.size(); ++index) {
            slab_metadata_[index] = geometry.slabs[index].metadata_offset;
        }
    }

    ArtBumpStateSnapshot LoadSnapshotAcquire() const override {
        const auto header = header_.Decode();
        ArtBumpStateSnapshot result;
        result.committed_root = header_.CommittedRootAcquire();
        result.generation = generation_;
        result.entry_limit = entry_limit_;
        result.active_zone = header_.ActiveZone();
        result.zones = header.raw_zones;
        result.derived = derived_;
        result.common_root_offset = common_root_offset_;
        result.tombstone_count = tombstone_count_;
        result.journal = header_.JournalAcquire();
        return result;
    }

    void WriteJournalCopyingPayload(
        const ArtBumpCompactJournal& journal) noexcept override {
        try {
            header_.StoreJournalPayload(journal);
        } catch (...) {
            std::terminate();
        }
    }

    void WriteJournalReadyField(
        const ArtBumpCompactJournal& journal,
        ArtBumpReadyField field) noexcept override {
        header_.StoreJournalReadyField(journal, field);
    }

    void PublishJournalStateRelease(
        ArtBumpJournalState state) noexcept override {
        header_.StoreJournalStateRelease(state);
    }

    void StoreActiveZone(std::uint32_t zone) noexcept override {
        header_.StoreActiveZone(zone);
    }

    void StoreDerived(const ArtBumpDerivedState& derived) noexcept override {
        derived_ = derived;
        common_root_offset_ = 0;
        tombstone_count_ = 0;
    }

    void StoreGeneration(std::uint64_t generation) noexcept override {
        generation_ = generation;
    }

    void StoreCommittedRootRelease(std::uint64_t root) noexcept override {
        header_.StoreCommittedRootRelease(root);
    }

    void AfterPersistentStep(
        ArtBumpApplyStep step,
        std::uint64_t ordinal) noexcept override {
        std::uint64_t live_nodes = 0;
        for (std::size_t index = 0; index < peak_live_by_kind_.size(); ++index) {
            const auto* encoded = region_base_ + slab_metadata_[index] + 40U;
            std::uint32_t used = 0;
            for (std::size_t byte = 0; byte < sizeof(used); ++byte) {
                used |= static_cast<std::uint32_t>(encoded[byte]) <<
                    static_cast<unsigned>(byte * 8U);
            }
            peak_live_by_kind_[index] =
                std::max(peak_live_by_kind_[index], used);
            live_nodes += used;
        }
        peak_live_nodes_ = std::max(peak_live_nodes_, live_nodes);
        if (event_count_ < events_.size()) {
            events_[event_count_++] = Event{step, ordinal};
        } else {
            event_overflow_ = true;
        }
        if (step == ArtBumpApplyStep::MutationAbortTopRestored) {
            ++abort_top_restored_steps_;
        }
        if (!fail_allocations_after_step_) return;
        fail_allocations_after_step_ = false;
        allocation_failure::enabled = true;
    }

    void FailAllocationsAfterNextPersistentStep() noexcept {
        fail_allocations_after_step_ = true;
    }

    std::uint64_t AbortTopRestoredSteps() const noexcept {
        return abort_top_restored_steps_;
    }

    void ClearEvents() noexcept {
        event_count_ = 0;
        event_overflow_ = false;
    }

    bool SawEvent(
        ArtBumpApplyStep step,
        std::uint64_t ordinal) const noexcept {
        for (std::size_t index = 0; index < event_count_; ++index) {
            if (events_[index].step == step &&
                events_[index].ordinal == ordinal) {
                return true;
            }
        }
        return false;
    }

    bool EventOverflow() const noexcept { return event_overflow_; }

    std::uint64_t PeakLiveNodes() const noexcept { return peak_live_nodes_; }

    std::uint32_t PeakLiveNodes(ArtBumpNodeKind kind) const noexcept {
        return peak_live_by_kind_[static_cast<std::size_t>(kind) - 1U];
    }

private:
    ArtBumpHeaderView header_;
    std::uint64_t entry_limit_ = 0;
    std::uint64_t generation_ = 0;
    ArtBumpDerivedState derived_{};
    std::uint64_t common_root_offset_ = 0;
    std::uint64_t tombstone_count_ = 0;
    std::uint64_t abort_top_restored_steps_ = 0;
    const std::uint8_t* region_base_ = nullptr;
    std::array<std::uint64_t, 4> slab_metadata_{};
    std::uint64_t peak_live_nodes_ = 0;
    std::array<std::uint32_t, 4> peak_live_by_kind_{};
    bool fail_allocations_after_step_ = false;
    std::array<Event, 256> events_{};
    std::size_t event_count_ = 0;
    bool event_overflow_ = false;
};

class AllocationFailureReset final {
public:
    AllocationFailureReset() = default;
    AllocationFailureReset(const AllocationFailureReset&) = delete;
    AllocationFailureReset& operator=(const AllocationFailureReset&) = delete;
    ~AllocationFailureReset() {
        allocation_failure::enabled = false;
        allocation_failure::fail_after = -1;
    }
};

struct Fixture {
    std::vector<std::uint8_t> bytes;
    kvspace::detail::ArtBumpGeometry geometry;
    ArtBumpHeaderView header;
    ArtBumpNodeStore nodes;
    std::array<ArtBumpRawZone, 2> zones;
    FakeState state;
    ArtBumpIndex index;

    explicit Fixture(std::uint64_t entry_limit = kEntryLimit)
        : bytes(kRegionBytes, 0),
          geometry(ArtBumpGeometry::Compute(
              bytes.size(), entry_limit, 2)),
          header(ArtBumpHeaderView::Initialize(
              bytes.data() + geometry.engine_offset,
              ArtBumpGeometry::kEngineHeaderBytes,
              geometry.InitialHeader())),
          nodes(ArtBumpNodeStore::Initialize(
              bytes.data(), bytes.size(), geometry.slabs,
              geometry.node_capacity)),
          zones{
              ArtBumpRawZone::Attach(
                  bytes.data(), bytes.size(), header.Data() + 160U, 32,
                  geometry.raw_zones[0].begin,
                  geometry.raw_zones[0].bytes),
              ArtBumpRawZone::Attach(
                  bytes.data(), bytes.size(), header.Data() + 192U, 32,
                  geometry.raw_zones[1].begin,
                  geometry.raw_zones[1].bytes)},
          state(header, entry_limit, bytes.data(), geometry),
          index(nodes, {&zones[0], &zones[1]}, state) {}
};

std::vector<std::uint8_t> stringValue(std::string_view value) {
    return kvspace::XValue::Str(value).Encode();
}

void commit(ArtBumpIndex::Mutation mutation) {
    auto prepared = std::move(mutation).PrepareCommit();
    std::move(prepared).Apply();
}

void testNonePrefixAndMultiUpdate() {
    Fixture fixture;
    auto mutation = fixture.index.BeginMutation();
    CHECK(mutation.Put("", {}));
    CHECK(mutation.Put("alphabet", stringValue("one")));
    CHECK(mutation.Put("alpha", {}));
    CHECK(mutation.Put("alpine", stringValue("two")));
    CHECK(!mutation.Put("alpha", {}));
    CHECK(mutation.Put("alphabet", stringValue("updated")));
    CHECK(mutation.Erase("alpine"));
    CHECK(!mutation.Erase("missing"));
    commit(std::move(mutation));

    const auto empty = fixture.index.Get("");
    CHECK(empty.has_value());
    CHECK(empty->empty());
    const auto none = fixture.index.Get("alpha");
    CHECK(none.has_value());
    CHECK(none->empty());
    CHECK(fixture.index.Get("alphabet") ==
          std::optional<std::vector<std::uint8_t>>(
              stringValue("updated")));
    CHECK(!fixture.index.Get("alpine").has_value());

    const auto prefix = fixture.index.EntriesWithPrefix("alpha");
    CHECK(prefix.size() == 2);
    CHECK(prefix[0].key == "alpha");
    CHECK(prefix[1].key == "alphabet");
    const auto mid_node_prefix = fixture.index.EntriesWithPrefix("alph");
    CHECK(mid_node_prefix.size() == 2);
    CHECK(fixture.index.EntriesWithPrefix("alphabetical").empty());
    const auto summary = fixture.index.InspectCommitted();
    CHECK(summary.entry_count == 3);
    std::uint64_t slab_used = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        const auto kind = static_cast<ArtBumpNodeKind>(index + 1U);
        slab_used += fixture.nodes.UsedCount(kind);
    }
    CHECK(slab_used == summary.node_count);
}

void testAbortRestoresCommittedAuthority() {
    Fixture fixture;
    {
        auto mutation = fixture.index.BeginMutation();
        CHECK(mutation.Put("stable", stringValue("old")));
        commit(std::move(mutation));
    }
    const auto top = fixture.zones[0].TopAcquire();
    {
        auto mutation = fixture.index.BeginMutation();
        CHECK(mutation.Put("stable", stringValue("new")));
        CHECK(mutation.Put("temporary", stringValue("garbage")));
        mutation.Abort();
    }
    CHECK(fixture.state.AbortTopRestoredSteps() == 1);
    CHECK(fixture.zones[0].TopAcquire() == top);
    CHECK(fixture.index.Get("stable") ==
          std::optional<std::vector<std::uint8_t>>(stringValue("old")));
    CHECK(!fixture.index.Get("temporary").has_value());
    CHECK(fixture.index.InspectCommitted().entry_count == 1);
}

void testAbortSkipsUnchangedRawTop() {
    Fixture fixture(1);
    const auto initial_top = fixture.zones[0].TopAcquire();

    {
        auto mutation = fixture.index.BeginMutation();
        mutation.Abort();
    }
    CHECK(fixture.zones[0].TopAcquire() == initial_top);
    CHECK(fixture.state.AbortTopRestoredSteps() == 0);

    {
        auto mutation = fixture.index.BeginMutation();
        CHECK(mutation.Put("", {}));
        CHECK(fixture.zones[0].TopAcquire() == initial_top);
        mutation.Abort();
    }
    CHECK(!fixture.index.Exists(""));
    CHECK(fixture.zones[0].TopAcquire() == initial_top);
    CHECK(fixture.state.AbortTopRestoredSteps() == 0);

    {
        auto mutation = fixture.index.BeginMutation();
        CHECK(mutation.Put("stable", stringValue("value")));
        commit(std::move(mutation));
    }
    const auto committed_top = fixture.zones[0].TopAcquire();
    {
        auto mutation = fixture.index.BeginMutation();
        CHECK(!mutation.Put("stable", stringValue("value")));
        CHECK(!mutation.Erase("missing"));
        mutation.Abort();
    }
    CHECK(fixture.zones[0].TopAcquire() == committed_top);
    CHECK(fixture.state.AbortTopRestoredSteps() == 0);

    {
        auto mutation = fixture.index.BeginMutation();
        bool caught = false;
        try {
            static_cast<void>(mutation.Put("over-limit", {}));
        } catch (const AllocatorError& error) {
            CHECK(error.Code() == AllocatorErrorCode::Capacity);
            caught = true;
        }
        CHECK(caught);
        mutation.Abort();
    }
    CHECK(fixture.zones[0].TopAcquire() == committed_top);
    CHECK(fixture.state.AbortTopRestoredSteps() == 0);
}

void testAllNodeGrowthAndShrinkThresholds() {
    Fixture fixture;
    auto mutation = fixture.index.BeginMutation();
    for (std::uint8_t edge = 0; edge < 61U; ++edge) {
        const std::string key(1, static_cast<char>(edge));
        CHECK(mutation.Put(key, stringValue(std::to_string(edge))));
    }
    CHECK(mutation.StagedEntryCount() == 61);
    for (std::uint8_t edge = 60U; edge > 0U; --edge) {
        const std::string key(1, static_cast<char>(edge));
        CHECK(mutation.Erase(key));
    }
    CHECK(mutation.StagedEntryCount() == 1);
    commit(std::move(mutation));
    const std::string zero_key(1, '\0');
    CHECK(fixture.index.Get(zero_key).has_value());
    const auto summary = fixture.index.InspectCommitted();
    CHECK(summary.entry_count == 1);
    CHECK(summary.node_count == 1);
    CHECK(fixture.nodes.UsedCount(ArtBumpNodeKind::Node4) == 1);
    CHECK(fixture.nodes.UsedCount(ArtBumpNodeKind::Node16) == 0);
    CHECK(fixture.nodes.UsedCount(ArtBumpNodeKind::Node48) == 0);
    CHECK(fixture.nodes.UsedCount(ArtBumpNodeKind::Node256) == 0);
    CHECK(fixture.state.PeakLiveNodes() <= 4U * kEntryLimit - 2U);
    for (std::size_t index = 0; index < 4; ++index) {
        const auto kind = static_cast<ArtBumpNodeKind>(index + 1U);
        CHECK(fixture.state.PeakLiveNodes(kind) <=
              fixture.nodes.Capacity(kind));
    }
}

void testCompactAndClearPlans() {
    Fixture fixture;
    {
        auto mutation = fixture.index.BeginMutation();
        for (int index = 0; index < 60; ++index) {
            CHECK(mutation.Put(
                "prefix/" + std::to_string(index),
                stringValue("value-" + std::to_string(index))));
        }
        CHECK(mutation.Put("prefix/3", stringValue("replacement")));
        CHECK(mutation.Erase("prefix/7"));
        commit(std::move(mutation));
    }
    const auto before = fixture.index.Entries();
    const auto old_active = fixture.state.LoadSnapshotAcquire().active_zone;
    fixture.state.ClearEvents();
    auto compact = fixture.index.PrepareCompact();
    std::move(compact).Apply();
    CHECK(!fixture.state.EventOverflow());
    CHECK(fixture.state.SawEvent(
        ArtBumpApplyStep::CompactBeforeJournalPayload, 0));
    for (std::uint64_t ordinal = 0; ordinal < 6; ++ordinal) {
        CHECK(fixture.state.SawEvent(
            ArtBumpApplyStep::CompactReadyFieldStored, ordinal));
    }
    for (std::uint64_t ordinal = 0; ordinal < 12; ++ordinal) {
        CHECK(fixture.state.SawEvent(
            ArtBumpApplyStep::CompactSlabRebuildWrite, ordinal));
    }
    const auto after = fixture.state.LoadSnapshotAcquire();
    CHECK(after.journal.state == ArtBumpJournalState::Idle);
    CHECK(after.active_zone == 1U - old_active);
    CHECK(fixture.zones[old_active].TopAcquire() ==
          fixture.zones[old_active].Begin());
    CHECK(fixture.index.Entries().size() == before.size());
    for (std::size_t index = 0; index < before.size(); ++index) {
        CHECK(fixture.index.Entries()[index].key == before[index].key);
        CHECK(fixture.index.Entries()[index].value == before[index].value);
    }
    CHECK(fixture.state.PeakLiveNodes() <= 4U * kEntryLimit - 2U);
    for (std::size_t index = 0; index < 4; ++index) {
        const auto kind = static_cast<ArtBumpNodeKind>(index + 1U);
        CHECK(fixture.state.PeakLiveNodes(kind) <=
              4U * kEntryLimit - 2U);
        CHECK(fixture.state.PeakLiveNodes(kind) <=
              fixture.nodes.Capacity(kind));
    }

    fixture.state.ClearEvents();
    auto clear = fixture.index.PrepareClear();
    std::move(clear).Apply();
    CHECK(!fixture.state.EventOverflow());
    for (std::uint64_t ordinal = 0; ordinal < 12; ++ordinal) {
        CHECK(fixture.state.SawEvent(
            ArtBumpApplyStep::ClearSlabRebuildWrite, ordinal));
    }
    CHECK(fixture.index.Entries().empty());
    CHECK(fixture.index.InspectCommitted().node_count == 0);
    CHECK(fixture.zones[0].TopAcquire() == fixture.zones[0].Begin());
    CHECK(fixture.zones[1].TopAcquire() == fixture.zones[1].Begin());
    CHECK(fixture.zones[0].Epoch() == 1);
    CHECK(fixture.zones[1].Epoch() == 1);
}

void testPreparedCommitAndCompactDoNotAllocateAfterFirstWrite() {
    Fixture fixture;
    auto mutation = fixture.index.BeginMutation();
    CHECK(mutation.Put("allocation-free", stringValue("commit")));
    auto prepared_commit = std::move(mutation).PrepareCommit();
    fixture.state.FailAllocationsAfterNextPersistentStep();
    {
        AllocationFailureReset reset;
        std::move(prepared_commit).Apply();
    }
    CHECK(fixture.index.Exists("allocation-free"));

    auto prepared_compact = fixture.index.PrepareCompact();
    fixture.state.FailAllocationsAfterNextPersistentStep();
    {
        AllocationFailureReset reset;
        std::move(prepared_compact).Apply();
    }
    CHECK(fixture.index.Get("allocation-free") ==
          std::optional<std::vector<std::uint8_t>>(stringValue("commit")));
}

void testCompactPreflightAllocationFailuresAreByteStable() {
    Fixture fixture;
    {
        auto mutation = fixture.index.BeginMutation();
        for (int index = 0; index < 12; ++index) {
            CHECK(mutation.Put(
                "preflight/" + std::to_string(index),
                stringValue("value-" + std::to_string(index))));
        }
        commit(std::move(mutation));
    }
    const auto before = fixture.bytes;
    bool reached_success = false;
    std::size_t failures = 0;
    for (std::ptrdiff_t fail_after = 0; fail_after < 4096; ++fail_after) {
        allocation_failure::fail_after = fail_after;
        try {
            auto prepared = fixture.index.PrepareCompact();
            allocation_failure::fail_after = -1;
            CHECK(fixture.bytes == before);
            reached_success = true;
            break;
        } catch (const std::bad_alloc&) {
            allocation_failure::fail_after = -1;
            CHECK(fixture.bytes == before);
            ++failures;
        }
    }
    CHECK(reached_success);
    CHECK(failures > 0);
}

void testDeterministicMixedBatches() {
    Fixture fixture;
    std::map<std::string, std::vector<std::uint8_t>> expected;
    std::vector<std::string> keys;
    for (int index = 0; index < 40; ++index) {
        keys.push_back(
            "shared/branch/" + std::to_string(index % 7) + "/leaf/" +
            std::to_string(index));
    }
    keys.push_back("");
    keys.push_back(std::string("binary\0key", 10));
    std::mt19937 random(0x4b564142U);
    for (int batch = 0; batch < 20; ++batch) {
        auto mutation = fixture.index.BeginMutation();
        for (int operation = 0; operation < 25; ++operation) {
            const auto key_index = static_cast<std::size_t>(random()) %
                keys.size();
            const auto& key = keys[key_index];
            if (random() % 4U == 0U) {
                const auto existed = expected.erase(key) != 0;
                CHECK(mutation.Erase(key) == existed);
                continue;
            }
            const auto next = random() % 5U == 0U
                ? std::vector<std::uint8_t>{}
                : stringValue(
                      "batch-" + std::to_string(batch) + "-op-" +
                      std::to_string(operation));
            const auto found = expected.find(key);
            const auto changed = found == expected.end() ||
                found->second != next;
            CHECK(mutation.Put(key, next) == changed);
            expected[key] = next;
        }
        commit(std::move(mutation));
        const auto actual = fixture.index.Entries();
        CHECK(actual.size() == expected.size());
        auto expected_iterator = expected.begin();
        for (const auto& entry : actual) {
            CHECK(expected_iterator != expected.end());
            CHECK(entry.key == expected_iterator->first);
            CHECK(entry.value == expected_iterator->second);
            ++expected_iterator;
        }
        CHECK(expected_iterator == expected.end());
        static_cast<void>(fixture.index.InspectCommitted());
    }
}

} // namespace

int main() {
    return runTest([] {
        testNonePrefixAndMultiUpdate();
        testAbortRestoresCommittedAuthority();
        testAbortSkipsUnchangedRawTop();
        testAllNodeGrowthAndShrinkThresholds();
        testCompactAndClearPlans();
        testPreparedCommitAndCompactDoNotAllocateAfterFirstWrite();
        testCompactPreflightAllocationFailuresAreByteStable();
        testDeterministicMixedBatches();
    });
}
