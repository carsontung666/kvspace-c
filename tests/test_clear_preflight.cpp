#include "test_support.h"

#include "kvspace/errors.h"
#include "kvspace/xvalue.h"
#include "shm_art_box_store.h"
#include "shm_art_bump_store.h"
#include "shm_region.h"
#include "shm_trie_node_store.h"

#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_set>
#include <utility>
#include <vector>

namespace allocation_failure_injection {

thread_local std::ptrdiff_t countdown = -1;
thread_local std::size_t calls = 0;
thread_local std::size_t bytes_requested = 0;
thread_local bool triggered = false;

void arm(std::size_t successful_allocations) noexcept {
    countdown = static_cast<std::ptrdiff_t>(successful_allocations);
    calls = 0;
    bytes_requested = 0;
    triggered = false;
}

void disable() noexcept { countdown = -1; }

void* allocate(std::size_t size) {
    ++calls;
    if (size <= std::numeric_limits<std::size_t>::max() - bytes_requested) {
        bytes_requested += size;
    } else {
        bytes_requested = std::numeric_limits<std::size_t>::max();
    }
    if (countdown == 0) {
        countdown = -1;
        triggered = true;
        throw std::bad_alloc();
    }
    if (countdown > 0) --countdown;
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

constexpr std::uint64_t kRegionTableCapacityOffset = 80;
constexpr std::uint64_t kRegionTableOffsetsOffset = 88;
constexpr std::uint64_t kRegionQueueCountOffset = 160;
constexpr std::uint64_t kRegionEngineOffset = 200;
constexpr std::uint64_t kRegionActiveTableOffset = 1360;

constexpr std::uint64_t kArtBumpCommittedRootOffset = 16;
constexpr std::uint64_t kArtBoxCommittedRootOffset = 16;
constexpr std::uint64_t kArtBoxSlabsOffset = 24;
constexpr std::uint64_t kArtBoxHeaderWidthOffset = 196;
constexpr std::uint64_t kHashBoxDataOffset = 32;
constexpr std::uint64_t kTrieCommittedRootOffset = 16;
constexpr std::uint64_t kTrieNodeMetadataOffset = 24;
constexpr std::uint64_t kTrieNodeZoneOffset = 40;
constexpr std::uint64_t kFixedBlockHeaderWidthOffset = 56;

constexpr std::uint64_t kHashBoxSlotBytes = 32;
constexpr std::uint64_t kSlotValueReferenceOffset = 16;
constexpr std::uint64_t kSlotStateOffset = 28;
constexpr std::uint32_t kOccupied = 1;
constexpr std::uint64_t kNodeKindOffset = 22;

constexpr std::string_view kDeepTrieKey = "/zzzz/deep/trie/final";

struct AllocationTrace {
    std::size_t calls = 0;
    std::size_t bytes = 0;
};

kvspace::ShmOptions options(kvspace::ShmEngine engine) {
    kvspace::ShmOptions result;
    result.initial_size = 128U * 1024U;
    result.max_size = 8U * 1024U * 1024U;
    result.max_entries = 64;
    result.max_queues = 16;
    result.engine = engine;
    return result;
}

std::string engineName(kvspace::ShmEngine engine) {
    switch (engine) {
    case kvspace::ShmEngine::ArtBump:
        return "art_bump";
    case kvspace::ShmEngine::ArtBox:
        return "art_box";
    case kvspace::ShmEngine::HashBox:
        return "hash_box";
    case kvspace::ShmEngine::TrieBox:
        return "trie_box";
    }
    CHECK(false);
    return "unknown";
}

std::string pathFor(
    kvspace::ShmEngine engine,
    std::string_view purpose) {
    return "/tmp/kvspace_clear_preflight_" + engineName(engine) + "_" +
        std::string(purpose) + "_" +
        std::to_string(static_cast<long long>(::getpid())) + ".kvshm";
}

std::uint32_t loadLe32(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t offset) {
    CHECK(offset <= static_cast<std::uint64_t>(bytes.size()));
    CHECK(4U <= static_cast<std::uint64_t>(bytes.size()) - offset);
    std::uint32_t result = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        result |= static_cast<std::uint32_t>(
            bytes[static_cast<std::size_t>(offset) + index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return result;
}

std::uint64_t loadLe64(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t offset) {
    CHECK(offset <= static_cast<std::uint64_t>(bytes.size()));
    CHECK(8U <= static_cast<std::uint64_t>(bytes.size()) - offset);
    std::uint64_t result = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        result |= static_cast<std::uint64_t>(
            bytes[static_cast<std::size_t>(offset) + index]) <<
            static_cast<unsigned>(index * 8U);
    }
    return result;
}

std::vector<std::uint8_t> readPersistentBytes(const std::string& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    CHECK(descriptor >= 0);
    struct stat status {};
    CHECK(::fstat(descriptor, &status) == 0);
    CHECK(status.st_size >= 0);
    const auto size = static_cast<std::uint64_t>(status.st_size);
    CHECK(size <= std::numeric_limits<std::size_t>::max());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        CHECK(bytes.size() <= static_cast<std::size_t>(SSIZE_MAX));
        CHECK(::pread(descriptor, bytes.data(), bytes.size(), 0) ==
              static_cast<ssize_t>(bytes.size()));
    }
    CHECK(::close(descriptor) == 0);
    return bytes;
}

void writePersistentByte(
    const std::string& path,
    std::uint64_t offset,
    std::uint8_t value) {
    CHECK(offset <= static_cast<std::uint64_t>(
                        std::numeric_limits<off_t>::max()));
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(descriptor >= 0);
    CHECK(::pwrite(
              descriptor,
              &value,
              sizeof(value),
              static_cast<off_t>(offset)) ==
          static_cast<ssize_t>(sizeof(value)));
    CHECK(::close(descriptor) == 0);
}

void writePersistentLe64(
    const std::string& path,
    std::uint64_t offset,
    std::uint64_t value) {
    std::array<std::uint8_t, 8> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        encoded[index] = static_cast<std::uint8_t>(
            value >> static_cast<unsigned>(index * 8U));
    }
    CHECK(offset <= static_cast<std::uint64_t>(
                        std::numeric_limits<off_t>::max()));
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(descriptor >= 0);
    CHECK(::pwrite(
              descriptor,
              encoded.data(),
              encoded.size(),
              static_cast<off_t>(offset)) ==
          static_cast<ssize_t>(encoded.size()));
    CHECK(::close(descriptor) == 0);
}

void populateQueues(kvspace::detail::Region* region) {
    const auto first = kvspace::XValue::Str("first-message").Encode();
    const auto second = kvspace::XValue::Str("second-message").Encode();
    for (const std::string_view queue : {
             std::string_view("queue-a"),
             std::string_view("queue-b"),
             std::string_view("queue-c")}) {
        region->Notify(queue, first);
        region->Notify(queue, second);
    }
}

void populateEngine(kvspace::detail::Region* region) {
    constexpr std::array<std::string_view, 7> keys = {
        "/alpha/branch/one/final",
        "/alpha/branch/two/final",
        "/alpha/other/two/final",
        "/beta/branch/one/final",
        "/beta/branch/two/final",
        kDeepTrieKey,
        "/zzzz/deep/trie/sibling"};
    std::size_t ordinal = 0;
    for (const auto key : keys) {
        const auto value = kvspace::XValue::Str(
            "engine-value-" + std::to_string(ordinal++)).Encode();
        region->Put(key, value);
    }
}

std::uint64_t artBoxNodeOffset(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t engine_offset,
    std::uint32_t reference) {
    CHECK(reference != kvspace::detail::ArtBoxNodeRefCodec::kEmpty);
    const auto kind_index = static_cast<std::uint64_t>(reference >> 30U);
    CHECK(kind_index < 4);
    const auto local_id = static_cast<std::uint64_t>(
        reference & UINT32_C(0x3fffffff));
    const auto descriptor = engine_offset + kArtBoxSlabsOffset +
        kind_index * 32U;
    const auto zone_offset = loadLe64(bytes, descriptor + 16U);
    const auto header_width = static_cast<std::uint64_t>(
        bytes[static_cast<std::size_t>(
            engine_offset + kArtBoxHeaderWidthOffset)]);
    const auto payload = static_cast<std::uint64_t>(
        kvspace::detail::ArtBoxNodeCodec::kPayloadBytes[
            static_cast<std::size_t>(kind_index)]);
    const auto stride = header_width + payload;
    CHECK(local_id <=
          (std::numeric_limits<std::uint64_t>::max() - zone_offset) /
              stride);
    const auto offset = zone_offset + local_id * stride + header_width;
    CHECK(offset <= static_cast<std::uint64_t>(bytes.size()));
    CHECK(payload <= static_cast<std::uint64_t>(bytes.size()) - offset);
    return offset;
}

void corruptDeepArtBoxObject(
    const std::string& path,
    const std::vector<std::uint8_t>& bytes) {
    const auto engine_offset = loadLe64(bytes, kRegionEngineOffset);
    const auto root = loadLe32(
        bytes, engine_offset + kArtBoxCommittedRootOffset);
    CHECK(root != kvspace::detail::ArtBoxNodeRefCodec::kEmpty);

    std::vector<std::pair<std::uint32_t, std::size_t>> pending{{root, 0}};
    std::unordered_set<std::uint32_t> visited;
    auto deepest = root;
    std::size_t deepest_depth = 0;
    while (!pending.empty()) {
        const auto [reference, depth] = pending.back();
        pending.pop_back();
        CHECK(visited.insert(reference).second);
        if (depth >= deepest_depth) {
            deepest = reference;
            deepest_depth = depth;
        }
        const auto kind = kvspace::detail::ArtBoxNodeRefCodec::Kind(reference);
        const auto payload =
            kvspace::detail::ArtBoxNodeCodec::PayloadBytes(kind);
        const auto offset = artBoxNodeOffset(bytes, engine_offset, reference);
        const auto node = kvspace::detail::ArtBoxNodeCodec::Decode(
            bytes.data() + static_cast<std::size_t>(offset), payload);
        for (const auto child : node.children) {
            if (child != kvspace::detail::ArtBoxNodeRecord::kEmptyChild) {
                pending.emplace_back(child, depth + 1U);
            }
        }
    }
    CHECK(deepest_depth >= 2);
    writePersistentByte(
        path,
        artBoxNodeOffset(bytes, engine_offset, deepest) + kNodeKindOffset,
        0);
}

void corruptDeepArtBumpObject(
    const std::string& path,
    const std::vector<std::uint8_t>& bytes) {
    const auto engine_offset = loadLe64(bytes, kRegionEngineOffset);
    const auto root = loadLe64(
        bytes, engine_offset + kArtBumpCommittedRootOffset);
    CHECK(root != 0);
    std::vector<std::pair<std::uint64_t, std::size_t>> pending{{root, 0}};
    std::unordered_set<std::uint64_t> visited;
    auto deepest = root;
    std::size_t deepest_depth = 0;
    while (!pending.empty()) {
        const auto [reference, depth] = pending.back();
        pending.pop_back();
        CHECK(visited.insert(reference).second);
        CHECK(reference <= static_cast<std::uint64_t>(bytes.size()));
        CHECK(kNodeKindOffset <
              static_cast<std::uint64_t>(bytes.size()) - reference);
        const auto raw_kind = bytes[static_cast<std::size_t>(
            reference + kNodeKindOffset)];
        CHECK(raw_kind >= 1 && raw_kind <= 4);
        const auto kind = static_cast<kvspace::detail::ArtBumpNodeKind>(
            raw_kind);
        const auto payload =
            kvspace::detail::ArtBumpNodeCodec::PayloadBytes(kind);
        CHECK(payload <= static_cast<std::uint64_t>(bytes.size()) - reference);
        const auto node = kvspace::detail::ArtBumpNodeCodec::Decode(
            bytes.data() + static_cast<std::size_t>(reference), payload);
        if (depth >= deepest_depth) {
            deepest = reference;
            deepest_depth = depth;
        }
        for (const auto child : node.children) {
            if (child != kvspace::detail::ArtBumpNodeRecord::kEmptyChild) {
                pending.emplace_back(child, depth + 1U);
            }
        }
    }
    CHECK(deepest_depth >= 2);
    writePersistentByte(path, deepest + kNodeKindOffset, 0);
}

void corruptLastHashBoxValue(
    const std::string& path,
    const std::vector<std::uint8_t>& bytes) {
    const auto capacity = loadLe64(bytes, kRegionTableCapacityOffset);
    const auto active = loadLe32(bytes, kRegionActiveTableOffset);
    CHECK(active <= 1);
    const auto table_offset = loadLe64(
        bytes, kRegionTableOffsetsOffset + static_cast<std::uint64_t>(active) * 8U);
    std::uint64_t last_slot = capacity;
    for (std::uint64_t index = 0; index < capacity; ++index) {
        const auto slot = table_offset + index * kHashBoxSlotBytes;
        if (loadLe32(bytes, slot + kSlotStateOffset) == kOccupied) {
            last_slot = index;
        }
    }
    CHECK(last_slot != capacity);
    const auto slot = table_offset + last_slot * kHashBoxSlotBytes;
    const auto value_reference = loadLe64(
        bytes, slot + kSlotValueReferenceOffset);
    CHECK(value_reference != 0);
    const auto engine_offset = loadLe64(bytes, kRegionEngineOffset);
    const auto box_data_offset = loadLe64(
        bytes, engine_offset + kHashBoxDataOffset);
    const auto value_offset = box_data_offset + value_reference - 1U;
    CHECK(value_offset < static_cast<std::uint64_t>(bytes.size()));
    writePersistentByte(path, value_offset, 0);
}

std::uint64_t trieNodeOffset(
    const std::vector<std::uint8_t>& bytes,
    std::uint64_t engine_offset,
    std::uint32_t id) {
    const auto metadata_offset = loadLe64(
        bytes, engine_offset + kTrieNodeMetadataOffset);
    const auto node_zone_offset = loadLe64(
        bytes, engine_offset + kTrieNodeZoneOffset);
    CHECK(metadata_offset + kFixedBlockHeaderWidthOffset <
          static_cast<std::uint64_t>(bytes.size()));
    const auto header_width = static_cast<std::uint64_t>(
        bytes[static_cast<std::size_t>(
            metadata_offset + kFixedBlockHeaderWidthOffset)]);
    const auto payload = static_cast<std::uint64_t>(
        kvspace::detail::TrieNodeCodec::kEncodedBytes);
    const auto stride = header_width + payload;
    CHECK(static_cast<std::uint64_t>(id) <=
          (std::numeric_limits<std::uint64_t>::max() - node_zone_offset) /
              stride);
    const auto offset = node_zone_offset +
        static_cast<std::uint64_t>(id) * stride + header_width;
    CHECK(offset <= static_cast<std::uint64_t>(bytes.size()));
    CHECK(payload <= static_cast<std::uint64_t>(bytes.size()) - offset);
    return offset;
}

void corruptDeepTrieBoxObject(
    const std::string& path,
    const std::vector<std::uint8_t>& bytes) {
    const auto engine_offset = loadLe64(bytes, kRegionEngineOffset);
    auto node_id = loadLe32(
        bytes, engine_offset + kTrieCommittedRootOffset);
    std::size_t depth = 0;
    for (const char edge_byte : kDeepTrieKey) {
        const auto edge = static_cast<unsigned char>(edge_byte);
        const auto offset = trieNodeOffset(bytes, engine_offset, node_id);
        const auto node = kvspace::detail::TrieNodeCodec::Decode(
            bytes.data() + static_cast<std::size_t>(offset),
            kvspace::detail::TrieNodeCodec::kEncodedBytes);
        const auto child = node.children[edge];
        CHECK(child != kvspace::detail::TrieNodeRecord::kEmptyChild);
        node_id = static_cast<std::uint32_t>(child);
        ++depth;
    }
    CHECK(depth == kDeepTrieKey.size());
    const auto leaf_offset = trieNodeOffset(bytes, engine_offset, node_id);
    const auto leaf = kvspace::detail::TrieNodeCodec::Decode(
        bytes.data() + static_cast<std::size_t>(leaf_offset),
        kvspace::detail::TrieNodeCodec::kEncodedBytes);
    CHECK(leaf.has_value);
    // has_value=false,value_ref=1 is a noncanonical node word.  The node is
    // the terminal leaf reached only after the complete long-key path.
    writePersistentLe64(path, leaf_offset, 2);
}

void corruptDeepEngineObject(
    kvspace::ShmEngine engine,
    const std::string& path,
    const std::vector<std::uint8_t>& bytes) {
    switch (engine) {
    case kvspace::ShmEngine::ArtBump:
        corruptDeepArtBumpObject(path, bytes);
        return;
    case kvspace::ShmEngine::ArtBox:
        corruptDeepArtBoxObject(path, bytes);
        return;
    case kvspace::ShmEngine::HashBox:
        corruptLastHashBoxValue(path, bytes);
        return;
    case kvspace::ShmEngine::TrieBox:
        corruptDeepTrieBoxObject(path, bytes);
        return;
    }
    CHECK(false);
}

void testDeepEngineCorruptionLeavesWholeRegionUnchanged(
    kvspace::ShmEngine engine) {
    const auto path = pathFor(engine, "corrupt");
    ScopedRegion scoped(path);
    auto region = kvspace::detail::Region::Open(
        path, options(engine), kvspace::detail::OpenMode::Create);
    auto guard = region->Lock();
    populateQueues(region.get());
    populateEngine(region.get());
    CHECK(region->Stats().entries == 7);
    CHECK(region->Stats().queues == 3);

    const auto valid = readPersistentBytes(path);
    corruptDeepEngineObject(engine, path, valid);
    const auto corrupted = readPersistentBytes(path);
    CHECK(corrupted != valid);
    expectThrows<kvspace::ErrCorruptRegion>([&] { region->Clear(); });
    CHECK(readPersistentBytes(path) == corrupted);
    (void)guard;
}

void testCorruptQueueCountIsRejectedBeforeReserve(
    kvspace::ShmEngine engine) {
    const auto path = pathFor(engine, "queue_count");
    ScopedRegion scoped(path);
    auto region = kvspace::detail::Region::Open(
        path, options(engine), kvspace::detail::OpenMode::Create);
    auto guard = region->Lock();
    writePersistentLe64(
        path,
        kRegionQueueCountOffset,
        std::numeric_limits<std::uint64_t>::max());
    const auto corrupted = readPersistentBytes(path);

    expectThrows<kvspace::ErrCorruptRegion>([&] { region->Clear(); });
    CHECK(readPersistentBytes(path) == corrupted);
    (void)guard;
}

AllocationTrace exhaustClearAllocations(
    kvspace::ShmEngine engine,
    bool with_engine_entries) {
    const auto path = pathFor(
        engine, with_engine_entries ? "allocation_full" : "allocation_common");
    ScopedRegion scoped(path);
    auto region = kvspace::detail::Region::Open(
        path, options(engine), kvspace::detail::OpenMode::Create);
    auto guard = region->Lock();
    populateQueues(region.get());
    if (with_engine_entries) populateEngine(region.get());
    const auto before = readPersistentBytes(path);

    constexpr std::size_t kMaximumAllocationSites = 8192;
    for (std::size_t fail_after = 0;
         fail_after < kMaximumAllocationSites;
         ++fail_after) {
        allocation_failure_injection::arm(fail_after);
        try {
            region->Clear();
            const AllocationTrace trace{
                allocation_failure_injection::calls,
                allocation_failure_injection::bytes_requested};
            const bool triggered = allocation_failure_injection::triggered;
            allocation_failure_injection::disable();
            CHECK(!triggered);
            CHECK(trace.calls == fail_after);
            CHECK(region->Stats().entries == 0);
            CHECK(region->Stats().queues == 0);
            (void)guard;
            return trace;
        } catch (const std::bad_alloc&) {
            const bool triggered = allocation_failure_injection::triggered;
            const auto calls = allocation_failure_injection::calls;
            allocation_failure_injection::disable();
            CHECK(triggered);
            CHECK(calls == fail_after + 1U);
            CHECK(readPersistentBytes(path) == before);
        } catch (...) {
            allocation_failure_injection::disable();
            throw;
        }
    }
    allocation_failure_injection::disable();
    CHECK(false);
    return {};
}

void testEveryScratchAllocationFailsBeforeAnyWrite(
    kvspace::ShmEngine engine) {
    // Identical queue content isolates the selected engine's nonempty graph.
    // A larger allocation trace than the queue-only engine image proves the
    // successful/exhaustive path reached object-dependent engine scratch.
    const auto queue_only = exhaustClearAllocations(engine, false);
    const auto complete = exhaustClearAllocations(engine, true);
    CHECK(complete.calls > queue_only.calls ||
          complete.bytes > queue_only.bytes);
    std::cout << engineName(engine)
              << " Clear allocation sites: queue-only=" << queue_only.calls
              << ", nonempty=" << complete.calls
              << ", queue-only-bytes=" << queue_only.bytes
              << ", nonempty-bytes=" << complete.bytes << '\n';
}

} // namespace

int main() {
    return runTest([] {
        constexpr std::array<kvspace::ShmEngine, 4> engines = {
            kvspace::ShmEngine::ArtBump,
            kvspace::ShmEngine::ArtBox,
            kvspace::ShmEngine::HashBox,
            kvspace::ShmEngine::TrieBox};
        for (const auto engine : engines) {
            testDeepEngineCorruptionLeavesWholeRegionUnchanged(engine);
            testCorruptQueueCountIsRejectedBeforeReserve(engine);
        }
        for (const auto engine : engines) {
            testEveryScratchAllocationFailsBeforeAnyWrite(engine);
        }
    });
}
