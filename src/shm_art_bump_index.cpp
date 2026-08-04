#include "shm_art_bump_index.h"

#include "kvspace/errors.h"
#include "kvspace/xvalue.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace kvspace::detail {
namespace {

using NodeRef = ArtBumpIndex::NodeRef;

struct RebuildCutRelay {
    ArtBumpIndexState* state = nullptr;
    ArtBumpApplyStep step = ArtBumpApplyStep::CompactSlabRebuildWrite;
};

void ReportRebuildCut(
    void* context,
    std::size_t slab_index,
    ArtBumpRebuildCut cut) noexcept {
    auto* relay = static_cast<RebuildCutRelay*>(context);
    relay->state->AfterPersistentStep(
        relay->step,
        static_cast<std::uint64_t>(slab_index) * 3U +
            static_cast<std::uint64_t>(cut));
}

[[noreturn]] void Raise(
    AllocatorErrorCode code,
    const std::string& message) {
    throw AllocatorError(code, "kvspace ArtBump index: " + message);
}

std::size_t KindIndex(
    ArtBumpNodeKind kind,
    AllocatorErrorCode code = AllocatorErrorCode::Corrupt) {
    const auto raw = static_cast<std::uint8_t>(kind);
    if (raw < static_cast<std::uint8_t>(ArtBumpNodeKind::Node4) ||
        raw > static_cast<std::uint8_t>(ArtBumpNodeKind::Node256)) {
        Raise(code, "invalid ART node kind");
    }
    return static_cast<std::size_t>(raw - 1U);
}

bool AddOverflows(std::uint64_t left, std::uint64_t right) noexcept {
    return right > std::numeric_limits<std::uint64_t>::max() - left;
}

std::uint64_t CheckedAdd(
    std::uint64_t left,
    std::uint64_t right,
    AllocatorErrorCode code,
    const char* what) {
    if (AddOverflows(left, right)) Raise(code, what);
    return left + right;
}

std::size_t CheckedSize(std::uint64_t value, const char* what) {
    if (value > std::numeric_limits<std::size_t>::max()) {
        Raise(AllocatorErrorCode::Capacity, what);
    }
    return static_cast<std::size_t>(value);
}

std::uint8_t KeyByte(std::string_view key, std::size_t index) noexcept {
    return static_cast<std::uint8_t>(
        static_cast<unsigned char>(key[index]));
}

std::uint64_t ChildAt(
    const ArtBumpNodeRecord& node,
    std::uint8_t edge) noexcept {
    switch (node.kind) {
    case ArtBumpNodeKind::Node4:
    case ArtBumpNodeKind::Node16:
        for (std::size_t index = 0; index < node.child_count; ++index) {
            if (node.keys[index] == edge) return node.children[index];
        }
        return 0;
    case ArtBumpNodeKind::Node48: {
        const auto slot = node.index[edge];
        return slot == UINT8_MAX ? 0 : node.children[slot];
    }
    case ArtBumpNodeKind::Node256:
        return node.children[edge];
    }
    return 0;
}

struct ChildPair {
    std::uint8_t edge = 0;
    NodeRef reference = 0;
};

struct LogicalNode {
    std::uint64_t prefix_offset = 0;
    std::uint64_t value_offset = 0;
    std::uint32_t prefix_len = 0;
    ArtBumpNodeKind original_kind = ArtBumpNodeKind::Node4;
    bool has_value = false;
    std::vector<ChildPair> children;
};

LogicalNode ToLogical(const ArtBumpNodeRecord& record) {
    LogicalNode result;
    result.prefix_offset = record.prefix_offset;
    result.value_offset = record.value_offset;
    result.prefix_len = record.prefix_len;
    result.original_kind = record.kind;
    result.has_value = record.has_value;
    result.children.reserve(record.child_count);
    for (std::size_t edge = 0; edge < 256; ++edge) {
        const auto reference = ChildAt(
            record, static_cast<std::uint8_t>(edge));
        if (reference == 0) continue;
        result.children.push_back(ChildPair{
            static_cast<std::uint8_t>(edge), reference});
    }
    return result;
}

ArtBumpNodeKind SelectKind(const LogicalNode& node) noexcept {
    if (node.children.size() <= 4U) return ArtBumpNodeKind::Node4;
    if (node.children.size() <= 16U) return ArtBumpNodeKind::Node16;
    if (node.children.size() <= 48U) {
        if (node.original_kind == ArtBumpNodeKind::Node256 &&
            node.children.size() > 37U) {
            return ArtBumpNodeKind::Node256;
        }
        return ArtBumpNodeKind::Node48;
    }
    return ArtBumpNodeKind::Node256;
}

ArtBumpNodeRecord ToRecord(const LogicalNode& logical) {
    if (logical.children.size() > 256U) {
        Raise(AllocatorErrorCode::Corrupt, "ART node has too many children");
    }
    ArtBumpNodeRecord record;
    record.prefix_offset = logical.prefix_offset;
    record.value_offset = logical.value_offset;
    record.prefix_len = logical.prefix_len;
    record.child_count = static_cast<std::uint16_t>(logical.children.size());
    record.kind = SelectKind(logical);
    record.has_value = logical.has_value;
    switch (record.kind) {
    case ArtBumpNodeKind::Node4:
    case ArtBumpNodeKind::Node16:
        for (std::size_t index = 0; index < logical.children.size(); ++index) {
            record.keys[index] = logical.children[index].edge;
            record.children[index] = logical.children[index].reference;
        }
        break;
    case ArtBumpNodeKind::Node48:
        for (std::size_t index = 0; index < logical.children.size(); ++index) {
            record.index[logical.children[index].edge] =
                static_cast<std::uint8_t>(index);
            record.children[index] = logical.children[index].reference;
        }
        break;
    case ArtBumpNodeKind::Node256:
        for (std::size_t index = 0; index < logical.children.size(); ++index) {
            record.children[logical.children[index].edge] =
                logical.children[index].reference;
        }
        break;
    }
    return record;
}

void ValidateCommittedShape(const ArtBumpNodeRecord& node) {
    const auto count = static_cast<std::size_t>(node.child_count);
    switch (node.kind) {
    case ArtBumpNodeKind::Node4:
        if (count > 4U) Raise(AllocatorErrorCode::Corrupt, "Node4 overflow");
        break;
    case ArtBumpNodeKind::Node16:
        if (count < 5U || count > 16U) {
            Raise(AllocatorErrorCode::Corrupt, "noncanonical Node16 occupancy");
        }
        break;
    case ArtBumpNodeKind::Node48:
        if (count < 17U || count > 48U) {
            Raise(AllocatorErrorCode::Corrupt, "noncanonical Node48 occupancy");
        }
        break;
    case ArtBumpNodeKind::Node256:
        if (count < 38U || count > 256U) {
            Raise(AllocatorErrorCode::Corrupt, "noncanonical Node256 occupancy");
        }
        break;
    }
    if (!node.has_value && count <= 1U) {
        Raise(
            AllocatorErrorCode::Corrupt,
            "unpruned ART node without a value");
    }
    if (count == 0U &&
        (!node.has_value || node.kind != ArtBumpNodeKind::Node4)) {
        Raise(AllocatorErrorCode::Corrupt, "invalid ART leaf");
    }
}

struct RawInterval {
    std::uint64_t begin = 0;
    std::uint64_t end = 0;
};

struct VisitedNode {
    NodeRef reference = 0;
    ArtBumpNodeKind kind = ArtBumpNodeKind::Node4;
};

struct InspectionWorkspace {
    std::array<std::vector<std::uint8_t>, 4> seen_bits;
    std::vector<NodeRef> stack;
    std::vector<VisitedNode> nodes;
    std::vector<RawInterval> intervals;

    void Reserve(
        const ArtBumpNodeStore& store,
        std::size_t maximum_nodes,
        std::size_t maximum_intervals) {
        for (std::size_t index = 0; index < seen_bits.size(); ++index) {
            const auto kind = static_cast<ArtBumpNodeKind>(index + 1U);
            const auto capacity = static_cast<std::size_t>(store.Capacity(kind));
            seen_bits[index].assign((capacity + 7U) / 8U, 0);
        }
        stack.reserve(maximum_nodes);
        nodes.reserve(maximum_nodes);
        intervals.reserve(maximum_intervals);
    }

