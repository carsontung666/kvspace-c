#include "test_support.h"

#include "kvspace/errors.h"
#include "kvspace/shm.h"
#include "kvspace/xvalue.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace injection {

bool fail_ftruncate = false;
bool fail_mmap = false;
bool guard_prefix_only = false;
bool forbidden_pread_attempted = false;
bool forbidden_mmap_attempted = false;
bool ftruncate_saw_empty_backing = false;
bool mmap_saw_zero_prefix = false;
std::size_t ftruncate_calls = 0;
std::size_t mmap_calls = 0;

void reset() noexcept {
    fail_ftruncate = false;
    fail_mmap = false;
    guard_prefix_only = false;
    forbidden_pread_attempted = false;
    forbidden_mmap_attempted = false;
    ftruncate_saw_empty_backing = false;
    mmap_saw_zero_prefix = false;
    ftruncate_calls = 0;
    mmap_calls = 0;
}

} // namespace injection

extern "C" int __real_ftruncate(int fd, off_t length);
extern "C" void* __real_mmap(
    void* address,
    std::size_t length,
    int protection,
    int flags,
    int fd,
    off_t offset);
extern "C" ssize_t __real_pread(
    int fd,
    void* buffer,
    std::size_t count,
    off_t offset);

extern "C" int __wrap_ftruncate(int fd, off_t length) {
    ++injection::ftruncate_calls;
    if (!injection::fail_ftruncate) {
        return __real_ftruncate(fd, length);
    }
    injection::fail_ftruncate = false;
    struct stat status {};
    injection::ftruncate_saw_empty_backing =
        ::fstat(fd, &status) == 0 && status.st_size == 0;
    errno = EIO;
    return -1;
}

extern "C" void* __wrap_mmap(
    void* address,
    std::size_t length,
    int protection,
    int flags,
    int fd,
    off_t offset) {
    ++injection::mmap_calls;
    if (injection::guard_prefix_only) {
        injection::forbidden_mmap_attempted = true;
        errno = EFAULT;
        return MAP_FAILED;
    }
    if (!injection::fail_mmap) {
        return __real_mmap(
            address, length, protection, flags, fd, offset);
    }
    injection::fail_mmap = false;
    std::array<std::uint8_t, 64> prefix{};
    struct stat status {};
    const auto count = __real_pread(
        fd, prefix.data(), prefix.size(), static_cast<off_t>(0));
    injection::mmap_saw_zero_prefix =
        ::fstat(fd, &status) == 0 && status.st_size >= 0 &&
        static_cast<std::uint64_t>(status.st_size) == length &&
        count == static_cast<ssize_t>(prefix.size()) &&
        std::all_of(prefix.begin(), prefix.end(),
                    [](std::uint8_t byte) { return byte == 0; });
    errno = ENOMEM;
    return MAP_FAILED;
}

extern "C" ssize_t __wrap_pread(
    int fd,
    void* buffer,
    std::size_t count,
    off_t offset) {
    if (injection::guard_prefix_only) {
        constexpr std::uint64_t prefix_size = 64;
        const bool outside_prefix = offset < 0 ||
            static_cast<std::uint64_t>(offset) >= prefix_size ||
            count > prefix_size - static_cast<std::uint64_t>(offset);
        if (outside_prefix) {
            injection::forbidden_pread_attempted = true;
            errno = EFAULT;
            return -1;
        }
    }
    return __real_pread(fd, buffer, count, offset);
}

namespace {

constexpr std::uint64_t kVersionOffset = 8;
constexpr std::uint64_t kEndianOffset = 12;
constexpr std::uint64_t kHeaderSizeOffset = 20;
constexpr std::uint64_t kEngineIdOffset = 24;
constexpr std::uint64_t kEngineAbiOffset = 28;
constexpr std::uint64_t kCommonLayoutHashOffset = 32;
constexpr std::uint64_t kEngineLayoutHashOffset = 40;
constexpr std::uint64_t kRegionSizeOffset = 48;
constexpr std::uint64_t kRegionMaxOffset = 56;
constexpr std::uint64_t kCommonHeaderBytes = 1472;
constexpr std::uint64_t kCommon04Hash = 0x434f4d4d4f4e30b4ULL;

class ScopedPath {
public:
    explicit ScopedPath(std::string path) : path_(std::move(path)) {
        static_cast<void>(::unlink(path_.c_str()));
    }
    ~ScopedPath() { static_cast<void>(::unlink(path_.c_str())); }
    ScopedPath(const ScopedPath&) = delete;
    ScopedPath& operator=(const ScopedPath&) = delete;

