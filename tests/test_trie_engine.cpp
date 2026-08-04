#include "test_support.h"

#include "kvspace/errors.h"
#include "kvspace/shm.h"
#include "kvspace/xvalue.h"
#include "shm_box_allocators.h"
#include "shm_region.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uint64_t kTrieEngineLayoutGolden =
    UINT64_C(0xb0d26c5bfe3d94c7);
constexpr std::uint64_t kLegacyTrieEngineLayoutHash =
    UINT64_C(0x52574f404a5f53b2);
constexpr std::uint64_t kCommonEngineLayoutHashOffset = 40;
constexpr std::uint64_t kCommonRootOffset = 104;
constexpr std::uint64_t kCommonNodeCountOffset = 112;
constexpr std::uint64_t kCommonEntryCountOffset = 120;
constexpr std::uint64_t kCommonTombstoneCountOffset = 128;
constexpr std::uint64_t kCommonEngineLiveBytesOffset = 216;
constexpr std::uint64_t kCommonActiveTableOffset = 1360;
constexpr std::uint64_t kCommonMutexOffset = 1368;
constexpr std::uint64_t kCommonMutexBytes = 40;

std::size_t recovery_prepare_events = 0;
std::size_t recovery_apply_events = 0;

void recoveryBoundaryHook(
    kvspace::detail::RegionTestEvent event) noexcept {
    if (event ==
        kvspace::detail::RegionTestEvent::RecoveryPrepareStarting) {
        ++recovery_prepare_events;
    }
    if (event == kvspace::detail::RegionTestEvent::RecoveryApplyStarting) {
        ++recovery_apply_events;
    }
}

// Exact engine-layout fingerprint input for the supported native ABI:
// RegionHeader=1472, pthread_mutex_t=40, pthread_cond_t=48. Every numeric
// field is an eight-byte little-endian value.
constexpr std::array<std::uint8_t, 88> kTrieEngineLayoutInput = {
    'T', 'R', 'I', 'E', 'B', 'O', 'X', '2',
    0xc0, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // RegionHeader
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Trie header
    0x08, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Trie node
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // FixedBlock
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Box header
    0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // Box node
    0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // QueueSlot
    0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // BlobHeader
    0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // mutex
    0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // condition
};

std::uint32_t load32(const std::uint8_t* data) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(data[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

std::uint64_t load64(const std::uint8_t* data) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(data[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

void store32(std::uint8_t* data, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        data[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
}

void store64(std::uint8_t* data, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        data[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
}

void hashByte(std::uint64_t* hash, std::uint8_t byte) noexcept {
    *hash ^= byte;
    *hash *= 1099511628211ULL;
}

std::uint64_t trieEngineLayoutHashFromGoldenBytes() noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : kTrieEngineLayoutInput) hashByte(&hash, byte);
    return hash;
}

void hash32(std::uint64_t* hash, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        hashByte(
            hash,
            static_cast<std::uint8_t>(
                value >> static_cast<unsigned>(index * 8U)));
    }
}

void hash64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        hashByte(
            hash,
            static_cast<std::uint8_t>(
                value >> static_cast<unsigned>(index * 8U)));
    }
}

std::uint64_t trieHeaderHash(const std::uint8_t* header) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (std::size_t index = 0; index < 8; ++index) {
        hashByte(&hash, header[index]);
    }
    hash32(&hash, 1);
    hash64(&hash, 128);
    for (const std::size_t offset : {
             24U, 32U, 40U, 48U, 56U, 64U, 72U, 80U}) {
        hash64(&hash, load64(header + offset));
    }
    return hash;
}

std::uint64_t blockGeometryHash(
    std::uint64_t zone_bytes,
    std::uint32_t capacity,
    std::uint8_t header_width) noexcept {
    constexpr std::array<std::uint8_t, 8> block_magic = {
        'K', 'V', 'B', 'L', 'O', 'C', 'K', '1'};
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : block_magic) hashByte(&hash, byte);
    hash32(&hash, 1);
    hash32(&hash, 64);
    hash64(&hash, zone_bytes);
    hash64(&hash, 1032);
    hash32(&hash, capacity);
    hashByte(&hash, header_width);
    return hash;
}

std::uint64_t alignUpForTest(
    std::uint64_t value,
    std::uint64_t alignment) {
    CHECK(alignment != 0 && (alignment & (alignment - 1U)) == 0);
    CHECK(value <= UINT64_MAX - (alignment - 1U));
    return (value + alignment - 1U) & ~(alignment - 1U);
}

std::vector<std::uint8_t> readFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    struct stat status {};
    CHECK(::fstat(fd, &status) == 0);
    CHECK(status.st_size > 0);
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(status.st_size));
    std::size_t total = 0;
    while (total < bytes.size()) {
        const auto count = ::pread(
            fd,
            bytes.data() + total,
            bytes.size() - total,
            static_cast<off_t>(total));
        CHECK(count > 0);
        total += static_cast<std::size_t>(count);
    }
    CHECK(::close(fd) == 0);
    return bytes;
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

template <std::size_t Size>
void writeBytes(
    const std::string& path,
    std::uint64_t offset,
    const std::array<std::uint8_t, Size>& bytes) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(::pwrite(
              fd,
              bytes.data(),
              bytes.size(),
              static_cast<off_t>(offset)) ==
          static_cast<ssize_t>(bytes.size()));
    CHECK(::close(fd) == 0);
}

std::size_t findMagic(
    const std::vector<std::uint8_t>& bytes,
    const std::array<std::uint8_t, 8>& magic) {
    const auto found = std::search(
        bytes.begin(), bytes.end(), magic.begin(), magic.end());
    CHECK(found != bytes.end());
    return static_cast<std::size_t>(found - bytes.begin());
}

