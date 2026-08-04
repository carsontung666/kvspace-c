#include "test_support.h"

#include "kvspace/errors.h"
#include "kvspace/shm.h"
#include "kvspace/xvalue.h"
#include "shm_art_bump_index.h"
#include "shm_region.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <future>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <poll.h>
#include <pthread.h>
#include <string>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
volatile sig_atomic_t die_on_cond_broadcast = 0;
volatile sig_atomic_t die_on_ftruncate = 0;
volatile sig_atomic_t mutation_was_published = 0;
volatile sig_atomic_t die_during_post_publish_gc = 0;
volatile sig_atomic_t allocator_event_to_die_at = 0;
volatile sig_atomic_t art_bump_step_to_die_at = -1;
volatile sig_atomic_t art_bump_ordinal_to_die_at = -1;
volatile sig_atomic_t backing_opened_notify_fd = -1;
volatile sig_atomic_t common_apply_phase_to_die_at = -1;
volatile sig_atomic_t common_apply_step_to_die_at = -1;
std::uint64_t common_apply_ordinal_to_die_at =
    std::numeric_limits<std::uint64_t>::max();
volatile sig_atomic_t common_apply_trace_fd = -1;
std::atomic<bool> terminate_worker_on_mutation_publish{false};

struct CommonApplyTraceRecord {
    std::uint64_t phase = 0;
    std::uint64_t step = 0;
    std::uint64_t ordinal = 0;
};

static_assert(sizeof(CommonApplyTraceRecord) == 24);

void regionTestHook(kvspace::detail::RegionTestEvent event) noexcept {
    if (event == kvspace::detail::RegionTestEvent::MutationPublished) {
        mutation_was_published = 1;
        if (terminate_worker_on_mutation_publish.exchange(
                false, std::memory_order_acq_rel)) {
            // SYS_exit terminates only this Linux thread and deliberately
            // bypasses C++ stack cleanup, so the kernel marks the robust
            // region mutex owner-dead while the process survives.
            static_cast<void>(::syscall(SYS_exit, 0));
            std::_Exit(83);
        }
    }
    if (die_during_post_publish_gc != 0 &&
        mutation_was_published != 0 &&
        (event == kvspace::detail::RegionTestEvent::BlobMarkedFree ||
         event == kvspace::detail::RegionTestEvent::TrieNodeReclaimed ||
         event == kvspace::detail::RegionTestEvent::TrieBoxReclaimed ||
         event == kvspace::detail::RegionTestEvent::ArtBoxNodeReclaimed ||
         event == kvspace::detail::RegionTestEvent::ArtBoxObjectReclaimed ||
         event == kvspace::detail::RegionTestEvent::HashBoxObjectReclaimed)) {
        ::_exit(76);
    }
    if (allocator_event_to_die_at ==
        static_cast<sig_atomic_t>(event)) {
        ::_exit(77);
    }
    if (event == kvspace::detail::RegionTestEvent::BackingOpenedBeforeLock &&
        backing_opened_notify_fd >= 0) {
        const char ready = 'o';
        const auto fd = static_cast<int>(backing_opened_notify_fd);
        backing_opened_notify_fd = -1;
        if (::write(fd, &ready, 1) != 1) ::_exit(78);
    }
}

void artBumpRegionTestHook(
    kvspace::detail::ArtBumpApplyStep step,
    std::uint64_t ordinal) noexcept {
    if (art_bump_step_to_die_at != static_cast<sig_atomic_t>(step)) return;
    if (art_bump_ordinal_to_die_at >= 0 &&
        ordinal != static_cast<std::uint64_t>(art_bump_ordinal_to_die_at)) {
        return;
    }
    ::_exit(80);
}

void commonApplyTestHook(
    kvspace::detail::CommonApplyPhase phase,
    kvspace::detail::CommonApplyStep step,
    std::uint64_t ordinal) noexcept {
    if (common_apply_trace_fd >= 0) {
        const CommonApplyTraceRecord record{
            static_cast<std::uint64_t>(phase),
            static_cast<std::uint64_t>(step),
            ordinal};
        const auto fd = static_cast<int>(common_apply_trace_fd);
        const auto written = ::write(fd, &record, sizeof(record));
        if (written != static_cast<ssize_t>(sizeof(record))) ::_exit(82);
    }
    if (common_apply_phase_to_die_at ==
            static_cast<sig_atomic_t>(phase) &&
        common_apply_step_to_die_at ==
            static_cast<sig_atomic_t>(step) &&
        common_apply_ordinal_to_die_at == ordinal) {
        ::_exit(81);
    }
}
}

extern "C" int __real_pthread_cond_broadcast(pthread_cond_t* condition);
extern "C" int __wrap_pthread_cond_broadcast(pthread_cond_t* condition) {
    if (die_on_cond_broadcast != 0) ::_exit(73);
    return __real_pthread_cond_broadcast(condition);
}
extern "C" int __real_ftruncate(int fd, off_t length);
extern "C" int __wrap_ftruncate(int fd, off_t length) {
    if (die_on_ftruncate != 0) ::_exit(74);
    return __real_ftruncate(fd, length);
}

namespace {

using namespace std::chrono_literals;

void configureArtBumpDeath(const std::string& encoded) {
    const auto separator = encoded.find('-');
    art_bump_step_to_die_at = static_cast<sig_atomic_t>(std::stoul(
        encoded.substr(0, separator)));
    art_bump_ordinal_to_die_at = separator == std::string::npos
        ? -1
        : static_cast<sig_atomic_t>(
              std::stoul(encoded.substr(separator + 1U)));
}

void configureCommonApplyDeath(const std::string& encoded) {
    const auto phase_end = encoded.find('-');
    const auto step_end = encoded.find('-', phase_end + 1U);
    CHECK(phase_end != std::string::npos);
    CHECK(step_end != std::string::npos);
    common_apply_phase_to_die_at = static_cast<sig_atomic_t>(
        std::stoul(encoded.substr(0, phase_end)));
    common_apply_step_to_die_at = static_cast<sig_atomic_t>(
        std::stoul(encoded.substr(phase_end + 1U, step_end - phase_end - 1U)));
    common_apply_ordinal_to_die_at = std::stoull(
        encoded.substr(step_end + 1U));
}

void requireChild(pid_t child) {
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

int childMain(const std::string& name, const std::string& mode) {
    try {
        if (mode == "write") {
            auto store = kvspace::ShmClient::Attach(name);
            store->Set("/child/value", kvspace::XValue::Int64(123));
            store->Notify("/ready", kvspace::XValue::Str("done"));
            return EXIT_SUCCESS;
        }
        if (mode == "lock-and-die") {
            auto region = kvspace::detail::Region::Open(
                name, {}, kvspace::detail::OpenMode::Attach);
            auto guard = region->Lock();
            (void)guard;
            ::_exit(73);
        }
        if (mode == "watch-forever") {
            auto store = kvspace::ShmClient::Attach(name);
            store->Set("/watcher-ready", kvspace::XValue::Bool(true));
            (void)store->Watch("/dead", 0ms);
            return EXIT_FAILURE;
        }
        if (mode == "notify-die-at-broadcast") {
            auto store = kvspace::ShmClient::Attach(name);
            die_on_cond_broadcast = 1;
            store->Notify("/crash-notify", kvspace::XValue::Int64(41));
            return EXIT_FAILURE;
        }
        if (mode == "create-die-at-ftruncate") {
            die_on_ftruncate = 1;
            (void)kvspace::ShmClient::Create(name);
            return EXIT_FAILURE;
        }
        if (mode == "mutation-die-before-commit") {
            auto region = kvspace::detail::Region::Open(
                name, {}, kvspace::detail::OpenMode::Attach);
            auto guard = region->Lock();
            auto mutation = region->BeginMutation();
            region->Put("/staged", kvspace::XValue::Int64(99).Encode());
            (void)guard;
            (void)mutation;
            ::_exit(75);
        }
        if (mode == "mutation-die-during-gc") {
            auto store = kvspace::ShmClient::Attach(name);
            die_during_post_publish_gc = 1;
            kvspace::detail::SetRegionTestHook(regionTestHook);
            store->Set("/victim", kvspace::XValue::Str("new-value"));
            return EXIT_FAILURE;
        }
        if (mode == "mutation-die-during-trie-box-gc") {
            auto store = kvspace::ShmClient::Attach(
                name, kvspace::ShmEngine::TrieBox);
            allocator_event_to_die_at = static_cast<sig_atomic_t>(
                kvspace::detail::RegionTestEvent::TrieBoxReclaimed);
            kvspace::detail::SetRegionTestHook(regionTestHook);
            store->Set("/victim", kvspace::XValue::Str("new-value"));
            return EXIT_FAILURE;
        }
        const std::string art_bump_mutation_prefix =
            "art-bump-mutation-die-";
        if (mode.compare(
                0,
                art_bump_mutation_prefix.size(),
                art_bump_mutation_prefix) == 0) {
            auto store = kvspace::ShmClient::Attach(
                name, kvspace::ShmEngine::ArtBump);
            configureArtBumpDeath(
                mode.substr(art_bump_mutation_prefix.size()));
            kvspace::detail::SetArtBumpRegionTestHook(
                artBumpRegionTestHook);
            store->Set(
                "/authority",
                kvspace::XValue::Str(std::string(2048, 'n')));
            return EXIT_FAILURE;
        }
        const std::string art_bump_compact_prefix =
            "art-bump-compact-die-";
        if (mode.compare(
                0,
                art_bump_compact_prefix.size(),
                art_bump_compact_prefix) == 0) {
            auto store = kvspace::ShmClient::Attach(
                name, kvspace::ShmEngine::ArtBump);
            configureArtBumpDeath(
                mode.substr(art_bump_compact_prefix.size()));
            kvspace::detail::SetArtBumpRegionTestHook(
                artBumpRegionTestHook);
            store->Compact();
            return EXIT_FAILURE;
        }
        const std::string art_bump_clear_prefix =
            "art-bump-clear-die-";
        if (mode.compare(
                0,
                art_bump_clear_prefix.size(),
                art_bump_clear_prefix) == 0) {
            auto store = kvspace::ShmClient::Attach(
                name, kvspace::ShmEngine::ArtBump);
            configureArtBumpDeath(
                mode.substr(art_bump_clear_prefix.size()));
            kvspace::detail::SetArtBumpRegionTestHook(
                artBumpRegionTestHook);
            store->Clear();
            return EXIT_FAILURE;
        }
        const std::string art_bump_recovery_prefix =
            "art-bump-recovery-die-";
        if (mode.compare(
                0,
                art_bump_recovery_prefix.size(),
                art_bump_recovery_prefix) == 0) {
            configureArtBumpDeath(
                mode.substr(art_bump_recovery_prefix.size()));
            kvspace::detail::SetArtBumpRegionTestHook(
                artBumpRegionTestHook);
            static_cast<void>(kvspace::ShmClient::Attach(
                name, kvspace::ShmEngine::ArtBump));
            return EXIT_FAILURE;
        }
        const std::string common_clear_trace_prefix =
            "common-clear-trace-";
        if (mode.compare(
                0,
                common_clear_trace_prefix.size(),
                common_clear_trace_prefix) == 0) {
            common_apply_trace_fd = static_cast<sig_atomic_t>(std::stoul(
                mode.substr(common_clear_trace_prefix.size())));
            kvspace::detail::SetCommonApplyTestHook(commonApplyTestHook);
            auto store = kvspace::ShmClient::Attach(name);
            store->Clear();
            return EXIT_SUCCESS;
        }
        const std::string common_recovery_trace_prefix =
            "common-recovery-trace-";
        if (mode.compare(
                0,
                common_recovery_trace_prefix.size(),
                common_recovery_trace_prefix) == 0) {
            common_apply_trace_fd = static_cast<sig_atomic_t>(std::stoul(
                mode.substr(common_recovery_trace_prefix.size())));
            kvspace::detail::SetCommonApplyTestHook(commonApplyTestHook);
            static_cast<void>(kvspace::ShmClient::Attach(name));
            return EXIT_SUCCESS;
        }
        const std::string common_clear_death_prefix =
            "common-clear-die-";
        if (mode.compare(
                0,
                common_clear_death_prefix.size(),
                common_clear_death_prefix) == 0) {
            configureCommonApplyDeath(
                mode.substr(common_clear_death_prefix.size()));
            kvspace::detail::SetCommonApplyTestHook(commonApplyTestHook);
            auto store = kvspace::ShmClient::Attach(name);
            store->Clear();
            return EXIT_FAILURE;
        }
        const std::string common_recovery_death_prefix =
            "common-recovery-die-";
        if (mode.compare(
                0,
                common_recovery_death_prefix.size(),
                common_recovery_death_prefix) == 0) {
            configureCommonApplyDeath(
                mode.substr(common_recovery_death_prefix.size()));
            kvspace::detail::SetCommonApplyTestHook(commonApplyTestHook);
            static_cast<void>(kvspace::ShmClient::Attach(name));
            return EXIT_FAILURE;
        }
        const std::string recovery_prefix = "recovery-die-";
        if (mode.compare(0, recovery_prefix.size(), recovery_prefix) == 0) {
            allocator_event_to_die_at = static_cast<sig_atomic_t>(
                std::stoul(mode.substr(recovery_prefix.size())));
            kvspace::detail::SetRegionTestHook(regionTestHook);
            static_cast<void>(kvspace::detail::Region::Open(
                name, {}, kvspace::detail::OpenMode::Attach));
            return EXIT_FAILURE;
        }
        const std::string clear_prefix = "clear-die-";
        if (mode.compare(0, clear_prefix.size(), clear_prefix) == 0) {
            allocator_event_to_die_at = static_cast<sig_atomic_t>(
                std::stoul(mode.substr(clear_prefix.size())));
            kvspace::detail::SetRegionTestHook(regionTestHook);
            auto store = kvspace::ShmClient::Attach(name);
            store->Clear();
            return EXIT_FAILURE;
        }
        const std::string split_prefix = "allocator-split-die-";
        if (mode.compare(0, split_prefix.size(), split_prefix) == 0) {
            const auto encoded = mode.substr(split_prefix.size());
            allocator_event_to_die_at = static_cast<sig_atomic_t>(
                std::stoul(encoded));
            auto region = kvspace::detail::Region::Open(
                name, {}, kvspace::detail::OpenMode::Attach);
            kvspace::detail::SetRegionTestHook(regionTestHook);
            auto guard = region->Lock();
            region->Notify(
                "tiny", kvspace::XValue::Bytes({1}).Encode());
            (void)guard;
            return EXIT_FAILURE;
        }
        return EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}

pid_t execChild(const char* self, const std::string& name, const char* mode) {
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        ::execl(self, self, "--child", name.c_str(), mode, nullptr);
        ::_exit(127);
    }
    return child;
}

pid_t forkChild(const std::string& name, const std::string& mode) {
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        const auto result = childMain(name, mode);
        ::_exit(result);
    }
    return child;
}

std::string encodeCommonApplyPoint(
    const char* prefix,
    const CommonApplyTraceRecord& point) {
    return std::string(prefix) + std::to_string(point.phase) + "-" +
        std::to_string(point.step) + "-" +
        std::to_string(point.ordinal);
}

std::vector<CommonApplyTraceRecord> traceCommonApply(
    const std::string& name,
    const char* mode_prefix,
    kvspace::detail::CommonApplyPhase expected_phase) {
    int descriptors[2] = {-1, -1};
    CHECK(::pipe(descriptors) == 0);
    const auto mode = std::string(mode_prefix) +
        std::to_string(descriptors[1]);
    const auto child = forkChild(name, mode);
    CHECK(::close(descriptors[1]) == 0);

    std::vector<std::uint8_t> bytes;
    std::array<std::uint8_t, 4096> buffer{};
    for (;;) {
        const auto received = ::read(
            descriptors[0], buffer.data(), buffer.size());
        if (received == 0) break;
        if (received < 0 && errno == EINTR) continue;
        CHECK(received > 0);
        bytes.insert(
            bytes.end(),
            buffer.begin(),
            buffer.begin() + received);
    }
    CHECK(::close(descriptors[0]) == 0);
    requireChild(child);
    CHECK(bytes.size() % sizeof(CommonApplyTraceRecord) == 0);

    std::vector<CommonApplyTraceRecord> trace(
        bytes.size() / sizeof(CommonApplyTraceRecord));
    if (!bytes.empty()) {
        std::memcpy(trace.data(), bytes.data(), bytes.size());
    }
    std::array<
        std::uint64_t,
        static_cast<std::size_t>(kvspace::detail::CommonApplyStep::Count)>
        next_ordinals{};
    for (const auto& point : trace) {
        CHECK(point.phase == static_cast<std::uint64_t>(expected_phase));
        CHECK(point.step > 0);
        CHECK(point.step < static_cast<std::uint64_t>(
            kvspace::detail::CommonApplyStep::Count));
        const auto step = static_cast<std::size_t>(point.step);
        CHECK(point.ordinal == next_ordinals[step]);
        ++next_ordinals[step];
    }
    return trace;
}

void testIndependentProcess(const char* self, kvspace::ShmEngine engine) {
    ScopedRegion region(uniqueShmName("process"));
    kvspace::ShmOptions options;
    options.max_entries = 1024;
    options.max_queues = 64;
    options.engine = engine;
    auto store = kvspace::ShmClient::Create(region.name(), options);
    const auto child = execChild(self, region.name(), "write");
    CHECK(store->Watch("/ready", 5s).AsStr() == "done");
    requireChild(child);
    CHECK(store->Get("/child/value").AsInt64() == 123);
}

void testConcurrentWriters(kvspace::ShmEngine engine) {
    ScopedRegion region(uniqueShmName("writers"));
    kvspace::ShmOptions options;
    options.initial_size = 512 * 1024;
    options.max_size =
        engine == kvspace::ShmEngine::ArtBox ||
            engine == kvspace::ShmEngine::ArtBump
        ? 64ULL * 1024 * 1024
        : 16ULL * 1024 * 1024;
    options.max_entries = 4096;
    options.engine = engine;
    auto store = kvspace::ShmClient::Create(region.name(), options);

    std::vector<pid_t> children;
    for (int process = 0; process < 4; ++process) {
        const auto child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            try {
                auto attached = kvspace::ShmClient::Attach(region.name());
                for (int item = 0; item < 100; ++item) {
                    attached->Set(
                        "/p" + std::to_string(process) + "/v" + std::to_string(item),
                        kvspace::XValue::Int64(process * 1000 + item));
                }
                ::_exit(0);
            } catch (...) {
                ::_exit(1);
            }
        }
        children.push_back(child);
    }
    for (const auto child : children) requireChild(child);
    for (int process = 0; process < 4; ++process) {
        for (int item = 0; item < 100; ++item) {
            CHECK(store->Get(
                "/p" + std::to_string(process) + "/v" + std::to_string(item)).AsInt64() ==
                process * 1000 + item);
        }
    }
}

