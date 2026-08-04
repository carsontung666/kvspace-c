#include "test_support.h"

#include "kvspace/errors.h"
#include "kvspace/shm.h"
#include "kvspace/xvalue.h"
#include "shm_region.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <fcntl.h>
#include <initializer_list>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace allocation_failure_injection {

thread_local std::ptrdiff_t countdown = -1;

bool shouldFail() noexcept {
    if (countdown < 0) return false;
    if (countdown == 0) {
        countdown = -1;
        return true;
    }
    --countdown;
    return false;
}

void failAfter(std::size_t successful_allocations) noexcept {
    countdown = static_cast<std::ptrdiff_t>(successful_allocations);
}

void disable() noexcept { countdown = -1; }

void* allocate(std::size_t size) {
    if (shouldFail()) throw std::bad_alloc();
    if (void* memory = std::malloc(size == 0 ? 1 : size); memory != nullptr) {
        return memory;
    }
    throw std::bad_alloc();
}

} // namespace allocation_failure_injection

void* operator new(std::size_t size) {
    return allocation_failure_injection::allocate(size);
}

void* operator new[](std::size_t size) {
    return allocation_failure_injection::allocate(size);
}

void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

using namespace std::chrono_literals;

kvspace::ShmOptions options(std::uint64_t entries = 256) {
    kvspace::ShmOptions result;
    result.initial_size = 128 * 1024;
    result.max_size = 4 * 1024 * 1024;
    result.max_entries = entries;
    result.max_queues = 32;
    return result;
}

constexpr std::uint64_t kRegionPageSizeOffset = 64;
constexpr std::uint64_t kRegionEngineAbiOffset = 28;
constexpr std::uint64_t kRegionSizeOffset = 48;
constexpr std::uint64_t kRegionMaxOffset = 56;
constexpr std::uint64_t kRegionTableCapacityOffset = 80;
constexpr std::uint64_t kRegionTableZeroOffset = 88;
constexpr std::uint64_t kRegionQueueLimitOffset = 136;
constexpr std::uint64_t kRegionQueueCapacityOffset = 144;
constexpr std::uint64_t kRegionQueueOffset = 152;
constexpr std::uint64_t kRegionHeapOffset = 168;
constexpr std::uint64_t kRegionHeapTopOffset = 176;
constexpr std::uint64_t kRegionHeapLimitOffset = 192;
constexpr std::uint64_t kRegionEngineOffset = 200;
constexpr std::uint64_t kRegionEngineSizeOffset = 208;
constexpr std::uint64_t kRegionReservedAllocatorSlotsOffset = 800;
constexpr std::uint64_t kRegionOuterPaddingOffset = 1456;

std::uint64_t readPersistentU64(
    const std::string& path,
    std::uint64_t offset) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    std::uint8_t bytes[8]{};
    CHECK(::pread(fd, bytes, sizeof(bytes), static_cast<off_t>(offset)) ==
          static_cast<ssize_t>(sizeof(bytes)));
    CHECK(::close(fd) == 0);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < sizeof(bytes); ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

std::uint32_t readPersistentU32(
    const std::string& path,
    std::uint64_t offset) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    std::uint8_t bytes[4]{};
    CHECK(::pread(fd, bytes, sizeof(bytes), static_cast<off_t>(offset)) ==
          static_cast<ssize_t>(sizeof(bytes)));
    CHECK(::close(fd) == 0);
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < sizeof(bytes); ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

void writePersistentU64(
    const std::string& path,
    std::uint64_t offset,
    std::uint64_t value) {
    std::uint8_t bytes[8]{};
    for (std::size_t index = 0; index < sizeof(bytes); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(::pwrite(fd, bytes, sizeof(bytes), static_cast<off_t>(offset)) ==
          static_cast<ssize_t>(sizeof(bytes)));
    CHECK(::close(fd) == 0);
}

void writePersistentU32(
    const std::string& path,
    std::uint64_t offset,
    std::uint32_t value) {
    std::uint8_t bytes[4]{};
    for (std::size_t index = 0; index < sizeof(bytes); ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(::pwrite(fd, bytes, sizeof(bytes), static_cast<off_t>(offset)) ==
          static_cast<ssize_t>(sizeof(bytes)));
    CHECK(::close(fd) == 0);
}

std::vector<std::uint8_t> readPersistentBytes(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
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
    return bytes;
}

void expectPersistentLe(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t offset,
    std::uint64_t value,
    std::size_t width) {
    CHECK(width == 2 || width == 4 || width == 8);
    CHECK(offset <= bytes.size());
    CHECK(width <= bytes.size() - static_cast<std::size_t>(offset));
    for (std::size_t index = 0; index < width; ++index) {
        CHECK(bytes[static_cast<std::size_t>(offset) + index] ==
              static_cast<std::uint8_t>(
                  value >> static_cast<unsigned>(index * 8U)));
    }
}

struct PersistentPatch {
    std::uint64_t offset;
    std::uint64_t value;
};

void expectStaticGeometryRejected(
    const std::string& path,
    std::initializer_list<PersistentPatch> patches) {
    std::vector<PersistentPatch> originals;
    originals.reserve(patches.size());
    for (const auto& patch : patches) {
        originals.push_back({
            patch.offset,
            readPersistentU64(path, patch.offset)});
        writePersistentU64(path, patch.offset, patch.value);
    }
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        (void)kvspace::ShmClient::Attach(path);
    });
    for (const auto& original : originals) {
        writePersistentU64(path, original.offset, original.value);
    }
}

std::uint64_t persistentHash(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char raw_byte : value) {
        hash ^= static_cast<unsigned char>(raw_byte);
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<std::string> collidingKeys(
    std::uint64_t capacity,
    std::size_t wanted,
    std::string_view prefix) {
    CHECK(capacity != 0 && (capacity & (capacity - 1U)) == 0);
    std::vector<std::vector<std::string>> buckets(
        static_cast<std::size_t>(capacity));
    for (std::uint64_t candidate = 0; candidate < 100000; ++candidate) {
        auto key = std::string(prefix) + std::to_string(candidate);
        auto& bucket = buckets[static_cast<std::size_t>(
            persistentHash(key) & (capacity - 1U))];
        bucket.push_back(std::move(key));
        if (bucket.size() == wanted) return bucket;
    }
    CHECK(false);
    return {};
}

std::string keyForHome(
    std::uint64_t capacity,
    std::uint64_t home,
    std::string_view prefix,
    std::uint64_t start = 0) {
    CHECK(capacity != 0 && (capacity & (capacity - 1U)) == 0);
    CHECK(home < capacity);
    for (std::uint64_t candidate = start; candidate < start + 100000; ++candidate) {
        auto key = std::string(prefix) + std::to_string(candidate);
        if ((persistentHash(key) & (capacity - 1U)) == home) return key;
    }
    CHECK(false);
    return {};
}

void testHashBoxAbi4HeaderExists() {
    const auto path = "/tmp/kvspace_hashbox_abi4_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    auto configured = options(8);
    configured.engine = kvspace::ShmEngine::HashBox;
    auto store = kvspace::ShmClient::Create(path, configured);
    store->Close();

    CHECK(readPersistentU32(path, kRegionEngineAbiOffset) == 4U);
    const auto engine_offset = readPersistentU64(path, kRegionEngineOffset);
    CHECK(engine_offset != 0);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    std::array<char, 8> magic{};
    CHECK(::pread(fd, magic.data(), magic.size(),
                  static_cast<off_t>(engine_offset)) ==
          static_cast<ssize_t>(magic.size()));
    CHECK(::close(fd) == 0);
    const std::array<char, 8> expected_magic = {
        'K', 'V', 'H', 'B', 'O', 'X', '0', '1'};
    CHECK(magic == expected_magic);
}

void testCommonReservedHeaderBytesAreZero() {
    const auto path = "/tmp/kvspace_common_reserved_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    auto configured = options(8);
    configured.engine = kvspace::ShmEngine::HashBox;
    auto store = kvspace::ShmClient::Create(path, configured);
    store->Close();

    writePersistentU64(path, kRegionReservedAllocatorSlotsOffset, 1);
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        (void)kvspace::ShmClient::Attach(path);
    });
    writePersistentU64(path, kRegionReservedAllocatorSlotsOffset, 0);

    writePersistentU32(path, kRegionOuterPaddingOffset, 1);
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        (void)kvspace::ShmClient::Attach(path);
    });
    writePersistentU32(path, kRegionOuterPaddingOffset, 0);
    auto attached = kvspace::ShmClient::Attach(
        path, kvspace::ShmEngine::HashBox);
    attached->Close();
}