    void Reset() noexcept {
        for (auto& bitmap : seen_bits) {
            std::fill(bitmap.begin(), bitmap.end(), std::uint8_t{0});
        }
        stack.clear();
        nodes.clear();
        intervals.clear();
    }
};

bool BitmapBit(const std::vector<std::uint8_t>& bitmap, std::uint32_t bit) {
    const auto byte = static_cast<std::size_t>(bit / 8U);
    if (byte >= bitmap.size()) {
        Raise(AllocatorErrorCode::Corrupt, "node slot exceeds slab capacity");
    }
    return (bitmap[byte] & static_cast<std::uint8_t>(1U << (bit % 8U))) != 0;
}

void SetBitmapBit(std::vector<std::uint8_t>* bitmap, std::uint32_t bit) {
    const auto byte = static_cast<std::size_t>(bit / 8U);
    if (byte >= bitmap->size()) {
        Raise(AllocatorErrorCode::Corrupt, "node slot exceeds slab capacity");
    }
    (*bitmap)[byte] = static_cast<std::uint8_t>(
        (*bitmap)[byte] | static_cast<std::uint8_t>(1U << (bit % 8U)));
}

struct TreeInspection {
    ArtBumpIndex::GraphSummary summary;
    InspectionWorkspace workspace;
};

struct InspectOptions {
    std::uint32_t zone = 0;
    std::uint64_t recorded_top = 0;
    std::uint64_t entry_limit = 0;
    bool recovery_read = false;
    bool canonical_values = true;
};

std::size_t ValueLength(
    const ArtBumpRawZone& zone,
    std::uint64_t offset,
    std::uint64_t recorded_top,
    bool canonical) {
    const auto* first = zone.ReadBounded(offset, 1, recorded_top);
    const auto kind_length = static_cast<std::size_t>(first[0]);
    if (kind_length == 0U) {
        Raise(AllocatorErrorCode::Corrupt, "TLV kind is empty");
    }
    const auto fixed = std::size_t{1} + kind_length + std::size_t{8};
    const auto* header = zone.ReadBounded(offset, fixed, recorded_top);
    const auto raw_offset = std::size_t{1} + kind_length + std::size_t{4};
    std::uint32_t raw_length = 0;
    for (std::size_t index = 0; index < sizeof(raw_length); ++index) {
        raw_length |= static_cast<std::uint32_t>(header[raw_offset + index]) <<
            static_cast<unsigned>(index * 8U);
    }
    if (static_cast<std::size_t>(raw_length) >
        std::numeric_limits<std::size_t>::max() - fixed) {
        Raise(AllocatorErrorCode::Corrupt, "TLV length exceeds address space");
    }
    const auto total = fixed + static_cast<std::size_t>(raw_length);
    const auto* bytes = zone.ReadBounded(offset, total, recorded_top);
    if (canonical) {
        try {
            const auto decoded = kvspace::XValue::Decode(bytes, total);
            if (decoded.Encode() !=
                std::vector<std::uint8_t>(bytes, bytes + total)) {
                Raise(AllocatorErrorCode::Corrupt, "noncanonical TLV value");
            }
        } catch (const AllocatorError&) {
            throw;
        } catch (const kvspace::Error& error) {
            Raise(AllocatorErrorCode::Corrupt, error.what());
        }
    }
    return total;
}

void AddInterval(
    InspectionWorkspace* workspace,
    std::uint64_t begin,
    std::size_t length) {
    const auto end = CheckedAdd(
        begin,
        static_cast<std::uint64_t>(length),
        AllocatorErrorCode::Corrupt,
        "raw interval overflows");
    if (workspace->intervals.size() == workspace->intervals.capacity()) {
        Raise(AllocatorErrorCode::Capacity, "inspection interval scratch exhausted");
    }
    workspace->intervals.push_back(RawInterval{begin, end});
}

void MarkNode(
    ArtBumpNodeStore& store,
    InspectionWorkspace* workspace,
    NodeRef reference) {
    const auto kind = store.ReferenceKind(reference);
    const auto kind_index = KindIndex(kind);
    const auto slot = store.SlotIndex(reference);
    if (BitmapBit(workspace->seen_bits[kind_index], slot)) {
        Raise(
            AllocatorErrorCode::Corrupt,
            "ART graph contains a cycle or shared node");
    }
    SetBitmapBit(&workspace->seen_bits[kind_index], slot);
}

void InspectTreeInto(
    ArtBumpNodeStore& store,
    const std::array<ArtBumpRawZone*, 2>& zones,
    NodeRef root,
    const InspectOptions& options,
    InspectionWorkspace* workspace,
    ArtBumpIndex::GraphSummary* summary) {
    if (options.zone > 1U || zones[options.zone] == nullptr) {
        Raise(AllocatorErrorCode::Corrupt, "invalid active raw zone");
    }
    workspace->Reset();
    *summary = ArtBumpIndex::GraphSummary{};
    summary->root = root;
    summary->recovered_top = zones[options.zone]->Begin();
    if (root == 0) return;

    MarkNode(store, workspace, root);
    workspace->stack.push_back(root);
    while (!workspace->stack.empty()) {
        const auto reference = workspace->stack.back();
        workspace->stack.pop_back();
        const auto node = options.recovery_read
            ? store.ReadForRecovery(reference)
            : store.Read(reference);
        ValidateCommittedShape(node);
        if (workspace->nodes.size() == workspace->nodes.capacity()) {
            Raise(AllocatorErrorCode::Capacity, "inspection node scratch exhausted");
        }
        workspace->nodes.push_back(VisitedNode{reference, node.kind});
        ++summary->node_count;
        const auto payload = static_cast<std::uint64_t>(
            ArtBumpNodeCodec::PayloadBytes(node.kind));
        summary->engine_live_bytes = CheckedAdd(
            summary->engine_live_bytes,
            payload,
            AllocatorErrorCode::Corrupt,
            "engine live bytes overflow");
        ++summary->slab_live_counts[KindIndex(node.kind)];

        if (node.prefix_len != 0U) {
            zones[options.zone]->ValidateInterval(
                node.prefix_offset, node.prefix_len, options.recorded_top);
            AddInterval(workspace, node.prefix_offset, node.prefix_len);
            const auto end = node.prefix_offset + node.prefix_len;
            summary->recovered_top = std::max(summary->recovered_top, end);
            summary->engine_live_bytes = CheckedAdd(
                summary->engine_live_bytes,
                node.prefix_len,
                AllocatorErrorCode::Corrupt,
                "engine live bytes overflow");
        }
        if (node.has_value) {
            ++summary->entry_count;
            if (summary->entry_count > options.entry_limit) {
                Raise(AllocatorErrorCode::Corrupt, "entry limit exceeded");
            }
            if (node.value_offset != 0) {
                const auto length = ValueLength(
                    *zones[options.zone],
                    node.value_offset,
                    options.recorded_top,
                    options.canonical_values);
                AddInterval(workspace, node.value_offset, length);
                const auto end = node.value_offset + length;
                summary->recovered_top = std::max(summary->recovered_top, end);
                summary->engine_live_bytes = CheckedAdd(
                    summary->engine_live_bytes,
                    static_cast<std::uint64_t>(length),
                    AllocatorErrorCode::Corrupt,
                    "engine live bytes overflow");
            }
        }

        for (std::size_t edge = 0; edge < 256; ++edge) {
            const auto child = ChildAt(node, static_cast<std::uint8_t>(edge));
            if (child == 0) continue;
            MarkNode(store, workspace, child);
            if (workspace->stack.size() == workspace->stack.capacity()) {
                Raise(AllocatorErrorCode::Capacity, "inspection stack exhausted");
            }
            workspace->stack.push_back(child);
        }
    }

    std::sort(
        workspace->intervals.begin(), workspace->intervals.end(),
        [](const RawInterval& left, const RawInterval& right) {
            return left.begin < right.begin ||
                (left.begin == right.begin && left.end < right.end);
        });
    for (std::size_t index = 1; index < workspace->intervals.size(); ++index) {
        if (workspace->intervals[index].begin <
            workspace->intervals[index - 1U].end) {
            Raise(
                AllocatorErrorCode::Corrupt,
                "ART raw intervals overlap or are duplicated");
        }
    }
}

std::size_t MaximumTreeNodes(std::uint64_t entry_limit) {
    if (entry_limit == 0) return 0;
    const auto doubled = CheckedAdd(
        entry_limit,
        entry_limit,
        AllocatorErrorCode::Capacity,
        "entry limit is too large for traversal scratch");
    return CheckedSize(doubled - 1U, "tree node count exceeds address space");
}

TreeInspection InspectTree(
    ArtBumpNodeStore& store,
    const std::array<ArtBumpRawZone*, 2>& zones,
    NodeRef root,
    const InspectOptions& options) {
    TreeInspection result;
    const auto maximum_nodes = MaximumTreeNodes(options.entry_limit);
    const auto maximum_intervals = maximum_nodes >
            std::numeric_limits<std::size_t>::max() / 2U
        ? std::numeric_limits<std::size_t>::max()
        : maximum_nodes * 2U;
    result.workspace.Reserve(store, maximum_nodes, maximum_intervals);
    InspectTreeInto(
        store, zones, root, options, &result.workspace, &result.summary);
    return result;
}

bool SameDerived(
    const ArtBumpDerivedState& left,
    const ArtBumpIndex::GraphSummary& right) noexcept {
    return left.node_count == right.node_count &&
        left.entry_count == right.entry_count &&
        left.engine_live_bytes == right.engine_live_bytes;
}

void ValidateRawZoneSnapshot(
    const ArtBumpStateSnapshot& snapshot,
    const std::array<ArtBumpRawZone*, 2>& zones) {
    for (std::size_t index = 0; index < zones.size(); ++index) {
        if (zones[index] == nullptr) {
            Raise(AllocatorErrorCode::InvalidArgument, "null raw-zone handle");
        }
        const auto& stored = snapshot.zones[index];
        if (stored.begin != zones[index]->Begin() ||
            stored.bytes != zones[index]->Bytes() ||
            stored.top != zones[index]->TopAcquire() ||
            stored.epoch != zones[index]->Epoch() ||
            stored.epoch == 0U) {
            Raise(AllocatorErrorCode::Corrupt, "raw-zone snapshot mismatch");
        }
        if (stored.top < stored.begin ||
            AddOverflows(stored.begin, stored.bytes) ||
            stored.top > stored.begin + stored.bytes) {
            Raise(AllocatorErrorCode::Corrupt, "invalid raw-zone bounds");
        }
    }
}

void ValidateIdleSnapshot(
    const ArtBumpStateSnapshot& snapshot,
    const std::array<ArtBumpRawZone*, 2>& zones) {
    if (snapshot.journal.state != ArtBumpJournalState::Idle) {
        Raise(AllocatorErrorCode::Corrupt, "compact journal is not IDLE");
    }
    if (snapshot.active_zone > 1U ||
        snapshot.common_root_offset != 0 || snapshot.tombstone_count != 0) {
        Raise(AllocatorErrorCode::Corrupt, "invalid clean ArtBump authority fields");
    }
    ValidateRawZoneSnapshot(snapshot, zones);
    const auto inactive = static_cast<std::size_t>(1U - snapshot.active_zone);
    if (snapshot.zones[inactive].top != snapshot.zones[inactive].begin) {
        Raise(AllocatorErrorCode::Corrupt, "inactive raw zone is not empty");
    }
}

void ValidateStoreMatchesTree(
    ArtBumpNodeStore& store,
    const ArtBumpIndex::GraphSummary& summary) {
    store.Validate();
    for (std::size_t index = 0; index < 4; ++index) {
        const auto kind = static_cast<ArtBumpNodeKind>(index + 1U);
        if (store.UsedCount(kind) != summary.slab_live_counts[index]) {
            Raise(AllocatorErrorCode::Corrupt, "slab live set disagrees with tree");
        }
    }
}

std::vector<std::uint8_t> ReadStoredValue(
    const ArtBumpRawZone& zone,
    const ArtBumpNodeRecord& node,
    std::uint64_t recorded_top) {
    if (!node.has_value) {
        Raise(AllocatorErrorCode::Corrupt, "node has no stored value");
    }
    if (node.value_offset == 0) return {};
    const auto length = ValueLength(
        zone, node.value_offset, recorded_top, true);
    const auto* bytes = zone.ReadBounded(
        node.value_offset, length, recorded_top);
    return {bytes, bytes + length};
}

void ValidateInputValue(const std::vector<std::uint8_t>& value) {
    if (value.empty()) return;
    try {
        const auto decoded = kvspace::XValue::Decode(value.data(), value.size());
        if (decoded.Encode() != value) {
            Raise(AllocatorErrorCode::InvalidArgument, "noncanonical input TLV");
        }
    } catch (const AllocatorError&) {
        throw;
    } catch (const kvspace::Error& error) {
        Raise(AllocatorErrorCode::InvalidArgument, error.what());
    }
}

} // namespace

struct ArtBumpIndex::Impl {
    ArtBumpNodeStore* nodes = nullptr;
    std::array<ArtBumpRawZone*, 2> zones{};
    ArtBumpIndexState* state = nullptr;
};

namespace {

std::string_view PrefixView(
    const ArtBumpRawZone& zone,
    const ArtBumpNodeRecord& node,
    std::uint64_t recorded_top) {
    if (node.prefix_len == 0U) return {};
    const auto* bytes = zone.ReadBounded(
        node.prefix_offset, node.prefix_len, recorded_top);
    return {
        reinterpret_cast<const char*>(bytes),
        static_cast<std::size_t>(node.prefix_len)};
}

NodeRef FindNode(
    ArtBumpNodeStore& store,
    const ArtBumpRawZone& zone,
    NodeRef root,
    std::uint64_t recorded_top,
    std::string_view key) {
    auto current = root;
    std::size_t depth = 0;
    while (current != 0) {
        const auto node = store.Read(current);
        const auto prefix = PrefixView(zone, node, recorded_top);
        if (depth > key.size() || prefix.size() > key.size() - depth ||
            key.substr(depth, prefix.size()) != prefix) {
            return 0;
        }
        depth += prefix.size();
        if (depth == key.size()) return current;
        const auto edge = KeyByte(key, depth);
        ++depth;
        current = ChildAt(node, edge);
    }
    return 0;
}

std::optional<std::vector<std::uint8_t>> GetAt(
    ArtBumpNodeStore& store,
    const ArtBumpRawZone& zone,
    NodeRef root,
    std::uint64_t recorded_top,
    std::string_view key) {
    const auto reference = FindNode(store, zone, root, recorded_top, key);
    if (reference == 0) return std::nullopt;
    const auto node = store.Read(reference);
    if (!node.has_value) return std::nullopt;
    return ReadStoredValue(zone, node, recorded_top);
}

std::vector<ArtBumpIndex::Entry> EntriesAt(
    ArtBumpNodeStore& store,
    const ArtBumpRawZone& zone,
    NodeRef root,
    std::uint64_t recorded_top,
    std::string_view wanted_prefix) {
    struct WalkItem {
        NodeRef reference = 0;
        std::string before;
    };

    std::vector<ArtBumpIndex::Entry> result;
    if (root == 0) return result;
    std::vector<WalkItem> stack;
    stack.push_back(WalkItem{root, {}});
    while (!stack.empty()) {
        auto item = std::move(stack.back());
        stack.pop_back();
        const auto node = store.Read(item.reference);
        item.before.append(PrefixView(zone, node, recorded_top));
        if (node.has_value &&
            (wanted_prefix.empty() ||
             (item.before.size() >= wanted_prefix.size() &&
              std::equal(
                  wanted_prefix.begin(),
                  wanted_prefix.end(),
                  item.before.begin())))) {
            result.push_back(ArtBumpIndex::Entry{
                item.before,
                ReadStoredValue(zone, node, recorded_top)});
        }
        for (std::size_t edge_plus_one = 256; edge_plus_one != 0;
             --edge_plus_one) {
            const auto edge = static_cast<std::uint8_t>(edge_plus_one - 1U);
            const auto child = ChildAt(node, edge);
            if (child == 0) continue;
            auto child_before = item.before;
            child_before.push_back(static_cast<char>(edge));
            stack.push_back(WalkItem{child, std::move(child_before)});
        }
    }
    return result;
}

struct Replacement {
    NodeRef reference = 0;
    std::int32_t build_index = -1;

