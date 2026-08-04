#include "test_support.h"

#include "kvspace/errors.h"
#include "kvspace/shm.h"
#include "kvspace/xvalue.h"
#include "shm_region.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace allocation_failure {

thread_local bool fail_next = false;

void arm() noexcept { fail_next = true; }

void* allocate(std::size_t size) {
    if (fail_next) {
        fail_next = false;
        throw std::bad_alloc();
    }
    if (void* memory = std::malloc(size == 0 ? 1 : size);
        memory != nullptr) {
        return memory;
    }
    throw std::bad_alloc();
}

} // namespace allocation_failure

void* operator new(std::size_t size) {
    return allocation_failure::allocate(size);
}

void* operator new[](std::size_t size) {
    return allocation_failure::allocate(size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace instrumentation {

enum class ChildHookMode : sig_atomic_t {
    None = 0,
    PrepareBadAlloc = 1,
    ApplyStartExit = 2,
    MidApplyExit = 3,
    BroadcastFailure = 4,
};

volatile sig_atomic_t child_hook_mode =
    static_cast<sig_atomic_t>(ChildHookMode::None);
volatile sig_atomic_t fail_next_broadcast = 0;
volatile sig_atomic_t made_consistent_seen = 0;
volatile sig_atomic_t broadcast_failure_seen = 0;
volatile sig_atomic_t recovery_prepare_events = 0;

bool track_mutex_results = false;
std::size_t owner_dead_results = 0;
std::size_t not_recoverable_results = 0;

ChildHookMode hookMode() noexcept {
    return static_cast<ChildHookMode>(child_hook_mode);
}

void resetParentObservation() noexcept {
    recovery_prepare_events = 0;
    track_mutex_results = false;
    owner_dead_results = 0;
    not_recoverable_results = 0;
}

void regionHook(kvspace::detail::RegionTestEvent event) noexcept {
    if (event ==
        kvspace::detail::RegionTestEvent::RecoveryPrepareStarting) {
        ++recovery_prepare_events;
        if (hookMode() == ChildHookMode::PrepareBadAlloc) {
            allocation_failure::arm();
        }
    }
    if (event == kvspace::detail::RegionTestEvent::RecoveryApplyStarting &&
        hookMode() == ChildHookMode::ApplyStartExit) {
        std::_Exit(126);
    }
    if (event == kvspace::detail::RegionTestEvent::HashBoxBoxRebuilt &&
        hookMode() == ChildHookMode::MidApplyExit) {
        std::_Exit(127);
    }
    if (event ==
            kvspace::detail::RegionTestEvent::RecoveryMadeConsistent &&
        hookMode() == ChildHookMode::BroadcastFailure) {
        made_consistent_seen = 1;
        fail_next_broadcast = 1;
    }
}

} // namespace instrumentation

extern "C" int __real_pthread_mutex_lock(pthread_mutex_t* mutex);
extern "C" int __wrap_pthread_mutex_lock(pthread_mutex_t* mutex) {
    const int result = __real_pthread_mutex_lock(mutex);
    if (instrumentation::track_mutex_results) {
        if (result == EOWNERDEAD) ++instrumentation::owner_dead_results;
        if (result == ENOTRECOVERABLE) {
            ++instrumentation::not_recoverable_results;
        }
    }
    return result;
}

extern "C" int __real_pthread_cond_broadcast(pthread_cond_t* condition);
extern "C" int __wrap_pthread_cond_broadcast(pthread_cond_t* condition) {
    if (instrumentation::fail_next_broadcast != 0) {
        instrumentation::fail_next_broadcast = 0;
        instrumentation::broadcast_failure_seen = 1;
        return EIO;
    }
    return __real_pthread_cond_broadcast(condition);
}

namespace {

using namespace std::chrono_literals;

constexpr std::uint64_t kQueueCapacityOffset = 144;
constexpr std::uint64_t kQueueTableOffset = 152;
constexpr std::uint64_t kTableOffsetsOffset = 88;
constexpr std::uint64_t kHeapOffsetOffset = 168;
constexpr std::uint64_t kHeapTopOffset = 176;
constexpr std::uint64_t kAllocatorJournalOffset = 224;
constexpr std::uint64_t kFreeHeadsOffset = 288;
constexpr std::uint64_t kMutexOffset = 1368;
constexpr std::uint64_t kMutexBytes = 40;
constexpr std::uint64_t kVersionOffset = 8;
constexpr std::uint64_t kEngineIdOffset = 24;
constexpr std::uint64_t kActiveTableOffset = 1360;
constexpr std::uint64_t kAllocatorJournalSalt = 0x4b5653504c495431ULL;
constexpr std::uint32_t kAllocatorJournalSplit = 1;
constexpr std::uint32_t kBlobMagic = 0x4b56424cU;

kvspace::ShmOptions hashOptions() {
    kvspace::ShmOptions options;
    options.initial_size = 8ULL * 1024U * 1024U;
    options.max_size = options.initial_size;
    options.max_entries = 64;
    options.max_queues = 8;
    options.engine = kvspace::ShmEngine::HashBox;
    return options;
}

std::unique_ptr<kvspace::ShmClient> createFixture(
    const std::string& name) {
    auto store = kvspace::ShmClient::Create(name, hashOptions());
    store->Set("/kept", kvspace::XValue::Str("committed"));
    store->Notify("/queue", kvspace::XValue::Int64(7));
    return store;
}

std::uint64_t readLittleEndianAt(
    int fd,
    std::uint64_t offset,
    std::size_t width) {
    CHECK(width == 1 || width == 2 || width == 4 || width == 8);
    std::array<std::uint8_t, 8> bytes{};
    CHECK(::pread(
              fd,
              bytes.data(),
              width,
              static_cast<off_t>(offset)) == static_cast<ssize_t>(width));
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

void writeLittleEndianAt(
    int fd,
    std::uint64_t offset,
    std::uint64_t value,
    std::size_t width) {
    CHECK(width == 1 || width == 2 || width == 4 || width == 8);
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t index = 0; index < width; ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
    CHECK(::pwrite(
              fd,
              bytes.data(),
              width,
              static_cast<off_t>(offset)) == static_cast<ssize_t>(width));
}

void writeByteAt(int fd, std::uint64_t offset, std::uint8_t byte) {
    CHECK(::pwrite(fd, &byte, 1, static_cast<off_t>(offset)) == 1);
}

std::vector<std::uint8_t> readRegionIgnoringMutex(
    const std::string& name) {
    const int fd = ::shm_open(name.c_str(), O_RDONLY | O_CLOEXEC, 0);
    CHECK(fd >= 0);
    struct stat status {};
    CHECK(::fstat(fd, &status) == 0);
    CHECK(status.st_size >= 0);
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(status.st_size));
    if (!bytes.empty()) {
        CHECK(::pread(fd, bytes.data(), bytes.size(), 0) ==
              static_cast<ssize_t>(bytes.size()));
    }
    CHECK(::close(fd) == 0);
    CHECK(kMutexOffset <= bytes.size());
    CHECK(kMutexBytes <= bytes.size() -
          static_cast<std::size_t>(kMutexOffset));
    std::fill(
        bytes.begin() + static_cast<std::ptrdiff_t>(kMutexOffset),
        bytes.begin() + static_cast<std::ptrdiff_t>(
            kMutexOffset + kMutexBytes),
        0);
    return bytes;
}

void corruptQueueKeyBlobFlags(const std::string& name) {
    const int fd = ::shm_open(name.c_str(), O_RDWR | O_CLOEXEC, 0);
    CHECK(fd >= 0);
    const auto capacity = readLittleEndianAt(fd, kQueueCapacityOffset, 8);
    const auto table = readLittleEndianAt(fd, kQueueTableOffset, 8);
    std::uint64_t key_offset = 0;
    for (std::uint64_t index = 0; index < capacity; ++index) {
        const auto slot = table + index * 32U;
        if (readLittleEndianAt(fd, slot + 28U, 4) != 1U) continue;
        key_offset = readLittleEndianAt(fd, slot + 8U, 8);
        if (key_offset != 0) break;
    }
    CHECK(key_offset != 0);
    // COMMON Blob flags accept only 0/1. This is reached by recovery
    // preflight, after EOWNERDEAD, but requires no repair to detect.
    writeByteAt(fd, key_offset + 40U, 3);
    CHECK(::close(fd) == 0);
}

std::size_t floorLog2ForFixture(std::uint64_t value) {
    CHECK(value != 0);
    std::size_t result = 0;
    while (value > 1) {
        value >>= 1U;
        ++result;
    }
    return result;
}

void installOverflowingSplitJournal(const std::string& name) {
    const int fd = ::shm_open(name.c_str(), O_RDWR | O_CLOEXEC, 0);
    CHECK(fd >= 0);

    // Reproduce the exact persistent image after removeFree(source) and after
    // publication of an allocator split journal.  All values are decoded and
    // encoded independently in little endian; no production struct is used.
    const auto source = readLittleEndianAt(fd, kHeapOffsetOffset, 8);
    const auto original_span = readLittleEndianAt(fd, source, 8);
    CHECK(source != 0);
    CHECK(original_span >= 64);
    CHECK(original_span % 16U == 0);
    CHECK(readLittleEndianAt(fd, source + 8U, 8) == 0);
    CHECK(readLittleEndianAt(fd, source + 32U, 4) == 0);
    CHECK(readLittleEndianAt(fd, source + 36U, 4) == kBlobMagic);
    CHECK(readLittleEndianAt(fd, source + 40U, 1) == 0);
    CHECK(readLittleEndianAt(fd, source + 41U, 1) == 0);
    CHECK(readLittleEndianAt(fd, source + 42U, 2) == 0);
    CHECK(readLittleEndianAt(fd, source + 44U, 4) == 0);

    const auto previous_free = readLittleEndianAt(fd, source + 24U, 8);
    const auto next_free = readLittleEndianAt(fd, source + 16U, 8);
    CHECK(previous_free == 0);
    const auto bucket = floorLog2ForFixture(original_span);
    CHECK(bucket < 64);
    const auto free_head = kFreeHeadsOffset + bucket * 8U;
    CHECK(readLittleEndianAt(fd, free_head, 8) == source);
    writeLittleEndianAt(fd, free_head, next_free, 8);
    if (next_free != 0) {
        CHECK(readLittleEndianAt(fd, next_free + 24U, 8) == source);
        writeLittleEndianAt(fd, next_free + 24U, 0, 8);
    }
    writeLittleEndianAt(fd, source + 16U, 0, 8);
    writeLittleEndianAt(fd, source + 24U, 0, 8);

    const auto requested_span = std::numeric_limits<std::uint64_t>::max();
    const auto checksum = kAllocatorJournalSalt ^ source ^ original_span ^
        requested_span;
    CHECK(readLittleEndianAt(
        fd, kAllocatorJournalOffset + 32U, 4) == 0);
    writeLittleEndianAt(fd, kAllocatorJournalOffset, source, 8);
    writeLittleEndianAt(
        fd, kAllocatorJournalOffset + 8U, original_span, 8);
    writeLittleEndianAt(
        fd, kAllocatorJournalOffset + 16U, requested_span, 8);
    writeLittleEndianAt(fd, kAllocatorJournalOffset + 24U, checksum, 8);
    writeLittleEndianAt(fd, kAllocatorJournalOffset + 36U, 0, 4);
    for (std::uint64_t index = 0; index < 3; ++index) {
        writeLittleEndianAt(
            fd,
            kAllocatorJournalOffset + 40U + index * 8U,
            0,
            8);
    }
    // State is the sole publication and is deliberately written last.
    writeLittleEndianAt(
        fd,
        kAllocatorJournalOffset + 32U,
        kAllocatorJournalSplit,
        4);
    CHECK(::close(fd) == 0);
}

void installOversizedUnreferencedPayload(const std::string& name) {
    const int fd = ::shm_open(name.c_str(), O_RDWR | O_CLOEXEC, 0);
    CHECK(fd >= 0);
    const auto heap_begin = readLittleEndianAt(fd, kHeapOffsetOffset, 8);
    const auto heap_top = readLittleEndianAt(fd, kHeapTopOffset, 8);
    CHECK(heap_begin < heap_top);

    bool patched = false;
    for (auto offset = heap_begin; offset < heap_top;) {
        const auto span = readLittleEndianAt(fd, offset, 8);
        CHECK(span >= 64);
        CHECK(span % 16U == 0);
        CHECK(span <= heap_top - offset);
        const auto flags = readLittleEndianAt(fd, offset + 40U, 1);
        if (flags == 0) {
            writeLittleEndianAt(
                fd,
                offset + 32U,
                std::numeric_limits<std::uint32_t>::max(),
                4);
            patched = true;
            break;
        }
        offset += span;
    }
    CHECK(patched);
    CHECK(::close(fd) == 0);
}

void installOutOfMappingActiveTableOffset(const std::string& name) {
    const int fd = ::shm_open(name.c_str(), O_RDWR | O_CLOEXEC, 0);
    CHECK(fd >= 0);
    struct stat status {};
    CHECK(::fstat(fd, &status) == 0);
    CHECK(status.st_size > 0);
    const auto active = readLittleEndianAt(fd, kActiveTableOffset, 4);
    CHECK(active <= 1);
    const auto outside_mapping =
        static_cast<std::uint64_t>(status.st_size) + 4096U;
    writeLittleEndianAt(
        fd,
        kTableOffsetsOffset + active * sizeof(std::uint64_t),
        outside_mapping,
        sizeof(std::uint64_t));
    CHECK(::close(fd) == 0);
}

pid_t spawnChild(const std::string& name, const char* mode) {
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        ::execl(
            "/proc/self/exe",
            "test_owner_death_protocol",
            "--child",
            mode,
            name.c_str(),
            static_cast<char*>(nullptr));
        ::_exit(120);
    }
    return child;
}

void requireExitCode(pid_t child, int expected) {
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == expected);
}

pid_t spawnInheritedSuccessfulGet(kvspace::ShmClient* store) {
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        try {
            const auto value = store->Get("/kept");
            std::_Exit(value.AsStr() == "committed" ? 0 : 97);
        } catch (...) {
            std::_Exit(98);
        }
    }
    return child;
}