std::uint32_t trieChild(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t node_zone,
    std::uint64_t stride,
    std::uint64_t header_width,
    std::uint32_t node_id,
    std::uint8_t edge) {
    const auto payload = node_zone +
        static_cast<std::uint64_t>(node_id) * stride + header_width;
    const auto child_offset = payload + 8U +
        static_cast<std::uint64_t>(edge) * sizeof(std::uint32_t);
    CHECK(child_offset + sizeof(std::uint32_t) <= bytes.size());
    return load32(bytes.data() + child_offset);
}

std::uint64_t trieValueWord(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t node_zone,
    std::uint64_t stride,
    std::uint64_t header_width,
    std::uint32_t root,
    const std::string& key) {
    auto node = root;
    for (const char raw : key) {
        node = trieChild(
            bytes,
            node_zone,
            stride,
            header_width,
            node,
            static_cast<std::uint8_t>(static_cast<unsigned char>(raw)));
        CHECK(node != UINT32_MAX);
    }
    const auto payload = node_zone +
        static_cast<std::uint64_t>(node) * stride + header_width;
    CHECK(payload + sizeof(std::uint64_t) <= bytes.size());
    return load64(bytes.data() + payload);
}

struct RawTrieLayout {
    std::uint64_t region_max = 0;
    std::uint64_t page_size = 0;
    std::uint64_t engine_offset = 0;
    std::size_t trie_header = 0;
    std::uint64_t node_metadata = 0;
    std::uint64_t node_metadata_bytes = 0;
    std::uint64_t node_zone = 0;
    std::uint64_t node_zone_bytes = 0;
    std::uint64_t node_stride = 0;
    std::uint64_t block_header_width = 0;
    std::uint64_t box_metadata = 0;
    std::uint64_t box_metadata_bytes = 0;
    std::uint64_t box_data = 0;
    std::uint64_t box_data_bytes = 0;
    std::uint32_t node_capacity = 0;
    std::uint32_t root = 0;
};

RawTrieLayout inspectLayout(const std::vector<std::uint8_t>& bytes) {
    constexpr std::array<std::uint8_t, 8> trie_magic = {
        'K', 'V', 'T', 'R', 'I', 'E', '0', '1'};
    constexpr std::array<std::uint8_t, 8> block_magic = {
        'K', 'V', 'B', 'L', 'O', 'C', 'K', '1'};
    constexpr std::array<std::uint8_t, 8> box_magic = {
        'K', 'V', 'B', 'O', 'X', 'A', '0', '1'};
    RawTrieLayout layout;
    CHECK(bytes.size() >= 64U);
    layout.region_max = load64(bytes.data() + 56U);
    layout.page_size = load64(bytes.data() + 64U);
    layout.engine_offset = load64(bytes.data() + 200U);
    layout.trie_header = findMagic(bytes, trie_magic);
    CHECK(layout.trie_header == layout.engine_offset);
    CHECK(layout.trie_header + 128U <= bytes.size());
    const auto* header = bytes.data() + layout.trie_header;
    CHECK(load32(header + 8) == 1);
    CHECK(load32(header + 12) == 128);
    layout.root = load32(header + 16);
    layout.node_metadata = load64(header + 24);
    layout.node_metadata_bytes = load64(header + 32);
    layout.node_zone = load64(header + 40);
    layout.node_zone_bytes = load64(header + 48);
    layout.box_metadata = load64(header + 56);
    layout.box_metadata_bytes = load64(header + 64);
    layout.box_data = load64(header + 72);
    layout.box_data_bytes = load64(header + 80);
    CHECK(layout.node_metadata + 64U <= bytes.size());
    CHECK(layout.box_metadata + 128U <= bytes.size());
    CHECK(std::equal(
        block_magic.begin(),
        block_magic.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(layout.node_metadata)));
    CHECK(std::equal(
        box_magic.begin(),
        box_magic.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(layout.box_metadata)));
    const auto payload_bytes = load64(
        bytes.data() + layout.node_metadata + 24U);
    CHECK(payload_bytes == 1032);
    layout.node_capacity = load32(
        bytes.data() + layout.node_metadata + 32U);
    layout.block_header_width =
        bytes[static_cast<std::size_t>(layout.node_metadata + 56U)];
    CHECK(layout.block_header_width == 2 ||
          layout.block_header_width == 4 ||
          layout.block_header_width == 8);
    layout.node_stride = payload_bytes + layout.block_header_width;
    return layout;
}

std::vector<std::uint8_t> readFileIgnoringMutex(
    const std::string& path) {
    auto bytes = readFile(path);
    CHECK(kCommonMutexOffset <= bytes.size());
    CHECK(kCommonMutexBytes <=
          bytes.size() - static_cast<std::size_t>(kCommonMutexOffset));
    std::fill(
        bytes.begin() + static_cast<std::ptrdiff_t>(kCommonMutexOffset),
        bytes.begin() + static_cast<std::ptrdiff_t>(
            kCommonMutexOffset + kCommonMutexBytes),
        0);
    return bytes;
}

bool physicalNodeIsAllocated(
    std::vector<std::uint8_t>* bytes,
    const RawTrieLayout& layout,
    std::uint32_t node_id) {
    CHECK(bytes != nullptr);
    const auto byte_count = static_cast<std::uint64_t>(bytes->size());
    CHECK(layout.node_metadata <= byte_count);
    CHECK(layout.node_metadata_bytes <= byte_count - layout.node_metadata);
    CHECK(layout.node_zone <= byte_count);
    CHECK(layout.node_zone_bytes <= byte_count - layout.node_zone);
    auto nodes = kvspace::detail::FixedBlockAllocator::Attach(
        bytes->data() + static_cast<std::size_t>(layout.node_metadata),
        static_cast<std::size_t>(layout.node_metadata_bytes),
        bytes->data() + static_cast<std::size_t>(layout.node_zone),
        static_cast<std::size_t>(layout.node_zone_bytes));
    CHECK(nodes.UsedCount() ==
          load32(bytes->data() + layout.node_metadata + 40U));
    if (node_id >= nodes.HighWater()) return false;
    return nodes.IsAllocated(node_id);
}