void testSmallCanonicalSlotCapacitiesAndHashReuse() {
    for (const auto& [limit, expected] : {
             std::pair<std::uint64_t, std::uint64_t>{1, 2},
             {2, 4},
             {3, 8},
             {6, 16}}) {
        const auto path = "/tmp/kvspace_small_slots_" +
            std::to_string(limit) + "_" +
            std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
        ScopedRegion scoped(path);
        auto configured = options(limit);
        configured.engine = kvspace::ShmEngine::HashBox;
        configured.max_queues = limit;
        auto store = kvspace::ShmClient::Create(path, configured);
        store->Close();

        CHECK(readPersistentU64(path, kRegionTableCapacityOffset) == expected);
        CHECK(readPersistentU64(path, kRegionQueueCapacityOffset) == expected);
        auto attached = kvspace::ShmClient::Attach(
            path, kvspace::ShmEngine::HashBox);
        attached->Close();

        if (limit == 1) {
            // 2 is the smallest power of two meeting ceil(10*limit/7).
            // A larger power of two is usable but noncanonical persistence.
            expectStaticGeometryRejected(path, {{
                kRegionTableCapacityOffset, 8}});
            expectStaticGeometryRejected(path, {{
                kRegionQueueCapacityOffset, 8}});
        }
    }

    const auto path = "/tmp/kvspace_small_hash_behaviour_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    auto configured = options(2);
    configured.engine = kvspace::ShmEngine::HashBox;
    configured.max_queues = 2;
    auto hash_keys = collidingKeys(4, 3, "hash-collision-");
    auto queue_keys = collidingKeys(4, 3, "queue-collision-");
    {
        auto region = kvspace::detail::Region::Open(
            path, configured, kvspace::detail::OpenMode::Create);
        auto guard = region->Lock();
        region->Put(hash_keys[0], kvspace::XValue::Int64(0).Encode());
        region->Put(hash_keys[1], kvspace::XValue::Int64(1).Encode());
        {
            auto mutation = region->BeginMutation();
            CHECK(region->Erase(hash_keys[0]));
            region->Put(hash_keys[2], kvspace::XValue::Int64(2).Encode());
            region->Put(hash_keys[1], kvspace::XValue::Int64(11).Encode());
            mutation.Commit();
        }

        region->Notify(queue_keys[0], kvspace::XValue::Int64(20).Encode());
        region->Notify(queue_keys[1], kvspace::XValue::Int64(21).Encode());
        std::vector<std::uint8_t> popped;
        CHECK(region->Watch(
            guard, queue_keys[0], 1ms, &popped, [] { return false; }));
        CHECK(kvspace::XValue::Decode(popped).AsInt64() == 20);
        region->Notify(queue_keys[2], kvspace::XValue::Int64(22).Encode());
        (void)guard;
    }

    {
        auto attached = kvspace::detail::Region::Open(
            path, {}, kvspace::detail::OpenMode::Attach,
            kvspace::ShmEngine::HashBox);
        auto guard = attached->Lock();
        std::vector<std::uint8_t> value;
        CHECK(!attached->Exists(hash_keys[0]));
        CHECK(attached->Get(hash_keys[1], &value));
        CHECK(kvspace::XValue::Decode(value).AsInt64() == 11);
        CHECK(attached->Get(hash_keys[2], &value));
        CHECK(kvspace::XValue::Decode(value).AsInt64() == 2);
        CHECK(attached->Stats().entries == 2);
        CHECK(attached->Stats().queues == 2);
        (void)guard;
    }

    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        try {
            auto attached = kvspace::detail::Region::Open(
                path, {}, kvspace::detail::OpenMode::Attach,
                kvspace::ShmEngine::HashBox);
            auto guard = attached->Lock();
            (void)guard;
            ::_exit(0);
        } catch (...) {
            ::_exit(91);
        }
    }
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);

    auto recovered = kvspace::detail::Region::Open(
        path, {}, kvspace::detail::OpenMode::Attach,
        kvspace::ShmEngine::HashBox);
    auto guard = recovered->Lock();
    std::vector<std::uint8_t> value;
    CHECK(!recovered->Exists(hash_keys[0]));
    CHECK(recovered->Get(hash_keys[1], &value));
    CHECK(kvspace::XValue::Decode(value).AsInt64() == 11);
    CHECK(recovered->Get(hash_keys[2], &value));
    CHECK(kvspace::XValue::Decode(value).AsInt64() == 2);
    for (const auto index : {1U, 2U}) {
        CHECK(recovered->Watch(
            guard, queue_keys[index], 1ms, &value, [] { return false; }));
        CHECK(kvspace::XValue::Decode(value).AsInt64() ==
              static_cast<std::int64_t>(20U + index));
    }
    CHECK(recovered->Stats().queues == 0);
    CHECK(recovered->Stats().recoveries >= 1);
}

void testQueueFullProbeReusesTombstone() {
    const auto path = "/tmp/kvspace_queue_full_tombstones_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    auto configured = options(4);
    configured.engine = kvspace::ShmEngine::HashBox;
    configured.max_queues = 1;
    auto store = kvspace::ShmClient::Create(path, configured);

    const auto home_zero = keyForHome(2, 0, "/queue-home-zero-");
    const auto home_one = keyForHome(2, 1, "/queue-home-one-");
    const auto home_zero_again = keyForHome(
        2, 0, "/queue-home-zero-again-");
    store->Notify(home_zero, kvspace::XValue::Int64(10));
    CHECK(store->Watch(home_zero, 5ms).AsInt64() == 10);
    store->Notify(home_one, kvspace::XValue::Int64(11));
    CHECK(store->Watch(home_one, 5ms).AsInt64() == 11);

    const auto queue_offset = readPersistentU64(path, kRegionQueueOffset);
    CHECK(readPersistentU32(path, queue_offset + 28U) == 2U);
    CHECK(readPersistentU32(path, queue_offset + 32U + 28U) == 2U);

    // There is no EMPTY terminator: lookup must finish all C probes and then
    // reuse the first tombstone it observed.
    store->Notify(home_zero_again, kvspace::XValue::Int64(12));
    CHECK(store->Watch(home_zero_again, 5ms).AsInt64() == 12);
    CHECK(store->Stats().queues == 0);
}

void testCrudAndDirectories() {
    ScopedRegion region(uniqueShmName("crud"));
    auto store = kvspace::ShmClient::Create(region.name(), options());

    CHECK(store->Get("/missing").IsNull());
    store->Set("/a/x", kvspace::XValue::Int64(42));
    store->Set("/a/y", kvspace::XValue::Str("hello"));
    CHECK(store->Get("/a/x").AsInt64() == 42);
    CHECK(store->Get("/a/y").AsStr() == "hello");
    CHECK(store->List("/a/") == std::vector<std::string>({"x", "y"}));

    store->Set("/a", kvspace::XValue::Str("file beside directory"));
    CHECK(store->Get("/a").AsStr() == "file beside directory");
    CHECK(store->Get("/a/x").AsInt64() == 42);

    store->Mkindex("/empty/nested/");
    CHECK(store->List("/empty/nested/").empty());
    CHECK(store->List("/") ==
          std::vector<std::string>({"a/", "a", "empty/"}));

    store->Del("/a/x");
    CHECK(store->Get("/a/x").IsNull());
    CHECK(store->List("/a/") == std::vector<std::string>({"y"}));
    store->DelTree("/empty/");
    CHECK(store->Get("/empty/").IsNull());

    auto attached = kvspace::ShmClient::Attach(region.name());
    CHECK(attached->Get("/a/y").AsStr() == "hello");
    attached->Set("/from-second", kvspace::XValue::Bool(true));
    CHECK(store->Get("/from-second").AsBool());

    const auto stats = store->Stats();
    CHECK(stats.entries >= 4);
    CHECK(stats.region_size <= stats.region_max);
}

void testLinks() {
    ScopedRegion region(uniqueShmName("links"));
    auto store = kvspace::ShmClient::Create(region.name(), options());
    store->Set("/target/x", kvspace::XValue::Int64(7));
    store->Link("/target/", "/link/");
    CHECK(store->Get("/link/x").AsInt64() == 7);
    store->Set("/link/y", kvspace::XValue::Int64(8));
    CHECK(store->Get("/target/y").AsInt64() == 8);
    CHECK(store->List("/link/") == std::vector<std::string>({"x", "y"}));
    store->Del("/link/x");
    CHECK(store->Get("/target/x").IsNull());
    store->Del("/link");
    CHECK(store->Get("/link/").IsNull());
    CHECK(store->Get("/target/y").AsInt64() == 8);

    store->Link("/target/", "/unlink-me/");
    store->Unlink("/unlink-me");
    CHECK(store->Get("/unlink-me/").IsNull());

    store->Link("/cycle-b/", "/cycle-a/");
    expectThrows<kvspace::ErrResolve>([&] {
        store->Link("/cycle-a/", "/cycle-b/");
    });
}

void testExtIndex() {
    ScopedRegion region(uniqueShmName("ext"));
    auto store = kvspace::ShmClient::Create(region.name(), options());
    store->Set("/base/a", kvspace::XValue::Int64(1));
    store->Set("/base/b", kvspace::XValue::Int64(2));
    store->ExtIndex("/merge/", "/base/");

    CHECK(store->Get("/merge/a").AsInt64() == 1);
    CHECK(store->List("/merge/") == std::vector<std::string>({"a", "b"}));
    store->Set("/merge/z", kvspace::XValue::Str("local"));
    CHECK(store->Get("/merge/z").AsStr() == "local");
    CHECK(store->Get("/base/z").IsNull());
    CHECK(store->List("/merge/") ==
          std::vector<std::string>({"z", "a", "b"}));

    expectThrows<kvspace::ErrExtCollision>([&] {
        store->Set("/base/z", kvspace::XValue::Int64(9));
    });
    expectThrows<kvspace::ErrExtWrite>([&] {
        store->Set("/merge/a", kvspace::XValue::Int64(9));
    });
    expectThrows<kvspace::ErrExtDelete>([&] { store->Del("/merge/a"); });
    expectThrows<kvspace::ErrExtCascade>([&] {
        store->ExtIndex("/top/", "/merge/");
    });

    store->Mkindex("/base/d/");
    store->Set("/merge/d", kvspace::XValue::Str("local file"));
    store->Set("/base/d/x", kvspace::XValue::Int64(11));
    CHECK(store->Get("/merge/d").AsStr() == "local file");
    CHECK(store->Get("/merge/d/x").AsInt64() == 11);

    store->Unlink("/merge");
    CHECK(store->Get("/merge/a").IsNull());
    CHECK(store->Get("/merge/z").AsStr() == "local");
    CHECK(store->Get("/base/a").AsInt64() == 1);

    store->Mkindex("/link-target/");
    store->Set("/overlay/", kvspace::XValue::Index({"ghost"}));
    store->ExtIndex("/overlay/", "/link-target/");
    expectThrows<kvspace::ErrExtCollision>([&] {
        store->Link("/elsewhere", "/link-target/ghost");
    });
}

