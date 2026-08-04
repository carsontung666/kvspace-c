#include "shm_region.h"
#include "shm_art_bump_index.h"
#include "shm_art_bump_store.h"
#include "shm_art_box_store.h"
#include "shm_box_allocators.h"
#include "shm_engine.h"
#include "shm_lifetime.h"
#include "shm_trie_index.h"
#include "shm_trie_node_store.h"

#include "kvspace/errors.h"
#include "kvspace/xvalue.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <new>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <type_traits>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>

#if !defined(__linux__) || !defined(__x86_64__) || !defined(__GLIBC__)
#error "kvspace COMMON04 requires the x86_64 Linux glibc pthread ABI"
#endif

namespace kvspace::detail {
namespace {

std::atomic<RegionTestHook> region_test_hook{nullptr};
std::atomic<CommonApplyTestHook> common_apply_test_hook{nullptr};
std::atomic<ArtBumpRegionTestHook> art_bump_region_test_hook{nullptr};

void emitRegionTestEvent(RegionTestEvent event) noexcept {
    const auto hook = region_test_hook.load(std::memory_order_acquire);
    if (hook != nullptr) hook(event);
}

void emitCommonApplyTestEvent(
    CommonApplyPhase phase,
    CommonApplyStep step,
    std::uint64_t ordinal) noexcept {
    const auto hook = common_apply_test_hook.load(std::memory_order_acquire);
    if (hook != nullptr) hook(phase, step, ordinal);
}

void emitArtBumpRegionTestEvent(
    ArtBumpApplyStep step,
    std::uint64_t ordinal) noexcept {
    const auto hook = art_bump_region_test_hook.load(
        std::memory_order_acquire);
    if (hook != nullptr) hook(step, ordinal);
}

constexpr std::array<char, 8> kMagic{{'K', 'V', 'S', 'H', 'M', '0', '1', '\0'}};
constexpr std::uint32_t kVersion = 4;
constexpr std::uint32_t kEndianMarker = 0x01020304U;
constexpr std::uint32_t kInitReady = 0x4b565231U;
constexpr std::uint32_t kArtBumpAbi = 4;
constexpr std::uint32_t kArtBoxAbi = 3;
constexpr std::uint32_t kHashBoxAbi = 4;
constexpr std::uint32_t kTrieBoxAbi = 4;
constexpr std::uint32_t kBlobMagic = 0x4b56424cU;
constexpr std::uint32_t kMessageMagic = 0x4b564d53U;
constexpr std::uint32_t kStateEmpty = 0;
constexpr std::uint32_t kStateOccupied = 1;
constexpr std::uint32_t kStateTombstone = 2;
constexpr std::uint8_t kBlobUsed = 1;
constexpr std::uint64_t kAlignment = 16;
constexpr std::uint64_t kMinimumBlockSize = 64;
constexpr std::size_t kFreeListCount = 64;
constexpr std::uint64_t kMinimumRegionSize = 64ULL * 1024;
constexpr std::uint32_t kAllocatorJournalIdle = 0;
constexpr std::uint32_t kAllocatorJournalSplit = 1;
constexpr std::uint32_t kAllocatorJournalCoalesce = 2;
constexpr std::uint64_t kAllocatorJournalSalt = 0x4b5653504c495431ULL;
constexpr std::uint64_t kAllocatorCoalesceJournalSalt =
    0x4b564d4552474531ULL;
constexpr std::array<std::uint8_t, 8> kHashBoxMagic = {
    'K', 'V', 'H', 'B', 'O', 'X', '0', '1'};
constexpr std::array<std::uint8_t, 8> kHashBoxLayoutDomain = {
    'H', 'B', 'O', 'X', 'A', 'B', 'I', '4'};
constexpr std::uint32_t kHashBoxVersion = 1;
constexpr std::uint64_t kHashBoxHeaderBytes = 64;
constexpr std::uint64_t kHashBoxSlotBytes = 32;
constexpr std::uint64_t kHashBoxMetadataOffset = 16;
constexpr std::uint64_t kHashBoxMetadataBytesOffset = 24;
constexpr std::uint64_t kHashBoxDataOffset = 32;
constexpr std::uint64_t kHashBoxDataBytesOffset = 40;
constexpr std::uint64_t kHashBoxImmutableHashOffset = 48;
constexpr std::uint64_t kHashBoxReservedOffset = 56;
constexpr std::array<std::uint8_t, 8> kTrieBoxMagic = {
    'K', 'V', 'T', 'R', 'I', 'E', '0', '1'};
constexpr std::array<std::uint8_t, 8> kTrieBoxLayoutDomain = {
    'T', 'R', 'I', 'E', 'B', 'O', 'X', '2'};
constexpr std::uint32_t kTrieBoxVersion = 1;
constexpr std::uint64_t kTrieBoxHeaderBytes = 128;
constexpr std::uint64_t kTrieRootOffset = 16;
constexpr std::uint64_t kTrieNodeMetadataOffset = 24;
constexpr std::uint64_t kTrieNodeMetadataBytesOffset = 32;
constexpr std::uint64_t kTrieNodeZoneOffset = 40;
constexpr std::uint64_t kTrieNodeZoneBytesOffset = 48;
constexpr std::uint64_t kTrieBoxMetadataOffset = 56;
constexpr std::uint64_t kTrieBoxMetadataBytesOffset = 64;
constexpr std::uint64_t kTrieBoxDataOffset = 72;
constexpr std::uint64_t kTrieBoxDataBytesOffset = 80;
constexpr std::uint64_t kTrieImmutableHashOffset = 88;
constexpr std::uint64_t kTrieReservedOffset = 96;
constexpr std::array<std::uint8_t, 8> kArtBoxMagic = {
    'K', 'V', 'A', 'R', 'T', 'B', '0', '1'};
constexpr std::array<std::uint8_t, 8> kArtBoxLayoutDomain = {
    'A', 'R', 'T', 'B', 'O', 'X', '0', '2'};
constexpr std::uint32_t kArtBoxVersion = 1;
constexpr std::uint64_t kArtBoxHeaderBytes = 256;
constexpr std::uint64_t kArtBoxRootOffset = 16;
constexpr std::uint64_t kArtBoxSlabDescriptorsOffset = 24;
constexpr std::uint64_t kArtBoxSlabDescriptorBytes = 32;
constexpr std::uint64_t kArtBoxBoxMetadataOffset = 152;
constexpr std::uint64_t kArtBoxBoxMetadataBytesOffset = 160;
constexpr std::uint64_t kArtBoxBoxDataOffset = 168;
constexpr std::uint64_t kArtBoxBoxDataBytesOffset = 176;
constexpr std::uint64_t kArtBoxImmutableHashOffset = 184;
constexpr std::uint64_t kArtBoxNodeCapacityOffset = 192;
constexpr std::uint64_t kArtBoxHeaderWidthOffset = 196;
constexpr std::uint64_t kArtBoxPayloadSizesOffset = 200;
constexpr std::uint64_t kArtBoxReservedOffset = 216;
constexpr std::array<std::uint32_t, 4> kArtBoxPayloadBytes = {
    48, 104, 472, 1048};
static_assert(kArtBoxPayloadBytes[0] == ArtBoxNodeCodec::kPayloadBytes[0]);
static_assert(kArtBoxPayloadBytes[1] == ArtBoxNodeCodec::kPayloadBytes[1]);
static_assert(kArtBoxPayloadBytes[2] == ArtBoxNodeCodec::kPayloadBytes[2]);
static_assert(kArtBoxPayloadBytes[3] == ArtBoxNodeCodec::kPayloadBytes[3]);

std::uint64_t alignUp(std::uint64_t value, std::uint64_t alignment) {
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        throw Error("kvspace: invalid alignment");
    }
    if (value > std::numeric_limits<std::uint64_t>::max() - (alignment - 1)) {
        throw ErrCapacity("integer overflow");
    }
    return (value + alignment - 1) & ~(alignment - 1);
}

std::uint64_t checkedAdd(std::uint64_t left, std::uint64_t right) {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        throw ErrCapacity("integer overflow");
    }
    return left + right;
}

std::uint64_t checkedMultiply(std::uint64_t left, std::uint64_t right) {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        throw ErrCapacity("integer overflow");
    }
    return left * right;
}

std::uint64_t nextPowerOfTwo(std::uint64_t value) {
    if (value <= 1) return 1;
    --value;
    for (std::size_t shift = 1; shift < 64; shift <<= 1U) {
        value |= value >> shift;
    }
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        throw ErrCapacity("capacity is too large");
    }
    return value + 1;
}

std::uint64_t slotCapacity(std::uint64_t limit) {
    if (limit == 0) throw ErrCapacity("entry limit must be positive");
    const auto scaled = checkedAdd(checkedMultiply(limit, 10), 6) / 7;
    return nextPowerOfTwo(scaled);
}

std::size_t floorLog2(std::uint64_t value) {
    std::size_t result = 0;
    while (value > 1) {
        value >>= 1U;
        ++result;
    }
    return std::min(result, kFreeListCount - 1);
}

std::uint64_t fnv1a(std::string_view value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char raw_byte : value) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool hostIsLittleEndian() noexcept {
    const std::uint32_t value = 1;
    return *reinterpret_cast<const std::uint8_t*>(&value) == 1;
}

std::uint32_t loadLe32(const std::uint8_t* bytes) noexcept {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(bytes[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

std::uint16_t loadLe16(const std::uint8_t* bytes) noexcept {
    std::uint16_t value = 0;
    for (std::size_t index = 0; index < 2; ++index) {
        const auto shifted = static_cast<std::uint16_t>(
            static_cast<std::uint32_t>(bytes[index]) <<
            static_cast<unsigned>(index * 8U));
        value = static_cast<std::uint16_t>(value | shifted);
    }
    return value;
}

std::uint64_t loadLe64(const std::uint8_t* bytes) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(bytes[index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return value;
}

void storeLe32(std::uint8_t* bytes, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
}

void storeLe16(std::uint8_t* bytes, std::uint16_t value) noexcept {
    for (std::size_t index = 0; index < 2; ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
}

void storeLe64(std::uint8_t* bytes, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
}

template <typename UInt>
struct alignas(sizeof(UInt)) PersistentLe {
    static_assert(std::is_unsigned_v<UInt>);
    static_assert(
        sizeof(UInt) == sizeof(std::uint16_t) ||
        sizeof(UInt) == sizeof(std::uint32_t) ||
        sizeof(UInt) == sizeof(std::uint64_t));

    using value_type = UInt;

    std::uint8_t bytes[sizeof(UInt)];

    PersistentLe() = default;
    explicit PersistentLe(UInt value) noexcept { Store(value); }

    UInt Load() const noexcept {
        if constexpr (sizeof(UInt) == sizeof(std::uint16_t)) {
            return static_cast<UInt>(loadLe16(bytes));
        } else if constexpr (sizeof(UInt) == sizeof(std::uint32_t)) {
            return static_cast<UInt>(loadLe32(bytes));
        } else {
            return static_cast<UInt>(loadLe64(bytes));
        }
    }

    void Store(UInt value) noexcept {
        if constexpr (sizeof(UInt) == sizeof(std::uint16_t)) {
            storeLe16(bytes, static_cast<std::uint16_t>(value));
        } else if constexpr (sizeof(UInt) == sizeof(std::uint32_t)) {
            storeLe32(bytes, static_cast<std::uint32_t>(value));
        } else {
            storeLe64(bytes, static_cast<std::uint64_t>(value));
        }
    }

    operator UInt() const noexcept { return Load(); }

    PersistentLe& operator=(UInt value) noexcept {
        Store(value);
        return *this;
    }

    PersistentLe& operator++() noexcept {
        Store(static_cast<UInt>(Load() + UInt{1}));
        return *this;
    }

    PersistentLe& operator--() noexcept {
        Store(static_cast<UInt>(Load() - UInt{1}));
        return *this;
    }

    PersistentLe& operator+=(UInt value) noexcept {
        Store(static_cast<UInt>(Load() + value));
        return *this;
    }

    PersistentLe& operator-=(UInt value) noexcept {
        Store(static_cast<UInt>(Load() - value));
        return *this;
    }
};

using PersistentLe16 = PersistentLe<std::uint16_t>;
using PersistentLe32 = PersistentLe<std::uint32_t>;
using PersistentLe64 = PersistentLe<std::uint64_t>;

static_assert(sizeof(PersistentLe16) == 2);
static_assert(alignof(PersistentLe16) == 2);
static_assert(std::is_standard_layout_v<PersistentLe16>);
static_assert(std::is_trivially_copyable_v<PersistentLe16>);
static_assert(std::is_trivially_default_constructible_v<PersistentLe16>);
static_assert(std::is_trivially_destructible_v<PersistentLe16>);
static_assert(sizeof(PersistentLe32) == 4);
static_assert(alignof(PersistentLe32) == 4);
static_assert(std::is_standard_layout_v<PersistentLe32>);
static_assert(std::is_trivially_copyable_v<PersistentLe32>);
static_assert(std::is_trivially_default_constructible_v<PersistentLe32>);
static_assert(std::is_trivially_destructible_v<PersistentLe32>);
static_assert(__atomic_always_lock_free(sizeof(PersistentLe32), nullptr));
static_assert(sizeof(PersistentLe64) == 8);
static_assert(alignof(PersistentLe64) == 8);
static_assert(std::is_standard_layout_v<PersistentLe64>);
static_assert(std::is_trivially_copyable_v<PersistentLe64>);
static_assert(std::is_trivially_default_constructible_v<PersistentLe64>);
static_assert(std::is_trivially_destructible_v<PersistentLe64>);
static_assert(__atomic_always_lock_free(sizeof(PersistentLe64), nullptr));

template <typename Encoded>
typename Encoded::value_type atomicLoadLe(
    const Encoded* source,
    int memory_order) noexcept {
    static_assert(std::is_trivially_copyable_v<Encoded>);
    Encoded encoded{};
    __atomic_load(source, &encoded, memory_order);
    return encoded.Load();
}

template <typename Encoded>
void atomicStoreLe(
    Encoded* destination,
    typename Encoded::value_type value,
    int memory_order) noexcept {
    static_assert(std::is_trivially_copyable_v<Encoded>);
    Encoded encoded(value);
    __atomic_store(destination, &encoded, memory_order);
}

void hashLe32(std::uint64_t* hash, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < 4; ++index) {
        *hash ^= static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
        *hash *= 1099511628211ULL;
    }
}

void hashLe64(std::uint64_t* hash, std::uint64_t value) noexcept {
    for (std::size_t index = 0; index < 8; ++index) {
        *hash ^= static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
        *hash *= 1099511628211ULL;
    }
}

std::string systemError(const std::string& operation) {
    return operation + ": " + std::strerror(errno);
}

bool isRegularPath(const std::string& name) {
    return name.size() > 1 && name.front() == '/' &&
           name.find('/', 1) != std::string::npos;
}

std::string shmName(const std::string& name) {
    if (name.empty()) throw ErrInvalidPath("empty shm name");
    if (isRegularPath(name)) return name;
    if (name.front() == '/') return name;
    return "/" + name;
}

struct RegionPrefix {
    char magic[8];
    PersistentLe32 version;
    PersistentLe32 endian;
    PersistentLe32 init_state;
    PersistentLe32 header_size;
    PersistentLe32 engine_id;
    PersistentLe32 engine_abi;
    PersistentLe64 common_layout_hash;
    PersistentLe64 engine_layout_hash;
    PersistentLe64 region_size;
    PersistentLe64 region_max;
};
static_assert(sizeof(RegionPrefix) == 64, "RegionPrefix ABI changed");
static_assert(alignof(RegionPrefix) == 8, "RegionPrefix alignment changed");
static_assert(std::is_standard_layout_v<RegionPrefix>);
static_assert(std::is_trivially_copyable_v<RegionPrefix>);

constexpr std::uint64_t commonLayoutHash() {
    return 0x434f4d4d4f4e3034ULL ^
        (static_cast<std::uint64_t>(sizeof(RegionPrefix)) << 1U);
}

ShmEngine decodeEngine(std::uint32_t engine_id) {
    switch (engine_id) {
    case static_cast<std::uint32_t>(ShmEngine::ArtBump):
        return ShmEngine::ArtBump;
    case static_cast<std::uint32_t>(ShmEngine::ArtBox):
        return ShmEngine::ArtBox;
    case static_cast<std::uint32_t>(ShmEngine::HashBox):
        return ShmEngine::HashBox;
    case static_cast<std::uint32_t>(ShmEngine::TrieBox):
        return ShmEngine::TrieBox;
    default:
        throw ErrUnsupportedEngine(std::to_string(engine_id));
    }
}

void requireImplementedEngine(ShmEngine engine) {
    if (engine != ShmEngine::ArtBump &&
        engine != ShmEngine::ArtBox &&
        engine != ShmEngine::HashBox &&
        engine != ShmEngine::TrieBox) {
        throw ErrUnsupportedEngine(std::to_string(
            static_cast<std::uint32_t>(engine)));
    }
}

struct QueueSlot {
    PersistentLe64 hash;
    PersistentLe64 key_offset;
    PersistentLe64 head_offset;
    PersistentLe32 key_length;
    PersistentLe32 state;
};
static_assert(sizeof(QueueSlot) == 32, "QueueSlot ABI changed");
static_assert(alignof(QueueSlot) == 8, "QueueSlot alignment changed");
static_assert(std::is_standard_layout_v<QueueSlot>);
static_assert(std::is_trivially_copyable_v<QueueSlot>);
static_assert(std::is_aggregate_v<QueueSlot>);
static_assert(std::is_trivially_destructible_v<QueueSlot>);
static_assert(offsetof(QueueSlot, hash) == 0);
static_assert(offsetof(QueueSlot, key_offset) == 8);
static_assert(offsetof(QueueSlot, head_offset) == 16);
static_assert(offsetof(QueueSlot, key_length) == 24);
static_assert(offsetof(QueueSlot, state) == 28);

struct BlobHeader {
    PersistentLe64 span;
    PersistentLe64 previous_span;
    PersistentLe64 next_free;
    PersistentLe64 previous_free;
    PersistentLe32 payload_size;
    PersistentLe32 magic;
    std::uint8_t flags;
    std::uint8_t mark;
    PersistentLe16 reserved16;
    PersistentLe32 reserved32;
};
static_assert(sizeof(BlobHeader) == 48, "BlobHeader ABI changed");
static_assert(alignof(BlobHeader) == 8, "BlobHeader alignment changed");
static_assert(std::is_standard_layout_v<BlobHeader>);
static_assert(std::is_trivially_copyable_v<BlobHeader>);
static_assert(std::is_aggregate_v<BlobHeader>);
static_assert(std::is_trivially_destructible_v<BlobHeader>);
static_assert(offsetof(BlobHeader, span) == 0);
static_assert(offsetof(BlobHeader, previous_span) == 8);
static_assert(offsetof(BlobHeader, next_free) == 16);
static_assert(offsetof(BlobHeader, previous_free) == 24);
static_assert(offsetof(BlobHeader, payload_size) == 32);
static_assert(offsetof(BlobHeader, magic) == 36);
static_assert(offsetof(BlobHeader, flags) == 40);
static_assert(offsetof(BlobHeader, mark) == 41);
static_assert(offsetof(BlobHeader, reserved16) == 42);
static_assert(offsetof(BlobHeader, reserved32) == 44);

std::uint64_t blobSpanForPayload(std::uint64_t payload_size) {
    return std::max(
        kMinimumBlockSize,
        alignUp(
            checkedAdd(sizeof(BlobHeader), payload_size),
            kAlignment));
}

struct MessageHeader {
    PersistentLe64 next;
    PersistentLe32 value_length;
    PersistentLe32 magic;
};
static_assert(sizeof(MessageHeader) == 16, "MessageHeader ABI changed");
static_assert(alignof(MessageHeader) == 8, "MessageHeader alignment changed");
static_assert(std::is_standard_layout_v<MessageHeader>);
static_assert(std::is_trivially_copyable_v<MessageHeader>);
static_assert(offsetof(MessageHeader, next) == 0);
static_assert(offsetof(MessageHeader, value_length) == 8);
static_assert(offsetof(MessageHeader, magic) == 12);

struct AllocatorJournal {
    PersistentLe64 offset;
    PersistentLe64 original_span;
    PersistentLe64 requested_span;
    PersistentLe64 checksum;
    PersistentLe32 state;
    PersistentLe32 reserved32;
    PersistentLe64 reserved64[3];
};
static_assert(sizeof(AllocatorJournal) == 64, "AllocatorJournal ABI changed");
static_assert(std::is_standard_layout_v<AllocatorJournal>);
static_assert(std::is_trivially_copyable_v<AllocatorJournal>);

struct alignas(64) RegionHeader {
    RegionPrefix prefix;
    PersistentLe64 page_size;
    PersistentLe64 entry_limit;
    PersistentLe64 table_capacity;
    PersistentLe64 table_offset[2];
    PersistentLe64 root_offset;
    PersistentLe64 node_count;
    PersistentLe64 entry_count;
    PersistentLe64 tombstone_count;
    PersistentLe64 queue_limit;
    PersistentLe64 queue_capacity;
    PersistentLe64 queue_offset;
    PersistentLe64 queue_count;
    PersistentLe64 heap_offset;
    PersistentLe64 heap_top;
    PersistentLe64 heap_last;
    PersistentLe64 heap_limit;
    PersistentLe64 engine_offset;
    PersistentLe64 engine_size;
    PersistentLe64 engine_live_bytes;
    AllocatorJournal allocator_journal;
    PersistentLe64 free_heads[kFreeListCount];
    // COMMON04 keeps the physical bytes that pre-resolution implementations
    // used for ArtBump allocator heads, but they no longer have allocator
    // semantics.  Every accepted engine leaves all 68 words zero.
    PersistentLe64 reserved_allocator_slots[68];
    PersistentLe64 generation;
    PersistentLe64 recovery_count;
    PersistentLe32 active_table;
    PersistentLe32 corrupt;
    pthread_mutex_t mutex;
    pthread_cond_t notify_cond;
};
static_assert(std::is_standard_layout_v<RegionHeader>);
static_assert(std::is_trivially_copyable_v<RegionHeader>);
static_assert(std::is_aggregate_v<RegionHeader>);
static_assert(std::is_trivially_destructible_v<RegionHeader>);

static_assert(sizeof(RegionPrefix) == 64);
static_assert(alignof(RegionPrefix) == 8);
static_assert(offsetof(RegionPrefix, magic) == 0);
static_assert(offsetof(RegionPrefix, version) == 8);
static_assert(offsetof(RegionPrefix, endian) == 12);
static_assert(offsetof(RegionPrefix, init_state) == 16);
static_assert(offsetof(RegionPrefix, header_size) == 20);
static_assert(offsetof(RegionPrefix, engine_id) == 24);
static_assert(offsetof(RegionPrefix, engine_abi) == 28);
static_assert(offsetof(RegionPrefix, common_layout_hash) == 32);
static_assert(offsetof(RegionPrefix, engine_layout_hash) == 40);
static_assert(offsetof(RegionPrefix, region_size) == 48);
static_assert(offsetof(RegionPrefix, region_max) == 56);

static_assert(sizeof(AllocatorJournal) == 64);
static_assert(alignof(AllocatorJournal) == 8);
static_assert(offsetof(AllocatorJournal, offset) == 0);
static_assert(offsetof(AllocatorJournal, original_span) == 8);
static_assert(offsetof(AllocatorJournal, requested_span) == 16);
static_assert(offsetof(AllocatorJournal, checksum) == 24);
static_assert(offsetof(AllocatorJournal, state) == 32);
static_assert(offsetof(AllocatorJournal, reserved32) == 36);
static_assert(offsetof(AllocatorJournal, reserved64) == 40);

static_assert(sizeof(pthread_mutex_t) == 40);
static_assert(alignof(pthread_mutex_t) == 8);
static_assert(std::is_trivial_v<pthread_mutex_t>);
static_assert(std::is_standard_layout_v<pthread_mutex_t>);
static_assert(std::is_trivially_copyable_v<pthread_mutex_t>);
static_assert(std::is_trivially_destructible_v<pthread_mutex_t>);
static_assert(sizeof(pthread_cond_t) == 48);
static_assert(alignof(pthread_cond_t) == 8);
static_assert(std::is_trivial_v<pthread_cond_t>);
static_assert(std::is_standard_layout_v<pthread_cond_t>);
static_assert(std::is_trivially_copyable_v<pthread_cond_t>);
static_assert(std::is_trivially_destructible_v<pthread_cond_t>);
static_assert(sizeof(RegionHeader) == 1472);
static_assert(alignof(RegionHeader) == 64);
static_assert(offsetof(RegionHeader, prefix) == 0);
static_assert(offsetof(RegionHeader, page_size) == 64);
static_assert(offsetof(RegionHeader, entry_limit) == 72);
static_assert(offsetof(RegionHeader, table_capacity) == 80);
static_assert(offsetof(RegionHeader, table_offset) == 88);
static_assert(offsetof(RegionHeader, root_offset) == 104);
static_assert(offsetof(RegionHeader, node_count) == 112);
static_assert(offsetof(RegionHeader, entry_count) == 120);
static_assert(offsetof(RegionHeader, tombstone_count) == 128);
static_assert(offsetof(RegionHeader, queue_limit) == 136);
static_assert(offsetof(RegionHeader, queue_capacity) == 144);
static_assert(offsetof(RegionHeader, queue_offset) == 152);
static_assert(offsetof(RegionHeader, queue_count) == 160);
static_assert(offsetof(RegionHeader, heap_offset) == 168);
static_assert(offsetof(RegionHeader, heap_top) == 176);
static_assert(offsetof(RegionHeader, heap_last) == 184);
static_assert(offsetof(RegionHeader, heap_limit) == 192);
static_assert(offsetof(RegionHeader, engine_offset) == 200);
static_assert(offsetof(RegionHeader, engine_size) == 208);
static_assert(offsetof(RegionHeader, engine_live_bytes) == 216);
static_assert(offsetof(RegionHeader, allocator_journal) == 224);
static_assert(offsetof(RegionHeader, free_heads) == 288);
static_assert(
    offsetof(RegionHeader, free_heads) + sizeof(RegionHeader::free_heads) ==
    800);
static_assert(offsetof(RegionHeader, reserved_allocator_slots) == 800);
static_assert(
    offsetof(RegionHeader, reserved_allocator_slots) +
        sizeof(RegionHeader::reserved_allocator_slots) ==
    1344);
static_assert(offsetof(RegionHeader, generation) == 1344);
static_assert(offsetof(RegionHeader, recovery_count) == 1352);
static_assert(offsetof(RegionHeader, active_table) == 1360);
static_assert(offsetof(RegionHeader, corrupt) == 1364);
static_assert(offsetof(RegionHeader, mutex) == 1368);
static_assert(offsetof(RegionHeader, notify_cond) == 1408);
static_assert(
    offsetof(RegionHeader, notify_cond) + sizeof(pthread_cond_t) == 1456);

std::uint64_t hashBoxLayoutHash() noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : kHashBoxLayoutDomain) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    for (const auto value : {
             static_cast<std::uint64_t>(sizeof(RegionHeader)),
             kHashBoxHeaderBytes,
             kHashBoxSlotBytes,
             static_cast<std::uint64_t>(
                 FixedBlockAllocator::kPersistentMetadataBytes),
             static_cast<std::uint64_t>(BoxAllocator::kPersistentHeaderBytes),
             static_cast<std::uint64_t>(BoxAllocator::kNodePayloadBytes),
             static_cast<std::uint64_t>(sizeof(QueueSlot)),
             static_cast<std::uint64_t>(sizeof(BlobHeader)),
             static_cast<std::uint64_t>(sizeof(pthread_mutex_t)),
             static_cast<std::uint64_t>(sizeof(pthread_cond_t))}) {
        hashLe64(&hash, value);
    }
    return hash;
}

std::uint64_t trieBoxLayoutHash() noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : kTrieBoxLayoutDomain) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    for (const auto value : {
             static_cast<std::uint64_t>(sizeof(RegionHeader)),
             kTrieBoxHeaderBytes,
             static_cast<std::uint64_t>(TrieNodeCodec::kEncodedBytes),
             static_cast<std::uint64_t>(
                 FixedBlockAllocator::kPersistentMetadataBytes),
             static_cast<std::uint64_t>(BoxAllocator::kPersistentHeaderBytes),
             static_cast<std::uint64_t>(BoxAllocator::kNodePayloadBytes),
             static_cast<std::uint64_t>(sizeof(QueueSlot)),
             static_cast<std::uint64_t>(sizeof(BlobHeader)),
             static_cast<std::uint64_t>(sizeof(pthread_mutex_t)),
             static_cast<std::uint64_t>(sizeof(pthread_cond_t))}) {
        hashLe64(&hash, value);
    }
    return hash;
}

std::uint64_t artBumpLayoutHash() noexcept {
    return ArtBumpHeaderCodec::EngineLayoutHash(
        sizeof(RegionHeader),
        sizeof(QueueSlot),
        sizeof(BlobHeader),
        sizeof(pthread_mutex_t),
        sizeof(pthread_cond_t));
}

std::uint64_t artBoxLayoutHash() noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : kArtBoxLayoutDomain) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    for (const auto value : {
             static_cast<std::uint64_t>(sizeof(RegionHeader)),
             kArtBoxHeaderBytes,
             static_cast<std::uint64_t>(kArtBoxPayloadBytes[0]),
             static_cast<std::uint64_t>(kArtBoxPayloadBytes[1]),
             static_cast<std::uint64_t>(kArtBoxPayloadBytes[2]),
             static_cast<std::uint64_t>(kArtBoxPayloadBytes[3]),
             static_cast<std::uint64_t>(
                 FixedBlockAllocator::kPersistentMetadataBytes),
             static_cast<std::uint64_t>(BoxAllocator::kPersistentHeaderBytes),
             static_cast<std::uint64_t>(BoxAllocator::kNodePayloadBytes),
             static_cast<std::uint64_t>(sizeof(QueueSlot)),
             static_cast<std::uint64_t>(sizeof(BlobHeader)),
             static_cast<std::uint64_t>(sizeof(pthread_mutex_t)),
             static_cast<std::uint64_t>(sizeof(pthread_cond_t))}) {
        hashLe64(&hash, value);
    }
    return hash;
}

std::uint32_t engineAbi(ShmEngine engine) {
    switch (engine) {
    case ShmEngine::ArtBump:
        return kArtBumpAbi;
    case ShmEngine::ArtBox:
        return kArtBoxAbi;
    case ShmEngine::HashBox:
        return kHashBoxAbi;
    case ShmEngine::TrieBox:
        return kTrieBoxAbi;
    default:
        requireImplementedEngine(engine);
        throw ErrUnsupportedEngine();
    }
}

std::uint64_t engineLayoutHash(ShmEngine engine) {
    switch (engine) {
    case ShmEngine::ArtBump:
        return artBumpLayoutHash();
    case ShmEngine::ArtBox:
        return artBoxLayoutHash();
    case ShmEngine::HashBox:
        return hashBoxLayoutHash();
    case ShmEngine::TrieBox:
        return trieBoxLayoutHash();
    default:
        requireImplementedEngine(engine);
        throw ErrUnsupportedEngine();
    }
}

struct HashBoxGeometry {
    std::uint64_t header_offset = 0;
    std::uint64_t box_metadata_offset = 0;
    std::uint64_t box_metadata_bytes = 0;
    std::uint64_t box_data_offset = 0;
    std::uint64_t box_data_bytes = 0;
};

bool sameHashBoxGeometry(
    const HashBoxGeometry& left,
    const HashBoxGeometry& right) noexcept {
    return left.header_offset == right.header_offset &&
        left.box_metadata_offset == right.box_metadata_offset &&
        left.box_metadata_bytes == right.box_metadata_bytes &&
        left.box_data_offset == right.box_data_offset &&
        left.box_data_bytes == right.box_data_bytes;
}

HashBoxGeometry makeHashBoxGeometry(
    std::uint64_t engine_offset,
    std::uint64_t region_max) {
    HashBoxGeometry geometry;
    geometry.header_offset = engine_offset;
    geometry.box_metadata_offset = checkedAdd(
        engine_offset, kHashBoxHeaderBytes);
    if (geometry.box_metadata_offset >= region_max) {
        throw ErrCapacity("max_size cannot hold the HashBox header");
    }
    const auto budget = region_max - geometry.box_metadata_offset;
    try {
        geometry.box_data_bytes =
            BoxAllocator::LargestFullyRepresentableDataSize(budget);
        geometry.box_metadata_bytes =
            BoxAllocator::MinimumMetadataBytesForFullExpansion(
                geometry.box_data_bytes);
    } catch (const AllocatorError& error) {
        throw ErrCapacity(error.what());
    }
    if (geometry.box_data_bytes == 0) {
        throw ErrCapacity("HashBox Box data section is empty");
    }
    geometry.box_data_offset = checkedAdd(
        geometry.box_metadata_offset, geometry.box_metadata_bytes);
    const auto end = checkedAdd(
        geometry.box_data_offset, geometry.box_data_bytes);
    if (end > region_max) {
        throw ErrCapacity("HashBox Box sections exceed max_size");
    }
    return geometry;
}

std::uint64_t hashBoxImmutableHash(
    const HashBoxGeometry& geometry,
    std::uint64_t entry_limit,
    std::uint64_t table_capacity,
    std::uint64_t table_zero,
    std::uint64_t table_one) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : kHashBoxMagic) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    hashLe32(&hash, kHashBoxVersion);
    hashLe32(&hash, static_cast<std::uint32_t>(kHashBoxHeaderBytes));
    for (const auto value : {
             entry_limit,
             table_capacity,
             table_zero,
             table_one,
             geometry.box_metadata_offset,
             geometry.box_metadata_bytes,
             geometry.box_data_offset,
             geometry.box_data_bytes}) {
        hashLe64(&hash, value);
    }
    return hash;
}

std::uint8_t* hashBoxHeaderBytes(
    std::uint8_t* base,
    const RegionHeader* header) {
    if (header->engine_offset == 0 ||
        header->engine_offset > header->prefix.region_max ||
        kHashBoxHeaderBytes >
            header->prefix.region_max - header->engine_offset) {
        throw ErrCorruptRegion("invalid HashBox header offset");
    }
    return base + header->engine_offset;
}

HashBoxGeometry readHashBoxGeometry(
    std::uint8_t* base,
    const RegionHeader* header) {
    const auto* bytes = hashBoxHeaderBytes(base, header);
    if (!std::equal(kHashBoxMagic.begin(), kHashBoxMagic.end(), bytes) ||
        loadLe32(bytes + 8) != kHashBoxVersion ||
        loadLe32(bytes + 12) != kHashBoxHeaderBytes) {
        throw ErrCorruptRegion("invalid HashBox persistent header");
    }
    HashBoxGeometry stored;
    stored.header_offset = header->engine_offset;
    stored.box_metadata_offset = loadLe64(
        bytes + kHashBoxMetadataOffset);
    stored.box_metadata_bytes = loadLe64(
        bytes + kHashBoxMetadataBytesOffset);
    stored.box_data_offset = loadLe64(bytes + kHashBoxDataOffset);
    stored.box_data_bytes = loadLe64(bytes + kHashBoxDataBytesOffset);

    HashBoxGeometry expected;
    try {
        expected = makeHashBoxGeometry(
            header->engine_offset, header->prefix.region_max);
    } catch (const Error& error) {
        throw ErrCorruptRegion(error.what());
    }
    if (!sameHashBoxGeometry(stored, expected) ||
        loadLe64(bytes + kHashBoxImmutableHashOffset) !=
            hashBoxImmutableHash(
                expected,
                header->entry_limit,
                header->table_capacity,
                header->table_offset[0],
                header->table_offset[1])) {
        throw ErrCorruptRegion("HashBox geometry mismatch");
    }
    for (std::uint64_t offset = kHashBoxReservedOffset;
         offset < kHashBoxHeaderBytes;
         ++offset) {
        if (bytes[offset] != 0) {
            throw ErrCorruptRegion("nonzero HashBox reserved byte");
        }
    }

    const auto require_zero = [base](
                                  std::uint64_t first,
                                  std::uint64_t last) {
        if (last < first ||
            !std::all_of(base + first, base + last,
                         [](std::uint8_t byte) { return byte == 0; })) {
            throw ErrCorruptRegion("nonzero HashBox alignment padding");
        }
    };
    const auto table_bytes = checkedMultiply(
        header->table_capacity, kHashBoxSlotBytes);
    const auto queue_bytes = checkedMultiply(
        header->queue_capacity,
        static_cast<std::uint64_t>(sizeof(QueueSlot)));
    require_zero(sizeof(RegionHeader), header->table_offset[0]);
    require_zero(
        checkedAdd(header->table_offset[0], table_bytes),
        header->table_offset[1]);
    require_zero(
        checkedAdd(header->table_offset[1], table_bytes),
        header->queue_offset);
    require_zero(
        checkedAdd(header->queue_offset, queue_bytes),
        header->heap_offset);
    return expected;
}