pid_t spawnInheritedCorruptionCheck(kvspace::ShmClient* store) {
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        try {
            static_cast<void>(store->Get("/kept"));
            std::_Exit(97);
        } catch (const kvspace::ErrCorruptRegion&) {
            std::_Exit(0);
        } catch (...) {
            std::_Exit(98);
        }
    }
    return child;
}

enum class PermanentFormatError {
    Version,
    UnsupportedEngine,
};

pid_t spawnInheritedFormatCheck(
    kvspace::ShmClient* store,
    PermanentFormatError expected) {
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        try {
            static_cast<void>(store->Get("/kept"));
            std::_Exit(97);
        } catch (const kvspace::ErrVersionMismatch&) {
            std::_Exit(expected == PermanentFormatError::Version ? 0 : 96);
        } catch (const kvspace::ErrUnsupportedEngine&) {
            std::_Exit(
                expected == PermanentFormatError::UnsupportedEngine ? 0 : 96);
        } catch (...) {
            std::_Exit(98);
        }
    }
    return child;
}

int childMain(const std::string& mode, const std::string& name) {
    try {
        if (mode == "lock-and-die") {
            auto region = kvspace::detail::Region::Open(
                name,
                {},
                kvspace::detail::OpenMode::Attach,
                kvspace::ShmEngine::HashBox);
            auto guard = region->Lock();
            (void)guard;
            std::_Exit(73);
        }

        instrumentation::made_consistent_seen = 0;
        instrumentation::broadcast_failure_seen = 0;
        instrumentation::recovery_prepare_events = 0;
        if (mode == "prepare-bad-alloc") {
            instrumentation::child_hook_mode = static_cast<sig_atomic_t>(
                instrumentation::ChildHookMode::PrepareBadAlloc);
        } else if (mode == "apply-start-exit") {
            instrumentation::child_hook_mode = static_cast<sig_atomic_t>(
                instrumentation::ChildHookMode::ApplyStartExit);
        } else if (mode == "mid-apply-exit") {
            instrumentation::child_hook_mode = static_cast<sig_atomic_t>(
                instrumentation::ChildHookMode::MidApplyExit);
        } else if (mode == "broadcast-failure") {
            instrumentation::child_hook_mode = static_cast<sig_atomic_t>(
                instrumentation::ChildHookMode::BroadcastFailure);
        } else {
            return 90;
        }
        kvspace::detail::SetRegionTestHook(instrumentation::regionHook);

        try {
            auto attached = kvspace::ShmClient::Attach(
                name, kvspace::ShmEngine::HashBox);
            attached->Close();
            return 91;
        } catch (const kvspace::ErrCorruptRegion&) {
            return mode == "broadcast-failure" ? 92 : 93;
        } catch (const kvspace::Error& error) {
            if (mode != "broadcast-failure") return 94;
            const bool expected =
                instrumentation::recovery_prepare_events == 1 &&
                instrumentation::made_consistent_seen != 0 &&
                instrumentation::broadcast_failure_seen != 0 &&
                std::string(error.what()).find("broadcast") !=
                    std::string::npos;
            return expected ? 0 : 95;
        }
    } catch (...) {
        return 96;
    }
}

