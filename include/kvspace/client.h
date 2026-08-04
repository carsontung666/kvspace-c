#pragma once

#include "kvspace/xvalue.h"

#include <chrono>
#include <string>
#include <vector>

namespace kvspace {

struct KVPair {
    std::string key;
    XValue value;
};

// Client mirrors github.com/array2d/kvspace-go.KVSpace.
class Client {
public:
    virtual ~Client() = default;

    virtual std::vector<XValue> Get(
        const std::string& prefix,
        const std::vector<std::string>& keys) = 0;
    virtual void Set(const std::vector<KVPair>& pairs) = 0;
    virtual std::vector<std::string> List(
        const std::string& prefix,
        bool expand_ext) = 0;
    virtual void Del(const std::vector<std::string>& keys) = 0;
    virtual void DelTree(const std::string& prefix) = 0;
    virtual void Notify(const std::string& key, const XValue& value) = 0;
    virtual XValue Watch(
        const std::string& key,
        std::chrono::milliseconds timeout) = 0;
    virtual void Mkindex(const std::string& path) = 0;
    virtual void Link(const std::string& target, const std::string& link_path) = 0;
    virtual void ExtIndex(const std::string& path, const std::string& ext_path) = 0;
    virtual void Unlink(const std::string& path) = 0;
    virtual void Clear() = 0;
    virtual void Close() = 0;

    XValue Get(const std::string& key);
    void Set(const std::string& key, const XValue& value);
    std::vector<XValue> GetMany(const std::vector<std::string>& keys);
    void SetMany(const std::vector<KVPair>& pairs) { Set(pairs); }
    std::vector<std::string> List(const std::string& prefix) {
        return List(prefix, true);
    }
    void Del(const std::string& key) { Del(std::vector<std::string>{key}); }
    void UnLink(const std::string& path) { Unlink(path); }
};

} // namespace kvspace
