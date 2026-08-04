#pragma once

#include "shm_art_bump_store.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kvspace::detail {

struct ArtBumpDerivedState {
    std::uint64_t node_count = 0;
    std::uint64_t entry_count = 0;
    std::uint64_t engine_live_bytes = 0;
};

struct ArtBumpStateSnapshot {
    std::uint64_t committed_root = 0;
    std::uint64_t generation = 0;
    std::uint64_t entry_limit = 0;
    std::uint32_t active_zone = 0;
    std::array<ArtBumpRawZoneDescriptor, 2> zones{};
    ArtBumpDerivedState derived{};
    std::uint64_t common_root_offset = 0;
    std::uint64_t tombstone_count = 0;
    ArtBumpCompactJournal journal{};
};

// Allocation-free fault-test/publication boundary notification. Except for
// CompactBeforeJournalPayload, notifications follow a complete logical
// persistent write. `ordinal` is the raw/node copy index or zone index where
// applicable. PreparedRebuild also exposes first/middle/final per-slab writes.
enum class ArtBumpApplyStep : std::uint8_t {
    MutationRawTopPublished,
    MutationNodeWritten,
    MutationAbortTopRestored,
    MutationCountersStored,
    MutationGenerationStored,
    MutationRootPublished,
    MutationNodeReclaimed,
    CompactCopyingPublished,
    CompactTargetEpochStored,
    CompactTargetTopStored,
    CompactRawCopied,
    CompactNodeCloned,
    CompactReadyPublished,
    CompactActiveStored,
    CompactCountersStored,
    CompactGenerationStored,
    CompactRootPublished,
    CompactSlabsRebuilt,
    CompactSourceEpochStored,
    CompactSourceTopStored,
    CompactIdlePublished,
    RecoveryIdleZoneEpochStored,
    RecoveryIdleZoneTopStored,
    RecoveryIdleActiveStored,
    RecoveryIdleSlabsRebuilt,
    RecoveryIdleCountersStored,
    RecoveryRollbackSourceEpochStored,
    RecoveryRollbackSourceTopStored,
    RecoveryRollbackTargetEpochStored,
    RecoveryRollbackTargetTopStored,
    RecoveryRollbackActiveStored,
    RecoveryRollbackCountersStored,
    RecoveryRollbackGenerationStored,
    RecoveryRollbackRootStored,
    RecoveryRollbackSlabsRebuilt,
    RecoveryRollbackIdlePublished,
    RecoveryForwardTargetEpochStored,
    RecoveryForwardTargetTopStored,
    RecoveryForwardActiveStored,
    RecoveryForwardCountersStored,
    RecoveryForwardGenerationStored,
    RecoveryForwardRootStored,
    RecoveryForwardSlabsRebuilt,
    RecoveryForwardSourceEpochStored,
    RecoveryForwardSourceTopStored,
    RecoveryForwardIdlePublished,
    ClearCountersStored,
    ClearRootPublished,
    ClearSlabsRebuilt,
    ClearZoneReset,
    ClearActiveStored,
    CompactBeforeJournalPayload,
    CompactReadyFieldStored,
    CompactSlabRebuildWrite,
    RecoveryIdleSlabRebuildWrite,
    RecoveryRollbackSlabRebuildWrite,
    RecoveryForwardSlabRebuildWrite,
    ClearSlabRebuildWrite,
};

// Narrow persistence port used by the storage-neutral ART algorithm. The port
// performs only field codecs, checked address translation, memcpy, and the
// requested acquire/release publication. It must not make tree, journal-branch,
// or recovery decisions.
class ArtBumpIndexState {
public:
    virtual ~ArtBumpIndexState() = default;

    // Loads journal.state and committed_root with acquire semantics. Callers
    // hold the enclosing robust mutex, so all other fields are stable. Owner-
    // death adapters must use HeaderView::DecodeForRecovery and raw zones from
    // ArtBumpRawZone::AttachForRecovery; active-journal mutable tops/epochs are
    // deliberately not authority.
    virtual ArtBumpStateSnapshot LoadSnapshotAcquire() const = 0;

    // Compact payload writers do not publish state. PublishJournalStateRelease
    // is the only journal authority publication.
    virtual void WriteJournalCopyingPayload(
        const ArtBumpCompactJournal& journal) noexcept = 0;
    virtual void WriteJournalReadyField(
        const ArtBumpCompactJournal& journal,
        ArtBumpReadyField field) noexcept = 0;
    virtual void PublishJournalStateRelease(
        ArtBumpJournalState state) noexcept = 0;
    virtual void StoreActiveZone(std::uint32_t zone) noexcept = 0;
    // The adapter also stores common root_offset=0 and tombstone_count=0.
    virtual void StoreDerived(const ArtBumpDerivedState& derived) noexcept = 0;
    virtual void StoreGeneration(std::uint64_t generation) noexcept = 0;
    virtual void StoreCommittedRootRelease(std::uint64_t root) noexcept = 0;

    // Production adapters normally inherit this no-op. Process fault tests
    // override it with a nonallocating `_exit`/event hook.
    virtual void AfterPersistentStep(
        ArtBumpApplyStep,
        std::uint64_t) noexcept {}
};

class ArtBumpIndex final {
public:
    using NodeRef = std::uint64_t;