void testNotifyWatch() {
    ScopedRegion region(uniqueShmName("notify"));
    auto store = kvspace::ShmClient::Create(region.name(), options());
    store->Notify("/queue", kvspace::XValue::Int64(1));
    store->Notify("/queue", kvspace::XValue::Int64(2));
    CHECK(store->Watch("/queue", 5ms).AsInt64() == 2);
    CHECK(store->Watch("/queue", 5ms).AsInt64() == 1);
    const auto start = std::chrono::steady_clock::now();
    CHECK(store->Watch("/queue", 10ms).IsNull());
    CHECK(std::chrono::steady_clock::now() - start >= 5ms);

    ScopedRegion one_queue_region(uniqueShmName("queue-reuse"));
    auto one_queue_options = options();
    one_queue_options.max_queues = 1;
    auto one_queue = kvspace::ShmClient::Create(
        one_queue_region.name(), one_queue_options);
    CHECK(one_queue->Watch("/first", 1ms).IsNull());
    CHECK(one_queue->Watch("/second", 1ms).IsNull());

    auto long_watch = std::async(std::launch::async, [&] {
        return one_queue->Watch("/active", 2s);
    });
    std::this_thread::sleep_for(20ms);
    CHECK(one_queue->Stats().queues == 0);
    CHECK(one_queue->Watch("/active", 5ms).IsNull());
    CHECK(one_queue->Stats().queues == 0);
    one_queue->Notify("/other", kvspace::XValue::Int64(3));
    CHECK(one_queue->Watch("/other", 5ms).AsInt64() == 3);
    one_queue->Notify("/active", kvspace::XValue::Int64(4));
    CHECK(long_watch.get().AsInt64() == 4);
    CHECK(one_queue->Stats().queues == 0);
    one_queue->Notify("/other", kvspace::XValue::Int64(5));
    CHECK(one_queue->Watch("/other", 5ms).AsInt64() == 5);

    ScopedRegion collision_region(uniqueShmName("queue-clear-collision"));
    auto collision_options = options();
    collision_options.max_queues = 2;
    auto collision = kvspace::ShmClient::Create(
        collision_region.name(), collision_options);
    auto colliding_watch = std::async(std::launch::async, [&] {
        return collision->Watch("/q8", 2s);
    });
    std::this_thread::sleep_for(20ms);
    collision->Notify("/q0", kvspace::XValue::Int64(6));
    collision->Clear();
    collision->Notify("/q8", kvspace::XValue::Int64(7));
    CHECK(colliding_watch.get().AsInt64() == 7);
    CHECK(collision->Stats().queues == 0);
}

void testCapacityAndReuse() {
    ScopedRegion region(uniqueShmName("capacity"));
    auto store = kvspace::ShmClient::Create(region.name(), options(4));
    store->Set("/a", kvspace::XValue::Str(std::string(1024, 'a')));
    store->Set("/b", kvspace::XValue::Str(std::string(1024, 'b')));
    store->Set("/c", kvspace::XValue::Str(std::string(1024, 'c')));
    CHECK(store->Stats().entries == 4); // root index + three files
    expectThrows<kvspace::ErrCapacity>([&] {
        store->Set("/d", kvspace::XValue::Int64(4));
    });
    CHECK(store->Get("/a").AsStr() == std::string(1024, 'a'));

    store->Del("/b");
    store->Set("/d", kvspace::XValue::Int64(4));
    CHECK(store->Get("/d").AsInt64() == 4);

    const auto before = store->Stats();
    for (int i = 0; i < 1000; ++i) {
        store->Set("/a", kvspace::XValue::Str(std::string(1024, static_cast<char>('a' + i % 20))));
    }
    const auto after = store->Stats();
    CHECK(after.heap_used < before.heap_used + 8192);
    CHECK(after.region_size <= after.region_max);
}

void testBatchValidationBoundary() {
    ScopedRegion region(uniqueShmName("batch"));
    auto store = kvspace::ShmClient::Create(region.name(), options());
    expectThrows<kvspace::ErrInvalidPath>([&] {
        store->Set(std::vector<kvspace::KVPair>{
            {"/first", kvspace::XValue::Int64(1)},
            {"relative", kvspace::XValue::Int64(2)},
            {"/never", kvspace::XValue::Int64(3)}});
    });
    CHECK(store->Get("/first").AsInt64() == 1);
    CHECK(store->Get("/never").IsNull());

    expectThrows<kvspace::ErrInvalidPath>([&] {
        store->Set("/bad-index/", kvspace::XValue::Index({"ok", "", "later"}));
    });
    CHECK(store->Get("/bad-index/").IsNull());
    expectThrows<kvspace::ErrInvalidValue>([&] {
        store->Set("/bad-bool", kvspace::XValue::Raw("bool", {2}, 1));
    });
    expectThrows<kvspace::ErrInvalidValue>([&] {
        store->Set("/custom", kvspace::XValue::Raw("custom", {1}, 1));
    });
}

void testCloseCancelsWatch() {
    ScopedRegion region(uniqueShmName("close"));
    auto store = kvspace::ShmClient::Create(region.name(), options());
    auto watched = std::async(std::launch::async, [&] {
        return store->Watch("/waiting", 0ms);
    });
    std::this_thread::sleep_for(20ms);
    CHECK(store->Stats().queues == 0);
    store->Close();
    CHECK(watched.wait_for(1s) == std::future_status::ready);
    CHECK(watched.get().IsNull());
    expectThrows<kvspace::ErrDisconnected>([&] { (void)store->Get("/after-close"); });
}

void testFixedSizeAndExistingMapping() {
    ScopedRegion region(uniqueShmName("fixed-size"));
    auto configured = options(16);
    configured.initial_size = 64 * 1024;
    configured.max_size = 2 * 1024 * 1024;
    auto owner = kvspace::ShmClient::Create(region.name(), configured);
    auto attached = kvspace::ShmClient::Attach(region.name());
    const std::string large(300 * 1024, 'g');
    owner->Set("/large", kvspace::XValue::Str(large));
    CHECK(attached->Get("/large").AsStr() == large);
    CHECK(owner->Stats().region_size == configured.max_size);
}