    bool IsBuild() const noexcept { return build_index >= 0; }
    bool IsEmpty() const noexcept {
        return reference == 0 && build_index < 0;
    }
};

struct PendingNode {
    LogicalNode logical;
    std::vector<std::int32_t> child_build;
    bool replace_prefix = false;
    std::vector<std::uint8_t> prefix_bytes;
    bool replace_value = false;
    std::vector<std::uint8_t> value_bytes;
};

struct PathStep {
    NodeRef reference = 0;
    LogicalNode logical;
    std::uint8_t edge_to_child = 0;
};

struct UpdateBuildPlan {
    std::vector<PendingNode> builds;
    std::vector<NodeRef> retired_path;
    std::vector<NodeRef> allocated_scratch;
    Replacement root;
    std::uint64_t projected_entries = 0;
    std::uint64_t raw_bytes = 0;
    std::array<std::uint64_t, 4> required_nodes{};
};

PendingNode PendingFromRecord(const ArtBumpNodeRecord& record) {
    PendingNode result;
    result.logical = ToLogical(record);
    result.child_build.assign(result.logical.children.size(), -1);
    return result;
}

PendingNode PendingFromLogical(const LogicalNode& logical) {
    PendingNode result;
    result.logical = logical;
    result.child_build.assign(result.logical.children.size(), -1);
    return result;
}

void SetPendingChild(
    PendingNode* node,
    std::uint8_t edge,
    Replacement replacement) {
    auto begin = node->logical.children.begin();
    auto end = node->logical.children.end();
    auto position = std::lower_bound(
        begin, end, edge,
        [](const ChildPair& child, std::uint8_t wanted) {
            return child.edge < wanted;
        });
    const auto index = static_cast<std::size_t>(position - begin);
    const auto exists = position != end && position->edge == edge;
    if (replacement.IsEmpty()) {
        if (!exists) {
            Raise(AllocatorErrorCode::Corrupt, "attempted to remove a missing child");
        }
        node->logical.children.erase(position);
        node->child_build.erase(
            node->child_build.begin() + static_cast<std::ptrdiff_t>(index));
        return;
    }
    if (!exists) {
        if (node->logical.children.size() == 256U) {
            Raise(AllocatorErrorCode::Corrupt, "ART node has too many children");
        }
        node->logical.children.insert(
            node->logical.children.begin() +
                static_cast<std::ptrdiff_t>(index),
            ChildPair{edge, replacement.reference});
        node->child_build.insert(
            node->child_build.begin() + static_cast<std::ptrdiff_t>(index),
            replacement.build_index);
        return;
    }
    node->logical.children[index] = ChildPair{edge, replacement.reference};
    node->child_build[index] = replacement.build_index;
}

std::int32_t AddBuild(UpdateBuildPlan* plan, PendingNode node) {
    const auto index = plan->builds.size();
    if (index > static_cast<std::size_t>(INT32_MAX)) {
        Raise(AllocatorErrorCode::Capacity, "replacement path is too deep");
    }
    plan->builds.push_back(std::move(node));
    return static_cast<std::int32_t>(index);
}

void AddRetired(UpdateBuildPlan* plan, NodeRef reference) {
    if (std::find(
            plan->retired_path.begin(),
            plan->retired_path.end(),
            reference) != plan->retired_path.end()) {
        return;
    }
    plan->retired_path.push_back(reference);
}

std::vector<std::uint8_t> PrefixBytes(
    const PendingNode& node,
    const ArtBumpRawZone& zone,
    std::uint64_t recorded_top) {
    if (node.replace_prefix) return node.prefix_bytes;
    if (node.logical.prefix_len == 0U) return {};
    const auto* bytes = zone.ReadBounded(
        node.logical.prefix_offset,
        node.logical.prefix_len,
        recorded_top);
    return {bytes, bytes + node.logical.prefix_len};
}

PendingNode PendingForReplacement(
    const Replacement& replacement,
    UpdateBuildPlan* plan,
    ArtBumpNodeStore& store,
    NodeRef* retired_existing) {
    if (replacement.IsBuild()) {
        return std::move(plan->builds[static_cast<std::size_t>(
            replacement.build_index)]);
    }
    if (replacement.reference == 0) {
        Raise(AllocatorErrorCode::Corrupt, "empty replacement has no node");
    }
    *retired_existing = replacement.reference;
    return PendingFromRecord(store.Read(replacement.reference));
}

Replacement NormalizeAndBuild(
    PendingNode node,
    UpdateBuildPlan* plan,
    ArtBumpNodeStore& store,
    const ArtBumpRawZone& zone,
    std::uint64_t recorded_top) {
    if (!node.logical.has_value && node.logical.children.empty()) {
        return Replacement{};
    }
    if (!node.logical.has_value && node.logical.children.size() == 1U) {
        const auto edge = node.logical.children[0].edge;
        const Replacement child{
            node.logical.children[0].reference,
            node.child_build[0]};
        NodeRef retired_existing = 0;
        auto child_node = PendingForReplacement(
            child, plan, store, &retired_existing);
        if (retired_existing != 0) AddRetired(plan, retired_existing);

        auto parent_prefix = PrefixBytes(node, zone, recorded_top);
        auto child_prefix = PrefixBytes(child_node, zone, recorded_top);
        const auto maximum = static_cast<std::size_t>(UINT32_MAX);
        if (parent_prefix.size() >= maximum ||
            child_prefix.size() >
                maximum - parent_prefix.size() - std::size_t{1}) {
            Raise(AllocatorErrorCode::Capacity, "compacted prefix exceeds uint32");
        }
        std::vector<std::uint8_t> combined;
        combined.reserve(
            parent_prefix.size() + std::size_t{1} + child_prefix.size());
        combined.insert(
            combined.end(), parent_prefix.begin(), parent_prefix.end());
        combined.push_back(edge);
        combined.insert(
            combined.end(), child_prefix.begin(), child_prefix.end());
        child_node.replace_prefix = true;
        child_node.prefix_bytes = std::move(combined);
        child_node.logical.prefix_offset = 0;
        child_node.logical.prefix_len = static_cast<std::uint32_t>(
            child_node.prefix_bytes.size());

        if (child.IsBuild()) {
            plan->builds[static_cast<std::size_t>(child.build_index)] =
                std::move(child_node);
            return child;
        }
        const auto build = AddBuild(plan, std::move(child_node));
        return Replacement{0, build};
    }
    const auto build = AddBuild(plan, std::move(node));
    return Replacement{0, build};
}

PendingNode MakeLeaf(
    std::string_view prefix,
    const std::vector<std::uint8_t>& value) {
    if (prefix.size() > static_cast<std::size_t>(UINT32_MAX)) {
        Raise(AllocatorErrorCode::Capacity, "ART prefix exceeds uint32");
    }
    PendingNode leaf;
    leaf.logical.has_value = true;
    leaf.logical.original_kind = ArtBumpNodeKind::Node4;
    leaf.replace_prefix = true;
    leaf.prefix_bytes.assign(prefix.begin(), prefix.end());
    leaf.logical.prefix_len = static_cast<std::uint32_t>(prefix.size());
    leaf.replace_value = true;
    leaf.value_bytes = value;
    return leaf;
}

void SetReplacementValue(
    PendingNode* node,
    const std::vector<std::uint8_t>& value) {
    node->logical.has_value = true;
    node->logical.value_offset = 0;
    node->replace_value = true;
    node->value_bytes = value;
}

void FinalizeBuildRequirements(UpdateBuildPlan* plan) {
    plan->raw_bytes = 0;
    plan->required_nodes.fill(0);
    for (const auto& build : plan->builds) {
        if (build.replace_prefix) {
            plan->raw_bytes = CheckedAdd(
                plan->raw_bytes,
                static_cast<std::uint64_t>(build.prefix_bytes.size()),
                AllocatorErrorCode::Capacity,
                "replacement raw bytes overflow");
        }
        if (build.replace_value) {
            plan->raw_bytes = CheckedAdd(
                plan->raw_bytes,
                static_cast<std::uint64_t>(build.value_bytes.size()),
                AllocatorErrorCode::Capacity,
                "replacement raw bytes overflow");
        }
        ++plan->required_nodes[KindIndex(SelectKind(build.logical))];
    }
    plan->allocated_scratch.reserve(plan->builds.size());
}

UpdateBuildPlan PlanPut(
    ArtBumpNodeStore& store,
    const ArtBumpRawZone& zone,
    NodeRef staged_root,
    std::uint64_t staged_entries,
    std::uint64_t entry_limit,
    std::uint64_t recorded_top,
    std::string_view key,
    const std::vector<std::uint8_t>& value,
    std::size_t maximum_nodes,
    bool* changed) {
    UpdateBuildPlan plan;
    plan.projected_entries = staged_entries;
    *changed = true;
    if (staged_root == 0) {
        if (staged_entries != 0) {
            Raise(AllocatorErrorCode::Corrupt, "empty root has nonzero entries");
        }
        if (entry_limit == 0) {
            Raise(AllocatorErrorCode::Capacity, "entry limit reached");
        }
        const auto leaf = AddBuild(&plan, MakeLeaf(key, value));
        plan.root = Replacement{0, leaf};
        plan.projected_entries = 1;
        FinalizeBuildRequirements(&plan);
        return plan;
    }

    std::vector<PathStep> path;
    auto current = staged_root;
    std::size_t depth = 0;
    for (;;) {
        if (path.size() >= maximum_nodes) {
            Raise(AllocatorErrorCode::Corrupt, "ART path exceeds entry limit");
        }
        const auto node = store.Read(current);
        path.push_back(PathStep{current, ToLogical(node), 0});
        const auto node_prefix = PrefixView(zone, node, recorded_top);
        const auto remaining = key.substr(depth);
        std::size_t common = 0;
        const auto compared = std::min(node_prefix.size(), remaining.size());
        while (common < compared && node_prefix[common] == remaining[common]) {
            ++common;
        }

        Replacement replacement;
        bool inserted = false;
        if (common != node_prefix.size()) {
            inserted = true;
            auto old_child = PendingFromLogical(path.back().logical);
            old_child.replace_prefix = true;
            old_child.prefix_bytes.assign(
                node_prefix.begin() + static_cast<std::ptrdiff_t>(common + 1U),
                node_prefix.end());
            old_child.logical.prefix_offset = 0;
            old_child.logical.prefix_len = static_cast<std::uint32_t>(
                old_child.prefix_bytes.size());
            const auto old_build = AddBuild(&plan, std::move(old_child));

            PendingNode parent;
            parent.replace_prefix = true;
            parent.prefix_bytes.assign(
                node_prefix.begin(),
                node_prefix.begin() + static_cast<std::ptrdiff_t>(common));
            parent.logical.prefix_len = static_cast<std::uint32_t>(common);
            const auto old_edge = static_cast<std::uint8_t>(
                static_cast<unsigned char>(node_prefix[common]));
            SetPendingChild(&parent, old_edge, Replacement{0, old_build});
            const auto split_depth = depth + common;
            if (split_depth == key.size()) {
                SetReplacementValue(&parent, value);
            } else {
                const auto new_edge = KeyByte(key, split_depth);
                const auto leaf = AddBuild(
                    &plan,
                    MakeLeaf(key.substr(split_depth + 1U), value));
                SetPendingChild(&parent, new_edge, Replacement{0, leaf});
            }
            const auto parent_build = AddBuild(&plan, std::move(parent));
            replacement = Replacement{0, parent_build};
        } else {
            depth += node_prefix.size();
            if (depth == key.size()) {
                if (node.has_value) {
                    const auto old_value = ReadStoredValue(
                        zone, node, recorded_top);
                    if (old_value == value) {
                        *changed = false;
                        return plan;
                    }
                } else {
                    inserted = true;
                }
                auto terminal = PendingFromLogical(path.back().logical);
                SetReplacementValue(&terminal, value);
                const auto terminal_build = AddBuild(
                    &plan, std::move(terminal));
                replacement = Replacement{0, terminal_build};
            } else {
                const auto edge = KeyByte(key, depth);
                ++depth;
                const auto next = ChildAt(node, edge);
                if (next != 0) {
                    path.back().edge_to_child = edge;
                    current = next;
                    continue;
                }
                inserted = true;
                const auto leaf = AddBuild(
                    &plan, MakeLeaf(key.substr(depth), value));
                auto terminal = PendingFromLogical(path.back().logical);
                SetPendingChild(&terminal, edge, Replacement{0, leaf});
                const auto terminal_build = AddBuild(
                    &plan, std::move(terminal));
                replacement = Replacement{0, terminal_build};
            }
        }

        if (inserted) {
            if (staged_entries >= entry_limit) {
                Raise(AllocatorErrorCode::Capacity, "entry limit reached");
            }
            plan.projected_entries = staged_entries + 1U;
        }
        for (const auto& step : path) AddRetired(&plan, step.reference);
        for (std::size_t index = path.size(); index > 1U; --index) {
            const auto& parent_step = path[index - 2U];
            auto parent = PendingFromLogical(parent_step.logical);
            SetPendingChild(&parent, parent_step.edge_to_child, replacement);
            const auto parent_build = AddBuild(&plan, std::move(parent));
            replacement = Replacement{0, parent_build};
        }
        plan.root = replacement;
        FinalizeBuildRequirements(&plan);
        return plan;
    }
}

UpdateBuildPlan PlanErase(
    ArtBumpNodeStore& store,
    const ArtBumpRawZone& zone,
    NodeRef staged_root,
    std::uint64_t staged_entries,
    std::uint64_t recorded_top,
    std::string_view key,
    std::size_t maximum_nodes,
    bool* changed) {
    UpdateBuildPlan plan;
    plan.projected_entries = staged_entries;
    *changed = false;
    if (staged_root == 0) return plan;

    std::vector<PathStep> path;
    auto current = staged_root;
    std::size_t depth = 0;
    for (;;) {
        if (path.size() >= maximum_nodes) {
            Raise(AllocatorErrorCode::Corrupt, "ART path exceeds entry limit");
        }
        const auto node = store.Read(current);
        path.push_back(PathStep{current, ToLogical(node), 0});
        const auto prefix = PrefixView(zone, node, recorded_top);
        if (prefix.size() > key.size() - depth ||
            key.substr(depth, prefix.size()) != prefix) {
            return plan;
        }
        depth += prefix.size();
        if (depth == key.size()) {
            if (!node.has_value) return plan;
            break;
        }
        const auto edge = KeyByte(key, depth);
        ++depth;
        const auto next = ChildAt(node, edge);
        if (next == 0) return plan;
        path.back().edge_to_child = edge;
        current = next;
    }

    *changed = true;
    for (const auto& step : path) AddRetired(&plan, step.reference);
    auto terminal = PendingFromLogical(path.back().logical);
    terminal.logical.has_value = false;
    terminal.logical.value_offset = 0;
    terminal.replace_value = false;
    terminal.value_bytes.clear();
    auto replacement = NormalizeAndBuild(
        std::move(terminal), &plan, store, zone, recorded_top);
    for (std::size_t index = path.size(); index > 1U; --index) {
        const auto& parent_step = path[index - 2U];
        auto parent = PendingFromLogical(parent_step.logical);
        SetPendingChild(&parent, parent_step.edge_to_child, replacement);
        replacement = NormalizeAndBuild(
            std::move(parent), &plan, store, zone, recorded_top);
    }
    plan.root = replacement;
    if (plan.projected_entries == 0) {
        Raise(AllocatorErrorCode::Corrupt, "ART entry count underflow");
    }
    --plan.projected_entries;
    FinalizeBuildRequirements(&plan);
    return plan;
}

bool CommittedBitmapContains(
    ArtBumpNodeStore& store,
    const InspectionWorkspace& committed,
    NodeRef reference) {
    const auto kind = store.ReferenceKind(reference);
    const auto slot = store.SlotIndex(reference);
    return BitmapBit(committed.seen_bits[KindIndex(kind)], slot);
}

void ValidateUnionAllocationState(
    ArtBumpNodeStore& store,
    const InspectionWorkspace& committed,
    const InspectionWorkspace& staged,
    bool validate_allocator_metadata) {
    if (validate_allocator_metadata) store.Validate();
    std::array<std::uint64_t, 4> union_counts{};
    for (const auto& node : committed.nodes) {
        ++union_counts[KindIndex(node.kind)];
    }
    for (const auto& node : staged.nodes) {
        if (!CommittedBitmapContains(store, committed, node.reference)) {
            ++union_counts[KindIndex(node.kind)];
        }
    }
    for (std::size_t index = 0; index < union_counts.size(); ++index) {
        const auto kind = static_cast<ArtBumpNodeKind>(index + 1U);
        if (store.UsedCount(kind) != union_counts[index]) {
            Raise(
                AllocatorErrorCode::Corrupt,
                "slab live set is not exactly Ctree union staged tree");
        }
    }
}

ArtBumpNodeLiveBitmaps LiveBitmapsFor(
    ArtBumpNodeStore& store,
    const std::vector<VisitedNode>& nodes,
    ArtBumpNodeLiveBitCounts* counts) {
    ArtBumpNodeLiveBitmaps bitmaps;
    counts->fill(0);
    for (std::size_t index = 0; index < bitmaps.size(); ++index) {
        const auto kind = static_cast<ArtBumpNodeKind>(index + 1U);
        const auto capacity = static_cast<std::size_t>(store.Capacity(kind));
        bitmaps[index].assign((capacity + 7U) / 8U, 0);
    }
    for (const auto& node : nodes) {
        const auto kind_index = KindIndex(node.kind);
        const auto slot = store.SlotIndex(node.reference);
        if (BitmapBit(bitmaps[kind_index], slot)) {
            Raise(AllocatorErrorCode::Corrupt, "duplicate node in rebuild plan");
        }
        SetBitmapBit(&bitmaps[kind_index], slot);
        const auto next = static_cast<std::size_t>(slot) + 1U;
        (*counts)[kind_index] = std::max((*counts)[kind_index], next);
    }
    return bitmaps;
}

} // namespace