struct TrieBoxGeometry {
    std::uint64_t header_offset = 0;
    std::uint64_t node_metadata_offset = 0;
    std::uint64_t node_metadata_bytes = 0;
    std::uint64_t node_zone_offset = 0;
    std::uint64_t node_zone_bytes = 0;
    std::uint64_t box_metadata_offset = 0;
    std::uint64_t box_metadata_bytes = 0;
    std::uint64_t box_data_offset = 0;
    std::uint64_t box_data_bytes = 0;
};

bool sameTrieBoxGeometry(
    const TrieBoxGeometry& left,
    const TrieBoxGeometry& right) noexcept {
    return left.header_offset == right.header_offset &&
        left.node_metadata_offset == right.node_metadata_offset &&
        left.node_metadata_bytes == right.node_metadata_bytes &&
        left.node_zone_offset == right.node_zone_offset &&
        left.node_zone_bytes == right.node_zone_bytes &&
        left.box_metadata_offset == right.box_metadata_offset &&
        left.box_metadata_bytes == right.box_metadata_bytes &&
        left.box_data_offset == right.box_data_offset &&
        left.box_data_bytes == right.box_data_bytes;
}

TrieBoxGeometry makeTrieBoxGeometry(
    std::uint64_t engine_offset,
    std::uint64_t region_max,
    std::uint64_t page_size) {
    TrieBoxGeometry geometry;
    geometry.header_offset = engine_offset;
    const auto payload_begin = alignUp(
        checkedAdd(engine_offset, kTrieBoxHeaderBytes), page_size);
    if (payload_begin >= region_max) {
        throw ErrCapacity("max_size cannot hold the TrieBox header");
    }
    const auto payload_bytes = region_max - payload_begin;
    const auto node_budget = (payload_bytes / 2 / page_size) * page_size;
    const auto box_budget = payload_bytes - node_budget;
    if (node_budget <= FixedBlockAllocator::kPersistentMetadataBytes ||
        box_budget < page_size) {
        throw ErrCapacity("max_size cannot hold the TrieBox allocators");
    }

    geometry.node_metadata_offset = payload_begin;
    geometry.node_metadata_bytes =
        FixedBlockAllocator::kPersistentMetadataBytes;
    geometry.node_zone_offset = checkedAdd(
        geometry.node_metadata_offset, geometry.node_metadata_bytes);
    geometry.node_zone_bytes = node_budget - geometry.node_metadata_bytes;
    geometry.box_metadata_offset = checkedAdd(payload_begin, node_budget);
    try {
        static_cast<void>(TrieNodeStore::CanonicalCapacityForZone(
            geometry.node_zone_bytes));
        geometry.box_data_bytes =
            BoxAllocator::LargestFullyRepresentableDataSize(box_budget);
        geometry.box_metadata_bytes =
            BoxAllocator::MinimumMetadataBytesForFullExpansion(
                geometry.box_data_bytes);
    } catch (const AllocatorError& error) {
        throw ErrCapacity(error.what());
    }
    geometry.box_data_offset = checkedAdd(
        geometry.box_metadata_offset, geometry.box_metadata_bytes);
    const auto box_end = checkedAdd(
        geometry.box_data_offset, geometry.box_data_bytes);
    if (box_end > region_max) {
        throw ErrCapacity("TrieBox Box sections exceed max_size");
    }
    return geometry;
}

struct ArtBoxSlabGeometry {
    std::uint64_t metadata_offset = 0;
    std::uint64_t metadata_bytes = 0;
    std::uint64_t zone_offset = 0;
    std::uint64_t zone_bytes = 0;
};

struct ArtBoxGeometry {
    std::uint64_t header_offset = 0;
    std::array<ArtBoxSlabGeometry, 4> slabs{};
    std::uint64_t box_metadata_offset = 0;
    std::uint64_t box_metadata_bytes = 0;
    std::uint64_t box_data_offset = 0;
    std::uint64_t box_data_bytes = 0;
    std::uint32_t node_capacity = 0;
    std::uint8_t header_width = 0;
};

std::uint64_t artBoxImmutableHash(const ArtBoxGeometry& geometry) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : kArtBoxMagic) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    hashLe32(&hash, kArtBoxVersion);
    hashLe32(&hash, static_cast<std::uint32_t>(kArtBoxHeaderBytes));
    hashLe32(&hash, geometry.node_capacity);
    hash ^= geometry.header_width;
    hash *= 1099511628211ULL;
    for (const auto payload : kArtBoxPayloadBytes) hashLe32(&hash, payload);
    for (const auto& slab : geometry.slabs) {
        hashLe64(&hash, slab.metadata_offset);
        hashLe64(&hash, slab.metadata_bytes);
        hashLe64(&hash, slab.zone_offset);
        hashLe64(&hash, slab.zone_bytes);
    }
    hashLe64(&hash, geometry.box_metadata_offset);
    hashLe64(&hash, geometry.box_metadata_bytes);
    hashLe64(&hash, geometry.box_data_offset);
    hashLe64(&hash, geometry.box_data_bytes);
    return hash;
}

ArtBoxGeometry makeArtBoxGeometry(
    std::uint64_t engine_offset,
    std::uint64_t region_max,
    std::uint64_t page_size,
    std::uint64_t entry_limit) {
    ArtBoxGeometry geometry;
    geometry.header_offset = engine_offset;
    const auto capacity64 = checkedAdd(checkedMultiply(entry_limit, 5), 1);
    if (capacity64 > 0x3fffffffULL) {
        throw ErrCapacity("ArtBox node capacity exceeds tagged references");
    }
    geometry.node_capacity = static_cast<std::uint32_t>(capacity64);
    const auto width = FixedBlockAllocator::MinimumHeaderWidth(capacity64);
    geometry.header_width = static_cast<std::uint8_t>(width);

    auto cursor = alignUp(
        checkedAdd(engine_offset, kArtBoxHeaderBytes), page_size);
    for (std::size_t index = 0; index < geometry.slabs.size(); ++index) {
        auto& slab = geometry.slabs[index];
        slab.metadata_offset = cursor;
        slab.metadata_bytes = FixedBlockAllocator::kPersistentMetadataBytes;
        slab.zone_offset = checkedAdd(
            slab.metadata_offset, slab.metadata_bytes);
        slab.zone_bytes = checkedMultiply(
            capacity64,
            checkedAdd(geometry.header_width, kArtBoxPayloadBytes[index]));
        cursor = alignUp(
            checkedAdd(slab.zone_offset, slab.zone_bytes), 64);
        if (cursor > region_max) {
            throw ErrCapacity("max_size cannot hold ArtBox node slabs");
        }
    }

    geometry.box_metadata_offset = cursor;
    if (cursor >= region_max) {
        throw ErrCapacity("max_size cannot hold the ArtBox Box allocator");
    }
    const auto box_budget = region_max - cursor;
    try {
        geometry.box_data_bytes =
            BoxAllocator::LargestFullyRepresentableDataSize(box_budget);
        geometry.box_metadata_bytes =
            BoxAllocator::MinimumMetadataBytesForFullExpansion(
                geometry.box_data_bytes);
    } catch (const AllocatorError& error) {
        throw ErrCapacity(error.what());
    }
    if (geometry.box_data_bytes == 0) {
        throw ErrCapacity("ArtBox Box data section is empty");
    }
    geometry.box_data_offset = checkedAdd(
        geometry.box_metadata_offset, geometry.box_metadata_bytes);
    const auto box_end = checkedAdd(
        geometry.box_data_offset, geometry.box_data_bytes);
    if (box_end > region_max) {
        throw ErrCapacity("ArtBox Box sections exceed max_size");
    }
    return geometry;
}

std::uint64_t trieBoxImmutableHash(const TrieBoxGeometry& geometry) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : kTrieBoxMagic) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    hashLe32(&hash, kTrieBoxVersion);
    hashLe64(&hash, kTrieBoxHeaderBytes);
    hashLe64(&hash, geometry.node_metadata_offset);
    hashLe64(&hash, geometry.node_metadata_bytes);
    hashLe64(&hash, geometry.node_zone_offset);
    hashLe64(&hash, geometry.node_zone_bytes);
    hashLe64(&hash, geometry.box_metadata_offset);
    hashLe64(&hash, geometry.box_metadata_bytes);
    hashLe64(&hash, geometry.box_data_offset);
    hashLe64(&hash, geometry.box_data_bytes);
    return hash;
}

std::uint8_t* trieBoxHeaderBytes(
    std::uint8_t* base,
    const RegionHeader* header) {
    if (header->engine_offset == 0 ||
        header->engine_offset > header->prefix.region_max ||
        kTrieBoxHeaderBytes >
            header->prefix.region_max - header->engine_offset) {
        throw ErrCorruptRegion("invalid TrieBox header offset");
    }
    return base + header->engine_offset;
}

ByteTrieIndex::CommittedRootSlot trieBoxRootStorage(
    std::uint8_t* base,
    const RegionHeader* header) {
    auto* storage = trieBoxHeaderBytes(base, header) + kTrieRootOffset;
    try {
        return ByteTrieIndex::CommittedRootSlot::Attach(
            storage, sizeof(std::uint32_t));
    } catch (const AllocatorError& error) {
        throw ErrCorruptRegion(error.what());
    }
}

TrieBoxGeometry readTrieBoxGeometry(
    std::uint8_t* base,
    const RegionHeader* header) {
    const auto* bytes = trieBoxHeaderBytes(base, header);
    if (!std::equal(kTrieBoxMagic.begin(), kTrieBoxMagic.end(), bytes) ||
        loadLe32(bytes + 8) != kTrieBoxVersion ||
        loadLe32(bytes + 12) != kTrieBoxHeaderBytes) {
        throw ErrCorruptRegion("invalid TrieBox persistent header");
    }
    TrieBoxGeometry stored;
    stored.header_offset = header->engine_offset;
    stored.node_metadata_offset = loadLe64(
        bytes + kTrieNodeMetadataOffset);
    stored.node_metadata_bytes = loadLe64(
        bytes + kTrieNodeMetadataBytesOffset);
    stored.node_zone_offset = loadLe64(bytes + kTrieNodeZoneOffset);
    stored.node_zone_bytes = loadLe64(bytes + kTrieNodeZoneBytesOffset);
    stored.box_metadata_offset = loadLe64(bytes + kTrieBoxMetadataOffset);
    stored.box_metadata_bytes = loadLe64(
        bytes + kTrieBoxMetadataBytesOffset);
    stored.box_data_offset = loadLe64(bytes + kTrieBoxDataOffset);
    stored.box_data_bytes = loadLe64(bytes + kTrieBoxDataBytesOffset);

    TrieBoxGeometry expected;
    try {
        expected = makeTrieBoxGeometry(
            header->engine_offset,
            header->prefix.region_max,
            header->page_size);
    } catch (const Error& error) {
        throw ErrCorruptRegion(error.what());
    }
    if (!sameTrieBoxGeometry(stored, expected) ||
        loadLe64(bytes + kTrieImmutableHashOffset) !=
            trieBoxImmutableHash(expected)) {
        throw ErrCorruptRegion("TrieBox geometry mismatch");
    }
    for (std::uint64_t offset = 20; offset < 24; ++offset) {
        if (bytes[offset] != 0) {
            throw ErrCorruptRegion("nonzero TrieBox root reserved byte");
        }
    }
    for (std::uint64_t offset = kTrieReservedOffset;
         offset < kTrieBoxHeaderBytes;
         ++offset) {
        if (bytes[offset] != 0) {
            throw ErrCorruptRegion("nonzero TrieBox reserved byte");
        }
    }
    const auto gap_begin = checkedAdd(
        expected.header_offset, kTrieBoxHeaderBytes);
    if (!std::all_of(
            base + gap_begin,
            base + expected.node_metadata_offset,
            [](std::uint8_t byte) { return byte == 0; })) {
        throw ErrCorruptRegion("nonzero TrieBox alignment padding");
    }
    return expected;
}

std::uint8_t* artBoxHeaderBytes(
    std::uint8_t* base,
    const RegionHeader* header) {
    if (header->engine_offset == 0 ||
        header->engine_offset > header->prefix.region_max ||
        kArtBoxHeaderBytes >
            header->prefix.region_max - header->engine_offset) {
        throw ErrCorruptRegion("invalid ArtBox header offset");
    }
    return base + header->engine_offset;
}

PersistentLe32* artBoxRootStorage(
    std::uint8_t* base,
    const RegionHeader* header) {
    auto* storage = artBoxHeaderBytes(base, header) + kArtBoxRootOffset;
    if (reinterpret_cast<std::uintptr_t>(storage) %
            alignof(PersistentLe32) != 0) {
        throw ErrCorruptRegion("unaligned ArtBox committed root");
    }
    return reinterpret_cast<PersistentLe32*>(storage);
}

bool sameArtBoxGeometry(
    const ArtBoxGeometry& left,
    const ArtBoxGeometry& right) noexcept {
    if (left.header_offset != right.header_offset ||
        left.box_metadata_offset != right.box_metadata_offset ||
        left.box_metadata_bytes != right.box_metadata_bytes ||
        left.box_data_offset != right.box_data_offset ||
        left.box_data_bytes != right.box_data_bytes ||
        left.node_capacity != right.node_capacity ||
        left.header_width != right.header_width) {
        return false;
    }
    for (std::size_t index = 0; index < left.slabs.size(); ++index) {
        const auto& a = left.slabs[index];
        const auto& b = right.slabs[index];
        if (a.metadata_offset != b.metadata_offset ||
            a.metadata_bytes != b.metadata_bytes ||
            a.zone_offset != b.zone_offset ||
            a.zone_bytes != b.zone_bytes) {
            return false;
        }
    }
    return true;
}

ArtBoxGeometry readArtBoxGeometry(
    std::uint8_t* base,
    const RegionHeader* header) {
    const auto* bytes = artBoxHeaderBytes(base, header);
    if (!std::equal(kArtBoxMagic.begin(), kArtBoxMagic.end(), bytes) ||
        loadLe32(bytes + 8) != kArtBoxVersion ||
        loadLe32(bytes + 12) != kArtBoxHeaderBytes) {
        throw ErrCorruptRegion("invalid ArtBox persistent header");
    }
    ArtBoxGeometry stored;
    stored.header_offset = header->engine_offset;
    for (std::size_t index = 0; index < stored.slabs.size(); ++index) {
        const auto offset = kArtBoxSlabDescriptorsOffset +
            index * kArtBoxSlabDescriptorBytes;
        auto& slab = stored.slabs[index];
        slab.metadata_offset = loadLe64(bytes + offset);
        slab.metadata_bytes = loadLe64(bytes + offset + 8);
        slab.zone_offset = loadLe64(bytes + offset + 16);
        slab.zone_bytes = loadLe64(bytes + offset + 24);
    }
    stored.box_metadata_offset = loadLe64(
        bytes + kArtBoxBoxMetadataOffset);
    stored.box_metadata_bytes = loadLe64(
        bytes + kArtBoxBoxMetadataBytesOffset);
    stored.box_data_offset = loadLe64(bytes + kArtBoxBoxDataOffset);
    stored.box_data_bytes = loadLe64(bytes + kArtBoxBoxDataBytesOffset);
    stored.node_capacity = loadLe32(bytes + kArtBoxNodeCapacityOffset);
    stored.header_width = bytes[kArtBoxHeaderWidthOffset];

    ArtBoxGeometry expected;
    try {
        expected = makeArtBoxGeometry(
            header->engine_offset,
            header->prefix.region_max,
            header->page_size,
            header->entry_limit);
    } catch (const Error& error) {
        throw ErrCorruptRegion(error.what());
    } catch (const AllocatorError& error) {
        throw ErrCorruptRegion(error.what());
    }
    if (!sameArtBoxGeometry(stored, expected) ||
        loadLe64(bytes + kArtBoxImmutableHashOffset) !=
            artBoxImmutableHash(stored)) {
        throw ErrCorruptRegion("ArtBox geometry mismatch");
    }
    for (std::size_t index = 0; index < kArtBoxPayloadBytes.size(); ++index) {
        if (loadLe32(bytes + kArtBoxPayloadSizesOffset + index * 4) !=
            kArtBoxPayloadBytes[index]) {
            throw ErrCorruptRegion("ArtBox node payload size mismatch");
        }
    }
    for (std::uint64_t offset = 20; offset < 24; ++offset) {
        if (bytes[offset] != 0) {
            throw ErrCorruptRegion("nonzero ArtBox root reserved byte");
        }
    }
    for (std::uint64_t offset = 197; offset < 200; ++offset) {
        if (bytes[offset] != 0) {
            throw ErrCorruptRegion("nonzero ArtBox width reserved byte");
        }
    }
    for (std::uint64_t offset = kArtBoxReservedOffset;
         offset < kArtBoxHeaderBytes;
         ++offset) {
        if (bytes[offset] != 0) {
            throw ErrCorruptRegion("nonzero ArtBox reserved byte");
        }
    }
    const auto require_zero = [base](
                                  std::uint64_t first,
                                  std::uint64_t last) {
        if (!std::all_of(base + first, base + last,
                         [](std::uint8_t byte) { return byte == 0; })) {
            throw ErrCorruptRegion("nonzero ArtBox alignment padding");
        }
    };
    require_zero(
        checkedAdd(header->engine_offset, kArtBoxHeaderBytes),
        stored.slabs.front().metadata_offset);
    for (std::size_t index = 0; index + 1 < stored.slabs.size(); ++index) {
        require_zero(
            checkedAdd(
                stored.slabs[index].zone_offset,
                stored.slabs[index].zone_bytes),
            stored.slabs[index + 1].metadata_offset);
    }
    require_zero(
        checkedAdd(stored.slabs.back().zone_offset,
                   stored.slabs.back().zone_bytes),
        stored.box_metadata_offset);
    return stored;
}

struct Layout {
    ShmEngine engine;
    std::uint64_t entry_limit;
    std::uint64_t table_capacity;
    std::uint64_t queue_limit;
    std::uint64_t queue_capacity;
    std::uint64_t table_offset[2];
    std::uint64_t queue_offset;
    std::uint64_t heap_offset;
    std::uint64_t heap_limit;
    std::uint64_t engine_offset;
    std::uint64_t engine_size;
    HashBoxGeometry hash_box;
    std::uint64_t trie_header_offset;
    std::uint64_t trie_node_metadata_offset;
    std::uint64_t trie_node_metadata_bytes;
    std::uint64_t trie_node_zone_offset;
    std::uint64_t trie_node_zone_bytes;
    std::uint64_t trie_box_metadata_offset;
    std::uint64_t trie_box_metadata_bytes;
    std::uint64_t trie_box_data_offset;
    std::uint64_t trie_box_data_bytes;
    ArtBumpGeometry art_bump;
    ArtBoxGeometry art_box;
    std::uint64_t initial_size;
    std::uint64_t max_size;
    std::uint64_t page_size;
};

struct CanonicalCommonGeometry {
    std::uint64_t table_capacity;
    std::uint64_t table_offset[2];
    std::uint64_t queue_capacity;
    std::uint64_t queue_offset;
    std::uint64_t heap_offset;
    std::uint64_t minimum_region_size;
    std::uint64_t heap_limit;
    std::uint64_t engine_offset;
    std::uint64_t engine_size;
};

std::uint64_t hostPageSize() {
    const long page_result = ::sysconf(_SC_PAGESIZE);
    if (page_result <= 0) throw Error("kvspace: cannot determine page size");
    return static_cast<std::uint64_t>(page_result);
}

constexpr std::uint64_t kSupportedPageSize = 4096;

void requireRepresentableRegionSize(std::uint64_t size) {
    if (size == 0 ||
        size > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()) ||
        size > static_cast<std::uint64_t>(
            std::numeric_limits<off_t>::max())) {
        throw ErrCapacity("region size is not representable");
    }
}

CanonicalCommonGeometry rebuildCanonicalCommonGeometry(
    ShmEngine engine,
    std::uint64_t page_size,
    std::uint64_t entry_limit,
    std::uint64_t queue_limit,
    std::uint64_t region_max) {
    CanonicalCommonGeometry geometry{};
    geometry.queue_capacity = slotCapacity(queue_limit);
    if (engine == ShmEngine::HashBox) {
        geometry.table_capacity = slotCapacity(entry_limit);
        geometry.table_offset[0] = alignUp(sizeof(RegionHeader), page_size);
        const auto table_bytes = checkedMultiply(
            geometry.table_capacity, kHashBoxSlotBytes);
        geometry.table_offset[1] = alignUp(
            checkedAdd(geometry.table_offset[0], table_bytes), page_size);
        geometry.queue_offset = alignUp(
            checkedAdd(geometry.table_offset[1], table_bytes), page_size);
    } else {
        geometry.queue_offset = alignUp(sizeof(RegionHeader), page_size);
    }
    const auto queue_bytes = checkedMultiply(
        geometry.queue_capacity, sizeof(QueueSlot));
    geometry.heap_offset = alignUp(
        checkedAdd(geometry.queue_offset, queue_bytes), page_size);
    geometry.minimum_region_size = alignUp(
        std::max(
            checkedAdd(geometry.heap_offset, page_size),
            kMinimumRegionSize),
        page_size);
    if (geometry.minimum_region_size > region_max) {
        throw ErrCapacity("maximum size cannot hold the common layout");
    }

    geometry.heap_limit = region_max;
    const auto available = region_max - geometry.heap_offset;
    const auto queue_heap_bytes = (available / 8 / page_size) * page_size;
    if (queue_heap_bytes < page_size) {
        throw ErrCapacity("max_size cannot hold the queue heap");
    }
    geometry.heap_limit = checkedAdd(
        geometry.heap_offset, queue_heap_bytes);
    geometry.engine_offset = geometry.heap_limit;
    geometry.engine_size = region_max - geometry.engine_offset;
    return geometry;
}

Layout makeLayout(const ShmOptions& options) {
    if (!hostIsLittleEndian()) {
        throw ErrVersionMismatch("shared-memory records require little endian");
    }
    requireImplementedEngine(options.engine);
    const auto page = hostPageSize();
    if (page != kSupportedPageSize) {
        throw ErrVersionMismatch("shared-memory engines require 4096-byte pages");
    }
    Layout layout{};
    layout.engine = options.engine;
    layout.page_size = page;
    layout.entry_limit = options.max_entries;
    if (options.max_entries == 0) {
        throw ErrCapacity("entry limit must be positive");
    }
    layout.queue_limit = options.max_queues;
    if (layout.queue_limit == 0) {
        throw ErrCapacity("queue limit must be positive");
    }
    layout.max_size = options.max_size;
    requireRepresentableRegionSize(layout.max_size);
    const auto canonical = rebuildCanonicalCommonGeometry(
        options.engine,
        page,
        layout.entry_limit,
        layout.queue_limit,
        layout.max_size);
    layout.table_capacity = canonical.table_capacity;
    layout.table_offset[0] = canonical.table_offset[0];
    layout.table_offset[1] = canonical.table_offset[1];
    layout.queue_capacity = canonical.queue_capacity;
    layout.queue_offset = canonical.queue_offset;
    layout.heap_offset = canonical.heap_offset;
    layout.heap_limit = canonical.heap_limit;
    layout.engine_offset = canonical.engine_offset;
    layout.engine_size = canonical.engine_size;
    layout.initial_size = std::max(
        options.initial_size, canonical.minimum_region_size);
    if (layout.max_size < layout.initial_size) {
        throw ErrCapacity("max_size is smaller than the required initial layout");
    }
    requireRepresentableRegionSize(layout.initial_size);
    if (options.engine == ShmEngine::ArtBump ||
        options.engine == ShmEngine::HashBox ||
        options.engine == ShmEngine::TrieBox ||
        options.engine == ShmEngine::ArtBox) {
        if (options.engine == ShmEngine::ArtBump) {
            try {
                layout.art_bump = ArtBumpGeometry::Compute(
                    layout.max_size,
                    layout.entry_limit,
                    layout.queue_capacity,
                    layout.page_size,
                    sizeof(RegionHeader),
                    sizeof(QueueSlot));
            } catch (const AllocatorError& error) {
                if (error.Code() == AllocatorErrorCode::Capacity) {
                    throw ErrCapacity(error.what());
                }
                throw Error(error.what());
            }
            if (layout.art_bump.queue_offset != layout.queue_offset ||
                layout.art_bump.heap_offset != layout.heap_offset ||
                layout.art_bump.engine_offset != layout.engine_offset ||
                layout.art_bump.engine_bytes != layout.engine_size) {
                throw ErrCapacity("ArtBump common and engine geometry disagree");
            }
        } else if (options.engine == ShmEngine::HashBox) {
            layout.hash_box = makeHashBoxGeometry(
                layout.engine_offset, layout.max_size);
        } else if (options.engine == ShmEngine::TrieBox) {
            const auto trie = makeTrieBoxGeometry(
                layout.engine_offset, layout.max_size, layout.page_size);
            layout.trie_header_offset = trie.header_offset;
            layout.trie_node_metadata_offset = trie.node_metadata_offset;
            layout.trie_node_metadata_bytes = trie.node_metadata_bytes;
            layout.trie_node_zone_offset = trie.node_zone_offset;
            layout.trie_node_zone_bytes = trie.node_zone_bytes;
            layout.trie_box_metadata_offset = trie.box_metadata_offset;
            layout.trie_box_metadata_bytes = trie.box_metadata_bytes;
            layout.trie_box_data_offset = trie.box_data_offset;
            layout.trie_box_data_bytes = trie.box_data_bytes;
        } else {
            layout.art_box = makeArtBoxGeometry(
                layout.engine_offset,
                layout.max_size,
                layout.page_size,
                layout.entry_limit);
        }
    }
    return layout;
}

bool incompleteBacking(int fd, off_t file_size) {
    if (file_size == 0) return true;
    const auto available = static_cast<std::size_t>(std::min<off_t>(
        file_size, static_cast<off_t>(sizeof(RegionPrefix))));
    std::array<std::uint8_t, sizeof(RegionPrefix)> bytes{};
    const auto count = ::pread(fd, bytes.data(), available, 0);
    if (count != static_cast<ssize_t>(available)) {
        throw Error(systemError("read initialization marker"));
    }
    if (std::all_of(bytes.begin(), bytes.begin() + available,
                    [](std::uint8_t byte) { return byte == 0; })) {
        return true;
    }
    if (available < sizeof(RegionPrefix)) return false;
    return std::memcmp(bytes.data(), kMagic.data(), kMagic.size()) == 0 &&
        loadLe32(bytes.data() + 16) != kInitReady;
}

RegionPrefix decodePrefixBytes(const std::uint8_t* bytes) noexcept {
    RegionPrefix prefix{};
    std::memcpy(prefix.magic, bytes, sizeof(prefix.magic));
    prefix.version = loadLe32(bytes + 8);
    prefix.endian = loadLe32(bytes + 12);
    prefix.init_state = loadLe32(bytes + 16);
    prefix.header_size = loadLe32(bytes + 20);
    prefix.engine_id = loadLe32(bytes + 24);
    prefix.engine_abi = loadLe32(bytes + 28);
    prefix.common_layout_hash = loadLe64(bytes + 32);
    prefix.engine_layout_hash = loadLe64(bytes + 40);
    prefix.region_size = loadLe64(bytes + 48);
    prefix.region_max = loadLe64(bytes + 56);
    return prefix;
}

} // namespace

struct Region::Impl {
    int fd = -1;
    std::uint8_t* base = nullptr;
    std::uint64_t mapped_size = 0;
    RegionHeader* header = nullptr;
    std::string name;
    bool regular_file = false;
    bool closed = false;
    bool mutation_active = false;
    std::vector<std::uint64_t> mutation_allocations;
    std::unique_ptr<ShmEngineStore> engine;
    std::optional<CommonApplyPhase> common_apply_phase;
    std::array<
        std::uint64_t,
        static_cast<std::size_t>(CommonApplyStep::Count)>
        common_apply_ordinals{};

    ~Impl() { closeNoThrow(); }

    class CommonApplyTraceScope {
    public:
        CommonApplyTraceScope(Impl* owner, CommonApplyPhase phase)
            : owner_(owner) {
            owner_->common_apply_phase = phase;
            owner_->common_apply_ordinals.fill(0);
        }
        ~CommonApplyTraceScope() { owner_->common_apply_phase.reset(); }
        CommonApplyTraceScope(const CommonApplyTraceScope&) = delete;
        CommonApplyTraceScope& operator=(const CommonApplyTraceScope&) =
            delete;

    private:
        Impl* owner_;
    };

    void afterCommonApplyWrite(CommonApplyStep step) noexcept {
        if (!common_apply_phase.has_value()) return;
        const auto index = static_cast<std::size_t>(step);
        if (index >= common_apply_ordinals.size()) std::abort();
        const auto ordinal = common_apply_ordinals[index]++;
        emitCommonApplyTestEvent(*common_apply_phase, step, ordinal);
    }

    template <typename Stored, typename Value>
    void storeCommon(
        Stored* destination,
        Value value,
        CommonApplyStep step) noexcept {
        *destination = value;
        afterCommonApplyWrite(step);
    }

    template <typename Encoded>
    void atomicStoreCommon(
        Encoded* destination,
        typename Encoded::value_type value,
        int memory_order,
        CommonApplyStep step) noexcept {
        atomicStoreLe(destination, value, memory_order);
        afterCommonApplyWrite(step);
    }

    void atomicStoreCommonByte(
        std::uint8_t* destination,
        std::uint8_t value,
        int memory_order,
        CommonApplyStep step) noexcept {
        __atomic_store_n(destination, value, memory_order);
        afterCommonApplyWrite(step);
    }

    template <typename T>
    T* at(std::uint64_t offset) const {
        if (offset == 0 || offset > mapped_size || sizeof(T) > mapped_size - offset) {
            throw ErrCorruptRegion("offset outside mapping");
        }
        return reinterpret_cast<T*>(base + offset);
    }

    QueueSlot* queueTable() const { return at<QueueSlot>(header->queue_offset); }

    void requireZeroRange(
        std::uint64_t first,
        std::uint64_t last,
        const char* description) const {
        if (last < first || last > mapped_size) {
            throw ErrCorruptRegion("invalid zero-padding range");
        }
        if (!std::all_of(
                base + first,
                base + last,
                [](std::uint8_t byte) { return byte == 0; })) {
            throw ErrCorruptRegion(description);
        }
    }

    ShmEngine storedEngine() const {
        return decodeEngine(header->prefix.engine_id);
    }

    void closeNoThrow() noexcept {
        engine.reset();
        if (base != nullptr && mapped_size != 0) {
            ::munmap(base, static_cast<std::size_t>(mapped_size));
        }
        if (fd >= 0) ::close(fd);
        base = nullptr;
        mapped_size = 0;
        header = nullptr;
        fd = -1;
        closed = true;
        mutation_active = false;
    }

    void validateStaticHeader() const {
        if (!hostIsLittleEndian()) {
            throw ErrVersionMismatch(
                "shared-memory records require little endian");
        }
        if (header == nullptr || std::memcmp(header->prefix.magic, kMagic.data(), kMagic.size()) != 0) {
            throw ErrCorruptRegion("bad magic");
        }
        if (header->prefix.version != kVersion || header->prefix.endian != kEndianMarker ||
            header->prefix.common_layout_hash != commonLayoutHash()) {
            throw ErrVersionMismatch("incompatible shared memory layout");
        }
        const auto stored_engine = decodeEngine(header->prefix.engine_id);
        requireImplementedEngine(stored_engine);
        if (header->prefix.engine_abi != engineAbi(stored_engine) ||
            header->prefix.engine_layout_hash != engineLayoutHash(stored_engine) ||
            header->prefix.header_size != sizeof(RegionHeader)) {
            throw ErrVersionMismatch("incompatible engine layout");
        }
        if (atomicLoadLe(&header->prefix.init_state, __ATOMIC_ACQUIRE) !=
            kInitReady) {
            throw ErrCorruptRegion("region initialization is incomplete");
        }
        const auto region_size = atomicLoadLe(
            &header->prefix.region_size, __ATOMIC_ACQUIRE);
        struct stat status {};
        if (::fstat(fd, &status) != 0) throw Error(systemError("fstat"));
        if (status.st_size < 0 ||
            static_cast<std::uint64_t>(status.st_size) < region_size ||
            header->prefix.region_max != mapped_size ||
            region_size < sizeof(RegionHeader) ||
            region_size != header->prefix.region_max ||
            header->heap_offset > region_size) {
            throw ErrCorruptRegion("invalid region bounds");
        }
        const auto power_of_two = [](std::uint64_t value) {
            return value != 0 && (value & (value - 1)) == 0;
        };
        if (hostPageSize() != kSupportedPageSize ||
            header->page_size != kSupportedPageSize ||
            !power_of_two(header->queue_capacity) ||
            header->entry_limit == 0 || header->queue_limit == 0 ||
            header->queue_limit > header->queue_capacity ||
            header->heap_offset % kAlignment != 0) {
            throw ErrCorruptRegion("invalid region layout values");
        }
        CanonicalCommonGeometry canonical{};
        try {
            canonical = rebuildCanonicalCommonGeometry(
                stored_engine,
                header->page_size,
                header->entry_limit,
                header->queue_limit,
                header->prefix.region_max);
        } catch (const ErrCapacity&) {
            throw ErrCorruptRegion("invalid canonical region geometry");
        }
        if (region_size < canonical.minimum_region_size ||
            header->table_capacity != canonical.table_capacity ||
            header->table_offset[0] != canonical.table_offset[0] ||
            header->table_offset[1] != canonical.table_offset[1] ||
            header->queue_capacity != canonical.queue_capacity ||
            header->queue_offset != canonical.queue_offset ||
            header->heap_offset != canonical.heap_offset ||
            header->heap_limit != canonical.heap_limit ||
            header->engine_offset != canonical.engine_offset ||
            header->engine_size != canonical.engine_size) {
            throw ErrCorruptRegion("noncanonical persistent region geometry");
        }
        if (!std::all_of(
                std::begin(header->reserved_allocator_slots),
                std::end(header->reserved_allocator_slots),
                [](std::uint64_t value) { return value == 0; })) {
            throw ErrCorruptRegion("nonzero reserved allocator slot");
        }
        requireZeroRange(
            offsetof(RegionHeader, notify_cond) + sizeof(pthread_cond_t),
            sizeof(RegionHeader),
            "nonzero RegionHeader outer padding");
        const auto table_bytes = checkedMultiply(
            header->table_capacity, kHashBoxSlotBytes);
        const auto queue_bytes = checkedMultiply(
            header->queue_capacity, sizeof(QueueSlot));
        const auto table_zero_end = checkedAdd(header->table_offset[0], table_bytes);
        const auto table_one_end = checkedAdd(header->table_offset[1], table_bytes);
        const auto queue_end = checkedAdd(header->queue_offset, queue_bytes);
        if (header->queue_offset < sizeof(RegionHeader) ||
            header->heap_offset < queue_end ||
            header->heap_offset > region_size ||
            header->heap_limit < header->heap_offset ||
            header->heap_limit > header->prefix.region_max) {
            throw ErrCorruptRegion("overlapping region sections");
        }
        if (header->engine_offset != header->heap_limit ||
            header->engine_size !=
                header->prefix.region_max - header->engine_offset) {
            throw ErrCorruptRegion("invalid fixed engine outer section");
        }
        if (stored_engine == ShmEngine::HashBox) {
            if (!power_of_two(header->table_capacity) ||
                header->entry_limit > header->table_capacity ||
                header->table_offset[0] < sizeof(RegionHeader) ||
                header->table_offset[1] < table_zero_end ||
                header->queue_offset < table_one_end ||
                header->root_offset != 0 || header->node_count != 0 ||
                header->heap_limit > region_size) {
                throw ErrCorruptRegion("invalid hash-box layout values");
            }
            requireZeroRange(
                sizeof(RegionHeader),
                header->table_offset[0],
                "nonzero header-to-table padding");
            requireZeroRange(
                table_zero_end,
                header->table_offset[1],
                "nonzero HashBox table padding");
            requireZeroRange(
                table_one_end,
                header->queue_offset,
                "nonzero HashBox queue padding");
            static_cast<void>(readHashBoxGeometry(base, header));
        } else {
            if (header->table_capacity != 0 || header->table_offset[0] != 0 ||
                header->table_offset[1] != 0) {
                throw ErrCorruptRegion("invalid tree-engine layout values");
            }
            requireZeroRange(
                sizeof(RegionHeader),
                header->queue_offset,
                "nonzero header-to-queue padding");
            if (stored_engine == ShmEngine::ArtBox) {
                if (header->root_offset != 0 ||
                    header->heap_limit > region_size) {
                    throw ErrCorruptRegion("invalid ArtBox outer sections");
                }
                static_cast<void>(readArtBoxGeometry(base, header));
            } else if (stored_engine == ShmEngine::TrieBox) {
                if (header->root_offset != 0 ||
                    header->heap_limit > region_size) {
                    throw ErrCorruptRegion("invalid TrieBox outer sections");
                }
                static_cast<void>(readTrieBoxGeometry(base, header));
            } else if (stored_engine == ShmEngine::ArtBump) {
                // The fixed-section ArtBump module validates the engine tail.
                // COMMON owns only the canonical outer split above.
            }
        }
        requireZeroRange(
            queue_end,
            header->heap_offset,
            "nonzero queue-to-heap padding");
    }