void triggerOwnerDeath(const std::string& name) {
    requireExitCode(spawnChild(name, "lock-and-die"), 73);
}

void beginParentObservation() {
    instrumentation::resetParentObservation();
    instrumentation::child_hook_mode = static_cast<sig_atomic_t>(
        instrumentation::ChildHookMode::None);
    kvspace::detail::SetRegionTestHook(instrumentation::regionHook);
    instrumentation::track_mutex_results = true;
}

void endParentObservation() {
    instrumentation::track_mutex_results = false;
    kvspace::detail::SetRegionTestHook(nullptr);
}

void testPrepareCorruptionPoisonsMutexWithoutApplicationWrites() {
    ScopedRegion scoped(uniqueShmName("prepare-corrupt-poison"));
    auto store = createFixture(scoped.name());
    triggerOwnerDeath(scoped.name());
    corruptQueueKeyBlobFlags(scoped.name());
    const auto before = readRegionIgnoringMutex(scoped.name());

    beginParentObservation();
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(store->Get("/kept"));
    });
    CHECK(instrumentation::owner_dead_results == 1);
    CHECK(instrumentation::not_recoverable_results == 0);
    CHECK(instrumentation::recovery_prepare_events == 1);
    CHECK(readRegionIgnoringMutex(scoped.name()) == before);

    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(store->Get("/kept"));
    });
    CHECK(instrumentation::owner_dead_results == 1);
    CHECK(instrumentation::not_recoverable_results == 1);
    CHECK(instrumentation::recovery_prepare_events == 1);
    endParentObservation();
    CHECK(readRegionIgnoringMutex(scoped.name()) == before);
}