void testRobustOwnerDeath(const char* self, kvspace::ShmEngine engine) {
    ScopedRegion region(uniqueShmName("recovery"));
    kvspace::ShmOptions options;
    options.engine = engine;
    auto store = kvspace::ShmClient::Create(region.name(), options);
    store->Set("/before", kvspace::XValue::Int64(9));
    const auto child = execChild(self, region.name(), "lock-and-die");
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 73);

    store->Set("/after", kvspace::XValue::Int64(10));
    CHECK(store->Get("/before").AsInt64() == 9);
    CHECK(store->Get("/after").AsInt64() == 10);
    CHECK(store->Stats().recoveries >= 1);
}

void testTrieBoxThreadOwnerDeathClearsMutationState() {
    ScopedRegion region(uniqueShmName("trie-thread-recovery"));
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::TrieBox;
    auto store = kvspace::detail::Region::Open(
        region.name(), options, kvspace::detail::OpenMode::Create,
        kvspace::ShmEngine::TrieBox);
    {
        auto guard = store->Lock();
        auto mutation = store->BeginMutation();
        store->Put(
            "/before-thread", kvspace::XValue::Int64(1).Encode());
        mutation.Commit();
    }

    mutation_was_published = 0;
    terminate_worker_on_mutation_publish.store(true, std::memory_order_release);
    kvspace::detail::SetRegionTestHook(regionTestHook);
    std::atomic<bool> worker_returned{false};
    std::thread worker([&] {
        auto guard = store->Lock();
        auto mutation = store->BeginMutation();
        store->Put(
            "/published-by-thread", kvspace::XValue::Int64(2).Encode());
        mutation.Commit();
        worker_returned.store(true, std::memory_order_release);
    });
    worker.join();
    kvspace::detail::SetRegionTestHook(nullptr);
    terminate_worker_on_mutation_publish.store(false, std::memory_order_release);

    CHECK(mutation_was_published != 0);
    CHECK(!worker_returned.load(std::memory_order_acquire));
    auto guard = store->Lock();
    CHECK(store->Stats().recoveries == 1);
    std::vector<std::uint8_t> encoded;
    CHECK(store->Get("/before-thread", &encoded));
    CHECK(kvspace::XValue::Decode(encoded).AsInt64() == 1);
    CHECK(store->Get("/published-by-thread", &encoded));
    CHECK(kvspace::XValue::Decode(encoded).AsInt64() == 2);

    // Recovery on the same Region object must discard the dead thread's
    // volatile mutation bookkeeping, not leave BeginMutation nested forever.
    auto mutation = store->BeginMutation();
    store->Put(
        "/after-thread-recovery", kvspace::XValue::Int64(3).Encode());
    mutation.Commit();
    CHECK(store->Get("/after-thread-recovery", &encoded));
    CHECK(kvspace::XValue::Decode(encoded).AsInt64() == 3);
}

void testTrieBoxThreadOwnerDeathInIsolatedRuntime(const char* self) {
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        ::execl(
            self,
            self,
            "--thread-owner-death",
            static_cast<char*>(nullptr));
        ::_exit(127);
    }
    requireChild(child);
}

void testKilledWatcherDoesNotLeakQueue(
    const char* self,
    kvspace::ShmEngine engine) {
    ScopedRegion region(uniqueShmName("killed-watcher"));
    kvspace::ShmOptions options;
    options.max_entries = 32;
    options.max_queues = 1;
    options.engine = engine;
    auto store = kvspace::ShmClient::Create(region.name(), options);
    const auto child = execChild(self, region.name(), "watch-forever");

    for (int attempt = 0;
         attempt < 1000 && store->Get("/watcher-ready").IsNull(); ++attempt) {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(store->Get("/watcher-ready").AsBool());
    for (int attempt = 0;
         attempt < 100 && store->Stats().queues == 0; ++attempt) {
        std::this_thread::sleep_for(1ms);
    }

    CHECK(::kill(child, SIGKILL) == 0);
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status));
    CHECK(WTERMSIG(status) == SIGKILL);
    CHECK(store->Stats().queues == 0);
    store->Notify("/new", kvspace::XValue::Int64(11));
    CHECK(store->Watch("/new", 10ms).AsInt64() == 11);
    CHECK(store->Stats().queues == 0);
}

void testNotifierDeathCannotStrandPublishedMessage(
    const char* self,
    kvspace::ShmEngine engine) {
    ScopedRegion region(uniqueShmName("killed-notifier"));
    kvspace::ShmOptions options;
    options.engine = engine;
    auto store = kvspace::ShmClient::Create(region.name(), options);
    auto watched = std::async(std::launch::async, [&] {
        return store->Watch("/crash-notify", 2s);
    });
    std::this_thread::sleep_for(20ms);

    const auto child = execChild(
        self, region.name(), "notify-die-at-broadcast");
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 73);

    CHECK(store->Stats().recoveries >= 1);
    CHECK(store->Stats().queues == 0);
    CHECK(watched.wait_for(50ms) == std::future_status::timeout);
    store->Notify("/crash-notify", kvspace::XValue::Int64(42));
    CHECK(watched.get().AsInt64() == 42);
    CHECK(store->Watch("/crash-notify", 10ms).IsNull());
}

void testOpenRecoversInterruptedCreate(const char* self) {
    ScopedRegion region(uniqueShmName("interrupted-create"));
    const auto child = execChild(
        self, region.name(), "create-die-at-ftruncate");
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 74);

    expectThrows<kvspace::ErrCorruptRegion>([&] {
        (void)kvspace::ShmClient::Attach(region.name());
    });
    auto store = kvspace::ShmClient::Open(region.name());
    store->Set("/recovered", kvspace::XValue::Int64(1));
    CHECK(store->Get("/recovered").AsInt64() == 1);
}

void testOwnerDeathDiscardsUncommittedMutation(
    const char* self,
    kvspace::ShmEngine engine) {
    ScopedRegion region(uniqueShmName("uncommitted-mutation"));
    kvspace::ShmOptions options;
    options.engine = engine;
    auto store = kvspace::ShmClient::Create(region.name(), options);
    store->Set("/kept", kvspace::XValue::Int64(1));
    const auto child = execChild(
        self, region.name(), "mutation-die-before-commit");
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 75);

    CHECK(store->Get("/kept").AsInt64() == 1);
    CHECK(store->Get("/staged").IsNull());
    CHECK(store->Stats().recoveries >= 1);
}

void testOwnerDeathDuringPostPublishGc(
    const char* self,
    kvspace::ShmEngine engine) {
    ScopedRegion region(uniqueShmName("post-publish-gc"));
    kvspace::ShmOptions options;
    options.engine = engine;
    auto store = kvspace::ShmClient::Create(region.name(), options);
    store->Set("/victim", kvspace::XValue::Str("old-value"));

    const auto child = execChild(
        self, region.name(), "mutation-die-during-gc");
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 76);

    CHECK(store->Get("/victim").AsStr() == "new-value");
    store->Set("/after-gc-recovery", kvspace::XValue::Int64(12));
    CHECK(store->Get("/after-gc-recovery").AsInt64() == 12);
    CHECK(store->Stats().recoveries >= 1);
}