void testCommonPersistentLittleEndianGolden() {
    constexpr std::uint64_t kHeaderSize = 1472;
    constexpr std::uint64_t kAllocatorJournalOffset = 224;
    constexpr std::uint64_t kAllocatorJournalSize = 64;
    constexpr std::uint64_t kBlobHeaderSize = 48;
    constexpr std::uint64_t kMessageHeaderSize = 16;
    constexpr std::uint32_t kBlobMagic = 0x4b56424cU;
    constexpr std::uint32_t kMessageMagic = 0x4b564d53U;
    const auto path = "/tmp/kvspace_common_le_golden_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    auto configured = options(0x0102U);
    configured.engine = kvspace::ShmEngine::HashBox;
    configured.max_queues = 0x0304U;
    configured.max_size = 8ULL * 1024U * 1024U + 0x123U;
    auto region = kvspace::detail::Region::Open(
        path, configured, kvspace::detail::OpenMode::Create);
    auto guard = region->Lock();
    const std::string key = "/codec";
    const auto value = kvspace::XValue::Int64(0x0102030405060708LL).Encode();
    region->Notify(key, value);

    const auto bytes = readPersistentBytes(path);
    const std::array<std::uint8_t, 8> expected_magic = {
        'K', 'V', 'S', 'H', 'M', '0', '1', '\0'};
    CHECK(std::equal(
        expected_magic.begin(), expected_magic.end(), bytes.begin()));
    expectPersistentLe(bytes, 8, 4, 4);
    expectPersistentLe(bytes, 12, 0x01020304U, 4);
    expectPersistentLe(bytes, 16, 0x4b565231U, 4);
    expectPersistentLe(bytes, 20, kHeaderSize, 4);
    expectPersistentLe(
        bytes, 24,
        static_cast<std::uint32_t>(kvspace::ShmEngine::HashBox), 4);
    expectPersistentLe(bytes, 28, 4, 4);
    expectPersistentLe(bytes, 32, 0x434f4d4d4f4e30b4ULL, 8);
    expectPersistentLe(bytes, kRegionSizeOffset, configured.max_size, 8);
    expectPersistentLe(bytes, kRegionMaxOffset, configured.max_size, 8);
    expectPersistentLe(bytes, kRegionPageSizeOffset, 4096, 8);
    expectPersistentLe(bytes, 72, configured.max_entries, 8);
    expectPersistentLe(bytes, kRegionQueueLimitOffset, configured.max_queues, 8);
    expectPersistentLe(bytes, 160, 1, 8);

    CHECK(std::all_of(
        bytes.begin() + kAllocatorJournalOffset,
        bytes.begin() + kAllocatorJournalOffset + kAllocatorJournalSize,
        [](std::uint8_t byte) { return byte == 0; }));

    const auto queue_offset = readPersistentU64(path, kRegionQueueOffset);
    const auto queue_capacity = readPersistentU64(
        path, kRegionQueueCapacityOffset);
    std::uint64_t occupied_offset = 0;
    for (std::uint64_t index = 0; index < queue_capacity; ++index) {
        const auto candidate = queue_offset + index * 32U;
        if (readPersistentU32(path, candidate + 28U) == 1U) {
            CHECK(occupied_offset == 0);
            occupied_offset = candidate;
        }
    }
    CHECK(occupied_offset != 0);
    const auto key_offset = readPersistentU64(path, occupied_offset + 8U);
    const auto message_offset = readPersistentU64(
        path, occupied_offset + 16U);
    expectPersistentLe(bytes, occupied_offset, persistentHash(key), 8);
    expectPersistentLe(bytes, occupied_offset + 8U, key_offset, 8);
    expectPersistentLe(bytes, occupied_offset + 16U, message_offset, 8);
    expectPersistentLe(bytes, occupied_offset + 24U, key.size(), 4);
    expectPersistentLe(bytes, occupied_offset + 28U, 1, 4);

    const auto key_span = std::uint64_t{64};
    expectPersistentLe(bytes, key_offset, key_span, 8);
    expectPersistentLe(bytes, key_offset + 8U, 0, 8);
    expectPersistentLe(bytes, key_offset + 16U, 0, 8);
    expectPersistentLe(bytes, key_offset + 24U, 0, 8);
    expectPersistentLe(bytes, key_offset + 32U, key.size(), 4);
    expectPersistentLe(bytes, key_offset + 36U, kBlobMagic, 4);
    CHECK(bytes[static_cast<std::size_t>(key_offset + 40U)] == 1U);
    CHECK(bytes[static_cast<std::size_t>(key_offset + 41U)] == 0U);
    expectPersistentLe(bytes, key_offset + 42U, 0, 2);
    expectPersistentLe(bytes, key_offset + 44U, 0, 4);
    CHECK(std::equal(
        key.begin(), key.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(
            key_offset + kBlobHeaderSize)));

    const auto message_payload_size = kMessageHeaderSize + value.size();
    const auto message_span = std::max<std::uint64_t>(
        64,
        (kBlobHeaderSize + message_payload_size + 15U) & ~std::uint64_t{15});
    CHECK(message_offset == key_offset + key_span);
    expectPersistentLe(bytes, message_offset, message_span, 8);
    expectPersistentLe(bytes, message_offset + 8U, key_span, 8);
    expectPersistentLe(bytes, message_offset + 16U, 0, 8);
    expectPersistentLe(bytes, message_offset + 24U, 0, 8);
    expectPersistentLe(
        bytes, message_offset + 32U, message_payload_size, 4);
    expectPersistentLe(bytes, message_offset + 36U, kBlobMagic, 4);
    CHECK(bytes[static_cast<std::size_t>(message_offset + 40U)] == 1U);
    CHECK(bytes[static_cast<std::size_t>(message_offset + 41U)] == 0U);
    expectPersistentLe(bytes, message_offset + 42U, 0, 2);
    expectPersistentLe(bytes, message_offset + 44U, 0, 4);
    const auto message_header_offset = message_offset + kBlobHeaderSize;
    expectPersistentLe(bytes, message_header_offset, 0, 8);
    expectPersistentLe(
        bytes, message_header_offset + 8U, value.size(), 4);
    expectPersistentLe(
        bytes, message_header_offset + 12U, kMessageMagic, 4);
    CHECK(std::equal(
        value.begin(), value.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(
            message_header_offset + kMessageHeaderSize)));
    (void)guard;
}

void testExactUnalignedMaxSizeForAllEngines() {
    constexpr std::uint64_t aligned_size = 8ULL * 1024U * 1024U;
    for (const auto engine : {
             kvspace::ShmEngine::ArtBump,
             kvspace::ShmEngine::ArtBox,
             kvspace::ShmEngine::HashBox,
             kvspace::ShmEngine::TrieBox}) {
        for (const std::uint64_t delta : {std::uint64_t{1},
                                         std::uint64_t{4095}}) {
            const auto path = "/tmp/kvspace_exact_unaligned_" +
                std::to_string(static_cast<std::uint32_t>(engine)) + "_" +
                std::to_string(delta) + "_" +
                std::to_string(static_cast<long long>(::getpid())) +
                ".kvshm";
            ScopedRegion scoped(path);
            auto configured = options(32);
            configured.engine = engine;
            configured.max_size = aligned_size + delta;
            configured.initial_size = configured.max_size;
            auto store = kvspace::ShmClient::Create(path, configured);
            store->Set("/exact", kvspace::XValue::Int64(41));
            const auto stats = store->Stats();
            CHECK(stats.region_size == configured.max_size);
            CHECK(stats.region_max == configured.max_size);
            CHECK(readPersistentU64(path, kRegionSizeOffset) ==
                  configured.max_size);
            CHECK(readPersistentU64(path, kRegionMaxOffset) ==
                  configured.max_size);
            struct stat status {};
            CHECK(::stat(path.c_str(), &status) == 0);
            CHECK(status.st_size == static_cast<off_t>(configured.max_size));
            store->Close();

            auto attached = kvspace::ShmClient::Attach(path, engine);
            CHECK(attached->Get("/exact").AsInt64() == 41);
            CHECK(attached->Stats().region_size == configured.max_size);
            attached->Close();
        }
    }
}

void testTrieBoxRejectsNonRootEmptyLeafOnCleanAttach() {
    const auto path = "/tmp/kvspace_trie_empty_leaf_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    auto configured = options(16);
    configured.engine = kvspace::ShmEngine::TrieBox;
    const std::string key = "/leaf";
    auto store = kvspace::ShmClient::Create(path, configured);
    store->Set(key, kvspace::XValue::Int64(7));
    store->Close();

    const auto bytes = readPersistentBytes(path);
    const auto load_u32 = [&bytes](std::uint64_t offset) {
        CHECK(offset <= bytes.size());
        CHECK(sizeof(std::uint32_t) <=
              bytes.size() - static_cast<std::size_t>(offset));
        std::uint32_t value = 0;
        for (std::size_t index = 0; index < sizeof(value); ++index) {
            value |= static_cast<std::uint32_t>(
                         bytes[static_cast<std::size_t>(offset) + index]) <<
                static_cast<unsigned>(index * 8U);
        }
        return value;
    };
    const auto load_u64 = [&bytes](std::uint64_t offset) {
        CHECK(offset <= bytes.size());
        CHECK(sizeof(std::uint64_t) <=
              bytes.size() - static_cast<std::size_t>(offset));
        std::uint64_t value = 0;
        for (std::size_t index = 0; index < sizeof(value); ++index) {
            value |= static_cast<std::uint64_t>(
                         bytes[static_cast<std::size_t>(offset) + index]) <<
                static_cast<unsigned>(index * 8U);
        }
        return value;
    };

    const auto engine_offset = load_u64(kRegionEngineOffset);
    auto node_id = load_u32(engine_offset + 16U);
    const auto node_metadata = load_u64(engine_offset + 24U);
    const auto node_zone = load_u64(engine_offset + 40U);
    const auto header_width =
        bytes[static_cast<std::size_t>(node_metadata + 56U)];
    CHECK(header_width == 2U || header_width == 4U || header_width == 8U);
    const auto stride = std::uint64_t{1032} + header_width;
    for (const char raw_edge : key) {
        const auto payload = node_zone +
            static_cast<std::uint64_t>(node_id) * stride + header_width;
        const auto edge = static_cast<std::uint8_t>(
            static_cast<unsigned char>(raw_edge));
        node_id = load_u32(payload + 8U +
                           static_cast<std::uint64_t>(edge) * 4U);
        CHECK(node_id != std::numeric_limits<std::uint32_t>::max());
    }
    const auto leaf_payload = node_zone +
        static_cast<std::uint64_t>(node_id) * stride + header_width;
    CHECK((load_u64(leaf_payload) & 1U) == 1U);
    for (std::uint64_t edge = 0; edge < 256U; ++edge) {
        CHECK(load_u32(leaf_payload + 8U + edge * 4U) ==
              std::numeric_limits<std::uint32_t>::max());
    }
    writePersistentU64(path, leaf_payload, 0);

    try {
        (void)kvspace::ShmClient::Attach(
            path, kvspace::ShmEngine::TrieBox);
        CHECK(false);
    } catch (const kvspace::ErrCorruptRegion& error) {
        CHECK(std::string(error.what()).find("non-root empty leaf") !=
              std::string::npos);
    }
}

void testRegularFileAndTruncationCheck() {
    const auto path = "/tmp/kvspace_file_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion region(path);
    auto store = kvspace::ShmClient::Create(path, options());
    store->Set("/file", kvspace::XValue::Int64(12));
    store->Close();
    auto attached = kvspace::ShmClient::Attach(path);
    CHECK(attached->Get("/file").AsInt64() == 12);
    attached->Close();
    CHECK(::truncate(path.c_str(), 32) == 0);
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        (void)kvspace::ShmClient::Attach(path);
    });
}