void expectTrieLogicalState(
    const std::string& path,
    std::uint64_t nodes,
    std::uint64_t entries,
    std::uint64_t live_bytes,
    std::uint32_t physical_nodes) {
    auto bytes = readFile(path);
    const auto layout = inspectLayout(bytes);
    CHECK(load64(bytes.data() + kCommonRootOffset) == 0);
    CHECK(load64(bytes.data() + kCommonNodeCountOffset) == nodes);
    CHECK(load64(bytes.data() + kCommonEntryCountOffset) == entries);
    CHECK(load64(bytes.data() + kCommonTombstoneCountOffset) == 0);
    CHECK(load64(bytes.data() + kCommonEngineLiveBytesOffset) == live_bytes);
    CHECK(load32(bytes.data() + kCommonActiveTableOffset) == 0);
    CHECK(layout.root < layout.node_capacity);
    CHECK(load32(bytes.data() + layout.node_metadata + 40U) ==
          physical_nodes);
    CHECK(physicalNodeIsAllocated(&bytes, layout, layout.root));
}

void attachTrieSuccessfully(const std::string& path) {
    auto attached = kvspace::ShmClient::Attach(
        path, kvspace::ShmEngine::TrieBox);
    attached->Close();
}

void leaveTrieMutexOwnerDead(const std::string& path) {
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        try {
            auto region = kvspace::detail::Region::Open(
                path,
                {},
                kvspace::detail::OpenMode::Attach,
                kvspace::ShmEngine::TrieBox);
            auto guard = region->Lock();
            (void)guard;
            ::_exit(73);
        } catch (...) {
            ::_exit(74);
        }
    }
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 73);
}

kvspace::ShmOptions trieGeometryOptions() {
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::TrieBox;
    options.initial_size = 64 * 1024;
    options.max_size = 1024 * 1024;
    options.max_entries = 16;
    options.max_queues = 1;
    return options;
}

void testTrieBoxEngineLayoutFingerprintGolden() {
    constexpr std::array<std::uint8_t, 8> header_length_le64 = {
        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(std::equal(
        header_length_le64.begin(),
        header_length_le64.end(),
        kTrieEngineLayoutInput.begin() + 16));
    CHECK(trieEngineLayoutHashFromGoldenBytes() == kTrieEngineLayoutGolden);

    const auto path = "/tmp/kvspace_trie_layout_hash_golden_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    auto store = kvspace::ShmClient::Create(path, trieGeometryOptions());
    store->Close();

    const auto bytes = readFile(path);
    CHECK(bytes.size() >= 64U);
    CHECK(load32(bytes.data() + 20U) == 1472U);
    CHECK(load32(bytes.data() + 24U) == 4U);
    CHECK(load32(bytes.data() + 28U) == 4U);
    CHECK(load64(bytes.data() + kCommonEngineLayoutHashOffset) ==
          kTrieEngineLayoutGolden);

    auto attached = kvspace::ShmClient::Attach(
        path, kvspace::ShmEngine::TrieBox);
    CHECK(attached->Stats().entries == 0);
}

void testTrieBoxRejectsLegacyAndCorruptEngineLayoutHashes() {
    constexpr std::array<std::uint64_t, 2> rejected_hashes = {
        kLegacyTrieEngineLayoutHash,
        kTrieEngineLayoutGolden ^ UINT64_C(1),
    };
    const auto suffix = std::to_string(static_cast<long long>(::getpid()));
    for (std::size_t index = 0; index < rejected_hashes.size(); ++index) {
        const auto path = "/tmp/kvspace_trie_layout_hash_reject_" +
            std::to_string(index) + "_" + suffix + ".kvshm";
        ScopedRegion scoped(path);
        auto store = kvspace::ShmClient::Create(path, trieGeometryOptions());
        store->Close();
        const auto bytes = readFile(path);
        CHECK(load64(bytes.data() + kCommonEngineLayoutHashOffset) ==
              kTrieEngineLayoutGolden);
        writeLittleEndian(
            path,
            kCommonEngineLayoutHashOffset,
            rejected_hashes[index],
            8);
        expectThrows<kvspace::ErrVersionMismatch>([&] {
            static_cast<void>(kvspace::ShmClient::Attach(path));
        });
    }
}

void testCanonicalTrieBoxHeaderAndGeometry() {
    const auto path = "/tmp/kvspace_trie_header_golden_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    const auto options = trieGeometryOptions();
    auto store = kvspace::ShmClient::Create(path, options);
    store->Close();

    const auto bytes = readFile(path);
    const auto layout = inspectLayout(bytes);
    CHECK(layout.root == 0);
    const auto payload_begin = alignUpForTest(
        layout.engine_offset + 128U, layout.page_size);
    const auto payload_bytes = layout.region_max - payload_begin;
    const auto node_budget =
        (payload_bytes / 2U / layout.page_size) * layout.page_size;
    const auto box_budget = payload_bytes - node_budget;
    const auto node_metadata = payload_begin;
    const auto node_metadata_bytes = UINT64_C(64);
    const auto node_zone = node_metadata + node_metadata_bytes;
    const auto node_zone_bytes = node_budget - node_metadata_bytes;
    const auto box_metadata = payload_begin + node_budget;
    const auto box_data_bytes =
        kvspace::detail::BoxAllocator::LargestFullyRepresentableDataSize(
            box_budget);
    const auto box_metadata_bytes =
        kvspace::detail::BoxAllocator::MinimumMetadataBytesForFullExpansion(
            box_data_bytes);
    const auto box_data = box_metadata + box_metadata_bytes;

    CHECK(layout.node_metadata == node_metadata);
    CHECK(layout.node_metadata_bytes == node_metadata_bytes);
    CHECK(layout.node_zone == node_zone);
    CHECK(layout.node_zone_bytes == node_zone_bytes);
    CHECK(layout.box_metadata == box_metadata);
    CHECK(layout.box_metadata_bytes == box_metadata_bytes);
    CHECK(layout.box_data == box_data);
    CHECK(layout.box_data_bytes == box_data_bytes);
    CHECK(box_data + box_data_bytes <= layout.region_max);

    constexpr std::array<std::uint8_t, 8> trie_magic = {
        'K', 'V', 'T', 'R', 'I', 'E', '0', '1'};
    std::array<std::uint8_t, 128> expected{};
    std::copy(trie_magic.begin(), trie_magic.end(), expected.begin());
    store32(expected.data() + 8U, 1);
    store32(expected.data() + 12U, 128);
    store32(expected.data() + 16U, 0);
    store64(expected.data() + 24U, node_metadata);
    store64(expected.data() + 32U, node_metadata_bytes);
    store64(expected.data() + 40U, node_zone);
    store64(expected.data() + 48U, node_zone_bytes);
    store64(expected.data() + 56U, box_metadata);
    store64(expected.data() + 64U, box_metadata_bytes);
    store64(expected.data() + 72U, box_data);
    store64(expected.data() + 80U, box_data_bytes);
    store64(expected.data() + 88U, trieHeaderHash(expected.data()));
    CHECK(std::equal(
        expected.begin(),
        expected.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(layout.trie_header)));

    auto attached = kvspace::ShmClient::Attach(
        path, kvspace::ShmEngine::TrieBox);
    CHECK(attached->Stats().entries == 0);
}

void testTrieBoxRejectsCorruptImmutableGeometryHash() {
    const auto path = "/tmp/kvspace_trie_geometry_hash_reject_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    auto store = kvspace::ShmClient::Create(path, trieGeometryOptions());
    store->Close();
    const auto bytes = readFile(path);
    const auto layout = inspectLayout(bytes);
    const auto stored_hash = load64(bytes.data() + layout.trie_header + 88U);
    writeLittleEndian(
        path, layout.engine_offset + 88U, stored_hash ^ UINT64_C(1), 8);
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(kvspace::ShmClient::Attach(path));
    });
}

