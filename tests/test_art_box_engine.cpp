#include "test_support.h"

#include "kvspace/errors.h"
#include "kvspace/shm.h"
#include "kvspace/xvalue.h"
#include "shm_region.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kKiB = 1024;
constexpr std::uint64_t kMiB = 1024 * kKiB;
constexpr std::uint64_t kCommonRootOffset = 104;
constexpr std::uint64_t kCommonEngineOffset = 200;
constexpr std::uint32_t kEmptyArtBoxRoot = UINT32_MAX;
constexpr std::uint32_t kLocalIdMask = UINT32_C(0x3fffffff);
constexpr std::array<std::uint32_t, 4> kExpectedPayloadBytes = {
    48, 104, 472, 1048};

struct RawSlabDescriptor {
    std::uint64_t metadata_offset = 0;
    std::uint64_t metadata_bytes = 0;
    std::uint64_t zone_offset = 0;
    std::uint64_t zone_bytes = 0;
};

struct RawArtBoxLayout {
    std::uint64_t page_size = 0;
    std::uint64_t region_size = 0;
    std::uint64_t region_max = 0;
    std::uint64_t common_root = 0;
    std::uint64_t engine_offset = 0;
    std::uint32_t root = kEmptyArtBoxRoot;
    std::array<RawSlabDescriptor, 4> slabs{};
    std::uint64_t box_metadata_offset = 0;
    std::uint64_t box_metadata_bytes = 0;
    std::uint64_t box_data_offset = 0;
    std::uint64_t box_data_bytes = 0;
    std::uint64_t geometry_hash = 0;
    std::uint32_t node_capacity = 0;
    std::uint8_t header_width = 0;
    std::array<std::uint32_t, 4> payload_bytes{};
};

struct ReferenceBoxGeometry {
    std::uint64_t metadata_bytes = 0;
    std::uint64_t data_bytes = 0;
    std::uint64_t node_count = 0;
    std::uint8_t node_header_width = 0;
    std::uint8_t root_level = 0;
    std::uint8_t root_slots = 0;
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

std::size_t asSize(std::uint64_t value) {
    CHECK(value <= std::numeric_limits<std::size_t>::max());
    return static_cast<std::size_t>(value);
}

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) {
    CHECK(alignment != 0);
    CHECK((alignment & (alignment - 1U)) == 0);
    CHECK(value <= std::numeric_limits<std::uint64_t>::max() - (alignment - 1U));
    return (value + alignment - 1U) & ~(alignment - 1U);
}

std::uint8_t referenceBlockHeaderWidth(std::uint64_t capacity) {
    CHECK(capacity != 0);
    CHECK(capacity <= UINT32_MAX);
    if (capacity <= (UINT64_C(1) << 14U)) return 2;
    if (capacity <= (UINT64_C(1) << 30U)) return 4;
    return 8;
}

ReferenceBoxGeometry referenceBoxGeometry(std::uint64_t budget) {
    ReferenceBoxGeometry best;
    std::uint64_t unit_bytes = 8;
    for (std::uint64_t level = 0; unit_bytes <= budget; ++level) {
        CHECK(level <= UINT8_MAX);
        for (std::uint64_t slots = 1; slots <= 15; ++slots) {
            CHECK(unit_bytes <= UINT64_MAX / slots);
            const auto data_bytes = unit_bytes * slots;

            std::uint64_t node_count = 1;
            std::uint64_t nodes_at_depth = slots;
            for (std::uint64_t depth = 0; depth < level; ++depth) {
                CHECK(node_count <= UINT64_MAX - nodes_at_depth);
                node_count += nodes_at_depth;
                if (depth + 1U < level) {
                    CHECK(nodes_at_depth <= UINT64_MAX / 16U);
                    nodes_at_depth *= 16U;
                }
            }
            if (node_count > UINT32_MAX) continue;
            const auto width = referenceBlockHeaderWidth(node_count);
            CHECK(node_count <= UINT64_MAX / (88U + width));
            const auto metadata_bytes =
                192U + node_count * (88U + width);
            if (metadata_bytes <= budget &&
                data_bytes <= budget - metadata_bytes &&
                data_bytes > best.data_bytes) {
                best = ReferenceBoxGeometry{
                    metadata_bytes,
                    data_bytes,
                    node_count,
                    width,
                    static_cast<std::uint8_t>(level),
                    static_cast<std::uint8_t>(slots)};
            }
        }
        if (unit_bytes > UINT64_MAX / 16U) break;
        unit_bytes *= 16U;
    }
    CHECK(best.data_bytes != 0);
    return best;
}