void testOwnerDeathDuringTrieBoxGc(
    const char* self) {
    ScopedRegion region(uniqueShmName("trie-box-post-publish-gc"));
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::TrieBox;
    auto store = kvspace::ShmClient::Create(region.name(), options);
    store->Set("/victim", kvspace::XValue::Str("old-value"));

    const auto child = execChild(
        self, region.name(), "mutation-die-during-trie-box-gc");
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 77);

    CHECK(store->Get("/victim").AsStr() == "new-value");
    store->Set("/after-box-gc-recovery", kvspace::XValue::Int64(13));
    CHECK(store->Get("/after-box-gc-recovery").AsInt64() == 13);
    CHECK(store->Stats().recoveries >= 1);
}

void testOwnerDeathAtAllocatorSplitCutPoints(
    const char* self,
    kvspace::ShmEngine engine) {
    for (const auto event : {
             kvspace::detail::RegionTestEvent::AllocatorSplitJournalPublished,
             kvspace::detail::RegionTestEvent::AllocatorSplitRemainderReady,
             kvspace::detail::RegionTestEvent::AllocatorSplitLinksReady,
             kvspace::detail::RegionTestEvent::AllocatorSplitBoundaryCommitted,
             kvspace::detail::RegionTestEvent::AllocatorSplitJournalCleared}) {
        ScopedRegion scoped(uniqueShmName("allocator-split"));
        kvspace::ShmOptions options;
        options.engine = engine;
        auto owner = kvspace::detail::Region::Open(
            scoped.name(), options, kvspace::detail::OpenMode::Create);
        {
            auto guard = owner->Lock();
            const auto expected = kvspace::XValue::Bytes(
                std::vector<std::uint8_t>(8192, 3)).Encode();
            owner->Notify("large", expected);
            std::vector<std::uint8_t> notification;
            CHECK(owner->Watch(
                guard, "large", 1ms, &notification, [] { return false; }));
            CHECK(notification == expected);
            (void)guard;
        }

        const auto mode = "allocator-split-die-" + std::to_string(
            static_cast<std::uint32_t>(event));
        const auto child = execChild(self, scoped.name(), mode.c_str());
        int status = 0;
        CHECK(::waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 77);

        owner.reset();
        auto recovered = kvspace::detail::Region::Open(
            scoped.name(), {}, kvspace::detail::OpenMode::Attach, engine);
        auto guard = recovered->Lock();
        CHECK(!recovered->Exists("tiny"));
        const auto expected =
            kvspace::XValue::Bytes({4, 5, 6}).Encode();
        recovered->Put("after", expected);
        std::vector<std::uint8_t> value;
        CHECK(recovered->Get("after", &value));
        CHECK(value == expected);
        CHECK(recovered->Stats().recoveries >= 1);
        (void)guard;
    }
}

void prepareInteriorCoalesceRun(kvspace::ShmClient* store) {
    store->Notify("/gone", kvspace::XValue::Int64(1));
    store->Notify("/gone", kvspace::XValue::Int64(2));
    store->Notify("/keep", kvspace::XValue::Int64(9));
    CHECK(store->Watch("/gone", 5ms).AsInt64() == 2);
    CHECK(store->Watch("/gone", 5ms).AsInt64() == 1);
}

void requireExitCode(pid_t child, int expected) {
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == expected);
}

pid_t recoveryChild(
    const char* self,
    const std::string& name,
    kvspace::detail::RegionTestEvent event) {
    const auto mode = "recovery-die-" + std::to_string(
        static_cast<std::uint32_t>(event));
    return execChild(self, name, mode.c_str());
}

pid_t clearChild(
    const char* self,
    const std::string& name,
    kvspace::detail::RegionTestEvent event) {
    const auto mode = "clear-die-" + std::to_string(
        static_cast<std::uint32_t>(event));
    return execChild(self, name, mode.c_str());
}

kvspace::ShmOptions commonClearTestOptions() {
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::HashBox;
    options.initial_size = 2ULL * 1024U * 1024U;
    options.max_size = options.initial_size;
    options.max_entries = 8;
    options.max_queues = 4;
    return options;
}

std::unique_ptr<kvspace::ShmClient> prepareCommonClearFixture(
    const std::string& name,
    bool single_span) {
    auto store = kvspace::ShmClient::Create(
        name, commonClearTestOptions());
    store->Set("/kept", kvspace::XValue::Str("old-engine"));
    if (single_span) {
        store->Notify("/discard", kvspace::XValue::Int64(1));
        CHECK(store->Watch("/discard", 5ms).AsInt64() == 1);
        requireExitCode(forkChild(name, "lock-and-die"), 73);
        const auto recovered = store->Stats();
        CHECK(recovered.queues == 0);
        CHECK(recovered.heap_free > 0);
        return store;
    }

    store->Notify("/clear-a", kvspace::XValue::Int64(1));
    store->Notify("/discard", kvspace::XValue::Int64(2));
    store->Notify(
        "/clear-a",
        kvspace::XValue::Bytes(std::vector<std::uint8_t>(257, 3)));
    store->Notify("/clear-b", kvspace::XValue::Int64(4));
    CHECK(store->Watch("/discard", 5ms).AsInt64() == 2);
    const auto prepared = store->Stats();
    CHECK(prepared.queues == 2);
    CHECK(prepared.heap_free > 0);
    return store;
}

std::size_t commonStepCount(
    const std::vector<CommonApplyTraceRecord>& trace,
    kvspace::detail::CommonApplyStep step) {
    return static_cast<std::size_t>(std::count_if(
        trace.begin(), trace.end(), [&](const auto& point) {
            return point.step == static_cast<std::uint64_t>(step);
        }));
}

CommonApplyTraceRecord requireCommonPoint(
    const std::vector<CommonApplyTraceRecord>& trace,
    kvspace::detail::CommonApplyStep step,
    std::uint64_t ordinal) {
    const auto found = std::find_if(
        trace.begin(), trace.end(), [&](const auto& point) {
            return point.step == static_cast<std::uint64_t>(step) &&
                point.ordinal == ordinal;
        });
    CHECK(found != trace.end());
    return *found;
}

void verifyCommonClearRecoveryConverged(kvspace::ShmClient* store) {
    CHECK(store->Get("/kept").AsStr() == "old-engine");
    store->Notify("/after-common-recovery", kvspace::XValue::Int64(91));
    CHECK(store->Watch("/after-common-recovery", 5ms).AsInt64() == 91);
    store->Clear();
    const auto cleared = store->Stats();
    CHECK(cleared.entries == 0);
    CHECK(cleared.queues == 0);
    CHECK(cleared.heap_used == 0);
    CHECK(cleared.recoveries >= 1);
}

void runCommonClearDoubleDeath(
    bool single_span,
    const CommonApplyTraceRecord& clear_point,
    const CommonApplyTraceRecord& recovery_point,
    std::string_view label) {
    ScopedRegion scoped(uniqueShmName(label));
    auto store = prepareCommonClearFixture(scoped.name(), single_span);
    requireExitCode(
        forkChild(
            scoped.name(),
            encodeCommonApplyPoint(
                "common-clear-die-", clear_point)),
        81);
    requireExitCode(
        forkChild(
            scoped.name(),
            encodeCommonApplyPoint(
                "common-recovery-die-", recovery_point)),
        81);
    verifyCommonClearRecoveryConverged(store.get());
}

std::vector<CommonApplyTraceRecord> traceCommonClearFixture(
    bool single_span,
    std::string_view label) {
    ScopedRegion scoped(uniqueShmName(label));
    auto store = prepareCommonClearFixture(scoped.name(), single_span);
    auto trace = traceCommonApply(
        scoped.name(),
        "common-clear-trace-",
        kvspace::detail::CommonApplyPhase::Clear);
    CHECK(!trace.empty());
    return trace;
}

std::vector<CommonApplyTraceRecord> traceRecoveryAfterClearCut(
    bool single_span,
    const CommonApplyTraceRecord& clear_point,
    std::string_view label) {
    ScopedRegion scoped(uniqueShmName(label));
    auto store = prepareCommonClearFixture(scoped.name(), single_span);
    requireExitCode(
        forkChild(
            scoped.name(),
            encodeCommonApplyPoint(
                "common-clear-die-", clear_point)),
        81);
    auto trace = traceCommonApply(
        scoped.name(),
        "common-recovery-trace-",
        kvspace::detail::CommonApplyPhase::Recovery);
    CHECK(!trace.empty());
    CHECK(store->Get("/kept").AsStr() == "old-engine");
    return trace;
}

void testCommonClearExhaustiveFaultMatrix() {
    {
        ScopedRegion scoped(uniqueShmName("common-clear-empty-trace"));
        auto store = kvspace::ShmClient::Create(
            scoped.name(), commonClearTestOptions());
        const auto trace = traceCommonApply(
            scoped.name(),
            "common-clear-trace-",
            kvspace::detail::CommonApplyPhase::Clear);
        CHECK(commonStepCount(
            trace, kvspace::detail::CommonApplyStep::QueueCountStored) == 1);
        CHECK(commonStepCount(
            trace, kvspace::detail::CommonApplyStep::HeapTopStored) == 0);
        CHECK(commonStepCount(
            trace, kvspace::detail::CommonApplyStep::HeapLastStored) == 0);
    }

    const auto single_trace = traceCommonClearFixture(
        true, "common-clear-single-trace");
    // One queue_count store, two complete eight-field Blob normalizations,
    // two 64-head clears, and top/last/generation: every store is distinct.
    CHECK(single_trace.size() == 148);
    CHECK(commonStepCount(
        single_trace,
        kvspace::detail::CommonApplyStep::QueueStateStored) == 0);
    CHECK(commonStepCount(
        single_trace,
        kvspace::detail::CommonApplyStep::FreeHeadStored) == 128);
    CHECK(commonStepCount(
        single_trace,
        kvspace::detail::CommonApplyStep::BlobFlagsStored) == 2);
    CHECK(commonStepCount(
        single_trace,
        kvspace::detail::CommonApplyStep::AllocatorJournalStateStored) == 0);
    CHECK(commonStepCount(
        single_trace,
        kvspace::detail::CommonApplyStep::HeapTopStored) == 1);
    CHECK(commonStepCount(
        single_trace,
        kvspace::detail::CommonApplyStep::HeapLastStored) == 1);

    const auto multi_trace = traceCommonClearFixture(
        false, "common-clear-multi-trace");
    // Seven old spans, five ownership releases, six journaled coalesces, and
    // both complete free-head clears produce 375 persistent write boundaries.
    CHECK(multi_trace.size() == 375);
    CHECK(commonStepCount(
        multi_trace,
        kvspace::detail::CommonApplyStep::QueueStateStored) == 2);
    CHECK(commonStepCount(
        multi_trace,
        kvspace::detail::CommonApplyStep::QueueMessageReleased) == 3);
    CHECK(commonStepCount(
        multi_trace,
        kvspace::detail::CommonApplyStep::QueueKeyReleased) == 2);
    CHECK(commonStepCount(
        multi_trace,
        kvspace::detail::CommonApplyStep::QueueHashStored) == 2);
    CHECK(commonStepCount(
        multi_trace,
        kvspace::detail::CommonApplyStep::QueueKeyOffsetStored) == 2);
    CHECK(commonStepCount(
        multi_trace,
        kvspace::detail::CommonApplyStep::QueueHeadOffsetStored) == 2);
    CHECK(commonStepCount(
        multi_trace,
        kvspace::detail::CommonApplyStep::QueueKeyLengthStored) == 2);
    CHECK(commonStepCount(
        multi_trace,
        kvspace::detail::CommonApplyStep::BlobFlagsStored) == 13);
    CHECK(commonStepCount(
        multi_trace,
        kvspace::detail::CommonApplyStep::AllocatorJournalOffsetStored) == 6);
    CHECK(commonStepCount(
        multi_trace,
        kvspace::detail::CommonApplyStep::AllocatorJournalStateStored) == 12);
    CHECK(commonStepCount(
        multi_trace,
        kvspace::detail::CommonApplyStep::HeapTopStored) == 1);
    CHECK(commonStepCount(
        multi_trace,
        kvspace::detail::CommonApplyStep::HeapLastStored) == 2);

    const CommonApplyTraceRecord recovery_first_hash{
        static_cast<std::uint64_t>(
            kvspace::detail::CommonApplyPhase::Recovery),
        static_cast<std::uint64_t>(
            kvspace::detail::CommonApplyStep::QueueHashStored),
        0};
    for (const auto& point : single_trace) {
        runCommonClearDoubleDeath(
            true,
            point,
            recovery_first_hash,
            "common-clear-single-cut");
    }
    for (const auto& point : multi_trace) {
        runCommonClearDoubleDeath(
            false,
            point,
            recovery_first_hash,
            "common-clear-multi-cut");
    }

    struct RecoveryFixture {
        bool single_span;
        CommonApplyTraceRecord clear_point;
        std::vector<CommonApplyTraceRecord> recovery_trace;
        std::string_view label;
    };
    std::vector<RecoveryFixture> recovery_fixtures;
    const auto first_queue_publication = requireCommonPoint(
        multi_trace,
        kvspace::detail::CommonApplyStep::QueueStateStored,
        0);
    auto queue_recovery_trace = traceRecoveryAfterClearCut(
        false,
        first_queue_publication,
        "common-recovery-queue-trace");
    CHECK(queue_recovery_trace.size() == 189);
    recovery_fixtures.push_back({
        false,
        first_queue_publication,
        std::move(queue_recovery_trace),
        "common-recovery-queue-cut"});
    const auto first_coalesce_publication = requireCommonPoint(
        multi_trace,
        kvspace::detail::CommonApplyStep::AllocatorJournalStateStored,
        0);
    auto journal_recovery_trace = traceRecoveryAfterClearCut(
        false,
        first_coalesce_publication,
        "common-recovery-journal-trace");
    CHECK(journal_recovery_trace.size() == 256);
    recovery_fixtures.push_back({
        false,
        first_coalesce_publication,
        std::move(journal_recovery_trace),
        "common-recovery-journal-cut"});
    const auto top_first_publication = requireCommonPoint(
        single_trace,
        kvspace::detail::CommonApplyStep::HeapTopStored,
        0);
    auto top_recovery_trace = traceRecoveryAfterClearCut(
        true,
        top_first_publication,
        "common-recovery-top-trace");
    CHECK(top_recovery_trace.size() == 100);
    recovery_fixtures.push_back({
        true,
        top_first_publication,
        std::move(top_recovery_trace),
        "common-recovery-top-cut"});

    for (const auto& fixture : recovery_fixtures) {
        CHECK(commonStepCount(
            fixture.recovery_trace,
            kvspace::detail::CommonApplyStep::RecoveryCountStored) == 1);
        CHECK(commonStepCount(
            fixture.recovery_trace,
            kvspace::detail::CommonApplyStep::GenerationStored) == 1);
        for (const auto& recovery_point : fixture.recovery_trace) {
            runCommonClearDoubleDeath(
                fixture.single_span,
                fixture.clear_point,
                recovery_point,
                fixture.label);
        }
    }
}