void testCreateRejectsZeroLogicalLimitsBeforeBackingMutation() {
    for (const auto engine : {
             kvspace::ShmEngine::ArtBump,
             kvspace::ShmEngine::ArtBox,
             kvspace::ShmEngine::HashBox,
             kvspace::ShmEngine::TrieBox}) {
        for (const bool zero_entries : {true, false}) {
            const auto path = "/tmp/kvspace_zero_limit_" +
                std::to_string(static_cast<std::uint32_t>(engine)) + "_" +
                std::string(zero_entries ? "entries_" : "queues_") +
                std::to_string(static_cast<long long>(::getpid())) +
                ".kvshm";
            ScopedRegion scoped(path);
            auto configured = options();
            configured.engine = engine;
            if (zero_entries) {
                configured.max_entries = 0;
            } else {
                configured.max_queues = 0;
            }
            expectThrows<kvspace::ErrCapacity>([&] {
                (void)kvspace::ShmClient::Create(path, configured);
            });
            errno = 0;
            CHECK(::access(path.c_str(), F_OK) == -1);
            CHECK(errno == ENOENT);
        }
    }
}

void testLogicalLimitsAreStrictAndReusable() {
    for (const auto engine : {
             kvspace::ShmEngine::ArtBump,
             kvspace::ShmEngine::ArtBox,
             kvspace::ShmEngine::HashBox,
             kvspace::ShmEngine::TrieBox}) {
        const auto path = "/tmp/kvspace_logical_limits_" +
            std::to_string(static_cast<std::uint32_t>(engine)) + "_" +
            std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
        ScopedRegion scoped(path);
        auto configured = options(1);
        configured.engine = engine;
        configured.max_queues = 1;
        auto region = kvspace::detail::Region::Open(
            path, configured, kvspace::detail::OpenMode::Create);
        auto guard = region->Lock();
        const auto one = kvspace::XValue::Int64(1).Encode();
        const auto two = kvspace::XValue::Int64(2).Encode();

        region->Put("first", one);
        const auto before_entry_limit = readPersistentBytes(path);
        expectThrows<kvspace::ErrCapacity>([&] {
            region->Put("second", two);
        });
        CHECK(readPersistentBytes(path) == before_entry_limit);
        region->Put("first", {});
        std::vector<std::uint8_t> value;
        CHECK(region->Get("first", &value));
        CHECK(value.empty());
        region->Put("first", two);
        CHECK(region->Get("first", &value));
        CHECK(kvspace::XValue::Decode(value).AsInt64() == 2);
        CHECK(region->Erase("first"));
        region->Put("second", one);
        CHECK(region->Get("second", &value));

        region->Notify("queue-one", one);
        const auto before_queue_limit = readPersistentBytes(path);
        expectThrows<kvspace::ErrCapacity>([&] {
            region->Notify("queue-two", two);
        });
        CHECK(readPersistentBytes(path) == before_queue_limit);
        region->Notify("queue-one", two);
        CHECK(region->Watch(
            guard, "queue-one", 1ms, &value, [] { return false; }));
        CHECK(kvspace::XValue::Decode(value).AsInt64() == 2);
        CHECK(region->Watch(
            guard, "queue-one", 1ms, &value, [] { return false; }));
        CHECK(kvspace::XValue::Decode(value).AsInt64() == 1);
        region->Notify("queue-two", two);
        CHECK(region->Watch(
            guard, "queue-two", 1ms, &value, [] { return false; }));
        CHECK(kvspace::XValue::Decode(value).AsInt64() == 2);
    }
}

void testTruncatedFixedRegionsNeverMapPastBacking() {
    for (const std::uint64_t requested_length : {
             std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{63},
             std::uint64_t{64}, std::uint64_t{1471},
             std::numeric_limits<std::uint64_t>::max()}) {
        const auto path = "/tmp/kvspace_truncated_bootstrap_" +
            std::to_string(requested_length) + "_" +
            std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
        ScopedRegion scoped(path);
        auto configured = options(4);
        configured.engine = kvspace::ShmEngine::HashBox;
        auto store = kvspace::ShmClient::Create(path, configured);
        store->Close();
        const auto region_size = readPersistentU64(path, kRegionSizeOffset);
        const auto length = requested_length ==
                std::numeric_limits<std::uint64_t>::max()
            ? region_size - 1U
            : requested_length;
        CHECK(::truncate(path.c_str(), static_cast<off_t>(length)) == 0);

        const auto child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            try {
                (void)kvspace::ShmClient::Attach(
                    path, kvspace::ShmEngine::HashBox);
                ::_exit(90);
            } catch (...) {
                ::_exit(0);
            }
        }
        int status = 0;
        CHECK(::waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 0);
    }
}

void testClearPreflightFailuresWriteNothing() {
    {
        const auto path = "/tmp/kvspace_clear_late_corruption_" +
            std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
        ScopedRegion scoped(path);
        auto configured = options(8);
        configured.engine = kvspace::ShmEngine::HashBox;
        auto region = kvspace::detail::Region::Open(
            path, configured, kvspace::detail::OpenMode::Create);
        auto guard = region->Lock();
        region->Put("engine-key", kvspace::XValue::Int64(5).Encode());
        region->Notify("queue-a", kvspace::XValue::Int64(6).Encode());
        region->Notify("queue-b", kvspace::XValue::Int64(7).Encode());

        const auto queue_offset = readPersistentU64(path, kRegionQueueOffset);
        const auto queue_capacity = readPersistentU64(
            path, kRegionQueueCapacityOffset);
        std::uint64_t last_occupied = queue_capacity;
        for (std::uint64_t index = 0; index < queue_capacity; ++index) {
            if (readPersistentU32(
                    path, queue_offset + index * 32U + 28U) == 1U) {
                last_occupied = index;
            }
        }
        CHECK(last_occupied != queue_capacity);
        const auto head_offset = readPersistentU64(
            path, queue_offset + last_occupied * 32U + 16U);
        CHECK(head_offset != 0);
        writePersistentU32(path, head_offset + 48U + 12U, 0xdeadbeefU);
        const auto corrupted = readPersistentBytes(path);
        expectThrows<kvspace::ErrCorruptRegion>([&] { region->Clear(); });
        CHECK(readPersistentBytes(path) == corrupted);
        (void)guard;
    }

    {
        const auto path = "/tmp/kvspace_clear_bad_alloc_" +
            std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
        ScopedRegion scoped(path);
        auto configured = options(8);
        configured.engine = kvspace::ShmEngine::HashBox;
        auto region = kvspace::detail::Region::Open(
            path, configured, kvspace::detail::OpenMode::Create);
        auto guard = region->Lock();
        region->Put("engine-key", kvspace::XValue::Str("value").Encode());
        region->Notify("queue-a", kvspace::XValue::Int64(8).Encode());
        region->Notify("queue-a", kvspace::XValue::Int64(9).Encode());
        const auto before = readPersistentBytes(path);

        bool saw_failure = false;
        bool completed = false;
        for (std::size_t fail_after = 0; fail_after < 256; ++fail_after) {
            allocation_failure_injection::failAfter(fail_after);
            try {
                region->Clear();
                allocation_failure_injection::disable();
                completed = true;
                break;
            } catch (const std::bad_alloc&) {
                allocation_failure_injection::disable();
                saw_failure = true;
                CHECK(readPersistentBytes(path) == before);
            }
        }
        allocation_failure_injection::disable();
        CHECK(saw_failure);
        CHECK(completed);
        CHECK(region->Stats().entries == 0);
        CHECK(region->Stats().queues == 0);
        (void)guard;
    }
}

void testInvalidCapacityDoesNotLeakBackingObject() {
    ScopedRegion region(uniqueShmName("invalid-capacity"));
    auto invalid = options();
    invalid.max_entries = std::numeric_limits<std::uint64_t>::max() / 10;
    expectThrows<kvspace::ErrCapacity>([&] {
        (void)kvspace::ShmClient::Create(region.name(), invalid);
    });
    auto store = kvspace::ShmClient::Create(region.name(), options());
    store->Set("/ok", kvspace::XValue::Int64(1));
    CHECK(store->Get("/ok").AsInt64() == 1);
}

void testMaxSizeIsAHardLimit() {
    ScopedRegion region(uniqueShmName("max-size-limit"));
    auto too_small = options();
    too_small.initial_size = 64 * 1024;
    too_small.max_size = 64 * 1024;
    too_small.max_entries = 100000;
    too_small.max_queues = 1;
    expectThrows<kvspace::ErrCapacity>([&] {
        (void)kvspace::ShmClient::Create(region.name(), too_small);
    });
    auto store = kvspace::ShmClient::Create(region.name(), options());
    CHECK(store->Stats().region_max == options().max_size);
}

void testSetCapacityErrorRollsBackWholePair() {
    ScopedRegion region(uniqueShmName("set-capacity-rollback"));
    kvspace::ShmOptions constrained;
    constrained.initial_size = 64 * 1024;
    constrained.max_size = 64 * 1024;
    constrained.max_entries = 16;
    constrained.max_queues = 1;
    auto store = kvspace::ShmClient::Create(region.name(), constrained);
    store->Mkindex("/a/");

    expectThrows<kvspace::ErrCapacity>([&] {
        store->Set("/a/x", kvspace::XValue::Str(std::string(48706, 'x')));
    });
    CHECK(store->Get("/a/x").IsNull());
    CHECK(store->List("/a/").empty());

    store->Set("/a/x", kvspace::XValue::Int64(7));
    CHECK(store->Get("/a/x").AsInt64() == 7);
    CHECK(store->List("/a/") == std::vector<std::string>({"x"}));
}