void testOverflowingSplitJournalPoisonsWithoutRetryLoop() {
    ScopedRegion scoped(uniqueShmName("split-overflow-poison"));
    auto store = createFixture(scoped.name());
    CHECK(store->Watch("/queue", 5ms).AsInt64() == 7);
    CHECK(store->Stats().queues == 0);
    CHECK(store->Stats().heap_free > 0);
    triggerOwnerDeath(scoped.name());

    CHECK(kvspace::detail::RegionAllocatorJournalOffsetForTest() ==
          kAllocatorJournalOffset);
    installOverflowingSplitJournal(scoped.name());
    const auto before = readRegionIgnoringMutex(scoped.name());

    beginParentObservation();
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(store->Get("/kept"));
    });
    CHECK(instrumentation::owner_dead_results == 1);
    CHECK(instrumentation::not_recoverable_results == 0);
    CHECK(instrumentation::recovery_prepare_events == 1);
    CHECK(readRegionIgnoringMutex(scoped.name()) == before);

    // The corrupt recovery preflight must have poisoned the robust mutex.
    // A second lock maps ENOTRECOVERABLE directly to ErrCorruptRegion: it
    // neither re-enters PrepareRecovery nor takes the fail-stop exit 125 path.
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(store->Get("/kept"));
    });
    CHECK(instrumentation::owner_dead_results == 1);
    CHECK(instrumentation::not_recoverable_results == 1);
    CHECK(instrumentation::recovery_prepare_events == 1);
    endParentObservation();
    CHECK(readRegionIgnoringMutex(scoped.name()) == before);
}

