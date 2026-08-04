#include "test_support.h"

#include "kvspace/errors.h"
#include "kvspace/shm.h"
#include "kvspace/xvalue.h"
#include "shm_region.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uint64_t kHashEngineLayoutGolden =
    UINT64_C(0xa23025d5ba9cdc0d);
constexpr std::uint64_t kLegacyHashEngineLayoutHash =
    UINT64_C(0x9e5729d93f0a7795);
constexpr std::uint64_t kEngineLayoutHashOffset = 40;
constexpr std::uint64_t kRegionMaxOffset = 56;
constexpr std::uint64_t kPageSizeOffset = 64;
constexpr std::uint64_t kEntryLimitOffset = 72;
constexpr std::uint64_t kTableCapacityOffset = 80;
constexpr std::uint64_t kTableZeroOffset = 88;
constexpr std::uint64_t kTableOneOffset = 96;
constexpr std::uint64_t kEntryCountOffset = 120;
constexpr std::uint64_t kTombstoneCountOffset = 128;
constexpr std::uint64_t kQueueCapacityOffset = 144;
constexpr std::uint64_t kQueueOffset = 152;
constexpr std::uint64_t kHeapOffset = 168;
constexpr std::uint64_t kHeapTopOffset = 176;
constexpr std::uint64_t kHeapLimitOffset = 192;
constexpr std::uint64_t kEngineOffset = 200;
constexpr std::uint64_t kEngineSizeOffset = 208;
constexpr std::uint64_t kEngineLiveBytesOffset = 216;
constexpr std::uint64_t kActiveTableOffset = 1360;
constexpr std::uint64_t kRegionHeaderBytes = 1472;

