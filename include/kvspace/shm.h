#pragma once

#include "kvspace/client.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace kvspace {

enum class ShmEngine : std::uint32_t {
    ArtBump = 1,
    ArtBox = 2,
    HashBox = 3,
    TrieBox = 4,
};

struct ShmOptions {
    std::uint64_t initial_size = 16ULL * 1024 * 1024;
    std::uint64_t max_size = 1024ULL * 1024 * 1024;
    std::uint64_t max_entries = 65536;
    std::uint64_t max_queues = 4096;
    unsigned int permissions = 0600;
    ShmEngine engine = ShmEngine::HashBox;
};

struct ShmStats {
    ShmEngine engine = ShmEngine::HashBox;
    std::uint64_t region_size = 0;
    std::uint64_t region_max = 0;
    std::uint64_t entries = 0;
    std::uint64_t engine_nodes = 0;
    std::uint64_t max_entries = 0;
    std::uint64_t tombstones = 0;
    std::uint64_t queues = 0;
    std::uint64_t heap_used = 0;
    std::uint64_t heap_free = 0;
    std::uint64_t recoveries = 0;
};

class ShmClient final : public Client {
public:
    using Client::Del;
    using Client::Get;
    using Client::List;
    using Client::Set;

    static std::unique_ptr<ShmClient> Create(
        const std::string& name,
        const ShmOptions& options = {});
    static std::unique_ptr<ShmClient> Attach(const std::string& name);
    static std::unique_ptr<ShmClient> Attach(
        const std::string& name,
        ShmEngine expected_engine);
    static std::unique_ptr<ShmClient> Open(
        const std::string& name,
        const ShmOptions& options = {});
    static void Destroy(const std::string& name);

    ~ShmClient() override;
    ShmClient(ShmClient&&) noexcept;
    ShmClient& operator=(ShmClient&&) noexcept;
    ShmClient(const ShmClient&) = delete;
    ShmClient& operator=(const ShmClient&) = delete;

    std::vector<XValue> Get(
        const std::string& prefix,
        const std::vector<std::string>& keys) override;
    void Set(const std::vector<KVPair>& pairs) override;
    std::vector<std::string> List(
        const std::string& prefix,
        bool expand_ext) override;
    void Del(const std::vector<std::string>& keys) override;
    void DelTree(const std::string& prefix) override;
    void Notify(const std::string& key, const XValue& value) override;
    XValue Watch(
        const std::string& key,
        std::chrono::milliseconds timeout) override;
    void Mkindex(const std::string& path) override;
    void Link(const std::string& target, const std::string& link_path) override;
    void ExtIndex(const std::string& path, const std::string& ext_path) override;
    void Unlink(const std::string& path) override;
    void Clear() override;
    void Compact();
    void Close() override;

    ShmStats Stats();

private:
    struct Impl;
    explicit ShmClient(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace kvspace