    void validateMutableHeader(bool recovering) const {
        const auto region_size = atomicLoadLe(
            &header->prefix.region_size, __ATOMIC_ACQUIRE);
        const auto stored_engine = decodeEngine(header->prefix.engine_id);
        const auto active_table = atomicLoadLe(
            &header->active_table, __ATOMIC_ACQUIRE);
        if (header->heap_top < header->heap_offset ||
            header->heap_top > region_size ||
            header->heap_top > header->heap_limit ||
            header->heap_top % kAlignment != 0 ||
            (stored_engine == ShmEngine::HashBox && active_table > 1) ||
            (stored_engine != ShmEngine::HashBox &&
             (active_table != 0 || header->tombstone_count != 0))) {
            throw ErrCorruptRegion("invalid mutable region bounds");
        }
        if (!recovering &&
            ((header->heap_top == header->heap_offset && header->heap_last != 0) ||
             (header->heap_top != header->heap_offset &&
              (header->heap_last < header->heap_offset ||
               header->heap_last >= header->heap_top)))) {
            throw ErrCorruptRegion("invalid heap tail");
        }
        if (!recovering && header->heap_last != 0) {
            const auto* tail = blob(header->heap_last, false);
            if (header->heap_last + tail->span != header->heap_top) {
                throw ErrCorruptRegion("heap tail does not reach heap top");
            }
        }
        if (!recovering && atomicLoadLe(
                &header->allocator_journal.state, __ATOMIC_ACQUIRE) !=
                kAllocatorJournalIdle) {
            throw ErrCorruptRegion("unfinished allocator journal");
        }
        if (!std::all_of(
                std::begin(header->allocator_journal.reserved64),
                std::end(header->allocator_journal.reserved64),
                [](std::uint64_t value) { return value == 0; })) {
            throw ErrCorruptRegion("nonzero allocator journal reserved word");
        }
        if (header->corrupt != 0) throw ErrCorruptRegion("region marked corrupt");
    }

    void validateHeader() const {
        validateStaticHeader();
        validateMutableHeader(false);
    }

    BlobHeader* blob(std::uint64_t offset, bool require_used = true) const {
        if (offset < header->heap_offset || offset >= header->heap_top ||
            offset % kAlignment != 0) {
            throw ErrCorruptRegion("invalid blob offset");
        }
        auto* result = at<BlobHeader>(offset);
        if (result->magic != kBlobMagic ||
            result->span < kMinimumBlockSize ||
            result->span % kAlignment != 0 || result->span > header->heap_top - offset ||
            result->payload_size > result->span - sizeof(BlobHeader) ||
            result->reserved16 != 0 || result->reserved32 != 0 ||
            (result->flags != 0 && result->flags != kBlobUsed)) {
            throw ErrCorruptRegion("invalid blob header");
        }
        if (require_used && result->flags != kBlobUsed) {
            throw ErrCorruptRegion("reference to free blob");
        }
        return result;
    }

    BlobHeader* recoveryBlob(std::uint64_t offset) const {
        if (offset < header->heap_offset || offset >= header->heap_top ||
            offset % kAlignment != 0) {
            throw ErrCorruptRegion("invalid recovery Blob offset");
        }
        auto* result = at<BlobHeader>(offset);
        if (result->magic != kBlobMagic ||
            result->span < kMinimumBlockSize ||
            result->span % kAlignment != 0 ||
            result->span > header->heap_top - offset ||
            result->reserved16 != 0 || result->reserved32 != 0 ||
            (result->flags != 0 && result->flags != kBlobUsed)) {
            throw ErrCorruptRegion("invalid structural recovery Blob header");
        }
        return result;
    }

    void removeFree(std::uint64_t offset) {
        auto* block = blob(offset, false);
        if (block->flags != 0) throw ErrCorruptRegion("invalid blob on free list");
        const auto bucket = floorLog2(block->span);
        if (block->previous_free != 0) {
            blob(block->previous_free, false)->next_free = block->next_free;
        } else {
            if (header->free_heads[bucket] != offset) {
                throw ErrCorruptRegion("free-list head mismatch");
            }
            header->free_heads[bucket] = block->next_free;
        }
        if (block->next_free != 0) {
            blob(block->next_free, false)->previous_free = block->previous_free;
        }
        block->next_free = 0;
        block->previous_free = 0;
    }

    void insertFree(std::uint64_t offset) {
        auto* block = blob(offset, false);
        storeCommon(
            &block->flags, std::uint8_t{0},
            CommonApplyStep::BlobFlagsStored);
        storeCommon(
            &block->payload_size, std::uint32_t{0},
            CommonApplyStep::BlobPayloadSizeStored);
        storeCommon(
            &block->mark, std::uint8_t{0},
            CommonApplyStep::BlobMarkStored);
        const auto bucket = floorLog2(block->span);
        const auto old_head = header->free_heads[bucket];
        storeCommon(
            &block->previous_free, std::uint64_t{0},
            CommonApplyStep::BlobPreviousFreeStored);
        storeCommon(
            &block->next_free, old_head,
            CommonApplyStep::BlobNextFreeStored);
        if (old_head != 0) {
            storeCommon(
                &blob(old_head, false)->previous_free, offset,
                CommonApplyStep::BlobPreviousFreeStored);
        }
        storeCommon(
            &header->free_heads[bucket], offset,
            CommonApplyStep::FreeHeadStored);
    }

    static std::uint64_t allocatorJournalChecksum(
        std::uint64_t offset,
        std::uint64_t original_span,
        std::uint64_t requested_span) noexcept {
        return kAllocatorJournalSalt ^ offset ^ original_span ^ requested_span;
    }

    static std::uint64_t allocatorCoalesceJournalChecksum(
        std::uint64_t offset,
        std::uint64_t left_span,
        std::uint64_t right_span) noexcept {
        return kAllocatorCoalesceJournalSalt ^ offset ^ left_span ^ right_span;
    }

    static bool allocatorJournalReservedZero(
        const AllocatorJournal& journal) noexcept {
        return std::all_of(
            std::begin(journal.reserved64),
            std::end(journal.reserved64),
            [](std::uint64_t value) { return value == 0; });
    }

    static bool canonicalGenericFreeFields(
        const BlobHeader& block) noexcept {
        return block.next_free == 0 && block.previous_free == 0 &&
            block.payload_size == 0 && block.magic == kBlobMagic &&
            block.flags == 0 && block.mark == 0 && block.reserved16 == 0 &&
            block.reserved32 == 0;
    }

    void completeAllocatorSplitJournal(
        AllocatorJournal& journal,
        bool recovering) {
        const auto offset = journal.offset;
        const auto original_span = journal.original_span;
        const auto requested_span = journal.requested_span;
        if (journal.reserved32 != 0) {
            throw ErrCorruptRegion("invalid allocator split class");
        }
        if (journal.checksum != allocatorJournalChecksum(
                offset, original_span, requested_span) ||
            offset < header->heap_offset || offset >= header->heap_top ||
            offset % kAlignment != 0 ||
            original_span % kAlignment != 0 ||
            requested_span % kAlignment != 0 ||
            original_span > header->heap_top - offset ||
            requested_span < kMinimumBlockSize ||
            requested_span > original_span ||
            original_span - requested_span < kMinimumBlockSize) {
            throw ErrCorruptRegion("invalid allocator split journal");
        }

        auto* block = at<BlobHeader>(offset);
        if (block->magic != kBlobMagic ||
            block->reserved16 != 0 || block->reserved32 != 0 ||
            block->flags != 0 ||
            (block->span != original_span && block->span != requested_span)) {
            throw ErrCorruptRegion("allocator split source changed");
        }
        const auto remainder_offset = offset + requested_span;
        auto* remainder = ::new (
            static_cast<void*>(base + remainder_offset)) BlobHeader{};
        remainder->span = original_span - requested_span;
        remainder->previous_span = requested_span;
        remainder->magic = kBlobMagic;
        remainder->flags = 0;
        emitRegionTestEvent(RegionTestEvent::AllocatorSplitRemainderReady);

        const auto following_offset = offset + original_span;
        if (following_offset < header->heap_top) {
            auto* following = recovering
                ? recoveryBlob(following_offset)
                : blob(following_offset, false);
            following->previous_span = remainder->span;
        } else {
            if (header->heap_last != offset &&
                header->heap_last != remainder_offset) {
                throw ErrCorruptRegion("allocator split tail changed");
            }
            header->heap_last = remainder_offset;
        }
        emitRegionTestEvent(RegionTestEvent::AllocatorSplitLinksReady);
        atomicStoreLe(&block->span, requested_span, __ATOMIC_RELEASE);
        emitRegionTestEvent(RegionTestEvent::AllocatorSplitBoundaryCommitted);
        atomicStoreLe(
            &journal.state, kAllocatorJournalIdle, __ATOMIC_RELEASE);
        emitRegionTestEvent(RegionTestEvent::AllocatorSplitJournalCleared);
    }

    void completeAllocatorCoalesceJournal(
        AllocatorJournal& journal,
        bool recovering) {
        const auto offset = journal.offset;
        const auto left_span = journal.original_span;
        const auto right_span = journal.requested_span;
        if (journal.reserved32 != 0 ||
            journal.checksum != allocatorCoalesceJournalChecksum(
                offset, left_span, right_span) ||
            offset < header->heap_offset || offset >= header->heap_top ||
            offset % kAlignment != 0 ||
            left_span < kMinimumBlockSize ||
            right_span < kMinimumBlockSize ||
            left_span % kAlignment != 0 ||
            right_span % kAlignment != 0 ||
            left_span > header->heap_top - offset) {
            throw ErrCorruptRegion("invalid allocator coalesce journal");
        }
        const auto right_offset = offset + left_span;
        if (right_offset >= header->heap_top ||
            right_span > header->heap_top - right_offset) {
            throw ErrCorruptRegion("allocator coalesce range is out of bounds");
        }
        const auto combined_span = left_span + right_span;
        const auto following_offset = right_offset + right_span;
        auto* left = at<BlobHeader>(offset);
        auto* right = at<BlobHeader>(right_offset);
        if (!canonicalGenericFreeFields(*left) ||
            !canonicalGenericFreeFields(*right) ||
            (left->span != left_span && left->span != combined_span) ||
            right->span != right_span ||
            right->previous_span != left_span ||
            (offset == header->heap_offset && left->previous_span != 0) ||
            (offset != header->heap_offset &&
             (left->previous_span < kMinimumBlockSize ||
              left->previous_span % kAlignment != 0 ||
              left->previous_span > offset - header->heap_offset))) {
            throw ErrCorruptRegion("allocator coalesce operands changed");
        }

        BlobHeader* following = nullptr;
        if (following_offset < header->heap_top) {
            following = recovering
                ? recoveryBlob(following_offset)
                : blob(following_offset, false);
            if (following->previous_span != right_span &&
                following->previous_span != combined_span) {
                throw ErrCorruptRegion(
                    "allocator coalesce following boundary changed");
            }
        } else if (following_offset == header->heap_top) {
            if (header->heap_last != right_offset &&
                header->heap_last != offset) {
                throw ErrCorruptRegion("allocator coalesce tail changed");
            }
        } else {
            throw ErrCorruptRegion("allocator coalesce exceeds heap top");
        }

        atomicStoreCommon(
            &left->span,
            combined_span,
            __ATOMIC_RELEASE,
            CommonApplyStep::BlobSpanStored);
        emitRegionTestEvent(
            RegionTestEvent::AllocatorCoalesceBoundaryCommitted);
        if (following != nullptr) {
            storeCommon(
                &following->previous_span,
                combined_span,
                CommonApplyStep::BlobPreviousSpanStored);
            emitRegionTestEvent(
                RegionTestEvent::AllocatorCoalesceFollowingLinked);
        } else {
            storeCommon(
                &header->heap_last,
                offset,
                CommonApplyStep::HeapLastStored);
            emitRegionTestEvent(
                RegionTestEvent::AllocatorCoalesceTailInstalled);
        }
        atomicStoreCommon(
            &journal.state,
            kAllocatorJournalIdle,
            __ATOMIC_RELEASE,
            CommonApplyStep::AllocatorJournalStateStored);
        emitRegionTestEvent(
            RegionTestEvent::AllocatorCoalesceJournalCleared);
    }

    void completeAllocatorJournal(bool recovering = false) {
        auto& journal = header->allocator_journal;
        const auto state = atomicLoadLe(&journal.state, __ATOMIC_ACQUIRE);
        if (!allocatorJournalReservedZero(journal)) {
            throw ErrCorruptRegion("nonzero allocator journal reserved word");
        }
        if (state == kAllocatorJournalIdle) return;
        if (state == kAllocatorJournalSplit) {
            completeAllocatorSplitJournal(journal, recovering);
            return;
        }
        if (state == kAllocatorJournalCoalesce) {
            completeAllocatorCoalesceJournal(journal, recovering);
            return;
        }
        throw ErrCorruptRegion("invalid allocator journal operation");
    }

    void coalesceFreeBlocks(
        std::uint64_t offset,
        std::uint64_t left_span,
        std::uint64_t right_span) {
        auto& journal = header->allocator_journal;
        if (atomicLoadLe(&journal.state, __ATOMIC_ACQUIRE) !=
            kAllocatorJournalIdle) {
            throw ErrCorruptRegion("allocator journal is busy");
        }
        auto* left = blob(offset, false);
        if (left->span != left_span || left->flags != 0 ||
            left_span > header->heap_top - offset) {
            throw ErrCorruptRegion("invalid allocator coalesce left span");
        }
        auto* right = blob(offset + left_span, false);
        if (right->span != right_span || right->flags != 0 ||
            right->previous_span != left_span) {
            throw ErrCorruptRegion("invalid allocator coalesce pair");
        }
        storeCommon(
            &left->next_free, std::uint64_t{0},
            CommonApplyStep::BlobNextFreeStored);
        storeCommon(
            &left->previous_free, std::uint64_t{0},
            CommonApplyStep::BlobPreviousFreeStored);
        storeCommon(
            &left->payload_size, std::uint32_t{0},
            CommonApplyStep::BlobPayloadSizeStored);
        storeCommon(
            &left->mark, std::uint8_t{0},
            CommonApplyStep::BlobMarkStored);
        storeCommon(
            &right->next_free, std::uint64_t{0},
            CommonApplyStep::BlobNextFreeStored);
        storeCommon(
            &right->previous_free, std::uint64_t{0},
            CommonApplyStep::BlobPreviousFreeStored);
        storeCommon(
            &right->payload_size, std::uint32_t{0},
            CommonApplyStep::BlobPayloadSizeStored);
        storeCommon(
            &right->mark, std::uint8_t{0},
            CommonApplyStep::BlobMarkStored);

        storeCommon(
            &journal.offset, offset,
            CommonApplyStep::AllocatorJournalOffsetStored);
        storeCommon(
            &journal.original_span, left_span,
            CommonApplyStep::AllocatorJournalOriginalSpanStored);
        storeCommon(
            &journal.requested_span, right_span,
            CommonApplyStep::AllocatorJournalRequestedSpanStored);
        storeCommon(
            &journal.checksum,
            allocatorCoalesceJournalChecksum(offset, left_span, right_span),
            CommonApplyStep::AllocatorJournalChecksumStored);
        storeCommon(
            &journal.reserved32, std::uint32_t{0},
            CommonApplyStep::AllocatorJournalReserved32Stored);
        for (auto& reserved : journal.reserved64) {
            storeCommon(
                &reserved, std::uint64_t{0},
                CommonApplyStep::AllocatorJournalReserved64Stored);
        }
        emitRegionTestEvent(
            RegionTestEvent::AllocatorCoalesceJournalPrepared);
        atomicStoreCommon(
            &journal.state,
            kAllocatorJournalCoalesce,
            __ATOMIC_RELEASE,
            CommonApplyStep::AllocatorJournalStateStored);
        emitRegionTestEvent(
            RegionTestEvent::AllocatorCoalesceJournalPublished);
        completeAllocatorJournal();
    }

    void splitFreeBlock(
        std::uint64_t offset,
        std::uint64_t requested_span) {
        auto& journal = header->allocator_journal;
        if (atomicLoadLe(&journal.state, __ATOMIC_ACQUIRE) !=
            kAllocatorJournalIdle) {
            throw ErrCorruptRegion("allocator journal is busy");
        }
        auto* block = blob(offset, false);
        const auto original_span = block->span;
        if (block->flags != 0) {
            throw ErrCorruptRegion("invalid allocator split class");
        }
        if (requested_span < kMinimumBlockSize ||
            requested_span % kAlignment != 0 ||
            requested_span > original_span) {
            throw ErrCorruptRegion("invalid allocator split size");
        }
        if (original_span - requested_span < kMinimumBlockSize) return;
        journal.offset = offset;
        journal.original_span = original_span;
        journal.requested_span = requested_span;
        journal.reserved32 = 0;
        std::fill(
            std::begin(journal.reserved64),
            std::end(journal.reserved64),
            0);
        journal.checksum = allocatorJournalChecksum(
            offset, original_span, requested_span);
        atomicStoreLe(
            &journal.state, kAllocatorJournalSplit, __ATOMIC_RELEASE);
        emitRegionTestEvent(RegionTestEvent::AllocatorSplitJournalPublished);
        completeAllocatorJournal();
        insertFree(offset + requested_span);
    }

    void requireWithinMappedRegion(std::uint64_t required) const {
        if (required <= header->prefix.region_size) return;
        throw ErrCapacity("fixed shared region is full");
    }

    std::uint64_t allocateCopy(const void* data, std::size_t size) {
        if (size > std::numeric_limits<std::uint32_t>::max() ||
            (size != 0 && data == nullptr)) {
            throw ErrCapacity("invalid common Blob allocation");
        }
        if (mutation_active) {
            mutation_allocations.reserve(mutation_allocations.size() + 1U);
        }
        const auto requested = blobSpanForPayload(size);
        std::uint64_t offset = 0;
        for (std::size_t bucket = floorLog2(requested);
             bucket < kFreeListCount && offset == 0;
             ++bucket) {
            for (auto candidate = header->free_heads[bucket];
                 candidate != 0;) {
                auto* free_block = blob(candidate, false);
                const auto next = free_block->next_free;
                if (free_block->span >= requested) {
                    offset = candidate;
                    removeFree(offset);
                    break;
                }
                candidate = next;
            }
        }

        BlobHeader* block = nullptr;
        if (offset != 0) {
            block = blob(offset, false);
            splitFreeBlock(offset, requested);
            block = blob(offset, false);
            if (size != 0) {
                std::memcpy(
                    reinterpret_cast<std::uint8_t*>(block) +
                        sizeof(BlobHeader),
                    data,
                    size);
            }
            block->next_free = 0;
            block->previous_free = 0;
            block->payload_size = static_cast<std::uint32_t>(size);
            block->mark = 0;
            block->magic = kBlobMagic;
            block->reserved16 = 0;
            block->reserved32 = 0;
            __atomic_store_n(&block->flags, kBlobUsed, __ATOMIC_RELEASE);
        } else {
            offset = header->heap_top;
            const auto end = checkedAdd(offset, requested);
            if (end > header->heap_limit) {
                throw ErrCapacity("blob arena is full");
            }
            requireWithinMappedRegion(end);
            block = ::new (
                static_cast<void*>(base + offset)) BlobHeader{};
            block->span = requested;
            block->previous_span = header->heap_last == 0
                ? std::uint64_t{0}
                : static_cast<std::uint64_t>(
                      blob(header->heap_last, false)->span);
            block->payload_size = static_cast<std::uint32_t>(size);
            block->magic = kBlobMagic;
            block->flags = kBlobUsed;
            if (size != 0) {
                std::memcpy(
                    reinterpret_cast<std::uint8_t*>(block) +
                        sizeof(BlobHeader),
                    data,
                    size);
            }
            atomicStoreLe(
                &header->heap_last, offset, __ATOMIC_RELEASE);
            atomicStoreLe(&header->heap_top, end, __ATOMIC_RELEASE);
        }
        if (mutation_active) mutation_allocations.push_back(offset);
        return offset;
    }

    void release(
        std::uint64_t offset,
        CommonApplyStep publication_step =
            CommonApplyStep::BlobFlagsStored) {
        if (offset == 0) return;
        if (mutation_active) {
            const auto tracked = std::find(
                mutation_allocations.begin(), mutation_allocations.end(), offset);
            if (tracked != mutation_allocations.end()) {
                mutation_allocations.erase(tracked);
            }
        }
        auto* block = blob(offset);
        atomicStoreCommonByte(
            &block->flags,
            std::uint8_t{0},
            __ATOMIC_RELEASE,
            publication_step);
        emitRegionTestEvent(RegionTestEvent::BlobMarkedFree);
        storeCommon(
            &block->payload_size, std::uint32_t{0},
            CommonApplyStep::BlobPayloadSizeStored);
        storeCommon(
            &block->mark, std::uint8_t{0},
            CommonApplyStep::BlobMarkStored);
        insertFree(offset);
    }

    struct QueuePosition {
        std::uint64_t index = 0;
        bool found = false;
        bool available = false;
    };

    struct QueueClearEntry {
        std::uint64_t slot_index = 0;
        std::uint64_t key_offset = 0;
        std::vector<std::uint64_t> message_offsets;
    };

    struct CommonClearPlan {
        std::uint64_t old_heap_top = 0;
        std::vector<QueueClearEntry> queues;
    };

    struct CommonRecoveryPlan {
        std::uint32_t journal_state = kAllocatorJournalIdle;
        std::uint64_t committed_top = 0;
        std::uint64_t repaired_last = 0;
        bool repair_pending_tail = false;
        std::optional<std::uint64_t> legacy_zero_head_slot;
        std::vector<std::uint64_t> cleanup_slots;
        std::vector<std::uint64_t> referenced_blobs;
        std::uint64_t queue_count = 0;
    };

    BlobHeader* queueBlob(std::uint64_t offset) const {
        auto* block = blob(offset);
        if (block->flags != kBlobUsed) {
            throw ErrCorruptRegion("queue references a non-generic Blob");
        }
        return block;
    }

    std::string_view queueBlobString(std::uint64_t offset) const {
        auto* block = queueBlob(offset);
        return {
            reinterpret_cast<const char*>(block) + sizeof(BlobHeader),
            block->payload_size};
    }

    static void requireCanonicalQueueValue(
        const std::uint8_t* data,
        std::size_t size,
        bool persistent) {
        try {
            const auto decoded = XValue::Decode(data, size);
            const auto encoded = decoded.Encode();
            const bool known = decoded.IsNull() ||
                XValue::IsKnownKind(decoded.Kind());
            const bool identical = encoded.size() == size &&
                (size == 0 || std::equal(
                    encoded.begin(), encoded.end(), data));
            if (!known || !identical) {
                throw ErrInvalidValue("noncanonical notification TLV");
            }
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& error) {
            if (persistent) {
                throw ErrCorruptRegion(
                    std::string("invalid retained notification: ") +
                    error.what());
            }
            throw ErrInvalidValue(error.what());
        }
    }

    CommonClearPlan prepareCommonClear(
        std::uint64_t staged_allocation = 0) const {
        validateStaticHeader();
        validateMutableHeader(false);
        if (atomicLoadLe(
                &header->allocator_journal.state,
                __ATOMIC_ACQUIRE) != kAllocatorJournalIdle ||
            header->allocator_journal.reserved32 != 0 ||
            !allocatorJournalReservedZero(header->allocator_journal)) {
            throw ErrCorruptRegion("allocator journal is not idle");
        }

        CommonClearPlan plan;
        plan.old_heap_top = header->heap_top;
        if (header->queue_count > header->queue_limit) {
            throw ErrCorruptRegion("queue count exceeds configured limit");
        }
        plan.queues.reserve(static_cast<std::size_t>(header->queue_count));

        std::unordered_set<std::uint64_t> allocated;
        std::unordered_set<std::uint64_t> free_spans;
        const auto span_count_bound = static_cast<std::size_t>(
            (header->heap_top - header->heap_offset) / kMinimumBlockSize);
        allocated.reserve(span_count_bound);
        free_spans.reserve(span_count_bound);

        std::uint64_t previous_span = 0;
        std::uint64_t derived_last = 0;
        for (auto offset = header->heap_offset;
             offset < header->heap_top;) {
            const auto* block = blob(offset, false);
            if (block->previous_span != previous_span) {
                throw ErrCorruptRegion("invalid Blob boundary tag");
            }
            if (block->flags == kBlobUsed) {
                const auto minimum = blobSpanForPayload(block->payload_size);
                if (block->next_free != 0 || block->previous_free != 0 ||
                    block->mark != 0 || block->span < minimum ||
                    block->span - minimum >= kMinimumBlockSize ||
                    !allocated.insert(offset).second) {
                    throw ErrCorruptRegion("noncanonical allocated Blob");
                }
            } else {
                if (block->payload_size != 0 || block->mark != 0 ||
                    !free_spans.insert(offset).second) {
                    throw ErrCorruptRegion("noncanonical free Blob");
                }
            }
            previous_span = block->span;
            derived_last = offset;
            offset = checkedAdd(offset, block->span);
        }
        if ((header->heap_top == header->heap_offset &&
             header->heap_last != 0) ||
            (header->heap_top != header->heap_offset &&
             header->heap_last != derived_last)) {
            throw ErrCorruptRegion("Blob chain tail mismatch");
        }

        std::unordered_set<std::uint64_t> listed_free;
        listed_free.reserve(free_spans.size());
        for (std::size_t bucket = 0; bucket < kFreeListCount; ++bucket) {
            std::uint64_t previous = 0;
            for (auto current = header->free_heads[bucket];
                 current != 0;) {
                const auto* block = blob(current, false);
                if (block->flags != 0 ||
                    floorLog2(block->span) != bucket ||
                    block->previous_free != previous ||
                    free_spans.count(current) == 0 ||
                    !listed_free.insert(current).second) {
                    throw ErrCorruptRegion("invalid common free list");
                }
                if (block->next_free != 0) {
                    const auto* next = blob(block->next_free, false);
                    if (next->previous_free != current) {
                        throw ErrCorruptRegion(
                            "nonreciprocal common free-list link");
                    }
                }
                previous = current;
                current = block->next_free;
            }
        }
        if (listed_free != free_spans) {
            throw ErrCorruptRegion("common free-list membership mismatch");
        }

        std::unordered_set<std::uint64_t> references;
        references.reserve(allocated.size());
        std::unordered_set<std::string> keys;
        keys.reserve(static_cast<std::size_t>(header->queue_count));
        std::uint64_t derived_queue_count = 0;
        auto* slots = queueTable();
        for (std::uint64_t index = 0;
             index < header->queue_capacity;
             ++index) {
            const auto& slot = slots[index];
            const auto state = atomicLoadLe(
                &slot.state, __ATOMIC_ACQUIRE);
            if (state == kStateEmpty || state == kStateTombstone) {
                if (slot.hash != 0 || slot.key_offset != 0 ||
                    slot.head_offset != 0 || slot.key_length != 0) {
                    throw ErrCorruptRegion(
                        "noncanonical empty notification slot");
                }
                continue;
            }
            if (state != kStateOccupied) {
                throw ErrCorruptRegion("invalid queue slot state");
            }
            if (slot.key_offset == 0 || slot.head_offset == 0 ||
                !references.insert(slot.key_offset).second) {
                throw ErrCorruptRegion("invalid queue key reference");
            }
            const auto* key_blob = queueBlob(slot.key_offset);
            if (key_blob->payload_size != slot.key_length) {
                throw ErrCorruptRegion("queue key length mismatch");
            }
            const auto key = queueBlobString(slot.key_offset);
            if (slot.hash != fnv1a(key) ||
                !keys.emplace(key).second) {
                throw ErrCorruptRegion("invalid or duplicate queue key");
            }

            const auto home = slot.hash & (header->queue_capacity - 1U);
            bool reachable = false;
            for (std::uint64_t probe = 0;
                 probe < header->queue_capacity;
                 ++probe) {
                const auto visited =
                    (home + probe) & (header->queue_capacity - 1U);
                if (visited == index) {
                    reachable = true;
                    break;
                }
                if (atomicLoadLe(
                        &slots[visited].state,
                        __ATOMIC_ACQUIRE) == kStateEmpty) {
                    break;
                }
            }
            if (!reachable) {
                throw ErrCorruptRegion("unreachable queue slot");
            }

            QueueClearEntry queue_plan;
            queue_plan.slot_index = index;
            queue_plan.key_offset = slot.key_offset;
            for (auto current = slot.head_offset; current != 0;) {
                if (!references.insert(current).second) {
                    throw ErrCorruptRegion(
                        "queue message cycle or sharing");
                }
                const auto* block = queueBlob(current);
                if (block->payload_size < sizeof(MessageHeader)) {
                    throw ErrCorruptRegion("truncated queue message");
                }
                const auto* message =
                    reinterpret_cast<const std::uint8_t*>(block) +
                    sizeof(BlobHeader);
                const auto next = loadLe64(message);
                const auto value_length = loadLe32(message + 8);
                const auto magic = loadLe32(message + 12);
                if (magic != kMessageMagic ||
                    value_length !=
                        block->payload_size - sizeof(MessageHeader)) {
                    throw ErrCorruptRegion("invalid queue message");
                }
                requireCanonicalQueueValue(
                    message + sizeof(MessageHeader),
                    value_length,
                    true);
                queue_plan.message_offsets.push_back(current);
                current = next;
            }
            plan.queues.push_back(std::move(queue_plan));
            ++derived_queue_count;
            if (derived_queue_count > header->queue_limit) {
                throw ErrCorruptRegion(
                    "queue count exceeds configured limit");
            }
        }
        if (derived_queue_count != header->queue_count) {
            throw ErrCorruptRegion("queue count mismatch");
        }
        const bool staged_is_valid = staged_allocation == 0 ||
            (allocated.count(staged_allocation) == 1 &&
             references.count(staged_allocation) == 0);
        const auto expected_allocated = references.size() +
            (staged_allocation == 0 ? 0U : 1U);
        if (!staged_is_valid || allocated.size() != expected_allocated) {
            throw ErrCorruptRegion(
                "common heap allocation ownership mismatch");
        }
        return plan;
    }

    void applyCommonClear(const CommonClearPlan& plan) {
        const int rc = ::pthread_cond_broadcast(&header->notify_cond);
        if (rc != 0) {
            throw Error("kvspace: pthread_cond_broadcast failed");
        }
        CommonApplyTraceScope trace(this, CommonApplyPhase::Clear);
        auto* slots = queueTable();
        for (const auto& queued : plan.queues) {
            auto* slot = &slots[queued.slot_index];
            atomicStoreCommon(
                &slot->state,
                kStateTombstone,
                __ATOMIC_RELEASE,
                CommonApplyStep::QueueStateStored);
            emitRegionTestEvent(RegionTestEvent::ClearQueueTombstoned);
            for (const auto message_offset : queued.message_offsets) {
                release(
                    message_offset,
                    CommonApplyStep::QueueMessageReleased);
            }
            release(
                queued.key_offset,
                CommonApplyStep::QueueKeyReleased);
            storeCommon(
                &slot->hash, std::uint64_t{0},
                CommonApplyStep::QueueHashStored);
            storeCommon(
                &slot->key_offset, std::uint64_t{0},
                CommonApplyStep::QueueKeyOffsetStored);
            storeCommon(
                &slot->head_offset, std::uint64_t{0},
                CommonApplyStep::QueueHeadOffsetStored);
            storeCommon(
                &slot->key_length, std::uint32_t{0},
                CommonApplyStep::QueueKeyLengthStored);
        }
        storeCommon(
            &header->queue_count, std::uint64_t{0},
            CommonApplyStep::QueueCountStored);

        if (plan.old_heap_top == header->heap_offset) return;
        for (auto& free_head : header->free_heads) {
            storeCommon(
                &free_head, std::uint64_t{0},
                CommonApplyStep::FreeHeadStored);
        }
        for (auto offset = header->heap_offset;
             offset < plan.old_heap_top;) {
            auto* block = blob(offset, false);
            const auto span = block->span;
            const bool was_allocated = block->flags == kBlobUsed;
            atomicStoreCommonByte(
                &block->flags,
                std::uint8_t{0},
                __ATOMIC_RELEASE,
                CommonApplyStep::BlobFlagsStored);
            if (was_allocated) {
                emitRegionTestEvent(RegionTestEvent::BlobMarkedFree);
            }
            storeCommon(
                &block->next_free, std::uint64_t{0},
                CommonApplyStep::BlobNextFreeStored);
            storeCommon(
                &block->previous_free, std::uint64_t{0},
                CommonApplyStep::BlobPreviousFreeStored);
            storeCommon(
                &block->payload_size, std::uint32_t{0},
                CommonApplyStep::BlobPayloadSizeStored);
            storeCommon(
                &block->mark, std::uint8_t{0},
                CommonApplyStep::BlobMarkStored);
            storeCommon(
                &block->magic, kBlobMagic,
                CommonApplyStep::BlobMagicStored);
            storeCommon(
                &block->reserved16, std::uint16_t{0},
                CommonApplyStep::BlobReserved16Stored);
            storeCommon(
                &block->reserved32, std::uint32_t{0},
                CommonApplyStep::BlobReserved32Stored);
            offset += span;
        }
        for (;;) {
            auto* first = blob(header->heap_offset, false);
            const auto right_offset = header->heap_offset + first->span;
            if (right_offset == plan.old_heap_top) break;
            auto* right = blob(right_offset, false);
            coalesceFreeBlocks(
                header->heap_offset, first->span, right->span);
        }
        auto* only = blob(header->heap_offset, false);
        storeCommon(
            &only->next_free, std::uint64_t{0},
            CommonApplyStep::BlobNextFreeStored);
        storeCommon(
            &only->previous_free, std::uint64_t{0},
            CommonApplyStep::BlobPreviousFreeStored);
        storeCommon(
            &only->payload_size, std::uint32_t{0},
            CommonApplyStep::BlobPayloadSizeStored);
        storeCommon(
            &only->flags, std::uint8_t{0},
            CommonApplyStep::BlobFlagsStored);
        storeCommon(
            &only->mark, std::uint8_t{0},
            CommonApplyStep::BlobMarkStored);
        storeCommon(
            &only->magic, kBlobMagic,
            CommonApplyStep::BlobMagicStored);
        storeCommon(
            &only->reserved16, std::uint16_t{0},
            CommonApplyStep::BlobReserved16Stored);
        storeCommon(
            &only->reserved32, std::uint32_t{0},
            CommonApplyStep::BlobReserved32Stored);
        for (auto& free_head : header->free_heads) {
            storeCommon(
                &free_head, std::uint64_t{0},
                CommonApplyStep::FreeHeadStored);
        }
        atomicStoreCommon(
            &header->heap_top,
            static_cast<std::uint64_t>(header->heap_offset),
            __ATOMIC_RELEASE,
            CommonApplyStep::HeapTopStored);
        emitRegionTestEvent(RegionTestEvent::ClearHeapTopReset);
        atomicStoreCommon(
            &header->heap_last,
            std::uint64_t{0},
            __ATOMIC_RELEASE,
            CommonApplyStep::HeapLastStored);
        emitRegionTestEvent(RegionTestEvent::ClearHeapLastReset);
        storeCommon(
            &header->generation,
            static_cast<std::uint64_t>(header->generation) + 1U,
            CommonApplyStep::GenerationStored);
    }

