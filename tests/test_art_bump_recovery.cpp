#include "test_support.h"

#include "kvspace/xvalue.h"
#include "shm_art_bump_index.h"
#include "shm_art_bump_store.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace allocation_failure {

bool enabled = false;

} // namespace allocation_failure

void* operator new(std::size_t size) {
    if (allocation_failure::enabled) throw std::bad_alloc();
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

using namespace kvspace::detail;

constexpr std::size_t kRegionBytes = 4U * 1024U * 1024U;
constexpr std::uint64_t kEntryLimit = 32;

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

class RecoveryState final : public ArtBumpIndexState {
public:
    struct Event {
        ArtBumpApplyStep step = ArtBumpApplyStep::MutationRawTopPublished;
        std::uint64_t ordinal = 0;
    };

    RecoveryState(ArtBumpHeaderView header, std::uint64_t limit) noexcept
        : header_(header), limit_(limit) {}

    ArtBumpStateSnapshot LoadSnapshotAcquire() const override {
        const auto decoded = header_.DecodeForRecovery();
        ArtBumpStateSnapshot result;
        result.committed_root = header_.CommittedRootAcquire();
        result.generation = generation_;
        result.entry_limit = limit_;
        result.active_zone = header_.ActiveZone();
        result.zones = decoded.raw_zones;
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
    void StoreDerived(const ArtBumpDerivedState& value) noexcept override {
        derived_ = value;
        common_root_offset_ = 0;
        tombstone_count_ = 0;
    }
    void StoreGeneration(std::uint64_t value) noexcept override {
        generation_ = value;
    }
    void StoreCommittedRootRelease(std::uint64_t root) noexcept override {
        header_.StoreCommittedRootRelease(root);
    }

    void AfterPersistentStep(
        ArtBumpApplyStep step,
        std::uint64_t ordinal) noexcept override {
        if (event_count_ < events_.size()) {
            events_[event_count_++] = Event{step, ordinal};
        } else {
            event_overflow_ = true;
        }
        if (!fail_allocations_after_step_) return;
        fail_allocations_after_step_ = false;
        allocation_failure::enabled = true;
    }

    void FailAllocationsAfterNextPersistentStep() noexcept {
        fail_allocations_after_step_ = true;
    }

    void InstallJournal(ArtBumpCompactJournal journal) {
        header_.StoreJournalPayload(journal);
        header_.StoreJournalStateRelease(journal.state);
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

private:
    ArtBumpHeaderView header_;
    std::uint64_t limit_ = 0;
    std::uint64_t generation_ = 0;
    ArtBumpDerivedState derived_{};
    std::uint64_t common_root_offset_ = 0;
    std::uint64_t tombstone_count_ = 0;
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
    ~AllocationFailureReset() { allocation_failure::enabled = false; }
};

struct Fixture {
    std::vector<std::uint8_t> bytes;
    ArtBumpGeometry geometry;
    ArtBumpHeaderView header;
    ArtBumpNodeStore nodes;
    std::array<ArtBumpRawZone, 2> zones;
    RecoveryState state;
    ArtBumpIndex index;

    Fixture()
        : bytes(kRegionBytes, 0),
          geometry(ArtBumpGeometry::Compute(
              bytes.size(), kEntryLimit, 2)),
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
          state(header, kEntryLimit),
          index(nodes, {&zones[0], &zones[1]}, state) {}

    void AttachNodesForRecovery() {
        header = ArtBumpHeaderView::AttachForRecovery(
            bytes.data() + geometry.engine_offset,
            ArtBumpGeometry::kEngineHeaderBytes);
        zones[0] = ArtBumpRawZone::AttachForRecovery(
            bytes.data(), bytes.size(), header.Data() + 160U, 32,
            geometry.raw_zones[0].begin,
            geometry.raw_zones[0].bytes);
        zones[1] = ArtBumpRawZone::AttachForRecovery(
            bytes.data(), bytes.size(), header.Data() + 192U, 32,
            geometry.raw_zones[1].begin,
            geometry.raw_zones[1].bytes);
        nodes = ArtBumpNodeStore::AttachForRecovery(
            bytes.data(), bytes.size(), geometry.slabs,
            geometry.node_capacity);
    }

    void Recover() {
        AttachNodesForRecovery();
        auto recovery = index.PrepareRecovery();
        std::move(recovery).Apply();
    }

    void CorruptNode(std::uint64_t reference) {
        const auto kind = nodes.ReferenceKind(reference);
        const auto kind_index = static_cast<std::size_t>(kind) - 1U;
        const auto slot = static_cast<std::uint64_t>(
            nodes.SlotIndex(reference));
        const auto payload = static_cast<std::uint64_t>(
            ArtBumpNodeCodec::PayloadBytes(kind));
        const auto offset = geometry.slabs[kind_index].zone_offset +
            slot * payload;
        std::memset(
            bytes.data() + static_cast<std::size_t>(offset),
            0xa5,
            static_cast<std::size_t>(payload));
    }

    void RewriteNode(
        std::uint64_t reference,
        const ArtBumpNodeRecord& record) {
        const auto kind = nodes.ReferenceKind(reference);
        CHECK(record.kind == kind);
        const auto encoded_bytes = ArtBumpNodeCodec::PayloadBytes(kind);
        CHECK(reference <= bytes.size());
        CHECK(encoded_bytes <= bytes.size() -
              static_cast<std::size_t>(reference));
        CHECK(ArtBumpNodeCodec::EncodeTo(
                  record,
                  bytes.data() + static_cast<std::size_t>(reference),
                  encoded_bytes) == encoded_bytes);
    }
};

std::vector<std::uint8_t> value(std::string_view text) {
    return kvspace::XValue::Str(text).Encode();
}

void putOne(Fixture* fixture, std::string_view key, std::string_view text) {
    auto mutation = fixture->index.BeginMutation();
    CHECK(mutation.Put(key, value(text)));
    auto commit = std::move(mutation).PrepareCommit();
    std::move(commit).Apply();
}

void checkRebuildCuts(
    const RecoveryState& state,
    ArtBumpApplyStep step) {
    CHECK(!state.EventOverflow());
    for (std::uint64_t ordinal = 0; ordinal < 12; ++ordinal) {
        CHECK(state.SawEvent(step, ordinal));
    }
}

ArtBumpCompactJournal copyingJournal(const ArtBumpStateSnapshot& snapshot) {
    ArtBumpCompactJournal journal;
    journal.old_root = snapshot.committed_root;
    journal.source_top = snapshot.zones[snapshot.active_zone].top;
    journal.operation_generation = snapshot.generation + 1U;
    journal.new_root = UINT64_MAX; // READY-only garbage must be ignored.
    journal.target_top = UINT64_MAX;
    journal.node_count = UINT64_MAX;
    journal.entry_count = UINT64_MAX;
    journal.engine_live_bytes = UINT64_MAX;
    journal.ready_checksum = UINT64_MAX;
    journal.state = ArtBumpJournalState::Copying;
    journal.source_zone = snapshot.active_zone;
    journal.target_zone = 1U - snapshot.active_zone;
    journal.source_epoch = snapshot.zones[journal.source_zone].epoch;
    journal.target_epoch = ArtBumpIndex::NextEpoch(
        snapshot.zones[journal.target_zone].epoch);
    journal.base_checksum = ArtBumpIndex::CompactBaseChecksum(journal);
    return journal;
}

void testCopyingRollbackIgnoresReadyFields() {
    Fixture fixture;
    putOne(&fixture, "copying", "old");
    const auto before = fixture.state.LoadSnapshotAcquire();
    const auto journal = copyingJournal(before);
    fixture.state.InstallJournal(journal);
    fixture.zones[journal.target_zone].StoreEpoch(journal.target_epoch);
    fixture.zones[journal.target_zone].StoreTopRelease(
        fixture.zones[journal.target_zone].Begin());
    const std::array<std::uint8_t, 7> garbage = {1, 2, 3, 4, 5, 6, 7};
    static_cast<void>(fixture.zones[journal.target_zone].Allocate(
        garbage.data(), garbage.size()));
    fixture.state.StoreActiveZone(journal.target_zone);
    fixture.state.StoreDerived(ArtBumpDerivedState{99, 99, 99});
    fixture.state.StoreGeneration(999);
    fixture.zones[journal.source_zone].StoreTopRelease(UINT64_MAX);
    fixture.zones[journal.source_zone].StoreEpoch(0);

    fixture.state.ClearEvents();
    fixture.Recover();
    checkRebuildCuts(
        fixture.state,
        ArtBumpApplyStep::RecoveryRollbackSlabRebuildWrite);
    auto recovered = fixture.state.LoadSnapshotAcquire();
    CHECK(recovered.journal.state == ArtBumpJournalState::Idle);
    CHECK(recovered.committed_root == before.committed_root);
    CHECK(recovered.active_zone == journal.source_zone);
    CHECK(recovered.generation == before.generation);
    CHECK(fixture.zones[journal.target_zone].TopAcquire() ==
          fixture.zones[journal.target_zone].Begin());
    CHECK(fixture.index.Get("copying") ==
          std::optional<std::vector<std::uint8_t>>(value("old")));

    // Recovery is repeatable after the active journal converges to IDLE.
    fixture.state.ClearEvents();
    fixture.Recover();
    checkRebuildCuts(
        fixture.state,
        ArtBumpApplyStep::RecoveryIdleSlabRebuildWrite);
    CHECK(fixture.index.Get("copying").has_value());
}

void testReadyRollbackNeverReadsNewTree() {
    Fixture fixture;
    putOne(&fixture, "rollback", "source");
    const auto before_bytes = fixture.bytes;
    const auto before = fixture.state.LoadSnapshotAcquire();
    std::array<std::uint32_t, 4> old_high_water{};
    for (std::size_t index = 0; index < 4; ++index) {
        old_high_water[index] = fixture.nodes.HighWater(
            static_cast<ArtBumpNodeKind>(index + 1U));
    }

    auto compact = fixture.index.PrepareCompact();
    std::move(compact).Apply();
    auto ready = fixture.state.LoadSnapshotAcquire().journal;
    ready.state = ArtBumpJournalState::Ready;
    CHECK(ready.old_root == before.committed_root);
    CHECK(ready.new_root != 0);

    // Restore only the old live prefix. This one-node fixture has no holes, so
    // the clone is in the next slot and remains independently present.
    for (std::size_t index = 0; index < 4; ++index) {
        const auto payload = static_cast<std::uint64_t>(
            ArtBumpNodeCodec::kPayloadBytes[index]);
        const auto bytes = static_cast<std::uint64_t>(old_high_water[index]) *
            payload;
        const auto offset = fixture.geometry.slabs[index].zone_offset;
        std::memcpy(
            fixture.bytes.data() + static_cast<std::size_t>(offset),
            before_bytes.data() + static_cast<std::size_t>(offset),
            static_cast<std::size_t>(bytes));
    }
    fixture.state.StoreCommittedRootRelease(ready.old_root);
    fixture.state.InstallJournal(ready);
    fixture.CorruptNode(ready.new_root);
    fixture.zones[ready.source_zone].StoreTopRelease(UINT64_MAX);
    fixture.zones[ready.source_zone].StoreEpoch(0);

    fixture.state.ClearEvents();
    fixture.Recover();
    checkRebuildCuts(
        fixture.state,
        ArtBumpApplyStep::RecoveryRollbackSlabRebuildWrite);
    CHECK(fixture.state.LoadSnapshotAcquire().journal.state ==
          ArtBumpJournalState::Idle);
    CHECK(fixture.index.Get("rollback") ==
          std::optional<std::vector<std::uint8_t>>(value("source")));
    CHECK(fixture.zones[ready.target_zone].TopAcquire() ==
          fixture.zones[ready.target_zone].Begin());
    fixture.state.ClearEvents();
    fixture.Recover();
    checkRebuildCuts(
        fixture.state,
        ArtBumpApplyStep::RecoveryIdleSlabRebuildWrite);
    CHECK(fixture.index.InspectCommitted().entry_count == 1);
}

void testReadyRollForwardNeverReadsOldTree() {
    Fixture fixture;
    putOne(&fixture, "forward", "target");
    auto compact = fixture.index.PrepareCompact();
    std::move(compact).Apply();
    auto ready = fixture.state.LoadSnapshotAcquire().journal;
    ready.state = ArtBumpJournalState::Ready;
    CHECK(ready.new_root == fixture.state.LoadSnapshotAcquire().committed_root);
    fixture.state.InstallJournal(ready);

    // Simulate death before mutable target descriptor/counter installation.
    fixture.zones[ready.target_zone].StoreTopRelease(UINT64_MAX);
    fixture.zones[ready.target_zone].StoreEpoch(0);
    fixture.state.StoreActiveZone(ready.source_zone);
    fixture.state.StoreDerived(ArtBumpDerivedState{});
    fixture.state.StoreGeneration(0);
    fixture.CorruptNode(ready.old_root);

    fixture.state.ClearEvents();
    fixture.Recover();
    checkRebuildCuts(
        fixture.state,
        ArtBumpApplyStep::RecoveryForwardSlabRebuildWrite);
    const auto recovered = fixture.state.LoadSnapshotAcquire();
    CHECK(recovered.journal.state == ArtBumpJournalState::Idle);
    CHECK(recovered.committed_root == ready.new_root);
    CHECK(recovered.active_zone == ready.target_zone);
    CHECK(recovered.generation == ready.operation_generation);
    CHECK(fixture.index.Get("forward") ==
          std::optional<std::vector<std::uint8_t>>(value("target")));
    CHECK(fixture.zones[ready.source_zone].TopAcquire() ==
          fixture.zones[ready.source_zone].Begin());
    fixture.state.ClearEvents();
    fixture.Recover();
    checkRebuildCuts(
        fixture.state,
        ArtBumpApplyStep::RecoveryIdleSlabRebuildWrite);
    CHECK(fixture.index.InspectCommitted().entry_count == 1);
}

void testRecoveryApplyDoesNotAllocateAfterFirstWrite() {
    Fixture fixture;
    putOne(&fixture, "allocation-free", "recovery");
    const auto before = fixture.state.LoadSnapshotAcquire();
    const auto journal = copyingJournal(before);
    fixture.state.InstallJournal(journal);
    fixture.state.StoreActiveZone(journal.target_zone);
    fixture.state.StoreDerived(ArtBumpDerivedState{});
    fixture.state.StoreGeneration(999);

    fixture.AttachNodesForRecovery();
    auto recovery = fixture.index.PrepareRecovery();
    fixture.state.FailAllocationsAfterNextPersistentStep();
    {
        AllocationFailureReset reset;
        std::move(recovery).Apply();
    }
    CHECK(fixture.state.LoadSnapshotAcquire().journal.state ==
          ArtBumpJournalState::Idle);
    CHECK(fixture.index.Get("allocation-free") ==
          std::optional<std::vector<std::uint8_t>>(value("recovery")));
}

void testRecoveryRejectsDuplicateRawIntervalsWithoutWrites() {
    Fixture fixture;
    putOne(&fixture, "a", "first");
    putOne(&fixture, "b", "second");
    const auto root = fixture.state.LoadSnapshotAcquire().committed_root;
    const auto root_node = fixture.nodes.Read(root);
    std::vector<std::uint64_t> children;
    for (const auto child : root_node.children) {
        if (child != 0) children.push_back(child);
    }
    CHECK(children.size() == 2);
    const auto first = fixture.nodes.Read(children[0]);
    auto second = fixture.nodes.Read(children[1]);
    CHECK(first.has_value);
    CHECK(second.has_value);
    CHECK(first.value_offset != 0);
    CHECK(second.value_offset != first.value_offset);
    second.value_offset = first.value_offset;
    fixture.RewriteNode(children[1], second);

    const auto before = fixture.bytes;
    fixture.AttachNodesForRecovery();
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(fixture.index.PrepareRecovery());
    });
    CHECK(fixture.bytes == before);
}

void testRecoveryRejectsPartiallyOverlappingRawIntervalsWithoutWrites() {
    Fixture fixture;
    putOne(&fixture, "value", "payload");
    const auto root = fixture.state.LoadSnapshotAcquire().committed_root;
    auto node = fixture.nodes.Read(root);
    CHECK(node.has_value);
    CHECK(node.value_offset != 0);
    node.prefix_offset = node.value_offset + 1U;
    node.prefix_len = 1;
    fixture.RewriteNode(root, node);

    const auto before = fixture.bytes;
    fixture.AttachNodesForRecovery();
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(fixture.index.PrepareRecovery());
    });
    CHECK(fixture.bytes == before);
}

