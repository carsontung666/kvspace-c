#include "test_support.h"

#include "kvspace/errors.h"
#include "kvspace/shm.h"
#include "kvspace/xvalue.h"
#include "shm_region.h"

#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uint64_t kLogicalLimit = 3;
constexpr std::uint64_t kLimitPlusOne = kLogicalLimit + 1U;
constexpr std::uint64_t kEntryLimitOffset = 72;
constexpr std::uint64_t kEntryCountOffset = 120;
constexpr std::uint64_t kQueueLimitOffset = 136;
constexpr std::uint64_t kQueueCountOffset = 160;
constexpr std::size_t kPthreadBytesBegin = 1368;
constexpr std::size_t kPthreadBytesEnd = 1456;

enum class Dimension : std::uint8_t {
    Entries,
    Queues,
};

enum class AttachPath : std::uint8_t {
    Clean,
    OwnerDeath,
};

kvspace::ShmOptions options(kvspace::ShmEngine engine) {
    kvspace::ShmOptions result;
    result.initial_size = 128 * 1024;
    result.max_size = 4 * 1024 * 1024;
    result.max_entries = kLogicalLimit;
    result.max_queues = kLogicalLimit;
    result.engine = engine;
    return result;
}

std::uint64_t loadLe64(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

std::vector<std::uint8_t> readBytes(const std::string& path) {
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

void writeLe64(
    const std::string& path,
    std::uint64_t offset,
    std::uint64_t value) {
    std::uint8_t bytes[8]{};
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(::pwrite(fd, bytes, sizeof(bytes), static_cast<off_t>(offset)) ==
          static_cast<ssize_t>(sizeof(bytes)));
    CHECK(::close(fd) == 0);
}

std::uint64_t readLe64At(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t offset) {
    CHECK(offset <= bytes.size());
    CHECK(8U <= bytes.size() - static_cast<std::size_t>(offset));
    return loadLe64(bytes.data() + static_cast<std::size_t>(offset));
}

void expectNonPthreadBytesEqual(
    const std::vector<std::uint8_t>& before,
    const std::vector<std::uint8_t>& after) {
    CHECK(before.size() == after.size());
    CHECK(kPthreadBytesEnd <= before.size());
    for (std::size_t index = 0; index < before.size(); ++index) {
        if (index >= kPthreadBytesBegin && index < kPthreadBytesEnd) continue;
        CHECK(before[index] == after[index]);
    }
}

std::string fixturePath(
    kvspace::ShmEngine engine,
    Dimension dimension,
    AttachPath attach_path,
    std::uint64_t count) {
    return "/tmp/kvspace_limit_recovery_" +
        std::to_string(static_cast<std::uint32_t>(engine)) + "_" +
        std::to_string(static_cast<std::uint32_t>(dimension)) + "_" +
        std::to_string(static_cast<std::uint32_t>(attach_path)) + "_" +
        std::to_string(count) + "_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
}

void addRecord(
    kvspace::detail::Region* region,
    kvspace::detail::Region::Guard& guard,
    Dimension dimension,
    std::uint64_t ordinal) {
    const auto suffix = std::to_string(ordinal);
    const auto value = kvspace::XValue::Int64(
        static_cast<std::int64_t>(ordinal + 1U)).Encode();
    if (dimension == Dimension::Entries) {
        region->Put("entry-" + suffix, value);
    } else {
        region->Notify("queue-" + suffix, value);
    }
    (void)guard;
}

std::unique_ptr<kvspace::detail::Region> createFixture(
    const std::string& path,
    kvspace::ShmEngine engine,
    Dimension dimension,
    std::uint64_t count) {
    auto region = kvspace::detail::Region::Open(
        path, options(engine), kvspace::detail::OpenMode::Create, engine);
    auto guard = region->Lock();
    const auto normal_count = count > kLogicalLimit
        ? kLogicalLimit
        : count;
    for (std::uint64_t ordinal = 0; ordinal < normal_count; ++ordinal) {
        addRecord(region.get(), guard, dimension, ordinal);
    }

    if (count == kLimitPlusOne) {
        const auto limit_offset = dimension == Dimension::Entries
            ? kEntryLimitOffset
            : kQueueLimitOffset;

        // This is a byte fixture, not a production bypass.  Temporarily raise
        // only the logical gate while the already-attached engine owns its
        // original limit-3 geometry, materialize a fourth canonical record,
        // then restore the immutable limit before any Attach under test.
        // The successful write itself proves that physical nodes/tables/Box
        // data or the common heap had spare capacity.
        writeLe64(path, limit_offset, kLimitPlusOne);
        try {
            addRecord(region.get(), guard, dimension, kLogicalLimit);
        } catch (...) {
            writeLe64(path, limit_offset, kLogicalLimit);
            throw;
        }
        writeLe64(path, limit_offset, kLogicalLimit);
    }
    return region;
}

void leaveMutexOwnerDead(kvspace::detail::Region* region) {
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        try {
            auto guard = region->Lock();
            (void)guard;
            ::_exit(77);
        } catch (...) {
            ::_exit(78);
        }
    }
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 77);
}