    const std::string& get() const noexcept { return path_; }

private:
    std::string path_;
};

std::string uniquePath(const std::string& label) {
    static std::uint64_t sequence = 0;
    ++sequence;
    return "/tmp/kvspace_bootstrap_" + label + "_" +
        std::to_string(static_cast<long long>(::getpid())) + "_" +
        std::to_string(sequence) + ".kvshm";
}

kvspace::ShmOptions options(kvspace::ShmEngine engine) {
    kvspace::ShmOptions result;
    result.initial_size = 128U * 1024U;
    result.max_size = 8ULL * 1024U * 1024U;
    result.max_entries = 8;
    result.max_queues = 4;
    result.engine = engine;
    return result;
}

std::uint64_t readLittleEndian(
    const std::string& path,
    std::uint64_t offset,
    std::size_t width) {
    CHECK(width == 4 || width == 8);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    std::array<std::uint8_t, 8> bytes{};
    CHECK(__real_pread(
              fd,
              bytes.data(),
              width,
              static_cast<off_t>(offset)) == static_cast<ssize_t>(width));
    CHECK(::close(fd) == 0);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

void writeLittleEndian(
    const std::string& path,
    std::uint64_t offset,
    std::uint64_t value,
    std::size_t width) {
    CHECK(width == 4 || width == 8);
    std::array<std::uint8_t, 8> bytes{};
    for (std::size_t index = 0; index < width; ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(::pwrite(
              fd,
              bytes.data(),
              width,
              static_cast<off_t>(offset)) == static_cast<ssize_t>(width));
    CHECK(::close(fd) == 0);
}

std::vector<std::uint8_t> readFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    struct stat status {};
    CHECK(::fstat(fd, &status) == 0);
    CHECK(status.st_size >= 0);
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(status.st_size));
    if (!bytes.empty()) {
        CHECK(__real_pread(fd, bytes.data(), bytes.size(), 0) ==
              static_cast<ssize_t>(bytes.size()));
    }
    CHECK(::close(fd) == 0);
    return bytes;
}

std::uint64_t loadLittleEndianBytes(
    const std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::size_t width) {
    CHECK(width == 4 || width == 8);
    CHECK(offset <= bytes.size());
    CHECK(width <= bytes.size() - offset);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < width; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

std::vector<std::uint8_t> buildIndependentCommon04Fixture(
    const std::vector<std::uint8_t>& source) {
    CHECK(source.size() >= kCommonHeaderBytes);
    std::vector<std::uint8_t> fixture(source.size(), 0);
    std::copy(
        source.begin() + static_cast<std::ptrdiff_t>(kCommonHeaderBytes),
        source.end(),
        fixture.begin() + static_cast<std::ptrdiff_t>(kCommonHeaderBytes));

    std::array<bool, kCommonHeaderBytes> authored{};
    const auto mark = [&authored](std::size_t offset, std::size_t width) {
        CHECK(offset <= authored.size());
        CHECK(width <= authored.size() - offset);
        for (std::size_t index = 0; index < width; ++index) {
            CHECK(!authored[offset + index]);
            authored[offset + index] = true;
        }
    };
    const auto put_le = [&fixture, &mark](
                            std::size_t offset,
                            std::uint64_t value,
                            std::size_t width) {
        CHECK(width == 4 || width == 8);
        mark(offset, width);
        for (std::size_t index = 0; index < width; ++index) {
            fixture[offset + index] = static_cast<std::uint8_t>(
                value >> static_cast<unsigned>(index * 8U));
        }
    };
    const auto copy_le = [&source, &put_le](
                             std::size_t offset,
                             std::size_t width) {
        put_le(
            offset,
            loadLittleEndianBytes(source, offset, width),
            width);
    };
    const auto put_zero_le = [&source, &put_le](
                                 std::size_t offset,
                                 std::size_t width) {
        CHECK(loadLittleEndianBytes(source, offset, width) == 0);
        put_le(offset, 0, width);
    };
    const auto put_bytes = [&fixture, &mark](
                               std::size_t offset,
                               const std::uint8_t* bytes,
                               std::size_t width) {
        mark(offset, width);
        std::copy(bytes, bytes + width, fixture.begin() +
                  static_cast<std::ptrdiff_t>(offset));
    };

    // RegionPrefix: fixed discriminator fields are authored from constants;
    // the engine hash and region extent are decoded and re-encoded field-wise.
    const std::array<std::uint8_t, 8> magic = {
        'K', 'V', 'S', 'H', 'M', '0', '1', '\0'};
    CHECK(std::equal(magic.begin(), magic.end(), source.begin()));
    put_bytes(0, magic.data(), magic.size());
    CHECK(loadLittleEndianBytes(source, 8, 4) == 4);
    put_le(8, 4, 4);
    CHECK(loadLittleEndianBytes(source, 12, 4) == 0x01020304U);
    put_le(12, 0x01020304U, 4);
    CHECK(loadLittleEndianBytes(source, 16, 4) == 0x4b565231U);
    put_le(16, 0x4b565231U, 4);
    CHECK(loadLittleEndianBytes(source, 20, 4) == kCommonHeaderBytes);
    put_le(20, kCommonHeaderBytes, 4);
    CHECK(loadLittleEndianBytes(source, 24, 4) ==
          static_cast<std::uint32_t>(kvspace::ShmEngine::HashBox));
    put_le(
        24, static_cast<std::uint32_t>(kvspace::ShmEngine::HashBox), 4);
    CHECK(loadLittleEndianBytes(source, 28, 4) == 4);
    put_le(28, 4, 4);
    CHECK(loadLittleEndianBytes(source, 32, 8) == kCommon04Hash);
    put_le(32, kCommon04Hash, 8);
    copy_le(40, 8);
    copy_le(48, 8);
    copy_le(56, 8);

    // Every scalar in the common immutable/mutable header is individually
    // decoded and little-endian encoded at its normative absolute offset.
    for (std::size_t offset = 64; offset < 224; offset += 8) {
        copy_le(offset, 8);
    }

    // A clean fixture has an idle all-zero allocator journal.  Its four u64,
    // two u32, and three reserved u64 fields are still authored separately.
    for (const std::size_t offset : {224U, 232U, 240U, 248U}) {
        put_zero_le(offset, 8);
    }
    put_zero_le(256, 4);
    put_zero_le(260, 4);
    for (const std::size_t offset : {264U, 272U, 280U}) {
        put_zero_le(offset, 8);
    }

    // HashBox does not consume COMMON Blob storage for an ordinary Set, so all
    // 64 free heads and all 68 reserved allocator slots are canonically zero.
    for (std::size_t index = 0; index < 64; ++index) {
        put_zero_le(288 + index * 8U, 8);
    }
    for (std::size_t index = 0; index < 68; ++index) {
        put_zero_le(800 + index * 8U, 8);
    }
    copy_le(1344, 8); // generation
    copy_le(1352, 8); // recovery_count
    copy_le(1360, 4); // active_table
    copy_le(1364, 4); // corrupt

    // Only the explicitly native pthread objects may be copied as bytes.
    put_bytes(1368, source.data() + 1368, 40);
    put_bytes(1408, source.data() + 1408, 48);
    for (std::size_t offset = 1456; offset < 1472; offset += 8) {
        put_zero_le(offset, 8);
    }

    CHECK(std::all_of(authored.begin(), authored.end(),
                      [](bool value) { return value; }));
    return fixture;
}

void copyFixture(
    const std::vector<std::uint8_t>& bytes,
    const std::string& path) {
    const int fd = ::open(
        path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    CHECK(bytes.size() <= static_cast<std::size_t>(
              std::numeric_limits<off_t>::max()));
    CHECK(__real_ftruncate(fd, static_cast<off_t>(bytes.size())) == 0);
    std::size_t written = 0;
    while (written < bytes.size()) {
        const auto count = ::pwrite(
            fd,
            bytes.data() + written,
            bytes.size() - written,
            static_cast<off_t>(written));
        CHECK(count > 0);
        written += static_cast<std::size_t>(count);
    }
    CHECK(::close(fd) == 0);
}

template <typename ErrorType>
void expectRejectedPatch(
    const std::string& path,
    std::uint64_t offset,
    std::uint64_t replacement,
    std::size_t width,
    kvspace::ShmEngine expected_engine,
    bool pass_expected_engine = true) {
    const auto original = readLittleEndian(path, offset, width);
    CHECK(original != replacement);
    writeLittleEndian(path, offset, replacement, width);
    expectThrows<ErrorType>([&] {
        if (pass_expected_engine) {
            (void)kvspace::ShmClient::Attach(path, expected_engine);
        } else {
            (void)kvspace::ShmClient::Attach(path);
        }
    });
    writeLittleEndian(path, offset, original, width);
}

void createEmptyFixture(
    const std::string& path,
    kvspace::ShmEngine engine) {
    auto store = kvspace::ShmClient::Create(path, options(engine));
    store->Close();
}

void testDiscriminatorMutationAndCrossedMatrix() {
    for (const auto engine : {
             kvspace::ShmEngine::ArtBump,
             kvspace::ShmEngine::ArtBox,
             kvspace::ShmEngine::HashBox,
             kvspace::ShmEngine::TrieBox}) {
        ScopedPath fixture(uniquePath(
            "discriminator_" +
            std::to_string(static_cast<std::uint32_t>(engine))));
        createEmptyFixture(fixture.get(), engine);

        const auto current_version = readLittleEndian(
            fixture.get(), kVersionOffset, 4);
        const auto current_endian = readLittleEndian(
            fixture.get(), kEndianOffset, 4);
        const auto current_header_size = readLittleEndian(
            fixture.get(), kHeaderSizeOffset, 4);
        const auto current_engine_abi = readLittleEndian(
            fixture.get(), kEngineAbiOffset, 4);
        const auto current_common_hash = readLittleEndian(
            fixture.get(), kCommonLayoutHashOffset, 8);
        const auto current_engine_hash = readLittleEndian(
            fixture.get(), kEngineLayoutHashOffset, 8);
        CHECK(current_version == 4);
        CHECK(current_endian == 0x01020304U);
        CHECK(current_header_size == kCommonHeaderBytes);
        CHECK(current_engine_abi > 1);
        CHECK(current_common_hash == kCommon04Hash);

        // Common crossed pairs: v4/old-hash and v3/COMMON04.
        expectRejectedPatch<kvspace::ErrVersionMismatch>(
            fixture.get(),
            kCommonLayoutHashOffset,
            current_common_hash ^ 0x0100000000000000ULL,
            8,
            engine);
        expectRejectedPatch<kvspace::ErrVersionMismatch>(
            fixture.get(), kVersionOffset, 3, 4, engine);

        // Independent common and engine discriminator mutations.
        expectRejectedPatch<kvspace::ErrVersionMismatch>(
            fixture.get(), kEndianOffset, 0x04030201U, 4, engine);
        expectRejectedPatch<kvspace::ErrVersionMismatch>(
            fixture.get(),
            kHeaderSizeOffset,
            current_header_size - 1U,
            4,
            engine);
        expectRejectedPatch<kvspace::ErrUnsupportedEngine>(
            fixture.get(),
            kEngineIdOffset,
            std::numeric_limits<std::uint32_t>::max(),
            4,
            engine,
            false);
        const auto different_engine = engine == kvspace::ShmEngine::ArtBump
            ? kvspace::ShmEngine::ArtBox
            : kvspace::ShmEngine::ArtBump;
        expectRejectedPatch<kvspace::ErrEngineMismatch>(
            fixture.get(),
            kEngineIdOffset,
            static_cast<std::uint32_t>(different_engine),
            4,
            engine);

        // Engine crossed pairs: old ABI/current hash and current ABI/old hash.
        // This explicitly includes ArtBox ABI2/ABI3-hash, TrieBox
        // ABI3/ABI4-hash, and the ArtBump ABI3/ABI4-hash boundary.
        expectRejectedPatch<kvspace::ErrVersionMismatch>(
            fixture.get(),
            kEngineAbiOffset,
            current_engine_abi - 1U,
            4,
            engine);
        expectRejectedPatch<kvspace::ErrVersionMismatch>(
            fixture.get(),
            kEngineLayoutHashOffset,
            current_engine_hash ^ 0x0000000000000100ULL,
            8,
            engine);

        auto attached = kvspace::ShmClient::Attach(fixture.get(), engine);
        CHECK(attached->Stats().engine == engine);
        attached->Close();
    }
}

void expectLegacyRejectedBeforeBodyRead(
    kvspace::ShmEngine engine,
    bool truncate_to_prefix) {
    ScopedPath fixture(uniquePath(
        truncate_to_prefix ? "legacy_truncated" : "legacy_guarded"));
    createEmptyFixture(fixture.get(), engine);
    const auto current_abi = readLittleEndian(
        fixture.get(), kEngineAbiOffset, 4);
    CHECK(current_abi > 1);
    writeLittleEndian(
        fixture.get(), kEngineAbiOffset, current_abi - 1U, 4);
    if (truncate_to_prefix) {
        const int fd = ::open(fixture.get().c_str(), O_RDWR | O_CLOEXEC);
        CHECK(fd >= 0);
        CHECK(__real_ftruncate(fd, static_cast<off_t>(64)) == 0);
        CHECK(::close(fd) == 0);
    }

    injection::reset();
    injection::guard_prefix_only = true;
    expectThrows<kvspace::ErrVersionMismatch>([&] {
        (void)kvspace::ShmClient::Attach(fixture.get(), engine);
    });
    injection::guard_prefix_only = false;
    CHECK(!injection::forbidden_pread_attempted);
    CHECK(!injection::forbidden_mmap_attempted);
}

void testLegacyDiscriminatorWinsBeforeGuardedOrTruncatedBody() {
    // Both accepted legacy fixed-engine families are exercised in both forms:
    // a full body made inaccessible to pread/mmap and a 64-byte-only body.
    for (const auto engine : {
             kvspace::ShmEngine::ArtBox,
             kvspace::ShmEngine::TrieBox}) {
        expectLegacyRejectedBeforeBodyRead(engine, false);
        expectLegacyRejectedBeforeBodyRead(engine, true);
    }
}

bool filePrefixIsZero(const std::string& path, std::uint64_t* size) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT;
    struct stat status {};
    CHECK(::fstat(fd, &status) == 0);
    CHECK(status.st_size >= 0);
    *size = static_cast<std::uint64_t>(status.st_size);
    const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(
        *size, kCommonHeaderBytes));
    std::vector<std::uint8_t> bytes(wanted);
    if (!bytes.empty()) {
        CHECK(__real_pread(fd, bytes.data(), bytes.size(), 0) ==
              static_cast<ssize_t>(bytes.size()));
    }
    CHECK(::close(fd) == 0);
    return std::all_of(bytes.begin(), bytes.end(),
                       [](std::uint8_t byte) { return byte == 0; });
}

enum class BootstrapFailure {
    Ftruncate,
    Mmap,
};

int runBootstrapFailureChild(
    const std::string& path,
    kvspace::ShmEngine engine,
    BootstrapFailure failure) noexcept {
    try {
        injection::reset();
        injection::fail_ftruncate = failure == BootstrapFailure::Ftruncate;
        injection::fail_mmap = failure == BootstrapFailure::Mmap;
        bool rejected = false;
        try {
            auto store = kvspace::ShmClient::Create(path, options(engine));
            store->Close();
        } catch (const kvspace::Error&) {
            rejected = true;
        }
        if (!rejected) return 20;
        if (failure == BootstrapFailure::Ftruncate) {
            if (injection::ftruncate_calls != 1 ||
                injection::mmap_calls != 0 ||
                !injection::ftruncate_saw_empty_backing) {
                return 21;
            }
        } else if (injection::ftruncate_calls != 1 ||
                   injection::mmap_calls != 1 ||
                   !injection::mmap_saw_zero_prefix) {
            return 22;
        }
        return 0;
    } catch (...) {
        return 23;
    }
}

void testBootstrapFailureMatrixWritesNoHeaderAndDoesNotSignal() {
    for (const auto engine : {
             kvspace::ShmEngine::ArtBump,
             kvspace::ShmEngine::ArtBox,
             kvspace::ShmEngine::HashBox,
             kvspace::ShmEngine::TrieBox}) {
        for (const auto failure : {
                 BootstrapFailure::Ftruncate,
                 BootstrapFailure::Mmap}) {
            ScopedPath fixture(uniquePath(
                failure == BootstrapFailure::Ftruncate
                    ? "fail_ftruncate"
                    : "fail_mmap"));
            const auto child = ::fork();
            CHECK(child >= 0);
            if (child == 0) {
                ::_exit(runBootstrapFailureChild(
                    fixture.get(), engine, failure));
            }
            int status = 0;
            CHECK(::waitpid(child, &status, 0) == child);
            CHECK(WIFEXITED(status));
            CHECK(WEXITSTATUS(status) == 0);

            std::uint64_t file_size = 0;
            CHECK(filePrefixIsZero(fixture.get(), &file_size));
            if (failure == BootstrapFailure::Ftruncate) {
                CHECK(file_size == 0);
            } else {
                CHECK(file_size == options(engine).max_size);
            }
        }
    }
}

void testUnrepresentableRegionFailsBeforeBackingMutation() {
    for (const auto engine : {
             kvspace::ShmEngine::ArtBump,
             kvspace::ShmEngine::ArtBox,
             kvspace::ShmEngine::HashBox,
             kvspace::ShmEngine::TrieBox}) {
        ScopedPath fixture(uniquePath("huge_region"));
        auto configured = options(engine);
        configured.max_size = std::numeric_limits<std::uint64_t>::max();
        injection::reset();
        expectThrows<kvspace::ErrCapacity>([&] {
            (void)kvspace::ShmClient::Create(fixture.get(), configured);
        });
        CHECK(injection::ftruncate_calls == 0);
        CHECK(injection::mmap_calls == 0);
        errno = 0;
        CHECK(::access(fixture.get().c_str(), F_OK) == -1);
        CHECK(errno == ENOENT);
    }
}

void testIndependentlyBuiltCommon04FixtureAttaches() {
    ScopedPath source(uniquePath("common04_source"));
    ScopedPath fixture(uniquePath("common04_fixture"));
    auto store = kvspace::ShmClient::Create(
        source.get(), options(kvspace::ShmEngine::HashBox));
    store->Set("/fixture", kvspace::XValue::Int64(41));
    store->Close();

    const auto source_bytes = readFile(source.get());
    CHECK(source_bytes.size() ==
          options(kvspace::ShmEngine::HashBox).max_size);
    const std::array<std::uint8_t, 8> magic = {
        'K', 'V', 'S', 'H', 'M', '0', '1', '\0'};
    CHECK(std::equal(magic.begin(), magic.end(), source_bytes.begin()));
    CHECK(readLittleEndian(source.get(), kVersionOffset, 4) == 4);
    CHECK(readLittleEndian(source.get(), kHeaderSizeOffset, 4) ==
          kCommonHeaderBytes);
    CHECK(readLittleEndian(source.get(), kCommonLayoutHashOffset, 8) ==
          kCommon04Hash);
    CHECK(readLittleEndian(source.get(), kRegionSizeOffset, 8) ==
          source_bytes.size());
    CHECK(readLittleEndian(source.get(), kRegionMaxOffset, 8) ==
          source_bytes.size());

    const auto fixture_bytes = buildIndependentCommon04Fixture(source_bytes);
    copyFixture(fixture_bytes, fixture.get());
    auto attached = kvspace::ShmClient::Attach(
        fixture.get(), kvspace::ShmEngine::HashBox);
    CHECK(attached->Get("/fixture").AsInt64() == 41);
    attached->Close();
}

} // namespace

int main() {
    return runTest([] {
        testDiscriminatorMutationAndCrossedMatrix();
        testLegacyDiscriminatorWinsBeforeGuardedOrTruncatedBody();
        testBootstrapFailureMatrixWritesNoHeaderAndDoesNotSignal();
        testUnrepresentableRegionFailsBeforeBackingMutation();
        testIndependentlyBuiltCommon04FixtureAttaches();
    });
}