std::vector<std::uint8_t> readFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    struct stat status {};
    CHECK(::fstat(fd, &status) == 0);
    CHECK(status.st_size > 0);
    const auto byte_count = static_cast<std::size_t>(status.st_size);
    std::vector<std::uint8_t> bytes(byte_count);
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

std::uint64_t fileSize(const std::string& path) {
    struct stat status {};
    CHECK(::stat(path.c_str(), &status) == 0);
    CHECK(status.st_size >= 0);
    return static_cast<std::uint64_t>(status.st_size);
}

void writeByte(const std::string& path, std::uint64_t offset, std::uint8_t value) {
    const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(::pwrite(
              fd,
              &value,
              sizeof(value),
              static_cast<off_t>(offset)) ==
          static_cast<ssize_t>(sizeof(value)));
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

template <std::size_t Size>
void expectMagic(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t offset,
    const std::array<std::uint8_t, Size>& magic) {
    const auto begin = asSize(offset);
    CHECK(begin <= bytes.size());
    CHECK(Size <= bytes.size() - begin);
    CHECK(std::equal(magic.begin(), magic.end(), bytes.data() + begin));
}

void expectZeroRange(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t begin_offset,
    std::uint64_t end_offset) {
    CHECK(begin_offset <= end_offset);
    const auto begin = asSize(begin_offset);
    const auto end = asSize(end_offset);
    CHECK(end <= bytes.size());
    CHECK(std::all_of(
        bytes.data() + begin,
        bytes.data() + end,
        [](std::uint8_t value) { return value == 0; }));
}

std::string uniquePath(const std::string& label) {
    static std::uint32_t counter = 0;
    ++counter;
    return "/tmp/kvspace_art_box_engine_" + label + "_" +
        std::to_string(static_cast<long long>(::getpid())) + "_" +
        std::to_string(counter) + ".kvshm";
}

kvspace::ShmOptions artBoxOptions(
    std::uint64_t max_entries,
    std::uint64_t max_size = 8 * kMiB) {
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::ArtBox;
    options.initial_size = 64 * kKiB;
    options.max_size = max_size;
    options.max_entries = max_entries;
    options.max_queues = 1;
    return options;
}

RawArtBoxLayout parseArtBoxLayout(const std::vector<std::uint8_t>& bytes) {
    constexpr std::array<std::uint8_t, 8> common_magic = {
        'K', 'V', 'S', 'H', 'M', '0', '1', 0};
    constexpr std::array<std::uint8_t, 8> art_box_magic = {
        'K', 'V', 'A', 'R', 'T', 'B', '0', '1'};
    CHECK(bytes.size() >= 256);
    expectMagic(bytes, 0, common_magic);
    CHECK(load32(bytes.data() + 8) == 4);
    CHECK(load32(bytes.data() + 24) == 2);
    CHECK(load32(bytes.data() + 28) == 3);
    CHECK(load64(bytes.data() + 32) != 0);
    CHECK(load64(bytes.data() + 40) != 0);

    RawArtBoxLayout layout;
    layout.region_size = load64(bytes.data() + 48);
    layout.region_max = load64(bytes.data() + 56);
    layout.page_size = load64(bytes.data() + 64);
    layout.common_root = load64(bytes.data() + kCommonRootOffset);
    layout.engine_offset = load64(bytes.data() + kCommonEngineOffset);
    CHECK(layout.engine_offset <= bytes.size());
    CHECK(256U <= bytes.size() - asSize(layout.engine_offset));
    expectMagic(bytes, layout.engine_offset, art_box_magic);

    const auto* header = bytes.data() + asSize(layout.engine_offset);
    CHECK(load32(header + 8) == 1);
    CHECK(load32(header + 12) == 256);
    layout.root = load32(header + 16);
    for (std::size_t index = 0; index < layout.slabs.size(); ++index) {
        const auto descriptor_offset = 24U + index * 32U;
        layout.slabs[index] = RawSlabDescriptor{
            load64(header + descriptor_offset),
            load64(header + descriptor_offset + 8U),
            load64(header + descriptor_offset + 16U),
            load64(header + descriptor_offset + 24U)};
    }
    layout.box_metadata_offset = load64(header + 152);
    layout.box_metadata_bytes = load64(header + 160);
    layout.box_data_offset = load64(header + 168);
    layout.box_data_bytes = load64(header + 176);
    layout.geometry_hash = load64(header + 184);
    layout.node_capacity = load32(header + 192);
    layout.header_width = header[196];
    for (std::size_t index = 0; index < layout.payload_bytes.size(); ++index) {
        layout.payload_bytes[index] = load32(header + 200U + index * 4U);
    }
    return layout;
}

void expectFixedBlockMetadata(
    const std::vector<std::uint8_t>& bytes,
    const RawSlabDescriptor& slab,
    std::uint32_t expected_capacity,
    std::uint32_t expected_payload,
    std::uint8_t expected_width,
    bool expect_empty) {
    constexpr std::array<std::uint8_t, 8> block_magic = {
        'K', 'V', 'B', 'L', 'O', 'C', 'K', '1'};
    expectMagic(bytes, slab.metadata_offset, block_magic);
    CHECK(slab.metadata_bytes == 64);
    CHECK(slab.metadata_offset + slab.metadata_bytes == slab.zone_offset);
    const auto* metadata = bytes.data() + asSize(slab.metadata_offset);
    CHECK(load32(metadata + 8) == 1);
    CHECK(load32(metadata + 12) == 64);
    CHECK(load64(metadata + 16) == slab.zone_bytes);
    CHECK(load64(metadata + 24) == expected_payload);
    CHECK(load32(metadata + 32) == expected_capacity);
    if (expect_empty) {
        CHECK(load32(metadata + 36) == 0);
        CHECK(load32(metadata + 40) == 0);
        CHECK(load32(metadata + 44) == UINT32_MAX);
    }
    CHECK(load64(metadata + 48) != 0);
    CHECK(metadata[56] == expected_width);
    expectZeroRange(bytes, slab.metadata_offset + 57U, slab.metadata_offset + 64U);
}

void expectBoxMetadata(
    const std::vector<std::uint8_t>& bytes,
    const RawArtBoxLayout& layout,
    const ReferenceBoxGeometry& expected) {
    constexpr std::array<std::uint8_t, 8> box_magic = {
        'K', 'V', 'B', 'O', 'X', 'A', '0', '1'};
    constexpr std::array<std::uint8_t, 8> block_magic = {
        'K', 'V', 'B', 'L', 'O', 'C', 'K', '1'};
    expectMagic(bytes, layout.box_metadata_offset, box_magic);
    const auto* box = bytes.data() + asSize(layout.box_metadata_offset);
    CHECK(load32(box + 8) == 1);
    CHECK(load32(box + 12) == 128);
    CHECK(load64(box + 16) == layout.box_metadata_bytes);
    CHECK(load64(box + 24) == layout.box_data_bytes);
    CHECK(load64(box + 32) == 128);
    CHECK(load64(box + 40) == 192);
    const auto block_zone_bytes = load64(box + 48);
    CHECK(block_zone_bytes ==
          expected.node_count * (88U + expected.node_header_width));
    CHECK(load32(box + 56) == 88);
    CHECK(load32(box + 60) == 0);
    CHECK(box[64] == expected.root_level);
    CHECK(box[65] == expected.root_slots);
    expectZeroRange(
        bytes, layout.box_metadata_offset + 66U, layout.box_metadata_offset + 72U);
    CHECK(load64(box + 72) != 0);
    expectZeroRange(
        bytes, layout.box_metadata_offset + 80U, layout.box_metadata_offset + 128U);

    const auto fixed_offset = layout.box_metadata_offset + 128U;
    expectMagic(bytes, fixed_offset, block_magic);
    const auto* fixed = bytes.data() + asSize(fixed_offset);
    CHECK(load32(fixed + 8) == 1);
    CHECK(load32(fixed + 12) == 64);
    CHECK(load64(fixed + 16) == block_zone_bytes);
    CHECK(load64(fixed + 24) == 88);
    CHECK(load32(fixed + 32) == expected.node_count);
    CHECK(load32(fixed + 36) == 1);
    CHECK(load32(fixed + 40) == 1);
    CHECK(load32(fixed + 44) == UINT32_MAX);
    CHECK(load64(fixed + 48) != 0);
    CHECK(fixed[56] == expected.node_header_width);
    expectZeroRange(bytes, fixed_offset + 57U, fixed_offset + 64U);
    CHECK(layout.box_metadata_offset + 192U <= layout.box_data_offset);
}

RawArtBoxLayout expectFreshArtBoxLayout(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t max_entries) {
    const auto layout = parseArtBoxLayout(bytes);
    CHECK(layout.region_size == bytes.size());
    CHECK(layout.region_max == bytes.size());
    CHECK(layout.common_root == 0);
    CHECK(layout.root == kEmptyArtBoxRoot);
    CHECK(layout.page_size != 0);
    CHECK((layout.page_size & (layout.page_size - 1U)) == 0);
    CHECK(layout.geometry_hash != 0);
    CHECK(max_entries <= (UINT32_MAX - 1U) / 5U);
    const auto expected_capacity = static_cast<std::uint32_t>(
        max_entries * 5U + 1U);
    CHECK(layout.node_capacity == expected_capacity);
    CHECK(layout.header_width == 2);
    CHECK(layout.payload_bytes == kExpectedPayloadBytes);

    expectZeroRange(
        bytes, layout.engine_offset + 20U, layout.engine_offset + 24U);
    expectZeroRange(
        bytes, layout.engine_offset + 197U, layout.engine_offset + 200U);
    expectZeroRange(
        bytes, layout.engine_offset + 216U, layout.engine_offset + 256U);

    auto cursor = alignUp(layout.engine_offset + 256U, layout.page_size);
    CHECK(layout.slabs[0].metadata_offset == cursor);
    expectZeroRange(bytes, layout.engine_offset + 256U, cursor);
    for (std::size_t index = 0; index < layout.slabs.size(); ++index) {
        const auto& slab = layout.slabs[index];
        CHECK(slab.metadata_offset == cursor);
        const auto expected_zone_bytes =
            static_cast<std::uint64_t>(expected_capacity) *
            (static_cast<std::uint64_t>(layout.header_width) +
             layout.payload_bytes[index]);
        CHECK(slab.zone_bytes == expected_zone_bytes);
        expectFixedBlockMetadata(
            bytes,
            slab,
            expected_capacity,
            layout.payload_bytes[index],
            layout.header_width,
            true);
        const auto zone_end = slab.zone_offset + slab.zone_bytes;
        cursor = alignUp(zone_end, 64);
        const auto next_section = index + 1U < layout.slabs.size()
            ? layout.slabs[index + 1U].metadata_offset
            : layout.box_metadata_offset;
        CHECK(next_section == cursor);
        expectZeroRange(bytes, zone_end, cursor);
    }

    CHECK(layout.box_metadata_offset == cursor);
    const auto expected_box = referenceBoxGeometry(
        layout.region_max - layout.box_metadata_offset);
    CHECK(layout.box_metadata_bytes == expected_box.metadata_bytes);
    CHECK(layout.box_data_bytes == expected_box.data_bytes);
    CHECK(layout.box_data_offset ==
          layout.box_metadata_offset + layout.box_metadata_bytes);
    CHECK(layout.box_data_offset + layout.box_data_bytes <= layout.region_max);
    expectBoxMetadata(bytes, layout, expected_box);
    expectZeroRange(
        bytes,
        layout.box_data_offset + layout.box_data_bytes,
        layout.region_max);
    return layout;
}

std::uint32_t rootKindTag(std::uint32_t root) {
    CHECK(root != kEmptyArtBoxRoot);
    return root >> 30U;
}

std::uint64_t rootPayloadOffset(const RawArtBoxLayout& layout) {
    CHECK(layout.root != kEmptyArtBoxRoot);
    const auto kind_index = static_cast<std::size_t>(rootKindTag(layout.root));
    CHECK(kind_index < layout.slabs.size());
    const auto local_id = layout.root & kLocalIdMask;
    const auto stride = static_cast<std::uint64_t>(layout.header_width) +
        layout.payload_bytes[kind_index];
    return layout.slabs[kind_index].zone_offset +
        static_cast<std::uint64_t>(local_id) * stride + layout.header_width;
}

std::uint32_t currentRoot(const std::string& path) {
    return parseArtBoxLayout(readFile(path)).root;
}

void expectAttachCorrupt(const std::string& path) {
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(kvspace::ShmClient::Attach(
            path, kvspace::ShmEngine::ArtBox));
    });
}