void testRecoveryRejectsNoncanonicalTlvWithoutWrites() {
    Fixture fixture;
    putOne(&fixture, "value", "payload");
    const auto root = fixture.state.LoadSnapshotAcquire().committed_root;
    const auto node = fixture.nodes.Read(root);
    CHECK(node.has_value);
    CHECK(node.value_offset != 0);
    CHECK(node.value_offset < fixture.bytes.size());
    fixture.bytes[static_cast<std::size_t>(node.value_offset)] = 0xff;

    const auto before = fixture.bytes;
    fixture.AttachNodesForRecovery();
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(fixture.index.PrepareRecovery());
    });
    CHECK(fixture.bytes == before);
}

void testRecoveryRejectsTlvLengthPastRecordedTopWithoutWrites() {
    Fixture fixture;
    putOne(&fixture, "value", "payload");
    const auto root = fixture.state.LoadSnapshotAcquire().committed_root;
    const auto node = fixture.nodes.Read(root);
    CHECK(node.has_value);
    CHECK(node.value_offset != 0);
    const auto value_offset = static_cast<std::size_t>(node.value_offset);
    CHECK(value_offset < fixture.bytes.size());
    const auto kind_length = static_cast<std::size_t>(
        fixture.bytes[value_offset]);
    const auto raw_length_offset = value_offset + 1U + kind_length + 4U;
    CHECK(raw_length_offset + 4U <= fixture.bytes.size());
    std::fill_n(fixture.bytes.begin() +
                    static_cast<std::ptrdiff_t>(raw_length_offset),
                4,
                std::uint8_t{0xff});

    const auto before = fixture.bytes;
    fixture.AttachNodesForRecovery();
    expectAllocatorError(AllocatorErrorCode::Corrupt, [&] {
        static_cast<void>(fixture.index.PrepareRecovery());
    });
    CHECK(fixture.bytes == before);
}

} // namespace

int main() {
    return runTest([] {
        testCopyingRollbackIgnoresReadyFields();
        testReadyRollbackNeverReadsNewTree();
        testReadyRollForwardNeverReadsOldTree();
        testRecoveryApplyDoesNotAllocateAfterFirstWrite();
        testRecoveryRejectsDuplicateRawIntervalsWithoutWrites();
        testRecoveryRejectsPartiallyOverlappingRawIntervalsWithoutWrites();
        testRecoveryRejectsNoncanonicalTlvWithoutWrites();
        testRecoveryRejectsTlvLengthPastRecordedTopWithoutWrites();
    });
}