void verifyAcceptedCount(
    kvspace::ShmClient* attached,
    Dimension dimension,
    std::uint64_t count,
    AttachPath attach_path) {
    const auto stats = attached->Stats();
    if (dimension == Dimension::Entries) {
        CHECK(stats.entries == count);
        CHECK(stats.queues == 0);
    } else {
        CHECK(stats.entries == 0);
        CHECK(stats.queues == count);
    }
    if (attach_path == AttachPath::OwnerDeath) {
        CHECK(stats.recoveries >= 1);
    }
}

void runFixture(
    kvspace::ShmEngine engine,
    Dimension dimension,
    AttachPath attach_path,
    std::uint64_t count) {
    const auto path = fixturePath(engine, dimension, attach_path, count);
    ScopedRegion scoped(path);
    auto owner = createFixture(path, engine, dimension, count);
    if (attach_path == AttachPath::OwnerDeath) {
        leaveMutexOwnerDead(owner.get());
    }
    owner.reset();

    if (count <= kLogicalLimit) {
        auto attached = kvspace::ShmClient::Attach(path, engine);
        verifyAcceptedCount(attached.get(), dimension, count, attach_path);
        attached->Close();
        return;
    }

    const auto before = readBytes(path);
    CHECK(readLe64At(before, kEntryLimitOffset) == kLogicalLimit);
    CHECK(readLe64At(before, kQueueLimitOffset) == kLogicalLimit);
    const auto count_offset = dimension == Dimension::Entries
        ? kEntryCountOffset
        : kQueueCountOffset;
    CHECK(readLe64At(before, count_offset) == kLimitPlusOne);

    expectThrows<kvspace::ErrCorruptRegion>([&] {
        (void)kvspace::ShmClient::Attach(path, engine);
    });
    const auto after = readBytes(path);

    // Clean validation and owner-death PrepareRecovery must both reject the
    // derived limit+1 count before their first application write.  Robust
    // mutex transitions are implementation-owned pthread bytes and are the
    // only excluded range.
    expectNonPthreadBytesEqual(before, after);
}

void testCleanAttachAndOwnerDeathLogicalLimitMatrix() {
    for (const auto engine : {
             kvspace::ShmEngine::ArtBump,
             kvspace::ShmEngine::ArtBox,
             kvspace::ShmEngine::HashBox,
             kvspace::ShmEngine::TrieBox}) {
        for (const auto dimension : {
                 Dimension::Entries,
                 Dimension::Queues}) {
            for (const auto attach_path : {
                     AttachPath::Clean,
                     AttachPath::OwnerDeath}) {
                for (const std::uint64_t count : {
                         UINT64_C(0), UINT64_C(1),
                         kLogicalLimit, kLimitPlusOne}) {
                    runFixture(
                        engine, dimension, attach_path, count);
                }
            }
        }
    }
}

} // namespace

int main() {
    return runTest(testCleanAttachAndOwnerDeathLogicalLimitMatrix);
}