void testTrieBoxRejectsEveryNoncanonicalDescriptor() {
    constexpr std::array<std::uint64_t, 8> descriptor_offsets = {
        24, 32, 40, 48, 56, 64, 72, 80};
    const auto suffix = std::to_string(static_cast<long long>(::getpid()));
    for (std::size_t index = 0; index < descriptor_offsets.size(); ++index) {
        const auto path = "/tmp/kvspace_trie_descriptor_" +
            std::to_string(index) + "_" + suffix + ".kvshm";
        ScopedRegion scoped(path);
        auto store = kvspace::ShmClient::Create(path, trieGeometryOptions());
        store->Close();
        const auto bytes = readFile(path);
        const auto layout = inspectLayout(bytes);
        std::array<std::uint8_t, 128> header{};
        std::copy_n(
            bytes.data() + layout.trie_header,
            header.size(),
            header.data());
        const auto field_offset = descriptor_offsets[index];
        const auto changed = load64(
            header.data() + static_cast<std::size_t>(field_offset)) + 1U;
        store64(
            header.data() + static_cast<std::size_t>(field_offset), changed);
        store64(header.data() + 88U, trieHeaderHash(header.data()));
        writeLittleEndian(
            path, layout.engine_offset + field_offset, changed, 8);
        writeLittleEndian(
            path, layout.engine_offset + 88U, load64(header.data() + 88U), 8);
        expectThrows<kvspace::ErrCorruptRegion>([&] {
            static_cast<void>(kvspace::ShmClient::Attach(path));
        });
    }
}

void testTrieBoxRejectsCoherentNoncanonicalNodeZone() {
    const auto path = "/tmp/kvspace_trie_coherent_geometry_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    auto store = kvspace::ShmClient::Create(path, trieGeometryOptions());
    store->Close();
    const auto bytes = readFile(path);
    const auto layout = inspectLayout(bytes);
    CHECK(layout.root == 0);
    CHECK(layout.node_capacity > 1);
    const auto stride = layout.node_stride;
    CHECK(layout.node_zone_bytes > stride);
    const auto smaller_zone_bytes = layout.node_zone_bytes - stride;
    const auto smaller_capacity = static_cast<std::uint32_t>(
        smaller_zone_bytes / stride);
    CHECK(smaller_capacity + 1U == layout.node_capacity);

    std::array<std::uint8_t, 128> header{};
    std::copy_n(
        bytes.data() + layout.trie_header,
        header.size(),
        header.data());
    store64(header.data() + 48U, smaller_zone_bytes);
    store64(header.data() + 88U, trieHeaderHash(header.data()));
    writeLittleEndian(
        path, layout.engine_offset + 48U, smaller_zone_bytes, 8);
    writeLittleEndian(
        path, layout.engine_offset + 88U, load64(header.data() + 88U), 8);

    // Keep the inner FixedBlock record coherent with the smaller zone. Its
    // fresh root 0, high-water 1, used-count 1, and free-head sentinel remain
    // valid. The former Attach logic accepted the resulting larger gap.
    writeLittleEndian(path, layout.node_metadata + 16U, smaller_zone_bytes, 8);
    writeLittleEndian(path, layout.node_metadata + 32U, smaller_capacity, 4);
    writeLittleEndian(
        path,
        layout.node_metadata + 48U,
        blockGeometryHash(
            smaller_zone_bytes,
            smaller_capacity,
            static_cast<std::uint8_t>(layout.block_header_width)),
        8);
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(kvspace::ShmClient::Attach(path));
    });
}

void testTrieBoxRejectsNonzeroHeaderAlignmentGap() {
    const auto path = "/tmp/kvspace_trie_header_gap_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    auto store = kvspace::ShmClient::Create(path, trieGeometryOptions());
    store->Close();
    const auto layout = inspectLayout(readFile(path));
    const auto gap_begin = layout.engine_offset + 128U;
    CHECK(gap_begin < layout.node_metadata);
    constexpr std::array<std::uint8_t, 1> nonzero = {0xa5};
    writeBytes(path, gap_begin, nonzero);
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(kvspace::ShmClient::Attach(path));
    });
}