    struct Entry {
        std::string key;
        // Empty bytes are the canonical stored-None value. Missing is
        // represented by std::nullopt from Get().
        std::vector<std::uint8_t> value;
    };

    struct GraphSummary {
        NodeRef root = 0;
        std::uint64_t node_count = 0;
        std::uint64_t entry_count = 0;
        std::uint64_t engine_live_bytes = 0;
        std::uint64_t recovered_top = 0;
        std::array<std::uint64_t, 4> slab_live_counts{};
    };

    class Mutation;
    class PreparedCommit;
    class PreparedCompact;
    class PreparedRecovery;
    class PreparedClear;

    ArtBumpIndex(
        ArtBumpNodeStore& nodes,
        std::array<ArtBumpRawZone*, 2> raw_zones,
        ArtBumpIndexState& state);
    ~ArtBumpIndex();
    ArtBumpIndex(const ArtBumpIndex&) = delete;
    ArtBumpIndex& operator=(const ArtBumpIndex&) = delete;

    GraphSummary InspectCommitted() const;
    std::optional<std::vector<std::uint8_t>> Get(
        std::string_view key) const;
    bool Exists(std::string_view key) const;
    std::vector<Entry> Entries() const;
    std::vector<Entry> EntriesWithPrefix(std::string_view prefix) const;

    // BeginMutation is a complete read-only preflight. The returned object is
    // the only place a staged root and per-operation graph scratch are kept.
    Mutation BeginMutation();

    // Every Prepare method performs all validation and volatile allocation.
    // Its Apply method performs persistent writes without allocating.
    PreparedCompact PrepareCompact();
    PreparedRecovery PrepareRecovery();
    PreparedClear PrepareClear();

    static std::uint64_t CompactBaseChecksum(
        const ArtBumpCompactJournal& journal) noexcept;
    static std::uint64_t CompactReadyChecksum(
        const ArtBumpCompactJournal& journal) noexcept;
    static std::uint32_t NextEpoch(std::uint32_t epoch) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ArtBumpIndex::Mutation final {
public:
    Mutation(const Mutation&) = delete;
    Mutation& operator=(const Mutation&) = delete;
    Mutation(Mutation&& other) noexcept;
    Mutation& operator=(Mutation&& other) noexcept;
    ~Mutation() noexcept;

    std::optional<std::vector<std::uint8_t>> Get(
        std::string_view key) const;
    std::vector<Entry> Entries() const;
    std::vector<Entry> EntriesWithPrefix(std::string_view prefix) const;

    // value must be one complete canonical TLV frame; an empty vector stores
    // None. Return false only for a byte-identical no-op Put.
    bool Put(
        std::string_view key,
        const std::vector<std::uint8_t>& value);
    bool Erase(std::string_view key);

    PreparedCommit PrepareCommit() &&;
    void Abort() noexcept;
    // Owner-death recovery rebuilds all persistent staged state from the
    // committed root. This only detaches the process-local mutation so its
    // destructor cannot perform an unplanned persistent rollback first.
    void AbandonForRecovery() noexcept;

    NodeRef StagedRoot() const noexcept;
    std::uint64_t StagedEntryCount() const noexcept;

private:
    friend class ArtBumpIndex;
    friend class PreparedCommit;
    struct Impl;
    explicit Mutation(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

class ArtBumpIndex::PreparedCommit final {
public:
    PreparedCommit(const PreparedCommit&) = delete;
    PreparedCommit& operator=(const PreparedCommit&) = delete;
    PreparedCommit(PreparedCommit&& other) noexcept;
    PreparedCommit& operator=(PreparedCommit&& other) noexcept;
    ~PreparedCommit() noexcept;

    void Apply() &&;

private:
    friend class Mutation;
    struct Impl;
    explicit PreparedCommit(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

class ArtBumpIndex::PreparedCompact final {
public:
    PreparedCompact(const PreparedCompact&) = delete;
    PreparedCompact& operator=(const PreparedCompact&) = delete;
    PreparedCompact(PreparedCompact&& other) noexcept;
    PreparedCompact& operator=(PreparedCompact&& other) noexcept;
    ~PreparedCompact();

    void Apply() &&;

private:
    friend class ArtBumpIndex;
    struct Impl;
    explicit PreparedCompact(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

class ArtBumpIndex::PreparedRecovery final {
public:
    PreparedRecovery(const PreparedRecovery&) = delete;
    PreparedRecovery& operator=(const PreparedRecovery&) = delete;
    PreparedRecovery(PreparedRecovery&& other) noexcept;
    PreparedRecovery& operator=(PreparedRecovery&& other) noexcept;
    ~PreparedRecovery();

    void Apply() &&;

private:
    friend class ArtBumpIndex;
    struct Impl;
    explicit PreparedRecovery(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

class ArtBumpIndex::PreparedClear final {
public:
    PreparedClear(const PreparedClear&) = delete;
    PreparedClear& operator=(const PreparedClear&) = delete;
    PreparedClear(PreparedClear&& other) noexcept;
    PreparedClear& operator=(PreparedClear&& other) noexcept;
    ~PreparedClear();

    // The common queue phase must have completed before this engine phase.
    void Apply() &&;

private:
    friend class ArtBumpIndex;
    struct Impl;
    explicit PreparedClear(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace kvspace::detail
