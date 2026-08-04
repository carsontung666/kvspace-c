#include "kvspace/shm.h"

#include "kvspace/constants.h"
#include "kvspace/errors.h"
#include "shm_region.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kvspace {
namespace {

bool isDirectory(std::string_view path) {
    return !path.empty() && path.back() == '/';
}

void validateAbsolutePath(std::string_view path) {
    if (path.empty() || path.front() != '/' || path.find("//") != std::string_view::npos) {
        throw ErrInvalidPath(std::string(path));
    }
    for (const char raw_byte : path) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        if (byte < 0x20U || byte == 0x7fU) {
            throw ErrInvalidPath(std::string(path));
        }
    }
    if (path == "/") return;
    std::size_t start = 1;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") {
            throw ErrInvalidPath(std::string(path));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
}

void validateDirectoryPath(std::string_view path) {
    validateAbsolutePath(path);
    if (!isDirectory(path)) throw ErrInvalidPath(std::string(path) + " must end with /");
}

void validateIndexChild(std::string_view child) {
    if (!child.empty() && child.back() == '/') child.remove_suffix(1);
    if (child.empty() || child == "." || child == ".." ||
        child.find('/') != std::string_view::npos) {
        throw ErrInvalidPath(std::string(child));
    }
    for (const char raw_byte : child) {
        const auto byte = static_cast<unsigned char>(raw_byte);
        if (byte < 0x20U || byte == 0x7fU) {
            throw ErrInvalidPath(std::string(child));
        }
    }
}

void validateChildKey(std::string_view key) {
    if (key.empty()) return;
    if (key.front() == '/') throw ErrInvalidPath(std::string(key));
    validateIndexChild(key);
}

std::string joinPath(std::string_view parent, std::string_view child) {
    std::string result(parent);
    if (parent == "/") {
        result.append(child);
    } else {
        if (result.empty() || result.back() != '/') result.push_back('/');
        result.append(child);
    }
    return result;
}

std::pair<std::string, std::string> parentName(std::string path) {
    if (isDirectory(path) && path != "/") path.pop_back();
    const auto slash = path.rfind('/');
    if (slash == std::string::npos) return {"", path};
    std::string parent = slash == 0 ? "/" : path.substr(0, slash + 1);
    return {std::move(parent), path.substr(slash + 1)};
}

std::string parentDirectory(const std::string& path) {
    std::string clean = path;
    if (isDirectory(clean) && clean != "/") clean.pop_back();
    return parentName(std::move(clean)).first;
}

std::string indexChild(std::string_view path, std::string name) {
    if (!name.empty() && isDirectory(path)) name.push_back('/');
    return name;
}

std::vector<std::string> normalizeIndex(const std::vector<std::string>& values) {
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    result.reserve(values.size());
    for (const auto& value : values) {
        if (!seen.insert(value).second) continue;
        result.push_back(value);
    }
    return result;
}

std::vector<std::string> without(
    const std::vector<std::string>& values,
    std::string_view target) {
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& value : values) {
        if (value != target) result.push_back(value);
    }
    return result;
}