void testIncompleteBackingPreservesTerminalTail() {
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::TrieBox;
    options.max_size = 1024 * 1024;
    options.initial_size = options.max_size;
    options.max_entries = 16;
    options.max_queues = 1;

    const auto suffix = std::to_string(static_cast<long long>(::getpid()));
    const auto normal_path =
        "/tmp/kvspace_trie_normal_full_initial_" + suffix + ".kvshm";
    ScopedRegion normal_scoped(normal_path);
    {
        auto store = kvspace::ShmClient::Create(normal_path, options);
        CHECK(store->Stats().region_size == options.max_size);
        store->Close();
    }
    const auto normal_bytes = readFile(normal_path);
    const auto normal_layout = inspectLayout(normal_bytes);
    const auto terminal_offset = normal_layout.box_data +
        normal_layout.box_data_bytes;
    constexpr std::array<std::uint8_t, 8> tail_canary = {
        0xb8, 0x14, 0x6d, 0xf2, 0x39, 0xa7, 0x50, 0xce};
    constexpr std::array<std::uint8_t, 8> node_data_canary = {
        0x43, 0xf9, 0x05, 0x7a, 0xd1, 0x2e, 0xb6, 0x8c};
    constexpr std::array<std::uint8_t, 8> box_data_canary = {
        0x6e, 0x21, 0xad, 0x58, 0x03, 0xc7, 0xf4, 0x9b};
    CHECK(normal_layout.region_max == options.max_size);
    CHECK(terminal_offset <= normal_layout.region_max);
    CHECK(tail_canary.size() <= normal_layout.region_max - terminal_offset);
    CHECK(normal_layout.node_capacity > 1);
    CHECK(node_data_canary.size() <= normal_layout.node_stride);
    CHECK(box_data_canary.size() <= normal_layout.box_data_bytes);
    const auto terminal = static_cast<std::size_t>(terminal_offset);
    for (std::size_t index = 0; index < tail_canary.size(); ++index) {
        CHECK(normal_bytes[terminal + index] == 0);
    }

    const auto interrupted_path =
        "/tmp/kvspace_trie_incomplete_tail_" + suffix + ".kvshm";
    ScopedRegion interrupted_scoped(interrupted_path);
    const int fd = ::open(
        interrupted_path.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC,
        0600);
    CHECK(fd >= 0);
    CHECK(::ftruncate(fd, static_cast<off_t>(options.max_size)) == 0);
    CHECK(::close(fd) == 0);

    constexpr std::array<std::uint8_t, 8> common_magic = {
        'K', 'V', 'S', 'H', 'M', '0', '1', 0};
    writeBytes(interrupted_path, 0, common_magic);
    writeLittleEndian(interrupted_path, 104U, UINT64_C(0xa5), 8);
    writeLittleEndian(
        interrupted_path,
        static_cast<std::uint64_t>(normal_layout.trie_header) + 96U,
        UINT64_C(0xa5),
        8);
    const auto gap_offset =
        static_cast<std::uint64_t>(normal_layout.trie_header) + 128U;
    CHECK(gap_offset < normal_layout.node_metadata);
    writeLittleEndian(interrupted_path, gap_offset, UINT64_C(0xa5), 8);
    writeLittleEndian(
        interrupted_path,
        normal_layout.node_metadata + 57U,
        UINT64_C(0xa5),
        4);
    writeLittleEndian(
        interrupted_path,
        normal_layout.box_metadata + 80U,
        UINT64_C(0xa5),
        8);
    writeBytes(
        interrupted_path,
        normal_layout.node_zone + normal_layout.node_stride,
        node_data_canary);
    writeBytes(
        interrupted_path,
        normal_layout.box_data,
        box_data_canary);
    writeBytes(interrupted_path, terminal_offset, tail_canary);

    {
        auto store = kvspace::ShmClient::Open(interrupted_path, options);
        const auto stats = store->Stats();
        CHECK(stats.engine == kvspace::ShmEngine::TrieBox);
        CHECK(stats.region_size == options.max_size);
        CHECK(stats.entries == 0);
        store->Close();
    }

    const auto bytes = readFile(interrupted_path);
    const auto layout = inspectLayout(bytes);
    CHECK(layout.trie_header == normal_layout.trie_header);
    CHECK(layout.node_metadata == normal_layout.node_metadata);
    CHECK(layout.box_metadata == normal_layout.box_metadata);
    CHECK(layout.box_data == normal_layout.box_data);
    CHECK(layout.box_data_bytes == normal_layout.box_data_bytes);
    CHECK(load64(bytes.data() + 104U) == 0);
    CHECK(bytes[layout.trie_header + 96U] == 0);
    CHECK(bytes[static_cast<std::size_t>(gap_offset)] == 0);
    CHECK(bytes[static_cast<std::size_t>(layout.node_metadata + 57U)] == 0);
    CHECK(bytes[static_cast<std::size_t>(layout.box_metadata + 80U)] == 0);
    const auto unused_node = static_cast<std::size_t>(
        layout.node_zone + layout.node_stride);
    const auto box_data = static_cast<std::size_t>(layout.box_data);
    for (std::size_t index = 0; index < node_data_canary.size(); ++index) {
        CHECK(bytes[unused_node + index] == node_data_canary[index]);
    }
    for (std::size_t index = 0; index < box_data_canary.size(); ++index) {
        CHECK(bytes[box_data + index] == box_data_canary[index]);
    }
    for (std::size_t index = 0; index < tail_canary.size(); ++index) {
        CHECK(bytes[terminal + index] == tail_canary[index]);
    }

    auto attached = kvspace::ShmClient::Attach(
        interrupted_path, kvspace::ShmEngine::TrieBox);
    attached->Set("/usable", kvspace::XValue::Int64(11));
    CHECK(attached->Get("/usable").AsInt64() == 11);
}

