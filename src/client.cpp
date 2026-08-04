#include "kvspace/client.h"
#include "kvspace/errors.h"

#include <string_view>

namespace kvspace {
namespace {

std::pair<std::string, std::string> splitKey(const std::string& key) {
    if (key.empty() || key.front() != '/') {
        throw ErrInvalidPath(key);
    }
    const bool directory = key.size() > 1 && key.back() == '/';
    const std::string_view clean = directory
        ? std::string_view(key).substr(0, key.size() - 1)
        : std::string_view(key);
    const std::size_t slash = clean.rfind('/');
    std::string prefix;
    if (slash == 0) {
        prefix = "/";
    } else {
        prefix.assign(clean.substr(0, slash + 1));
    }
    std::string child(clean.substr(slash + 1));
    if (directory) {
        child.push_back('/');
    }
    return {std::move(prefix), std::move(child)};
}

} // namespace

XValue Client::Get(const std::string& key) {
    auto [prefix, child] = splitKey(key);
    return Get(prefix, {child}).at(0);
}

void Client::Set(const std::string& key, const XValue& value) {
    Set(std::vector<KVPair>{{key, value}});
}

std::vector<XValue> Client::GetMany(const std::vector<std::string>& keys) {
    std::vector<XValue> values;
    values.reserve(keys.size());
    for (const auto& key : keys) {
        values.push_back(Get(key));
    }
    return values;
}

} // namespace kvspace