struct ArtBumpIndex::Mutation::Impl {
    ArtBumpIndex::Impl* owner = nullptr;
    ArtBumpStateSnapshot baseline{};
    TreeInspection committed;
    InspectionWorkspace staged_scratch;
    NodeRef staged_root = 0;
    std::uint64_t staged_entries = 0;
    std::uint64_t entry_raw_top = 0;
    std::size_t maximum_nodes = 0;
    std::vector<NodeRef> staged_only;
    bool raw_top_advanced = false;
    bool active = true;
    bool failed = false;

    bool IsCommitted(NodeRef reference) const {
        return CommittedBitmapContains(
            *owner->nodes, committed.workspace, reference);
    }

    void ValidateCurrent(
        bool canonical_values,
        bool validate_allocator_metadata = true) {
        if (!active || failed) {
            Raise(AllocatorErrorCode::InvalidArgument, "mutation is not active");
        }
        const auto snapshot = owner->state->LoadSnapshotAcquire();
        ValidateIdleSnapshot(snapshot, owner->zones);
        if (snapshot.committed_root != baseline.committed_root ||
            snapshot.generation != baseline.generation ||
            snapshot.active_zone != baseline.active_zone ||
            snapshot.entry_limit != baseline.entry_limit ||
            !SameDerived(snapshot.derived, committed.summary) ||
            snapshot.zones[snapshot.active_zone].top < entry_raw_top) {
            Raise(AllocatorErrorCode::Corrupt, "committed state changed during mutation");
        }
        ArtBumpIndex::GraphSummary staged_summary;
        InspectTreeInto(
            *owner->nodes,
            owner->zones,
            staged_root,
            InspectOptions{
                snapshot.active_zone,
                snapshot.zones[snapshot.active_zone].top,
                snapshot.entry_limit,
                false,
                canonical_values},
            &staged_scratch,
            &staged_summary);
        if (staged_summary.entry_count != staged_entries) {
            Raise(AllocatorErrorCode::Corrupt, "staged entry count mismatch");
        }
        ValidateUnionAllocationState(
            *owner->nodes,
            committed.workspace,
            staged_scratch,
            validate_allocator_metadata);
    }

    void RemoveStagedOnly(NodeRef reference) {
        const auto found = std::find(
            staged_only.begin(), staged_only.end(), reference);
        if (found == staged_only.end()) {
            Raise(
                AllocatorErrorCode::Corrupt,
                "superseded staged node is not owned by the mutation");
        }
        owner->nodes->DiscardUnpublished(reference);
        staged_only.erase(found);
    }

    void AbortNoexcept() noexcept {
        if (!active) return;
        try {
            for (auto iterator = staged_only.rbegin();
                 iterator != staged_only.rend(); ++iterator) {
                owner->nodes->DiscardUnpublished(*iterator);
            }
            staged_only.clear();
            if (raw_top_advanced) {
                owner->zones[baseline.active_zone]->StoreTopRelease(
                    entry_raw_top);
                owner->state->AfterPersistentStep(
                    ArtBumpApplyStep::MutationAbortTopRestored, 0);
                raw_top_advanced = false;
            }
        } catch (...) {
            std::terminate();
        }
        active = false;
        failed = true;
    }

    bool ApplyUpdate(UpdateBuildPlan plan) {
        const auto active_zone = baseline.active_zone;
        auto& zone = *owner->zones[active_zone];

        std::array<std::uint64_t, 4> freed{};
        for (const auto reference : plan.retired_path) {
            if (!IsCommitted(reference)) {
                ++freed[KindIndex(owner->nodes->ReferenceKind(reference))];
            }
        }
        for (std::size_t index = 0; index < freed.size(); ++index) {
            const auto kind = static_cast<ArtBumpNodeKind>(index + 1U);
            const auto used = static_cast<std::uint64_t>(
                owner->nodes->UsedCount(kind));
            const auto capacity = static_cast<std::uint64_t>(
                owner->nodes->Capacity(kind));
            if (freed[index] > used ||
                plan.required_nodes[index] > capacity - (used - freed[index])) {
                Raise(AllocatorErrorCode::Capacity, "ART node slab is full");
            }
        }
        if (plan.raw_bytes > zone.Remaining()) {
            Raise(AllocatorErrorCode::Capacity, "active raw zone is full");
        }
        if (plan.projected_entries > baseline.entry_limit) {
            Raise(AllocatorErrorCode::Capacity, "entry limit reached");
        }

        auto& allocated = plan.allocated_scratch;
        allocated.clear();
        try {
            for (const auto reference : plan.retired_path) {
                if (!IsCommitted(reference)) RemoveStagedOnly(reference);
            }
            std::uint64_t raw_ordinal = 0;
            std::uint64_t node_ordinal = 0;
            for (auto& build : plan.builds) {
                if (build.replace_prefix) {
                    build.logical.prefix_offset = build.prefix_bytes.empty()
                        ? 0
                        : zone.Allocate(
                              build.prefix_bytes.data(),
                              build.prefix_bytes.size());
                    if (!build.prefix_bytes.empty()) {
                        raw_top_advanced = true;
                        owner->state->AfterPersistentStep(
                            ArtBumpApplyStep::MutationRawTopPublished,
                            raw_ordinal++);
                    }
                    build.logical.prefix_len = static_cast<std::uint32_t>(
                        build.prefix_bytes.size());
                }
                if (build.replace_value) {
                    build.logical.value_offset = build.value_bytes.empty()
                        ? 0
                        : zone.Allocate(
                              build.value_bytes.data(),
                              build.value_bytes.size());
                    if (!build.value_bytes.empty()) {
                        raw_top_advanced = true;
                        owner->state->AfterPersistentStep(
                            ArtBumpApplyStep::MutationRawTopPublished,
                            raw_ordinal++);
                    }
                    build.logical.has_value = true;
                }
                for (std::size_t slot = 0;
                     slot < build.logical.children.size(); ++slot) {
                    const auto child_build = build.child_build[slot];
                    if (child_build < 0) continue;
                    const auto child_index = static_cast<std::size_t>(child_build);
                    if (child_index >= allocated.size()) {
                        Raise(
                            AllocatorErrorCode::Corrupt,
                            "replacement path is not postorder");
                    }
                    build.logical.children[slot].reference =
                        allocated[child_index];
                }
                if (staged_only.size() == staged_only.capacity()) {
                    Raise(
                        AllocatorErrorCode::Capacity,
                        "mutation ownership scratch exhausted");
                }
                const auto reference = owner->nodes->Allocate(
                    ToRecord(build.logical));
                owner->state->AfterPersistentStep(
                    ArtBumpApplyStep::MutationNodeWritten,
                    node_ordinal++);
                allocated.push_back(reference);
                staged_only.push_back(reference);
            }
            NodeRef new_root = plan.root.reference;
            if (plan.root.IsBuild()) {
                const auto index = static_cast<std::size_t>(plan.root.build_index);
                if (index >= allocated.size()) {
                    Raise(AllocatorErrorCode::Corrupt, "invalid replacement root");
                }
                new_root = allocated[index];
            }
            staged_root = new_root;
            staged_entries = plan.projected_entries;
            ValidateCurrent(false, false);
            return true;
        } catch (...) {
            AbortNoexcept();
            throw;
        }
    }
};