void testEngineIdentityAndMismatch() {
    ScopedRegion region(uniqueShmName("engine-id"));
    auto configured = options();
    configured.engine = kvspace::ShmEngine::HashBox;
    auto owner = kvspace::ShmClient::Create(region.name(), configured);
    CHECK(owner->Stats().engine == kvspace::ShmEngine::HashBox);

    auto detected = kvspace::ShmClient::Attach(region.name());
    CHECK(detected->Stats().engine == kvspace::ShmEngine::HashBox);
    auto expected = kvspace::ShmClient::Attach(
        region.name(), kvspace::ShmEngine::HashBox);
    CHECK(expected->Stats().engine == kvspace::ShmEngine::HashBox);
    expectThrows<kvspace::ErrEngineMismatch>([&] {
        (void)kvspace::ShmClient::Attach(
            region.name(), kvspace::ShmEngine::ArtBox);
    });

    auto wrong_open = configured;
    wrong_open.engine = kvspace::ShmEngine::TrieBox;
    expectThrows<kvspace::ErrEngineMismatch>([&] {
        (void)kvspace::ShmClient::Open(region.name(), wrong_open);
    });
    owner->Set("/still-hash", kvspace::XValue::Bool(true));
    CHECK(detected->Get("/still-hash").AsBool());

    for (const auto engine : {
             kvspace::ShmEngine::ArtBump,
             kvspace::ShmEngine::ArtBox,
             kvspace::ShmEngine::TrieBox}) {
        ScopedRegion supported(uniqueShmName("supported-engine"));
        auto candidate = configured;
        candidate.engine = engine;
        auto created = kvspace::ShmClient::Create(supported.name(), candidate);
        CHECK(created->Stats().engine == engine);
        created->Set("/engine", kvspace::XValue::Int64(4));
        auto attached_engine = kvspace::ShmClient::Attach(
            supported.name(), engine);
        CHECK(attached_engine->Get("/engine").AsInt64() == 4);
        expectThrows<kvspace::ErrEngineMismatch>([&] {
            (void)kvspace::ShmClient::Attach(
                supported.name(), kvspace::ShmEngine::HashBox);
        });
    }
}

void testTreeEngineSemanticSurface(kvspace::ShmEngine engine) {
    ScopedRegion region(uniqueShmName("tree-engine"));
    auto configured = options(1024);
    configured.initial_size = 512 * 1024;
    configured.max_size = 64 * 1024 * 1024;
    configured.engine = engine;
    auto store = kvspace::ShmClient::Create(region.name(), configured);

    CHECK(store->Stats().engine == engine);
    store->Set("/a/x", kvspace::XValue::Int64(1));
    store->Set("/a/y", kvspace::XValue::Str("two"));
    store->Set("/a", kvspace::XValue::Str("file"));
    CHECK(store->Get("/a/x").AsInt64() == 1);
    CHECK(store->List("/a/") == std::vector<std::string>({"x", "y"}));
    CHECK(store->Get("/a").AsStr() == "file");

    store->Mkindex("/empty/nested/");
    CHECK(store->List("/empty/nested/").empty());
    store->Set("/target/value", kvspace::XValue::Int64(7));
    store->Link("/target/", "/link/");
    CHECK(store->Get("/link/value").AsInt64() == 7);

    store->Set("/base/remote", kvspace::XValue::Int64(8));
    store->ExtIndex("/overlay/", "/base/");
    CHECK(store->Get("/overlay/remote").AsInt64() == 8);
    store->Set("/overlay/local", kvspace::XValue::Int64(9));
    CHECK(store->List("/overlay/") ==
          std::vector<std::string>({"local", "remote"}));
    store->Unlink("/overlay");
    CHECK(store->Get("/overlay/remote").IsNull());
    CHECK(store->Get("/overlay/local").AsInt64() == 9);

    store->Notify("/ready", kvspace::XValue::Bool(true));
    CHECK(store->Watch("/ready", 10ms).AsBool());
    auto attached = kvspace::ShmClient::Attach(
        region.name(), engine);
    CHECK(attached->Get("/a/y").AsStr() == "two");
    expectThrows<kvspace::ErrEngineMismatch>([&] {
        (void)kvspace::ShmClient::Attach(
            region.name(), kvspace::ShmEngine::HashBox);
    });

    store->Set("/hot", kvspace::XValue::Str(std::string(1024, 'a')));
    const auto before = store->Stats();
    for (int i = 0; i < 500; ++i) {
        store->Set("/hot", kvspace::XValue::Str(
            std::string(1024, static_cast<char>('a' + i % 20))));
    }
    const auto after = store->Stats();
    CHECK(after.engine_nodes > 0);
    if (engine == kvspace::ShmEngine::ArtBump) {
        CHECK(after.heap_used > before.heap_used + 256 * 1024);
    } else {
        CHECK(after.heap_used < before.heap_used + 256 * 1024);
    }

    store->Del("/a/x");
    CHECK(store->Get("/a/x").IsNull());
    store->DelTree("/empty/");
    CHECK(store->Get("/empty/").IsNull());
    store->Clear();
    CHECK(store->Stats().entries == 0);
    CHECK(store->Stats().engine_nodes == 0);
    CHECK(store->Stats().heap_used == 0);
}

void testDelTreeUsesStoredPrefixAndCleansEveryParent(
    kvspace::ShmEngine engine) {
    ScopedRegion region(uniqueShmName("del-tree-prefix"));
    auto configured = options(256);
    configured.engine = engine;
    auto store = kvspace::ShmClient::Create(region.name(), configured);

    store->Mkindex("/a/");
    store->Set("/a/x", kvspace::XValue::Str("orphan candidate"));
    store->Set("/a/", kvspace::XValue::Index({}));
    store->DelTree("/a/");
    CHECK(store->Get("/a/x").IsNull());
    CHECK(store->Get("/a/").IsNull());
    const auto root_after_a = store->List("/");
    CHECK(std::find(root_after_a.begin(), root_after_a.end(), "a/") ==
          root_after_a.end());

    store->Set("/b", kvspace::XValue::Str("file"));
    store->Set("/b/x", kvspace::XValue::Int64(1));
    store->DelTree("/b");
    CHECK(store->Get("/b").IsNull());
    CHECK(store->Get("/b/").IsNull());
    CHECK(store->Get("/b/x").IsNull());
    const auto root = store->List("/");
    CHECK(std::find(root.begin(), root.end(), "b") == root.end());
    CHECK(std::find(root.begin(), root.end(), "b/") == root.end());
}

void testArtBumpCompactAndClearReset() {
    ScopedRegion region(uniqueShmName("art-bump-compact"));
    auto configured = options(128);
    configured.initial_size = 128 * 1024;
    configured.max_size = 2 * 1024 * 1024;
    configured.engine = kvspace::ShmEngine::ArtBump;
    auto store = kvspace::ShmClient::Create(region.name(), configured);

    for (int update = 0; update < 120; ++update) {
        store->Set("/hot", kvspace::XValue::Str(std::string(
            1024, static_cast<char>('a' + update % 20))));
    }
    const auto before = store->Stats();
    store->Compact();
    const auto compacted = store->Stats();
    CHECK(compacted.heap_used + 32 * 1024 < before.heap_used);
    CHECK(compacted.heap_free >= before.heap_free);
    CHECK(store->Get("/hot").AsStr() == std::string(1024, 't'));

    store->Set("/hot", kvspace::XValue::Str(std::string(256, 'z')));
    CHECK(store->Stats().heap_used > compacted.heap_used);
    CHECK(store->Get("/hot").AsStr() == std::string(256, 'z'));

    store->Clear();
    CHECK(store->Stats().heap_used == 0);
    store->Set("/large", kvspace::XValue::Str(std::string(128 * 1024, 'L')));
    CHECK(store->Get("/large").AsStr() == std::string(128 * 1024, 'L'));
}

void testArtBumpCompactRestoresContiguousCapacity() {
    ScopedRegion region(uniqueShmName("art-bump-contiguous"));
    auto configured = options(64);
    configured.initial_size = 64 * 1024;
    configured.max_size = 4 * 1024 * 1024;
    configured.max_queues = 8;
    configured.engine = kvspace::ShmEngine::ArtBump;
    auto store = kvspace::ShmClient::Create(region.name(), configured);

    int successful_updates = 0;
    bool filled = false;
    for (int update = 0; update < 64; ++update) {
        try {
            store->Set("/hot", kvspace::XValue::Str(std::string(
                64 * 1024, static_cast<char>('a' + update % 20))));
            ++successful_updates;
        } catch (const kvspace::ErrCapacity&) {
            filled = true;
            break;
        }
    }
    CHECK(filled);
    CHECK(successful_updates > 5);
    const auto expected = std::string(
        64 * 1024,
        static_cast<char>('a' + (successful_updates - 1) % 20));
    CHECK(store->Get("/hot").AsStr() == expected);

    bool large_failed = false;
    try {
        store->Set("/large", kvspace::XValue::Str(std::string(256 * 1024, 'L')));
    } catch (const kvspace::ErrCapacity&) {
        large_failed = true;
    }
    CHECK(large_failed);
    const auto fragmented = store->Stats();

    store->Compact();
    const auto compacted = store->Stats();
    CHECK(compacted.heap_used + 256 * 1024 < fragmented.heap_used);
    CHECK(store->Get("/hot").AsStr() == expected);
    store->Set("/large", kvspace::XValue::Str(std::string(256 * 1024, 'L')));
    CHECK(store->Get("/large").AsStr() == std::string(256 * 1024, 'L'));
}