void testClearTopFirstResetSurvivesRepeatedRecovery(const char* self) {
    ScopedRegion scoped(uniqueShmName("clear-top-first-repeat"));
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::HashBox;
    options.initial_size = 16ULL * 1024U * 1024U;
    options.max_size = options.initial_size;
    options.max_entries = 64;
    options.max_queues = 8;
    auto store = kvspace::ShmClient::Create(scoped.name(), options);
    store->Set("/kept", kvspace::XValue::Str("old-engine"));
    store->Notify("/clear-a", kvspace::XValue::Int64(1));
    store->Notify("/clear-a", kvspace::XValue::Bytes(
        std::vector<std::uint8_t>(4096, 2)));
    store->Notify("/clear-b", kvspace::XValue::Int64(3));

    // Clear has removed all queue authority and coalesced the old extent, but
    // dies after publishing top=H and before last=0.  The next owner must use
    // only the existing pending-first-append rollback classification.
    requireExitCode(
        clearChild(
            self,
            scoped.name(),
            kvspace::detail::RegionTestEvent::ClearHeapTopReset),
        77);
    requireExitCode(
        recoveryChild(
            self,
            scoped.name(),
            kvspace::detail::RegionTestEvent::RecoveryPendingTailReset),
        77);

    // A third owner repeats the full plans and converges.  The interrupted
    // Clear had not reached the engine phase, so the old committed map remains
    // authoritative while its already-published queue deletions remain gone.
    CHECK(store->Get("/kept").AsStr() == "old-engine");
    CHECK(store->Watch("/clear-a", 5ms).IsNull());
    CHECK(store->Watch("/clear-b", 5ms).IsNull());
    CHECK(store->Stats().queues == 0);
    CHECK(store->Stats().recoveries >= 1);

    store->Clear();
    const auto cleared = store->Stats();
    CHECK(cleared.entries == 0);
    CHECK(cleared.queues == 0);
    CHECK(cleared.heap_used == 0);
}

void testRepeatedOwnerDeathDuringArtBoxRecovery(const char* self) {
    ScopedRegion scoped(uniqueShmName("art-box-recovery-repeat"));
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::ArtBox;
    options.max_entries = 64;
    options.max_size = 16ULL * 1024 * 1024;
    auto store = kvspace::ShmClient::Create(scoped.name(), options);
    store->Set("/kept/a", kvspace::XValue::Str("alpha"));
    store->Set("/kept/b", kvspace::XValue::Bytes(
        std::vector<std::uint8_t>(4096, 7)));

    requireExitCode(
        execChild(self, scoped.name(), "lock-and-die"), 73);
    for (int attempt = 0; attempt < 2; ++attempt) {
        requireExitCode(
            recoveryChild(
                self,
                scoped.name(),
                kvspace::detail::RegionTestEvent::ArtBoxNodesRebuilt),
            77);
    }

    // The third recovery replays both prepared plans after two deaths at the
    // exact node-applied/Box-not-yet-applied boundary.
    CHECK(store->Get("/kept/a").AsStr() == "alpha");
    CHECK(store->Get("/kept/b").AsBytes() ==
          std::vector<std::uint8_t>(4096, 7));
    store->Set("/after-repeat", kvspace::XValue::Int64(31));
    CHECK(store->Get("/after-repeat").AsInt64() == 31);

    requireExitCode(
        execChild(self, scoped.name(), "lock-and-die"), 73);
    requireExitCode(
        recoveryChild(
            self,
            scoped.name(),
            kvspace::detail::RegionTestEvent::ArtBoxBoxRebuilt),
        77);
    CHECK(store->Get("/kept/a").AsStr() == "alpha");
    store->Set("/after-box-cut", kvspace::XValue::Int64(32));
    CHECK(store->Get("/after-box-cut").AsInt64() == 32);
    CHECK(store->Stats().recoveries >= 1);
}

void testRepeatedOwnerDeathBetweenTrieBoxRecoveryApplies(
    const char* self) {
    ScopedRegion scoped(uniqueShmName("trie-box-recovery-repeat"));
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::TrieBox;
    options.initial_size = 16ULL * 1024U * 1024U;
    options.max_size = options.initial_size;
    options.max_entries = 64;
    options.max_queues = 8;
    auto store = kvspace::ShmClient::Create(scoped.name(), options);
    store->Set("/kept/none", kvspace::XValue::Null());
    store->Set(
        "/kept/value",
        kvspace::XValue::Bytes(std::vector<std::uint8_t>(4096, 7)));
    const auto expected_entries = store->Stats().entries;

    requireExitCode(
        execChild(self, scoped.name(), "lock-and-die"), 73);
    for (int attempt = 0; attempt < 2; ++attempt) {
        requireExitCode(
            recoveryChild(
                self,
                scoped.name(),
                kvspace::detail::RegionTestEvent::TrieBoxNodesRebuilt),
            77);
    }

    // The third recovery replays both deterministic allocator plans and
    // completes without any persistent epoch or recovery journal.
    CHECK(store->Get("/kept/none").IsNull());
    CHECK(store->Get("/kept/value").AsBytes() ==
          std::vector<std::uint8_t>(4096, 7));
    CHECK(store->Stats().entries == expected_entries);
    store->Set("/after-repeat", kvspace::XValue::Int64(33));
    CHECK(store->Get("/after-repeat").AsInt64() == 33);

    // Event 34 is a separate, append-only cut point after the Box apply. Its
    // interrupted derived-counter/queue tail is likewise replayable.
    requireExitCode(
        execChild(self, scoped.name(), "lock-and-die"), 73);
    requireExitCode(
        recoveryChild(
            self,
            scoped.name(),
            kvspace::detail::RegionTestEvent::TrieBoxBoxRebuilt),
        77);
    CHECK(store->Get("/kept/value").AsBytes() ==
          std::vector<std::uint8_t>(4096, 7));
    store->Set("/after-box-cut", kvspace::XValue::Int64(34));
    CHECK(store->Get("/after-box-cut").AsInt64() == 34);
    CHECK(store->Stats().recoveries >= 1);
}

void testRepeatedOwnerDeathDuringHashBoxRecovery(const char* self) {
    ScopedRegion scoped(uniqueShmName("hash-box-recovery-repeat"));
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::HashBox;
    options.initial_size = 16ULL * 1024U * 1024U;
    options.max_size = options.initial_size;
    options.max_entries = 64;
    options.max_queues = 8;
    auto store = kvspace::ShmClient::Create(scoped.name(), options);
    store->Set("/kept/a", kvspace::XValue::Str("alpha"));
    store->Set(
        "/kept/value",
        kvspace::XValue::Bytes(std::vector<std::uint8_t>(4096, 7)));
    const auto expected_entries = store->Stats().entries;

    requireExitCode(
        execChild(self, scoped.name(), "lock-and-die"), 73);
    for (int attempt = 0; attempt < 2; ++attempt) {
        requireExitCode(
            recoveryChild(
                self,
                scoped.name(),
                kvspace::detail::RegionTestEvent::HashBoxBoxRebuilt),
            77);
    }

    CHECK(store->Get("/kept/a").AsStr() == "alpha");
    CHECK(store->Get("/kept/value").AsBytes() ==
          std::vector<std::uint8_t>(4096, 7));
    CHECK(store->Stats().entries == expected_entries);
    store->Set("/after-repeat", kvspace::XValue::Int64(35));
    CHECK(store->Get("/after-repeat").AsInt64() == 35);
    CHECK(store->Stats().recoveries >= 1);
}

void verifyCoalesceRecovery(kvspace::ShmClient* store) {
    CHECK(store->Watch("/keep", 5ms).AsInt64() == 9);
    CHECK(store->Watch("/keep", 5ms).IsNull());
    store->Set("/after-coalesce-recovery", kvspace::XValue::Int64(14));
    CHECK(store->Get("/after-coalesce-recovery").AsInt64() == 14);
    CHECK(store->Stats().recoveries >= 1);
}

void testOwnerDeathAtAllocatorCoalesceCutPoints(const char* self) {
    for (const auto event : {
             kvspace::detail::RegionTestEvent::
                 AllocatorCoalesceJournalPrepared,
             kvspace::detail::RegionTestEvent::
                 AllocatorCoalesceJournalPublished,
             kvspace::detail::RegionTestEvent::
                 AllocatorCoalesceBoundaryCommitted,
             kvspace::detail::RegionTestEvent::
                 AllocatorCoalesceFollowingLinked,
             kvspace::detail::RegionTestEvent::
                 AllocatorCoalesceJournalCleared}) {
        ScopedRegion scoped(uniqueShmName("allocator-coalesce"));
        kvspace::ShmOptions options;
        options.engine = kvspace::ShmEngine::TrieBox;
        auto store = kvspace::ShmClient::Create(scoped.name(), options);
        prepareInteriorCoalesceRun(store.get());

        requireExitCode(
            execChild(self, scoped.name(), "lock-and-die"), 73);
        requireExitCode(
            recoveryChild(self, scoped.name(), event), 77);
        verifyCoalesceRecovery(store.get());
    }

    {
        ScopedRegion scoped(uniqueShmName("allocator-coalesce-tail"));
        kvspace::ShmOptions options;
        options.engine = kvspace::ShmEngine::TrieBox;
        auto store = kvspace::ShmClient::Create(scoped.name(), options);
        store->Notify("/gone", kvspace::XValue::Int64(1));
        store->Notify("/gone", kvspace::XValue::Int64(2));
        CHECK(store->Watch("/gone", 5ms).AsInt64() == 2);
        CHECK(store->Watch("/gone", 5ms).AsInt64() == 1);

        requireExitCode(
            execChild(self, scoped.name(), "lock-and-die"), 73);
        requireExitCode(
            recoveryChild(
                self,
                scoped.name(),
                kvspace::detail::RegionTestEvent::
                    AllocatorCoalesceTailInstalled),
            77);
        store->Set("/after-tail-recovery", kvspace::XValue::Int64(15));
        CHECK(store->Get("/after-tail-recovery").AsInt64() == 15);
        CHECK(store->Stats().recoveries >= 1);
    }

    {
        ScopedRegion scoped(uniqueShmName("allocator-coalesce-replay"));
        kvspace::ShmOptions options;
        options.engine = kvspace::ShmEngine::TrieBox;
        auto store = kvspace::ShmClient::Create(scoped.name(), options);
        prepareInteriorCoalesceRun(store.get());

        requireExitCode(
            execChild(self, scoped.name(), "lock-and-die"), 73);
        requireExitCode(
            recoveryChild(
                self,
                scoped.name(),
                kvspace::detail::RegionTestEvent::
                    AllocatorCoalesceJournalPublished),
            77);
        requireExitCode(
            recoveryChild(
                self,
                scoped.name(),
                kvspace::detail::RegionTestEvent::
                    AllocatorCoalesceBoundaryCommitted),
            77);
        verifyCoalesceRecovery(store.get());
    }
}