bool hasPrefix(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::vector<std::string> pathParts(std::string_view path) {
    std::vector<std::string> result;
    if (path == "/") return result;
    if (path.front() == '/') path.remove_prefix(1);
    if (!path.empty() && path.back() == '/') path.remove_suffix(1);
    for (std::size_t start = 0; start < path.size();) {
        const auto end = path.find('/', start);
        result.emplace_back(path.substr(start, end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

} // namespace

struct ShmClient::Impl {
    explicit Impl(std::unique_ptr<detail::Region> mapped)
        : region(std::move(mapped)) {}

    std::unique_ptr<detail::Region> region;
    mutable std::shared_mutex lifecycle;
    std::atomic<bool> closing{false};
    bool closed = false;

    std::shared_lock<std::shared_mutex> begin() const {
        std::shared_lock<std::shared_mutex> call(lifecycle);
        if (closing.load(std::memory_order_acquire) || closed || region == nullptr) {
            throw ErrDisconnected();
        }
        return call;
    }

    std::optional<XValue> value(const std::string& key) const {
        std::vector<std::uint8_t> encoded;
        if (!region->Get(key, &encoded)) return std::nullopt;
        return XValue::Decode(encoded);
    }

    std::optional<std::vector<std::uint8_t>> encodedValue(const std::string& key) const {
        std::vector<std::uint8_t> encoded;
        if (!region->Get(key, &encoded)) return std::nullopt;
        return encoded;
    }

    void putValue(const std::string& key, const XValue& input) {
        region->Put(key, input.Encode());
    }

    bool hasKind(const std::string& key, std::string_view wanted) const {
        const auto found = value(key);
        return found.has_value() && found->Kind() == wanted;
    }

    std::optional<std::string> linkTarget(const std::string& key) const {
        const auto found = value(key);
        if (!found.has_value() || found->Kind() != kind::LinkIndex) return std::nullopt;
        return found->LinkTarget();
    }

    std::vector<std::string> readDirectoryIndex(const std::string& path) const {
        const auto found = value(path);
        if (!found.has_value()) return {};
        if (found->Kind() != kind::Index && found->Kind() != kind::ExtIndex) return {};
        return normalizeIndex(found->Children());
    }

    std::string resolveOne(const std::string& path, bool* changed) const {
        *changed = false;
        if (path == "/") return path;
        const auto parts = pathParts(path);
        std::string current = "/";
        for (std::size_t i = 0; i < parts.size(); ++i) {
            current = joinPath(current, parts[i]);
            std::string link_key = current;
            if (i + 1 < parts.size() || isDirectory(path)) link_key.push_back('/');
            const auto target = linkTarget(link_key);
            if (!target.has_value()) continue;
            *changed = true;
            if (i + 1 == parts.size()) return *target;
            std::string remaining;
            for (std::size_t j = i + 1; j < parts.size(); ++j) {
                if (!remaining.empty()) remaining.push_back('/');
                remaining += parts[j];
            }
            auto result = joinPath(*target, remaining);
            if (isDirectory(path) && !isDirectory(result)) result.push_back('/');
            return result;
        }
        return path;
    }

    std::string resolvePath(const std::string& input) const {
        std::string path = input;
        std::unordered_set<std::string> seen;
        for (std::size_t hops = 0;; ++hops) {
            if (!seen.insert(path).second) throw ErrResolve("link cycle at " + path);
            bool changed = false;
            auto resolved = resolveOne(path, &changed);
            if (!changed) return resolved;
            if (hops >= kMaxLinkResolutions) {
                throw ErrResolve("more than 64 link resolutions");
            }
            path = std::move(resolved);
        }
    }

    std::string resolveParent(const std::string& input) const {
        const bool directory = isDirectory(input) && input != "/";
        std::string clean = input;
        if (directory) clean.pop_back();
        const auto [parent, last] = parentName(clean);
        if (parent == clean) return input;
        auto result = joinPath(resolvePath(parent), last);
        if (directory) result.push_back('/');
        return result;
    }

    std::optional<std::string> extFallbackPath(const std::string& path) const {
        std::string directory = isDirectory(path) ? path : parentName(path).first;
        for (;;) {
            const auto found = value(directory);
            if (found.has_value() && found->Kind() == kind::ExtIndex) {
                const auto ext_path = found->ExtPath();
                if (ext_path.empty()) return std::nullopt;
                const auto target = resolvePath(ext_path);
                const auto relative = std::string_view(path).substr(directory.size());
                if (relative.empty()) return target;
                return resolvePath(joinPath(target, relative));
            }
            if (directory == "/") return std::nullopt;
            directory = parentDirectory(directory);
        }
    }

    bool entryExists(const std::string& parent, const std::string& name) const {
        return !name.empty() && region->Exists(joinPath(parent, name));
    }

    void writeDirectory(
        const std::string& path,
        std::string_view directory_kind,
        const std::vector<std::string>& children,
        const std::string& ext_path = {}) {
        if (directory_kind == kind::ExtIndex) {
            putValue(path, XValue::ExtIndex(children, ext_path));
        } else {
            putValue(path, XValue::Index(children));
        }
    }

    void addChild(const std::string& parent, const std::string& name) {
        if (name.empty()) return;
        const auto found = value(parent);
        std::vector<std::string> children;
        std::string directory_kind(kind::Index);
        std::string ext_path;
        if (found.has_value()) {
            if (found->Kind() != kind::Index && found->Kind() != kind::ExtIndex) {
                throw ErrNotDirectory(parent);
            }
            children = normalizeIndex(found->Children());
            directory_kind = found->Kind();
            if (found->Kind() == kind::ExtIndex) ext_path = found->ExtPath();
        }
        if (std::find(children.begin(), children.end(), name) != children.end()) return;
        children.push_back(name);
        writeDirectory(parent, directory_kind, children, ext_path);
    }

    void removeChild(const std::string& parent, const std::string& name) {
        if (name.empty()) return;
        const auto found = value(parent);
        if (!found.has_value()) return;
        if (found->Kind() == kind::Index) {
            putValue(parent, XValue::Index(without(found->Children(), name)));
        } else if (found->Kind() == kind::ExtIndex) {
            putValue(parent, XValue::ExtIndex(
                without(found->Children(), name), found->ExtPath()));
        }
    }

    struct ExtBinding {
        std::string local;
        std::string target;
    };

    std::vector<ExtBinding> extBindings() const {
        std::vector<ExtBinding> result;
        for (const auto& [key, encoded] : region->Entries()) {
            const auto decoded = XValue::Decode(encoded);
            if (decoded.Kind() == kind::ExtIndex) {
                result.push_back({key, resolvePath(decoded.ExtPath())});
            }
        }
        return result;
    }

    bool pathUnderExtIndex(std::string path) const {
        for (;;) {
            if (hasKind(path, kind::ExtIndex)) return true;
            if (path == "/") return false;
            path = parentDirectory(path);
        }
    }

    std::string firstExactSubtreeCollision(
        const std::string& local,
        const std::string& target) const {
        for (const auto& [key, ignored] : region->EntriesWithPrefix(local)) {
            (void)ignored;
            const auto relative = std::string_view(key).substr(local.size());
            if (relative.empty()) continue;
            if (region->Exists(joinPath(target, relative))) {
                const auto slash = relative.find('/');
                return std::string(relative.substr(0, slash));
            }
        }
        return {};
    }

    void validateExtTargets() const {
        const auto bindings = extBindings();
        for (std::size_t i = 0; i < bindings.size(); ++i) {
            const auto& binding = bindings[i];
            if (hasPrefix(binding.local, binding.target) ||
                pathUnderExtIndex(binding.target)) {
                throw ErrExtCascade(binding.local);
            }
            if (!hasKind(binding.target, kind::Index)) {
                throw ErrExtTarget(binding.target);
            }
            for (std::size_t j = 0; j < bindings.size(); ++j) {
                if (i != j && hasPrefix(bindings[j].local, binding.target)) {
                    throw ErrExtCascade(bindings[j].local);
                }
            }
        }
        for (const auto& binding : bindings) {
            std::unordered_set<std::string> local;
            for (const auto& child : readDirectoryIndex(binding.local)) local.insert(child);
            for (const auto& child : readDirectoryIndex(binding.target)) {
                if (local.count(child) != 0) throw ErrExtCollision(child);
            }
            const auto exact = firstExactSubtreeCollision(binding.local, binding.target);
            if (!exact.empty()) throw ErrExtCollision(exact);
        }
    }

    template <typename ErrorType>
    void checkExtMutation(const std::string& path) const {
        if (region->Exists(path)) return;
        const auto target = extFallbackPath(path);
        if (!target.has_value()) return;
        if (region->Exists(*target)) throw ErrorType(path);
        auto [parent, name] = parentName(*target);
        name = indexChild(*target, std::move(name));
        const auto children = readDirectoryIndex(parent);
        if (std::find(children.begin(), children.end(), name) != children.end()) {
            throw ErrorType(path);
        }
    }

    void checkExtTargetMutation(
        const std::string& path,
        const std::vector<std::string>& replacement,
        bool replace_index) const {
        for (const auto& binding : extBindings()) {
            if (!hasPrefix(path, binding.target)) continue;
            const auto relative = std::string_view(path).substr(binding.target.size());
            if (relative.empty()) {
                if (replace_index) {
                    std::unordered_set<std::string> local;
                    for (const auto& child : readDirectoryIndex(binding.local)) local.insert(child);
                    for (const auto& child : replacement) {
                        if (local.count(child) != 0) throw ErrExtCollision(child);
                    }
                }
                continue;
            }
            const auto slash = relative.find('/');
            auto name = std::string(relative.substr(0, slash));
            if (slash != std::string_view::npos) name.push_back('/');
            const auto local_children = readDirectoryIndex(binding.local);
            if (std::find(local_children.begin(), local_children.end(), name) !=
                local_children.end()) {
                throw ErrExtCollision(name);
            }
            auto local = joinPath(binding.local, relative);
            while (local != binding.local) {
                if (region->Exists(local)) throw ErrExtCollision(name);
                local = parentDirectory(local);
            }
        }
    }

    void ensureDirectory(const std::string& path) {
        if (!isDirectory(path)) throw ErrInvalidPath(path + " must end with /");
        auto ensure_one = [this](const std::string& parent,
                                 const std::string& name,
                                 const std::string& current) {
            const auto found = value(current);
            if (found.has_value()) {
                if (found->Kind() != kind::Index && found->Kind() != kind::ExtIndex) {
                    throw ErrNotDirectory(current);
                }
                if (!name.empty()) addChild(parent, indexChild(current, name));
                return;
            }
            if (!name.empty()) {
                checkExtTargetMutation(current, {}, false);
                checkExtMutation<ErrExtWrite>(current);
            }
            putValue(current, XValue::Index({}));
            if (!name.empty()) addChild(parent, indexChild(current, name));
        };

        ensure_one("/", "", "/");
        if (path == "/") return;
        std::string parent = "/";
        std::string current = "/";
        for (const auto& name : pathParts(path)) {
            current = joinPath(current, name) + "/";
            ensure_one(parent, name, current);
            parent = current;
        }
    }

    void canEnsureDirectory(const std::string& path) const {
        if (!isDirectory(path)) throw ErrInvalidPath(path + " must end with /");
        auto check = [this](const std::string& current, const std::string& name) {
            const auto found = value(current);
            if (found.has_value()) {
                if (found->Kind() != kind::Index && found->Kind() != kind::ExtIndex) {
                    throw ErrNotDirectory(current);
                }
                return;
            }
            if (!name.empty()) {
                checkExtTargetMutation(current, {}, false);
                checkExtMutation<ErrExtWrite>(current);
            }
        };

        check("/", "");
        if (path == "/") return;
        std::string current = "/";
        for (const auto& name : pathParts(path)) {
            current = joinPath(current, name) + "/";
            check(current, name);
        }
    }

    void replaceDirectory(
        const std::string& path,
        const std::vector<std::string>& children) {
        const auto found = value(path);
        if (found.has_value() && found->Kind() != kind::Index) {
            throw ErrInvalidDirectoryValue(path);
        }
        putValue(path, XValue::Index(children));
    }

    void eraseResolved(const std::string& resolved) {
        auto [parent, name] = parentName(resolved);
        name = indexChild(resolved, std::move(name));
        checkExtMutation<ErrExtDelete>(resolved);
        if (!region->Exists(resolved)) {
            if (!entryExists(parent, name)) removeChild(parent, name);
            return;
        }
        region->Erase(resolved);
        validateExtTargets();
        if (!entryExists(parent, name)) removeChild(parent, name);
    }
};

ShmClient::ShmClient(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ShmClient::~ShmClient() {
    try {
        Close();
    } catch (...) {
    }
}
ShmClient::ShmClient(ShmClient&&) noexcept = default;
ShmClient& ShmClient::operator=(ShmClient&&) noexcept = default;

std::unique_ptr<ShmClient> ShmClient::Create(
    const std::string& name,
    const ShmOptions& options) {
    return std::unique_ptr<ShmClient>(new ShmClient(std::make_unique<Impl>(
        detail::Region::Open(
            name, options, detail::OpenMode::Create, options.engine))));
}

std::unique_ptr<ShmClient> ShmClient::Attach(const std::string& name) {
    return std::unique_ptr<ShmClient>(new ShmClient(std::make_unique<Impl>(
        detail::Region::Open(name, {}, detail::OpenMode::Attach))));
}

std::unique_ptr<ShmClient> ShmClient::Attach(
    const std::string& name,
    ShmEngine expected_engine) {
    return std::unique_ptr<ShmClient>(new ShmClient(std::make_unique<Impl>(
        detail::Region::Open(
            name, {}, detail::OpenMode::Attach, expected_engine))));
}

std::unique_ptr<ShmClient> ShmClient::Open(
    const std::string& name,
    const ShmOptions& options) {
    return std::unique_ptr<ShmClient>(new ShmClient(std::make_unique<Impl>(
        detail::Region::Open(
            name, options, detail::OpenMode::Open, options.engine))));
}

void ShmClient::Destroy(const std::string& name) { detail::Region::Destroy(name); }

std::vector<XValue> ShmClient::Get(
    const std::string& prefix,
    const std::vector<std::string>& keys) {
    const auto call = impl_->begin();
    validateDirectoryPath(prefix);
    for (const auto& key : keys) validateChildKey(key);
    auto guard = impl_->region->Lock();
    const auto resolved_prefix = impl_->resolvePath(prefix);
    const auto ext_prefix = impl_->extFallbackPath(resolved_prefix);
    std::vector<XValue> result;
    result.reserve(keys.size());
    for (const auto& key : keys) {
        const auto full = impl_->resolvePath(joinPath(resolved_prefix, key));
        const auto local = impl_->value(full);
        if (local.has_value()) {
            result.push_back(*local);
            continue;
        }
        if (ext_prefix.has_value()) {
            const auto target = impl_->resolvePath(joinPath(*ext_prefix, key));
            const auto extended = impl_->value(target);
            if (extended.has_value()) {
                result.push_back(*extended);
                continue;
            }
        }
        result.push_back(XValue::Null());
    }
    return result;
}

void ShmClient::Set(const std::vector<KVPair>& pairs) {
    const auto call = impl_->begin();
    struct Prepared {
        std::string key;
        XValue value;
        std::vector<std::string> children;
        bool directory = false;
    };
    std::vector<Prepared> prepared;
    std::exception_ptr validation_error;
    prepared.reserve(pairs.size());
    for (const auto& pair : pairs) {
        try {
            validateAbsolutePath(pair.key);
            const auto encoded = pair.value.Encode();
            const auto decoded = XValue::Decode(encoded);
            if (decoded != pair.value ||
                (!pair.value.IsNull() && !XValue::IsKnownKind(pair.value.Kind()))) {
                throw ErrInvalidValue(pair.key);
            }
            Prepared item{pair.key, pair.value, {}, isDirectory(pair.key)};
            if (item.directory) {
                if (pair.value.Kind() != kind::Index) {
                    throw ErrInvalidDirectoryValue(pair.key);
                }
                item.children = normalizeIndex(pair.value.Children());
                for (const auto& child : item.children) validateIndexChild(child);
            } else if (pair.value.Kind() == kind::Index ||
                       pair.value.Kind() == kind::LinkIndex ||
                       pair.value.Kind() == kind::ExtIndex) {
                throw ErrInvalidValue(pair.key + " uses a reserved kind");
            }
            prepared.push_back(std::move(item));
        } catch (...) {
            validation_error = std::current_exception();
            break;
        }
    }

    if (prepared.empty() && validation_error != nullptr) {
        std::rethrow_exception(validation_error);
    }

    auto guard = impl_->region->Lock();
    for (const auto& pair : prepared) {
        auto mutation = impl_->region->BeginMutation();
        const auto resolved = impl_->resolvePath(pair.key);
        impl_->checkExtTargetMutation(resolved, pair.children, pair.directory);
        auto [parent, name] = parentName(resolved);
        impl_->ensureDirectory(parent);
        impl_->checkExtMutation<ErrExtWrite>(resolved);
        if (pair.directory) {
            impl_->replaceDirectory(resolved, pair.children);
        } else {
            impl_->putValue(resolved, pair.value);
        }
        impl_->addChild(parent, indexChild(resolved, std::move(name)));
        mutation.Commit();
    }
    if (validation_error != nullptr) std::rethrow_exception(validation_error);
}

std::vector<std::string> ShmClient::List(
    const std::string& prefix,
    bool expand_ext) {
    const auto call = impl_->begin();
    validateDirectoryPath(prefix);
    auto guard = impl_->region->Lock();
    const auto resolved = impl_->resolvePath(prefix);
    auto local = impl_->readDirectoryIndex(resolved);
    if (!expand_ext) return local;
    const auto target = impl_->extFallbackPath(resolved);
    if (!target.has_value()) return local;
    std::unordered_set<std::string> seen(local.begin(), local.end());
    for (const auto& child : impl_->readDirectoryIndex(*target)) {
        if (seen.insert(child).second) local.push_back(child);
    }
    return local;
}

void ShmClient::Del(const std::vector<std::string>& keys) {
    const auto call = impl_->begin();
    auto guard = impl_->region->Lock();
    for (const auto& key : keys) {
        auto mutation = impl_->region->BeginMutation();
        validateAbsolutePath(key);
        auto resolved = impl_->resolveParent(key);
        if (!impl_->region->Exists(resolved) && !isDirectory(key)) {
            const auto directory = impl_->resolveParent(key + "/");
            const auto directory_value = impl_->value(directory);
            if (directory_value.has_value() &&
                directory_value->Kind() == kind::LinkIndex) {
                resolved = directory;
            }
        }
        impl_->eraseResolved(resolved);
        mutation.Commit();
    }
}

void ShmClient::DelTree(const std::string& prefix) {
    const auto call = impl_->begin();
    validateAbsolutePath(prefix);
    auto guard = impl_->region->Lock();
    auto mutation = impl_->region->BeginMutation();
    const auto final_path = impl_->resolveParent(prefix);
    const auto final_value = impl_->value(final_path);
    if (final_value.has_value() && final_value->Kind() == kind::LinkIndex) {
        impl_->eraseResolved(final_path);
        mutation.Commit();
        return;
    }
    const auto resolved = impl_->resolvePath(prefix);
    impl_->checkExtMutation<ErrExtDelete>(resolved);
    auto [parent, name] = parentName(resolved);

    std::vector<std::string> doomed;
    if (isDirectory(resolved)) {
        for (const auto& entry : impl_->region->EntriesWithPrefix(resolved)) {
            doomed.push_back(entry.first);
        }
    } else {
        if (impl_->region->Exists(resolved)) doomed.push_back(resolved);
        for (const auto& entry :
             impl_->region->EntriesWithPrefix(resolved + "/")) {
            doomed.push_back(entry.first);
        }
    }

    std::unordered_map<std::string, std::unordered_set<std::string>> parents;
    for (const auto& key : doomed) {
        auto [candidate_parent, child] = parentName(key);
        child = indexChild(key, std::move(child));
        if (!child.empty()) {
            parents[std::move(candidate_parent)].insert(std::move(child));
        }
        impl_->region->Erase(key);
    }
    impl_->validateExtTargets();
    for (const auto& [candidate_parent, children] : parents) {
        for (const auto& child : children) {
            if (!impl_->entryExists(candidate_parent, child)) {
                impl_->removeChild(candidate_parent, child);
            }
        }
    }
    if (doomed.empty()) {
        name = indexChild(resolved, std::move(name));
        if (!impl_->entryExists(parent, name)) {
            impl_->removeChild(parent, name);
        }
    }
    mutation.Commit();
}

void ShmClient::Notify(const std::string& key, const XValue& value) {
    const auto call = impl_->begin();
    validateAbsolutePath(key);
    const auto encoded = value.Encode();
    if (XValue::Decode(encoded) != value ||
        (!value.IsNull() && !XValue::IsKnownKind(value.Kind()))) {
        throw ErrInvalidValue(key);
    }
    auto guard = impl_->region->Lock();
    impl_->region->Notify(impl_->resolvePath(key), encoded);
}

XValue ShmClient::Watch(
    const std::string& key,
    std::chrono::milliseconds timeout) {
    const auto call = impl_->begin();
    validateAbsolutePath(key);
    auto guard = impl_->region->Lock();
    const auto resolved = impl_->resolvePath(key);
    std::vector<std::uint8_t> encoded;
    if (!impl_->region->Watch(guard, resolved, timeout, &encoded, [this] {
            return impl_->closing.load(std::memory_order_acquire);
        })) {
        return XValue::Null();
    }
    return XValue::Decode(encoded);
}

void ShmClient::Mkindex(const std::string& path) {
    const auto call = impl_->begin();
    validateDirectoryPath(path);
    auto guard = impl_->region->Lock();
    auto mutation = impl_->region->BeginMutation();
    impl_->ensureDirectory(impl_->resolvePath(path));
    mutation.Commit();
}

void ShmClient::Link(const std::string& target, const std::string& link_path) {
    const auto call = impl_->begin();
    validateAbsolutePath(target);
    validateAbsolutePath(link_path);
    if (isDirectory(target) != isDirectory(link_path)) {
        throw ErrLinkTypeMismatch(target + " -> " + link_path);
    }
    auto guard = impl_->region->Lock();
    auto mutation = impl_->region->BeginMutation();
    const auto resolved = impl_->resolveParent(link_path);
    auto [parent, name] = parentName(resolved);
    impl_->checkExtMutation<ErrExtWrite>(resolved);
    impl_->checkExtTargetMutation(resolved, {}, false);
    const auto old = impl_->value(resolved);
    if (old.has_value()) {
        const auto& decoded = *old;
        if (decoded.Kind() != kind::LinkIndex) throw ErrLinkPathExists(resolved);
    }
    impl_->putValue(resolved, XValue::LinkIndex(target));
    (void)impl_->resolvePath(resolved);
    impl_->validateExtTargets();
    impl_->canEnsureDirectory(parent);
    impl_->ensureDirectory(parent);
    impl_->addChild(parent, indexChild(resolved, std::move(name)));
    impl_->validateExtTargets();
    mutation.Commit();
}

void ShmClient::ExtIndex(const std::string& path, const std::string& ext_path) {
    const auto call = impl_->begin();
    validateDirectoryPath(path);
    validateDirectoryPath(ext_path);
    auto guard = impl_->region->Lock();
    auto mutation = impl_->region->BeginMutation();
    const auto resolved = impl_->resolveParent(path);
    auto [parent, name] = parentName(resolved);
    impl_->checkExtMutation<ErrExtWrite>(resolved);
    impl_->checkExtTargetMutation(resolved, {}, false);
    std::vector<std::string> local;
    if (const auto current = impl_->value(resolved); current.has_value()) {
        if (current->Kind() != kind::Index && current->Kind() != kind::ExtIndex) {
            throw ErrNotDirectory(resolved);
        }
        local = normalizeIndex(current->Children());
    }
    impl_->putValue(resolved, XValue::ExtIndex(local, ext_path));
    impl_->validateExtTargets();
    impl_->canEnsureDirectory(parent);
    impl_->ensureDirectory(parent);
    impl_->addChild(parent, indexChild(resolved, std::move(name)));
    impl_->validateExtTargets();
    mutation.Commit();
}

void ShmClient::Unlink(const std::string& path) {
    const auto call = impl_->begin();
    validateAbsolutePath(path);
    auto guard = impl_->region->Lock();
    auto mutation = impl_->region->BeginMutation();
    auto resolved = impl_->resolveParent(path);
    auto current = impl_->value(resolved);
    if ((!current.has_value() ||
         (current->Kind() != kind::LinkIndex && current->Kind() != kind::ExtIndex)) &&
        !isDirectory(path)) {
        const auto directory = impl_->resolveParent(path + "/");
        const auto directory_value = impl_->value(directory);
        if (directory_value.has_value() &&
            (directory_value->Kind() == kind::LinkIndex ||
             directory_value->Kind() == kind::ExtIndex)) {
            resolved = directory;
            current = directory_value;
        }
    }
    if (!current.has_value()) return;
    if (current->Kind() == kind::LinkIndex) {
        impl_->eraseResolved(resolved);
    } else if (current->Kind() == kind::ExtIndex) {
        impl_->putValue(resolved, XValue::Index(current->Children()));
    }
    mutation.Commit();
}

void ShmClient::Clear() {
    const auto call = impl_->begin();
    auto guard = impl_->region->Lock();
    impl_->region->Clear();
}

void ShmClient::Compact() {
    const auto call = impl_->begin();
    auto guard = impl_->region->Lock();
    impl_->region->CompactValues();
}

void ShmClient::Close() {
    if (impl_ == nullptr) return;
    bool expected = false;
    if (!impl_->closing.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        return;
    }
    impl_->region->WakeAll();
    std::unique_lock<std::shared_mutex> close_guard(impl_->lifecycle);
    impl_->region->Close();
    impl_->closed = true;
}

ShmStats ShmClient::Stats() {
    const auto call = impl_->begin();
    auto guard = impl_->region->Lock();
    return impl_->region->Stats();
}

} // namespace kvspace