void testOversizedUnreferencedPayloadIsCanonicalized() {
    ScopedRegion scoped(uniqueShmName("unreferenced-payload"));
    auto store = createFixture(scoped.name());
    CHECK(store->Watch("/queue", 5ms).AsInt64() == 7);
    CHECK(store->Stats().queues == 0);
    triggerOwnerDeath(scoped.name());
    installOversizedUnreferencedPayload(scoped.name());

    requireExitCode(spawnInheritedSuccessfulGet(store.get()), 0);
    store.reset();
    auto recovered = kvspace::ShmClient::Attach(
        scoped.name(), kvspace::ShmEngine::HashBox);
    CHECK(recovered->Get("/kept").AsStr() == "committed");
    CHECK(recovered->Stats().recoveries == 1);
    CHECK(recovered->Stats().heap_free > 0);
}

void verifyPermanentPrepareFormatErrorPoisons(
    std::string_view label,
    std::uint64_t field_offset,
    std::uint32_t replacement,
    PermanentFormatError expected) {
    ScopedRegion scoped(uniqueShmName(label));
    auto store = createFixture(scoped.name());
    triggerOwnerDeath(scoped.name());
    const int fd = ::shm_open(
        scoped.name().c_str(), O_RDWR | O_CLOEXEC, 0);
    CHECK(fd >= 0);
    writeLittleEndianAt(fd, field_offset, replacement, 4);
    CHECK(::close(fd) == 0);
    const auto before = readRegionIgnoringMutex(scoped.name());

    requireExitCode(spawnInheritedFormatCheck(store.get(), expected), 0);
    CHECK(readRegionIgnoringMutex(scoped.name()) == before);
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(store->Get("/kept"));
    });
    CHECK(readRegionIgnoringMutex(scoped.name()) == before);
}