std::uint64_t readLittleEndianAt(
    int fd,
    std::uint64_t offset,
    std::size_t width) {
    CHECK(width == 4 || width == 8);
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
    CHECK(width == 4 || width == 8);
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

void writeByteAt(int fd, std::uint64_t offset, std::uint8_t value) {
    CHECK(::pwrite(
              fd,
              &value,
              1,
              static_cast<off_t>(offset)) == 1);
}

std::vector<std::uint8_t> readBytesAt(
    int fd,
    std::uint64_t offset,
    std::size_t size) {
    std::vector<std::uint8_t> result(size);
    CHECK(::pread(
              fd,
              result.data(),
              result.size(),
              static_cast<off_t>(offset)) ==
          static_cast<ssize_t>(result.size()));
    return result;
}

std::uint64_t loadLittleEndian(
    const std::uint8_t* bytes,
    std::size_t width) {
    CHECK(width == 4 || width == 8);
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < width; ++index) {
        result |= static_cast<std::uint64_t>(bytes[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return result;
}

std::uint64_t findEngineHeader(
    int fd,
    const std::array<std::uint8_t, 8>& magic) {
    struct stat status {};
    CHECK(::fstat(fd, &status) == 0);
    CHECK(status.st_size > 0);
    void* mapping = ::mmap(
        nullptr,
        static_cast<std::size_t>(status.st_size),
        PROT_READ,
        MAP_SHARED,
        fd,
        0);
    CHECK(mapping != MAP_FAILED);
    const auto* begin = static_cast<const std::uint8_t*>(mapping);
    const auto* end = begin + static_cast<std::size_t>(status.st_size);
    const auto* found = std::search(
        begin, end, magic.begin(), magic.end());
    CHECK(found != end);
    const auto offset = static_cast<std::uint64_t>(found - begin);
    CHECK(::munmap(mapping, static_cast<std::size_t>(status.st_size)) == 0);
    return offset;
}

std::uint64_t findTrieBoxHeader(int fd) {
    const std::array<std::uint8_t, 8> magic = {
        'K', 'V', 'T', 'R', 'I', 'E', '0', '1'};
    return findEngineHeader(fd, magic);
}

std::uint64_t findArtBoxHeader(int fd) {
    const std::array<std::uint8_t, 8> magic = {
        'K', 'V', 'A', 'R', 'T', 'B', '0', '1'};
    return findEngineHeader(fd, magic);
}

std::uint64_t findHashBoxHeader(int fd) {
    const std::array<std::uint8_t, 8> magic = {
        'K', 'V', 'H', 'B', 'O', 'X', '0', '1'};
    return findEngineHeader(fd, magic);
}

std::uint64_t findArtBumpHeader(int fd) {
    const std::array<std::uint8_t, 8> magic = {
        'K', 'V', 'A', 'B', 'U', 'M', 'P', '1'};
    return findEngineHeader(fd, magic);
}

struct ArtBumpDiskAuthority {
    std::uint64_t root = 0;
    std::uint64_t generation = 0;
    std::uint32_t active_zone = 0;
    std::uint32_t journal_state = 0;
    std::array<std::uint64_t, 2> raw_begin{};
    std::array<std::uint64_t, 2> raw_top{};
};

ArtBumpDiskAuthority readArtBumpAuthority(const std::string& name) {
    const int fd = ::shm_open(name.c_str(), O_RDONLY | O_CLOEXEC, 0);
    CHECK(fd >= 0);
    const auto header = findArtBumpHeader(fd);
    ArtBumpDiskAuthority result;
    result.root = readLittleEndianAt(fd, header + 16U, 8);
    result.generation = readLittleEndianAt(fd, 1344U, 8);
    result.active_zone = static_cast<std::uint32_t>(
        readLittleEndianAt(fd, header + 24U, 4));
    result.journal_state = static_cast<std::uint32_t>(
        readLittleEndianAt(fd, header + 256U + 80U, 4));
    for (std::size_t index = 0; index < result.raw_begin.size(); ++index) {
        const auto descriptor = header + 160U + index * 32U;
        result.raw_begin[index] = readLittleEndianAt(fd, descriptor, 8);
        result.raw_top[index] = readLittleEndianAt(fd, descriptor + 16U, 8);
    }
    CHECK(::close(fd) == 0);
    return result;
}

std::vector<std::uint8_t> readArtBumpRegionIgnoringRobustMutex(
    const std::string& name) {
    const int fd = ::shm_open(name.c_str(), O_RDONLY | O_CLOEXEC, 0);
    CHECK(fd >= 0);
    struct stat status {};
    CHECK(::fstat(fd, &status) == 0);
    CHECK(status.st_size >= 1408);
    const auto size = static_cast<std::size_t>(status.st_size);
    auto result = readBytesAt(fd, 0, size);
    CHECK(::close(fd) == 0);

    // A process killed while owning a POSIX robust mutex necessarily changes
    // that implementation-owned 40-byte object. COMMON04 fixes it at
    // [1368,1408); every byte outside that unavoidable owner-death state is
    // still required to match the pre-Compact image exactly.
    std::fill(result.begin() + 1368, result.begin() + 1408, 0);
    return result;
}

std::uint64_t firstCommittedTrieBoxValueReference(
    int fd,
    std::uint64_t trie_header) {
    constexpr std::size_t node_bytes = 1032;
    const auto root = static_cast<std::uint32_t>(
        readLittleEndianAt(fd, trie_header + 16U, 4));
    const auto node_metadata = readLittleEndianAt(
        fd, trie_header + 24U, 8);
    const auto node_zone = readLittleEndianAt(
        fd, trie_header + 40U, 8);
    const auto capacity = static_cast<std::uint32_t>(
        readLittleEndianAt(fd, node_metadata + 32U, 4));
    const auto width = static_cast<std::size_t>(
        readBytesAt(fd, node_metadata + 56U, 1).front());
    CHECK(width == 2 || width == 4 || width == 8);
    const auto stride = node_bytes + width;
    std::vector<std::uint8_t> seen(
        (static_cast<std::size_t>(capacity) + 7U) / 8U, 0);
    std::vector<std::uint32_t> stack{root};
    while (!stack.empty()) {
        const auto id = stack.back();
        stack.pop_back();
        CHECK(id < capacity);
        const auto byte = static_cast<std::size_t>(id) / 8U;
        const auto bit = static_cast<std::uint8_t>(
            1U << static_cast<unsigned>(id % 8U));
        CHECK((seen[byte] & bit) == 0);
        seen[byte] |= bit;
        const auto payload = readBytesAt(
            fd,
            node_zone + static_cast<std::uint64_t>(id) * stride + width,
            node_bytes);
        const auto packed = loadLittleEndian(payload.data(), 8);
        if ((packed & 1U) != 0 && (packed >> 1U) != 0) {
            return packed >> 1U;
        }
        for (std::size_t slot = 0; slot < 256; ++slot) {
            const auto child = static_cast<std::uint32_t>(loadLittleEndian(
                payload.data() + 8U + slot * sizeof(std::uint32_t), 4));
            if (child != UINT32_MAX) stack.push_back(child);
        }
    }
    CHECK(false);
    return 0;
}

void testTrieBoxRecoveryPreflightDoesNotWriteEitherAllocator(
    const char* self) {
    ScopedRegion scoped(uniqueShmName("trie-box-preflight-strong"));
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::TrieBox;
    options.initial_size = 16ULL * 1024U * 1024U;
    options.max_size = options.initial_size;
    options.max_entries = 64;
    options.max_queues = 8;
    auto store = kvspace::ShmClient::Create(scoped.name(), options);
    store->Set("/kept", kvspace::XValue::Str("committed"));

    requireExitCode(execChild(self, scoped.name(), "lock-and-die"), 73);
    const int fd = ::shm_open(
        scoped.name().c_str(), O_RDWR | O_CLOEXEC, 0);
    CHECK(fd >= 0);
    const auto trie_header = findTrieBoxHeader(fd);
    const auto node_metadata = readLittleEndianAt(
        fd, trie_header + 24U, 8);
    const auto node_metadata_bytes = static_cast<std::size_t>(
        readLittleEndianAt(fd, trie_header + 32U, 8));
    const auto box_metadata = readLittleEndianAt(
        fd, trie_header + 56U, 8);
    const auto box_metadata_bytes = static_cast<std::size_t>(
        readLittleEndianAt(fd, trie_header + 64U, 8));
    const auto box_data = readLittleEndianAt(
        fd, trie_header + 72U, 8);
    const auto value_reference = firstCommittedTrieBoxValueReference(
        fd, trie_header);

    // Make the node allocator's mutable state visibly noncanonical. Recovery
    // must not repair it until the invalid committed Box value has passed its
    // complete raw-TLV and interval preflight.
    writeLittleEndianAt(fd, node_metadata + 36U, UINT32_MAX, 4);
    writeLittleEndianAt(fd, node_metadata + 40U, UINT32_MAX, 4);
    writeByteAt(fd, box_data + value_reference - 1U, 0);
    const auto node_before = readBytesAt(
        fd, node_metadata, node_metadata_bytes);
    const auto box_before = readBytesAt(
        fd, box_metadata, box_metadata_bytes);

    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(store->Get("/kept"));
    });
    CHECK(readBytesAt(fd, node_metadata, node_metadata_bytes) == node_before);
    CHECK(readBytesAt(fd, box_metadata, box_metadata_bytes) == box_before);
    CHECK(::close(fd) == 0);
}

void testHashBoxRecoveryPreflightDoesNotWriteBoxAllocator(
    const char* self) {
    ScopedRegion scoped(uniqueShmName("hash-box-preflight-strong"));
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::HashBox;
    options.initial_size = 16ULL * 1024U * 1024U;
    options.max_size = options.initial_size;
    options.max_entries = 64;
    options.max_queues = 8;
    auto store = kvspace::ShmClient::Create(scoped.name(), options);
    store->Set("/kept", kvspace::XValue::Str("committed"));

    requireExitCode(execChild(self, scoped.name(), "lock-and-die"), 73);
    const int fd = ::shm_open(
        scoped.name().c_str(), O_RDWR | O_CLOEXEC, 0);
    CHECK(fd >= 0);
    const auto header = findHashBoxHeader(fd);
    const auto box_metadata = readLittleEndianAt(fd, header + 16U, 8);
    const auto box_metadata_bytes = static_cast<std::size_t>(
        readLittleEndianAt(fd, header + 24U, 8));
    const auto box_data = readLittleEndianAt(fd, header + 32U, 8);
    const auto table_capacity = readLittleEndianAt(fd, 80U, 8);
    const auto selected = static_cast<std::uint32_t>(
        readLittleEndianAt(fd, 1360U, 4));
    CHECK(selected <= 1U);
    const auto table = readLittleEndianAt(
        fd, selected == 0 ? 88U : 96U, 8);
    std::uint64_t value_ref = 0;
    for (std::uint64_t index = 0; index < table_capacity; ++index) {
        const auto slot = readBytesAt(fd, table + index * 32U, 32);
        if (loadLittleEndian(slot.data() + 28U, 4) != 1U) continue;
        const auto candidate = loadLittleEndian(slot.data() + 16U, 8);
        if (candidate != 0) {
            value_ref = candidate;
            break;
        }
    }
    CHECK(value_ref != 0);

    // Make the embedded FixedBlock dynamic counters visibly invalid. The
    // malformed selected TLV must be rejected before PreparedRebuild applies
    // or any Box allocator byte is repaired.
    writeLittleEndianAt(fd, box_metadata + 128U + 36U, UINT32_MAX, 4);
    writeLittleEndianAt(fd, box_metadata + 128U + 40U, UINT32_MAX, 4);
    writeByteAt(fd, box_data + value_ref - 1U, 0);
    const auto box_before = readBytesAt(
        fd, box_metadata, box_metadata_bytes);

    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(store->Get("/kept"));
    });
    CHECK(readBytesAt(fd, box_metadata, box_metadata_bytes) == box_before);
    CHECK(::close(fd) == 0);
}