void testProductionTrieBoxLayoutAndReclamation() {
    const auto path = "/tmp/kvspace_trie_engine_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::TrieBox;
    options.initial_size = 64 * 1024;
    options.max_size = 16 * 1024 * 1024;
    options.max_entries = 64;
    options.max_queues = 8;
    auto region = kvspace::detail::Region::Open(
        path, options, kvspace::detail::OpenMode::Create);
    {
        auto guard = region->Lock();
        region->Put("/none", {});
        const auto first_value = kvspace::XValue::Int64(1).Encode();
        region->Put("/value", first_value);
        CHECK(region->Exists("/none"));
        std::vector<std::uint8_t> none;
        CHECK(region->Get("/none", &none));
        CHECK(none.empty());
        std::vector<std::uint8_t> value;
        CHECK(region->Get("/value", &value));
        CHECK(value == first_value);
        (void)guard;
    }

    auto bytes = readFile(path);
    auto layout = inspectLayout(bytes);
    const auto none_word = trieValueWord(
        bytes,
        layout.node_zone,
        layout.node_stride,
        layout.block_header_width,
        layout.root,
        "/none");
    const auto value_word = trieValueWord(
        bytes,
        layout.node_zone,
        layout.node_stride,
        layout.block_header_width,
        layout.root,
        "/value");
    CHECK(none_word == 1); // has_value=true, biased Box ref=0: stored None.
    CHECK(value_word == 3); // has_value=true, biased ref=1: Box offset zero.

    {
        auto guard = region->Lock();
        for (std::int64_t value = 2; value <= 501; ++value) {
            region->Put("/value", kvspace::XValue::Int64(value).Encode());
        }
        CHECK(region->Stats().entries == 2);
        CHECK(region->Stats().engine_nodes == 11);
        (void)guard;
    }
    bytes = readFile(path);
    layout = inspectLayout(bytes);
    CHECK(load32(bytes.data() + layout.node_metadata + 40U) == 11);

    region.reset();
    auto attached = kvspace::ShmClient::Attach(
        path, kvspace::ShmEngine::TrieBox);
    CHECK(attached->Get("/value").AsInt64() == 501);
    CHECK(attached->Get("/none").IsNull());
    attached->Clear();
    CHECK(attached->Stats().entries == 0);
    CHECK(attached->Stats().engine_nodes == 0);
    CHECK(attached->Stats().heap_used == 0);
    attached->Close();

    bytes = readFile(path);
    layout = inspectLayout(bytes);
    CHECK(load32(bytes.data() + layout.node_metadata + 40U) == 1);
}

void testProductionTrieBoxNodeCapacityErrorsArePublicAndStrong() {
    const auto put_path = "/tmp/kvspace_trie_put_capacity_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    {
        ScopedRegion scoped(put_path);
        kvspace::ShmOptions options;
        options.engine = kvspace::ShmEngine::TrieBox;
        options.initial_size = 64 * 1024;
        options.max_size = 256 * 1024;
        options.max_entries = 256;
        options.max_queues = 1;
        auto region = kvspace::detail::Region::Open(
            put_path, options, kvspace::detail::OpenMode::Create);
        const auto layout = inspectLayout(readFile(put_path));
        CHECK(layout.node_capacity > 16);
        const std::string impossible_key = "/" + std::string(
            static_cast<std::size_t>(layout.node_capacity) + 1U, 'x');
        {
            auto guard = region->Lock();
            expectThrows<kvspace::ErrCapacity>([&] {
                region->Put(impossible_key, {});
            });
            CHECK(!region->Exists(impossible_key));
            region->Put("/ok", kvspace::XValue::Int64(1).Encode());
            std::vector<std::uint8_t> value;
            CHECK(region->Get("/ok", &value));
            CHECK(kvspace::XValue::Decode(value).AsInt64() == 1);
            (void)guard;
        }
        region.reset();
        auto attached = kvspace::ShmClient::Attach(
            put_path, kvspace::ShmEngine::TrieBox);
        expectThrows<kvspace::ErrCapacity>([&] {
            attached->Set(impossible_key, kvspace::XValue::Null());
        });
        CHECK(attached->Get(impossible_key).IsNull());
        CHECK(attached->Get("/ok").AsInt64() == 1);
    }

    const auto erase_path = "/tmp/kvspace_trie_erase_capacity_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(erase_path);
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::TrieBox;
    options.initial_size = 64 * 1024;
    options.max_size = 256 * 1024;
    options.max_entries = 256;
    options.max_queues = 1;
    auto region = kvspace::detail::Region::Open(
        erase_path, options, kvspace::detail::OpenMode::Create);
    const auto layout = inspectLayout(readFile(erase_path));
    CHECK(layout.node_capacity > 48);
    const auto target_length = std::min<std::size_t>(
        24, static_cast<std::size_t>(layout.node_capacity / 4U));
    CHECK(target_length > 8);
    const std::string target = "/" + std::string(target_length - 1U, 'a');
    const auto target_value = kvspace::XValue::Int64(7).Encode();

    {
        auto guard = region->Lock();
        region->Put(target, target_value);
        for (std::size_t depth = 1; depth < target.size(); ++depth) {
            region->Put(target.substr(0, depth) + 'b', {});
        }

        bool reached_capacity = false;
        for (std::size_t index = 0; index < 512; ++index) {
            try {
                region->Put("/f/" + std::to_string(index), {});
            } catch (const kvspace::ErrCapacity&) {
                reached_capacity = true;
                break;
            }
        }
        CHECK(reached_capacity);
        expectThrows<kvspace::ErrCapacity>([&] {
            static_cast<void>(region->Erase(target));
        });
        std::vector<std::uint8_t> value;
        CHECK(region->Get(target, &value));
        CHECK(value == target_value);
        (void)guard;
    }

    region.reset();
    {
        auto attached = kvspace::detail::Region::Open(
            erase_path, {}, kvspace::detail::OpenMode::Attach,
            kvspace::ShmEngine::TrieBox);
        auto guard = attached->Lock();
        std::vector<std::uint8_t> value;
        CHECK(attached->Get(target, &value));
        CHECK(value == target_value);
        (void)guard;
    }
    auto client = kvspace::ShmClient::Attach(
        erase_path, kvspace::ShmEngine::TrieBox);
    expectThrows<kvspace::ErrCapacity>([&] { client->Del(target); });
    CHECK(client->Get(target).AsInt64() == 7);
}