    CommonRecoveryPlan prepareCommonRecovery() const {
        validateStaticHeader();
        validateMutableHeader(true);
        if (header->allocator_journal.reserved32 != 0 ||
            !allocatorJournalReservedZero(header->allocator_journal)) {
            throw ErrCorruptRegion("nonzero allocator journal reserved word");
        }

        CommonRecoveryPlan plan;
        plan.journal_state = atomicLoadLe(
            &header->allocator_journal.state, __ATOMIC_ACQUIRE);
        if (plan.journal_state > kAllocatorJournalCoalesce) {
            throw ErrCorruptRegion("invalid allocator journal operation");
        }
        const auto heap_begin = header->heap_offset;
        const auto heap_top = atomicLoadLe(
            &header->heap_top, __ATOMIC_ACQUIRE);
        plan.committed_top = heap_top;
        const auto extent = heap_top - heap_begin;
        std::vector<std::uint64_t> virtual_heap(
            static_cast<std::size_t>(extent / sizeof(std::uint64_t)));
        if (extent != 0) {
            std::memcpy(
                virtual_heap.data(),
                base + heap_begin,
                static_cast<std::size_t>(extent));
        }
        const auto virtual_bytes = [&](std::uint64_t offset) {
            if (offset < heap_begin || offset >= heap_top ||
                offset % kAlignment != 0 ||
                sizeof(BlobHeader) > heap_top - offset) {
                throw ErrCorruptRegion("invalid virtual Blob offset");
            }
            return reinterpret_cast<std::uint8_t*>(virtual_heap.data()) +
                (offset - heap_begin);
        };
        const auto load_virtual_blob = [&](std::uint64_t offset) {
            BlobHeader result{};
            std::memcpy(
                &result, virtual_bytes(offset), sizeof(result));
            return result;
        };
        const auto store_virtual_blob = [&] (
            std::uint64_t offset,
            const BlobHeader& block) {
            std::memcpy(
                virtual_bytes(offset), &block, sizeof(block));
        };
        const auto require_structural = [&](std::uint64_t offset) {
            const auto block = load_virtual_blob(offset);
            if (block.magic != kBlobMagic ||
                block.span < kMinimumBlockSize ||
                block.span % kAlignment != 0 ||
                block.span > heap_top - offset ||
                block.reserved16 != 0 || block.reserved32 != 0 ||
                (block.flags != 0 && block.flags != kBlobUsed)) {
                throw ErrCorruptRegion("invalid structural Blob header");
            }
        };
        const auto canonical_unlinked_free = [&](std::uint64_t offset) {
            require_structural(offset);
            const auto block = load_virtual_blob(offset);
            return canonicalGenericFreeFields(block);
        };
        const auto walk_prefix_to = [&](std::uint64_t stop) {
            std::uint64_t previous_span = 0;
            std::uint64_t previous_offset = 0;
            for (auto offset = heap_begin; offset < stop;) {
                require_structural(offset);
                const auto block = load_virtual_blob(offset);
                if (block.previous_span != previous_span ||
                    block.span > stop - offset) {
                    throw ErrCorruptRegion("invalid allocator journal prefix");
                }
                previous_span = block.span;
                previous_offset = offset;
                offset += block.span;
                if (offset > stop) {
                    throw ErrCorruptRegion(
                        "allocator journal source is not a boundary");
                }
            }
            return std::pair<std::uint64_t, std::uint64_t>{
                previous_offset, previous_span};
        };

        std::uint64_t virtual_last = header->heap_last;
        if (plan.journal_state == kAllocatorJournalSplit) {
            const auto& journal = header->allocator_journal;
            const auto source = journal.offset;
            const auto original = journal.original_span;
            const auto requested = journal.requested_span;
            if (journal.checksum != allocatorJournalChecksum(
                    source, original, requested) ||
                source < heap_begin || source >= heap_top ||
                source % kAlignment != 0 ||
                requested < kMinimumBlockSize ||
                requested > original ||
                original - requested < kMinimumBlockSize ||
                original % kAlignment != 0 ||
                requested % kAlignment != 0 ||
                original > heap_top - source) {
                throw ErrCorruptRegion("invalid allocator split journal");
            }
            const auto remainder_span = original - requested;
            const auto remainder_offset = source + requested;
            const auto following_offset = source + original;
            const auto prefix = walk_prefix_to(source);
            if (!canonical_unlinked_free(source)) {
                throw ErrCorruptRegion("invalid allocator split source");
            }
            auto source_block = load_virtual_blob(source);
            if ((source == heap_begin && source_block.previous_span != 0) ||
                (source != heap_begin &&
                 (prefix.second == 0 ||
                  source_block.previous_span != prefix.second ||
                  prefix.first + prefix.second != source)) ||
                (source_block.span != original &&
                 source_block.span != requested)) {
                throw ErrCorruptRegion("unanchored allocator split source");
            }

            bool final_boundary = false;
            bool old_boundary = false;
            if (following_offset < heap_top) {
                require_structural(following_offset);
                const auto following = load_virtual_blob(following_offset);
                old_boundary = following.previous_span == original;
                final_boundary = following.previous_span == remainder_span;
                std::uint64_t expected_previous = following.span;
                std::uint64_t tail = following_offset;
                for (auto offset = following_offset + following.span;
                     offset < heap_top;) {
                    require_structural(offset);
                    const auto block = load_virtual_blob(offset);
                    if (block.previous_span != expected_previous) {
                        throw ErrCorruptRegion(
                            "invalid allocator split suffix");
                    }
                    expected_previous = block.span;
                    tail = offset;
                    offset += block.span;
                }
                if (tail != header->heap_last) {
                    throw ErrCorruptRegion("allocator split tail mismatch");
                }
            } else if (following_offset == heap_top) {
                old_boundary = header->heap_last == source;
                final_boundary = header->heap_last == remainder_offset;
            } else {
                throw ErrCorruptRegion("allocator split exceeds heap top");
            }
            bool exact_remainder = false;
            if (remainder_offset < heap_top) {
                const auto remainder = load_virtual_blob(remainder_offset);
                exact_remainder = canonicalGenericFreeFields(remainder) &&
                    remainder.span == remainder_span &&
                    remainder.previous_span == requested;
            }
            const bool accepted =
                (source_block.span == original && old_boundary) ||
                (source_block.span == original && final_boundary &&
                 exact_remainder) ||
                (source_block.span == requested && final_boundary &&
                 exact_remainder);
            if (!accepted || (old_boundary && final_boundary)) {
                throw ErrCorruptRegion("invalid allocator split stage");
            }
            BlobHeader remainder{};
            remainder.span = remainder_span;
            remainder.previous_span = requested;
            remainder.magic = kBlobMagic;
            store_virtual_blob(remainder_offset, remainder);
            if (following_offset < heap_top) {
                auto following = load_virtual_blob(following_offset);
                following.previous_span = remainder_span;
                store_virtual_blob(following_offset, following);
            } else {
                virtual_last = remainder_offset;
            }
            source_block.span = requested;
            store_virtual_blob(source, source_block);
        } else if (plan.journal_state == kAllocatorJournalCoalesce) {
            const auto& journal = header->allocator_journal;
            const auto source = journal.offset;
            const auto left_span = journal.original_span;
            const auto right_span = journal.requested_span;
            if (journal.checksum != allocatorCoalesceJournalChecksum(
                    source, left_span, right_span) ||
                source < heap_begin || source >= heap_top ||
                source % kAlignment != 0 ||
                left_span < kMinimumBlockSize ||
                right_span < kMinimumBlockSize ||
                left_span % kAlignment != 0 ||
                right_span % kAlignment != 0 ||
                left_span > heap_top - source ||
                right_span > heap_top - source - left_span) {
                throw ErrCorruptRegion("invalid allocator coalesce journal");
            }
            const auto right_offset = source + left_span;
            const auto following_offset = right_offset + right_span;
            const auto combined = checkedAdd(left_span, right_span);
            const auto prefix = walk_prefix_to(source);
            if (!canonical_unlinked_free(source) ||
                !canonical_unlinked_free(right_offset)) {
                throw ErrCorruptRegion("invalid allocator coalesce operands");
            }
            auto left = load_virtual_blob(source);
            const auto right = load_virtual_blob(right_offset);
            if ((left.span != left_span && left.span != combined) ||
                right.span != right_span ||
                right.previous_span != left_span ||
                (source == heap_begin && left.previous_span != 0) ||
                (source != heap_begin &&
                 (prefix.second == 0 || left.previous_span != prefix.second ||
                  prefix.first + prefix.second != source))) {
                throw ErrCorruptRegion("invalid allocator coalesce anchor");
            }
            bool old_boundary = false;
            bool final_boundary = false;
            if (following_offset < heap_top) {
                require_structural(following_offset);
                const auto following = load_virtual_blob(following_offset);
                old_boundary = following.previous_span == right_span;
                final_boundary = following.previous_span == combined;
                std::uint64_t expected_previous = following.span;
                std::uint64_t tail = following_offset;
                for (auto offset = following_offset + following.span;
                     offset < heap_top;) {
                    require_structural(offset);
                    const auto block = load_virtual_blob(offset);
                    if (block.previous_span != expected_previous) {
                        throw ErrCorruptRegion(
                            "invalid allocator coalesce suffix");
                    }
                    expected_previous = block.span;
                    tail = offset;
                    offset += block.span;
                }
                if (tail != header->heap_last) {
                    throw ErrCorruptRegion("allocator coalesce tail mismatch");
                }
            } else if (following_offset == heap_top) {
                old_boundary = header->heap_last == right_offset;
                final_boundary = header->heap_last == source;
            } else {
                throw ErrCorruptRegion("allocator coalesce exceeds heap top");
            }
            const bool accepted =
                (left.span == left_span && old_boundary) ||
                (left.span == combined && old_boundary) ||
                (left.span == combined && final_boundary);
            if (!accepted || (old_boundary && final_boundary)) {
                throw ErrCorruptRegion("invalid allocator coalesce stage");
            }
            left.span = combined;
            store_virtual_blob(source, left);
            if (following_offset < heap_top) {
                auto following = load_virtual_blob(following_offset);
                following.previous_span = combined;
                store_virtual_blob(following_offset, following);
            } else {
                virtual_last = source;
            }
        }

        std::unordered_set<std::uint64_t> boundaries;
        std::unordered_set<std::uint64_t> allocated;
        const auto maximum_spans = static_cast<std::size_t>(
            extent / kMinimumBlockSize);
        boundaries.reserve(maximum_spans);
        allocated.reserve(maximum_spans);
        std::uint64_t previous_span = 0;
        std::uint64_t derived_last = 0;
        for (auto offset = heap_begin; offset < heap_top;) {
            require_structural(offset);
            const auto block = load_virtual_blob(offset);
            if (block.previous_span != previous_span ||
                !boundaries.insert(offset).second) {
                throw ErrCorruptRegion("invalid recovered Blob chain");
            }
            if (block.flags == kBlobUsed) allocated.insert(offset);
            previous_span = block.span;
            derived_last = offset;
            offset += block.span;
        }
        if (plan.journal_state != kAllocatorJournalIdle &&
            ((heap_top == heap_begin && virtual_last != 0) ||
             (heap_top != heap_begin && virtual_last != derived_last))) {
            throw ErrCorruptRegion("recovered Blob tail mismatch");
        }
        plan.repaired_last = derived_last;

        if (plan.journal_state == kAllocatorJournalIdle &&
            header->heap_last != derived_last) {
            if (header->heap_last != heap_top) {
                throw ErrCorruptRegion("invalid pending append tail");
            }
            const auto readable_limit = std::min(
                header->prefix.region_size, header->heap_limit);
            if (sizeof(BlobHeader) > readable_limit - heap_top) {
                throw ErrCorruptRegion("truncated pending append header");
            }
            BlobHeader candidate{};
            std::memcpy(
                &candidate, base + heap_top, sizeof(candidate));
            if (candidate.magic != kBlobMagic ||
                candidate.span < kMinimumBlockSize ||
                candidate.span % kAlignment != 0 ||
                candidate.span > readable_limit - heap_top ||
                candidate.previous_span != previous_span ||
                candidate.reserved16 != 0 || candidate.reserved32 != 0 ||
                candidate.mark != 0 || candidate.next_free != 0 ||
                candidate.previous_free != 0 ||
                (candidate.flags != 0 &&
                 candidate.flags != kBlobUsed) ||
                (candidate.flags == 0 && candidate.payload_size != 0) ||
                (candidate.flags == kBlobUsed &&
                 (candidate.payload_size >
                      candidate.span - sizeof(BlobHeader) ||
                  candidate.span !=
                      blobSpanForPayload(candidate.payload_size)))) {
                throw ErrCorruptRegion("invalid pending append evidence");
            }
            plan.repair_pending_tail = true;
        }

        const auto virtual_payload = [&](std::uint64_t offset) {
            return virtual_bytes(offset) + sizeof(BlobHeader);
        };
        const auto require_allocated = [&](std::uint64_t offset) {
            if (boundaries.count(offset) == 0 ||
                allocated.count(offset) == 0) {
                throw ErrCorruptRegion("queue reference is not allocated");
            }
            const auto block = load_virtual_blob(offset);
            const auto minimum = blobSpanForPayload(block.payload_size);
            if (block.next_free != 0 || block.previous_free != 0 ||
                block.span < minimum ||
                block.span - minimum >= kMinimumBlockSize) {
                throw ErrCorruptRegion("noncanonical referenced Blob");
            }
        };

        std::unordered_set<std::uint64_t> claimed;
        claimed.reserve(allocated.size());
        std::unordered_set<std::uint64_t> referenced;
        referenced.reserve(allocated.size());
        std::unordered_set<std::string> keys;
        keys.reserve(static_cast<std::size_t>(header->queue_limit));
        auto* queues = queueTable();
        std::uint64_t occupied_count = 0;
        std::uint64_t legacy_count = 0;
        for (std::uint64_t index = 0;
             index < header->queue_capacity;
             ++index) {
            const auto& queue = queues[index];
            const auto state = atomicLoadLe(
                &queue.state, __ATOMIC_ACQUIRE);
            if (state == kStateEmpty || state == kStateTombstone) {
                plan.cleanup_slots.push_back(index);
                continue;
            }
            if (state != kStateOccupied || queue.key_offset == 0) {
                throw ErrCorruptRegion("invalid queue slot during recovery");
            }
            require_allocated(queue.key_offset);
            const auto key_block = load_virtual_blob(queue.key_offset);
            if (key_block.payload_size != queue.key_length) {
                throw ErrCorruptRegion("queue key length mismatch");
            }
            const std::string_view key(
                reinterpret_cast<const char*>(
                    virtual_payload(queue.key_offset)),
                queue.key_length);
            if (queue.hash != fnv1a(key) || !keys.emplace(key).second) {
                throw ErrCorruptRegion("invalid or duplicate queue key");
            }
            if (!claimed.insert(queue.key_offset).second) {
                throw ErrCorruptRegion("shared queue key Blob");
            }
            const auto home = queue.hash & (header->queue_capacity - 1U);
            bool reachable = false;
            for (std::uint64_t probe = 0;
                 probe < header->queue_capacity;
                 ++probe) {
                const auto visited =
                    (home + probe) & (header->queue_capacity - 1U);
                if (visited == index) {
                    reachable = true;
                    break;
                }
                if (atomicLoadLe(
                        &queues[visited].state,
                        __ATOMIC_ACQUIRE) == kStateEmpty) {
                    break;
                }
            }
            if (!reachable) throw ErrCorruptRegion("unreachable queue slot");

            if (queue.head_offset == 0) {
                ++legacy_count;
                if (legacy_count != 1) {
                    throw ErrCorruptRegion(
                        "multiple legacy zero-head queues");
                }
                plan.legacy_zero_head_slot = index;
                plan.cleanup_slots.push_back(index);
                continue;
            }
            referenced.insert(queue.key_offset);
            for (auto current = queue.head_offset; current != 0;) {
                require_allocated(current);
                if (!claimed.insert(current).second) {
                    throw ErrCorruptRegion("queue message cycle or sharing");
                }
                referenced.insert(current);
                const auto block = load_virtual_blob(current);
                if (block.payload_size < sizeof(MessageHeader)) {
                    throw ErrCorruptRegion("truncated queue message");
                }
                const auto* bytes = virtual_payload(current);
                const auto next = loadLe64(bytes);
                const auto value_length = loadLe32(bytes + 8);
                const auto magic = loadLe32(bytes + 12);
                if (magic != kMessageMagic ||
                    value_length !=
                        block.payload_size - sizeof(MessageHeader)) {
                    throw ErrCorruptRegion("invalid queue message");
                }
                requireCanonicalQueueValue(
                    bytes + sizeof(MessageHeader), value_length, true);
                current = next;
            }
            ++occupied_count;
        }
        if (occupied_count + legacy_count > header->queue_limit) {
            throw ErrCorruptRegion("queue count exceeds configured limit");
        }
        plan.queue_count = occupied_count;
        plan.referenced_blobs.assign(referenced.begin(), referenced.end());
        std::sort(
            plan.referenced_blobs.begin(), plan.referenced_blobs.end());
        return plan;
    }

    void applyCommonRecovery(
        const CommonRecoveryPlan& plan,
        bool count_recovery = true) {
        CommonApplyTraceScope trace(this, CommonApplyPhase::Recovery);
        if (plan.journal_state != kAllocatorJournalIdle) {
            completeAllocatorJournal(true);
        }
        if (plan.repair_pending_tail) {
            atomicStoreCommon(
                &header->heap_last,
                plan.repaired_last,
                __ATOMIC_RELEASE,
                CommonApplyStep::HeapLastStored);
            emitRegionTestEvent(RegionTestEvent::RecoveryPendingTailReset);
        }
        auto* queues = queueTable();
        if (plan.legacy_zero_head_slot.has_value()) {
            atomicStoreCommon(
                &queues[*plan.legacy_zero_head_slot].state,
                kStateTombstone,
                __ATOMIC_RELEASE,
                CommonApplyStep::QueueStateStored);
        }
        for (const auto index : plan.cleanup_slots) {
            auto& queue = queues[index];
            storeCommon(
                &queue.hash, std::uint64_t{0},
                CommonApplyStep::QueueHashStored);
            storeCommon(
                &queue.key_offset, std::uint64_t{0},
                CommonApplyStep::QueueKeyOffsetStored);
            storeCommon(
                &queue.head_offset, std::uint64_t{0},
                CommonApplyStep::QueueHeadOffsetStored);
            storeCommon(
                &queue.key_length, std::uint32_t{0},
                CommonApplyStep::QueueKeyLengthStored);
        }
        for (auto& free_head : header->free_heads) {
            storeCommon(
                &free_head, std::uint64_t{0},
                CommonApplyStep::FreeHeadStored);
        }
        for (auto offset = header->heap_offset;
             offset < header->heap_top;) {
            auto* block = recoveryBlob(offset);
            const auto span = block->span;
            const bool live = std::binary_search(
                plan.referenced_blobs.begin(),
                plan.referenced_blobs.end(),
                offset);
            if (live) {
                storeCommon(
                    &block->next_free, std::uint64_t{0},
                    CommonApplyStep::BlobNextFreeStored);
                storeCommon(
                    &block->previous_free, std::uint64_t{0},
                    CommonApplyStep::BlobPreviousFreeStored);
                storeCommon(
                    &block->mark, std::uint8_t{0},
                    CommonApplyStep::BlobMarkStored);
            } else {
                const bool was_allocated = block->flags == kBlobUsed;
                atomicStoreCommonByte(
                    &block->flags,
                    std::uint8_t{0},
                    __ATOMIC_RELEASE,
                    CommonApplyStep::BlobFlagsStored);
                if (was_allocated) {
                    emitRegionTestEvent(RegionTestEvent::BlobMarkedFree);
                }
                storeCommon(
                    &block->next_free, std::uint64_t{0},
                    CommonApplyStep::BlobNextFreeStored);
                storeCommon(
                    &block->previous_free, std::uint64_t{0},
                    CommonApplyStep::BlobPreviousFreeStored);
                storeCommon(
                    &block->payload_size, std::uint32_t{0},
                    CommonApplyStep::BlobPayloadSizeStored);
                storeCommon(
                    &block->mark, std::uint8_t{0},
                    CommonApplyStep::BlobMarkStored);
                storeCommon(
                    &block->magic, kBlobMagic,
                    CommonApplyStep::BlobMagicStored);
                storeCommon(
                    &block->reserved16, std::uint16_t{0},
                    CommonApplyStep::BlobReserved16Stored);
                storeCommon(
                    &block->reserved32, std::uint32_t{0},
                    CommonApplyStep::BlobReserved32Stored);
            }
            offset += span;
        }
        for (auto offset = header->heap_offset;
             offset < header->heap_top;) {
            auto* block = blob(offset, false);
            const auto next_offset = offset + block->span;
            if (block->flags == 0 && next_offset < header->heap_top) {
                auto* next = blob(next_offset, false);
                if (next->flags == 0) {
                    coalesceFreeBlocks(offset, block->span, next->span);
                    continue;
                }
            }
            offset += block->span;
        }
        for (auto offset = header->heap_offset;
             offset < header->heap_top;) {
            auto* block = blob(offset, false);
            if (block->flags == 0) insertFree(offset);
            offset += block->span;
        }
        storeCommon(
            &header->queue_count,
            plan.queue_count,
            CommonApplyStep::QueueCountStored);
        if (count_recovery) {
            storeCommon(
                &header->recovery_count,
                static_cast<std::uint64_t>(header->recovery_count) + 1U,
                CommonApplyStep::RecoveryCountStored);
        }
        storeCommon(
            &header->generation,
            static_cast<std::uint64_t>(header->generation) + 1U,
            CommonApplyStep::GenerationStored);
    }

    QueuePosition findQueue(std::string_view key, std::uint64_t hash) const {
        auto* slots = queueTable();
        std::uint64_t first_tombstone = header->queue_capacity;
        for (std::uint64_t probe = 0; probe < header->queue_capacity; ++probe) {
            const auto index = (hash + probe) & (header->queue_capacity - 1);
            const auto state = atomicLoadLe(
                &slots[index].state, __ATOMIC_ACQUIRE);
            if (state == kStateEmpty) {
                return {first_tombstone == header->queue_capacity ? index : first_tombstone,
                        false, true};
            }
            if (state == kStateTombstone) {
                if (first_tombstone == header->queue_capacity) first_tombstone = index;
                continue;
            }
            if (state != kStateOccupied) throw ErrCorruptRegion("invalid queue slot state");
            if (slots[index].hash == hash && slots[index].key_length == key.size() &&
                queueBlobString(slots[index].key_offset) == key) {
                return {index, true, true};
            }
        }
        if (first_tombstone != header->queue_capacity) {
            return {first_tombstone, false, true};
        }
        return {};
    }

    void queuePush(std::string_view key, const std::vector<std::uint8_t>& value) {
        if (key.size() > std::numeric_limits<std::uint32_t>::max() ||
            value.size() >
                std::numeric_limits<std::uint32_t>::max() -
                    sizeof(MessageHeader)) {
            throw ErrCapacity("notification key or value exceeds 4 GiB");
        }
        requireCanonicalQueueValue(value.data(), value.size(), false);
        static_cast<void>(prepareCommonClear());
        const auto hash = fnv1a(key);
        const auto position = findQueue(key, hash);
        if (!position.found &&
            (!position.available ||
             header->queue_count >= header->queue_limit)) {
            throw ErrCapacity("maximum notification queue count reached");
        }
        const auto size = sizeof(MessageHeader) + value.size();
        std::vector<std::uint8_t> encoded_message(size);
        const std::uint64_t old_head = position.found
            ? static_cast<std::uint64_t>(
                  queueTable()[position.index].head_offset)
            : std::uint64_t{0};
        storeLe64(encoded_message.data(), old_head);
        storeLe32(
            encoded_message.data() + 8,
            static_cast<std::uint32_t>(value.size()));
        storeLe32(encoded_message.data() + 12, kMessageMagic);
        if (!value.empty()) {
            std::memcpy(
                encoded_message.data() + sizeof(MessageHeader),
                value.data(),
                value.size());
        }
        std::uint64_t key_offset = 0;
        if (!position.found) {
            key_offset = allocateCopy(key.data(), key.size());
        }
        auto* slot = &queueTable()[position.index];
        std::uint64_t offset = 0;
        try {
            if (key_offset != 0) {
                static_cast<void>(prepareCommonClear(key_offset));
            }
            offset = allocateCopy(encoded_message.data(), encoded_message.size());
        } catch (...) {
            if (key_offset != 0) release(key_offset);
            throw;
        }
        // Wake before publishing while still holding the region mutex. A waiter
        // cannot inspect the queue until we unlock; if this process dies after
        // the wake, the waiter acquires the robust mutex and runs recovery. This
        // removes the publish-then-die-before-wake lost-notification window.
        const int rc = ::pthread_cond_broadcast(&header->notify_cond);
        if (rc != 0) {
            release(offset);
            if (key_offset != 0) release(key_offset);
            throw Error("kvspace: pthread_cond_broadcast failed");
        }
        if (position.found) {
            atomicStoreLe(&slot->head_offset, offset, __ATOMIC_RELEASE);
        } else {
            slot->hash = hash;
            slot->key_offset = key_offset;
            slot->head_offset = offset;
            slot->key_length = static_cast<std::uint32_t>(key.size());
            atomicStoreLe(&slot->state, kStateOccupied, __ATOMIC_RELEASE);
            ++header->queue_count;
        }
    }

    bool queuePop(std::string_view key, std::vector<std::uint8_t>* value) {
        static_cast<void>(prepareCommonClear());
        const auto position = findQueue(key, fnv1a(key));
        if (!position.found) return false;
        auto* slot = &queueTable()[position.index];
        const auto offset = atomicLoadLe(
            &slot->head_offset, __ATOMIC_ACQUIRE);
        if (offset == 0) return false;
        auto* block = queueBlob(offset);
        if (block->payload_size < sizeof(MessageHeader)) {
            throw ErrCorruptRegion("truncated queue message");
        }
        const auto* message = reinterpret_cast<const std::uint8_t*>(block) +
            sizeof(BlobHeader);
        const auto next = loadLe64(message);
        const auto value_length = loadLe32(message + 8);
        const auto magic = loadLe32(message + 12);
        if (magic != kMessageMagic ||
            value_length != block->payload_size - sizeof(MessageHeader)) {
            throw ErrCorruptRegion("invalid queue message");
        }
        const auto* first = message + sizeof(MessageHeader);
        requireCanonicalQueueValue(
            first, value_length, true);
        value->assign(first, first + value_length);
        if (next != 0) {
            atomicStoreLe(
                &slot->head_offset, next, __ATOMIC_RELEASE);
            release(offset);
            return true;
        }

        // Final pop uses the slot state as its sole publication.  In
        // particular, no new writer state contains OCCUPIED with head zero.
        const auto key_offset = slot->key_offset;
        atomicStoreLe(
            &slot->state, kStateTombstone, __ATOMIC_RELEASE);
        --header->queue_count;
        release(offset);
        release(key_offset);
        slot->hash = 0;
        slot->key_offset = 0;
        slot->head_offset = 0;
        slot->key_length = 0;
        return true;
    }

    [[noreturn]] static void failStopRecovery() noexcept {
        std::_Exit(125);
    }

    void poisonOwnerDeadMutex(bool* mutex_owned) {
        if (::pthread_mutex_unlock(&header->mutex) != 0) {
            failStopRecovery();
        }
        if (mutex_owned != nullptr) *mutex_owned = false;
    }

    void recoverOwnerDeadMutex(bool* mutex_owned = nullptr) {
        std::unique_ptr<ShmPreparedRecovery> prepared;
        try {
            if (engine == nullptr) {
                throw ErrCorruptRegion("missing storage engine");
            }
            emitRegionTestEvent(RegionTestEvent::RecoveryPrepareStarting);
            prepared = engine->PrepareRecovery();
            if (prepared == nullptr) {
                throw Error("kvspace: missing prepared recovery");
            }
        } catch (const ErrCorruptRegion&) {
            // Unlocking an EOWNERDEAD robust mutex without first making it
            // consistent permanently poisons it.  No application-owned byte
            // is written on this path; subsequent lock attempts report
            // ENOTRECOVERABLE, which lock()/wait() map back to corruption.
            poisonOwnerDeadMutex(mutex_owned);
            throw;
        } catch (const ErrVersionMismatch&) {
            poisonOwnerDeadMutex(mutex_owned);
            throw;
        } catch (const ErrUnsupportedEngine&) {
            poisonOwnerDeadMutex(mutex_owned);
            throw;
        } catch (...) {
            // Allocation failure and other transient preparation failures must
            // not return while this process owns an inconsistent mutex. A
            // process exit leaves it owner-dead so another process may retry.
            failStopRecovery();
        }

        try {
            emitRegionTestEvent(RegionTestEvent::RecoveryApplyStarting);
            std::move(*prepared).Apply();
        } catch (...) {
            // Once Apply begins, persistent recovery may be partially written.
            // Exiting while holding the mutex is the only safe retry boundary.
            failStopRecovery();
        }

        if (::pthread_mutex_consistent(&header->mutex) != 0) {
            failStopRecovery();
        }
        emitRegionTestEvent(RegionTestEvent::RecoveryMadeConsistent);
        if (::pthread_cond_broadcast(&header->notify_cond) != 0) {
            // Recovery is already complete and the mutex is consistent. Do
            // not manufacture a second owner-death cycle for a wake failure.
            if (::pthread_mutex_unlock(&header->mutex) != 0) {
                failStopRecovery();
            }
            if (mutex_owned != nullptr) *mutex_owned = false;
            throw Error("kvspace: recovery broadcast failed");
        }
    }

    void lock() {
        if (closed) throw ErrDisconnected();
        const int rc = ::pthread_mutex_lock(&header->mutex);
        if (rc == 0) {
            if (header->corrupt != 0) {
                ::pthread_mutex_unlock(&header->mutex);
                throw ErrCorruptRegion("region marked corrupt");
            }
            return;
        }
        if (rc == EOWNERDEAD) {
            recoverOwnerDeadMutex();
            return;
        }
        if (rc == ENOTRECOVERABLE) throw ErrCorruptRegion("mutex is not recoverable");
        throw Error("kvspace: pthread_mutex_lock failed");
    }

    void unlock() noexcept {
        if (!closed && header != nullptr) ::pthread_mutex_unlock(&header->mutex);
    }

    int wait(const timespec* deadline, bool* mutex_owned) {
        if (mutex_owned == nullptr) {
            throw Error("kvspace: missing wait mutex state");
        }
        *mutex_owned = true;
        const int rc = deadline == nullptr
            ? ::pthread_cond_wait(&header->notify_cond, &header->mutex)
            : ::pthread_cond_timedwait(&header->notify_cond, &header->mutex, deadline);
        if (rc == EOWNERDEAD) {
            recoverOwnerDeadMutex(mutex_owned);
            return 0;
        }
        if (rc == ENOTRECOVERABLE) {
            *mutex_owned = false;
            throw ErrCorruptRegion("mutex is not recoverable");
        }
        return rc;
    }
};

class HashBoxEngine final : public ShmEngineStore {
public:
    explicit HashBoxEngine(Region::Impl* owner)
        : owner_(owner),
          geometry_(readHashBoxGeometry(owner->base, owner->header)) {}