void testArtBoxSecondPlanFailureDoesNotWriteAnyEngineAllocator(
    const char* self) {
    ScopedRegion scoped(uniqueShmName("art-box-second-plan-strong"));
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::ArtBox;
    options.initial_size = 16ULL * 1024U * 1024U;
    options.max_size = options.initial_size;
    options.max_entries = 64;
    options.max_queues = 8;
    auto region = kvspace::detail::Region::Open(
        scoped.name(), options, kvspace::detail::OpenMode::Create);
    const auto encoded = kvspace::XValue::Int64(77).Encode();
    {
        auto guard = region->Lock();
        region->Put("", encoded);
        (void)guard;
    }

    requireExitCode(execChild(self, scoped.name(), "lock-and-die"), 73);
    const int fd = ::shm_open(
        scoped.name().c_str(), O_RDWR | O_CLOEXEC, 0);
    CHECK(fd >= 0);
    const auto header = findArtBoxHeader(fd);
    const auto root = static_cast<std::uint32_t>(
        readLittleEndianAt(fd, header + 16U, 4));
    CHECK(root != UINT32_MAX);
    const auto kind_index = static_cast<std::size_t>(root >> 30U);
    const auto local_id = root & UINT32_C(0x3fffffff);
    CHECK(kind_index < 4);
    const auto header_width = static_cast<std::uint64_t>(
        readBytesAt(fd, header + 196U, 1).front());
    const auto payload_bytes = readLittleEndianAt(
        fd, header + 200U + kind_index * 4U, 4);
    const auto root_descriptor = header + 24U + kind_index * 32U;
    const auto root_metadata = readLittleEndianAt(
        fd, root_descriptor, 8);
    const auto root_zone = readLittleEndianAt(
        fd, root_descriptor + 16U, 8);
    const auto root_payload = root_zone +
        static_cast<std::uint64_t>(local_id) *
            (header_width + payload_bytes) +
        header_width;
    CHECK(readLittleEndianAt(fd, root_payload + 8U, 8) == 1);
    const auto box_data = readLittleEndianAt(fd, header + 168U, 8);
    const auto box_metadata = readLittleEndianAt(fd, header + 152U, 8);
    const auto box_metadata_bytes = static_cast<std::size_t>(
        readLittleEndianAt(fd, header + 160U, 8));

    // The raw recovery inspection accepts this canonical TLV at offset one,
    // but the second prepared plan rejects the noncanonical Box alignment.
    // Thus node planning has completed while owner/allocator writes remain
    // forbidden.
    const auto value_bytes = readBytesAt(fd, box_data, encoded.size());
    CHECK(::pwrite(
              fd,
              value_bytes.data(),
              value_bytes.size(),
              static_cast<off_t>(box_data + 1U)) ==
          static_cast<ssize_t>(value_bytes.size()));
    writeLittleEndianAt(fd, root_payload + 8U, 2, 8);
    writeLittleEndianAt(fd, root_metadata + 36U, UINT32_MAX, 4);
    writeLittleEndianAt(fd, root_metadata + 40U, UINT32_MAX, 4);

    std::array<std::vector<std::uint8_t>, 4> metadata_before;
    std::array<std::vector<std::uint8_t>, 4> zones_before;
    std::array<std::uint64_t, 4> metadata_offsets{};
    std::array<std::uint64_t, 4> zone_offsets{};
    std::array<std::size_t, 4> zone_sizes{};
    for (std::size_t index = 0; index < 4; ++index) {
        const auto descriptor = header + 24U + index * 32U;
        metadata_offsets[index] = readLittleEndianAt(fd, descriptor, 8);
        const auto metadata_size = static_cast<std::size_t>(
            readLittleEndianAt(fd, descriptor + 8U, 8));
        zone_offsets[index] = readLittleEndianAt(fd, descriptor + 16U, 8);
        zone_sizes[index] = static_cast<std::size_t>(
            readLittleEndianAt(fd, descriptor + 24U, 8));
        metadata_before[index] = readBytesAt(
            fd, metadata_offsets[index], metadata_size);
        zones_before[index] = readBytesAt(
            fd, zone_offsets[index], zone_sizes[index]);
    }
    const auto box_before = readBytesAt(
        fd, box_metadata, box_metadata_bytes);

    expectThrows<kvspace::ErrCorruptRegion>([&] {
        auto guard = region->Lock();
        (void)guard;
    });
    for (std::size_t index = 0; index < 4; ++index) {
        CHECK(readBytesAt(
                  fd,
                  metadata_offsets[index],
                  metadata_before[index].size()) == metadata_before[index]);
        CHECK(readBytesAt(fd, zone_offsets[index], zone_sizes[index]) ==
              zones_before[index]);
    }
    CHECK(readBytesAt(fd, box_metadata, box_metadata_bytes) == box_before);
    CHECK(::close(fd) == 0);
}

struct RawQueueReference {
    std::uint64_t slot = 0;
    std::uint64_t key = 0;
    std::uint64_t head = 0;
};

RawQueueReference occupiedQueueReference(int fd) {
    const auto capacity = readLittleEndianAt(
        fd,
        kvspace::detail::RegionQueueCapacityOffsetForTest(),
        8);
    const auto table = readLittleEndianAt(
        fd,
        kvspace::detail::RegionQueueTableOffsetOffsetForTest(),
        8);
    CHECK(capacity > 0);
    for (std::uint64_t index = 0; index < capacity; ++index) {
        const auto slot = table + index * 32U;
        if (readLittleEndianAt(fd, slot + 28U, 4) != 1) continue;
        const auto key = readLittleEndianAt(fd, slot + 8U, 8);
        const auto head = readLittleEndianAt(fd, slot + 16U, 8);
        CHECK(key != 0 && head != 0);
        return {slot, key, head};
    }
    CHECK(false);
    return {};
}

void testAllocatorCoalesceJournalRejectsCorruption(const char* self) {
    constexpr std::uint64_t coalesce_salt = 0x4b564d4552474531ULL;
    for (int corruption = 0; corruption < 6; ++corruption) {
        ScopedRegion scoped(uniqueShmName("allocator-coalesce-corrupt"));
        kvspace::ShmOptions options;
        options.engine = kvspace::ShmEngine::TrieBox;
        auto store = kvspace::ShmClient::Create(scoped.name(), options);
        prepareInteriorCoalesceRun(store.get());
        requireExitCode(
            execChild(self, scoped.name(), "lock-and-die"), 73);
        requireExitCode(
            recoveryChild(
                self,
                scoped.name(),
                kvspace::detail::RegionTestEvent::
                    AllocatorCoalesceJournalPublished),
            77);

        const int fd = ::shm_open(
            scoped.name().c_str(), O_RDWR | O_CLOEXEC, 0);
        CHECK(fd >= 0);
        const auto journal = static_cast<std::uint64_t>(
            kvspace::detail::RegionAllocatorJournalOffsetForTest());
        const auto source = readLittleEndianAt(fd, journal, 8);
        const auto left_span = readLittleEndianAt(fd, journal + 8U, 8);
        const auto right_span = readLittleEndianAt(fd, journal + 16U, 8);
        switch (corruption) {
        case 0:
            writeLittleEndianAt(fd, journal + 32U, 99, 4);
            break;
        case 1:
            writeLittleEndianAt(
                fd,
                journal + 24U,
                readLittleEndianAt(fd, journal + 24U, 8) ^ 1U,
                8);
            break;
        case 2:
            writeLittleEndianAt(fd, journal + 36U, 2, 4);
            break;
        case 3:
            writeLittleEndianAt(fd, journal + 40U, 1, 8);
            break;
        case 4:
            writeLittleEndianAt(fd, journal, 0, 8);
            writeLittleEndianAt(
                fd,
                journal + 24U,
                coalesce_salt ^ left_span ^ right_span,
                8);
            break;
        case 5:
            writeLittleEndianAt(
                fd, source + left_span + 36U, 0, 4);
            break;
        default:
            CHECK(false);
        }
        CHECK(::close(fd) == 0);
        expectThrows<kvspace::ErrCorruptRegion>([&] {
            static_cast<void>(store->Get("/keep"));
        });
    }
}

void testAllocatorSplitRejectsSubminimumSpan(const char* self) {
    constexpr std::uint64_t split_salt = 0x4b5653504c495431ULL;
    ScopedRegion scoped(uniqueShmName("allocator-split-minimum"));
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::TrieBox;
    auto owner = kvspace::detail::Region::Open(
        scoped.name(), options, kvspace::detail::OpenMode::Create);
    {
        auto guard = owner->Lock();
        const auto expected = kvspace::XValue::Bytes(
            std::vector<std::uint8_t>(8192, 3)).Encode();
        owner->Notify("large", expected);
        std::vector<std::uint8_t> notification;
        CHECK(owner->Watch(
            guard, "large", 5ms, &notification, [] { return false; }));
        CHECK(notification == expected);
        (void)guard;
    }

    const auto mode = "allocator-split-die-" + std::to_string(
        static_cast<std::uint32_t>(
            kvspace::detail::RegionTestEvent::
                AllocatorSplitJournalPublished));
    requireExitCode(execChild(self, scoped.name(), mode.c_str()), 77);

    const int fd = ::shm_open(
        scoped.name().c_str(), O_RDWR | O_CLOEXEC, 0);
    CHECK(fd >= 0);
    const auto journal = static_cast<std::uint64_t>(
        kvspace::detail::RegionAllocatorJournalOffsetForTest());
    const auto source = readLittleEndianAt(fd, journal, 8);
    const auto original = readLittleEndianAt(fd, journal + 8U, 8);
    const auto free_class = readLittleEndianAt(fd, journal + 36U, 4);
    CHECK(readLittleEndianAt(fd, source, 8) == original);
    constexpr std::uint64_t invalid_requested = 48;
    writeLittleEndianAt(fd, journal + 16U, invalid_requested, 8);
    writeLittleEndianAt(
        fd,
        journal + 24U,
        split_salt ^ source ^ original ^ invalid_requested ^
            (free_class << 56U),
        8);
    CHECK(::close(fd) == 0);

    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(owner->Lock());
    });
    const int verify_fd = ::shm_open(
        scoped.name().c_str(), O_RDONLY | O_CLOEXEC, 0);
    CHECK(verify_fd >= 0);
    CHECK(readLittleEndianAt(verify_fd, source, 8) == original);
    CHECK(::close(verify_fd) == 0);
}

void testQueueRejectsNonGenericBlobFlags(const char* self) {
    for (const bool corrupt_message : {false, true}) {
        ScopedRegion scoped(uniqueShmName("queue-flags-corrupt"));
        kvspace::ShmOptions options;
        options.engine = kvspace::ShmEngine::TrieBox;
        auto store = kvspace::ShmClient::Create(scoped.name(), options);
        store->Notify("/queue", kvspace::XValue::Int64(1));
        requireExitCode(
            execChild(self, scoped.name(), "lock-and-die"), 73);

        const int fd = ::shm_open(
            scoped.name().c_str(), O_RDWR | O_CLOEXEC, 0);
        CHECK(fd >= 0);
        const auto queue = occupiedQueueReference(fd);
        writeByteAt(
            fd,
            (corrupt_message ? queue.head : queue.key) + 40U,
            3);
        CHECK(::close(fd) == 0);
        expectThrows<kvspace::ErrCorruptRegion>([&] {
            static_cast<void>(store->Get("/unrelated"));
        });
    }
}

void testMalformedQueueValueIsNeverConsumed(const char* self) {
    for (const bool recover_first : {false, true}) {
        ScopedRegion scoped(uniqueShmName("queue-value-corrupt"));
        kvspace::ShmOptions options;
        options.engine = kvspace::ShmEngine::TrieBox;
        auto store = kvspace::ShmClient::Create(scoped.name(), options);
        store->Notify("/queue", kvspace::XValue::Int64(1));
        if (recover_first) {
            requireExitCode(
                execChild(self, scoped.name(), "lock-and-die"), 73);
        }

        int fd = ::shm_open(
            scoped.name().c_str(), O_RDWR | O_CLOEXEC, 0);
        CHECK(fd >= 0);
        const auto queue = occupiedQueueReference(fd);
        writeByteAt(fd, queue.head + 64U, 0);
        CHECK(::close(fd) == 0);

        expectThrows<kvspace::ErrCorruptRegion>([&] {
            static_cast<void>(store->Watch("/queue", 5ms));
        });
        fd = ::shm_open(
            scoped.name().c_str(), O_RDONLY | O_CLOEXEC, 0);
        CHECK(fd >= 0);
        CHECK(readLittleEndianAt(fd, queue.slot + 16U, 8) == queue.head);
        CHECK(::close(fd) == 0);
    }
}

pid_t artBumpCutChild(
    const char* self,
    const std::string& name,
    const char* operation,
    kvspace::detail::ArtBumpApplyStep step,
    int ordinal = -1) {
    auto mode = std::string("art-bump-") + operation + "-die-" +
        std::to_string(static_cast<std::uint32_t>(step));
    if (ordinal >= 0) mode += "-" + std::to_string(ordinal);
    return execChild(self, name, mode.c_str());
}

kvspace::ShmOptions artBumpProcessOptions(std::uint64_t max_entries = 256) {
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::ArtBump;
    options.initial_size = 16ULL * 1024U * 1024U;
    options.max_size = options.initial_size;
    options.max_entries = max_entries;
    options.max_queues = 8;
    return options;
}

std::unique_ptr<kvspace::ShmClient> createArtBumpFixture(
    const std::string& name) {
    auto owner = kvspace::ShmClient::Create(
        name, artBumpProcessOptions());
    owner->Set(
        "/authority", kvspace::XValue::Str(std::string(2048, 'o')));
    owner->Set("/base/a", kvspace::XValue::Int64(1));
    owner->Set("/base/b", kvspace::XValue::Int64(2));
    owner->Set("/target/value", kvspace::XValue::Int64(7));
    owner->Notify("/compact-queue", kvspace::XValue::Int64(1));
    owner->Notify("/compact-queue", kvspace::XValue::Int64(2));
    return owner;
}

void verifyArtBumpFixture(kvspace::ShmClient* recovered) {
    CHECK(recovered->Get("/base/a").AsInt64() == 1);
    CHECK(recovered->Get("/base/b").AsInt64() == 2);
    CHECK(recovered->Get("/target/value").AsInt64() == 7);
    CHECK(recovered->List("/base/") ==
          std::vector<std::string>({"a", "b"}));
    CHECK(recovered->Watch("/compact-queue", 5ms).AsInt64() == 2);
    CHECK(recovered->Watch("/compact-queue", 5ms).AsInt64() == 1);
    recovered->Set("/after-art-bump-recovery", kvspace::XValue::Int64(91));
    CHECK(recovered->Get("/after-art-bump-recovery").AsInt64() == 91);
    CHECK(recovered->Stats().recoveries >= 1);
}