void testExactHeaderGeometryFixedExtentAndReopen() {
    const auto path = uniquePath("layout");
    ScopedRegion scoped(path);
    const auto options = artBoxOptions(17);
    {
        auto store = kvspace::ShmClient::Create(path, options);
        const auto stats = store->Stats();
        CHECK(stats.engine == kvspace::ShmEngine::ArtBox);
        CHECK(stats.region_size == options.max_size);
        CHECK(stats.region_max == options.max_size);
        CHECK(stats.entries == 0);
        CHECK(stats.engine_nodes == 0);
        store->Close();
    }
    CHECK(fileSize(path) == options.max_size);
    const auto first_bytes = readFile(path);
    const auto first_layout = expectFreshArtBoxLayout(first_bytes, 17);
    CHECK(first_layout.engine_offset + 256U <= first_layout.slabs[0].metadata_offset);

    {
        auto reopened = kvspace::ShmClient::Open(path, options);
        CHECK(reopened->Stats().engine == kvspace::ShmEngine::ArtBox);
        CHECK(reopened->Stats().region_size == options.max_size);
        reopened->Close();
    }
    const auto reopened_layout = parseArtBoxLayout(readFile(path));
    CHECK(reopened_layout.root == kEmptyArtBoxRoot);
    CHECK(reopened_layout.common_root == 0);
    CHECK(reopened_layout.geometry_hash == first_layout.geometry_hash);

    const auto truncated_path = uniquePath("truncated");
    ScopedRegion truncated_scoped(truncated_path);
    {
        auto store = kvspace::ShmClient::Create(truncated_path, options);
        store->Close();
    }
    const int fd = ::open(truncated_path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    CHECK(::ftruncate(fd, static_cast<off_t>(options.initial_size)) == 0);
    CHECK(::close(fd) == 0);
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        try {
            static_cast<void>(kvspace::ShmClient::Attach(
                truncated_path, kvspace::ShmEngine::ArtBox));
            ::_exit(41);
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

void testIncompleteBackingPreservesTerminalTail() {
    auto options = artBoxOptions(17);
    options.initial_size = options.max_size;

    const auto normal_path = uniquePath("normal_full_initial");
    ScopedRegion normal_scoped(normal_path);
    {
        auto store = kvspace::ShmClient::Create(normal_path, options);
        CHECK(store->Stats().region_size == options.max_size);
        store->Close();
    }
    const auto normal_layout = expectFreshArtBoxLayout(
        readFile(normal_path), options.max_entries);
    const auto terminal_offset = normal_layout.box_data_offset +
        normal_layout.box_data_bytes;
    constexpr std::array<std::uint8_t, 8> tail_canary = {
        0x71, 0x29, 0xe4, 0x5b, 0x9d, 0x03, 0xc6, 0xaf};
    constexpr std::array<std::uint8_t, 8> node_data_canary = {
        0x18, 0xd3, 0x47, 0xbc, 0x62, 0x0f, 0xa9, 0x75};
    constexpr std::array<std::uint8_t, 8> box_data_canary = {
        0xc1, 0x56, 0x2a, 0x8e, 0xf4, 0x39, 0x07, 0xbd};
    CHECK(terminal_offset <= options.max_size);
    CHECK(tail_canary.size() <= options.max_size - terminal_offset);
    CHECK(node_data_canary.size() <= normal_layout.slabs[0].zone_bytes);
    CHECK(box_data_canary.size() <= normal_layout.box_data_bytes);

    const auto interrupted_path = uniquePath("incomplete_tail");
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
    writeByte(interrupted_path, kCommonRootOffset, 0xa5);
    writeByte(interrupted_path, normal_layout.engine_offset + 216U, 0xa5);
    const auto gap_offset = normal_layout.engine_offset + 256U;
    CHECK(gap_offset < normal_layout.slabs[0].metadata_offset);
    writeByte(interrupted_path, gap_offset, 0xa5);
    writeByte(
        interrupted_path,
        normal_layout.slabs[0].metadata_offset + 57U,
        0xa5);
    writeByte(
        interrupted_path,
        normal_layout.box_metadata_offset + 80U,
        0xa5);
    writeBytes(
        interrupted_path,
        normal_layout.slabs[0].zone_offset,
        node_data_canary);
    writeBytes(
        interrupted_path,
        normal_layout.box_data_offset,
        box_data_canary);
    writeBytes(interrupted_path, terminal_offset, tail_canary);

    {
        auto store = kvspace::ShmClient::Open(interrupted_path, options);
        const auto stats = store->Stats();
        CHECK(stats.engine == kvspace::ShmEngine::ArtBox);
        CHECK(stats.region_size == options.max_size);
        CHECK(stats.entries == 0);
        store->Close();
    }

    const auto bytes = readFile(interrupted_path);
    const auto layout = parseArtBoxLayout(bytes);
    CHECK(layout.engine_offset == normal_layout.engine_offset);
    CHECK(layout.box_metadata_offset == normal_layout.box_metadata_offset);
    CHECK(layout.box_data_offset == normal_layout.box_data_offset);
    CHECK(layout.box_data_bytes == normal_layout.box_data_bytes);
    CHECK(layout.common_root == 0);
    CHECK(bytes[asSize(layout.engine_offset + 216U)] == 0);
    CHECK(bytes[asSize(gap_offset)] == 0);
    CHECK(bytes[asSize(layout.slabs[0].metadata_offset + 57U)] == 0);
    CHECK(bytes[asSize(layout.box_metadata_offset + 80U)] == 0);
    for (std::size_t index = 0; index < node_data_canary.size(); ++index) {
        CHECK(
            bytes[asSize(layout.slabs[0].zone_offset) + index] ==
            node_data_canary[index]);
    }
    for (std::size_t index = 0; index < box_data_canary.size(); ++index) {
        CHECK(
            bytes[asSize(layout.box_data_offset) + index] ==
            box_data_canary[index]);
    }
    for (std::size_t index = 0; index < tail_canary.size(); ++index) {
        CHECK(bytes[asSize(terminal_offset) + index] == tail_canary[index]);
    }

    auto attached = kvspace::ShmClient::Attach(
        interrupted_path, kvspace::ShmEngine::ArtBox);
    attached->Set("/usable", kvspace::XValue::Int64(9));
    CHECK(attached->Get("/usable").AsInt64() == 9);
}

void testTaggedRootAndBiasedBoxReferences() {
    const auto none_path = uniquePath("none");
    ScopedRegion none_scoped(none_path);
    const auto options = artBoxOptions(8);
    auto none_region = kvspace::detail::Region::Open(
        none_path,
        options,
        kvspace::detail::OpenMode::Create,
        kvspace::ShmEngine::ArtBox);
    {
        auto guard = none_region->Lock();
        none_region->Put("/none", {});
        CHECK(none_region->Exists("/none"));
        std::vector<std::uint8_t> encoded;
        CHECK(none_region->Get("/none", &encoded));
        CHECK(encoded.empty());
        (void)guard;
    }
    auto none_bytes = readFile(none_path);
    const auto none_layout = parseArtBoxLayout(none_bytes);
    CHECK(none_layout.common_root == 0);
    CHECK(none_layout.root == 0); // Node4 tag zero, allocator-local ID zero.
    const auto none_payload = rootPayloadOffset(none_layout);
    CHECK(load64(none_bytes.data() + asSize(none_payload + 8U)) == 0);
    CHECK(none_bytes[asSize(none_payload + 23U)] == 1);
    none_region.reset();
    {
        auto attached = kvspace::detail::Region::Open(
            none_path,
            {},
            kvspace::detail::OpenMode::Attach,
            kvspace::ShmEngine::ArtBox);
        auto guard = attached->Lock();
        CHECK(attached->Exists("/none"));
        (void)guard;
    }

    const auto value_path = uniquePath("offset_zero");
    ScopedRegion value_scoped(value_path);
    auto value_region = kvspace::detail::Region::Open(
        value_path,
        options,
        kvspace::detail::OpenMode::Create,
        kvspace::ShmEngine::ArtBox);
    const auto expected = kvspace::XValue::Int64(7).Encode();
    {
        auto guard = value_region->Lock();
        value_region->Put("/value", expected);
        (void)guard;
    }
    auto value_bytes = readFile(value_path);
    auto value_layout = parseArtBoxLayout(value_bytes);
    CHECK(value_layout.root == 0);
    const auto first_root = value_layout.root;
    auto value_payload = rootPayloadOffset(value_layout);
    CHECK(load64(value_bytes.data() + asSize(value_payload + 8U)) == 1);
    CHECK(value_bytes[asSize(value_payload + 23U)] == 1);
    {
        auto guard = value_region->Lock();
        value_region->Put("x", {});
        (void)guard;
    }
    value_bytes = readFile(value_path);
    value_layout = parseArtBoxLayout(value_bytes);
    CHECK(value_layout.common_root == 0);
    CHECK(value_layout.root != kEmptyArtBoxRoot);
    CHECK(value_layout.root != first_root);
    CHECK(rootKindTag(value_layout.root) == 0);
    value_region.reset();
    auto attached = kvspace::detail::Region::Open(
        value_path,
        {},
        kvspace::detail::OpenMode::Attach,
        kvspace::ShmEngine::ArtBox);
    {
        auto guard = attached->Lock();
        std::vector<std::uint8_t> actual;
        CHECK(attached->Get("/value", &actual));
        CHECK(actual == expected);
        (void)guard;
    }
}

void testGrowthAndShrinkBoundaries() {
    const auto path = uniquePath("shape");
    ScopedRegion scoped(path);
    const auto options = artBoxOptions(64);
    auto region = kvspace::detail::Region::Open(
        path,
        options,
        kvspace::detail::OpenMode::Create,
        kvspace::ShmEngine::ArtBox);
    std::vector<std::string> keys;
    keys.reserve(49);
    for (std::size_t index = 0; index < 49; ++index) {
        std::string key = "/";
        key.push_back(static_cast<char>(33U + index));
        keys.push_back(std::move(key));
    }

    {
        auto guard = region->Lock();
        for (std::size_t index = 0; index < keys.size(); ++index) {
            region->Put(keys[index], {});
            if (index == 0) CHECK(rootKindTag(currentRoot(path)) == 0);
            if (index == 4) CHECK(rootKindTag(currentRoot(path)) == 1);
            if (index == 16) CHECK(rootKindTag(currentRoot(path)) == 2);
            if (index == 48) CHECK(rootKindTag(currentRoot(path)) == 3);
        }
        CHECK(region->Stats().entries == 49);

        for (std::size_t index = 49; index > 37; --index) {
            CHECK(region->Erase(keys[index - 1U]));
        }
        CHECK(region->Stats().entries == 37);
        CHECK(rootKindTag(currentRoot(path)) == 2);

        for (std::size_t index = 37; index > 16; --index) {
            CHECK(region->Erase(keys[index - 1U]));
        }
        CHECK(region->Stats().entries == 16);
        CHECK(rootKindTag(currentRoot(path)) == 1);

        for (std::size_t index = 16; index > 4; --index) {
            CHECK(region->Erase(keys[index - 1U]));
        }
        CHECK(region->Stats().entries == 4);
        CHECK(rootKindTag(currentRoot(path)) == 0);
        (void)guard;
    }
    region.reset();
    auto attached = kvspace::detail::Region::Open(
        path,
        {},
        kvspace::detail::OpenMode::Attach,
        kvspace::ShmEngine::ArtBox);
    {
        auto guard = attached->Lock();
        CHECK(attached->Stats().entries == 4);
        for (std::size_t index = 0; index < 4; ++index) {
            CHECK(attached->Exists(keys[index]));
        }
        (void)guard;
    }
}

void testBatchRollbackAndCapacityFailureRemainAttachable() {
    const auto rollback_path = uniquePath("rollback");
    ScopedRegion rollback_scoped(rollback_path);
    const auto options = artBoxOptions(8);
    auto region = kvspace::detail::Region::Open(
        rollback_path,
        options,
        kvspace::detail::OpenMode::Create,
        kvspace::ShmEngine::ArtBox);
    const auto stable_value = kvspace::XValue::Int64(11).Encode();
    {
        auto guard = region->Lock();
        region->Put("/stable", stable_value);
        const auto committed_root = currentRoot(rollback_path);
        {
            auto mutation = region->BeginMutation();
            region->Put("/first", {});
            region->Put("/second", kvspace::XValue::Int64(22).Encode());
            CHECK(region->Erase("/stable"));
            CHECK(region->Exists("/first"));
            CHECK(region->Exists("/second"));
            CHECK(!region->Exists("/stable"));
            (void)mutation;
        }
        CHECK(currentRoot(rollback_path) == committed_root);
        CHECK(region->Exists("/stable"));
        CHECK(!region->Exists("/first"));
        CHECK(!region->Exists("/second"));
        CHECK(region->Stats().entries == 1);
        (void)guard;
    }
    region.reset();
    {
        auto attached = kvspace::detail::Region::Open(
            rollback_path,
            {},
            kvspace::detail::OpenMode::Attach,
            kvspace::ShmEngine::ArtBox);
        auto guard = attached->Lock();
        std::vector<std::uint8_t> actual;
        CHECK(attached->Get("/stable", &actual));
        CHECK(actual == stable_value);
        CHECK(!attached->Exists("/first"));
        CHECK(!attached->Exists("/second"));
        (void)guard;
    }

    const auto capacity_path = uniquePath("capacity");
    ScopedRegion capacity_scoped(capacity_path);
    const auto capacity_options = artBoxOptions(2);
    auto capacity_region = kvspace::detail::Region::Open(
        capacity_path,
        capacity_options,
        kvspace::detail::OpenMode::Create,
        kvspace::ShmEngine::ArtBox);
    {
        auto guard = capacity_region->Lock();
        capacity_region->Put("/stable", stable_value);
        const auto committed_root = currentRoot(capacity_path);
        {
            auto mutation = capacity_region->BeginMutation();
            capacity_region->Put("/transient", {});
            expectThrows<kvspace::ErrCapacity>([&] {
                capacity_region->Put("/overflow", {});
            });
            (void)mutation;
        }
        CHECK(currentRoot(capacity_path) == committed_root);
        CHECK(capacity_region->Exists("/stable"));
        CHECK(!capacity_region->Exists("/transient"));
        CHECK(!capacity_region->Exists("/overflow"));
        CHECK(capacity_region->Stats().entries == 1);
        (void)guard;
    }
    capacity_region.reset();
    auto capacity_attached = kvspace::detail::Region::Open(
        capacity_path,
        {},
        kvspace::detail::OpenMode::Attach,
        kvspace::ShmEngine::ArtBox);
    {
        auto guard = capacity_attached->Lock();
        CHECK(capacity_attached->Exists("/stable"));
        capacity_attached->Put("/after", {});
        CHECK(capacity_attached->Exists("/after"));
        CHECK(capacity_attached->Stats().entries == 2);
        (void)guard;
    }
}

void testRawCorruptionIsRejectedOnAttach() {
    const auto options = artBoxOptions(8);
    {
        const auto path = uniquePath("reserved_corrupt");
        ScopedRegion scoped(path);
        auto store = kvspace::ShmClient::Create(path, options);
        store->Close();
        const auto layout = parseArtBoxLayout(readFile(path));
        writeByte(path, layout.engine_offset + 216U, 1);
        expectAttachCorrupt(path);
    }
    {
        const auto path = uniquePath("gap_corrupt");
        ScopedRegion scoped(path);
        auto store = kvspace::ShmClient::Create(path, options);
        store->Close();
        const auto layout = parseArtBoxLayout(readFile(path));
        const auto gap_begin = layout.engine_offset + 256U;
        CHECK(gap_begin < layout.slabs[0].metadata_offset);
        writeByte(path, gap_begin, 1);
        expectAttachCorrupt(path);
    }
    {
        const auto path = uniquePath("key_corrupt");
        ScopedRegion scoped(path);
        auto region = kvspace::detail::Region::Open(
            path,
            options,
            kvspace::detail::OpenMode::Create,
            kvspace::ShmEngine::ArtBox);
        {
            auto guard = region->Lock();
            region->Put("/a", {});
            region->Put("/b", {});
            (void)guard;
        }
        region.reset();
        const auto bytes = readFile(path);
        const auto layout = parseArtBoxLayout(bytes);
        CHECK(rootKindTag(layout.root) == 0);
        const auto payload = rootPayloadOffset(layout);
        CHECK(load32(bytes.data() + asSize(payload + 16U)) == 1);
        CHECK(bytes[asSize(payload + 20U)] == 2);
        CHECK(bytes[asSize(payload + 24U)] < bytes[asSize(payload + 25U)]);
        writeByte(path, payload + 25U, bytes[asSize(payload + 24U)]);
        expectAttachCorrupt(path);
    }
}

void runArtBoxEngineTests() {
    testExactHeaderGeometryFixedExtentAndReopen();
    testIncompleteBackingPreservesTerminalTail();
    testTaggedRootAndBiasedBoxReferences();
    testGrowthAndShrinkBoundaries();
    testBatchRollbackAndCapacityFailureRemainAttachable();
    testRawCorruptionIsRejectedOnAttach();
}

} // namespace

int main() {
    return runTest(runArtBoxEngineTests);
}