// Exact engine-layout fingerprint input for the supported native ABI.
// Every numeric field is an independent eight-byte little-endian value.
constexpr std::array<std::uint8_t, 88> kHashEngineLayoutInput = {
    'H', 'B', 'O', 'X', 'A', 'B', 'I', '4',
    0xc0, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x58, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

std::uint32_t load32(const std::uint8_t* data) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(data[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

std::uint64_t load64(const std::uint8_t* data) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(data[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

void hashByte(std::uint64_t* hash, std::uint8_t byte) noexcept {
    *hash ^= byte;
    *hash *= UINT64_C(1099511628211);
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

std::uint64_t hashBytes(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (std::size_t index = 0; index < size; ++index) {
        hashByte(&hash, data[index]);
    }
    return hash;
}

std::uint64_t hashString(std::string_view value) noexcept {
    return hashBytes(
        reinterpret_cast<const std::uint8_t*>(value.data()),
        value.size());
}

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) {
    CHECK(alignment != 0);
    const auto remainder = value % alignment;
    return remainder == 0 ? value : value + alignment - remainder;
}

std::uint64_t roundBoxSize(std::uint64_t requested) {
    CHECK(requested != 0);
    auto units = requested / 8U + (requested % 8U == 0 ? 0U : 1U);
    std::uint64_t unit_bytes = 8;
    while (units > 15U) {
        units = units / 16U + (units % 16U == 0 ? 0U : 1U);
        unit_bytes *= 16U;
    }
    return units * unit_bytes;
}

std::vector<std::uint8_t> readFile(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    struct stat status {};
    CHECK(::fstat(fd, &status) == 0);
    CHECK(status.st_size >= 0);
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(status.st_size));
    CHECK(::pread(fd, bytes.data(), bytes.size(), 0) ==
          static_cast<ssize_t>(bytes.size()));
    CHECK(::close(fd) == 0);
    return bytes;
}

void writeLittleEndian(
    const std::string& path,
    std::uint64_t offset,
    std::uint64_t value,
    std::size_t width) {
    std::array<std::uint8_t, 8> bytes{};
    CHECK(width <= bytes.size());
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
              static_cast<off_t>(offset)) ==
          static_cast<ssize_t>(width));
    CHECK(::close(fd) == 0);
}

kvspace::ShmOptions hashOptions() {
    kvspace::ShmOptions options;
    options.engine = kvspace::ShmEngine::HashBox;
    options.initial_size = 64 * 1024;
    options.max_size = 1024 * 1024;
    options.max_entries = 8;
    options.max_queues = 1;
    return options;
}

std::uint64_t hashHeaderGeometryHash(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t engine_offset) {
    const auto* header = bytes.data() + engine_offset;
    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (std::size_t index = 0; index < 8; ++index) {
        hashByte(&hash, header[index]);
    }
    hash32(&hash, 1);
    hash32(&hash, 64);
    hash64(&hash, load64(bytes.data() + kEntryLimitOffset));
    hash64(&hash, load64(bytes.data() + kTableCapacityOffset));
    hash64(&hash, load64(bytes.data() + kTableZeroOffset));
    hash64(&hash, load64(bytes.data() + kTableOneOffset));
    for (const std::uint64_t offset : {16U, 24U, 32U, 40U}) {
        hash64(&hash, load64(header + offset));
    }
    return hash;
}

void testHashBoxLayoutFingerprintAndHeaderGolden() {
    CHECK(hashBytes(
              kHashEngineLayoutInput.data(),
              kHashEngineLayoutInput.size()) ==
          kHashEngineLayoutGolden);

    const auto path = "/tmp/kvspace_hash_layout_golden_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    auto store = kvspace::ShmClient::Create(path, hashOptions());
    store->Close();
    const auto bytes = readFile(path);
    CHECK(bytes.size() == hashOptions().max_size);
    CHECK(load32(bytes.data() + 20U) == kRegionHeaderBytes);
    CHECK(load32(bytes.data() + 24U) == 3U);
    CHECK(load32(bytes.data() + 28U) == 4U);
    CHECK(load64(bytes.data() + kEngineLayoutHashOffset) ==
          kHashEngineLayoutGolden);

    const auto page = load64(bytes.data() + kPageSizeOffset);
    const auto region_max = load64(bytes.data() + kRegionMaxOffset);
    const auto capacity = load64(bytes.data() + kTableCapacityOffset);
    const auto table_zero = load64(bytes.data() + kTableZeroOffset);
    const auto table_one = load64(bytes.data() + kTableOneOffset);
    const auto queue_capacity = load64(bytes.data() + kQueueCapacityOffset);
    const auto queue_offset = load64(bytes.data() + kQueueOffset);
    const auto heap_offset = load64(bytes.data() + kHeapOffset);
    const auto engine_offset = load64(bytes.data() + kEngineOffset);
    CHECK(table_zero == alignUp(kRegionHeaderBytes, page));
    CHECK(table_one == alignUp(table_zero + capacity * 32U, page));
    CHECK(queue_offset == alignUp(table_one + capacity * 32U, page));
    CHECK(heap_offset == alignUp(queue_offset + queue_capacity * 32U, page));
    const auto queue_heap_bytes =
        ((region_max - heap_offset) / 8U / page) * page;
    CHECK(queue_heap_bytes >= page);
    CHECK(engine_offset == heap_offset + queue_heap_bytes);
    CHECK(load64(bytes.data() + kHeapLimitOffset) == engine_offset);
    CHECK(load64(bytes.data() + kEngineSizeOffset) ==
          region_max - engine_offset);

    constexpr std::array<std::uint8_t, 8> hash_magic = {
        'K', 'V', 'H', 'B', 'O', 'X', '0', '1'};
    const auto* header = bytes.data() + engine_offset;
    CHECK(std::equal(hash_magic.begin(), hash_magic.end(), header));
    CHECK(load32(header + 8) == 1U);
    CHECK(load32(header + 12) == 64U);
    const auto box_metadata = load64(header + 16);
    const auto box_metadata_bytes = load64(header + 24);
    const auto box_data = load64(header + 32);
    const auto box_data_bytes = load64(header + 40);
    CHECK(box_metadata == engine_offset + 64U);
    CHECK(box_data == box_metadata + box_metadata_bytes);
    CHECK(box_data_bytes != 0);
    CHECK(box_data + box_data_bytes <= region_max);
    CHECK(load64(header + 48) ==
          hashHeaderGeometryHash(bytes, engine_offset));
    CHECK(std::all_of(
        header + 56,
        header + 64,
        [](std::uint8_t byte) { return byte == 0; }));
    constexpr std::array<std::uint8_t, 8> box_magic = {
        'K', 'V', 'B', 'O', 'X', 'A', '0', '1'};
    CHECK(std::equal(
        box_magic.begin(), box_magic.end(), bytes.data() + box_metadata));

    const auto all_zero = [&bytes](std::uint64_t first, std::uint64_t last) {
        return std::all_of(
            bytes.begin() + static_cast<std::ptrdiff_t>(first),
            bytes.begin() + static_cast<std::ptrdiff_t>(last),
            [](std::uint8_t byte) { return byte == 0; });
    };
    CHECK(all_zero(kRegionHeaderBytes, table_zero));
    CHECK(all_zero(table_zero + capacity * 32U, table_one));
    CHECK(all_zero(table_one + capacity * 32U, queue_offset));
    CHECK(all_zero(queue_offset + queue_capacity * 32U, heap_offset));
}

void testHashBoxRejectsLegacyAndCorruptImmutableBytes() {
    const auto suffix = std::to_string(static_cast<long long>(::getpid()));
    {
        const auto path = "/tmp/kvspace_hash_abi3_reject_" + suffix +
            ".kvshm";
        ScopedRegion scoped(path);
        auto store = kvspace::ShmClient::Create(path, hashOptions());
        store->Close();
        writeLittleEndian(path, 28, 3, 4);
        expectThrows<kvspace::ErrVersionMismatch>([&] {
            static_cast<void>(kvspace::ShmClient::Attach(path));
        });
    }
    {
        const auto path = "/tmp/kvspace_hash_old_hash_reject_" + suffix +
            ".kvshm";
        ScopedRegion scoped(path);
        auto store = kvspace::ShmClient::Create(path, hashOptions());
        store->Close();
        writeLittleEndian(
            path,
            kEngineLayoutHashOffset,
            kLegacyHashEngineLayoutHash,
            8);
        expectThrows<kvspace::ErrVersionMismatch>([&] {
            static_cast<void>(kvspace::ShmClient::Attach(path));
        });
    }
    for (std::size_t mutation = 0; mutation < 3; ++mutation) {
        const auto path = "/tmp/kvspace_hash_header_reject_" +
            std::to_string(mutation) + "_" + suffix + ".kvshm";
        ScopedRegion scoped(path);
        auto store = kvspace::ShmClient::Create(path, hashOptions());
        store->Close();
        const auto bytes = readFile(path);
        const auto engine_offset = load64(bytes.data() + kEngineOffset);
        if (mutation == 0) {
            writeLittleEndian(
                path,
                engine_offset + 16U,
                load64(bytes.data() + engine_offset + 16U) + 8U,
                8);
        } else if (mutation == 1) {
            writeLittleEndian(
                path,
                engine_offset + 48U,
                load64(bytes.data() + engine_offset + 48U) ^ 1U,
                8);
        } else {
            writeLittleEndian(path, engine_offset + 56U, 1U, 1);
        }
        expectThrows<kvspace::ErrCorruptRegion>([&] {
            static_cast<void>(kvspace::ShmClient::Attach(path));
        });
    }
}

void testHashBoxRawSlotsSelectorAndEngineOnlyBox() {
    const auto path = "/tmp/kvspace_hash_raw_slots_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    const auto keep_value = kvspace::XValue::Str("replacement").Encode();
    {
        auto region = kvspace::detail::Region::Open(
            path,
            hashOptions(),
            kvspace::detail::OpenMode::Create,
            kvspace::ShmEngine::HashBox);
        auto guard = region->Lock();
        region->Put("keep", kvspace::XValue::Int64(1).Encode());
        region->Put("none", {});
        region->Put("gone", kvspace::XValue::Bool(true).Encode());
        auto mutation = region->BeginMutation();
        CHECK(region->Erase("gone"));
        region->Put("keep", keep_value);
        mutation.Commit();
        (void)guard;
    }

    const auto bytes = readFile(path);
    CHECK(load64(bytes.data() + kHeapTopOffset) ==
          load64(bytes.data() + kHeapOffset));
    CHECK(load64(bytes.data() + kEntryCountOffset) == 2U);
    CHECK(load64(bytes.data() + kTombstoneCountOffset) == 1U);
    const auto selected = load32(bytes.data() + kActiveTableOffset);
    CHECK(selected <= 1U);
    const auto capacity = load64(bytes.data() + kTableCapacityOffset);
    const auto selected_offset = load64(
        bytes.data() + (selected == 0 ? kTableZeroOffset : kTableOneOffset));
    const auto ignored_offset = load64(
        bytes.data() + (selected == 0 ? kTableOneOffset : kTableZeroOffset));
    const auto engine_offset = load64(bytes.data() + kEngineOffset);
    const auto box_data = load64(bytes.data() + engine_offset + 32U);
    const auto box_data_bytes = load64(bytes.data() + engine_offset + 40U);
    std::map<std::string, std::uint64_t> values;
    std::uint64_t tombstones = 0;
    std::uint64_t empty = 0;
    bool saw_reference_one = false;
    for (std::uint64_t index = 0; index < capacity; ++index) {
        const auto* slot = bytes.data() + selected_offset + index * 32U;
        const auto state = load32(slot + 28U);
        if (state == 0) {
            CHECK(std::all_of(
                slot,
                slot + 32,
                [](std::uint8_t byte) { return byte == 0; }));
            ++empty;
            continue;
        }
        if (state == 2) {
            CHECK(std::all_of(
                slot,
                slot + 28,
                [](std::uint8_t byte) { return byte == 0; }));
            ++tombstones;
            continue;
        }
        CHECK(state == 1U);
        const auto hash = load64(slot);
        const auto key_ref = load64(slot + 8U);
        const auto value_ref = load64(slot + 16U);
        const auto key_len = load32(slot + 24U);
        CHECK(key_ref != 0);
        CHECK(key_len != 0);
        const auto key_offset = key_ref - 1U;
        CHECK(key_offset < box_data_bytes);
        CHECK(key_len <= box_data_bytes - key_offset);
        const std::string key(
            reinterpret_cast<const char*>(
                bytes.data() + box_data + key_offset),
            key_len);
        CHECK(hash == hashString(key));
        values.emplace(key, value_ref);
        if (key_ref == 1U) saw_reference_one = true;
    }
    CHECK(empty != 0);
    CHECK(tombstones == 1U);
    CHECK(values.size() == 2U);
    CHECK(values.at("none") == 0U);
    CHECK(values.at("keep") != 0U);
    CHECK(saw_reference_one);
    const auto expected_live =
        roundBoxSize(4) + roundBoxSize(4) +
        roundBoxSize(keep_value.size());
    CHECK(load64(bytes.data() + kEngineLiveBytesOffset) == expected_live);
    CHECK(std::all_of(
        bytes.begin() + static_cast<std::ptrdiff_t>(ignored_offset),
        bytes.begin() + static_cast<std::ptrdiff_t>(
            ignored_offset + capacity * 32U),
        [](std::uint8_t byte) { return byte == 0; }));
    CHECK(std::all_of(
        bytes.begin() + static_cast<std::ptrdiff_t>(engine_offset + 56U),
        bytes.begin() + static_cast<std::ptrdiff_t>(engine_offset + 64U),
        [](std::uint8_t byte) { return byte == 0; }));

    auto attached = kvspace::detail::Region::Open(
        path,
        {},
        kvspace::detail::OpenMode::Attach,
        kvspace::ShmEngine::HashBox);
    auto guard = attached->Lock();
    std::vector<std::uint8_t> value;
    CHECK(attached->Get("keep", &value));
    CHECK(value == keep_value);
    CHECK(attached->Get("none", &value));
    CHECK(value.empty());
    CHECK(!attached->Exists("gone"));
    (void)guard;
}

void testHashBoxIgnoresUnselectedAndRejectsSelectedRawCorruption() {
    const auto path = "/tmp/kvspace_hash_selected_only_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
    ScopedRegion scoped(path);
    const auto encoded = kvspace::XValue::Int64(7).Encode();
    {
        auto region = kvspace::detail::Region::Open(
            path,
            hashOptions(),
            kvspace::detail::OpenMode::Create,
            kvspace::ShmEngine::HashBox);
        auto guard = region->Lock();
        region->Put("kept", encoded);
        (void)guard;
    }
    auto bytes = readFile(path);
    const auto selected = load32(bytes.data() + kActiveTableOffset);
    const auto capacity = load64(bytes.data() + kTableCapacityOffset);
    const auto selected_offset = load64(
        bytes.data() + (selected == 0 ? kTableZeroOffset : kTableOneOffset));
    const auto ignored_offset = load64(
        bytes.data() + (selected == 0 ? kTableOneOffset : kTableZeroOffset));
    writeLittleEndian(path, ignored_offset + 28U, 99U, 4);
    {
        auto attached = kvspace::detail::Region::Open(
            path,
            {},
            kvspace::detail::OpenMode::Attach,
            kvspace::ShmEngine::HashBox);
        auto guard = attached->Lock();
        std::vector<std::uint8_t> value;
        CHECK(attached->Get("kept", &value));
        CHECK(value == encoded);
        (void)guard;
    }

    std::uint64_t empty_index = capacity;
    std::uint64_t occupied_index = capacity;
    for (std::uint64_t index = 0; index < capacity; ++index) {
        const auto state = load32(
            bytes.data() + selected_offset + index * 32U + 28U);
        if (state == 0 && empty_index == capacity) empty_index = index;
        if (state == 1 && occupied_index == capacity) occupied_index = index;
    }
    CHECK(empty_index != capacity);
    CHECK(occupied_index != capacity);
    writeLittleEndian(path, selected_offset + empty_index * 32U, 1U, 8);
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(kvspace::ShmClient::Attach(path));
    });
    writeLittleEndian(path, selected_offset + empty_index * 32U, 0U, 8);
    const auto old_hash = load64(
        bytes.data() + selected_offset + occupied_index * 32U);
    writeLittleEndian(
        path,
        selected_offset + occupied_index * 32U,
        old_hash ^ 1U,
        8);
    expectThrows<kvspace::ErrCorruptRegion>([&] {
        static_cast<void>(kvspace::ShmClient::Attach(path));
    });
}

} // namespace

int main() {
    return runTest([] {
        testHashBoxLayoutFingerprintAndHeaderGolden();
        testHashBoxRejectsLegacyAndCorruptImmutableBytes();
        testHashBoxRawSlotsSelectorAndEngineOnlyBox();
        testHashBoxIgnoresUnselectedAndRejectsSelectedRawCorruption();
    });
}