void requireArtBumpCompactAuthority(
    const ArtBumpDiskAuthority& before,
    const ArtBumpDiskAuthority& after,
    bool roll_forward) {
    CHECK(after.journal_state == 0);
    if (roll_forward) {
        CHECK(after.root != before.root);
        CHECK(after.active_zone == 1U - before.active_zone);
        CHECK(after.generation >= before.generation + 1U);
    } else {
        CHECK(after.root == before.root);
        CHECK(after.active_zone == before.active_zone);
        CHECK(after.generation == before.generation);
    }
}

void testOwnerDeathAtArtBumpMutationCutPoints(const char* self) {
    struct Cut {
        kvspace::detail::ArtBumpApplyStep step;
        bool publishes_new_root;
    };
    for (const auto cut : {
             Cut{kvspace::detail::ArtBumpApplyStep::MutationRawTopPublished,
                 false},
             Cut{kvspace::detail::ArtBumpApplyStep::MutationNodeWritten,
                 false},
             Cut{kvspace::detail::ArtBumpApplyStep::MutationCountersStored,
                 false},
             Cut{kvspace::detail::ArtBumpApplyStep::MutationGenerationStored,
                 false},
             Cut{kvspace::detail::ArtBumpApplyStep::MutationRootPublished,
                 true},
             Cut{kvspace::detail::ArtBumpApplyStep::MutationNodeReclaimed,
                 true}}) {
        ScopedRegion scoped(uniqueShmName("art-bump-mutation-cut"));
        auto owner = createArtBumpFixture(scoped.name());
        const auto before = readArtBumpAuthority(scoped.name());
        CHECK(before.root != 0);
        CHECK(before.journal_state == 0);
        owner.reset();

        requireExitCode(
            artBumpCutChild(
                self, scoped.name(), "mutation", cut.step),
            80);

        auto recovered = kvspace::ShmClient::Attach(
            scoped.name(), kvspace::ShmEngine::ArtBump);
        const auto after = readArtBumpAuthority(scoped.name());
        CHECK(after.journal_state == 0);
        CHECK(after.active_zone == before.active_zone);
        CHECK((after.root != before.root) == cut.publishes_new_root);
        CHECK(recovered->Get("/authority").AsStr() ==
              std::string(2048, cut.publishes_new_root ? 'n' : 'o'));
        verifyArtBumpFixture(recovered.get());
    }
}

void testArtBumpCompactBeforeJournalIsByteStable(const char* self) {
    ScopedRegion scoped(uniqueShmName("art-bump-before-journal-cut"));
    auto owner = createArtBumpFixture(scoped.name());
    const auto authority_before = readArtBumpAuthority(scoped.name());
    owner.reset();
    const auto bytes_before =
        readArtBumpRegionIgnoringRobustMutex(scoped.name());

    requireExitCode(
        artBumpCutChild(
            self,
            scoped.name(),
            "compact",
            kvspace::detail::ArtBumpApplyStep::
                CompactBeforeJournalPayload),
        80);
    CHECK(readArtBumpRegionIgnoringRobustMutex(scoped.name()) ==
          bytes_before);

    auto recovered = kvspace::ShmClient::Attach(
        scoped.name(), kvspace::ShmEngine::ArtBump);
    const auto authority_after = readArtBumpAuthority(scoped.name());
    CHECK(authority_after.root == authority_before.root);
    CHECK(authority_after.active_zone == authority_before.active_zone);
    CHECK(authority_after.journal_state == 0);
    CHECK(recovered->Get("/authority").AsStr() == std::string(2048, 'o'));
    verifyArtBumpFixture(recovered.get());
}

void testOwnerDeathAtArtBumpCompactCutPoints(const char* self) {
    struct Cut {
        kvspace::detail::ArtBumpApplyStep step;
        bool roll_forward;
    };
    for (const auto cut : {
             Cut{kvspace::detail::ArtBumpApplyStep::CompactCopyingPublished,
                 false},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactRawCopied, false},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactNodeCloned, false},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactReadyPublished,
                 false},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactActiveStored, false},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactGenerationStored,
                 false},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactRootPublished, true},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactSlabsRebuilt, true},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactSourceTopStored,
                 true},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactIdlePublished,
                 true}}) {
        ScopedRegion scoped(uniqueShmName("art-bump-compact-cut"));
        auto owner = createArtBumpFixture(scoped.name());
        const auto before = readArtBumpAuthority(scoped.name());
        CHECK(before.root != 0);
        CHECK(before.journal_state == 0);
        owner.reset();

        requireExitCode(
            artBumpCutChild(self, scoped.name(), "compact", cut.step),
            80);

        auto recovered = kvspace::ShmClient::Attach(
            scoped.name(), kvspace::ShmEngine::ArtBump);
        const auto after = readArtBumpAuthority(scoped.name());
        requireArtBumpCompactAuthority(before, after, cut.roll_forward);
        CHECK(recovered->Get("/authority").AsStr() ==
              std::string(2048, 'o'));
        verifyArtBumpFixture(recovered.get());
    }
}

void testOwnerDeathAtArtBumpReadyFieldCuts(const char* self) {
    for (int field = 0; field < 6; ++field) {
        ScopedRegion scoped(uniqueShmName("art-bump-ready-field-cut"));
        auto owner = createArtBumpFixture(scoped.name());
        const auto before = readArtBumpAuthority(scoped.name());
        owner.reset();

        // READY is not yet published, so every partial ready-only payload must
        // remain COPYING and select the old root. Kill the first recovery once
        // more after it resets target top, then require a third owner to
        // repeat the same rollback without inspecting the clone.
        requireExitCode(
            artBumpCutChild(
                self,
                scoped.name(),
                "compact",
                kvspace::detail::ArtBumpApplyStep::
                    CompactReadyFieldStored,
                field),
            80);
        requireExitCode(
            artBumpCutChild(
                self,
                scoped.name(),
                "recovery",
                kvspace::detail::ArtBumpApplyStep::
                    RecoveryRollbackTargetTopStored),
            80);

        auto recovered = kvspace::ShmClient::Attach(
            scoped.name(), kvspace::ShmEngine::ArtBump);
        const auto after = readArtBumpAuthority(scoped.name());
        requireArtBumpCompactAuthority(before, after, false);
        CHECK(recovered->Get("/authority").AsStr() ==
              std::string(2048, 'o'));
        verifyArtBumpFixture(recovered.get());
    }
}

void testOwnerDeathAtArtBumpCompactSlabWriteCuts(const char* self) {
    constexpr int rebuild_cut_count = 4 * 3;
    for (int ordinal = 0; ordinal < rebuild_cut_count; ++ordinal) {
        ScopedRegion scoped(uniqueShmName("art-bump-compact-slab-cut"));
        auto owner = createArtBumpFixture(scoped.name());
        const auto before = readArtBumpAuthority(scoped.name());
        owner.reset();

        // The compact root is already published at every slab write. Kill the
        // corresponding first/middle/final write in roll-forward recovery as
        // well; the third owner must still choose only the new root.
        requireExitCode(
            artBumpCutChild(
                self,
                scoped.name(),
                "compact",
                kvspace::detail::ArtBumpApplyStep::
                    CompactSlabRebuildWrite,
                ordinal),
            80);
        requireExitCode(
            artBumpCutChild(
                self,
                scoped.name(),
                "recovery",
                kvspace::detail::ArtBumpApplyStep::
                    RecoveryForwardSlabRebuildWrite,
                ordinal),
            80);

        auto recovered = kvspace::ShmClient::Attach(
            scoped.name(), kvspace::ShmEngine::ArtBump);
        const auto after = readArtBumpAuthority(scoped.name());
        requireArtBumpCompactAuthority(before, after, true);
        CHECK(recovered->Get("/authority").AsStr() ==
              std::string(2048, 'o'));
        verifyArtBumpFixture(recovered.get());
    }
}

void testOwnerDeathAtArtBumpRollbackSlabWriteCuts(const char* self) {
    constexpr int rebuild_cut_count = 4 * 3;
    for (int ordinal = 0; ordinal < rebuild_cut_count; ++ordinal) {
        ScopedRegion scoped(uniqueShmName("art-bump-rollback-slab-cut"));
        auto owner = createArtBumpFixture(scoped.name());
        const auto before = readArtBumpAuthority(scoped.name());
        owner.reset();

        requireExitCode(
            artBumpCutChild(
                self,
                scoped.name(),
                "compact",
                kvspace::detail::ArtBumpApplyStep::CompactReadyPublished),
            80);
        requireExitCode(
            artBumpCutChild(
                self,
                scoped.name(),
                "recovery",
                kvspace::detail::ArtBumpApplyStep::
                    RecoveryRollbackSlabRebuildWrite,
                ordinal),
            80);

        auto recovered = kvspace::ShmClient::Attach(
            scoped.name(), kvspace::ShmEngine::ArtBump);
        const auto after = readArtBumpAuthority(scoped.name());
        requireArtBumpCompactAuthority(before, after, false);
        CHECK(recovered->Get("/authority").AsStr() ==
              std::string(2048, 'o'));
        verifyArtBumpFixture(recovered.get());
    }
}

void testOwnerDeathAtArtBumpIdleRecoveryWriteCuts(const char* self) {
    struct RecoveryCut {
        kvspace::detail::ArtBumpApplyStep step;
        int ordinal;
    };
    std::vector<RecoveryCut> cuts = {
        {kvspace::detail::ArtBumpApplyStep::RecoveryIdleZoneTopStored, 0},
        {kvspace::detail::ArtBumpApplyStep::RecoveryIdleZoneTopStored, 1},
        {kvspace::detail::ArtBumpApplyStep::RecoveryIdleCountersStored, -1},
    };
    for (int ordinal = 0; ordinal < 4 * 3; ++ordinal) {
        cuts.push_back({
            kvspace::detail::ArtBumpApplyStep::
                RecoveryIdleSlabRebuildWrite,
            ordinal});
    }

    for (const auto cut : cuts) {
        ScopedRegion scoped(uniqueShmName("art-bump-idle-recovery-cut"));
        auto owner = createArtBumpFixture(scoped.name());
        const auto before = readArtBumpAuthority(scoped.name());
        owner.reset();

        // This update has advanced raw top but has not published its root.
        requireExitCode(
            artBumpCutChild(
                self,
                scoped.name(),
                "mutation",
                kvspace::detail::ArtBumpApplyStep::MutationRawTopPublished),
            80);
        requireExitCode(
            artBumpCutChild(
                self,
                scoped.name(),
                "recovery",
                cut.step,
                cut.ordinal),
            80);

        auto recovered = kvspace::ShmClient::Attach(
            scoped.name(), kvspace::ShmEngine::ArtBump);
        const auto after = readArtBumpAuthority(scoped.name());
        CHECK(after.journal_state == 0);
        CHECK(after.root == before.root);
        CHECK(after.active_zone == before.active_zone);
        CHECK(after.raw_top == before.raw_top);
        CHECK(recovered->Get("/authority").AsStr() ==
              std::string(2048, 'o'));
        verifyArtBumpFixture(recovered.get());
    }
}

void testOwnerDeathDuringEmptyArtBumpCompact(const char* self) {
    struct Cut {
        kvspace::detail::ArtBumpApplyStep step;
        bool roll_forward;
    };
    for (const auto cut : {
             Cut{kvspace::detail::ArtBumpApplyStep::CompactCopyingPublished,
                 false},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactTargetTopStored,
                 false},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactReadyPublished,
                 true},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactActiveStored, true},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactRootPublished, true},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactSourceTopStored,
                 true},
             Cut{kvspace::detail::ArtBumpApplyStep::CompactIdlePublished,
                 true}}) {
        ScopedRegion scoped(uniqueShmName("empty-compact-cut"));
        auto options = artBumpProcessOptions(64);
        auto owner = kvspace::detail::Region::Open(
            scoped.name(), options, kvspace::detail::OpenMode::Create);
        {
            auto guard = owner->Lock();
            owner->Put(
                "garbage",
                kvspace::XValue::Bytes(
                    std::vector<std::uint8_t>(4096, 3)).Encode());
            CHECK(owner->Erase("garbage"));
            CHECK(owner->Stats().entries == 0);
            (void)guard;
        }
        const auto heap_used_before = owner->Stats().heap_used;
        CHECK(heap_used_before > 0);
        const auto before = readArtBumpAuthority(scoped.name());
        CHECK(before.root == 0);
        CHECK(before.journal_state == 0);
        owner.reset();

        requireExitCode(
            artBumpCutChild(
                self, scoped.name(), "compact", cut.step),
            80);

        auto recovered = kvspace::ShmClient::Attach(
            scoped.name(), kvspace::ShmEngine::ArtBump);
        const auto after = readArtBumpAuthority(scoped.name());
        CHECK(after.root == 0);
        CHECK(after.journal_state == 0);
        if (cut.step ==
            kvspace::detail::ArtBumpApplyStep::CompactIdlePublished) {
            // The journal was already IDLE when the owner died. Empty-IDLE
            // recovery canonicalizes both zones and selects zone zero.
            CHECK(after.active_zone == 0);
            CHECK(after.generation >= before.generation + 1U);
        } else if (cut.roll_forward) {
            CHECK(after.active_zone == 1U - before.active_zone);
            CHECK(after.generation >= before.generation + 1U);
        } else {
            CHECK(after.active_zone == before.active_zone);
            CHECK(after.generation == before.generation);
        }
        CHECK(recovered->Stats().entries == 0);
        CHECK(recovered->Stats().heap_used ==
              (cut.roll_forward ? 0 : heap_used_before));
        recovered->Set("/after-empty-compact", kvspace::XValue::Int64(17));
        CHECK(recovered->Get("/after-empty-compact").AsInt64() == 17);
        CHECK(recovered->Stats().recoveries >= 1);
    }
}