void testPermanentPrepareFormatErrorsPoisonWithoutRetryLoop() {
    verifyPermanentPrepareFormatErrorPoisons(
        "prepare-version-poison",
        kVersionOffset,
        0,
        PermanentFormatError::Version);
    verifyPermanentPrepareFormatErrorPoisons(
        "prepare-engine-poison",
        kEngineIdOffset,
        0,
        PermanentFormatError::UnsupportedEngine);
}

void testCommonGeometryPreflightPrecedesEngineAccess() {
    ScopedRegion scoped(uniqueShmName("prepare-geometry-first"));
    auto store = createFixture(scoped.name());
    triggerOwnerDeath(scoped.name());
    installOutOfMappingActiveTableOffset(scoped.name());
    const auto before = readRegionIgnoringMutex(scoped.name());

    // This client was attached before the persistent offset was corrupted.
    // Recovery must validate COMMON geometry before HashBox follows that
    // offset, then poison EOWNERDEAD without touching application bytes.
    requireExitCode(spawnInheritedCorruptionCheck(store.get()), 0);
    CHECK(readRegionIgnoringMutex(scoped.name()) == before);
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(store->Get("/kept"));
    });
    CHECK(readRegionIgnoringMutex(scoped.name()) == before);
}

void testPrepareBadAllocFailsStopAndNextOwnerRetries() {
    ScopedRegion scoped(uniqueShmName("prepare-bad-alloc"));
    auto owner = createFixture(scoped.name());
    owner.reset();
    triggerOwnerDeath(scoped.name());
    const auto before = readRegionIgnoringMutex(scoped.name());

    // The hook arms global operator new immediately before PrepareRecovery.
    // A leaked bad_alloc would produce a child error code; protocol fail-stop
    // is the exact process exit 125.
    requireExitCode(spawnChild(scoped.name(), "prepare-bad-alloc"), 125);
    CHECK(readRegionIgnoringMutex(scoped.name()) == before);

    beginParentObservation();
    auto recovered = kvspace::ShmClient::Attach(
        scoped.name(), kvspace::ShmEngine::HashBox);
    endParentObservation();
    CHECK(instrumentation::owner_dead_results == 1);
    CHECK(instrumentation::not_recoverable_results == 0);
    CHECK(instrumentation::recovery_prepare_events == 1);
    CHECK(recovered->Get("/kept").AsStr() == "committed");
    CHECK(recovered->Stats().recoveries == 1);
    recovered->Set("/after-retry", kvspace::XValue::Int64(11));
    CHECK(recovered->Get("/after-retry").AsInt64() == 11);
}