    ShmEngine Id() const noexcept override { return ShmEngine::HashBox; }
    void Validate() const override {
        auto* self = const_cast<HashBoxEngine*>(this);
        self->attachNormal();
        const auto state = self->inspect(self->committedTableIndex(), true);
        self->requireHeaderState(state);
    }
    bool Get(
        std::string_view key,
        std::vector<std::uint8_t>* value) const override {
        requireAttached();
        const auto position = findIn(
            visibleTableIndex(), key, fnv1a(key));
        if (!position.found) return false;
        if (value != nullptr) {
            *value = readValue(
                loadSlot(slotBytes(visibleTableIndex(), position.index))
                    .value_ref);
        }
        return true;
    }
    bool Exists(std::string_view key) const override {
        return Get(key, nullptr);
    }
    void Put(
        std::string_view key,
        const std::vector<std::uint8_t>& value) override {
        const bool implicit = !owner_->mutation_active;
        if (implicit) BeginMutation();
        try {
            putInMutation(key, value);
            if (implicit) CommitMutation();
        } catch (...) {
            RollbackMutation();
            throw;
        }
    }
    bool Erase(std::string_view key) override {
        const bool implicit = !owner_->mutation_active;
        if (implicit) BeginMutation();
        try {
            const auto erased = eraseInMutation(key);
            if (implicit) CommitMutation();
            return erased;
        } catch (...) {
            RollbackMutation();
            throw;
        }
    }
    std::vector<EngineEntry> Entries() const override {
        return entriesFrom(visibleTableIndex(), {}, false);
    }
    std::vector<EngineEntry> EntriesWithPrefix(
        std::string_view prefix) const override {
        return entriesFrom(visibleTableIndex(), prefix, true);
    }
    std::unique_ptr<ShmPreparedClear> PrepareClear() override {
        if (owner_->mutation_active) {
            throw Error("kvspace: cannot clear during a mutation");
        }
        requireAttached();
        try {
            const auto source = committedTableIndex();
            const auto current = inspect(source, true);
            requireHeaderState(current);
            auto recovery_box = BoxAllocator::AttachForRecovery(
                owner_->base + geometry_.box_metadata_offset,
                static_cast<std::size_t>(geometry_.box_metadata_bytes),
                geometry_.box_data_bytes);
            const std::vector<BoxLiveInterval> empty;
            auto box_plan = std::move(recovery_box).PrepareRebuild(empty);
            const auto target = static_cast<std::uint32_t>(1U - source);
            return MakeShmPreparedClear(
                [this,
                 source,
                 target,
                 box_plan = std::move(box_plan)]() mutable {
                    zeroTable(target);
                    owner_->header->root_offset = 0;
                    owner_->header->node_count = 0;
                    owner_->header->entry_count = 0;
                    owner_->header->tombstone_count = 0;
                    owner_->header->engine_live_bytes = 0;
                    ++owner_->header->generation;
                    atomicStoreLe(
                        &owner_->header->active_table,
                        target,
                        __ATOMIC_RELEASE);
                    emitRegionTestEvent(RegionTestEvent::MutationPublished);
                    *box_ = std::move(box_plan).Apply();
                    emitRegionTestEvent(RegionTestEvent::HashBoxBoxRebuilt);
                    zeroTable(source);
                    resetMutationState();
                    owner_->mutation_allocations.clear();
                });
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
    }
    void Compact() override {}

    void BeginMutation() override {
        if (owner_->mutation_active) {
            throw Error("kvspace: nested mutation");
        }
        requireAttached();
        const auto source_index = committedTableIndex();
        const auto source = inspect(source_index, true);
        requireHeaderState(source);
        staged_table_ = static_cast<std::uint32_t>(1U - source_index);
        staged_entries_ = source.entries;
        staged_materialized_ = false;
        new_boxes_.clear();
        owner_->mutation_allocations.clear();
        owner_->mutation_active = true;
    }

    void CommitMutation() override {
        if (!owner_->mutation_active) {
            throw Error("kvspace: no active mutation");
        }
        requireAttached();
        if (!staged_materialized_) {
            owner_->mutation_active = false;
            resetMutationState();
            owner_->mutation_allocations.clear();
            return;
        }
        const auto old_index = committedTableIndex();
        const auto old_state = inspect(old_index, true);
        requireHeaderState(old_state);
        const auto final_state = inspect(staged_table_, true);
        if (final_state.entries != staged_entries_) {
            throw ErrCorruptRegion("staged HashBox entry count mismatch");
        }

        owner_->header->root_offset = 0;
        owner_->header->node_count = 0;
        owner_->header->entry_count = final_state.entries;
        owner_->header->tombstone_count = final_state.tombstones;
        owner_->header->engine_live_bytes = final_state.live_bytes;
        ++owner_->header->generation;
        atomicStoreLe(
            &owner_->header->active_table,
            staged_table_,
            __ATOMIC_RELEASE);
        emitRegionTestEvent(RegionTestEvent::MutationPublished);
        owner_->mutation_active = false;

        try {
            reclaimAfterPublish(old_state, final_state);
            zeroTable(old_index);
            resetMutationState();
            owner_->mutation_allocations.clear();
        } catch (...) {
            owner_->mutation_allocations.clear();
            try {
                Recover();
            } catch (...) {
                owner_->header->corrupt = 1;
                throw;
            }
        }
    }

    void RollbackMutation() noexcept override {
        if (!owner_->mutation_active) return;
        owner_->mutation_active = false;
        try {
            if (staged_materialized_) {
                for (auto cursor = new_boxes_.rbegin();
                     cursor != new_boxes_.rend();
                     ++cursor) {
                    box_->Free(cursor->offset);
                }
                zeroTable(staged_table_);
            }
        } catch (...) {
            try {
                Recover();
            } catch (...) {
                owner_->header->corrupt = 1;
            }
        }
        resetMutationState();
        owner_->mutation_allocations.clear();
    }

    std::unique_ptr<ShmPreparedRecovery> PrepareRecovery() override {
        owner_->mutation_active = false;
        owner_->mutation_allocations.clear();
        resetMutationState();
        box_.reset();
        try {
            auto common_plan = owner_->prepareCommonRecovery();
            const auto selected = committedTableIndex();
            box_ = std::make_unique<BoxAllocator>(
                BoxAllocator::AttachForRecovery(
                    owner_->base + geometry_.box_metadata_offset,
                    static_cast<std::size_t>(geometry_.box_metadata_bytes),
                    geometry_.box_data_bytes));
            const auto preflight = inspect(selected, false);
            auto box_plan = std::move(*box_).PrepareRebuild(
                preflight.intervals);
            return MakeShmPreparedRecovery(
                [this,
                 selected,
                 preflight = std::move(preflight),
                 box_plan = std::move(box_plan),
                 common_plan = std::move(common_plan)]() mutable {
                    try {
                        *box_ = std::move(box_plan).Apply();
                        emitRegionTestEvent(
                            RegionTestEvent::HashBoxBoxRebuilt);
                        owner_->header->root_offset = 0;
                        owner_->header->node_count = 0;
                        owner_->header->entry_count = preflight.entries;
                        owner_->header->tombstone_count =
                            preflight.tombstones;
                        owner_->header->engine_live_bytes =
                            preflight.live_bytes;
                        zeroTable(
                            static_cast<std::uint32_t>(1U - selected));
                        owner_->applyCommonRecovery(common_plan);
                    } catch (const AllocatorError& error) {
                        throw ErrCorruptRegion(error.what());
                    }
                });
        } catch (const AllocatorError& error) {
            throw ErrCorruptRegion(error.what());
        }
    }

private:
    struct SlotRecord {
        std::uint64_t hash = 0;
        std::uint64_t key_ref = 0;
        std::uint64_t value_ref = 0;
        std::uint32_t key_len = 0;
        std::uint32_t state = 0;
    };

    struct Position {
        std::uint64_t index = 0;
        bool found = false;
        bool available = false;
    };

    struct BoxAllocation {
        std::uint64_t offset = 0;
        std::uint64_t logical_size = 0;
    };

    struct Inspection {
        std::uint64_t entries = 0;
        std::uint64_t tombstones = 0;
        std::uint64_t live_bytes = 0;
        std::vector<BoxLiveInterval> intervals;
        std::unordered_set<std::uint64_t> box_offsets;
    };

    [[noreturn]] static void raiseAllocator(const AllocatorError& error) {
        if (error.Code() == AllocatorErrorCode::Capacity) {
            throw ErrCapacity(error.what());
        }
        throw ErrCorruptRegion(error.what());
    }

    void requireAttached() const {
        if (box_ == nullptr) {
            throw ErrCorruptRegion("HashBox allocator is not attached");
        }
    }

    void attachNormal() {
        try {
            box_ = std::make_unique<BoxAllocator>(
                BoxAllocator::Attach(
                    owner_->base + geometry_.box_metadata_offset,
                    static_cast<std::size_t>(geometry_.box_metadata_bytes),
                    geometry_.box_data_bytes));
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
    }

    std::uint32_t committedTableIndex() const {
        const auto selected = atomicLoadLe(
            &owner_->header->active_table, __ATOMIC_ACQUIRE);
        if (selected > 1) {
            throw ErrCorruptRegion("invalid HashBox active table");
        }
        return selected;
    }

    std::uint32_t visibleTableIndex() const {
        return owner_->mutation_active && staged_materialized_
            ? staged_table_
            : committedTableIndex();
    }

    void materializeStagedTable() {
        if (staged_materialized_) return;
        const auto source_index = committedTableIndex();
        if (staged_table_ != 1U - source_index) {
            throw ErrCorruptRegion("invalid HashBox staged selector");
        }
        zeroTable(staged_table_);
        staged_materialized_ = true;
        for (std::uint64_t index = 0;
             index < owner_->header->table_capacity;
             ++index) {
            const auto slot = loadSlot(slotBytes(source_index, index));
            if (slot.state != kStateOccupied) continue;
            const auto key = keyAt(slot);
            const auto position = findIn(staged_table_, key, slot.hash);
            if (!position.available || position.found) {
                throw ErrCorruptRegion("cannot stage HashBox table image");
            }
            storeOccupied(slotBytes(staged_table_, position.index), slot);
        }
    }

    std::uint8_t* tableBytes(std::uint32_t index) const {
        if (index > 1) {
            throw ErrCorruptRegion("invalid HashBox table image");
        }
        return owner_->base + owner_->header->table_offset[index];
    }

    std::uint8_t* slotBytes(
        std::uint32_t table_index,
        std::uint64_t slot_index) const {
        if (slot_index >= owner_->header->table_capacity) {
            throw ErrCorruptRegion("HashBox slot index is out of bounds");
        }
        return tableBytes(table_index) +
            static_cast<std::size_t>(slot_index * kHashBoxSlotBytes);
    }

    static SlotRecord loadSlot(const std::uint8_t* bytes) noexcept {
        return SlotRecord{
            loadLe64(bytes),
            loadLe64(bytes + 8),
            loadLe64(bytes + 16),
            loadLe32(bytes + 24),
            loadLe32(bytes + 28)};
    }

    static void storeOccupied(
        std::uint8_t* bytes,
        const SlotRecord& slot) noexcept {
        std::memset(bytes, 0, static_cast<std::size_t>(kHashBoxSlotBytes));
        storeLe64(bytes, slot.hash);
        storeLe64(bytes + 8, slot.key_ref);
        storeLe64(bytes + 16, slot.value_ref);
        storeLe32(bytes + 24, slot.key_len);
        storeLe32(bytes + 28, kStateOccupied);
    }

    static void storeTombstone(std::uint8_t* bytes) noexcept {
        std::memset(bytes, 0, static_cast<std::size_t>(kHashBoxSlotBytes));
        storeLe32(bytes + 28, kStateTombstone);
    }

    void zeroTable(std::uint32_t index) const noexcept {
        std::memset(
            tableBytes(index),
            0,
            static_cast<std::size_t>(
                owner_->header->table_capacity * kHashBoxSlotBytes));
    }

    const std::uint8_t* boxData(std::uint64_t offset) const {
        if (offset >= geometry_.box_data_bytes) {
            throw ErrCorruptRegion("HashBox object offset is out of bounds");
        }
        return owner_->base + geometry_.box_data_offset + offset;
    }

    std::string_view keyAt(const SlotRecord& slot) const {
        if (slot.key_ref == 0 || slot.key_len == 0) {
            throw ErrCorruptRegion("invalid empty HashBox key reference");
        }
        const auto offset = slot.key_ref - 1U;
        if (offset >= geometry_.box_data_bytes ||
            slot.key_len > geometry_.box_data_bytes - offset) {
            throw ErrCorruptRegion("HashBox key exceeds the Box data zone");
        }
        return {
            reinterpret_cast<const char*>(boxData(offset)),
            static_cast<std::size_t>(slot.key_len)};
    }

    std::size_t valueLengthAt(std::uint64_t offset) const {
        const auto* data = boxData(offset);
        const auto available = geometry_.box_data_bytes - offset;
        if (available < 9) {
            throw ErrCorruptRegion("truncated HashBox TLV header");
        }
        const auto kind_length = static_cast<std::uint64_t>(data[0]);
        const auto fixed = checkedAdd(checkedAdd(1, kind_length), 8);
        if (kind_length == 0 || fixed > available) {
            throw ErrCorruptRegion("invalid HashBox TLV kind length");
        }
        const auto raw_length = static_cast<std::uint64_t>(
            loadLe32(data + 1U + kind_length + 4U));
        const auto total = checkedAdd(fixed, raw_length);
        if (total > available ||
            total > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            throw ErrCorruptRegion("HashBox TLV exceeds the Box data zone");
        }
        try {
            const auto size = static_cast<std::size_t>(total);
            const auto decoded = XValue::Decode(data, size);
            if (decoded.Encode() !=
                std::vector<std::uint8_t>(data, data + size)) {
                throw ErrCorruptRegion("noncanonical HashBox TLV");
            }
        } catch (const ErrCorruptRegion&) {
            throw;
        } catch (const Error& error) {
            throw ErrCorruptRegion(error.what());
        }
        return static_cast<std::size_t>(total);
    }

    std::vector<std::uint8_t> readValue(std::uint64_t reference) const {
        if (reference == 0) return {};
        requireAttached();
        const auto offset = reference - 1U;
        const auto size = valueLengthAt(offset);
        try {
            if (box_->AllocatedSize(offset) != BoxAllocator::RoundSize(size)) {
                throw ErrCorruptRegion(
                    "HashBox value allocation size mismatch");
            }
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
        const auto* data = boxData(offset);
        return {data, data + size};
    }

    std::uint64_t allocateBytes(const void* data, std::size_t size) {
        if (data == nullptr || size == 0) {
            throw ErrCorruptRegion("invalid empty HashBox allocation");
        }
        requireAttached();
        new_boxes_.reserve(new_boxes_.size() + 1U);
        try {
            const auto offset = box_->Allocate(size);
            if (offset == UINT64_MAX) {
                box_->Free(offset);
                throw ErrCapacity("HashBox reference bias overflows");
            }
            std::memcpy(
                owner_->base + geometry_.box_data_offset + offset,
                data,
                size);
            new_boxes_.push_back(BoxAllocation{offset, size});
            return offset + 1U;
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
    }

    std::uint64_t allocateValue(
        const std::vector<std::uint8_t>& value) {
        if (value.empty()) return 0;
        const auto decoded = XValue::Decode(value);
        if (decoded.Encode() != value) {
            throw ErrInvalidValue("noncanonical encoded value");
        }
        return allocateBytes(value.data(), value.size());
    }

    Position findIn(
        std::uint32_t table_index,
        std::string_view key,
        std::uint64_t hash) const {
        const auto capacity = owner_->header->table_capacity;
        std::uint64_t first_tombstone = capacity;
        const auto home = hash & (capacity - 1U);
        for (std::uint64_t probe = 0; probe < capacity; ++probe) {
            const auto index = (home + probe) & (capacity - 1U);
            const auto slot = loadSlot(slotBytes(table_index, index));
            if (slot.state == kStateEmpty) {
                return {
                    first_tombstone == capacity ? index : first_tombstone,
                    false,
                    true};
            }
            if (slot.state == kStateTombstone) {
                if (first_tombstone == capacity) first_tombstone = index;
                continue;
            }
            if (slot.state != kStateOccupied) {
                throw ErrCorruptRegion("invalid HashBox slot state");
            }
            if (slot.hash == hash && slot.key_len == key.size() &&
                keyAt(slot) == key) {
                return {index, true, true};
            }
        }
        if (first_tombstone != capacity) {
            return {first_tombstone, false, true};
        }
        return {};
    }

    Inspection inspect(
        std::uint32_t table_index,
        bool require_box) const {
        Inspection state;
        std::unordered_set<std::string> keys;
        const auto capacity = owner_->header->table_capacity;
        state.intervals.reserve(static_cast<std::size_t>(
            std::min(owner_->header->entry_limit * 2U, capacity * 2U)));
        const auto add_interval = [&](std::uint64_t offset,
                                      std::uint64_t logical_size) {
            if (!state.box_offsets.insert(offset).second) {
                throw ErrCorruptRegion(
                    "HashBox objects share a Box allocation");
            }
            std::uint64_t rounded = 0;
            try {
                rounded = BoxAllocator::RoundSize(logical_size);
                if (offset >= geometry_.box_data_bytes ||
                    rounded > geometry_.box_data_bytes - offset) {
                    throw ErrCorruptRegion(
                        "HashBox Box interval exceeds the data zone");
                }
                if (require_box &&
                    box_->AllocatedSize(offset) != rounded) {
                    throw ErrCorruptRegion(
                        "HashBox Box allocation size mismatch");
                }
            } catch (const AllocatorError& error) {
                raiseAllocator(error);
            }
            state.intervals.push_back(BoxLiveInterval{
                offset, logical_size});
            state.live_bytes = checkedAdd(state.live_bytes, rounded);
        };

        for (std::uint64_t index = 0; index < capacity; ++index) {
            const auto* raw = slotBytes(table_index, index);
            const auto slot = loadSlot(raw);
            if (slot.state == kStateEmpty) {
                if (!std::all_of(
                        raw,
                        raw + kHashBoxSlotBytes,
                        [](std::uint8_t byte) { return byte == 0; })) {
                    throw ErrCorruptRegion(
                        "noncanonical empty HashBox slot");
                }
                continue;
            }
            if (slot.state == kStateTombstone) {
                if (!std::all_of(
                        raw,
                        raw + 28,
                        [](std::uint8_t byte) { return byte == 0; })) {
                    throw ErrCorruptRegion(
                        "noncanonical HashBox tombstone");
                }
                ++state.tombstones;
                continue;
            }
            if (slot.state != kStateOccupied) {
                throw ErrCorruptRegion("invalid HashBox slot state");
            }
            const auto key = keyAt(slot);
            if (fnv1a(key) != slot.hash ||
                !keys.emplace(key).second) {
                throw ErrCorruptRegion(
                    "invalid or duplicate HashBox key");
            }
            add_interval(slot.key_ref - 1U, slot.key_len);
            if (slot.value_ref != 0) {
                const auto offset = slot.value_ref - 1U;
                add_interval(offset, valueLengthAt(offset));
            }
            ++state.entries;
            if (state.entries > owner_->header->entry_limit) {
                throw ErrCorruptRegion(
                    "HashBox entries exceed the configured limit");
            }
        }

        for (std::uint64_t index = 0; index < capacity; ++index) {
            const auto slot = loadSlot(slotBytes(table_index, index));
            if (slot.state != kStateOccupied) continue;
            const auto home = slot.hash & (capacity - 1U);
            bool reachable = false;
            for (std::uint64_t probe = 0; probe < capacity; ++probe) {
                const auto visited = (home + probe) & (capacity - 1U);
                if (visited == index) {
                    reachable = true;
                    break;
                }
                if (loadSlot(slotBytes(table_index, visited)).state ==
                    kStateEmpty) {
                    break;
                }
            }
            if (!reachable) {
                throw ErrCorruptRegion("unreachable HashBox slot");
            }
        }

        std::sort(
            state.intervals.begin(),
            state.intervals.end(),
            [](const BoxLiveInterval& left,
               const BoxLiveInterval& right) {
                return left.offset < right.offset;
            });
        std::uint64_t previous_end = 0;
        bool have_previous = false;
        for (const auto& interval : state.intervals) {
            std::uint64_t rounded = 0;
            try {
                rounded = BoxAllocator::RoundSize(interval.size);
            } catch (const AllocatorError& error) {
                raiseAllocator(error);
            }
            if (have_previous && interval.offset < previous_end) {
                throw ErrCorruptRegion("HashBox Box intervals overlap");
            }
            previous_end = checkedAdd(interval.offset, rounded);
            have_previous = true;
        }
        return state;
    }

    void requireHeaderState(const Inspection& state) const {
        if (state.entries != owner_->header->entry_count ||
            state.tombstones != owner_->header->tombstone_count ||
            state.live_bytes != owner_->header->engine_live_bytes ||
            owner_->header->root_offset != 0 ||
            owner_->header->node_count != 0) {
            throw ErrCorruptRegion(
                "HashBox counters do not match the selected image");
        }
    }

    void putInMutation(
        std::string_view key,
        const std::vector<std::uint8_t>& value) {
        requireAttached();
        if (key.empty()) {
            throw ErrInvalidPath("HashBox keys must be nonempty");
        }
        if (key.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw ErrCapacity("HashBox key exceeds 4 GiB");
        }
        if (!value.empty()) {
            const auto decoded = XValue::Decode(value);
            if (decoded.Encode() != value) {
                throw ErrInvalidValue("noncanonical encoded value");
            }
        }
        const auto hash = fnv1a(key);
        const auto before = findIn(visibleTableIndex(), key, hash);
        if (staged_entries_ > owner_->header->entry_limit) {
            throw ErrCorruptRegion("HashBox staged count exceeds limit");
        }
        const auto projected = before.found
            ? staged_entries_
            : checkedAdd(staged_entries_, 1U);
        if (projected > owner_->header->entry_limit) {
            throw ErrCapacity("maximum key count reached");
        }
        if (!before.found && !before.available) {
            throw ErrCapacity("HashBox table is full");
        }
        new_boxes_.reserve(
            new_boxes_.size() + (before.found ? 1U : 2U));
        materializeStagedTable();
        const auto position = findIn(staged_table_, key, hash);
        if (position.found) {
            auto slot = loadSlot(slotBytes(staged_table_, position.index));
            slot.value_ref = allocateValue(value);
            storeOccupied(slotBytes(staged_table_, position.index), slot);
            staged_entries_ = projected;
            pruneBatchCreated();
            return;
        }
        if (!position.available) {
            throw ErrCorruptRegion("HashBox staged lookup diverged");
        }
        const auto key_ref = allocateBytes(key.data(), key.size());
        const auto value_ref = allocateValue(value);
        storeOccupied(
            slotBytes(staged_table_, position.index),
            SlotRecord{
                hash,
                key_ref,
                value_ref,
                static_cast<std::uint32_t>(key.size()),
                kStateOccupied});
        staged_entries_ = projected;
        pruneBatchCreated();
    }

    bool eraseInMutation(std::string_view key) {
        requireAttached();
        const auto hash = fnv1a(key);
        const auto before = findIn(visibleTableIndex(), key, hash);
        if (staged_entries_ > owner_->header->entry_limit) {
            throw ErrCorruptRegion("HashBox staged count exceeds limit");
        }
        if (!before.found) return false;
        if (staged_entries_ == 0) {
            throw ErrCorruptRegion("HashBox staged count underflows");
        }
        const auto projected = staged_entries_ - 1U;
        materializeStagedTable();
        const auto position = findIn(staged_table_, key, hash);
        if (!position.found) {
            throw ErrCorruptRegion("HashBox staged erase lookup diverged");
        }
        storeTombstone(slotBytes(staged_table_, position.index));
        staged_entries_ = projected;
        pruneBatchCreated();
        return true;
    }

    std::vector<EngineEntry> entriesFrom(
        std::uint32_t table_index,
        std::string_view prefix,
        bool filter) const {
        requireAttached();
        std::vector<EngineEntry> result;
        result.reserve(static_cast<std::size_t>(
            owner_->mutation_active
                ? staged_entries_
                : owner_->header->entry_count));
        for (std::uint64_t index = 0;
             index < owner_->header->table_capacity;
             ++index) {
            const auto slot = loadSlot(slotBytes(table_index, index));
            if (slot.state != kStateOccupied) continue;
            const auto key = keyAt(slot);
            if (filter &&
                (key.size() < prefix.size() ||
                 key.substr(0, prefix.size()) != prefix)) {
                continue;
            }
            result.emplace_back(std::string(key), readValue(slot.value_ref));
        }
        return result;
    }

    void pruneBatchCreated() {
        const auto committed = inspect(committedTableIndex(), true);
        const auto staged = inspect(staged_table_, true);
        for (std::size_t index = 0; index < new_boxes_.size();) {
            const auto offset = new_boxes_[index].offset;
            if (committed.box_offsets.count(offset) != 0 ||
                staged.box_offsets.count(offset) != 0) {
                ++index;
                continue;
            }
            try {
                box_->Free(offset);
            } catch (const AllocatorError& error) {
                raiseAllocator(error);
            }
            new_boxes_[index] = new_boxes_.back();
            new_boxes_.pop_back();
        }
    }

    void reclaimAfterPublish(
        const Inspection& old_state,
        const Inspection& final_state) {
        for (const auto& interval : old_state.intervals) {
            if (final_state.box_offsets.count(interval.offset) != 0) continue;
            box_->Free(interval.offset);
            emitRegionTestEvent(RegionTestEvent::HashBoxObjectReclaimed);
        }
        for (const auto& allocation : new_boxes_) {
            if (final_state.box_offsets.count(allocation.offset) != 0) continue;
            box_->Free(allocation.offset);
            emitRegionTestEvent(RegionTestEvent::HashBoxObjectReclaimed);
        }
    }

    void resetMutationState() noexcept {
        staged_table_ = 0;
        staged_entries_ = 0;
        staged_materialized_ = false;
        new_boxes_.clear();
    }

    Region::Impl* owner_;
    HashBoxGeometry geometry_;
    mutable std::unique_ptr<BoxAllocator> box_;
    std::uint32_t staged_table_ = 0;
    std::uint64_t staged_entries_ = 0;
    bool staged_materialized_ = false;
    std::vector<BoxAllocation> new_boxes_;
};

class ArtBumpEngineV4 final : public ShmEngineStore,
                              private ArtBumpIndexState {
public:
    explicit ArtBumpEngineV4(Region::Impl* owner)
        : owner_(owner), geometry_(expectedGeometry(owner)) {}

    ShmEngine Id() const noexcept override { return ShmEngine::ArtBump; }

    std::uint64_t PhysicalUsedBytes() const {
        requireAttached();
        std::uint64_t result = 0;
        for (std::size_t index = 0;
             index < ArtBumpNodeCodec::kPayloadBytes.size();
             ++index) {
            const auto kind = static_cast<ArtBumpNodeKind>(index + 1U);
            result = checkedAdd(
                result,
                checkedMultiply(
                    nodes_->UsedCount(kind),
                    ArtBumpNodeCodec::kPayloadBytes[index]));
        }
        for (const auto& zone : raw_zones_) {
            result = checkedAdd(
                result,
                zone->TopAcquire() - zone->Begin());
        }
        return result;
    }

    void Validate() const override {
        auto* self = const_cast<ArtBumpEngineV4*>(this);
        try {
            self->attach(false);
            static_cast<void>(self->index_->InspectCommitted());
        } catch (const AllocatorError& error) {
            raisePersistent(error);
        }
    }

    bool Get(
        std::string_view key,
        std::vector<std::uint8_t>* value) const override {
        requireAttached();
        try {
            const auto found = mutation_.has_value()
                ? mutation_->Get(key)
                : index_->Get(key);
            if (!found.has_value()) return false;
            if (value != nullptr) *value = *found;
            return true;
        } catch (const AllocatorError& error) {
            raiseOperation(error);
        }
    }

    bool Exists(std::string_view key) const override {
        return Get(key, nullptr);
    }

    void Put(
        std::string_view key,
        const std::vector<std::uint8_t>& value) override {
        const bool implicit = !owner_->mutation_active;
        if (implicit) BeginMutation();
        try {
            static_cast<void>(mutation_->Put(key, value));
            if (implicit) CommitMutation();
        } catch (const AllocatorError& error) {
            RollbackMutation();
            raisePut(error);
        } catch (...) {
            RollbackMutation();
            throw;
        }
    }

    bool Erase(std::string_view key) override {
        const bool implicit = !owner_->mutation_active;
        if (implicit) BeginMutation();
        try {
            const bool erased = mutation_->Erase(key);
            if (implicit) CommitMutation();
            return erased;
        } catch (const AllocatorError& error) {
            RollbackMutation();
            raiseOperation(error);
        } catch (...) {
            RollbackMutation();
            throw;
        }
    }

    std::vector<EngineEntry> Entries() const override {
        requireAttached();
        try {
            return convertEntries(
                mutation_.has_value()
                    ? mutation_->Entries()
                    : index_->Entries());
        } catch (const AllocatorError& error) {
            raiseOperation(error);
        }
    }

    std::vector<EngineEntry> EntriesWithPrefix(
        std::string_view prefix) const override {
        requireAttached();
        try {
            return convertEntries(
                mutation_.has_value()
                    ? mutation_->EntriesWithPrefix(prefix)
                    : index_->EntriesWithPrefix(prefix));
        } catch (const AllocatorError& error) {
            raiseOperation(error);
        }
    }

    std::unique_ptr<ShmPreparedClear> PrepareClear() override {
        if (owner_->mutation_active) {
            throw Error("kvspace: cannot clear during a mutation");
        }
        requireAttached();
        try {
            auto plan = index_->PrepareClear();
            return MakeShmPreparedClear(
                [plan = std::move(plan)]() mutable {
                    std::move(plan).Apply();
                });
        } catch (const AllocatorError& error) {
            raiseOperation(error);
        }
    }

    void Compact() override {
        if (owner_->mutation_active) {
            throw Error("kvspace: cannot compact during a mutation");
        }
        requireAttached();
        try {
            auto plan = index_->PrepareCompact();
            std::move(plan).Apply();
        } catch (const AllocatorError& error) {
            raiseOperation(error);
        }
    }

    void BeginMutation() override {
        if (owner_->mutation_active || mutation_.has_value()) {
            throw Error("kvspace: nested mutation");
        }
        requireAttached();
        try {
            mutation_.emplace(index_->BeginMutation());
            owner_->mutation_active = true;
        } catch (const AllocatorError& error) {
            raiseOperation(error);
        }
    }

    void CommitMutation() override {
        if (!owner_->mutation_active || !mutation_.has_value()) {
            throw Error("kvspace: no active mutation");
        }
        try {
            auto plan = std::move(*mutation_).PrepareCommit();
            mutation_.reset();
            std::move(plan).Apply();
            owner_->mutation_active = false;
        } catch (const AllocatorError& error) {
            RollbackMutation();
            raiseOperation(error);
        } catch (...) {
            RollbackMutation();
            throw;
        }
    }

    void RollbackMutation() noexcept override {
        if (mutation_.has_value()) mutation_->Abort();
        mutation_.reset();
        owner_->mutation_active = false;
        owner_->mutation_allocations.clear();
    }

    std::unique_ptr<ShmPreparedRecovery> PrepareRecovery() override {
        if (mutation_.has_value()) {
            mutation_->AbandonForRecovery();
            mutation_.reset();
        }
        owner_->mutation_active = false;
        owner_->mutation_allocations.clear();
        try {
            auto common_plan = owner_->prepareCommonRecovery();
            attach(true);
            auto engine_plan = index_->PrepareRecovery();
            return MakeShmPreparedRecovery(
                [this,
                 common_plan = std::move(common_plan),
                 engine_plan = std::move(engine_plan)]() mutable {
                    try {
                        owner_->applyCommonRecovery(common_plan);
                        std::move(engine_plan).Apply();
                        recovery_access_ = false;
                    } catch (const AllocatorError& error) {
                        throw ErrCorruptRegion(error.what());
                    }
                });
        } catch (const AllocatorError& error) {
            raisePersistent(error);
        }
    }

private:
    static ArtBumpGeometry expectedGeometry(Region::Impl* owner) {
        try {
            return ArtBumpGeometry::Compute(
                owner->mapped_size,
                owner->header->entry_limit,
                owner->header->queue_capacity,
                owner->header->page_size,
                sizeof(RegionHeader),
                sizeof(QueueSlot));
        } catch (const AllocatorError& error) {
            throw ErrCorruptRegion(error.what());
        }
    }

    [[noreturn]] static void raisePersistent(const AllocatorError& error) {
        throw ErrCorruptRegion(error.what());
    }

    [[noreturn]] static void raiseOperation(const AllocatorError& error) {
        if (error.Code() == AllocatorErrorCode::Capacity) {
            throw ErrCapacity(error.what());
        }
        throw ErrCorruptRegion(error.what());
    }

    [[noreturn]] static void raisePut(const AllocatorError& error) {
        if (error.Code() == AllocatorErrorCode::InvalidArgument) {
            throw ErrInvalidValue(error.what());
        }
        raiseOperation(error);
    }

    static std::vector<EngineEntry> convertEntries(
        std::vector<ArtBumpIndex::Entry> entries) {
        std::vector<EngineEntry> result;
        result.reserve(entries.size());
        for (auto& entry : entries) {
            result.emplace_back(
                std::move(entry.key),
                std::move(entry.value));
        }
        return result;
    }

    void requireAttached() const {
        if (!header_.has_value() || !nodes_.has_value() ||
            !raw_zones_[0].has_value() ||
            !raw_zones_[1].has_value() || index_ == nullptr) {
            throw ErrCorruptRegion("ArtBump engine is not attached");
        }
    }

    void requireExpectedHeader(const ArtBumpEngineHeader& stored) const {
        if (stored.geometry_hash != geometry_.geometry_hash ||
            stored.node_capacity != geometry_.node_capacity) {
            throw ErrCorruptRegion("ArtBump engine geometry mismatch");
        }
        for (std::size_t index = 0;
             index < stored.payload_bytes.size();
             ++index) {
            if (stored.payload_bytes[index] !=
                ArtBumpNodeCodec::kPayloadBytes[index]) {
                throw ErrCorruptRegion("ArtBump payload geometry mismatch");
            }
        }
        for (std::size_t index = 0; index < stored.slabs.size(); ++index) {
            const auto& actual = stored.slabs[index];
            const auto& expected = geometry_.slabs[index];
            if (actual.metadata_offset != expected.metadata_offset ||
                actual.metadata_bytes != expected.metadata_bytes ||
                actual.zone_offset != expected.zone_offset ||
                actual.zone_bytes != expected.zone_bytes) {
                throw ErrCorruptRegion("ArtBump slab geometry mismatch");
            }
        }
        for (std::size_t index = 0; index < stored.raw_zones.size(); ++index) {
            if (stored.raw_zones[index].begin !=
                    geometry_.raw_zones[index].begin ||
                stored.raw_zones[index].bytes !=
                    geometry_.raw_zones[index].bytes) {
                throw ErrCorruptRegion("ArtBump raw-zone geometry mismatch");
            }
        }
    }

    void requireZeroPadding() const {
        owner_->requireZeroRange(
            geometry_.engine_offset + ArtBumpGeometry::kEngineHeaderBytes,
            geometry_.slabs.front().metadata_offset,
            "nonzero ArtBump header-to-slab padding");
        for (std::size_t index = 0;
             index + 1U < geometry_.slabs.size();
             ++index) {
            owner_->requireZeroRange(
                geometry_.slabs[index].zone_offset +
                    geometry_.slabs[index].zone_bytes,
                geometry_.slabs[index + 1U].metadata_offset,
                "nonzero ArtBump slab alignment padding");
        }
        owner_->requireZeroRange(
            geometry_.slabs.back().zone_offset +
                geometry_.slabs.back().zone_bytes,
            geometry_.raw_zones.front().begin,
            "nonzero ArtBump slab-to-raw padding");
    }

    void attach(bool recovery) {
        index_.reset();
        nodes_.reset();
        raw_zones_[0].reset();
        raw_zones_[1].reset();
        header_.reset();

        auto* const encoded = owner_->base + geometry_.engine_offset;
        header_.emplace(
            recovery
                ? ArtBumpHeaderView::AttachForRecovery(
                      encoded, ArtBumpHeaderCodec::kHeaderBytes)
                : ArtBumpHeaderView::Attach(
                      encoded, ArtBumpHeaderCodec::kHeaderBytes));
        const auto stored = recovery
            ? header_->DecodeForRecovery()
            : header_->Decode();
        requireExpectedHeader(stored);
        requireZeroPadding();

        nodes_.emplace(
            recovery
                ? ArtBumpNodeStore::AttachForRecovery(
                      owner_->base,
                      static_cast<std::size_t>(owner_->mapped_size),
                      geometry_.slabs,
                      geometry_.node_capacity)
                : ArtBumpNodeStore::Attach(
                      owner_->base,
                      static_cast<std::size_t>(owner_->mapped_size),
                      geometry_.slabs,
                      geometry_.node_capacity));
        for (std::size_t index = 0; index < raw_zones_.size(); ++index) {
            auto* const descriptor = header_->Data() +
                ArtBumpHeaderCodec::kRawZonesOffset +
                index * ArtBumpHeaderCodec::kRawZoneDescriptorBytes;
            const auto& expected = geometry_.raw_zones[index];
            raw_zones_[index].emplace(
                recovery
                    ? ArtBumpRawZone::AttachForRecovery(
                          owner_->base,
                          static_cast<std::size_t>(owner_->mapped_size),
                          descriptor,
                          ArtBumpHeaderCodec::kRawZoneDescriptorBytes,
                          expected.begin,
                          expected.bytes)
                    : ArtBumpRawZone::Attach(
                          owner_->base,
                          static_cast<std::size_t>(owner_->mapped_size),
                          descriptor,
                          ArtBumpHeaderCodec::kRawZoneDescriptorBytes,
                          expected.begin,
                          expected.bytes));
        }
        recovery_access_ = recovery;
        index_ = std::make_unique<ArtBumpIndex>(
            *nodes_,
            std::array<ArtBumpRawZone*, 2>{
                &*raw_zones_[0], &*raw_zones_[1]},
            static_cast<ArtBumpIndexState&>(*this));
    }

    ArtBumpStateSnapshot LoadSnapshotAcquire() const override {
        requireAttached();
        auto stored = recovery_access_
            ? header_->DecodeForRecovery()
            : header_->Decode();
        stored.committed_root = header_->CommittedRootAcquire();
        stored.active_zone = header_->ActiveZone();
        stored.journal = header_->JournalAcquire();
        for (std::size_t index = 0; index < raw_zones_.size(); ++index) {
            stored.raw_zones[index].top = raw_zones_[index]->TopAcquire();
            stored.raw_zones[index].epoch = raw_zones_[index]->Epoch();
        }
        return ArtBumpStateSnapshot{
            stored.committed_root,
            owner_->header->generation,
            owner_->header->entry_limit,
            stored.active_zone,
            stored.raw_zones,
            ArtBumpDerivedState{
                owner_->header->node_count,
                owner_->header->entry_count,
                owner_->header->engine_live_bytes},
            owner_->header->root_offset,
            owner_->header->tombstone_count,
            stored.journal};
    }

    void WriteJournalCopyingPayload(
        const ::kvspace::detail::ArtBumpCompactJournal& journal)
        noexcept override {
        header_->StoreJournalPayload(journal);
    }

    void WriteJournalReadyField(
        const ::kvspace::detail::ArtBumpCompactJournal& journal,
        ArtBumpReadyField field) noexcept override {
        header_->StoreJournalReadyField(journal, field);
    }

    void PublishJournalStateRelease(
        ArtBumpJournalState state) noexcept override {
        header_->StoreJournalStateRelease(state);
    }

    void StoreActiveZone(std::uint32_t zone) noexcept override {
        header_->StoreActiveZone(zone);
    }

    void StoreDerived(const ArtBumpDerivedState& derived) noexcept override {
        owner_->header->root_offset = 0;
        owner_->header->node_count = derived.node_count;
        owner_->header->entry_count = derived.entry_count;
        owner_->header->tombstone_count = 0;
        owner_->header->engine_live_bytes = derived.engine_live_bytes;
    }

    void StoreGeneration(std::uint64_t generation) noexcept override {
        owner_->header->generation = generation;
    }

    void StoreCommittedRootRelease(std::uint64_t root) noexcept override {
        header_->StoreCommittedRootRelease(root);
    }

    void AfterPersistentStep(
        ArtBumpApplyStep step,
        std::uint64_t ordinal) noexcept override {
        emitArtBumpRegionTestEvent(step, ordinal);
    }

    Region::Impl* owner_;
    ArtBumpGeometry geometry_;
    bool recovery_access_ = false;
    std::optional<ArtBumpHeaderView> header_;
    std::optional<ArtBumpNodeStore> nodes_;
    std::array<std::optional<ArtBumpRawZone>, 2> raw_zones_;
    std::unique_ptr<ArtBumpIndex> index_;
    std::optional<ArtBumpIndex::Mutation> mutation_;
};

class ArtBoxEngine final : public ShmEngineStore {
public:
    explicit ArtBoxEngine(Region::Impl* owner);

    ShmEngine Id() const noexcept override;
    void Validate() const override;
    bool Get(
        std::string_view key,
        std::vector<std::uint8_t>* value) const override;
    bool Exists(std::string_view key) const override;
    void Put(
        std::string_view key,
        const std::vector<std::uint8_t>& value) override;
    bool Erase(std::string_view key) override;
    std::vector<EngineEntry> Entries() const override;
    std::vector<EngineEntry> EntriesWithPrefix(
        std::string_view prefix) const override;
    std::unique_ptr<ShmPreparedClear> PrepareClear() override;
    void Compact() override;
    void BeginMutation() override;
    void CommitMutation() override;
    void RollbackMutation() noexcept override;
    std::unique_ptr<ShmPreparedRecovery> PrepareRecovery() override;

private:
    struct BoxAllocation {
        std::uint64_t offset = 0;
    };

    struct NodeData {
        std::uint64_t prefix_ref = 0;
        std::uint64_t value_ref = 0;
        std::uint32_t prefix_length = 0;
        ArtBoxNodeKind kind = ArtBoxNodeKind::Node4;
        bool has_value = false;
        std::vector<std::pair<std::uint8_t, std::uint32_t>> children;
    };

    struct PathFrame {
        std::uint32_t reference = ArtBoxNodeRefCodec::kEmpty;
        std::uint8_t edge = 0;
    };

    struct WalkItem {
        std::uint32_t reference = ArtBoxNodeRefCodec::kEmpty;
        std::string before;
    };

    enum class InspectionMode {
        Normal,
        Recovery,
    };

    struct Inspection {
        std::uint64_t nodes = 0;
        std::uint64_t entries = 0;
        std::uint64_t live_bytes = 0;
        std::vector<std::uint32_t> node_refs;
        std::unordered_set<std::uint32_t> node_set;
        std::vector<BoxLiveInterval> intervals;
        std::unordered_map<std::uint64_t, std::uint64_t> box_sizes;
    };

    [[noreturn]] static void raiseAllocator(const AllocatorError& error);
    static std::size_t kindIndex(ArtBoxNodeKind kind);
    static ArtBoxNodeKind chooseKind(
        ArtBoxNodeKind previous,
        std::size_t children);

    ArtBoxNodeStoreRegions nodeRegions() const;
    void requireAttached() const;
    void attachNormal();
    std::uint32_t committedRoot() const;
    std::uint32_t visibleRoot() const;
    const std::uint8_t* boxData(std::uint64_t offset) const;
    std::size_t valueLengthAt(std::uint64_t offset) const;
    std::vector<std::uint8_t> readValue(std::uint64_t reference) const;
    std::string_view readPrefix(
        std::uint64_t reference,
        std::uint32_t length,
        bool require_allocation) const;
    std::uint64_t allocateBoxBytes(const void* data, std::size_t size);
    std::uint64_t allocateValue(const std::vector<std::uint8_t>& value);
    std::uint64_t allocatePrefix(std::string_view prefix);
    ArtBoxNodeRecord readRecord(
        std::uint32_t reference,
        InspectionMode mode) const;
    NodeData readData(std::uint32_t reference) const;
    std::string_view dataPrefix(const NodeData& data) const;
    static std::uint32_t childAt(
        const ArtBoxNodeRecord& record,
        std::uint8_t edge);
    static std::vector<std::pair<std::uint8_t, std::uint32_t>> childrenOf(
        const ArtBoxNodeRecord& record);
    static void setChild(
        NodeData* data,
        std::uint8_t edge,
        std::uint32_t child);
    void replacePrefix(NodeData* data, std::string_view prefix);
    std::uint32_t allocateNode(const NodeData& data);
    std::uint32_t makeLeaf(
        std::string_view prefix,
        std::uint64_t value_ref);
    std::uint32_t normalizeAndAllocate(NodeData data);
    std::uint32_t rebuildPath(
        const std::vector<PathFrame>& frames,
        std::uint32_t child);
    std::uint32_t findNode(std::string_view key) const;
    std::uint32_t subtreeForPrefix(
        std::string_view wanted,
        std::string* before) const;
    std::vector<EngineEntry> entriesFrom(
        std::uint32_t root,
        std::string before) const;
    Inspection inspect(
        std::uint32_t root,
        InspectionMode mode,
        bool require_box,
        bool require_exact_nodes) const;
    void requireHeaderState(const Inspection& state) const;
    void putInMutation(
        std::string_view key,
        const std::vector<std::uint8_t>& value);
    bool eraseInMutation(std::string_view key);
    void clearInMutation();
    void pruneBatchCreated();
    void reclaimAfterPublish(
        const Inspection& old_state,
        const Inspection& final_state);
    void resetMutationState() noexcept;

    Region::Impl* owner_;
    ArtBoxGeometry geometry_;
    mutable std::unique_ptr<ArtBoxNodeStore> nodes_;
    mutable std::unique_ptr<BoxAllocator> box_;
    std::uint32_t staged_root_ = ArtBoxNodeRefCodec::kEmpty;
    std::uint64_t staged_entries_ = 0;
    std::vector<std::uint32_t> new_nodes_;
    std::vector<BoxAllocation> new_boxes_;
};

ArtBoxEngine::ArtBoxEngine(Region::Impl* owner)
    : owner_(owner),
      geometry_(readArtBoxGeometry(owner->base, owner->header)) {}

ShmEngine ArtBoxEngine::Id() const noexcept { return ShmEngine::ArtBox; }

[[noreturn]] void ArtBoxEngine::raiseAllocator(
    const AllocatorError& error) {
    if (error.Code() == AllocatorErrorCode::Capacity) {
        throw ErrCapacity(error.what());
    }
    throw ErrCorruptRegion(error.what());
}

std::size_t ArtBoxEngine::kindIndex(ArtBoxNodeKind kind) {
    const auto raw = static_cast<std::uint8_t>(kind);
    if (raw < static_cast<std::uint8_t>(ArtBoxNodeKind::Node4) ||
        raw > static_cast<std::uint8_t>(ArtBoxNodeKind::Node256)) {
        throw ErrCorruptRegion("invalid ArtBox node kind");
    }
    return static_cast<std::size_t>(raw - 1U);
}

ArtBoxNodeKind ArtBoxEngine::chooseKind(
    ArtBoxNodeKind previous,
    std::size_t children) {
    if (children > 256) {
        throw ErrCorruptRegion("ArtBox node has too many children");
    }
    if (children <= 4) return ArtBoxNodeKind::Node4;
    if (children <= 16) return ArtBoxNodeKind::Node16;
    if (children <= 48) {
        return previous == ArtBoxNodeKind::Node256 && children > 37
            ? ArtBoxNodeKind::Node256
            : ArtBoxNodeKind::Node48;
    }
    return ArtBoxNodeKind::Node256;
}

ArtBoxNodeStoreRegions ArtBoxEngine::nodeRegions() const {
    ArtBoxNodeStoreRegions regions{};
    for (std::size_t index = 0; index < regions.size(); ++index) {
        regions[index] = ArtBoxNodeStoreRegion{
            owner_->base + geometry_.slabs[index].metadata_offset,
            static_cast<std::size_t>(
                geometry_.slabs[index].metadata_bytes),
            owner_->base + geometry_.slabs[index].zone_offset,
            static_cast<std::size_t>(geometry_.slabs[index].zone_bytes)};
    }
    return regions;
}

void ArtBoxEngine::requireAttached() const {
    if (nodes_ == nullptr || box_ == nullptr) {
        throw ErrCorruptRegion("ArtBox allocators are not attached");
    }
}

void ArtBoxEngine::attachNormal() {
    try {
        nodes_ = std::make_unique<ArtBoxNodeStore>(
            ArtBoxNodeStore::Attach(nodeRegions(), geometry_.node_capacity));
        box_ = std::make_unique<BoxAllocator>(
            BoxAllocator::Attach(
                owner_->base + geometry_.box_metadata_offset,
                static_cast<std::size_t>(geometry_.box_metadata_bytes),
                geometry_.box_data_bytes));
    } catch (const AllocatorError& error) {
        raiseAllocator(error);
    }
}

std::uint32_t ArtBoxEngine::committedRoot() const {
    return atomicLoadLe(
        artBoxRootStorage(owner_->base, owner_->header),
        __ATOMIC_ACQUIRE);
}

std::uint32_t ArtBoxEngine::visibleRoot() const {
    return owner_->mutation_active ? staged_root_ : committedRoot();
}

const std::uint8_t* ArtBoxEngine::boxData(std::uint64_t offset) const {
    if (offset >= geometry_.box_data_bytes) {
        throw ErrCorruptRegion("ArtBox object offset is out of bounds");
    }
    return owner_->base + geometry_.box_data_offset + offset;
}

std::size_t ArtBoxEngine::valueLengthAt(std::uint64_t offset) const {
    const auto* data = boxData(offset);
    const auto available = geometry_.box_data_bytes - offset;
    if (available < 9) {
        throw ErrCorruptRegion("truncated ArtBox TLV header");
    }
    const auto kind_length = static_cast<std::uint64_t>(data[0]);
    const auto fixed = checkedAdd(checkedAdd(1, kind_length), 8);
    if (kind_length == 0 || fixed > available) {
        throw ErrCorruptRegion("invalid ArtBox TLV kind length");
    }
    const auto raw_length_offset = 1U + kind_length + 4U;
    const auto raw_length = static_cast<std::uint64_t>(
        loadLe32(data + raw_length_offset));
    const auto total = checkedAdd(fixed, raw_length);
    if (total > available ||
        total > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max())) {
        throw ErrCorruptRegion("ArtBox TLV exceeds the Box data zone");
    }
    try {
        const auto size = static_cast<std::size_t>(total);
        const auto decoded = XValue::Decode(data, size);
        if (decoded.Encode() != std::vector<std::uint8_t>(data, data + size)) {
            throw ErrCorruptRegion("noncanonical ArtBox TLV");
        }
    } catch (const ErrCorruptRegion&) {
        throw;
    } catch (const Error& error) {
        throw ErrCorruptRegion(error.what());
    }
    return static_cast<std::size_t>(total);
}

std::vector<std::uint8_t> ArtBoxEngine::readValue(
    std::uint64_t reference) const {
    if (reference == 0) return {};
    requireAttached();
    const auto offset = reference - 1U;
    const auto size = valueLengthAt(offset);
    try {
        if (box_->AllocatedSize(offset) != BoxAllocator::RoundSize(size)) {
            throw ErrCorruptRegion("ArtBox value allocation size mismatch");
        }
    } catch (const AllocatorError& error) {
        raiseAllocator(error);
    }
    const auto* data = boxData(offset);
    return {data, data + size};
}

std::string_view ArtBoxEngine::readPrefix(
    std::uint64_t reference,
    std::uint32_t length,
    bool require_allocation) const {
    if (reference == 0 || length == 0) {
        if (reference == 0 && length == 0) return {};
        throw ErrCorruptRegion("invalid ArtBox prefix reference");
    }
    const auto offset = reference - 1U;
    if (offset >= geometry_.box_data_bytes ||
        length > geometry_.box_data_bytes - offset) {
        throw ErrCorruptRegion("ArtBox prefix exceeds the Box data zone");
    }
    if (require_allocation) {
        requireAttached();
        try {
            if (box_->AllocatedSize(offset) !=
                BoxAllocator::RoundSize(length)) {
                throw ErrCorruptRegion(
                    "ArtBox prefix allocation size mismatch");
            }
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
    }
    return {
        reinterpret_cast<const char*>(boxData(offset)),
        static_cast<std::size_t>(length)};
}

std::uint64_t ArtBoxEngine::allocateBoxBytes(
    const void* data,
    std::size_t size) {
    if (size == 0 || data == nullptr) {
        throw ErrCorruptRegion("invalid empty ArtBox allocation");
    }
    requireAttached();
    new_boxes_.reserve(new_boxes_.size() + 1U);
    try {
        const auto offset = box_->Allocate(size);
        std::memcpy(
            owner_->base + geometry_.box_data_offset + offset,
            data,
            size);
        new_boxes_.push_back(
            BoxAllocation{offset});
        return offset + 1U;
    } catch (const AllocatorError& error) {
        raiseAllocator(error);
    }
}

std::uint64_t ArtBoxEngine::allocateValue(
    const std::vector<std::uint8_t>& value) {
    if (value.empty()) return 0;
    const auto decoded = XValue::Decode(value);
    if (decoded.Encode() != value) {
        throw ErrInvalidValue("noncanonical encoded value");
    }
    return allocateBoxBytes(value.data(), value.size());
}

std::uint64_t ArtBoxEngine::allocatePrefix(std::string_view prefix) {
    if (prefix.empty()) return 0;
    if (prefix.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw ErrCapacity("ArtBox prefix exceeds 4 GiB");
    }
    return allocateBoxBytes(prefix.data(), prefix.size());
}

ArtBoxNodeRecord ArtBoxEngine::readRecord(
    std::uint32_t reference,
    InspectionMode mode) const {
    if (reference == ArtBoxNodeRefCodec::kEmpty) {
        throw ErrCorruptRegion("empty ArtBox node reference");
    }
    if (nodes_ == nullptr) {
        throw ErrCorruptRegion("ArtBox node allocators are not attached");
    }
    try {
        return mode == InspectionMode::Recovery
            ? nodes_->ReadForRecovery(reference)
            : nodes_->Read(reference);
    } catch (const AllocatorError& error) {
        raiseAllocator(error);
    }
}

std::uint32_t ArtBoxEngine::childAt(
    const ArtBoxNodeRecord& record,
    std::uint8_t edge) {
    if (record.kind == ArtBoxNodeKind::Node4 ||
        record.kind == ArtBoxNodeKind::Node16) {
        for (std::size_t slot = 0; slot < record.child_count; ++slot) {
            if (record.keys[slot] == edge) return record.children[slot];
        }
        return ArtBoxNodeRefCodec::kEmpty;
    }
    if (record.kind == ArtBoxNodeKind::Node48) {
        const auto slot = record.index[edge];
        return slot == UINT8_MAX
            ? ArtBoxNodeRefCodec::kEmpty
            : record.children[slot];
    }
    if (record.kind == ArtBoxNodeKind::Node256) {
        return record.children[edge];
    }
    throw ErrCorruptRegion("invalid ArtBox node kind");
}

std::vector<std::pair<std::uint8_t, std::uint32_t>>
ArtBoxEngine::childrenOf(const ArtBoxNodeRecord& record) {
    std::vector<std::pair<std::uint8_t, std::uint32_t>> children;
    children.reserve(record.child_count);
    for (std::size_t byte = 0; byte < 256; ++byte) {
        const auto edge = static_cast<std::uint8_t>(byte);
        const auto child = childAt(record, edge);
        if (child != ArtBoxNodeRefCodec::kEmpty) {
            children.emplace_back(edge, child);
        }
    }
    if (children.size() != record.child_count) {
        throw ErrCorruptRegion("ArtBox child count mismatch");
    }
    return children;
}

ArtBoxEngine::NodeData ArtBoxEngine::readData(
    std::uint32_t reference) const {
    const auto record = readRecord(reference, InspectionMode::Normal);
    NodeData data;
    data.prefix_ref = record.prefix_ref;
    data.value_ref = record.value_ref;
    data.prefix_length = record.prefix_len;
    data.kind = record.kind;
    data.has_value = record.has_value;
    data.children = childrenOf(record);
    return data;
}

std::string_view ArtBoxEngine::dataPrefix(const NodeData& data) const {
    return readPrefix(data.prefix_ref, data.prefix_length, true);
}

void ArtBoxEngine::setChild(
    NodeData* data,
    std::uint8_t edge,
    std::uint32_t child) {
    const auto position = std::lower_bound(
        data->children.begin(), data->children.end(), edge,
        [](const auto& item, std::uint8_t wanted) {
            return item.first < wanted;
        });
    if (position != data->children.end() && position->first == edge) {
        if (child == ArtBoxNodeRefCodec::kEmpty) {
            data->children.erase(position);
        } else {
            position->second = child;
        }
        return;
    }
    if (child == ArtBoxNodeRefCodec::kEmpty) {
        throw ErrCorruptRegion("missing ArtBox child");
    }
    data->children.insert(position, {edge, child});
}

void ArtBoxEngine::replacePrefix(
    NodeData* data,
    std::string_view prefix) {
    data->prefix_ref = allocatePrefix(prefix);
    data->prefix_length = static_cast<std::uint32_t>(prefix.size());
}

std::uint32_t ArtBoxEngine::allocateNode(const NodeData& data) {
    if (data.children.size() > 256 ||
        (data.prefix_length == 0) != (data.prefix_ref == 0) ||
        dataPrefix(data).size() != data.prefix_length ||
        (!data.has_value && data.value_ref != 0)) {
        throw ErrCorruptRegion("invalid ArtBox node data");
    }
    for (std::size_t index = 0; index < data.children.size(); ++index) {
        if (data.children[index].second == ArtBoxNodeRefCodec::kEmpty ||
            (index != 0 &&
             data.children[index - 1U].first >= data.children[index].first)) {
            throw ErrCorruptRegion("ArtBox children are not strictly sorted");
        }
    }

    ArtBoxNodeRecord record;
    record.prefix_ref = data.prefix_ref;
    record.value_ref = data.value_ref;
    record.prefix_len = data.prefix_length;
    record.child_count = static_cast<std::uint16_t>(data.children.size());
    record.kind = chooseKind(data.kind, data.children.size());
    record.has_value = data.has_value;
    if (record.kind == ArtBoxNodeKind::Node4 ||
        record.kind == ArtBoxNodeKind::Node16) {
        for (std::size_t slot = 0; slot < data.children.size(); ++slot) {
            record.keys[slot] = data.children[slot].first;
            record.children[slot] = data.children[slot].second;
        }
    } else if (record.kind == ArtBoxNodeKind::Node48) {
        for (std::size_t slot = 0; slot < data.children.size(); ++slot) {
            record.index[data.children[slot].first] =
                static_cast<std::uint8_t>(slot);
            record.children[slot] = data.children[slot].second;
        }
    } else {
        for (const auto& child : data.children) {
            record.children[child.first] = child.second;
        }
    }

    requireAttached();
    new_nodes_.reserve(new_nodes_.size() + 1U);
    try {
        const auto reference = nodes_->Allocate(record);
        new_nodes_.push_back(reference);
        return reference;
    } catch (const AllocatorError& error) {
        raiseAllocator(error);
    }
}

std::uint32_t ArtBoxEngine::makeLeaf(
    std::string_view prefix,
    std::uint64_t value_ref) {
    NodeData leaf;
    replacePrefix(&leaf, prefix);
    leaf.value_ref = value_ref;
    leaf.has_value = true;
    return allocateNode(leaf);
}

std::uint32_t ArtBoxEngine::normalizeAndAllocate(NodeData data) {
    if (!data.has_value && data.children.empty()) {
        return ArtBoxNodeRefCodec::kEmpty;
    }
    if (!data.has_value && data.children.size() == 1) {
        const auto edge = data.children.front().first;
        auto child = readData(data.children.front().second);
        const auto parent_prefix = dataPrefix(data);
        const auto child_prefix = dataPrefix(child);
        const auto maximum = static_cast<std::size_t>(
            std::numeric_limits<std::uint32_t>::max());
        if (child_prefix.size() >= maximum ||
            parent_prefix.size() > maximum - child_prefix.size() - 1U) {
            throw ErrCapacity("compacted ArtBox prefix exceeds 4 GiB");
        }
        std::string combined;
        combined.reserve(parent_prefix.size() + 1U + child_prefix.size());
        combined.append(parent_prefix);
        combined.push_back(static_cast<char>(edge));
        combined.append(child_prefix);
        replacePrefix(&child, combined);
        return allocateNode(child);
    }
    return allocateNode(data);
}

std::uint32_t ArtBoxEngine::rebuildPath(
    const std::vector<PathFrame>& frames,
    std::uint32_t child) {
    for (auto iterator = frames.rbegin(); iterator != frames.rend(); ++iterator) {
        auto parent = readData(iterator->reference);
        setChild(&parent, iterator->edge, child);
        child = normalizeAndAllocate(std::move(parent));
    }
    return child;
}

std::uint32_t ArtBoxEngine::findNode(std::string_view key) const {
    auto reference = visibleRoot();
    std::size_t depth = 0;
    while (reference != ArtBoxNodeRefCodec::kEmpty) {
        const auto record = readRecord(reference, InspectionMode::Normal);
        const auto prefix = readPrefix(
            record.prefix_ref, record.prefix_len, true);
        if (prefix.size() > key.size() - depth ||
            key.substr(depth, prefix.size()) != prefix) {
            return ArtBoxNodeRefCodec::kEmpty;
        }
        depth += prefix.size();
        if (depth == key.size()) return reference;
        const auto edge = static_cast<std::uint8_t>(
            static_cast<unsigned char>(key[depth]));
        ++depth;
        reference = childAt(record, edge);
    }
    return ArtBoxNodeRefCodec::kEmpty;
}

std::uint32_t ArtBoxEngine::subtreeForPrefix(
    std::string_view wanted,
    std::string* before) const {
    auto reference = visibleRoot();
    std::size_t depth = 0;
    before->clear();
    while (reference != ArtBoxNodeRefCodec::kEmpty) {
        const auto record = readRecord(reference, InspectionMode::Normal);
        const auto prefix = readPrefix(
            record.prefix_ref, record.prefix_len, true);
        const auto remaining = wanted.substr(depth);
        const auto compared = std::min(prefix.size(), remaining.size());
        if (prefix.substr(0, compared) != remaining.substr(0, compared)) {
            return ArtBoxNodeRefCodec::kEmpty;
        }
        if (remaining.size() <= prefix.size()) return reference;
        before->append(prefix);
        depth += prefix.size();
        const auto edge = static_cast<std::uint8_t>(
            static_cast<unsigned char>(wanted[depth]));
        ++depth;
        before->push_back(static_cast<char>(edge));
        reference = childAt(record, edge);
    }
    return ArtBoxNodeRefCodec::kEmpty;
}

std::vector<EngineEntry> ArtBoxEngine::entriesFrom(
    std::uint32_t root,
    std::string before) const {
    std::vector<EngineEntry> result;
    if (root == ArtBoxNodeRefCodec::kEmpty) return result;
    std::vector<WalkItem> stack;
    stack.push_back({root, std::move(before)});
    while (!stack.empty()) {
        auto item = std::move(stack.back());
        stack.pop_back();
        const auto record = readRecord(
            item.reference, InspectionMode::Normal);
        item.before.append(readPrefix(
            record.prefix_ref, record.prefix_len, true));
        if (record.has_value) {
            result.emplace_back(item.before, readValue(record.value_ref));
        }
        for (std::size_t index = 256; index != 0; --index) {
            const auto edge = static_cast<std::uint8_t>(index - 1U);
            const auto child = childAt(record, edge);
            if (child == ArtBoxNodeRefCodec::kEmpty) continue;
            auto child_before = item.before;
            child_before.push_back(static_cast<char>(edge));
            stack.push_back({child, std::move(child_before)});
        }
    }
    return result;
}

ArtBoxEngine::Inspection ArtBoxEngine::inspect(
    std::uint32_t root,
    InspectionMode mode,
    bool require_box,
    bool require_exact_nodes) const {
    if (nodes_ == nullptr || (require_box && box_ == nullptr)) {
        throw ErrCorruptRegion("ArtBox allocator is not attached");
    }
    if (mode == InspectionMode::Recovery &&
        (require_box || require_exact_nodes)) {
        throw ErrCorruptRegion("invalid ArtBox recovery inspection mode");
    }

    Inspection state;
    const auto add_box = [&](std::uint64_t reference, std::uint64_t size) {
        if (reference == 0 || size == 0) {
            throw ErrCorruptRegion("invalid empty ArtBox Box reference");
        }
        const auto offset = reference - 1U;
        if (!state.box_sizes.emplace(offset, size).second) {
            throw ErrCorruptRegion("ArtBox objects share a Box allocation");
        }
        try {
            const auto rounded = BoxAllocator::RoundSize(size);
            if (offset >= geometry_.box_data_bytes ||
                rounded > geometry_.box_data_bytes - offset) {
                throw ErrCorruptRegion(
                    "ArtBox Box interval exceeds the data zone");
            }
            if (require_box && box_->AllocatedSize(offset) != rounded) {
                throw ErrCorruptRegion(
                    "ArtBox Box allocation size mismatch");
            }
            state.intervals.push_back(BoxLiveInterval{offset, size});
            state.live_bytes = checkedAdd(state.live_bytes, rounded);
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
    };

    std::vector<std::uint32_t> stack;
    if (root != ArtBoxNodeRefCodec::kEmpty) stack.push_back(root);
    while (!stack.empty()) {
        const auto reference = stack.back();
        stack.pop_back();
        if (!state.node_set.insert(reference).second) {
            throw ErrCorruptRegion("ArtBox node is shared or cyclic");
        }
        state.node_refs.push_back(reference);
        const auto record = readRecord(reference, mode);
        const auto children = childrenOf(record);
        if ((!record.has_value && children.size() <= 1U) ||
            (record.kind == ArtBoxNodeKind::Node16 &&
             children.size() < 5U) ||
            (record.kind == ArtBoxNodeKind::Node48 &&
             children.size() < 17U) ||
            (record.kind == ArtBoxNodeKind::Node256 &&
             children.size() < 38U)) {
            throw ErrCorruptRegion("noncanonical committed ArtBox shape");
        }

        state.live_bytes = checkedAdd(
            state.live_bytes,
            static_cast<std::uint64_t>(
                ArtBoxNodeCodec::kPayloadBytes[kindIndex(record.kind)]));
        if (record.prefix_ref != 0) {
            static_cast<void>(readPrefix(
                record.prefix_ref, record.prefix_len, false));
            add_box(record.prefix_ref, record.prefix_len);
        }
        if (record.has_value) {
            ++state.entries;
            if (record.value_ref != 0) {
                const auto logical_size = valueLengthAt(
                    record.value_ref - 1U);
                add_box(record.value_ref, logical_size);
            }
        }
        for (const auto& child : children) stack.push_back(child.second);
    }
    state.nodes = state.node_refs.size();
    if (state.entries > owner_->header->entry_limit) {
        throw ErrCorruptRegion(
            "ArtBox committed entries exceed the configured limit");
    }

    std::sort(
        state.intervals.begin(),
        state.intervals.end(),
        [](const BoxLiveInterval& left, const BoxLiveInterval& right) {
            return left.offset < right.offset;
        });
    std::uint64_t previous_end = 0;
    bool have_previous = false;
    for (const auto& interval : state.intervals) {
        std::uint64_t rounded = 0;
        try {
            rounded = BoxAllocator::RoundSize(interval.size);
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
        if (have_previous && interval.offset < previous_end) {
            throw ErrCorruptRegion("ArtBox Box intervals overlap");
        }
        previous_end = checkedAdd(interval.offset, rounded);
        have_previous = true;
    }

    if (require_exact_nodes) {
        try {
            nodes_->Validate();
            std::uint64_t used = 0;
            for (std::size_t index = 0; index < 4; ++index) {
                used = checkedAdd(
                    used,
                    nodes_->UsedCount(
                        static_cast<ArtBoxNodeKind>(index + 1U)));
            }
            if (used != state.nodes) {
                throw ErrCorruptRegion(
                    "ArtBox allocators contain unreachable nodes");
            }
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
    }
    return state;
}

void ArtBoxEngine::requireHeaderState(const Inspection& state) const {
    if (state.nodes != owner_->header->node_count ||
        state.entries != owner_->header->entry_count ||
        state.live_bytes != owner_->header->engine_live_bytes ||
        owner_->header->tombstone_count != 0 ||
        owner_->header->root_offset != 0) {
        throw ErrCorruptRegion(
            "ArtBox counters do not match committed data");
    }
}

void ArtBoxEngine::Validate() const {
    auto* self = const_cast<ArtBoxEngine*>(this);
    self->attachNormal();
    const auto state = self->inspect(
        self->committedRoot(), InspectionMode::Normal, true, true);
    self->requireHeaderState(state);
}

bool ArtBoxEngine::Get(
    std::string_view key,
    std::vector<std::uint8_t>* value) const {
    requireAttached();
    const auto reference = findNode(key);
    if (reference == ArtBoxNodeRefCodec::kEmpty) return false;
    const auto record = readRecord(reference, InspectionMode::Normal);
    if (!record.has_value) return false;
    if (value != nullptr) *value = readValue(record.value_ref);
    return true;
}

bool ArtBoxEngine::Exists(std::string_view key) const {
    return Get(key, nullptr);
}

void ArtBoxEngine::Put(
    std::string_view key,
    const std::vector<std::uint8_t>& value) {
    const bool implicit = !owner_->mutation_active;
    if (implicit) BeginMutation();
    try {
        putInMutation(key, value);
        if (implicit) CommitMutation();
    } catch (...) {
        RollbackMutation();
        throw;
    }
}

bool ArtBoxEngine::Erase(std::string_view key) {
    const bool implicit = !owner_->mutation_active;
    if (implicit) BeginMutation();
    try {
        const bool erased = eraseInMutation(key);
        if (implicit) CommitMutation();
        return erased;
    } catch (...) {
        RollbackMutation();
        throw;
    }
}

std::vector<EngineEntry> ArtBoxEngine::Entries() const {
    requireAttached();
    return entriesFrom(visibleRoot(), {});
}

std::vector<EngineEntry> ArtBoxEngine::EntriesWithPrefix(
    std::string_view prefix) const {
    requireAttached();
    std::string before;
    const auto root = subtreeForPrefix(prefix, &before);
    if (root == ArtBoxNodeRefCodec::kEmpty) return {};
    return entriesFrom(root, std::move(before));
}

std::unique_ptr<ShmPreparedClear> ArtBoxEngine::PrepareClear() {
    if (owner_->mutation_active) {
        throw Error("kvspace: cannot clear during a mutation");
    }
    requireAttached();
    try {
        const auto current = inspect(
            committedRoot(), InspectionMode::Normal, true, true);
        requireHeaderState(current);

        auto recovery_nodes = ArtBoxNodeStore::AttachForRecovery(
            nodeRegions(), geometry_.node_capacity);
        ArtBoxNodeLiveBitmaps empty_bitmaps;
        ArtBoxNodeLiveBitCounts empty_counts{};
        auto node_plan = std::move(recovery_nodes).PrepareRecovery(
            std::move(empty_bitmaps), empty_counts);
        auto recovery_box = BoxAllocator::AttachForRecovery(
            owner_->base + geometry_.box_metadata_offset,
            static_cast<std::size_t>(geometry_.box_metadata_bytes),
            geometry_.box_data_bytes);
        const std::vector<BoxLiveInterval> empty_intervals;
        auto box_plan = std::move(recovery_box).PrepareRebuild(
            empty_intervals);

        return MakeShmPreparedClear(
            [this,
             node_plan = std::move(node_plan),
             box_plan = std::move(box_plan)]() mutable {
                owner_->header->root_offset = 0;
                owner_->header->node_count = 0;
                owner_->header->entry_count = 0;
                owner_->header->tombstone_count = 0;
                owner_->header->engine_live_bytes = 0;
                ++owner_->header->generation;
                atomicStoreLe(
                    artBoxRootStorage(owner_->base, owner_->header),
                    ArtBoxNodeRefCodec::kEmpty,
                    __ATOMIC_RELEASE);
                emitRegionTestEvent(RegionTestEvent::MutationPublished);
                *nodes_ = std::move(node_plan).Apply();
                emitRegionTestEvent(RegionTestEvent::ArtBoxNodesRebuilt);
                *box_ = std::move(box_plan).Apply();
                emitRegionTestEvent(RegionTestEvent::ArtBoxBoxRebuilt);
                resetMutationState();
                owner_->mutation_allocations.clear();
            });
    } catch (const AllocatorError& error) {
        raiseAllocator(error);
    }
}

void ArtBoxEngine::Compact() {}

void ArtBoxEngine::BeginMutation() {
    if (owner_->mutation_active) throw Error("kvspace: nested mutation");
    requireAttached();
    owner_->mutation_allocations.clear();
    owner_->mutation_active = true;
    staged_root_ = committedRoot();
    staged_entries_ = owner_->header->entry_count;
    new_nodes_.clear();
    new_boxes_.clear();
}

void ArtBoxEngine::putInMutation(
    std::string_view key,
    const std::vector<std::uint8_t>& value) {
    requireAttached();
    if (staged_entries_ > owner_->header->entry_limit) {
        throw ErrCorruptRegion("ArtBox staged count exceeds limit");
    }
    const auto existing_ref = findNode(key);
    bool existed = false;
    if (existing_ref != ArtBoxNodeRefCodec::kEmpty) {
        const auto existing = readRecord(
            existing_ref, InspectionMode::Normal);
        existed = existing.has_value;
        if (existed && readValue(existing.value_ref) == value) return;
    }
    const auto projected = existed
        ? staged_entries_
        : checkedAdd(staged_entries_, 1U);
    if (projected > owner_->header->entry_limit) {
        throw ErrCapacity("maximum key count reached");
    }
    const auto value_ref = allocateValue(value);

    const auto apply = [&] {
        if (staged_root_ == ArtBoxNodeRefCodec::kEmpty) {
            staged_root_ = makeLeaf(key, value_ref);
            staged_entries_ = projected;
            return;
        }

        std::vector<PathFrame> frames;
        auto current = staged_root_;
        std::size_t depth = 0;
        for (;;) {
            const auto record = readRecord(
                current, InspectionMode::Normal);
            const auto prefix = readPrefix(
                record.prefix_ref, record.prefix_len, true);
            const auto remaining = key.substr(depth);
            std::size_t common = 0;
            const auto compared = std::min(prefix.size(), remaining.size());
            while (common < compared &&
                   prefix[common] == remaining[common]) {
                ++common;
            }
            if (common != prefix.size()) {
                auto old_child = readData(current);
                const auto old_edge = static_cast<std::uint8_t>(
                    static_cast<unsigned char>(prefix[common]));
                replacePrefix(&old_child, prefix.substr(common + 1U));
                const auto old_child_ref = allocateNode(old_child);

                NodeData parent;
                replacePrefix(&parent, prefix.substr(0, common));
                parent.children.emplace_back(old_edge, old_child_ref);
                const auto split_depth = depth + common;
                if (split_depth == key.size()) {
                    parent.has_value = true;
                    parent.value_ref = value_ref;
                } else {
                    const auto new_edge = static_cast<std::uint8_t>(
                        static_cast<unsigned char>(key[split_depth]));
                    const auto leaf = makeLeaf(
                        key.substr(split_depth + 1U), value_ref);
                    parent.children.emplace_back(new_edge, leaf);
                    std::sort(
                        parent.children.begin(), parent.children.end());
                }
                staged_root_ = rebuildPath(
                    frames, allocateNode(parent));
                staged_entries_ = projected;
                return;
            }

            depth += prefix.size();
            if (depth == key.size()) {
                auto replacement = readData(current);
                replacement.has_value = true;
                replacement.value_ref = value_ref;
                staged_root_ = rebuildPath(
                    frames, allocateNode(replacement));
                staged_entries_ = projected;
                return;
            }

            const auto edge = static_cast<std::uint8_t>(
                static_cast<unsigned char>(key[depth]));
            ++depth;
            const auto next = childAt(record, edge);
            if (next == ArtBoxNodeRefCodec::kEmpty) {
                const auto leaf = makeLeaf(key.substr(depth), value_ref);
                auto replacement = readData(current);
                setChild(&replacement, edge, leaf);
                staged_root_ = rebuildPath(
                    frames, allocateNode(replacement));
                staged_entries_ = projected;
                return;
            }
            frames.push_back({current, edge});
            current = next;
        }
    };
    apply();
    pruneBatchCreated();
}

bool ArtBoxEngine::eraseInMutation(std::string_view key) {
    requireAttached();
    if (staged_entries_ > owner_->header->entry_limit) {
        throw ErrCorruptRegion("ArtBox staged count exceeds limit");
    }
    std::vector<PathFrame> frames;
    auto current = staged_root_;
    std::size_t depth = 0;
    while (current != ArtBoxNodeRefCodec::kEmpty) {
        const auto record = readRecord(current, InspectionMode::Normal);
        const auto prefix = readPrefix(
            record.prefix_ref, record.prefix_len, true);
        if (prefix.size() > key.size() - depth ||
            key.substr(depth, prefix.size()) != prefix) {
            return false;
        }
        depth += prefix.size();
        if (depth == key.size()) {
            if (!record.has_value) return false;
            if (staged_entries_ == 0) {
                throw ErrCorruptRegion("ArtBox staged count underflows");
            }
            const auto projected = staged_entries_ - 1U;
            auto replacement = readData(current);
            replacement.has_value = false;
            replacement.value_ref = 0;
            auto child = normalizeAndAllocate(std::move(replacement));
            staged_root_ = rebuildPath(frames, child);
            staged_entries_ = projected;
            pruneBatchCreated();
            return true;
        }
        const auto edge = static_cast<std::uint8_t>(
            static_cast<unsigned char>(key[depth]));
        ++depth;
        const auto next = childAt(record, edge);
        if (next == ArtBoxNodeRefCodec::kEmpty) return false;
        frames.push_back({current, edge});
        current = next;
    }
    return false;
}

void ArtBoxEngine::clearInMutation() {
    if (staged_root_ == ArtBoxNodeRefCodec::kEmpty) return;
    staged_root_ = ArtBoxNodeRefCodec::kEmpty;
    staged_entries_ = 0;
    pruneBatchCreated();
}

void ArtBoxEngine::pruneBatchCreated() {
    const auto committed = inspect(
        committedRoot(), InspectionMode::Normal, true, false);
    const auto staged = inspect(
        staged_root_, InspectionMode::Normal, true, false);

    for (std::size_t index = 0; index < new_nodes_.size();) {
        const auto reference = new_nodes_[index];
        if (committed.node_set.count(reference) != 0 ||
            staged.node_set.count(reference) != 0) {
            ++index;
            continue;
        }
        try {
            nodes_->DiscardUnpublished(reference);
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
        new_nodes_[index] = new_nodes_.back();
        new_nodes_.pop_back();
    }
    for (std::size_t index = 0; index < new_boxes_.size();) {
        const auto offset = new_boxes_[index].offset;
        if (committed.box_sizes.count(offset) != 0 ||
            staged.box_sizes.count(offset) != 0) {
            ++index;
            continue;
        }
        try {
            box_->Free(offset);
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
        new_boxes_[index] = new_boxes_.back();
        new_boxes_.pop_back();
    }
}

void ArtBoxEngine::reclaimAfterPublish(
    const Inspection& old_state,
    const Inspection& final_state) {
    for (const auto reference : old_state.node_refs) {
        if (final_state.node_set.count(reference) != 0) continue;
        nodes_->ReclaimPublished(reference);
        emitRegionTestEvent(RegionTestEvent::ArtBoxNodeReclaimed);
    }
    for (const auto reference : new_nodes_) {
        if (final_state.node_set.count(reference) != 0) continue;
        nodes_->DiscardUnpublished(reference);
        emitRegionTestEvent(RegionTestEvent::ArtBoxNodeReclaimed);
    }
    for (const auto& interval : old_state.intervals) {
        if (final_state.box_sizes.count(interval.offset) != 0) continue;
        box_->Free(interval.offset);
        emitRegionTestEvent(RegionTestEvent::ArtBoxObjectReclaimed);
    }
    for (const auto& allocation : new_boxes_) {
        if (final_state.box_sizes.count(allocation.offset) != 0) continue;
        box_->Free(allocation.offset);
        emitRegionTestEvent(RegionTestEvent::ArtBoxObjectReclaimed);
    }
}

void ArtBoxEngine::CommitMutation() {
    if (!owner_->mutation_active) throw Error("kvspace: no active mutation");
    requireAttached();
    const auto old_state = inspect(
        committedRoot(), InspectionMode::Normal, true, false);
    const auto final_state = inspect(
        staged_root_, InspectionMode::Normal, true, false);
    if (final_state.entries != staged_entries_) {
        throw ErrCorruptRegion("staged ArtBox entry count mismatch");
    }
    auto* const root_storage =
        artBoxRootStorage(owner_->base, owner_->header);

    owner_->header->entry_count = staged_entries_;
    owner_->header->node_count = final_state.nodes;
    owner_->header->tombstone_count = 0;
    owner_->header->engine_live_bytes = final_state.live_bytes;
    ++owner_->header->generation;
    atomicStoreLe(
        root_storage,
        staged_root_,
        __ATOMIC_RELEASE);
    emitRegionTestEvent(RegionTestEvent::MutationPublished);
    owner_->mutation_active = false;

    try {
        reclaimAfterPublish(old_state, final_state);
        resetMutationState();
        owner_->mutation_allocations.clear();
    } catch (const AllocatorError& error) {
        owner_->mutation_allocations.clear();
        try {
            Recover();
        } catch (...) {
            owner_->header->corrupt = 1;
            throw ErrCorruptRegion(error.what());
        }
    } catch (...) {
        owner_->mutation_allocations.clear();
        try {
            Recover();
        } catch (...) {
            owner_->header->corrupt = 1;
            throw;
        }
    }
}

void ArtBoxEngine::RollbackMutation() noexcept {
    if (!owner_->mutation_active) return;
    owner_->mutation_active = false;
    try {
        for (auto iterator = new_nodes_.rbegin();
             iterator != new_nodes_.rend();
             ++iterator) {
            nodes_->DiscardUnpublished(*iterator);
        }
        for (auto iterator = new_boxes_.rbegin();
             iterator != new_boxes_.rend();
             ++iterator) {
            box_->Free(iterator->offset);
        }
    } catch (...) {
        try {
            Recover();
        } catch (...) {
            owner_->header->corrupt = 1;
        }
    }
    resetMutationState();
    owner_->mutation_allocations.clear();
}

std::unique_ptr<ShmPreparedRecovery> ArtBoxEngine::PrepareRecovery() {
    owner_->mutation_active = false;
    owner_->mutation_allocations.clear();
    nodes_.reset();
    box_.reset();
    try {
        auto common_plan = owner_->prepareCommonRecovery();
        nodes_ = std::make_unique<ArtBoxNodeStore>(
            ArtBoxNodeStore::AttachForRecovery(
                nodeRegions(), geometry_.node_capacity));
        box_ = std::make_unique<BoxAllocator>(
            BoxAllocator::AttachForRecovery(
                owner_->base + geometry_.box_metadata_offset,
                static_cast<std::size_t>(geometry_.box_metadata_bytes),
                geometry_.box_data_bytes));

        const auto root = committedRoot();
        const auto preflight = inspect(
            root, InspectionMode::Recovery, false, false);
        ArtBoxNodeLiveBitCounts bit_counts{};
        for (const auto reference : preflight.node_refs) {
            const auto kind = ArtBoxNodeRefCodec::Kind(reference);
            const auto local_id = ArtBoxNodeRefCodec::LocalId(reference);
            const auto index = kindIndex(kind);
            bit_counts[index] = std::max(
                bit_counts[index],
                static_cast<std::size_t>(local_id) + 1U);
        }
        ArtBoxNodeLiveBitmaps bitmaps;
        for (std::size_t index = 0; index < bitmaps.size(); ++index) {
            bitmaps[index].resize((bit_counts[index] + 7U) / 8U, 0);
        }
        for (const auto reference : preflight.node_refs) {
            const auto kind = ArtBoxNodeRefCodec::Kind(reference);
            const auto local_id = ArtBoxNodeRefCodec::LocalId(reference);
            const auto index = kindIndex(kind);
            bitmaps[index][local_id / 8U] |= static_cast<std::uint8_t>(
                1U << static_cast<unsigned>(local_id % 8U));
        }

        auto node_plan = std::move(*nodes_).PrepareRecovery(
            std::move(bitmaps), bit_counts);
        auto box_plan = std::move(*box_).PrepareRebuild(
            preflight.intervals);

        // Both recovery handles, the complete raw graph/Box inspection, all
        // live bitmaps, normalized intervals, and all five allocator scratch
        // resources exist before either allocator writes mutable metadata.
        return MakeShmPreparedRecovery(
            [this,
             preflight = std::move(preflight),
             node_plan = std::move(node_plan),
             box_plan = std::move(box_plan),
             common_plan = std::move(common_plan)]() mutable {
                try {
                    *nodes_ = std::move(node_plan).Apply();
                    emitRegionTestEvent(
                        RegionTestEvent::ArtBoxNodesRebuilt);
                    *box_ = std::move(box_plan).Apply();
                    emitRegionTestEvent(
                        RegionTestEvent::ArtBoxBoxRebuilt);
                    owner_->header->node_count = preflight.nodes;
                    owner_->header->entry_count = preflight.entries;
                    owner_->header->tombstone_count = 0;
                    owner_->header->engine_live_bytes =
                        preflight.live_bytes;
                    owner_->applyCommonRecovery(common_plan);
                    resetMutationState();
                } catch (const AllocatorError& error) {
                    throw ErrCorruptRegion(error.what());
                }
            });
    } catch (const AllocatorError& error) {
        throw ErrCorruptRegion(error.what());
    }
}

void ArtBoxEngine::resetMutationState() noexcept {
    staged_root_ = ArtBoxNodeRefCodec::kEmpty;
    staged_entries_ = 0;
    new_nodes_.clear();
    new_boxes_.clear();
}

class TrieBoxEngine final : public ShmEngineStore {
public:
    explicit TrieBoxEngine(Region::Impl* owner)
        : owner_(owner), geometry_(readTrieBoxGeometry(owner->base, owner->header)) {}

    ShmEngine Id() const noexcept override { return ShmEngine::TrieBox; }

    void Validate() const override {
        auto* self = const_cast<TrieBoxEngine*>(this);
        try {
            self->attachNormal();
            const auto state = self->inspect(
                self->committedRoot(), true, true);
            self->requireHeaderState(state);
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
    }

    bool Get(
        std::string_view key,
        std::vector<std::uint8_t>* value) const override {
        requireAttached();
        ByteTrieIndex index(*nodes_);
        const auto found = index.Get(visibleRoot(), key);
        if (!found.has_value()) return false;
        if (value != nullptr) *value = readValue(*found);
        return true;
    }

    bool Exists(std::string_view key) const override { return Get(key, nullptr); }

    void Put(
        std::string_view key,
        const std::vector<std::uint8_t>& value) override {
        const bool implicit = !owner_->mutation_active;
        if (implicit) BeginMutation();
        try {
            putInMutation(key, value);
            if (implicit) CommitMutation();
        } catch (...) {
            if (implicit) RollbackMutation();
            throw;
        }
    }

    bool Erase(std::string_view key) override {
        const bool implicit = !owner_->mutation_active;
        if (implicit) BeginMutation();
        try {
            const bool erased = eraseInMutation(key);
            if (implicit) CommitMutation();
            return erased;
        } catch (...) {
            if (implicit) RollbackMutation();
            throw;
        }
    }

    std::vector<EngineEntry> Entries() const override {
        return entriesFrom(visibleRoot(), {});
    }

    std::vector<EngineEntry> EntriesWithPrefix(
        std::string_view prefix) const override {
        auto entries = entriesFrom(visibleRoot(), {});
        entries.erase(
            std::remove_if(
                entries.begin(),
                entries.end(),
                [prefix](const EngineEntry& entry) {
                    return entry.first.size() < prefix.size() ||
                        std::string_view(entry.first).substr(0, prefix.size()) !=
                            prefix;
                }),
            entries.end());
        return entries;
    }

    std::unique_ptr<ShmPreparedClear> PrepareClear() override {
        if (owner_->mutation_active) {
            throw Error("kvspace: cannot clear during a mutation");
        }
        requireAttached();
        try {
            auto current = inspect(committedRoot(), true, true);
            requireHeaderState(current);
            if (current.entries != 0 &&
                nodes_->UsedCount() >= nodes_->Capacity()) {
                throw ErrCapacity(
                    "TrieBox cannot materialize an empty Clear root");
            }
            return MakeShmPreparedClear(
                [this, current = std::move(current)]() mutable {
                    std::uint32_t empty_root = committedRoot();
                    if (current.entries != 0) {
                        // Allocate encodes the complete canonical empty payload
                        // before touching FixedBlock metadata and performs no
                        // dynamic allocation.  Until root publication it is an
                        // unreachable staged node recoverable from the old root.
                        empty_root = nodes_->Allocate();
                    }
                    owner_->header->root_offset = 0;
                    owner_->header->node_count = 0;
                    owner_->header->entry_count = 0;
                    owner_->header->tombstone_count = 0;
                    owner_->header->engine_live_bytes = 0;
                    ++owner_->header->generation;
                    trieBoxRootStorage(owner_->base, owner_->header)
                        .StoreRelease(empty_root);
                    emitRegionTestEvent(RegionTestEvent::MutationPublished);
                    if (current.entries != 0) {
                        for (const auto id : current.node_ids) {
                            nodes_->ReclaimPublished(id);
                            emitRegionTestEvent(
                                RegionTestEvent::TrieNodeReclaimed);
                        }
                        for (const auto& interval : current.intervals) {
                            box_->Free(interval.offset);
                            emitRegionTestEvent(
                                RegionTestEvent::TrieBoxReclaimed);
                        }
                    }
                    resetMutationState();
                    owner_->mutation_allocations.clear();
                });
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
    }

    void Compact() override {}

    void BeginMutation() override {
        if (owner_->mutation_active) throw Error("kvspace: nested mutation");
        requireAttached();
        owner_->mutation_allocations.clear();
        owner_->mutation_active = true;
        staged_root_ = committedRoot();
        staged_entries_ = owner_->header->entry_count;
        new_nodes_.clear();
        retired_nodes_.clear();
        new_boxes_.clear();
        retired_boxes_.clear();
    }

    void CommitMutation() override {
        if (!owner_->mutation_active) throw Error("kvspace: no active mutation");
        requireAttached();
        const auto state = inspect(staged_root_, true, false);
        if (state.entries != staged_entries_) {
            throw ErrCorruptRegion("staged trie-box entry count mismatch");
        }

        owner_->header->entry_count = staged_entries_;
        owner_->header->node_count = state.logical_nodes;
        owner_->header->tombstone_count = 0;
        owner_->header->engine_live_bytes = state.live_bytes;
        ++owner_->header->generation;
        trieBoxRootStorage(owner_->base, owner_->header)
            .StoreRelease(staged_root_);
        emitRegionTestEvent(RegionTestEvent::MutationPublished);
        owner_->mutation_active = false;

        try {
            for (const auto id : retired_nodes_) {
                nodes_->ReclaimPublished(id);
                emitRegionTestEvent(
                    RegionTestEvent::TrieNodeReclaimed);
            }
            for (const auto& allocation : retired_boxes_) {
                box_->Free(allocation.offset);
                emitRegionTestEvent(
                    RegionTestEvent::TrieBoxReclaimed);
            }
            nodes_->Validate();
            box_->Validate();
            const auto committed = inspect(staged_root_, true, true);
            requireHeaderState(committed);
            new_nodes_.clear();
            retired_nodes_.clear();
            new_boxes_.clear();
            retired_boxes_.clear();
            owner_->mutation_allocations.clear();
        } catch (const AllocatorError& error) {
            owner_->mutation_allocations.clear();
            try {
                Recover();
            } catch (...) {
                owner_->header->corrupt = 1;
                throw ErrCorruptRegion(error.what());
            }
        } catch (...) {
            owner_->mutation_allocations.clear();
            try {
                Recover();
            } catch (...) {
                owner_->header->corrupt = 1;
                throw;
            }
        }
    }

    void RollbackMutation() noexcept override {
        if (!owner_->mutation_active) return;
        owner_->mutation_active = false;
        try {
            for (auto cursor = new_nodes_.rbegin();
                 cursor != new_nodes_.rend();
                 ++cursor) {
                nodes_->DiscardUnpublished(*cursor);
            }
            for (auto cursor = new_boxes_.rbegin();
                 cursor != new_boxes_.rend();
                 ++cursor) {
                box_->Free(cursor->offset);
            }
        } catch (...) {
            try {
                Recover();
            } catch (...) {
                owner_->header->corrupt = 1;
            }
        }
        resetMutationState();
        owner_->mutation_allocations.clear();
    }

    std::unique_ptr<ShmPreparedRecovery> PrepareRecovery() override {
        owner_->mutation_active = false;
        owner_->mutation_allocations.clear();
        resetMutationState();
        nodes_.reset();
        box_.reset();
        try {
            auto common_plan = owner_->prepareCommonRecovery();
            const auto root = committedRoot();
            nodes_ = std::make_unique<TrieNodeStore>(
                TrieNodeStore::AttachForRecovery(
                    owner_->base + geometry_.node_metadata_offset,
                    static_cast<std::size_t>(geometry_.node_metadata_bytes),
                    owner_->base + geometry_.node_zone_offset,
                    static_cast<std::size_t>(geometry_.node_zone_bytes)));
            box_ = std::make_unique<BoxAllocator>(
                BoxAllocator::AttachForRecovery(
                    owner_->base + geometry_.box_metadata_offset,
                    static_cast<std::size_t>(geometry_.box_metadata_bytes),
                    geometry_.box_data_bytes));

            auto node_plan = std::move(*nodes_)
                .PrepareRecoveryFromCommittedRoot(root);
            const auto preflight = inspectPreparedRecovery(node_plan);
            auto box_plan = std::move(*box_).PrepareRebuild(
                preflight.intervals);

            // Both unique_ptr object shells, both immutable attaches, the
            // complete root-derived node/value scan, and every Box resource
            // are ready before the first recovery write in either allocator.
            return MakeShmPreparedRecovery(
                [this,
                 preflight = std::move(preflight),
                 node_plan = std::move(node_plan),
                 box_plan = std::move(box_plan),
                 common_plan = std::move(common_plan)]() mutable {
                    try {
                        *nodes_ = std::move(node_plan).Apply();
                        emitRegionTestEvent(
                            RegionTestEvent::TrieBoxNodesRebuilt);
                        *box_ = std::move(box_plan).Apply();
                        emitRegionTestEvent(
                            RegionTestEvent::TrieBoxBoxRebuilt);

                        owner_->header->node_count =
                            preflight.logical_nodes;
                        owner_->header->entry_count = preflight.entries;
                        owner_->header->tombstone_count = 0;
                        owner_->header->engine_live_bytes =
                            preflight.live_bytes;
                        owner_->applyCommonRecovery(common_plan);
                        resetMutationState();
                    } catch (const AllocatorError& error) {
                        throw ErrCorruptRegion(error.what());
                    }
                });
        } catch (const AllocatorError& error) {
            throw ErrCorruptRegion(error.what());
        }
    }

private:
    struct BoxAllocation {
        std::uint64_t offset = 0;
        std::uint64_t logical_size = 0;
    };

    struct Inspection {
        std::uint64_t nodes = 0;
        std::uint64_t logical_nodes = 0;
        std::uint64_t entries = 0;
        std::uint64_t live_bytes = 0;
        std::vector<BoxLiveInterval> intervals;
        std::vector<std::uint32_t> node_ids;
        std::vector<ByteTrieIndex::Entry> values;
    };

    [[noreturn]] static void raiseAllocator(const AllocatorError& error) {
        if (error.Code() == AllocatorErrorCode::Capacity) {
            throw ErrCapacity(error.what());
        }
        throw ErrCorruptRegion(error.what());
    }

    void requireAttached() const {
        if (nodes_ == nullptr || box_ == nullptr) {
            throw ErrCorruptRegion("TrieBox allocators are not attached");
        }
    }

    void attachNormal() {
        try {
            nodes_ = std::make_unique<TrieNodeStore>(
                TrieNodeStore::Attach(
                    owner_->base + geometry_.node_metadata_offset,
                    static_cast<std::size_t>(geometry_.node_metadata_bytes),
                    owner_->base + geometry_.node_zone_offset,
                    static_cast<std::size_t>(geometry_.node_zone_bytes)));
            box_ = std::make_unique<BoxAllocator>(
                BoxAllocator::Attach(
                    owner_->base + geometry_.box_metadata_offset,
                    static_cast<std::size_t>(geometry_.box_metadata_bytes),
                    geometry_.box_data_bytes));
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
    }

    std::uint32_t committedRoot() const {
        return trieBoxRootStorage(owner_->base, owner_->header)
            .LoadAcquire();
    }

    std::uint32_t visibleRoot() const {
        return owner_->mutation_active
            ? staged_root_
            : committedRoot();
    }

    std::size_t valueLengthAt(std::uint64_t offset) const {
        if (offset >= geometry_.box_data_bytes) {
            throw ErrCorruptRegion("TrieBox value offset is out of bounds");
        }
        const auto available = geometry_.box_data_bytes - offset;
        const auto* data = owner_->base + geometry_.box_data_offset + offset;
        if (available < 9) {
            throw ErrCorruptRegion("truncated TrieBox TLV header");
        }
        const auto kind_length = static_cast<std::uint64_t>(data[0]);
        const auto fixed = checkedAdd(checkedAdd(1, kind_length), 8);
        if (kind_length == 0 || fixed > available) {
            throw ErrCorruptRegion("invalid TrieBox TLV kind length");
        }
        const auto raw_length_offset = 1 + kind_length + 4;
        const auto raw_length = static_cast<std::uint64_t>(
            loadLe32(data + raw_length_offset));
        const auto total = checkedAdd(fixed, raw_length);
        if (total > available ||
            total > static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            throw ErrCorruptRegion("TrieBox TLV exceeds the Box data zone");
        }
        try {
            const auto decoded = XValue::Decode(
                data, static_cast<std::size_t>(total));
            if (decoded.Encode() != std::vector<std::uint8_t>(
                    data, data + static_cast<std::size_t>(total))) {
                throw ErrCorruptRegion("noncanonical TrieBox TLV");
            }
        } catch (const ErrCorruptRegion&) {
            throw;
        } catch (const Error& error) {
            throw ErrCorruptRegion(error.what());
        }
        return static_cast<std::size_t>(total);
    }

    std::vector<std::uint8_t> readValue(std::uint64_t reference) const {
        if (reference == 0) return {};
        const auto offset = reference - 1U;
        const auto size = valueLengthAt(offset);
        try {
            const auto allocated = box_->AllocatedSize(offset);
            if (allocated != BoxAllocator::RoundSize(size)) {
                throw ErrCorruptRegion("TrieBox value allocation size mismatch");
            }
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
        const auto* data = owner_->base + geometry_.box_data_offset + offset;
        return {data, data + size};
    }

    std::uint64_t allocateValue(const std::vector<std::uint8_t>& value) {
        if (value.empty()) return 0;
        const auto decoded = XValue::Decode(value);
        if (decoded.Encode() != value) {
            throw ErrInvalidValue("noncanonical encoded value");
        }
        new_boxes_.reserve(new_boxes_.size() + 1U);
        try {
            const auto offset = box_->Allocate(value.size());
            if (offset == UINT64_MAX ||
                offset + 1U > TrieNodeCodec::kMaxValueRef) {
                box_->Free(offset);
                throw ErrCapacity("TrieBox value reference exceeds 63 bits");
            }
            std::memcpy(
                owner_->base + geometry_.box_data_offset + offset,
                value.data(),
                value.size());
            new_boxes_.push_back(BoxAllocation{offset, value.size()});
            return offset + 1U;
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
    }

    void discardDelta(ByteTrieIndex::StagedDelta* delta) noexcept {
        try {
            for (auto cursor = delta->allocations.rbegin();
                 cursor != delta->allocations.rend();
                 ++cursor) {
                nodes_->DiscardUnpublished(*cursor);
            }
        } catch (...) {
            std::terminate();
        }
        delta->allocations.clear();
    }

    void retireNode(std::uint32_t id) {
        const auto fresh = std::find(new_nodes_.begin(), new_nodes_.end(), id);
        if (fresh != new_nodes_.end()) {
            nodes_->DiscardUnpublished(id);
            new_nodes_.erase(fresh);
            return;
        }
        if (std::find(retired_nodes_.begin(), retired_nodes_.end(), id) ==
            retired_nodes_.end()) {
            retired_nodes_.push_back(id);
        }
    }

    void adoptDelta(ByteTrieIndex::StagedDelta delta) {
        if (!delta.changed) return;
        try {
            new_nodes_.reserve(new_nodes_.size() + delta.allocations.size());
            retired_nodes_.reserve(
                retired_nodes_.size() + delta.retired.size());
        } catch (...) {
            discardDelta(&delta);
            throw;
        }
        new_nodes_.insert(
            new_nodes_.end(),
            delta.allocations.begin(),
            delta.allocations.end());
        staged_root_ = delta.new_root;
        for (const auto id : delta.retired) retireNode(id);
    }

    void retireBox(std::uint64_t reference) {
        if (reference == 0) return;
        const auto offset = reference - 1U;
        const auto fresh = std::find_if(
            new_boxes_.begin(),
            new_boxes_.end(),
            [offset](const BoxAllocation& allocation) {
                return allocation.offset == offset;
            });
        if (fresh != new_boxes_.end()) {
            box_->Free(offset);
            new_boxes_.erase(fresh);
            return;
        }
        if (std::none_of(
                retired_boxes_.begin(),
                retired_boxes_.end(),
                [offset](const BoxAllocation& allocation) {
                    return allocation.offset == offset;
                })) {
            retired_boxes_.push_back(
                BoxAllocation{offset, valueLengthAt(offset)});
        }
    }

    void putInMutation(
        std::string_view key,
        const std::vector<std::uint8_t>& value) {
        requireAttached();
        ByteTrieIndex index(*nodes_);
        const auto old_value = index.Get(staged_root_, key);
        const bool existed = old_value.has_value();
        if (staged_entries_ > owner_->header->entry_limit) {
            throw ErrCorruptRegion("TrieBox staged count exceeds limit");
        }
        const auto projected = existed
            ? staged_entries_
            : checkedAdd(staged_entries_, 1U);
        if (projected > owner_->header->entry_limit) {
            throw ErrCapacity("maximum key count reached");
        }
        if (existed && readValue(*old_value) == value) return;
        retired_boxes_.reserve(retired_boxes_.size() + (existed ? 1U : 0U));
        const auto new_box_count = new_boxes_.size();
        const auto reference = allocateValue(value);
        const auto discard_new_value = [&]() noexcept {
            if (new_boxes_.size() <= new_box_count) return;
            try {
                box_->Free(new_boxes_.back().offset);
                new_boxes_.pop_back();
            } catch (...) {
                std::terminate();
            }
        };
        try {
            auto prepared = index.PutFromValidatedRoot(
                staged_root_, key, reference);
            auto delta = std::move(prepared).DetachForBatch();
            adoptDelta(std::move(delta));
            if (old_value.has_value()) retireBox(*old_value);
        } catch (const AllocatorError& error) {
            discard_new_value();
            raiseAllocator(error);
        } catch (...) {
            discard_new_value();
            throw;
        }
        staged_entries_ = projected;
    }

    bool eraseInMutation(std::string_view key) {
        requireAttached();
        ByteTrieIndex index(*nodes_);
        const auto old_value = index.Get(staged_root_, key);
        if (staged_entries_ > owner_->header->entry_limit) {
            throw ErrCorruptRegion("TrieBox staged count exceeds limit");
        }
        if (!old_value.has_value()) return false;
        if (staged_entries_ == 0) {
            throw ErrCorruptRegion("TrieBox staged count underflows");
        }
        const auto projected = staged_entries_ - 1U;
        retired_boxes_.reserve(retired_boxes_.size() + 1U);
        try {
            auto prepared = index.EraseFromValidatedRoot(staged_root_, key);
            auto delta = std::move(prepared).DetachForBatch();
            adoptDelta(std::move(delta));
            retireBox(*old_value);
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
        staged_entries_ = projected;
        return true;
    }

    std::vector<EngineEntry> entriesFrom(
        std::uint32_t root,
        std::string) const {
        requireAttached();
        ByteTrieIndex index(*nodes_);
        std::vector<EngineEntry> result;
        const auto entries = index.Entries(root);
        result.reserve(entries.size());
        for (const auto& entry : entries) {
            result.emplace_back(entry.key, readValue(entry.value_ref));
        }
        return result;
    }

    Inspection inspect(
        std::uint32_t root,
        bool require_box,
        bool require_exact_nodes) const {
        if (nodes_ == nullptr || (require_box && box_ == nullptr)) {
            throw ErrCorruptRegion("TrieBox allocator is not attached");
        }
        Inspection state;
        std::vector<std::uint32_t> stack{root};
        std::unordered_set<std::uint32_t> seen;
        while (!stack.empty()) {
            const auto id = stack.back();
            stack.pop_back();
            if (!seen.insert(id).second) {
                throw ErrCorruptRegion("trie-box node is shared or cyclic");
            }
            state.node_ids.push_back(id);
            const auto node = nodes_->Read(id);
            bool has_child = false;
            for (const auto child : node.children) {
                if (child != TrieNodeRecord::kEmptyChild) {
                    has_child = true;
                    stack.push_back(static_cast<std::uint32_t>(child));
                }
            }
            if (id != root && !node.has_value && !has_child) {
                throw ErrCorruptRegion(
                    "TrieBox contains a non-root empty leaf");
            }
        }
        state.nodes = state.node_ids.size();
        ByteTrieIndex index(*nodes_);
        state.values = index.EntriesFromValidatedRoot(root);
        state.entries = state.values.size();
        if (state.entries > owner_->header->entry_limit) {
            throw ErrCorruptRegion(
                "TrieBox committed entries exceed the configured limit");
        }
        state.logical_nodes = state.entries == 0 ? 0 : state.nodes;
        state.live_bytes = checkedMultiply(
            state.logical_nodes, TrieNodeStore::kPersistentNodeBytes);
        std::unordered_set<std::uint64_t> offsets;
        state.intervals.reserve(state.values.size());
        for (const auto& entry : state.values) {
            if (entry.value_ref == 0) continue;
            const auto offset = entry.value_ref - 1U;
            if (!offsets.insert(offset).second) {
                throw ErrCorruptRegion("TrieBox values share a Box allocation");
            }
            const auto logical_size = valueLengthAt(offset);
            state.intervals.push_back(BoxLiveInterval{
                offset, static_cast<std::uint64_t>(logical_size)});
            if (require_box) {
                try {
                    const auto allocated = box_->AllocatedSize(offset);
                    if (allocated != BoxAllocator::RoundSize(logical_size)) {
                        throw ErrCorruptRegion(
                            "TrieBox value allocation size mismatch");
                    }
                    state.live_bytes = checkedAdd(
                        state.live_bytes, allocated);
                } catch (const AllocatorError& error) {
                    raiseAllocator(error);
                }
            } else {
                state.live_bytes = checkedAdd(
                    state.live_bytes,
                    BoxAllocator::RoundSize(logical_size));
            }
        }
        if (require_exact_nodes && nodes_->UsedCount() != state.nodes) {
            throw ErrCorruptRegion("TrieBox node allocator has unreachable nodes");
        }
        return state;
    }

    Inspection inspectPreparedRecovery(
        const TrieNodeStore::PreparedRecovery& prepared) const {
        Inspection state;
        state.nodes = prepared.ReachableNodeCount();
        state.entries = prepared.ValueReferences().size();
        if (state.entries > owner_->header->entry_limit) {
            throw ErrCorruptRegion(
                "TrieBox committed entries exceed the configured limit");
        }
        state.logical_nodes = state.entries == 0 ? 0 : state.nodes;
        state.live_bytes = checkedMultiply(
            state.logical_nodes, TrieNodeStore::kPersistentNodeBytes);
        std::unordered_set<std::uint64_t> offsets;
        state.intervals.reserve(prepared.ValueReferences().size());
        for (const auto reference : prepared.ValueReferences()) {
            if (reference == 0) continue;
            const auto offset = reference - 1U;
            if (!offsets.insert(offset).second) {
                throw ErrCorruptRegion(
                    "TrieBox values share a Box allocation");
            }
            const auto logical_size = valueLengthAt(offset);
            state.intervals.push_back(BoxLiveInterval{
                offset, static_cast<std::uint64_t>(logical_size)});
            state.live_bytes = checkedAdd(
                state.live_bytes,
                BoxAllocator::RoundSize(logical_size));
        }
        return state;
    }

    void requireHeaderState(const Inspection& state) const {
        if (state.logical_nodes != owner_->header->node_count ||
            state.entries != owner_->header->entry_count ||
            state.live_bytes != owner_->header->engine_live_bytes) {
            throw ErrCorruptRegion("TrieBox counters do not match committed data");
        }
    }

    void clearInMutation() {
        if (staged_entries_ == 0) return;
        const auto old_state = inspect(staged_root_, true, false);
        new_nodes_.reserve(new_nodes_.size() + 1U);
        retired_nodes_.reserve(retired_nodes_.size() + old_state.node_ids.size());
        retired_boxes_.reserve(
            retired_boxes_.size() + old_state.intervals.size());
        std::uint32_t new_root = 0;
        try {
            new_root = nodes_->Allocate();
            new_nodes_.push_back(new_root);
            staged_root_ = new_root;
            for (const auto id : old_state.node_ids) retireNode(id);
            for (const auto& entry : old_state.values) {
                retireBox(entry.value_ref);
            }
            staged_entries_ = 0;
        } catch (const AllocatorError& error) {
            raiseAllocator(error);
        }
    }

    void resetMutationState() noexcept {
        staged_root_ = 0;
        staged_entries_ = 0;
        new_nodes_.clear();
        retired_nodes_.clear();
        new_boxes_.clear();
        retired_boxes_.clear();
    }

    Region::Impl* owner_;
    TrieBoxGeometry geometry_;
    mutable std::unique_ptr<TrieNodeStore> nodes_;
    mutable std::unique_ptr<BoxAllocator> box_;
    std::uint32_t staged_root_ = 0;
    std::uint64_t staged_entries_ = 0;
    std::vector<std::uint32_t> new_nodes_;
    std::vector<std::uint32_t> retired_nodes_;
    std::vector<BoxAllocation> new_boxes_;
    std::vector<BoxAllocation> retired_boxes_;
};

std::unique_ptr<ShmEngineStore> makeEngine(
    ShmEngine engine,
    Region::Impl* owner) {
    requireImplementedEngine(engine);
    if (engine == ShmEngine::ArtBump) {
        return std::make_unique<ArtBumpEngineV4>(owner);
    }
    if (engine == ShmEngine::ArtBox) {
        return std::make_unique<ArtBoxEngine>(owner);
    }
    if (engine == ShmEngine::HashBox) {
        return std::make_unique<HashBoxEngine>(owner);
    }
    return std::make_unique<TrieBoxEngine>(owner);
}

namespace {

void initializeArtBumpSections(
    Region::Impl& impl,
    const Layout& layout) {
    const auto& geometry = layout.art_bump;
    static_cast<void>(ArtBumpHeaderView::Initialize(
        impl.base + geometry.engine_offset,
        ArtBumpGeometry::kEngineHeaderBytes,
        geometry.InitialHeader()));
    static_cast<void>(ArtBumpNodeStore::Initialize(
        impl.base,
        static_cast<std::size_t>(impl.mapped_size),
        geometry.slabs,
        geometry.node_capacity));

    const auto zero_range = [&impl](std::uint64_t first, std::uint64_t last) {
        if (last < first || last > impl.mapped_size) {
            throw ErrCapacity("ArtBump alignment padding is out of bounds");
        }
        std::memset(
            impl.base + first,
            0,
            static_cast<std::size_t>(last - first));
    };
    zero_range(
        geometry.engine_offset + ArtBumpGeometry::kEngineHeaderBytes,
        geometry.slabs.front().metadata_offset);
    for (std::size_t index = 0; index + 1U < geometry.slabs.size(); ++index) {
        zero_range(
            geometry.slabs[index].zone_offset +
                geometry.slabs[index].zone_bytes,
            geometry.slabs[index + 1U].metadata_offset);
    }
    zero_range(
        geometry.slabs.back().zone_offset +
            geometry.slabs.back().zone_bytes,
        geometry.raw_zones.front().begin);
}

void initializeHashBoxSections(
    Region::Impl& impl,
    const Layout& layout) {
    const auto& geometry = layout.hash_box;
    auto* bytes = impl.base + geometry.header_offset;
    std::memset(bytes, 0, static_cast<std::size_t>(kHashBoxHeaderBytes));
    std::copy(kHashBoxMagic.begin(), kHashBoxMagic.end(), bytes);
    storeLe32(bytes + 8, kHashBoxVersion);
    storeLe32(
        bytes + 12, static_cast<std::uint32_t>(kHashBoxHeaderBytes));
    storeLe64(
        bytes + kHashBoxMetadataOffset,
        geometry.box_metadata_offset);
    storeLe64(
        bytes + kHashBoxMetadataBytesOffset,
        geometry.box_metadata_bytes);
    storeLe64(bytes + kHashBoxDataOffset, geometry.box_data_offset);
    storeLe64(bytes + kHashBoxDataBytesOffset, geometry.box_data_bytes);
    storeLe64(
        bytes + kHashBoxImmutableHashOffset,
        hashBoxImmutableHash(
            geometry,
            layout.entry_limit,
            layout.table_capacity,
            layout.table_offset[0],
            layout.table_offset[1]));

    const auto table_bytes = checkedMultiply(
        layout.table_capacity, kHashBoxSlotBytes);
    std::memset(
        impl.base + layout.table_offset[0],
        0,
        static_cast<std::size_t>(table_bytes));
    std::memset(
        impl.base + layout.table_offset[1],
        0,
        static_cast<std::size_t>(table_bytes));
    try {
        static_cast<void>(BoxAllocator::Initialize(
            impl.base + geometry.box_metadata_offset,
            static_cast<std::size_t>(geometry.box_metadata_bytes),
            geometry.box_data_bytes));
    } catch (const AllocatorError& error) {
        if (error.Code() == AllocatorErrorCode::Capacity ||
            error.Code() == AllocatorErrorCode::InvalidArgument) {
            throw ErrCapacity(error.what());
        }
        throw ErrCorruptRegion(error.what());
    }
}

void initializeArtBoxSections(
    Region::Impl& impl,
    const Layout& layout) {
    const auto& geometry = layout.art_box;
    auto* bytes = impl.base + geometry.header_offset;
    std::memset(bytes, 0, static_cast<std::size_t>(kArtBoxHeaderBytes));
    std::copy(kArtBoxMagic.begin(), kArtBoxMagic.end(), bytes);
    storeLe32(bytes + 8, kArtBoxVersion);
    storeLe32(
        bytes + 12, static_cast<std::uint32_t>(kArtBoxHeaderBytes));
    storeLe32(bytes + kArtBoxRootOffset, UINT32_MAX);
    for (std::size_t index = 0; index < geometry.slabs.size(); ++index) {
        const auto offset = kArtBoxSlabDescriptorsOffset +
            index * kArtBoxSlabDescriptorBytes;
        const auto& slab = geometry.slabs[index];
        storeLe64(bytes + offset, slab.metadata_offset);
        storeLe64(bytes + offset + 8, slab.metadata_bytes);
        storeLe64(bytes + offset + 16, slab.zone_offset);
        storeLe64(bytes + offset + 24, slab.zone_bytes);
        storeLe32(
            bytes + kArtBoxPayloadSizesOffset + index * 4,
            kArtBoxPayloadBytes[index]);
    }
    storeLe64(
        bytes + kArtBoxBoxMetadataOffset,
        geometry.box_metadata_offset);
    storeLe64(
        bytes + kArtBoxBoxMetadataBytesOffset,
        geometry.box_metadata_bytes);
    storeLe64(bytes + kArtBoxBoxDataOffset, geometry.box_data_offset);
    storeLe64(bytes + kArtBoxBoxDataBytesOffset, geometry.box_data_bytes);
    storeLe64(
        bytes + kArtBoxImmutableHashOffset,
        artBoxImmutableHash(geometry));
    storeLe32(
        bytes + kArtBoxNodeCapacityOffset,
        geometry.node_capacity);
    bytes[kArtBoxHeaderWidthOffset] = geometry.header_width;

    const auto clear_gap = [&impl](
                               std::uint64_t first,
                               std::uint64_t last) {
        if (last > first) {
            std::memset(
                impl.base + first,
                0,
                static_cast<std::size_t>(last - first));
        }
    };
    clear_gap(
        checkedAdd(geometry.header_offset, kArtBoxHeaderBytes),
        geometry.slabs.front().metadata_offset);
    try {
        const auto width = static_cast<FixedBlockHeaderWidth>(
            geometry.header_width);
        for (std::size_t index = 0; index < geometry.slabs.size(); ++index) {
            const auto& slab = geometry.slabs[index];
            static_cast<void>(FixedBlockAllocator::Initialize(
                impl.base + slab.metadata_offset,
                static_cast<std::size_t>(slab.metadata_bytes),
                impl.base + slab.zone_offset,
                static_cast<std::size_t>(slab.zone_bytes),
                kArtBoxPayloadBytes[index],
                width));
            const auto end = checkedAdd(slab.zone_offset, slab.zone_bytes);
            const auto next = index + 1 < geometry.slabs.size()
                ? geometry.slabs[index + 1].metadata_offset
                : geometry.box_metadata_offset;
            clear_gap(end, next);
        }
        static_cast<void>(BoxAllocator::Initialize(
            impl.base + geometry.box_metadata_offset,
            static_cast<std::size_t>(geometry.box_metadata_bytes),
            geometry.box_data_bytes));
    } catch (const AllocatorError& error) {
        if (error.Code() == AllocatorErrorCode::Capacity ||
            error.Code() == AllocatorErrorCode::InvalidArgument) {
            throw ErrCapacity(error.what());
        }
        throw ErrCorruptRegion(error.what());
    }
}

void initializeTrieBoxSections(
    Region::Impl& impl,
    const Layout& layout) {
    TrieBoxGeometry geometry;
    geometry.header_offset = layout.trie_header_offset;
    geometry.node_metadata_offset = layout.trie_node_metadata_offset;
    geometry.node_metadata_bytes = layout.trie_node_metadata_bytes;
    geometry.node_zone_offset = layout.trie_node_zone_offset;
    geometry.node_zone_bytes = layout.trie_node_zone_bytes;
    geometry.box_metadata_offset = layout.trie_box_metadata_offset;
    geometry.box_metadata_bytes = layout.trie_box_metadata_bytes;
    geometry.box_data_offset = layout.trie_box_data_offset;
    geometry.box_data_bytes = layout.trie_box_data_bytes;

    auto* bytes = impl.base + geometry.header_offset;
    std::memset(bytes, 0, static_cast<std::size_t>(kTrieBoxHeaderBytes));
    std::copy(kTrieBoxMagic.begin(), kTrieBoxMagic.end(), bytes);
    storeLe32(bytes + 8, kTrieBoxVersion);
    storeLe32(
        bytes + 12, static_cast<std::uint32_t>(kTrieBoxHeaderBytes));
    storeLe32(bytes + kTrieRootOffset, UINT32_MAX);
    storeLe64(bytes + kTrieNodeMetadataOffset,
              geometry.node_metadata_offset);
    storeLe64(bytes + kTrieNodeMetadataBytesOffset,
              geometry.node_metadata_bytes);
    storeLe64(bytes + kTrieNodeZoneOffset, geometry.node_zone_offset);
    storeLe64(bytes + kTrieNodeZoneBytesOffset, geometry.node_zone_bytes);
    storeLe64(bytes + kTrieBoxMetadataOffset,
              geometry.box_metadata_offset);
    storeLe64(bytes + kTrieBoxMetadataBytesOffset,
              geometry.box_metadata_bytes);
    storeLe64(bytes + kTrieBoxDataOffset, geometry.box_data_offset);
    storeLe64(bytes + kTrieBoxDataBytesOffset, geometry.box_data_bytes);
    storeLe64(bytes + kTrieImmutableHashOffset,
              trieBoxImmutableHash(geometry));

    const auto header_end = checkedAdd(
        geometry.header_offset, kTrieBoxHeaderBytes);
    if (geometry.node_metadata_offset > header_end) {
        std::memset(
            impl.base + header_end,
            0,
            static_cast<std::size_t>(
                geometry.node_metadata_offset - header_end));
    }

    try {
        static_cast<void>(BoxAllocator::Initialize(
            impl.base + geometry.box_metadata_offset,
            static_cast<std::size_t>(geometry.box_metadata_bytes),
            geometry.box_data_bytes));
        auto nodes = TrieNodeStore::Initialize(
            impl.base + geometry.node_metadata_offset,
            static_cast<std::size_t>(geometry.node_metadata_bytes),
            impl.base + geometry.node_zone_offset,
            static_cast<std::size_t>(geometry.node_zone_bytes));
        ByteTrieIndex index(nodes);
        const auto initial_root = index.CreateInitialRoot();
        static_cast<void>(ByteTrieIndex::CommittedRootSlot::Initialize(
            bytes + kTrieRootOffset,
            sizeof(std::uint32_t),
            initial_root));
    } catch (const AllocatorError& error) {
        if (error.Code() == AllocatorErrorCode::Capacity ||
            error.Code() == AllocatorErrorCode::InvalidArgument) {
            throw ErrCapacity(error.what());
        }
        throw ErrCorruptRegion(error.what());
    }
}

void initializeRegion(Region::Impl& impl, const Layout& layout) {
    const auto backing_size = layout.max_size;
    if (::ftruncate(impl.fd, static_cast<off_t>(backing_size)) != 0) {
        throw Error(systemError("ftruncate"));
    }
    void* mapped = ::mmap(nullptr, static_cast<std::size_t>(layout.max_size),
                          PROT_READ | PROT_WRITE, MAP_SHARED, impl.fd, 0);
    if (mapped == MAP_FAILED) throw Error(systemError("mmap"));
    impl.base = static_cast<std::uint8_t*>(mapped);
    impl.mapped_size = layout.max_size;
    StartImplicitLifetimes(
        impl.base, static_cast<std::size_t>(impl.mapped_size));
    impl.header = reinterpret_cast<RegionHeader*>(impl.base);
    // Fixed engines initialize their own tail headers and allocator metadata.
    // Preserve sparse allocator data and unowned terminal tails.
    const auto clear_bytes = std::min(layout.initial_size, layout.engine_offset);
    std::memset(impl.base, 0, static_cast<std::size_t>(clear_bytes));

    auto* header = impl.header;
    std::memcpy(header->prefix.magic, kMagic.data(), kMagic.size());
    header->prefix.version = kVersion;
    header->prefix.endian = kEndianMarker;
    header->prefix.header_size = sizeof(RegionHeader);
    header->prefix.engine_id = static_cast<std::uint32_t>(layout.engine);
    header->prefix.engine_abi = engineAbi(layout.engine);
    header->prefix.common_layout_hash = commonLayoutHash();
    header->prefix.engine_layout_hash = engineLayoutHash(layout.engine);
    header->prefix.region_size = backing_size;
    header->prefix.region_max = layout.max_size;
    header->page_size = layout.page_size;
    header->entry_limit = layout.entry_limit;
    header->table_capacity = layout.table_capacity;
    header->table_offset[0] = layout.table_offset[0];
    header->table_offset[1] = layout.table_offset[1];
    header->queue_limit = layout.queue_limit;
    header->queue_capacity = layout.queue_capacity;
    header->queue_offset = layout.queue_offset;
    header->heap_offset = layout.heap_offset;
    header->heap_top = layout.heap_offset;
    header->heap_limit = layout.heap_limit;
    header->engine_offset = layout.engine_offset;
    header->engine_size = layout.engine_size;
    if (layout.engine == ShmEngine::ArtBump) {
        initializeArtBumpSections(impl, layout);
    } else if (layout.engine == ShmEngine::HashBox) {
        initializeHashBoxSections(impl, layout);
    } else if (layout.engine == ShmEngine::ArtBox) {
        initializeArtBoxSections(impl, layout);
    } else if (layout.engine == ShmEngine::TrieBox) {
        initializeTrieBoxSections(impl, layout);
    }

    pthread_mutexattr_t mutex_attr;
    if (::pthread_mutexattr_init(&mutex_attr) != 0) {
        throw Error("kvspace: pthread_mutexattr_init failed");
    }
    if (::pthread_mutexattr_setpshared(&mutex_attr, PTHREAD_PROCESS_SHARED) != 0 ||
        ::pthread_mutexattr_setrobust(&mutex_attr, PTHREAD_MUTEX_ROBUST) != 0 ||
        ::pthread_mutex_init(&header->mutex, &mutex_attr) != 0) {
        ::pthread_mutexattr_destroy(&mutex_attr);
        throw Error("kvspace: cannot initialize process-shared robust mutex");
    }
    ::pthread_mutexattr_destroy(&mutex_attr);

    pthread_condattr_t cond_attr;
    if (::pthread_condattr_init(&cond_attr) != 0) {
        throw Error("kvspace: pthread_condattr_init failed");
    }
    if (::pthread_condattr_setpshared(&cond_attr, PTHREAD_PROCESS_SHARED) != 0 ||
        ::pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC) != 0 ||
        ::pthread_cond_init(&header->notify_cond, &cond_attr) != 0) {
        ::pthread_condattr_destroy(&cond_attr);
        throw Error("kvspace: cannot initialize process-shared condition variable");
    }
    ::pthread_condattr_destroy(&cond_attr);
    impl.engine = makeEngine(layout.engine, &impl);
    atomicStoreLe(&header->prefix.init_state, kInitReady, __ATOMIC_RELEASE);
}

void mapExisting(
    Region::Impl& impl,
    std::optional<ShmEngine> expected_engine) {
    struct stat status {};
    if (::fstat(impl.fd, &status) != 0) throw Error(systemError("fstat"));
    if (status.st_size < 0 ||
        static_cast<std::uint64_t>(status.st_size) < sizeof(RegionPrefix)) {
        throw ErrCorruptRegion("truncated region header");
    }
    std::array<std::uint8_t, sizeof(RegionPrefix)> prefix_bytes{};
    const auto count = ::pread(
        impl.fd, prefix_bytes.data(), prefix_bytes.size(), 0);
    if (count != static_cast<ssize_t>(prefix_bytes.size())) {
        throw ErrCorruptRegion("truncated region header");
    }
    const auto prefix = decodePrefixBytes(prefix_bytes.data());
    if (std::memcmp(prefix.magic, kMagic.data(), kMagic.size()) != 0) {
        throw ErrCorruptRegion("bad magic");
    }
    if (prefix.version != kVersion || prefix.endian != kEndianMarker ||
        prefix.common_layout_hash != commonLayoutHash()) {
        throw ErrVersionMismatch("incompatible shared memory layout");
    }
    if (prefix.init_state != kInitReady) {
        throw ErrCorruptRegion("region initialization is incomplete");
    }
    const auto engine = decodeEngine(prefix.engine_id);
    if (expected_engine.has_value() && *expected_engine != engine) {
        throw ErrEngineMismatch(
            "expected " + std::to_string(static_cast<std::uint32_t>(*expected_engine)) +
            ", found " + std::to_string(prefix.engine_id));
    }
    requireImplementedEngine(engine);
    if (prefix.engine_abi != engineAbi(engine) ||
        prefix.engine_layout_hash != engineLayoutHash(engine) ||
        prefix.header_size != sizeof(RegionHeader)) {
        throw ErrVersionMismatch("incompatible engine layout");
    }
    if (prefix.region_size != prefix.region_max ||
        prefix.region_max < sizeof(RegionHeader) ||
        prefix.region_max > static_cast<std::uint64_t>(
            std::numeric_limits<std::size_t>::max()) ||
        prefix.region_max > static_cast<std::uint64_t>(
            std::numeric_limits<off_t>::max())) {
        throw ErrCorruptRegion("invalid maximum mapping size");
    }
    if (static_cast<std::uint64_t>(status.st_size) < sizeof(RegionHeader)) {
        throw ErrCorruptRegion("truncated shared region");
    }

    // Read immutable common geometry into bounded volatile storage before a
    // full mapping.  No queue/heap/engine byte is touched until the backing
    // object is proven to cover the accepted fixed region size.
    std::array<std::uint8_t, sizeof(RegionHeader)> header_bytes{};
    const auto header_count = ::pread(
        impl.fd, header_bytes.data(), header_bytes.size(), 0);
    if (header_count != static_cast<ssize_t>(header_bytes.size())) {
        throw ErrCorruptRegion("truncated shared region");
    }
    const auto scalar64 = [&header_bytes](std::size_t offset) {
        return loadLe64(header_bytes.data() + offset);
    };
    const auto page_size = scalar64(offsetof(RegionHeader, page_size));
    const auto entry_limit = scalar64(offsetof(RegionHeader, entry_limit));
    const auto queue_limit = scalar64(offsetof(RegionHeader, queue_limit));
    if (hostPageSize() != kSupportedPageSize ||
        page_size != kSupportedPageSize ||
        entry_limit == 0 || queue_limit == 0) {
        throw ErrCorruptRegion("invalid fixed region geometry");
    }
    CanonicalCommonGeometry canonical{};
    try {
        canonical = rebuildCanonicalCommonGeometry(
            engine,
            page_size,
            entry_limit,
            queue_limit,
            prefix.region_max);
    } catch (const ErrCapacity&) {
        throw ErrCorruptRegion("invalid canonical region geometry");
    }
    if (scalar64(offsetof(RegionHeader, table_capacity)) !=
            canonical.table_capacity ||
        scalar64(offsetof(RegionHeader, table_offset)) !=
            canonical.table_offset[0] ||
        scalar64(offsetof(RegionHeader, table_offset) + sizeof(std::uint64_t)) !=
            canonical.table_offset[1] ||
        scalar64(offsetof(RegionHeader, queue_capacity)) !=
            canonical.queue_capacity ||
        scalar64(offsetof(RegionHeader, queue_offset)) !=
            canonical.queue_offset ||
        scalar64(offsetof(RegionHeader, heap_offset)) !=
            canonical.heap_offset ||
        scalar64(offsetof(RegionHeader, heap_limit)) !=
            canonical.heap_limit ||
        scalar64(offsetof(RegionHeader, engine_offset)) !=
            canonical.engine_offset ||
        scalar64(offsetof(RegionHeader, engine_size)) !=
            canonical.engine_size) {
        throw ErrCorruptRegion("noncanonical persistent region geometry");
    }
    if (static_cast<std::uint64_t>(status.st_size) < prefix.region_max) {
        throw ErrCorruptRegion("truncated fixed shared region");
    }
    void* mapped = ::mmap(nullptr, static_cast<std::size_t>(prefix.region_max),
                          PROT_READ | PROT_WRITE, MAP_SHARED, impl.fd, 0);
    if (mapped == MAP_FAILED) throw Error(systemError("mmap"));
    impl.base = static_cast<std::uint8_t*>(mapped);
    impl.mapped_size = prefix.region_max;
    StartImplicitLifetimes(
        impl.base, static_cast<std::size_t>(impl.mapped_size));
    impl.header = reinterpret_cast<RegionHeader*>(impl.base);
    impl.validateStaticHeader();
    impl.engine = makeEngine(engine, &impl);
}

int openBacking(const std::string& name, bool regular, int flags, unsigned int permissions) {
    const auto mode = static_cast<mode_t>(permissions);
    return regular
        ? ::open(name.c_str(), flags, mode)
        : ::shm_open(name.c_str(), flags, mode);
}

bool backingNameMatchesFd(
    const std::string& name,
    bool regular,
    int fd) {
    struct stat opened_status {};
    if (::fstat(fd, &opened_status) != 0) {
        throw Error(systemError("fstat opened backing"));
    }
    const auto probe = openBacking(name, regular, O_RDWR | O_CLOEXEC, 0);
    if (probe < 0) {
        if (errno == ENOENT) return false;
        throw Error(systemError("reopen shared region"));
    }
    struct stat named_status {};
    if (::fstat(probe, &named_status) != 0) {
        const auto saved_errno = errno;
        ::close(probe);
        errno = saved_errno;
        throw Error(systemError("fstat named backing"));
    }
    ::close(probe);
    return opened_status.st_dev == named_status.st_dev &&
        opened_status.st_ino == named_status.st_ino;
}

} // namespace

void SetRegionTestHook(RegionTestHook hook) noexcept {
    region_test_hook.store(hook, std::memory_order_release);
}

void SetCommonApplyTestHook(CommonApplyTestHook hook) noexcept {
    common_apply_test_hook.store(hook, std::memory_order_release);
}

void SetArtBumpRegionTestHook(ArtBumpRegionTestHook hook) noexcept {
    art_bump_region_test_hook.store(hook, std::memory_order_release);
}

std::size_t RegionAllocatorJournalOffsetForTest() noexcept {
    return offsetof(RegionHeader, allocator_journal);
}

std::size_t RegionQueueCapacityOffsetForTest() noexcept {
    return offsetof(RegionHeader, queue_capacity);
}

std::size_t RegionQueueTableOffsetOffsetForTest() noexcept {
    return offsetof(RegionHeader, queue_offset);
}

Region::Region(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<Region> Region::Open(
    const std::string& requested_name,
    const ShmOptions& options,
    OpenMode mode,
    std::optional<ShmEngine> expected_engine) {
    const bool regular = isRegularPath(requested_name);
    const auto name = regular ? requested_name : shmName(requested_name);
    const int base_flags = O_RDWR | O_CLOEXEC;
    std::optional<Layout> create_layout;
    if (mode != OpenMode::Attach) {
        if (expected_engine.has_value() && *expected_engine != options.engine) {
            throw ErrEngineMismatch("create options disagree with expected engine");
        }
        create_layout = makeLayout(options);
    }

    for (std::size_t attempt = 0; attempt < 64; ++attempt) {
        auto impl = std::make_unique<Impl>();
        impl->regular_file = regular;
        impl->name = name;
        bool created = false;
        if (mode == OpenMode::Create) {
            impl->fd = openBacking(
                name, regular,
                base_flags | O_CREAT | O_EXCL, options.permissions);
            created = impl->fd >= 0;
        } else if (mode == OpenMode::Open) {
            impl->fd = openBacking(
                name, regular,
                base_flags | O_CREAT | O_EXCL, options.permissions);
            if (impl->fd >= 0) {
                created = true;
            } else if (errno == EEXIST) {
                impl->fd = openBacking(
                    name, regular, base_flags, options.permissions);
            }
        } else {
            impl->fd = openBacking(
                name, regular, base_flags, options.permissions);
        }
        if (impl->fd < 0) {
            if (errno == EEXIST) throw ErrAlreadyExists(name);
            if (errno == ENOENT) throw ErrNotFound(name);
            throw Error(systemError("open shared region"));
        }
        emitRegionTestEvent(RegionTestEvent::BackingOpenedBeforeLock);
        if (::flock(impl->fd, LOCK_EX) != 0) {
            throw Error(systemError("flock"));
        }
        try {
            if (!backingNameMatchesFd(name, regular, impl->fd)) {
                if (::flock(impl->fd, LOCK_UN) != 0) {
                    throw Error(systemError("flock unlock"));
                }
                if (mode == OpenMode::Create) {
                    throw ErrNotFound("backing name changed during create");
                }
                continue;
            }

            struct stat status {};
            if (::fstat(impl->fd, &status) != 0) {
                throw Error(systemError("fstat"));
            }
            bool initialize = created;
            if (!initialize && mode == OpenMode::Open &&
                incompleteBacking(impl->fd, status.st_size)) {
                initialize = true;
            }
            if (!initialize && status.st_size == 0) {
                throw ErrCorruptRegion("empty region");
            }
            if (initialize) {
                initializeRegion(*impl, *create_layout);
            } else {
                mapExisting(*impl, expected_engine);
            }

            impl->lock();
            try {
                impl->validateHeader();
                static_cast<void>(impl->prepareCommonClear());
                impl->engine->Validate();
                impl->unlock();
            } catch (...) {
                impl->unlock();
                throw;
            }

            if (!backingNameMatchesFd(name, regular, impl->fd)) {
                if (::flock(impl->fd, LOCK_UN) != 0) {
                    throw Error(systemError("flock unlock"));
                }
                if (mode == OpenMode::Create) {
                    throw ErrNotFound("backing name changed during create");
                }
                continue;
            }
            if (::flock(impl->fd, LOCK_UN) != 0) {
                throw Error(systemError("flock unlock"));
            }
            return std::unique_ptr<Region>(new Region(std::move(impl)));
        } catch (...) {
            ::flock(impl->fd, LOCK_UN);
            throw;
        }
    }
    throw Error("kvspace: backing name changed too often while opening");
}

void Region::Destroy(const std::string& requested_name) {
    const bool regular = isRegularPath(requested_name);
    const auto name = regular ? requested_name : shmName(requested_name);
    const auto fd = openBacking(name, regular, O_RDWR | O_CLOEXEC, 0);
    if (fd < 0) {
        if (errno == ENOENT) return;
        throw Error(systemError("open shared region for destroy"));
    }
    if (::flock(fd, LOCK_EX) != 0) {
        const auto saved_errno = errno;
        ::close(fd);
        errno = saved_errno;
        throw Error(systemError("flock destroy"));
    }
    try {
        if (backingNameMatchesFd(name, regular, fd)) {
            const int rc = regular
                ? ::unlink(name.c_str())
                : ::shm_unlink(name.c_str());
            if (rc != 0 && errno != ENOENT) {
                throw Error(systemError("destroy shared region"));
            }
        }
        ::flock(fd, LOCK_UN);
        ::close(fd);
    } catch (...) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        throw;
    }
}

Region::~Region() = default;

Region::Guard::~Guard() {
    if (region_ != nullptr) region_->Unlock();
}
Region::Guard::Guard(Guard&& other) noexcept : region_(other.region_) {
    other.region_ = nullptr;
}
Region::Guard& Region::Guard::operator=(Guard&& other) noexcept {
    if (this != &other) {
        if (region_ != nullptr) region_->Unlock();
        region_ = other.region_;
        other.region_ = nullptr;
    }
    return *this;
}

Region::Mutation::~Mutation() {
    if (region_ != nullptr) region_->RollbackMutation();
}
Region::Mutation::Mutation(Mutation&& other) noexcept : region_(other.region_) {
    other.region_ = nullptr;
}
Region::Mutation& Region::Mutation::operator=(Mutation&& other) noexcept {
    if (this != &other) {
        if (region_ != nullptr) region_->RollbackMutation();
        region_ = other.region_;
        other.region_ = nullptr;
    }
    return *this;
}
void Region::Mutation::Commit() {
    if (region_ == nullptr) throw Error("kvspace: inactive mutation");
    region_->CommitMutation();
    region_ = nullptr;
}

Region::Guard Region::Lock() {
    impl_->lock();
    return Guard(this);
}

Region::Mutation Region::BeginMutation() {
    impl_->engine->BeginMutation();
    return Mutation(this);
}

void Region::Unlock() noexcept { impl_->unlock(); }

void Region::CommitMutation() { impl_->engine->CommitMutation(); }

void Region::RollbackMutation() noexcept { impl_->engine->RollbackMutation(); }

void Region::Close() {
    if (impl_ == nullptr || impl_->closed) return;
    impl_->closeNoThrow();
}

bool Region::Get(std::string_view key, std::vector<std::uint8_t>* value) const {
    return impl_->engine->Get(key, value);
}
bool Region::Exists(std::string_view key) const { return impl_->engine->Exists(key); }
void Region::Put(std::string_view key, const std::vector<std::uint8_t>& value) {
    impl_->engine->Put(key, value);
}
bool Region::Erase(std::string_view key) { return impl_->engine->Erase(key); }
std::vector<Region::Entry> Region::Entries() const { return impl_->engine->Entries(); }
std::vector<Region::Entry> Region::EntriesWithPrefix(std::string_view prefix) const {
    return impl_->engine->EntriesWithPrefix(prefix);
}

void Region::Clear() {
    auto common_plan = impl_->prepareCommonClear();
    auto engine_plan = impl_->engine->PrepareClear();
    impl_->applyCommonClear(common_plan);
    std::move(*engine_plan).Apply();
}

void Region::CompactValues() { impl_->engine->Compact(); }

void Region::Notify(std::string_view key, const std::vector<std::uint8_t>& value) {
    impl_->queuePush(key, value);
}

bool Region::Watch(
    Guard& guard,
    std::string_view key,
    std::chrono::milliseconds timeout,
    std::vector<std::uint8_t>* value,
    const std::function<bool()>& cancelled) {
    if (guard.region_ != this) throw Error("kvspace: invalid region guard");
    timespec deadline{};
    const timespec* deadline_ptr = nullptr;
    if (timeout.count() > 0) {
        if (::clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
            throw Error(systemError("clock_gettime"));
        }
        const auto seconds = timeout.count() / 1000;
        const auto nanos = (timeout.count() % 1000) * 1000000;
        deadline.tv_sec += static_cast<time_t>(seconds);
        deadline.tv_nsec += static_cast<long>(nanos);
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
        deadline_ptr = &deadline;
    }
    for (;;) {
        if (impl_->queuePop(key, value)) return true;
        if (cancelled()) return false;
        bool mutex_owned = true;
        int rc = 0;
        try {
            rc = impl_->wait(deadline_ptr, &mutex_owned);
        } catch (...) {
            if (!mutex_owned) guard.region_ = nullptr;
            throw;
        }
        if (rc == ETIMEDOUT) {
            return impl_->queuePop(key, value);
        }
        if (rc != 0) throw Error("kvspace: pthread_cond_wait failed");
        if (impl_->closed) throw ErrDisconnected();
    }
}

void Region::WakeAll() {
    auto guard = Lock();
    const int rc = ::pthread_cond_broadcast(&impl_->header->notify_cond);
    if (rc != 0) throw Error("kvspace: pthread_cond_broadcast failed");
}

ShmStats Region::Stats() const {
    ShmStats stats{};
    stats.engine = impl_->engine->Id();
    stats.region_size = impl_->header->prefix.region_size;
    stats.region_max = impl_->header->prefix.region_max;
    stats.entries = impl_->header->entry_count;
    stats.engine_nodes = impl_->header->node_count;
    stats.max_entries = impl_->header->entry_limit;
    stats.tombstones = impl_->header->tombstone_count;
    stats.queues = impl_->header->queue_count;
    stats.heap_used = impl_->header->heap_top - impl_->header->heap_offset;
    stats.heap_used = checkedAdd(
        stats.heap_used, impl_->header->engine_live_bytes);
    for (auto offset = impl_->header->heap_offset; offset < impl_->header->heap_top;) {
        auto* block = impl_->blob(offset, false);
        if (block->flags == 0) stats.heap_free += block->span;
        offset += block->span;
    }
    if (impl_->storedEngine() == ShmEngine::ArtBump) {
        const auto* art_bump = dynamic_cast<const ArtBumpEngineV4*>(
            impl_->engine.get());
        if (art_bump == nullptr ||
            stats.heap_used < impl_->header->engine_live_bytes) {
            throw ErrCorruptRegion("invalid ArtBump statistics state");
        }
        stats.heap_used -= impl_->header->engine_live_bytes;
        stats.heap_used = checkedAdd(
            stats.heap_used,
            art_bump->PhysicalUsedBytes());
    }
    stats.recoveries = impl_->header->recovery_count;
    return stats;
}

} // namespace kvspace::detail