void testTrieBoxLogicalCountersAndRetainedPhysicalRootMatrix() {
    const auto suffix = std::to_string(static_cast<long long>(::getpid()));

    {
        const auto path =
            "/tmp/kvspace_trie_counter_fresh_" + suffix + ".kvshm";
        ScopedRegion scoped(path);
        auto store = kvspace::ShmClient::Create(path, trieGeometryOptions());
        store->Close();
        expectTrieLogicalState(path, 0, 0, 0, 1);
        attachTrieSuccessfully(path);
    }

    {
        const auto path =
            "/tmp/kvspace_trie_counter_cleared_" + suffix + ".kvshm";
        ScopedRegion scoped(path);
        auto store = kvspace::ShmClient::Create(path, trieGeometryOptions());
        store->Set("/before-clear", kvspace::XValue::Int64(1));
        store->Clear();
        store->Close();
        expectTrieLogicalState(path, 0, 0, 0, 1);
        attachTrieSuccessfully(path);
    }

    {
        const auto path =
            "/tmp/kvspace_trie_counter_root_none_" + suffix + ".kvshm";
        ScopedRegion scoped(path);
        auto region = kvspace::detail::Region::Open(
            path,
            trieGeometryOptions(),
            kvspace::detail::OpenMode::Create);
        {
            auto guard = region->Lock();
            region->Put("", {});
            (void)guard;
        }
        region.reset();
        expectTrieLogicalState(path, 1, 1, 1032, 1);
        attachTrieSuccessfully(path);
    }

    {
        const auto path =
            "/tmp/kvspace_trie_counter_root_value_" + suffix + ".kvshm";
        ScopedRegion scoped(path);
        const auto encoded = kvspace::XValue::Int64(7).Encode();
        auto region = kvspace::detail::Region::Open(
            path,
            trieGeometryOptions(),
            kvspace::detail::OpenMode::Create);
        {
            auto guard = region->Lock();
            region->Put("", encoded);
            (void)guard;
        }
        region.reset();
        const auto live = UINT64_C(1032) +
            kvspace::detail::BoxAllocator::RoundSize(encoded.size());
        expectTrieLogicalState(path, 1, 1, live, 1);
        attachTrieSuccessfully(path);
    }

    {
        const auto path =
            "/tmp/kvspace_trie_counter_prune_" + suffix + ".kvshm";
        ScopedRegion scoped(path);
        const std::string value_key = "/deep/a";
        const std::string none_key = "/deep/b";
        const auto encoded = kvspace::XValue::Int64(8).Encode();
        auto region = kvspace::detail::Region::Open(
            path,
            trieGeometryOptions(),
            kvspace::detail::OpenMode::Create);
        {
            auto guard = region->Lock();
            region->Put(value_key, encoded);
            region->Put(none_key, {});
            (void)guard;
        }
        // Reachable prefixes are root, /, /d, /de, /dee, /deep, /deep/,
        // /deep/a, and /deep/b. Stored None counts as an entry but not a Box.
        const auto deep_live = UINT64_C(9) * UINT64_C(1032) +
            kvspace::detail::BoxAllocator::RoundSize(encoded.size());
        expectTrieLogicalState(path, 9, 2, deep_live, 9);
        {
            auto guard = region->Lock();
            CHECK(region->Erase(value_key));
            CHECK(region->Erase(none_key));
            (void)guard;
        }
        region.reset();
        expectTrieLogicalState(path, 0, 0, 0, 1);
        attachTrieSuccessfully(path);
    }
}

void testTrieBoxCleanAttachRejectsCounterCorruptionWithoutWrites() {
    constexpr std::array<std::uint64_t, 3> counter_offsets = {
        kCommonNodeCountOffset,
        kCommonEntryCountOffset,
        kCommonEngineLiveBytesOffset,
    };
    const auto suffix = std::to_string(static_cast<long long>(::getpid()));
    for (std::size_t index = 0; index < counter_offsets.size(); ++index) {
        const auto path = "/tmp/kvspace_trie_counter_corrupt_" +
            std::to_string(index) + "_" + suffix + ".kvshm";
        ScopedRegion scoped(path);
        auto store = kvspace::ShmClient::Create(path, trieGeometryOptions());
        store->Set("/value", kvspace::XValue::Int64(9));
        store->Close();
        const auto clean = readFile(path);
        const auto offset = counter_offsets[index];
        const auto original = load64(clean.data() + offset);
        writeLittleEndian(path, offset, original + 1U, 8);
        const auto before = readFileIgnoringMutex(path);
        expectThrows<kvspace::ErrCorruptRegion>([&] {
            static_cast<void>(kvspace::ShmClient::Attach(
                path, kvspace::ShmEngine::TrieBox));
        });
        CHECK(readFileIgnoringMutex(path) == before);
    }
}