void testArtBumpCompactPreflightFailureLeavesJournalIdle() {
    ScopedRegion scoped(uniqueShmName("art-bump-preflight"));
    kvspace::ShmOptions configured;
    configured.initial_size = 64 * 1024;
    configured.max_size = 512 * 1024;
    configured.max_entries = 16;
    configured.max_queues = 1;
    configured.engine = kvspace::ShmEngine::ArtBump;
    auto owner = kvspace::detail::Region::Open(
        scoped.name(), configured, kvspace::detail::OpenMode::Create);
    const auto kept_value = kvspace::XValue::Bytes(
        std::vector<std::uint8_t>{1, 2, 3}).Encode();
    {
        auto guard = owner->Lock();
        owner->Put("kept", kept_value);
        std::size_t message_count = 0;
        for (const std::size_t size : {32768U, 4096U, 512U, 64U, 1U}) {
            for (;;) {
                try {
                    owner->Notify(
                        "queue",
                        kvspace::XValue::Bytes(
                            std::vector<std::uint8_t>(size, 7)).Encode());
                    ++message_count;
                } catch (const kvspace::ErrCapacity&) {
                    break;
                }
            }
        }
        CHECK(message_count > 0);
        owner->CompactValues();
        std::vector<std::uint8_t> value;
        CHECK(owner->Get("kept", &value));
        CHECK(value == kept_value);
        (void)guard;
    }
    owner.reset();

    auto attached = kvspace::detail::Region::Open(
        scoped.name(), {}, kvspace::detail::OpenMode::Attach,
        kvspace::ShmEngine::ArtBump);
    auto guard = attached->Lock();
    std::vector<std::uint8_t> value;
    CHECK(attached->Get("kept", &value));
    CHECK(value == kept_value);
    (void)guard;
}

void testEntryCapacityRollbackForAllEngines() {
    for (const auto engine : {
             kvspace::ShmEngine::ArtBump,
             kvspace::ShmEngine::ArtBox,
             kvspace::ShmEngine::HashBox,
             kvspace::ShmEngine::TrieBox}) {
        ScopedRegion region(uniqueShmName("entry-capacity"));
        auto configured = options(2);
        configured.engine = engine;
        auto store = kvspace::ShmClient::Create(region.name(), configured);
        store->Set("/a", kvspace::XValue::Int64(1));
        CHECK(store->Stats().entries == 2); // root index + /a
        expectThrows<kvspace::ErrCapacity>([&] {
            store->Set("/b", kvspace::XValue::Int64(2));
        });
        CHECK(store->Get("/b").IsNull());
        CHECK(store->List("/") == std::vector<std::string>({"a"}));
        store->Del("/a");
        store->Set("/b", kvspace::XValue::Int64(2));
        CHECK(store->Get("/b").AsInt64() == 2);
    }
}

void testRawEmptyValueForAllEngines() {
    for (const auto engine : {
             kvspace::ShmEngine::ArtBump,
             kvspace::ShmEngine::ArtBox,
             kvspace::ShmEngine::HashBox,
             kvspace::ShmEngine::TrieBox}) {
        ScopedRegion scoped(uniqueShmName("empty-value"));
        auto configured = options(32);
        configured.engine = engine;
        {
            auto region = kvspace::detail::Region::Open(
                scoped.name(), configured, kvspace::detail::OpenMode::Create);
            auto guard = region->Lock();
            region->Put(
                "temporary",
                kvspace::XValue::Bytes(
                    std::vector<std::uint8_t>(128, 7)).Encode());
            CHECK(region->Erase("temporary"));
            region->Put("none", {});
            std::vector<std::uint8_t> value{1};
            CHECK(region->Get("none", &value));
            CHECK(value.empty());
            (void)guard;
        }
        auto attached = kvspace::detail::Region::Open(
            scoped.name(), {}, kvspace::detail::OpenMode::Attach, engine);
        auto guard = attached->Lock();
        std::vector<std::uint8_t> value{1};
        CHECK(attached->Get("none", &value));
        CHECK(value.empty());
        (void)guard;
    }
}

void testArtAdaptiveNodesAndLongPrefixes(kvspace::ShmEngine engine) {
    ScopedRegion scoped(uniqueShmName("art-adaptive"));
    auto configured = options(512);
    configured.initial_size = 512 * 1024;
    configured.max_size = 64 * 1024 * 1024;
    configured.engine = engine;
    auto region = kvspace::detail::Region::Open(
        scoped.name(), configured, kvspace::detail::OpenMode::Create);
    auto guard = region->Lock();

    for (std::size_t byte = 0; byte < 256; ++byte) {
        const std::string key(1, static_cast<char>(byte));
        region->Put(
            key,
            kvspace::XValue::Bytes(
                {static_cast<std::uint8_t>(byte)}).Encode());
    }
    CHECK(region->Stats().entries == 256);
    CHECK(region->Stats().engine_nodes == 257);
    for (std::size_t byte = 0; byte < 256; ++byte) {
        const std::string key(1, static_cast<char>(byte));
        std::vector<std::uint8_t> value;
        CHECK(region->Get(key, &value));
        CHECK(value == kvspace::XValue::Bytes(
            {static_cast<std::uint8_t>(byte)}).Encode());
    }

    for (std::size_t byte = 0; byte < 208; ++byte) {
        CHECK(region->Erase(std::string(1, static_cast<char>(byte))));
    }
    CHECK(region->Stats().entries == 48);
    for (std::size_t byte = 208; byte < 240; ++byte) {
        CHECK(region->Erase(std::string(1, static_cast<char>(byte))));
    }
    CHECK(region->Stats().entries == 16);
    for (std::size_t byte = 240; byte < 252; ++byte) {
        CHECK(region->Erase(std::string(1, static_cast<char>(byte))));
    }
    CHECK(region->Stats().entries == 4);
    for (std::size_t byte = 252; byte < 255; ++byte) {
        CHECK(region->Erase(std::string(1, static_cast<char>(byte))));
    }
    CHECK(region->Stats().entries == 1);
    CHECK(region->Stats().engine_nodes == 1);

    std::string long_key(8192, 'q');
    long_key.back() = 'r';
    const auto long_value =
        kvspace::XValue::Bytes({9, 8, 7}).Encode();
    region->Put(long_key, long_value);
    const auto prefixed = region->EntriesWithPrefix(
        std::string_view(long_key).substr(0, 4096));
    CHECK(prefixed.size() == 1);
    CHECK(prefixed.front().first == long_key);
    CHECK(prefixed.front().second == long_value);
    CHECK(region->Erase(long_key));
    CHECK(region->Erase(std::string(1, static_cast<char>(255))));
    CHECK(region->Stats().entries == 0);
    CHECK(region->Stats().engine_nodes == 0);
    (void)guard;
}

std::uint64_t nextRandom(std::uint64_t* state) {
    *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
    return *state;
}

void testRawEngineDifferential(kvspace::ShmEngine engine) {
    ScopedRegion scoped(uniqueShmName("raw-differential"));
    auto configured = options(512);
    configured.initial_size = 512 * 1024;
    configured.max_size = 64 * 1024 * 1024;
    configured.engine = engine;
    auto region = kvspace::detail::Region::Open(
        scoped.name(), configured, kvspace::detail::OpenMode::Create);
    auto guard = region->Lock();
    std::map<std::string, std::vector<std::uint8_t>> model;
    std::uint64_t random = 0x4b565350414345ULL;

    const auto make_key = [](std::uint64_t number) {
        std::string key = "/raw/" + std::to_string(number % 19) + "/";
        key.push_back(static_cast<char>((number * 37) & 0xffU));
        key += std::to_string((number / 19) % 23);
        if (number % 97 == 0) key.append(1024, 'p');
        return key;
    };

    for (std::size_t step = 0; step < 1500; ++step) {
        const auto key_number = nextRandom(&random) % 240;
        const auto key = make_key(key_number);
        const auto operation = nextRandom(&random) % 100;
        if (operation < 58) {
            const auto length = static_cast<std::size_t>(
                nextRandom(&random) % 65);
            std::vector<std::uint8_t> value(length);
            for (auto& byte : value) {
                byte = static_cast<std::uint8_t>(nextRandom(&random));
            }
            const auto encoded = kvspace::XValue::Bytes(value).Encode();
            region->Put(key, encoded);
            model[key] = encoded;
        } else if (operation < 82) {
            const bool expected = model.erase(key) != 0;
            CHECK(region->Erase(key) == expected);
        } else {
            const auto prefix_length = static_cast<std::size_t>(
                nextRandom(&random) % (key.size() + 1));
            const auto wanted = std::string_view(key).substr(0, prefix_length);
            auto actual = region->EntriesWithPrefix(wanted);
            std::sort(actual.begin(), actual.end());
            std::vector<kvspace::detail::Region::Entry> expected;
            for (const auto& item : model) {
                if (item.first.size() >= wanted.size() &&
                    std::string_view(item.first).substr(0, wanted.size()) == wanted) {
                    expected.push_back(item);
                }
            }
            std::sort(expected.begin(), expected.end());
            CHECK(actual == expected);
        }

        if (step % 41 == 0) {
            auto actual = region->Entries();
            std::sort(actual.begin(), actual.end());
            std::vector<kvspace::detail::Region::Entry> expected(
                model.begin(), model.end());
            std::sort(expected.begin(), expected.end());
            CHECK(actual == expected);
            CHECK(region->Stats().entries == model.size());
        }
    }
    (void)guard;
}