ArtBumpIndex::Mutation ArtBumpIndex::BeginMutation() {
    const auto snapshot = impl_->state->LoadSnapshotAcquire();
    ValidateIdleSnapshot(snapshot, impl_->zones);
    auto committed = InspectTree(
        *impl_->nodes,
        impl_->zones,
        snapshot.committed_root,
        InspectOptions{
            snapshot.active_zone,
            snapshot.zones[snapshot.active_zone].top,
            snapshot.entry_limit,
            false,
            true});
    if (!SameDerived(snapshot.derived, committed.summary)) {
        Raise(AllocatorErrorCode::Corrupt, "common counters disagree with ART graph");
    }
    ValidateStoreMatchesTree(*impl_->nodes, committed.summary);

    auto mutation = std::make_unique<Mutation::Impl>();
    mutation->owner = impl_.get();
    mutation->baseline = snapshot;
    mutation->staged_root = snapshot.committed_root;
    mutation->staged_entries = snapshot.derived.entry_count;
    mutation->entry_raw_top = snapshot.zones[snapshot.active_zone].top;
    mutation->maximum_nodes = MaximumTreeNodes(snapshot.entry_limit);
    mutation->committed = std::move(committed);
    const auto maximum_intervals = mutation->maximum_nodes >
            std::numeric_limits<std::size_t>::max() / 2U
        ? std::numeric_limits<std::size_t>::max()
        : mutation->maximum_nodes * 2U;
    mutation->staged_scratch.Reserve(
        *impl_->nodes, mutation->maximum_nodes, maximum_intervals);
    mutation->staged_only.reserve(mutation->maximum_nodes);
    return Mutation(std::move(mutation));
}

