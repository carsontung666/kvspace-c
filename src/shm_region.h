#pragma once

#include "kvspace/shm.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kvspace::detail {

enum class ArtBumpApplyStep : std::uint8_t;

enum class RegionTestEvent : std::uint32_t {
    MutationPublished = 1,
    BlobMarkedFree = 2,
    AllocatorSplitJournalPublished = 3,
    AllocatorSplitRemainderReady = 4,
    AllocatorSplitLinksReady = 5,
    AllocatorSplitBoundaryCommitted = 6,
    AllocatorSplitJournalCleared = 7,
    BackingOpenedBeforeLock = 8,
    CompactJournalPublished = 9,
    CompactTargetReset = 10,
    CompactPayloadCopied = 11,
    CompactTreeReady = 12,
    CompactBeforeRootPublish = 13,
    CompactRootPublished = 14,
    CompactCleanupStarted = 15,
    CompactSourceReset = 16,
    CompactJournalCleared = 17,
    CompactNodeCloned = 18,
    CompactActiveZoneInstalled = 19,
    CompactEmptySourceTopReset = 20,
    TrieNodeReclaimed = 21,
    TrieBoxReclaimed = 22,
    AllocatorCoalesceJournalPrepared = 23,
    AllocatorCoalesceJournalPublished = 24,
    AllocatorCoalesceBoundaryCommitted = 25,
    AllocatorCoalesceFollowingLinked = 26,
    AllocatorCoalesceTailInstalled = 27,
    AllocatorCoalesceJournalCleared = 28,
    ArtBoxNodeReclaimed = 29,
    ArtBoxObjectReclaimed = 30,
    ArtBoxBoxRebuilt = 31,
    ArtBoxNodesRebuilt = 32,
    TrieBoxNodesRebuilt = 33,
    TrieBoxBoxRebuilt = 34,
    HashBoxBoxRebuilt = 35,
    HashBoxObjectReclaimed = 36,
    ClearQueueTombstoned = 37,
    ClearHeapTopReset = 38,
    ClearHeapLastReset = 39,
    RecoveryPendingTailReset = 40,
    RecoveryPrepareStarting = 41,
    RecoveryApplyStarting = 42,
    RecoveryMadeConsistent = 43,
};

using RegionTestHook = void (*)(RegionTestEvent) noexcept;

// Test-only observation of every persistent COMMON Clear/recovery apply
// write.  The ordinal is per (phase, step) and is reset at the start of each
// apply.  This is deliberately volatile-only test plumbing: it changes no
// persistent layout or recovery protocol.
enum class CommonApplyPhase : std::uint8_t {
    Clear = 1,
    Recovery = 2,
};

enum class CommonApplyStep : std::uint8_t {
    QueueStateStored = 1,
    QueueHashStored = 2,
    QueueKeyOffsetStored = 3,
    QueueHeadOffsetStored = 4,
    QueueKeyLengthStored = 5,
    QueueCountStored = 6,
    QueueMessageReleased = 7,
    QueueKeyReleased = 8,
    BlobFlagsStored = 9,
    BlobNextFreeStored = 10,
    BlobPreviousFreeStored = 11,
    BlobPayloadSizeStored = 12,
    BlobMarkStored = 13,
    BlobMagicStored = 14,
    BlobReserved16Stored = 15,
    BlobReserved32Stored = 16,
    BlobSpanStored = 17,
    BlobPreviousSpanStored = 18,
    FreeHeadStored = 19,
    AllocatorJournalOffsetStored = 20,
    AllocatorJournalOriginalSpanStored = 21,
    AllocatorJournalRequestedSpanStored = 22,
    AllocatorJournalChecksumStored = 23,
    AllocatorJournalReserved32Stored = 24,
    AllocatorJournalReserved64Stored = 25,
    AllocatorJournalStateStored = 26,
    HeapTopStored = 27,
    HeapLastStored = 28,
    RecoveryCountStored = 29,
    GenerationStored = 30,
    Count = 31,
};

using CommonApplyTestHook = void (*)(
    CommonApplyPhase,
    CommonApplyStep,
    std::uint64_t) noexcept;
using ArtBumpRegionTestHook = void (*)(
    ArtBumpApplyStep,
    std::uint64_t) noexcept;
void SetRegionTestHook(RegionTestHook hook) noexcept;
void SetCommonApplyTestHook(CommonApplyTestHook hook) noexcept;
void SetArtBumpRegionTestHook(ArtBumpRegionTestHook hook) noexcept;
std::size_t RegionAllocatorJournalOffsetForTest() noexcept;
std::size_t RegionQueueCapacityOffsetForTest() noexcept;
std::size_t RegionQueueTableOffsetOffsetForTest() noexcept;

enum class OpenMode {
    Create,
    Attach,
    Open,
};

class Region {
public:
    struct Impl;

    class Guard {
    public:
        Guard() = default;
        explicit Guard(Region* region) : region_(region) {}
        ~Guard();
        Guard(Guard&& other) noexcept;
        Guard& operator=(Guard&& other) noexcept;
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;

    private:
        friend class Region;
        Region* region_ = nullptr;
    };

    class Mutation {
    public:
        Mutation() = default;
        ~Mutation();
        Mutation(Mutation&& other) noexcept;
        Mutation& operator=(Mutation&& other) noexcept;
        Mutation(const Mutation&) = delete;
        Mutation& operator=(const Mutation&) = delete;

        void Commit();

    private:
        friend class Region;
        explicit Mutation(Region* region) : region_(region) {}
        Region* region_ = nullptr;
    };

    using Entry = std::pair<std::string, std::vector<std::uint8_t>>;

    static std::unique_ptr<Region> Open(
        const std::string& name,
        const ShmOptions& options,
        OpenMode mode,
        std::optional<ShmEngine> expected_engine = std::nullopt);
    static void Destroy(const std::string& name);

    ~Region();
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    Guard Lock();
    Mutation BeginMutation();
    void Close();

    bool Get(std::string_view key, std::vector<std::uint8_t>* value) const;
    bool Exists(std::string_view key) const;
    void Put(std::string_view key, const std::vector<std::uint8_t>& value);
    bool Erase(std::string_view key);
    std::vector<Entry> Entries() const;
    std::vector<Entry> EntriesWithPrefix(std::string_view prefix) const;
    void Clear();
    void CompactValues();

    void Notify(std::string_view key, const std::vector<std::uint8_t>& value);
    bool Watch(
        Guard& guard,
        std::string_view key,
        std::chrono::milliseconds timeout,
        std::vector<std::uint8_t>* value,
        const std::function<bool()>& cancelled);
    void WakeAll();

    ShmStats Stats() const;

private:
    explicit Region(std::unique_ptr<Impl> impl);
    void Unlock() noexcept;
    void CommitMutation();
    void RollbackMutation() noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace kvspace::detail