void testUnknownPersistedEngineIsRejected() {
    const auto path = "/tmp/kvspace_engine_id_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion region(path);
    auto store = kvspace::ShmClient::Create(path, options());
    store->Close();

    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    const std::uint32_t unknown_engine = 0xffffU;
    // RegionPrefix v2 fixes engine_id immediately after its first four u32s.
    constexpr off_t engine_id_offset = 8 + 4 * 4;
    CHECK(::pwrite(
        fd, &unknown_engine, sizeof(unknown_engine), engine_id_offset) ==
        static_cast<ssize_t>(sizeof(unknown_engine)));
    CHECK(::close(fd) == 0);

    expectThrows<kvspace::ErrUnsupportedEngine>([&] {
        (void)kvspace::ShmClient::Attach(path);
    });
}

void testNonCanonicalPersistentGeometryIsRejected() {
    for (const auto engine : {
             kvspace::ShmEngine::HashBox,
             kvspace::ShmEngine::ArtBump,
             kvspace::ShmEngine::ArtBox,
             kvspace::ShmEngine::TrieBox}) {
        const auto path = "/tmp/kvspace_static_geometry_" +
            std::to_string(static_cast<std::uint32_t>(engine)) + "_" +
            std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
        ScopedRegion region(path);
        auto configured = options();
        configured.engine = engine;
        auto store = kvspace::ShmClient::Create(path, configured);
        store->Close();

        const auto page_size = readPersistentU64(
            path, kRegionPageSizeOffset);
        const auto queue_limit = readPersistentU64(
            path, kRegionQueueLimitOffset);
        const auto queue_capacity = readPersistentU64(
            path, kRegionQueueCapacityOffset);
        const auto queue_offset = readPersistentU64(
            path, kRegionQueueOffset);
        const auto heap_offset = readPersistentU64(
            path, kRegionHeapOffset);
        const auto heap_top = readPersistentU64(
            path, kRegionHeapTopOffset);
        const auto heap_limit = readPersistentU64(
            path, kRegionHeapLimitOffset);
        const auto engine_offset = readPersistentU64(
            path, kRegionEngineOffset);
        const auto engine_size = readPersistentU64(
            path, kRegionEngineSizeOffset);
        CHECK(page_size != 0);
        CHECK(queue_capacity > queue_limit);
        CHECK(heap_top == heap_offset);

        // These values remain superficially aligned and non-overlapping, but
        // none is the unique geometry produced from the immutable limits.
        expectStaticGeometryRejected(path, {{
            kRegionPageSizeOffset, page_size * 2U}});
        expectStaticGeometryRejected(path, {{
            kRegionQueueCapacityOffset, queue_capacity / 2U}});
        expectStaticGeometryRejected(path, {{
            kRegionQueueOffset, queue_offset + 16U}});
        expectStaticGeometryRejected(path, {
            {kRegionHeapOffset, heap_offset + 16U},
            {kRegionHeapTopOffset, heap_top + 16U},
        });
        if (engine == kvspace::ShmEngine::HashBox ||
            engine == kvspace::ShmEngine::ArtBump) {
            expectStaticGeometryRejected(path, {{
                kRegionSizeOffset, heap_offset}});
        }

        if (engine == kvspace::ShmEngine::HashBox) {
            const auto table_capacity = readPersistentU64(
                path, kRegionTableCapacityOffset);
            const auto table_zero = readPersistentU64(
                path, kRegionTableZeroOffset);
            CHECK(table_capacity > configured.max_entries);
            CHECK(table_zero >= page_size);
            expectStaticGeometryRejected(path, {{
                kRegionTableCapacityOffset, table_capacity / 2U}});
            expectStaticGeometryRejected(path, {{
                kRegionTableZeroOffset, table_zero - 16U}});
        } else if (engine == kvspace::ShmEngine::ArtBump) {
            expectStaticGeometryRejected(path, {{
                kRegionHeapLimitOffset, heap_limit + page_size}});
            constexpr std::uint64_t raw_zone_zero_offset = 160;
            const auto raw_begin_offset =
                engine_offset + raw_zone_zero_offset;
            const auto raw_begin = readPersistentU64(
                path, raw_begin_offset);
            expectStaticGeometryRejected(path, {{
                raw_begin_offset, raw_begin + page_size}});
        } else {
            CHECK(engine_offset == heap_limit);
            CHECK(engine_size >= page_size);
            expectStaticGeometryRejected(path, {
                {kRegionHeapLimitOffset, heap_limit + page_size},
                {kRegionEngineOffset, engine_offset + page_size},
                {kRegionEngineSizeOffset, engine_size - page_size},
            });
        }

        auto attached = kvspace::ShmClient::Attach(path, engine);
        CHECK(attached->Stats().engine == engine);
    }
}

void testArtBumpRejectsNonemptyInactiveZoneWithoutJournal() {
    const auto path = "/tmp/kvspace_art_bump_inactive_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion region(path);
    auto configured = options();
    configured.engine = kvspace::ShmEngine::ArtBump;
    auto store = kvspace::ShmClient::Create(path, configured);
    store->Set("/kept", kvspace::XValue::Int64(1));
    store->Close();

    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    constexpr std::uint64_t raw_zones_offset = 160;
    constexpr std::uint64_t raw_zone_descriptor_bytes = 32;
    constexpr std::uint64_t raw_top_offset = 16;
    std::uint64_t engine_offset = 0;
    CHECK(::pread(
              fd,
              &engine_offset,
              sizeof(engine_offset),
              static_cast<off_t>(kRegionEngineOffset)) ==
          static_cast<ssize_t>(sizeof(engine_offset)));
    std::uint64_t inactive_begin = 0;
    const auto inactive_begin_offset = static_cast<off_t>(
        engine_offset + raw_zones_offset + raw_zone_descriptor_bytes);
    CHECK(::pread(
              fd, &inactive_begin, sizeof(inactive_begin), inactive_begin_offset) ==
          static_cast<ssize_t>(sizeof(inactive_begin)));
    const auto nonempty_top = inactive_begin + 16;
    const auto inactive_top_offset = inactive_begin_offset +
        static_cast<off_t>(raw_top_offset);
    CHECK(::pwrite(
              fd, &nonempty_top, sizeof(nonempty_top), inactive_top_offset) ==
          static_cast<ssize_t>(sizeof(nonempty_top)));
    CHECK(::close(fd) == 0);

    expectThrows<kvspace::ErrCorruptRegion>([&] {
        (void)kvspace::ShmClient::Attach(path);
    });
}

} // namespace

int main() {
    return runTest([] {
        testHashBoxAbi4HeaderExists();
        testCommonReservedHeaderBytesAreZero();
        testSmallCanonicalSlotCapacitiesAndHashReuse();
        testQueueFullProbeReusesTombstone();
        testCrudAndDirectories();
        testLinks();
        testExtIndex();
        testNotifyWatch();
        testCapacityAndReuse();
        testBatchValidationBoundary();
        testCloseCancelsWatch();
        testFixedSizeAndExistingMapping();
        testCommonPersistentLittleEndianGolden();
        testExactUnalignedMaxSizeForAllEngines();
        testTrieBoxRejectsNonRootEmptyLeafOnCleanAttach();
        testRegularFileAndTruncationCheck();
        testCreateRejectsZeroLogicalLimitsBeforeBackingMutation();
        testLogicalLimitsAreStrictAndReusable();
        testTruncatedFixedRegionsNeverMapPastBacking();
        testClearPreflightFailuresWriteNothing();
        testInvalidCapacityDoesNotLeakBackingObject();
        testMaxSizeIsAHardLimit();
        testSetCapacityErrorRollsBackWholePair();
        testEngineIdentityAndMismatch();
        for (const auto engine : {
                 kvspace::ShmEngine::ArtBump,
                 kvspace::ShmEngine::ArtBox,
                 kvspace::ShmEngine::TrieBox}) {
            testTreeEngineSemanticSurface(engine);
        }
        testArtBumpCompactAndClearReset();
        testArtBumpCompactRestoresContiguousCapacity();
        testArtBumpCompactPreflightFailureLeavesJournalIdle();
        testEntryCapacityRollbackForAllEngines();
        testRawEmptyValueForAllEngines();
        for (const auto engine : {
                 kvspace::ShmEngine::ArtBump,
                 kvspace::ShmEngine::ArtBox}) {
            testArtAdaptiveNodesAndLongPrefixes(engine);
        }
        for (const auto engine : {
                 kvspace::ShmEngine::ArtBump,
                 kvspace::ShmEngine::ArtBox,
                 kvspace::ShmEngine::HashBox,
                 kvspace::ShmEngine::TrieBox}) {
            testRawEngineDifferential(engine);
            testDelTreeUsesStoredPrefixAndCleansEveryParent(engine);
        }
        testUnknownPersistedEngineIsRejected();
        testNonCanonicalPersistentGeometryIsRejected();
        testArtBumpRejectsNonemptyInactiveZoneWithoutJournal();
    });
}