void testRepeatedOwnerDeathDuringArtBumpRecovery(const char* self) {
    struct Cut {
        kvspace::detail::ArtBumpApplyStep interrupted_compact_step;
        kvspace::detail::ArtBumpApplyStep interrupted_recovery_step;
        bool roll_forward;
    };
    for (const auto cut : {
             Cut{
                 kvspace::detail::ArtBumpApplyStep::CompactCopyingPublished,
                 kvspace::detail::ArtBumpApplyStep::
                     RecoveryRollbackSourceTopStored,
                 false},
             Cut{
                 kvspace::detail::ArtBumpApplyStep::CompactCopyingPublished,
                 kvspace::detail::ArtBumpApplyStep::
                     RecoveryRollbackRootStored,
                 false},
             Cut{
                 kvspace::detail::ArtBumpApplyStep::CompactReadyPublished,
                 kvspace::detail::ArtBumpApplyStep::
                     RecoveryRollbackSlabsRebuilt,
                 false},
             Cut{
                 kvspace::detail::ArtBumpApplyStep::CompactRootPublished,
                 kvspace::detail::ArtBumpApplyStep::
                     RecoveryForwardTargetTopStored,
                 true},
             Cut{
                 kvspace::detail::ArtBumpApplyStep::CompactRootPublished,
                 kvspace::detail::ArtBumpApplyStep::
                     RecoveryForwardRootStored,
                 true},
             Cut{
                 kvspace::detail::ArtBumpApplyStep::CompactRootPublished,
                 kvspace::detail::ArtBumpApplyStep::
                     RecoveryForwardSlabsRebuilt,
                 true},
             Cut{
                 kvspace::detail::ArtBumpApplyStep::CompactRootPublished,
                 kvspace::detail::ArtBumpApplyStep::
                     RecoveryForwardSourceTopStored,
                 true}}) {
        ScopedRegion scoped(uniqueShmName("art-bump-recovery-repeat"));
        auto owner = createArtBumpFixture(scoped.name());
        const auto before = readArtBumpAuthority(scoped.name());
        CHECK(before.root != 0);
        owner.reset();

        requireExitCode(
            artBumpCutChild(
                self,
                scoped.name(),
                "compact",
                cut.interrupted_compact_step),
            80);
        requireExitCode(
            artBumpCutChild(
                self,
                scoped.name(),
                "recovery",
                cut.interrupted_recovery_step),
            80);

        auto recovered = kvspace::ShmClient::Attach(
            scoped.name(), kvspace::ShmEngine::ArtBump);
        const auto after = readArtBumpAuthority(scoped.name());
        requireArtBumpCompactAuthority(before, after, cut.roll_forward);
        CHECK(recovered->Get("/authority").AsStr() ==
              std::string(2048, 'o'));
        verifyArtBumpFixture(recovered.get());
    }
}

void verifyArtBumpClearResult(
    kvspace::ShmClient* recovered,
    bool engine_cleared) {
    if (engine_cleared) {
        CHECK(recovered->Get("/authority").IsNull());
        CHECK(recovered->Get("/base/a").IsNull());
        CHECK(recovered->Stats().entries == 0);
    } else {
        CHECK(recovered->Get("/authority").AsStr() ==
              std::string(2048, 'o'));
        CHECK(recovered->Get("/base/a").AsInt64() == 1);
    }
    CHECK(recovered->Watch("/compact-queue", 5ms).IsNull());
    CHECK(recovered->Stats().queues == 0);
    recovered->Set("/after-art-bump-clear-cut", kvspace::XValue::Int64(37));
    CHECK(recovered->Get("/after-art-bump-clear-cut").AsInt64() == 37);
    CHECK(recovered->Stats().recoveries >= 1);
}

void testOwnerDeathAtArtBumpClearCutPoints(const char* self) {
    struct Cut {
        kvspace::detail::ArtBumpApplyStep step;
        int ordinal;
        bool engine_cleared;
    };
    std::vector<Cut> cuts = {
        {kvspace::detail::ArtBumpApplyStep::ClearCountersStored, -1, false},
        {kvspace::detail::ArtBumpApplyStep::ClearRootPublished, -1, true},
        {kvspace::detail::ArtBumpApplyStep::ClearSlabsRebuilt, -1, true},
        {kvspace::detail::ArtBumpApplyStep::ClearZoneReset, 0, true},
        {kvspace::detail::ArtBumpApplyStep::ClearZoneReset, 1, true},
        {kvspace::detail::ArtBumpApplyStep::ClearActiveStored, -1, true},
    };
    for (int ordinal = 0; ordinal < 4 * 3; ++ordinal) {
        cuts.push_back({
            kvspace::detail::ArtBumpApplyStep::ClearSlabRebuildWrite,
            ordinal,
            true});
    }

    for (const auto cut : cuts) {
        ScopedRegion scoped(uniqueShmName("art-bump-clear-cut"));
        auto owner = createArtBumpFixture(scoped.name());
        const auto before = readArtBumpAuthority(scoped.name());
        owner.reset();

        requireExitCode(
            artBumpCutChild(
                self,
                scoped.name(),
                "clear",
                cut.step,
                cut.ordinal),
            80);

        auto recovered = kvspace::ShmClient::Attach(
            scoped.name(), kvspace::ShmEngine::ArtBump);
        const auto after = readArtBumpAuthority(scoped.name());
        CHECK(after.journal_state == 0);
        if (cut.engine_cleared) {
            CHECK(after.root == 0);
            CHECK(after.active_zone == 0);
            CHECK(after.raw_top == after.raw_begin);
        } else {
            CHECK(after.root == before.root);
            CHECK(after.active_zone == before.active_zone);
        }
        verifyArtBumpClearResult(recovered.get(), cut.engine_cleared);
    }
}

void testRepeatedOwnerDeathDuringEmptyArtBumpRecovery(const char* self) {
    struct RecoveryCut {
        kvspace::detail::ArtBumpApplyStep step;
        int ordinal;
    };
    const std::array<RecoveryCut, 6> cuts = {{
        {kvspace::detail::ArtBumpApplyStep::RecoveryIdleZoneEpochStored, 0},
        {kvspace::detail::ArtBumpApplyStep::RecoveryIdleZoneTopStored, 0},
        {kvspace::detail::ArtBumpApplyStep::RecoveryIdleZoneEpochStored, 1},
        {kvspace::detail::ArtBumpApplyStep::RecoveryIdleZoneTopStored, 1},
        {kvspace::detail::ArtBumpApplyStep::RecoveryIdleActiveStored, -1},
        {kvspace::detail::ArtBumpApplyStep::RecoveryIdleCountersStored, -1},
    }};
    for (const auto cut : cuts) {
        ScopedRegion scoped(uniqueShmName("art-bump-empty-recovery-repeat"));
        auto owner = createArtBumpFixture(scoped.name());
        owner.reset();

        requireExitCode(
            artBumpCutChild(
                self,
                scoped.name(),
                "clear",
                kvspace::detail::ArtBumpApplyStep::ClearRootPublished),
            80);
        requireExitCode(
            artBumpCutChild(
                self,
                scoped.name(),
                "recovery",
                cut.step,
                cut.ordinal),
            80);

        auto recovered = kvspace::ShmClient::Attach(
            scoped.name(), kvspace::ShmEngine::ArtBump);
        const auto after = readArtBumpAuthority(scoped.name());
        CHECK(after.root == 0);
        CHECK(after.journal_state == 0);
        CHECK(after.active_zone == 0);
        CHECK(after.raw_top == after.raw_begin);
        verifyArtBumpClearResult(recovered.get(), true);
    }
}

void testOpenRetriesWhenLockedBackingLosesItsName() {
    ScopedRegion scoped(uniqueShmName("open-name-race"));
    const auto locked_fd = ::shm_open(
        scoped.name().c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    CHECK(locked_fd >= 0);
    CHECK(::flock(locked_fd, LOCK_EX) == 0);

    int ready_pipe[2] = {-1, -1};
    CHECK(::pipe(ready_pipe) == 0);
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        ::close(ready_pipe[0]);
        ::close(locked_fd);
        backing_opened_notify_fd = ready_pipe[1];
        kvspace::detail::SetRegionTestHook(regionTestHook);
        try {
            kvspace::ShmOptions options;
            options.engine = kvspace::ShmEngine::TrieBox;
            auto store = kvspace::ShmClient::Open(scoped.name(), options);
            store->Set("/current-name", kvspace::XValue::Int64(88));
            ::_exit(0);
        } catch (...) {
            ::_exit(1);
        }
    }

    ::close(ready_pipe[1]);
    pollfd waited{ready_pipe[0], POLLIN, 0};
    CHECK(::poll(&waited, 1, 5000) == 1);
    char ready = 0;
    CHECK(::read(ready_pipe[0], &ready, 1) == 1);
    CHECK(ready == 'o');
    ::close(ready_pipe[0]);

    CHECK(::shm_unlink(scoped.name().c_str()) == 0);
    CHECK(::flock(locked_fd, LOCK_UN) == 0);
    ::close(locked_fd);
    requireChild(child);

    auto attached = kvspace::ShmClient::Attach(scoped.name());
    CHECK(attached->Get("/current-name").AsInt64() == 88);
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 &&
        std::string(argv[1]) == "--thread-owner-death") {
        const auto result = runTest([] {
            testTrieBoxThreadOwnerDeathClearsMutationState();
        });
        // The fixture deliberately uses SYS_exit to bypass all worker-thread
        // cleanup.  Keep the resulting sanitizer/runtime TSD state inside this
        // short-lived process; the recovery assertions above have completed.
        std::cerr.flush();
        ::_exit(result);
    }
    if (argc == 4 && std::string(argv[1]) == "--child") {
        return childMain(argv[2], argv[3]);
    }
    return runTest([&] {
        testCommonClearExhaustiveFaultMatrix();
        testTrieBoxThreadOwnerDeathInIsolatedRuntime(argv[0]);
        testTrieBoxRecoveryPreflightDoesNotWriteEitherAllocator(argv[0]);
        testHashBoxRecoveryPreflightDoesNotWriteBoxAllocator(argv[0]);
        testArtBoxSecondPlanFailureDoesNotWriteAnyEngineAllocator(argv[0]);
        testClearTopFirstResetSurvivesRepeatedRecovery(argv[0]);
        for (const auto engine : {
                 kvspace::ShmEngine::ArtBump,
                 kvspace::ShmEngine::ArtBox,
                 kvspace::ShmEngine::HashBox,
                 kvspace::ShmEngine::TrieBox}) {
            testIndependentProcess(argv[0], engine);
            testConcurrentWriters(engine);
            testRobustOwnerDeath(argv[0], engine);
            testKilledWatcherDoesNotLeakQueue(argv[0], engine);
            testNotifierDeathCannotStrandPublishedMessage(argv[0], engine);
            testOwnerDeathDiscardsUncommittedMutation(argv[0], engine);
            if (engine != kvspace::ShmEngine::ArtBump) {
                testOwnerDeathDuringPostPublishGc(argv[0], engine);
            }
            testOwnerDeathAtAllocatorSplitCutPoints(argv[0], engine);
        }
        testOwnerDeathAtArtBumpMutationCutPoints(argv[0]);
        testArtBumpCompactBeforeJournalIsByteStable(argv[0]);
        testOwnerDeathAtArtBumpCompactCutPoints(argv[0]);
        testOwnerDeathAtArtBumpReadyFieldCuts(argv[0]);
        testOwnerDeathAtArtBumpCompactSlabWriteCuts(argv[0]);
        testOwnerDeathAtArtBumpRollbackSlabWriteCuts(argv[0]);
        testOwnerDeathAtArtBumpIdleRecoveryWriteCuts(argv[0]);
        testOwnerDeathDuringEmptyArtBumpCompact(argv[0]);
        testRepeatedOwnerDeathDuringArtBumpRecovery(argv[0]);
        testOwnerDeathAtArtBumpClearCutPoints(argv[0]);
        testRepeatedOwnerDeathDuringEmptyArtBumpRecovery(argv[0]);
        testOwnerDeathDuringTrieBoxGc(argv[0]);
        testRepeatedOwnerDeathDuringArtBoxRecovery(argv[0]);
        testRepeatedOwnerDeathBetweenTrieBoxRecoveryApplies(argv[0]);
        testRepeatedOwnerDeathDuringHashBoxRecovery(argv[0]);
        testOwnerDeathAtAllocatorCoalesceCutPoints(argv[0]);
        testAllocatorCoalesceJournalRejectsCorruption(argv[0]);
        testAllocatorSplitRejectsSubminimumSpan(argv[0]);
        testQueueRejectsNonGenericBlobFlags(argv[0]);
        testMalformedQueueValueIsNeverConsumed(argv[0]);
        testOpenRecoversInterruptedCreate(argv[0]);
        testOpenRetriesWhenLockedBackingLosesItsName();
    });
}