ArtBumpIndex::Mutation::Mutation(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ArtBumpIndex::Mutation::Mutation(Mutation&& other) noexcept = default;

ArtBumpIndex::Mutation& ArtBumpIndex::Mutation::operator=(
    Mutation&& other) noexcept {
    if (this == &other) return *this;
    Abort();
    impl_ = std::move(other.impl_);
    return *this;
}

ArtBumpIndex::Mutation::~Mutation() noexcept { Abort(); }

std::optional<std::vector<std::uint8_t>> ArtBumpIndex::Mutation::Get(
    std::string_view key) const {
    if (!impl_ || !impl_->active || impl_->failed) {
        Raise(AllocatorErrorCode::InvalidArgument, "mutation is not active");
    }
    const auto snapshot = impl_->owner->state->LoadSnapshotAcquire();
    return GetAt(
        *impl_->owner->nodes,
        *impl_->owner->zones[impl_->baseline.active_zone],
        impl_->staged_root,
        snapshot.zones[impl_->baseline.active_zone].top,
        key);
}

std::vector<ArtBumpIndex::Entry> ArtBumpIndex::Mutation::Entries() const {
    return EntriesWithPrefix({});
}

std::vector<ArtBumpIndex::Entry>
ArtBumpIndex::Mutation::EntriesWithPrefix(std::string_view prefix) const {
    if (!impl_ || !impl_->active || impl_->failed) {
        Raise(AllocatorErrorCode::InvalidArgument, "mutation is not active");
    }
    const auto snapshot = impl_->owner->state->LoadSnapshotAcquire();
    return EntriesAt(
        *impl_->owner->nodes,
        *impl_->owner->zones[impl_->baseline.active_zone],
        impl_->staged_root,
        snapshot.zones[impl_->baseline.active_zone].top,
        prefix);
}

bool ArtBumpIndex::Mutation::Put(
    std::string_view key,
    const std::vector<std::uint8_t>& value) {
    if (!impl_) {
        Raise(AllocatorErrorCode::InvalidArgument, "mutation is not active");
    }
    ValidateInputValue(value);
    impl_->ValidateCurrent(true);
    const auto snapshot = impl_->owner->state->LoadSnapshotAcquire();
    bool changed = false;
    auto plan = PlanPut(
        *impl_->owner->nodes,
        *impl_->owner->zones[impl_->baseline.active_zone],
        impl_->staged_root,
        impl_->staged_entries,
        impl_->baseline.entry_limit,
        snapshot.zones[impl_->baseline.active_zone].top,
        key,
        value,
        impl_->maximum_nodes,
        &changed);
    if (!changed) return false;
    return impl_->ApplyUpdate(std::move(plan));
}

bool ArtBumpIndex::Mutation::Erase(std::string_view key) {
    if (!impl_) {
        Raise(AllocatorErrorCode::InvalidArgument, "mutation is not active");
    }
    impl_->ValidateCurrent(true);
    const auto snapshot = impl_->owner->state->LoadSnapshotAcquire();
    bool changed = false;
    auto plan = PlanErase(
        *impl_->owner->nodes,
        *impl_->owner->zones[impl_->baseline.active_zone],
        impl_->staged_root,
        impl_->staged_entries,
        snapshot.zones[impl_->baseline.active_zone].top,
        key,
        impl_->maximum_nodes,
        &changed);
    if (!changed) return false;
    return impl_->ApplyUpdate(std::move(plan));
}

void ArtBumpIndex::Mutation::Abort() noexcept {
    if (impl_) impl_->AbortNoexcept();
}

void ArtBumpIndex::Mutation::AbandonForRecovery() noexcept {
    if (!impl_) return;
    impl_->active = false;
    impl_->failed = true;
    impl_->staged_only.clear();
}

ArtBumpIndex::NodeRef ArtBumpIndex::Mutation::StagedRoot() const noexcept {
    return impl_ ? impl_->staged_root : 0;
}

std::uint64_t ArtBumpIndex::Mutation::StagedEntryCount() const noexcept {
    return impl_ ? impl_->staged_entries : 0;
}

struct ArtBumpIndex::PreparedCommit::Impl {
    std::unique_ptr<Mutation::Impl> mutation;
    ArtBumpDerivedState derived{};
    std::vector<NodeRef> reclaim_after_publish;
    bool applied = false;
};

ArtBumpIndex::PreparedCommit ArtBumpIndex::Mutation::PrepareCommit() && {
    if (!impl_ || !impl_->active || impl_->failed) {
        Raise(AllocatorErrorCode::InvalidArgument, "mutation is not active");
    }
    impl_->ValidateCurrent(true);

    auto prepared = std::make_unique<PreparedCommit::Impl>();
    ArtBumpIndex::GraphSummary final_summary;
    const auto snapshot = impl_->owner->state->LoadSnapshotAcquire();
    InspectTreeInto(
        *impl_->owner->nodes,
        impl_->owner->zones,
        impl_->staged_root,
        InspectOptions{
            impl_->baseline.active_zone,
            snapshot.zones[impl_->baseline.active_zone].top,
            impl_->baseline.entry_limit,
            false,
            true},
        &impl_->staged_scratch,
        &final_summary);
    prepared->derived = ArtBumpDerivedState{
        final_summary.node_count,
        final_summary.entry_count,
        final_summary.engine_live_bytes};

    prepared->reclaim_after_publish.reserve(
        impl_->committed.workspace.nodes.size());
    for (const auto& node : impl_->committed.workspace.nodes) {
        const auto kind_index = KindIndex(node.kind);
        const auto slot = impl_->owner->nodes->SlotIndex(node.reference);
        if (!BitmapBit(impl_->staged_scratch.seen_bits[kind_index], slot)) {
            prepared->reclaim_after_publish.push_back(node.reference);
        }
    }
    prepared->mutation = std::move(impl_);
    return PreparedCommit(std::move(prepared));
}

ArtBumpIndex::PreparedCommit::PreparedCommit(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ArtBumpIndex::PreparedCommit::PreparedCommit(
    PreparedCommit&& other) noexcept = default;

ArtBumpIndex::PreparedCommit& ArtBumpIndex::PreparedCommit::operator=(
    PreparedCommit&& other) noexcept {
    if (this == &other) return *this;
    if (impl_ && impl_->mutation) impl_->mutation->AbortNoexcept();
    impl_ = std::move(other.impl_);
    return *this;
}

ArtBumpIndex::PreparedCommit::~PreparedCommit() noexcept {
    if (impl_ && impl_->mutation && !impl_->applied) {
        impl_->mutation->AbortNoexcept();
    }
}

void ArtBumpIndex::PreparedCommit::Apply() && {
    if (!impl_ || !impl_->mutation || impl_->applied ||
        !impl_->mutation->active || impl_->mutation->failed) {
        Raise(AllocatorErrorCode::InvalidArgument, "commit plan is not active");
    }
    auto& mutation = *impl_->mutation;
    auto& state = *mutation.owner->state;
    state.StoreDerived(impl_->derived);
    state.AfterPersistentStep(
        ArtBumpApplyStep::MutationCountersStored, 0);
    state.StoreGeneration(mutation.baseline.generation + 1U);
    state.AfterPersistentStep(
        ArtBumpApplyStep::MutationGenerationStored, 0);
    state.StoreCommittedRootRelease(mutation.staged_root);
    state.AfterPersistentStep(
        ArtBumpApplyStep::MutationRootPublished, 0);

    // The semantic commit point has passed. Destructor rollback must never
    // reclaim final-tree nodes from this point onward.
    mutation.active = false;
    mutation.staged_only.clear();
    impl_->applied = true;
    for (const auto reference : impl_->reclaim_after_publish) {
        mutation.owner->nodes->ReclaimPublished(reference);
        state.AfterPersistentStep(
            ArtBumpApplyStep::MutationNodeReclaimed, reference);
    }
}

namespace {

struct RawCopy {
    std::uint64_t source_offset = 0;
    std::uint64_t target_offset = 0;
    std::size_t length = 0;
};

std::uint64_t TranslateRaw(
    const std::vector<RawCopy>& copies,
    std::uint64_t source_offset) {
    if (source_offset == 0) return 0;
    const auto found = std::lower_bound(
        copies.begin(), copies.end(), source_offset,
        [](const RawCopy& copy, std::uint64_t wanted) {
            return copy.source_offset < wanted;
        });
    if (found == copies.end() || found->source_offset != source_offset) {
        Raise(
            AllocatorErrorCode::Corrupt,
            "node references raw bytes outside the compact plan");
    }
    return found->target_offset;
}

std::size_t SourceNodeIndex(
    ArtBumpNodeStore& store,
    const std::array<std::vector<std::int32_t>, 4>& source_indices,
    NodeRef reference) {
    const auto kind_index = KindIndex(store.ReferenceKind(reference));
    const auto slot = static_cast<std::size_t>(store.SlotIndex(reference));
    if (slot >= source_indices[kind_index].size() ||
        source_indices[kind_index][slot] < 0) {
        Raise(AllocatorErrorCode::Corrupt, "child is absent from source tree");
    }
    return static_cast<std::size_t>(source_indices[kind_index][slot]);
}

void TranslateRecordChildrenNoAlloc(
    ArtBumpNodeRecord* record,
    ArtBumpNodeStore& store,
    const std::array<std::vector<std::int32_t>, 4>& source_indices,
    const std::vector<NodeRef>& clone_references) {
    const auto translate = [&](NodeRef child) -> NodeRef {
        if (child == 0) {
            Raise(AllocatorErrorCode::Corrupt, "compact source child is null");
        }
        const auto child_index = SourceNodeIndex(
            store, source_indices, child);
        const auto clone = clone_references[child_index];
        if (clone == 0) {
            Raise(
                AllocatorErrorCode::Corrupt,
                "compact clone is not postorder");
        }
        return clone;
    };

    switch (record->kind) {
    case ArtBumpNodeKind::Node4:
    case ArtBumpNodeKind::Node16:
    case ArtBumpNodeKind::Node48:
        for (std::size_t slot = 0; slot < record->child_count; ++slot) {
            record->children[slot] = translate(record->children[slot]);
        }
        break;
    case ArtBumpNodeKind::Node256:
        for (auto& child : record->children) {
            if (child != 0) child = translate(child);
        }
        break;
    }
}

} // namespace

struct ArtBumpIndex::PreparedCompact::Impl {
    ArtBumpIndex::Impl* owner = nullptr;
    ArtBumpStateSnapshot baseline{};
    TreeInspection source;
    InspectionWorkspace clone_workspace;
    std::vector<RawCopy> raw_copies;
    std::array<std::vector<std::int32_t>, 4> source_indices;
    std::vector<NodeRef> clone_references;
    ArtBumpNodeLiveBitmaps clone_bitmaps;
    ArtBumpNodeLiveBitCounts clone_bit_counts{};
    ArtBumpCompactJournal journal{};
    bool started = false;
    bool applied = false;
};

ArtBumpIndex::PreparedCompact ArtBumpIndex::PrepareCompact() {
    const auto snapshot = impl_->state->LoadSnapshotAcquire();
    ValidateIdleSnapshot(snapshot, impl_->zones);
    auto source = InspectTree(
        *impl_->nodes,
        impl_->zones,
        snapshot.committed_root,
        InspectOptions{
            snapshot.active_zone,
            snapshot.zones[snapshot.active_zone].top,
            snapshot.entry_limit,
            false,
            true});
    if (!SameDerived(snapshot.derived, source.summary)) {
        Raise(AllocatorErrorCode::Corrupt, "common counters disagree with ART graph");
    }
    ValidateStoreMatchesTree(*impl_->nodes, source.summary);

    auto prepared = std::make_unique<PreparedCompact::Impl>();
    prepared->owner = impl_.get();
    prepared->baseline = snapshot;
    prepared->source = std::move(source);
    const auto source_zone = snapshot.active_zone;
    const auto target_zone = static_cast<std::uint32_t>(1U - source_zone);
    auto target_cursor = snapshot.zones[target_zone].begin;
    prepared->raw_copies.reserve(
        prepared->source.workspace.intervals.size());
    for (const auto& interval : prepared->source.workspace.intervals) {
        const auto length64 = interval.end - interval.begin;
        const auto length = CheckedSize(
            length64, "compact raw interval exceeds address space");
        prepared->raw_copies.push_back(RawCopy{
            interval.begin, target_cursor, length});
        target_cursor = CheckedAdd(
            target_cursor,
            length64,
            AllocatorErrorCode::Capacity,
            "compact target top overflows");
    }
    const auto target_end = CheckedAdd(
        snapshot.zones[target_zone].begin,
        snapshot.zones[target_zone].bytes,
        AllocatorErrorCode::Corrupt,
        "target zone end overflows");
    if (target_cursor > target_end) {
        Raise(AllocatorErrorCode::Capacity, "target raw zone is too small");
    }

    for (std::size_t index = 0; index < 4; ++index) {
        const auto kind = static_cast<ArtBumpNodeKind>(index + 1U);
        const auto capacity = static_cast<std::uint64_t>(
            impl_->nodes->Capacity(kind));
        const auto used = static_cast<std::uint64_t>(
            impl_->nodes->UsedCount(kind));
        if (prepared->source.summary.slab_live_counts[index] >
            capacity - used) {
            Raise(
                AllocatorErrorCode::Capacity,
                "node slab cannot hold compact clone");
        }
        prepared->source_indices[index].assign(
            static_cast<std::size_t>(capacity), -1);
        prepared->clone_bitmaps[index].assign(
            (static_cast<std::size_t>(capacity) + 7U) / 8U, 0);
    }
    if (prepared->source.workspace.nodes.size() >
        static_cast<std::size_t>(INT32_MAX)) {
        Raise(AllocatorErrorCode::Capacity, "compact node map exceeds int32");
    }
    for (std::size_t index = 0;
         index < prepared->source.workspace.nodes.size(); ++index) {
        const auto& node = prepared->source.workspace.nodes[index];
        const auto kind_index = KindIndex(node.kind);
        const auto slot = impl_->nodes->SlotIndex(node.reference);
        prepared->source_indices[kind_index][slot] =
            static_cast<std::int32_t>(index);
    }
    prepared->clone_references.assign(
        prepared->source.workspace.nodes.size(), 0);
    const auto maximum_nodes = MaximumTreeNodes(snapshot.entry_limit);
    const auto maximum_intervals = maximum_nodes >
            std::numeric_limits<std::size_t>::max() / 2U
        ? std::numeric_limits<std::size_t>::max()
        : maximum_nodes * 2U;
    prepared->clone_workspace.Reserve(
        *impl_->nodes, maximum_nodes, maximum_intervals);

    prepared->journal.old_root = snapshot.committed_root;
    prepared->journal.source_top = snapshot.zones[source_zone].top;
    prepared->journal.operation_generation = snapshot.generation + 1U;
    prepared->journal.new_root = 0;
    prepared->journal.target_top = 0;
    prepared->journal.node_count = 0;
    prepared->journal.entry_count = 0;
    prepared->journal.engine_live_bytes = 0;
    prepared->journal.ready_checksum = 0;
    prepared->journal.state = ArtBumpJournalState::Copying;
    prepared->journal.source_zone = source_zone;
    prepared->journal.target_zone = target_zone;
    prepared->journal.source_epoch = snapshot.zones[source_zone].epoch;
    prepared->journal.target_epoch = NextEpoch(
        snapshot.zones[target_zone].epoch);
    prepared->journal.base_checksum = CompactBaseChecksum(prepared->journal);
    return PreparedCompact(std::move(prepared));
}

ArtBumpIndex::PreparedCompact::PreparedCompact(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ArtBumpIndex::PreparedCompact::PreparedCompact(
    PreparedCompact&& other) noexcept = default;

ArtBumpIndex::PreparedCompact& ArtBumpIndex::PreparedCompact::operator=(
    PreparedCompact&& other) noexcept = default;

ArtBumpIndex::PreparedCompact::~PreparedCompact() = default;

void ArtBumpIndex::PreparedCompact::Apply() && {
    if (!impl_ || impl_->started || impl_->applied) {
        Raise(AllocatorErrorCode::InvalidArgument, "compact plan is not active");
    }
    impl_->started = true;
    auto& store = *impl_->owner->nodes;
    auto& state = *impl_->owner->state;
    auto& source_zone = *impl_->owner->zones[impl_->journal.source_zone];
    auto& target_zone = *impl_->owner->zones[impl_->journal.target_zone];

    // Everything used below, including both validation workspaces and all
    // maps, was allocated by Prepare. This notification deliberately precedes
    // the first persistent journal-payload write.
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactBeforeJournalPayload, 0);
    state.WriteJournalCopyingPayload(impl_->journal);
    state.PublishJournalStateRelease(ArtBumpJournalState::Copying);
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactCopyingPublished, 0);
    target_zone.StoreEpoch(impl_->journal.target_epoch);
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactTargetEpochStored, 0);
    target_zone.StoreTopRelease(target_zone.Begin());
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactTargetTopStored, 0);

    std::uint64_t raw_ordinal = 0;
    for (const auto& copy : impl_->raw_copies) {
        const auto* bytes = source_zone.ReadBounded(
            copy.source_offset,
            copy.length,
            impl_->journal.source_top);
        const auto target = target_zone.Allocate(bytes, copy.length);
        if (target != copy.target_offset) {
            Raise(AllocatorErrorCode::Corrupt, "compact raw packing diverged");
        }
        state.AfterPersistentStep(
            ArtBumpApplyStep::CompactRawCopied, raw_ordinal++);
    }

    std::uint64_t node_ordinal = 0;
    for (std::size_t index = impl_->source.workspace.nodes.size();
         index != 0; --index) {
        const auto source_index = index - 1U;
        const auto source_ref =
            impl_->source.workspace.nodes[source_index].reference;
        auto record = store.Read(source_ref);
        record.prefix_offset = TranslateRaw(
            impl_->raw_copies, record.prefix_offset);
        if (record.has_value) {
            record.value_offset = TranslateRaw(
                impl_->raw_copies, record.value_offset);
        }
        TranslateRecordChildrenNoAlloc(
            &record,
            store,
            impl_->source_indices,
            impl_->clone_references);
        const auto clone_reference = store.Allocate(record);
        state.AfterPersistentStep(
            ArtBumpApplyStep::CompactNodeCloned, node_ordinal++);
        impl_->clone_references[source_index] = clone_reference;
        const auto clone_kind = store.ReferenceKind(clone_reference);
        const auto clone_kind_index = KindIndex(clone_kind);
        const auto clone_slot = store.SlotIndex(clone_reference);
        if (BitmapBit(impl_->clone_bitmaps[clone_kind_index], clone_slot)) {
            Raise(AllocatorErrorCode::Corrupt, "compact allocated duplicate slot");
        }
        SetBitmapBit(
            &impl_->clone_bitmaps[clone_kind_index], clone_slot);
        impl_->clone_bit_counts[clone_kind_index] = std::max(
            impl_->clone_bit_counts[clone_kind_index],
            static_cast<std::size_t>(clone_slot) + 1U);
    }

    NodeRef new_root = 0;
    if (impl_->journal.old_root != 0) {
        const auto root_index = SourceNodeIndex(
            store, impl_->source_indices, impl_->journal.old_root);
        new_root = impl_->clone_references[root_index];
        if (new_root == 0) {
            Raise(AllocatorErrorCode::Corrupt, "compact clone has no root");
        }
    }
    const auto target_top = target_zone.TopAcquire();
    ArtBumpIndex::GraphSummary clone_summary;
    InspectTreeInto(
        store,
        impl_->owner->zones,
        new_root,
        InspectOptions{
            impl_->journal.target_zone,
            target_top,
            impl_->baseline.entry_limit,
            false,
            false},
        &impl_->clone_workspace,
        &clone_summary);
    if (clone_summary.node_count != impl_->source.summary.node_count ||
        clone_summary.entry_count != impl_->source.summary.entry_count ||
        clone_summary.engine_live_bytes !=
            impl_->source.summary.engine_live_bytes) {
        Raise(AllocatorErrorCode::Corrupt, "compact clone differs from source");
    }

    // The bitmap storage was allocated before the first journal write. This
    // post-clone prepare only validates the now-known absolute clone slots and
    // moves those buffers; it performs no heap allocation or persistent write.
    auto slab_rebuild = std::move(store).PrepareRebuild(
        std::move(impl_->clone_bitmaps), impl_->clone_bit_counts);

    impl_->journal.new_root = new_root;
    impl_->journal.target_top = target_top;
    impl_->journal.node_count = clone_summary.node_count;
    impl_->journal.entry_count = clone_summary.entry_count;
    impl_->journal.engine_live_bytes = clone_summary.engine_live_bytes;
    impl_->journal.ready_checksum = CompactReadyChecksum(impl_->journal);
    impl_->journal.state = ArtBumpJournalState::Ready;
    constexpr std::array<ArtBumpReadyField, 6> kReadyFields = {
        ArtBumpReadyField::NewRoot,
        ArtBumpReadyField::TargetTop,
        ArtBumpReadyField::NodeCount,
        ArtBumpReadyField::EntryCount,
        ArtBumpReadyField::EngineLiveBytes,
        ArtBumpReadyField::ReadyChecksum,
    };
    for (std::size_t index = 0; index < kReadyFields.size(); ++index) {
        state.WriteJournalReadyField(impl_->journal, kReadyFields[index]);
        state.AfterPersistentStep(
            ArtBumpApplyStep::CompactReadyFieldStored,
            static_cast<std::uint64_t>(index));
    }
    state.PublishJournalStateRelease(ArtBumpJournalState::Ready);
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactReadyPublished, 0);

    target_zone.StoreEpoch(impl_->journal.target_epoch);
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactTargetEpochStored, 1);
    target_zone.StoreTopRelease(impl_->journal.target_top);
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactTargetTopStored, 1);
    state.StoreActiveZone(impl_->journal.target_zone);
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactActiveStored, 0);
    state.StoreDerived(ArtBumpDerivedState{
        impl_->journal.node_count,
        impl_->journal.entry_count,
        impl_->journal.engine_live_bytes});
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactCountersStored, 0);
    state.StoreGeneration(impl_->journal.operation_generation);
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactGenerationStored, 0);
    state.StoreCommittedRootRelease(impl_->journal.new_root);
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactRootPublished, 0);
    RebuildCutRelay compact_rebuild_relay{
        &state, ArtBumpApplyStep::CompactSlabRebuildWrite};
    store = std::move(slab_rebuild).Apply(
        &ReportRebuildCut, &compact_rebuild_relay);
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactSlabsRebuilt, 0);
    source_zone.StoreEpoch(impl_->journal.source_epoch);
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactSourceEpochStored, 0);
    source_zone.StoreTopRelease(source_zone.Begin());
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactSourceTopStored, 0);
    state.PublishJournalStateRelease(ArtBumpJournalState::Idle);
    state.AfterPersistentStep(
        ArtBumpApplyStep::CompactIdlePublished, 0);
    impl_->applied = true;
}