void testTrieBoxRejectsPhysicallyUnallocatedCommittedRootBeforeWrites() {
    const auto suffix = std::to_string(static_cast<long long>(::getpid()));
    {
        const auto path =
            "/tmp/kvspace_trie_unallocated_root_clean_" + suffix +
            ".kvshm";
        ScopedRegion scoped(path);
        auto store = kvspace::ShmClient::Create(path, trieGeometryOptions());
        store->Close();
        auto bytes = readFile(path);
        const auto layout = inspectLayout(bytes);
        CHECK(layout.root == 0);
        CHECK(layout.node_capacity > 1);
        CHECK(!physicalNodeIsAllocated(&bytes, layout, 1));
        writeLittleEndian(path, layout.trie_header + 16U, 1, 4);
        const auto before = readFileIgnoringMutex(path);
        expectThrows<kvspace::ErrCorruptRegion>([&] {
            static_cast<void>(kvspace::ShmClient::Attach(
                path, kvspace::ShmEngine::TrieBox));
        });
        CHECK(readFileIgnoringMutex(path) == before);
    }

    {
        const auto path =
            "/tmp/kvspace_trie_unallocated_root_recovery_" + suffix +
            ".kvshm";
        ScopedRegion scoped(path);
        auto store = kvspace::ShmClient::Create(path, trieGeometryOptions());
        leaveTrieMutexOwnerDead(path);
        auto bytes = readFile(path);
        const auto layout = inspectLayout(bytes);
        CHECK(layout.root == 0);
        CHECK(layout.node_capacity > 1);
        CHECK(!physicalNodeIsAllocated(&bytes, layout, 1));
        writeLittleEndian(path, layout.trie_header + 16U, 1, 4);
        const auto before = readFileIgnoringMutex(path);
        recovery_prepare_events = 0;
        recovery_apply_events = 0;
        kvspace::detail::SetRegionTestHook(recoveryBoundaryHook);
        expectThrows<kvspace::ErrCorruptRegion>([&] {
            static_cast<void>(store->Get("/unrelated"));
        });
        kvspace::detail::SetRegionTestHook(nullptr);
        CHECK(recovery_prepare_events == 1);
        CHECK(recovery_apply_events == 0);
        CHECK(readFileIgnoringMutex(path) == before);
    }
}

void testCommonVersionFourRejectsOldDiscriminators() {
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::TrieBox;
    options.initial_size = 64 * 1024;
    options.max_size = 256 * 1024;
    options.max_entries = 16;
    options.max_queues = 1;

    const auto version_path = "/tmp/kvspace_old_common_version_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    {
        ScopedRegion scoped(version_path);
        auto store = kvspace::ShmClient::Create(version_path, options);
        store->Close();
        writeLittleEndian(version_path, 8, 3, 4);
        expectThrows<kvspace::ErrVersionMismatch>([&] {
            static_cast<void>(kvspace::ShmClient::Attach(version_path));
        });
    }

    const auto hash_path = "/tmp/kvspace_old_common_hash_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(hash_path);
    auto store = kvspace::ShmClient::Create(hash_path, options);
    store->Close();
    constexpr std::uint64_t old_common_hash =
        0x434f4d4d4f4e3033ULL ^ (64ULL << 1U);
    writeLittleEndian(hash_path, 32, old_common_hash, 8);
    expectThrows<kvspace::ErrVersionMismatch>([&] {
        static_cast<void>(kvspace::ShmClient::Attach(hash_path));
    });
}

void testTrieBoxRejectsInvalidFixedExtentAndIdleJournal() {
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::TrieBox;
    options.initial_size = 64 * 1024;
    options.max_size = 256 * 1024;
    options.max_entries = 16;
    options.max_queues = 1;

    const auto short_size_path = "/tmp/kvspace_short_region_size_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    {
        ScopedRegion scoped(short_size_path);
        auto store = kvspace::ShmClient::Create(short_size_path, options);
        store->Close();
        writeLittleEndian(short_size_path, 48, options.initial_size, 8);
        expectThrows<kvspace::ErrCorruptRegion>([&] {
            static_cast<void>(kvspace::ShmClient::Attach(short_size_path));
        });
    }

    const auto truncated_path = "/tmp/kvspace_truncated_extent_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    {
        ScopedRegion scoped(truncated_path);
        auto store = kvspace::ShmClient::Create(truncated_path, options);
        store->Close();
        writeLittleEndian(truncated_path, 48, options.initial_size, 8);
        const int fd = ::open(truncated_path.c_str(), O_RDWR | O_CLOEXEC);
        CHECK(fd >= 0);
        CHECK(::ftruncate(fd, static_cast<off_t>(options.initial_size)) == 0);
        CHECK(::close(fd) == 0);

        const auto child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            try {
                static_cast<void>(kvspace::ShmClient::Attach(truncated_path));
                ::_exit(44);
            } catch (const kvspace::ErrCorruptRegion&) {
                ::_exit(42);
            } catch (...) {
                ::_exit(43);
            }
        }
        int status = 0;
        CHECK(::waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status));
        CHECK(WEXITSTATUS(status) == 42);
    }

    const auto reserved_path = "/tmp/kvspace_idle_journal_reserved_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(reserved_path);
    auto store = kvspace::ShmClient::Create(reserved_path, options);
    store->Close();
    writeLittleEndian(
        reserved_path,
        kvspace::detail::RegionAllocatorJournalOffsetForTest() + 40U,
        1,
        8);
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(kvspace::ShmClient::Attach(reserved_path));
    });
}

} // namespace

int main() {
    return runTest([] {
        testTrieBoxEngineLayoutFingerprintGolden();
        testTrieBoxRejectsLegacyAndCorruptEngineLayoutHashes();
        testCanonicalTrieBoxHeaderAndGeometry();
        testTrieBoxRejectsCorruptImmutableGeometryHash();
        testTrieBoxRejectsEveryNoncanonicalDescriptor();
        testTrieBoxRejectsCoherentNoncanonicalNodeZone();
        testTrieBoxRejectsNonzeroHeaderAlignmentGap();
        testIncompleteBackingPreservesTerminalTail();
        testProductionTrieBoxLayoutAndReclamation();
        testProductionTrieBoxNodeCapacityErrorsArePublicAndStrong();
        testTrieBoxLogicalCountersAndRetainedPhysicalRootMatrix();
        testTrieBoxCleanAttachRejectsCounterCorruptionWithoutWrites();
        testTrieBoxRejectsPhysicallyUnallocatedCommittedRootBeforeWrites();
        testCommonVersionFourRejectsOldDiscriminators();
        testTrieBoxRejectsInvalidFixedExtentAndIdleJournal();
    });
}