void verifyApplyFailureRetry(
    const char* label,
    const char* mode,
    int expected_exit,
    bool must_be_byte_stable) {
    ScopedRegion scoped(uniqueShmName(label));
    auto owner = createFixture(scoped.name());
    owner.reset();
    triggerOwnerDeath(scoped.name());
    const auto before = readRegionIgnoringMutex(scoped.name());

    requireExitCode(spawnChild(scoped.name(), mode), expected_exit);
    if (must_be_byte_stable) {
        CHECK(readRegionIgnoringMutex(scoped.name()) == before);
    }

    beginParentObservation();
    auto recovered = kvspace::ShmClient::Attach(
        scoped.name(), kvspace::ShmEngine::HashBox);
    endParentObservation();
    CHECK(instrumentation::owner_dead_results == 1);
    CHECK(instrumentation::not_recoverable_results == 0);
    CHECK(instrumentation::recovery_prepare_events == 1);
    CHECK(recovered->Get("/kept").AsStr() == "committed");
    CHECK(recovered->Watch("/queue", 5ms).AsInt64() == 7);
    CHECK(recovered->Stats().recoveries == 1);
    recovered->Set("/after-apply-retry", kvspace::XValue::Int64(12));
    CHECK(recovered->Get("/after-apply-retry").AsInt64() == 12);
}

void testApplyFailuresLeaveRetryableOwnerDeath() {
    verifyApplyFailureRetry(
        "apply-start-exit", "apply-start-exit", 126, true);
    // HashBoxBoxRebuilt is emitted after the Box rebuild has written but
    // before common recovery/counters complete, exercising idempotent replay
    // from a genuinely partial Apply.
    verifyApplyFailureRetry(
        "mid-apply-exit", "mid-apply-exit", 127, false);
}

void testBroadcastFailureAfterConsistentUnlocksWithoutNewRecovery() {
    ScopedRegion scoped(uniqueShmName("consistent-broadcast-failure"));
    auto owner = createFixture(scoped.name());
    owner.reset();
    triggerOwnerDeath(scoped.name());

    requireExitCode(spawnChild(scoped.name(), "broadcast-failure"), 0);

    beginParentObservation();
    auto attached = kvspace::ShmClient::Attach(
        scoped.name(), kvspace::ShmEngine::HashBox);
    endParentObservation();
    CHECK(instrumentation::owner_dead_results == 0);
    CHECK(instrumentation::not_recoverable_results == 0);
    CHECK(instrumentation::recovery_prepare_events == 0);
    CHECK(attached->Stats().recoveries == 1);
    CHECK(attached->Get("/kept").AsStr() == "committed");
    attached->Set("/after-broadcast", kvspace::XValue::Int64(13));
    CHECK(attached->Get("/after-broadcast").AsInt64() == 13);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--child") == 0) {
        return childMain(argv[2], argv[3]);
    }
    return runTest([] {
        testPrepareCorruptionPoisonsMutexWithoutApplicationWrites();
        testOverflowingSplitJournalPoisonsWithoutRetryLoop();
        testOversizedUnreferencedPayloadIsCanonicalized();
        testPermanentPrepareFormatErrorsPoisonWithoutRetryLoop();
        testCommonGeometryPreflightPrecedesEngineAccess();
        testPrepareBadAllocFailsStopAndNextOwnerRetries();
        testApplyFailuresLeaveRetryableOwnerDeath();
        testBroadcastFailureAfterConsistentUnlocksWithoutNewRecovery();
    });
}