namespace {

enum class RecoveryBranch {
    Idle,
    Rollback,
    RollForward,
};

void ValidateZoneGeometryOnly(
    const ArtBumpStateSnapshot& snapshot,
    const std::array<ArtBumpRawZone*, 2>& zones) {
    for (std::size_t index = 0; index < zones.size(); ++index) {
        if (zones[index] == nullptr ||
            snapshot.zones[index].begin != zones[index]->Begin() ||
            snapshot.zones[index].bytes != zones[index]->Bytes() ||
            AddOverflows(
                snapshot.zones[index].begin,
                snapshot.zones[index].bytes)) {
            Raise(AllocatorErrorCode::Corrupt, "invalid raw-zone geometry");
        }
    }
}

void ValidateRecordedTop(
    const ArtBumpRawZoneDescriptor& zone,
    std::uint64_t recorded_top,
    const char* message) {
    if (recorded_top < zone.begin ||
        AddOverflows(zone.begin, zone.bytes) ||
        recorded_top > zone.begin + zone.bytes) {
        Raise(AllocatorErrorCode::Corrupt, message);
    }
}

void ValidatePackedIntervals(
    const InspectionWorkspace& workspace,
    std::uint64_t begin,
    std::uint64_t top) {
    auto cursor = begin;
    for (const auto& interval : workspace.intervals) {
        if (interval.begin != cursor) {
            Raise(
                AllocatorErrorCode::Corrupt,
                "compacted raw intervals are not tightly packed");
        }
        cursor = interval.end;
    }
    if (cursor != top) {
        Raise(
            AllocatorErrorCode::Corrupt,
            "compacted raw intervals do not reach target top");
    }
}

} // namespace

struct ArtBumpIndex::PreparedRecovery::Impl {
    ArtBumpIndex::Impl* owner = nullptr;
    ArtBumpStateSnapshot snapshot{};
    RecoveryBranch branch = RecoveryBranch::Idle;
    TreeInspection authoritative;
    ArtBumpDerivedState derived{};
    std::optional<ArtBumpNodeStore::PreparedRebuild> rebuild;
    std::uint32_t authoritative_zone = 0;
    std::uint64_t authoritative_top = 0;
    bool applied = false;

    void Cancel() noexcept {
        if (!rebuild.has_value() || applied) return;
        *owner->nodes = std::move(*rebuild).Cancel();
        rebuild.reset();
    }
};

ArtBumpIndex::PreparedRecovery ArtBumpIndex::PrepareRecovery() {
    const auto snapshot = impl_->state->LoadSnapshotAcquire();
    ValidateZoneGeometryOnly(snapshot, impl_->zones);

    RecoveryBranch branch = RecoveryBranch::Idle;
    std::uint32_t authoritative_zone = 0;
    std::uint64_t authoritative_top = 0;
    NodeRef authoritative_root = snapshot.committed_root;
    if (snapshot.journal.state == ArtBumpJournalState::Idle) {
        ValidateIdleSnapshot(snapshot, impl_->zones);
        authoritative_zone = snapshot.active_zone;
        authoritative_top = snapshot.zones[authoritative_zone].top;
    } else if (snapshot.journal.state == ArtBumpJournalState::Copying ||
               snapshot.journal.state == ArtBumpJournalState::Ready) {
        const auto& journal = snapshot.journal;
        if (journal.base_checksum != CompactBaseChecksum(journal)) {
            Raise(AllocatorErrorCode::Corrupt, "bad compact base checksum");
        }
        if (journal.source_zone > 1U || journal.target_zone > 1U ||
            journal.source_zone == journal.target_zone ||
            journal.source_epoch == 0U || journal.target_epoch == 0U) {
            Raise(AllocatorErrorCode::Corrupt, "invalid compact zone record");
        }
        ValidateRecordedTop(
            snapshot.zones[journal.source_zone],
            journal.source_top,
            "invalid compact source top");
        if (journal.state == ArtBumpJournalState::Copying) {
            // Every ready-only byte is deliberately ignored in COPYING.
            if (snapshot.committed_root != journal.old_root) {
                Raise(
                    AllocatorErrorCode::Corrupt,
                    "COPYING root does not equal old root");
            }
            branch = RecoveryBranch::Rollback;
            authoritative_root = journal.old_root;
            authoritative_zone = journal.source_zone;
            authoritative_top = journal.source_top;
        } else {
            if (journal.ready_checksum != CompactReadyChecksum(journal)) {
                Raise(AllocatorErrorCode::Corrupt, "bad compact READY checksum");
            }
            ValidateRecordedTop(
                snapshot.zones[journal.target_zone],
                journal.target_top,
                "invalid compact target top");
            if (journal.old_root == journal.new_root &&
                journal.old_root != 0) {
                Raise(
                    AllocatorErrorCode::Corrupt,
                    "nonempty READY roots may not be equal");
            }
            if ((journal.old_root == 0) != (journal.new_root == 0)) {
                Raise(
                    AllocatorErrorCode::Corrupt,
                    "READY old/new root emptiness differs");
            }
            if (journal.old_root == 0 && journal.new_root == 0 &&
                snapshot.committed_root == 0) {
                branch = RecoveryBranch::RollForward;
            } else if (snapshot.committed_root == journal.old_root) {
                branch = RecoveryBranch::Rollback;
            } else if (snapshot.committed_root == journal.new_root) {
                branch = RecoveryBranch::RollForward;
            } else {
                Raise(
                    AllocatorErrorCode::Corrupt,
                    "READY root matches neither journal root");
            }
            if (branch == RecoveryBranch::Rollback) {
                authoritative_root = journal.old_root;
                authoritative_zone = journal.source_zone;
                authoritative_top = journal.source_top;
            } else {
                authoritative_root = journal.new_root;
                authoritative_zone = journal.target_zone;
                authoritative_top = journal.target_top;
            }
        }
    } else {
        Raise(AllocatorErrorCode::Corrupt, "invalid compact journal state");
    }

    auto authoritative = InspectTree(
        *impl_->nodes,
        impl_->zones,
        authoritative_root,
        InspectOptions{
            authoritative_zone,
            authoritative_top,
            snapshot.entry_limit,
            true,
            true});
    if (branch == RecoveryBranch::RollForward &&
        (authoritative.summary.node_count != snapshot.journal.node_count ||
         authoritative.summary.entry_count != snapshot.journal.entry_count ||
         authoritative.summary.engine_live_bytes !=
             snapshot.journal.engine_live_bytes)) {
        Raise(
            AllocatorErrorCode::Corrupt,
            "READY counters disagree with authoritative new tree");
    }
    if (branch == RecoveryBranch::RollForward) {
        ValidatePackedIntervals(
            authoritative.workspace,
            snapshot.zones[authoritative_zone].begin,
            authoritative_top);
    }

    ArtBumpNodeLiveBitCounts bit_counts{};
    auto bitmaps = LiveBitmapsFor(
        *impl_->nodes, authoritative.workspace.nodes, &bit_counts);
    auto prepared = std::make_unique<PreparedRecovery::Impl>();
    prepared->owner = impl_.get();
    prepared->snapshot = snapshot;
    prepared->branch = branch;
    prepared->derived = ArtBumpDerivedState{
        authoritative.summary.node_count,
        authoritative.summary.entry_count,
        authoritative.summary.engine_live_bytes};
    prepared->authoritative_zone = authoritative_zone;
    prepared->authoritative_top = authoritative.summary.recovered_top;
    prepared->authoritative = std::move(authoritative);
    prepared->rebuild.emplace(
        std::move(*impl_->nodes).PrepareRebuild(
            std::move(bitmaps), bit_counts));
    return PreparedRecovery(std::move(prepared));
}

ArtBumpIndex::PreparedRecovery::PreparedRecovery(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ArtBumpIndex::PreparedRecovery::PreparedRecovery(
    PreparedRecovery&& other) noexcept = default;

ArtBumpIndex::PreparedRecovery& ArtBumpIndex::PreparedRecovery::operator=(
    PreparedRecovery&& other) noexcept {
    if (this == &other) return *this;
    if (impl_) impl_->Cancel();
    impl_ = std::move(other.impl_);
    return *this;
}

ArtBumpIndex::PreparedRecovery::~PreparedRecovery() {
    if (impl_) impl_->Cancel();
}

void ArtBumpIndex::PreparedRecovery::Apply() && {
    if (!impl_ || !impl_->rebuild.has_value() || impl_->applied) {
        Raise(AllocatorErrorCode::InvalidArgument, "recovery plan is not active");
    }
    auto& state = *impl_->owner->state;
    auto& zones = impl_->owner->zones;
    const auto& journal = impl_->snapshot.journal;
    if (impl_->branch == RecoveryBranch::Idle) {
        if (impl_->snapshot.committed_root == 0) {
            zones[0]->StoreEpoch(1);
            state.AfterPersistentStep(
                ArtBumpApplyStep::RecoveryIdleZoneEpochStored, 0);
            zones[0]->StoreTopRelease(zones[0]->Begin());
            state.AfterPersistentStep(
                ArtBumpApplyStep::RecoveryIdleZoneTopStored, 0);
            zones[1]->StoreEpoch(1);
            state.AfterPersistentStep(
                ArtBumpApplyStep::RecoveryIdleZoneEpochStored, 1);
            zones[1]->StoreTopRelease(zones[1]->Begin());
            state.AfterPersistentStep(
                ArtBumpApplyStep::RecoveryIdleZoneTopStored, 1);
            state.StoreActiveZone(0);
            state.AfterPersistentStep(
                ArtBumpApplyStep::RecoveryIdleActiveStored, 0);
        } else {
            const auto active = impl_->snapshot.active_zone;
            const auto inactive = static_cast<std::uint32_t>(1U - active);
            zones[active]->StoreTopRelease(impl_->authoritative_top);
            state.AfterPersistentStep(
                ArtBumpApplyStep::RecoveryIdleZoneTopStored, active);
            zones[inactive]->StoreTopRelease(zones[inactive]->Begin());
            state.AfterPersistentStep(
                ArtBumpApplyStep::RecoveryIdleZoneTopStored, inactive);
        }
        RebuildCutRelay rebuild_relay{
            &state, ArtBumpApplyStep::RecoveryIdleSlabRebuildWrite};
        *impl_->owner->nodes = std::move(*impl_->rebuild).Apply(
            &ReportRebuildCut, &rebuild_relay);
        impl_->rebuild.reset();
        state.AfterPersistentStep(
            ArtBumpApplyStep::RecoveryIdleSlabsRebuilt, 0);
        state.StoreDerived(
            impl_->snapshot.committed_root == 0
                ? ArtBumpDerivedState{}
                : impl_->derived);
        state.AfterPersistentStep(
            ArtBumpApplyStep::RecoveryIdleCountersStored, 0);
        impl_->applied = true;
        return;
    }

    if (impl_->branch == RecoveryBranch::Rollback) {
        zones[journal.source_zone]->StoreEpoch(journal.source_epoch);
        state.AfterPersistentStep(
            ArtBumpApplyStep::RecoveryRollbackSourceEpochStored, 0);
        zones[journal.source_zone]->StoreTopRelease(journal.source_top);
        state.AfterPersistentStep(
            ArtBumpApplyStep::RecoveryRollbackSourceTopStored, 0);
        zones[journal.target_zone]->StoreEpoch(journal.target_epoch);
        state.AfterPersistentStep(
            ArtBumpApplyStep::RecoveryRollbackTargetEpochStored, 0);
        zones[journal.target_zone]->StoreTopRelease(
            zones[journal.target_zone]->Begin());
        state.AfterPersistentStep(
            ArtBumpApplyStep::RecoveryRollbackTargetTopStored, 0);
        state.StoreActiveZone(journal.source_zone);
        state.AfterPersistentStep(
            ArtBumpApplyStep::RecoveryRollbackActiveStored, 0);
        state.StoreDerived(impl_->derived);
        state.AfterPersistentStep(
            ArtBumpApplyStep::RecoveryRollbackCountersStored, 0);
        state.StoreGeneration(journal.operation_generation - 1U);
        state.AfterPersistentStep(
            ArtBumpApplyStep::RecoveryRollbackGenerationStored, 0);
        state.StoreCommittedRootRelease(journal.old_root);
        state.AfterPersistentStep(
            ArtBumpApplyStep::RecoveryRollbackRootStored, 0);
        RebuildCutRelay rebuild_relay{
            &state, ArtBumpApplyStep::RecoveryRollbackSlabRebuildWrite};
        *impl_->owner->nodes = std::move(*impl_->rebuild).Apply(
            &ReportRebuildCut, &rebuild_relay);
        impl_->rebuild.reset();
        state.AfterPersistentStep(
            ArtBumpApplyStep::RecoveryRollbackSlabsRebuilt, 0);
        state.PublishJournalStateRelease(ArtBumpJournalState::Idle);
        state.AfterPersistentStep(
            ArtBumpApplyStep::RecoveryRollbackIdlePublished, 0);
        impl_->applied = true;
        return;
    }

    zones[journal.target_zone]->StoreEpoch(journal.target_epoch);
    state.AfterPersistentStep(
        ArtBumpApplyStep::RecoveryForwardTargetEpochStored, 0);
    zones[journal.target_zone]->StoreTopRelease(journal.target_top);
    state.AfterPersistentStep(
        ArtBumpApplyStep::RecoveryForwardTargetTopStored, 0);
    state.StoreActiveZone(journal.target_zone);
    state.AfterPersistentStep(
        ArtBumpApplyStep::RecoveryForwardActiveStored, 0);
    state.StoreDerived(impl_->derived);
    state.AfterPersistentStep(
        ArtBumpApplyStep::RecoveryForwardCountersStored, 0);
    state.StoreGeneration(journal.operation_generation);
    state.AfterPersistentStep(
        ArtBumpApplyStep::RecoveryForwardGenerationStored, 0);
    state.StoreCommittedRootRelease(journal.new_root);
    state.AfterPersistentStep(
        ArtBumpApplyStep::RecoveryForwardRootStored, 0);
    RebuildCutRelay rebuild_relay{
        &state, ArtBumpApplyStep::RecoveryForwardSlabRebuildWrite};
    *impl_->owner->nodes = std::move(*impl_->rebuild).Apply(
        &ReportRebuildCut, &rebuild_relay);
    impl_->rebuild.reset();
    state.AfterPersistentStep(
        ArtBumpApplyStep::RecoveryForwardSlabsRebuilt, 0);
    zones[journal.source_zone]->StoreEpoch(journal.source_epoch);
    state.AfterPersistentStep(
        ArtBumpApplyStep::RecoveryForwardSourceEpochStored, 0);
    zones[journal.source_zone]->StoreTopRelease(
        zones[journal.source_zone]->Begin());
    state.AfterPersistentStep(
        ArtBumpApplyStep::RecoveryForwardSourceTopStored, 0);
    state.PublishJournalStateRelease(ArtBumpJournalState::Idle);
    state.AfterPersistentStep(
        ArtBumpApplyStep::RecoveryForwardIdlePublished, 0);
    impl_->applied = true;
}

struct ArtBumpIndex::PreparedClear::Impl {
    ArtBumpIndex::Impl* owner = nullptr;
    ArtBumpStateSnapshot snapshot{};
    std::optional<ArtBumpNodeStore::PreparedRebuild> rebuild;
    std::array<std::optional<ArtBumpRawZone::PreparedState>, 2> raw_states;
    bool applied = false;

    void Cancel() noexcept {
        if (!rebuild.has_value() || applied) return;
        *owner->nodes = std::move(*rebuild).Cancel();
        rebuild.reset();
    }
};

ArtBumpIndex::PreparedClear ArtBumpIndex::PrepareClear() {
    const auto snapshot = impl_->state->LoadSnapshotAcquire();
    ValidateIdleSnapshot(snapshot, impl_->zones);
    auto old_tree = InspectTree(
        *impl_->nodes,
        impl_->zones,
        snapshot.committed_root,
        InspectOptions{
            snapshot.active_zone,
            snapshot.zones[snapshot.active_zone].top,
            snapshot.entry_limit,
            false,
            true});
    if (!SameDerived(snapshot.derived, old_tree.summary)) {
        Raise(AllocatorErrorCode::Corrupt, "common counters disagree with ART graph");
    }
    ValidateStoreMatchesTree(*impl_->nodes, old_tree.summary);

    ArtBumpNodeLiveBitmaps empty_bitmaps;
    ArtBumpNodeLiveBitCounts empty_counts{};
    for (std::size_t index = 0; index < empty_bitmaps.size(); ++index) {
        const auto kind = static_cast<ArtBumpNodeKind>(index + 1U);
        const auto capacity = static_cast<std::size_t>(
            impl_->nodes->Capacity(kind));
        empty_bitmaps[index].assign((capacity + 7U) / 8U, 0);
    }
    auto prepared = std::make_unique<PreparedClear::Impl>();
    prepared->owner = impl_.get();
    prepared->snapshot = snapshot;
    prepared->raw_states[0].emplace(
        impl_->zones[0]->PrepareState(impl_->zones[0]->Begin(), 1));
    prepared->raw_states[1].emplace(
        impl_->zones[1]->PrepareState(impl_->zones[1]->Begin(), 1));
    prepared->rebuild.emplace(
        std::move(*impl_->nodes).PrepareRebuild(
            std::move(empty_bitmaps), empty_counts));
    return PreparedClear(std::move(prepared));
}

ArtBumpIndex::PreparedClear::PreparedClear(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ArtBumpIndex::PreparedClear::PreparedClear(PreparedClear&& other) noexcept =
    default;

ArtBumpIndex::PreparedClear& ArtBumpIndex::PreparedClear::operator=(
    PreparedClear&& other) noexcept {
    if (this == &other) return *this;
    if (impl_) impl_->Cancel();
    impl_ = std::move(other.impl_);
    return *this;
}

ArtBumpIndex::PreparedClear::~PreparedClear() {
    if (impl_) impl_->Cancel();
}

void ArtBumpIndex::PreparedClear::Apply() && {
    if (!impl_ || !impl_->rebuild.has_value() || impl_->applied) {
        Raise(AllocatorErrorCode::InvalidArgument, "clear plan is not active");
    }
    auto& state = *impl_->owner->state;
    state.StoreDerived(ArtBumpDerivedState{});
    state.AfterPersistentStep(ArtBumpApplyStep::ClearCountersStored, 0);
    state.StoreCommittedRootRelease(0);
    state.AfterPersistentStep(ArtBumpApplyStep::ClearRootPublished, 0);
    RebuildCutRelay rebuild_relay{
        &state, ArtBumpApplyStep::ClearSlabRebuildWrite};
    *impl_->owner->nodes = std::move(*impl_->rebuild).Apply(
        &ReportRebuildCut, &rebuild_relay);
    impl_->rebuild.reset();
    state.AfterPersistentStep(ArtBumpApplyStep::ClearSlabsRebuilt, 0);
    impl_->raw_states[0]->Apply();
    state.AfterPersistentStep(ArtBumpApplyStep::ClearZoneReset, 0);
    impl_->raw_states[1]->Apply();
    state.AfterPersistentStep(ArtBumpApplyStep::ClearZoneReset, 1);
    state.StoreActiveZone(0);
    state.AfterPersistentStep(ArtBumpApplyStep::ClearActiveStored, 0);
    impl_->applied = true;
}

ArtBumpIndex::ArtBumpIndex(
    ArtBumpNodeStore& nodes,
    std::array<ArtBumpRawZone*, 2> raw_zones,
    ArtBumpIndexState& state)
    : impl_(std::make_unique<Impl>()) {
    impl_->nodes = &nodes;
    impl_->zones = raw_zones;
    impl_->state = &state;
}

ArtBumpIndex::~ArtBumpIndex() = default;

ArtBumpIndex::GraphSummary ArtBumpIndex::InspectCommitted() const {
    const auto snapshot = impl_->state->LoadSnapshotAcquire();
    ValidateIdleSnapshot(snapshot, impl_->zones);
    const auto inspection = InspectTree(
        *impl_->nodes,
        impl_->zones,
        snapshot.committed_root,
        InspectOptions{
            snapshot.active_zone,
            snapshot.zones[snapshot.active_zone].top,
            snapshot.entry_limit,
            false,
            true});
    if (!SameDerived(snapshot.derived, inspection.summary)) {
        Raise(AllocatorErrorCode::Corrupt, "common counters disagree with ART graph");
    }
    ValidateStoreMatchesTree(*impl_->nodes, inspection.summary);
    return inspection.summary;
}

std::optional<std::vector<std::uint8_t>> ArtBumpIndex::Get(
    std::string_view key) const {
    const auto snapshot = impl_->state->LoadSnapshotAcquire();
    ValidateIdleSnapshot(snapshot, impl_->zones);
    return GetAt(
        *impl_->nodes,
        *impl_->zones[snapshot.active_zone],
        snapshot.committed_root,
        snapshot.zones[snapshot.active_zone].top,
        key);
}

bool ArtBumpIndex::Exists(std::string_view key) const {
    return Get(key).has_value();
}

std::vector<ArtBumpIndex::Entry> ArtBumpIndex::Entries() const {
    const auto snapshot = impl_->state->LoadSnapshotAcquire();
    ValidateIdleSnapshot(snapshot, impl_->zones);
    return EntriesAt(
        *impl_->nodes,
        *impl_->zones[snapshot.active_zone],
        snapshot.committed_root,
        snapshot.zones[snapshot.active_zone].top,
        {});
}

std::vector<ArtBumpIndex::Entry> ArtBumpIndex::EntriesWithPrefix(
    std::string_view prefix) const {
    const auto snapshot = impl_->state->LoadSnapshotAcquire();
    ValidateIdleSnapshot(snapshot, impl_->zones);
    return EntriesAt(
        *impl_->nodes,
        *impl_->zones[snapshot.active_zone],
        snapshot.committed_root,
        snapshot.zones[snapshot.active_zone].top,
        prefix);
}

std::uint64_t ArtBumpIndex::CompactBaseChecksum(
    const ArtBumpCompactJournal& journal) noexcept {
    return ArtBumpHeaderCodec::BaseChecksum(journal);
}

std::uint64_t ArtBumpIndex::CompactReadyChecksum(
    const ArtBumpCompactJournal& journal) noexcept {
    return ArtBumpHeaderCodec::ReadyChecksum(journal);
}

std::uint32_t ArtBumpIndex::NextEpoch(std::uint32_t epoch) noexcept {
    const auto next = static_cast<std::uint32_t>(epoch + 1U);
    return next == 0U ? 1U : next;
}

} // namespace kvspace::detail
