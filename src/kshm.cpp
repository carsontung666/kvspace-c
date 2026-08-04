#include "kvspace/kshm.h"

#include "kvspace/errors.h"
#include "kvspace/shm.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

struct kshm {
    std::unique_ptr<kvspace::ShmClient> client;
};

namespace {

constexpr std::size_t kLastErrorCapacity = 1024;
thread_local char last_error[kLastErrorCapacity] = {};

void clearError() noexcept { last_error[0] = '\0'; }

int remember(kshm_status_t status, const char* message) noexcept {
    const char* source = message == nullptr ? "kvspace: unknown error" : message;
    std::size_t copied = 0;
    while (copied + 1 < kLastErrorCapacity && source[copied] != '\0') {
        last_error[copied] = source[copied];
        ++copied;
    }
    last_error[copied] = '\0';
    return static_cast<int>(status);
}

int translateException() noexcept {
    try {
        throw;
    } catch (const kvspace::ErrInvalidPath& error) {
        return remember(KSHM_ERR_INVALID_PATH, error.what());
    } catch (const kvspace::ErrInvalidValue& error) {
        return remember(KSHM_ERR_INVALID_VALUE, error.what());
    } catch (const kvspace::ErrInvalidDirectoryValue& error) {
        return remember(KSHM_ERR_INVALID_DIRECTORY_VALUE, error.what());
    } catch (const kvspace::ErrNotDirectory& error) {
        return remember(KSHM_ERR_NOT_DIRECTORY, error.what());
    } catch (const kvspace::ErrDisconnected& error) {
        return remember(KSHM_ERR_DISCONNECTED, error.what());
    } catch (const kvspace::ErrCapacity& error) {
        return remember(KSHM_ERR_CAPACITY, error.what());
    } catch (const kvspace::ErrCorruptRegion& error) {
        return remember(KSHM_ERR_CORRUPT, error.what());
    } catch (const kvspace::ErrVersionMismatch& error) {
        return remember(KSHM_ERR_VERSION, error.what());
    } catch (const kvspace::ErrEngineMismatch& error) {
        return remember(KSHM_ERR_ENGINE_MISMATCH, error.what());
    } catch (const kvspace::ErrUnsupportedEngine& error) {
        return remember(KSHM_ERR_UNSUPPORTED_ENGINE, error.what());
    } catch (const kvspace::ErrAlreadyExists& error) {
        return remember(KSHM_ERR_EXISTS, error.what());
    } catch (const kvspace::ErrNotFound& error) {
        return remember(KSHM_ERR_NOT_FOUND, error.what());
    } catch (const kvspace::ErrResolve& error) {
        return remember(KSHM_ERR_RESOLVE, error.what());
    } catch (const kvspace::ErrExtWrite& error) {
        return remember(KSHM_ERR_EXT_WRITE, error.what());
    } catch (const kvspace::ErrExtDelete& error) {
        return remember(KSHM_ERR_EXT_DELETE, error.what());
    } catch (const kvspace::ErrExtCascade& error) {
        return remember(KSHM_ERR_EXT_CASCADE, error.what());
    } catch (const kvspace::ErrExtTarget& error) {
        return remember(KSHM_ERR_EXT_TARGET, error.what());
    } catch (const kvspace::ErrExtCollision& error) {
        return remember(KSHM_ERR_EXT_COLLISION, error.what());
    } catch (const kvspace::ErrLinkTypeMismatch& error) {
        return remember(KSHM_ERR_LINK_TYPE, error.what());
    } catch (const kvspace::ErrLinkPathExists& error) {
        return remember(KSHM_ERR_LINK_EXISTS, error.what());
    } catch (const std::invalid_argument& error) {
        return remember(KSHM_ERR_INVALID_ARGUMENT, error.what());
    } catch (const kvspace::Error& error) {
        return remember(KSHM_ERR_INTERNAL, error.what());
    } catch (const std::exception& error) {
        return remember(KSHM_ERR_INTERNAL, error.what());
    } catch (...) {
        return remember(KSHM_ERR_INTERNAL, "kvspace: unknown exception");
    }
}

template <typename Function>
int invoke(Function&& function) noexcept {
    clearError();
    try {
        std::forward<Function>(function)();
        return KSHM_OK;
    } catch (...) {
        return translateException();
    }
}

template <typename Function>
void invokeVoid(Function&& function) noexcept {
    clearError();
    try {
        std::forward<Function>(function)();
    } catch (...) {
        (void)translateException();
    }
}

template <typename Function>
kshm_t* openHandle(Function&& function) noexcept {
    clearError();
    try {
        auto handle = std::make_unique<kshm_t>();
        handle->client = std::forward<Function>(function)();
        return handle.release();
    } catch (...) {
        (void)translateException();
        return nullptr;
    }
}

kvspace::ShmOptions defaultOptions(size_t initial_size) {
    kvspace::ShmOptions options;
    if (initial_size != 0) options.initial_size = initial_size;
    return options;
}

kvspace::ShmEngine convertEngine(std::uint32_t input) {
    switch (input) {
    case KSHM_ENGINE_ART_BUMP:
        return kvspace::ShmEngine::ArtBump;
    case KSHM_ENGINE_ART_BOX:
        return kvspace::ShmEngine::ArtBox;
    case KSHM_ENGINE_HASH_BOX:
        return kvspace::ShmEngine::HashBox;
    case KSHM_ENGINE_TRIE_BOX:
        return kvspace::ShmEngine::TrieBox;
    default:
        throw kvspace::ErrUnsupportedEngine(std::to_string(input));
    }
}

kvspace::ShmOptions convertOptions(const kshm_options_t* input) {
    if (input == nullptr) return {};
    if (input->struct_size != sizeof(*input)) {
        throw std::invalid_argument("kvspace: incompatible kshm_options_t size");
    }
    kvspace::ShmOptions options;
    options.initial_size = input->initial_size;
    options.max_size = input->max_size;
    options.max_entries = input->max_entries;
    options.max_queues = input->max_queues;
    options.permissions = input->permissions;
    options.engine = convertEngine(input->engine_id);
    return options;
}

kvspace::ShmClient& requireHandle(kshm_t* handle) {
    if (handle == nullptr || handle->client == nullptr) {
        throw kvspace::ErrDisconnected();
    }
    return *handle->client;
}

const char* requireString(const char* value, const char* field) {
    if (value == nullptr) {
        throw std::invalid_argument(std::string("kvspace: null ") + field);
    }
    return value;
}

kvspace::XValue decodeValue(const uint8_t* value, size_t value_len) {
    auto decoded = kvspace::XValue::Decode(value, value_len);
    const auto canonical = decoded.Encode();
    if (canonical.size() != value_len ||
        (value_len != 0 && std::memcmp(canonical.data(), value, value_len) != 0)) {
        throw kvspace::ErrInvalidValue("non-canonical TLV frame");
    }
    if (!decoded.IsNull() && !kvspace::XValue::IsKnownKind(decoded.Kind())) {
        throw kvspace::ErrInvalidValue("unsupported TLV kind");
    }
    return decoded;
}

void copyValue(const kvspace::XValue& source, kshm_value_t* destination) {
    const auto encoded = source.Encode();
    destination->data = nullptr;
    destination->len = 0;
    if (encoded.empty()) return;
    auto* data = static_cast<uint8_t*>(std::malloc(encoded.size()));
    if (data == nullptr) throw std::bad_alloc();
    std::memcpy(data, encoded.data(), encoded.size());
    destination->data = data;
    destination->len = encoded.size();
}

void copyString(const std::string& source, kshm_string_t* destination) {
    destination->data = nullptr;
    destination->len = 0;
    auto* data = static_cast<char*>(std::malloc(source.size() + 1));
    if (data == nullptr) throw std::bad_alloc();
    std::memcpy(data, source.data(), source.size());
    data[source.size()] = '\0';
    destination->data = data;
    destination->len = source.size();
}

} // namespace

extern "C" {

void kshm_options_init(kshm_options_t* options) {
    invokeVoid([&] {
        if (options == nullptr) {
            (void)remember(KSHM_ERR_INVALID_ARGUMENT, "kvspace: null options");
            return;
        }
        const kvspace::ShmOptions defaults;
        options->struct_size = sizeof(*options);
        options->engine_id = static_cast<std::uint32_t>(defaults.engine);
        options->initial_size = defaults.initial_size;
        options->max_size = defaults.max_size;
        options->max_entries = defaults.max_entries;
        options->max_queues = defaults.max_queues;
        options->permissions = defaults.permissions;
    });
}

kshm_t* kshm_create(const char* name, size_t initial_size) {
    return openHandle([&] {
        return kvspace::ShmClient::Create(
            requireString(name, "name"), defaultOptions(initial_size));
    });
}

kshm_t* kshm_create_with_options(
    const char* name, const kshm_options_t* options) {
    return openHandle([&] {
        return kvspace::ShmClient::Create(
            requireString(name, "name"), convertOptions(options));
    });
}

kshm_t* kshm_attach(const char* name) {
    return openHandle([&] {
        return kvspace::ShmClient::Attach(requireString(name, "name"));
    });
}

kshm_t* kshm_attach_with_engine(
    const char* name, kshm_engine_t expected_engine) {
    return openHandle([&] {
        return kvspace::ShmClient::Attach(
            requireString(name, "name"),
            convertEngine(static_cast<std::uint32_t>(expected_engine)));
    });
}

kshm_t* kshm_open(const char* name, size_t initial_size) {
    return openHandle([&] {
        return kvspace::ShmClient::Open(
            requireString(name, "name"), defaultOptions(initial_size));
    });
}

kshm_t* kshm_open_with_options(
    const char* name, const kshm_options_t* options) {
    return openHandle([&] {
        return kvspace::ShmClient::Open(
            requireString(name, "name"), convertOptions(options));
    });
}

int kshm_disconnect(kshm_t* handle) {
    return invoke([&] { requireHandle(handle).Close(); });
}

void kshm_close(kshm_t* handle) {
    if (handle == nullptr) return;
    (void)kshm_disconnect(handle);
    delete handle;
}

int kshm_destroy(const char* name) {
    return invoke([&] {
        kvspace::ShmClient::Destroy(requireString(name, "name"));
    });
}

const char* kshm_last_error(void) { return last_error; }

int kshm_get(
    kshm_t* handle,
    const char* prefix,
    const char* const* keys,
    size_t nkeys,
    kshm_value_list_t* out) {
    if (out != nullptr) {
        out->items = nullptr;
        out->len = 0;
    }
    return invoke([&] {
        if (out == nullptr) throw std::invalid_argument("kvspace: null get output");
        if (nkeys != 0 && keys == nullptr) {
            throw std::invalid_argument("kvspace: null key array");
        }
        std::vector<std::string> input;
        input.reserve(nkeys);
        for (size_t i = 0; i < nkeys; ++i) {
            input.emplace_back(requireString(keys[i], "key"));
        }
        const auto values = requireHandle(handle).Get(
            requireString(prefix, "prefix"), input);
        std::unique_ptr<kshm_value_t, decltype(&std::free)> items(
            static_cast<kshm_value_t*>(std::calloc(values.size(), sizeof(kshm_value_t))),
            &std::free);
        if (!values.empty() && items == nullptr) throw std::bad_alloc();
        size_t copied = 0;
        try {
            for (; copied < values.size(); ++copied) {
                copyValue(values[copied], &items.get()[copied]);
            }
        } catch (...) {
            for (size_t i = 0; i < copied; ++i) std::free(items.get()[i].data);
            throw;
        }
        out->items = items.release();
        out->len = values.size();
    });
}

int kshm_set(kshm_t* handle, const kshm_kv_t* pairs, size_t count) {
    return invoke([&] {
        if (count != 0 && pairs == nullptr) {
            throw std::invalid_argument("kvspace: null pair array");
        }
        auto& client = requireHandle(handle);
        std::vector<kvspace::KVPair> input;
        input.reserve(count);
        std::exception_ptr validation_error;
        for (size_t i = 0; i < count; ++i) {
            try {
                input.push_back({
                    requireString(pairs[i].key, "key"),
                    decodeValue(pairs[i].value, pairs[i].value_len)});
            } catch (...) {
                validation_error = std::current_exception();
                break;
            }
        }
        client.Set(input);
        if (validation_error != nullptr) std::rethrow_exception(validation_error);
    });
}

int kshm_list(
    kshm_t* handle,
    const char* prefix,
    int expand_ext,
    kshm_string_list_t* out) {
    if (out != nullptr) {
        out->items = nullptr;
        out->len = 0;
    }
    return invoke([&] {
        if (out == nullptr) throw std::invalid_argument("kvspace: null list output");
        const auto values = requireHandle(handle).List(
            requireString(prefix, "prefix"), expand_ext != 0);
        std::unique_ptr<kshm_string_t, decltype(&std::free)> items(
            static_cast<kshm_string_t*>(std::calloc(values.size(), sizeof(kshm_string_t))),
            &std::free);
        if (!values.empty() && items == nullptr) throw std::bad_alloc();
        size_t copied = 0;
        try {
            for (; copied < values.size(); ++copied) {
                copyString(values[copied], &items.get()[copied]);
            }
        } catch (...) {
            for (size_t i = 0; i < copied; ++i) std::free(items.get()[i].data);
            throw;
        }
        out->items = items.release();
        out->len = values.size();
    });
}

int kshm_del(kshm_t* handle, const char* const* keys, size_t count) {
    return invoke([&] {
        if (count != 0 && keys == nullptr) {
            throw std::invalid_argument("kvspace: null key array");
        }
        std::vector<std::string> input;
        input.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            input.emplace_back(requireString(keys[i], "key"));
        }
        requireHandle(handle).Del(input);
    });
}

int kshm_del_tree(kshm_t* handle, const char* prefix) {
    return invoke([&] {
        requireHandle(handle).DelTree(requireString(prefix, "prefix"));
    });
}

int kshm_notify(
    kshm_t* handle,
    const char* key,
    const uint8_t* value,
    size_t value_len) {
    return invoke([&] {
        requireHandle(handle).Notify(
            requireString(key, "key"), decodeValue(value, value_len));
    });
}

int kshm_watch(
    kshm_t* handle,
    const char* key,
    int64_t timeout_ms,
    kshm_value_t* out) {
    if (out != nullptr) {
        out->data = nullptr;
        out->len = 0;
    }
    return invoke([&] {
        if (out == nullptr) throw std::invalid_argument("kvspace: null watch output");
        if (timeout_ms < 0 ||
            timeout_ms > std::numeric_limits<std::chrono::milliseconds::rep>::max()) {
            throw std::invalid_argument("kvspace: invalid watch timeout");
        }
        copyValue(requireHandle(handle).Watch(
            requireString(key, "key"), std::chrono::milliseconds(timeout_ms)), out);
    });
}

int kshm_mkindex(kshm_t* handle, const char* path) {
    return invoke([&] {
        requireHandle(handle).Mkindex(requireString(path, "path"));
    });
}

int kshm_link(kshm_t* handle, const char* target, const char* link_path) {
    return invoke([&] {
        requireHandle(handle).Link(
            requireString(target, "target"), requireString(link_path, "link path"));
    });
}

int kshm_ext_index(kshm_t* handle, const char* path, const char* ext_path) {
    return invoke([&] {
        requireHandle(handle).ExtIndex(
            requireString(path, "path"), requireString(ext_path, "extension path"));
    });
}

int kshm_unlink(kshm_t* handle, const char* path) {
    return invoke([&] {
        requireHandle(handle).Unlink(requireString(path, "path"));
    });
}

int kshm_clear(kshm_t* handle) {
    return invoke([&] { requireHandle(handle).Clear(); });
}

int kshm_compact(kshm_t* handle) {
    return invoke([&] { requireHandle(handle).Compact(); });
}

int kshm_stats(kshm_t* handle, kshm_stats_t* out) {
    return invoke([&] {
        if (out == nullptr) throw std::invalid_argument("kvspace: null stats output");
        const auto stats = requireHandle(handle).Stats();
        out->engine_id = static_cast<std::uint32_t>(stats.engine);
        out->reserved = 0;
        out->region_size = stats.region_size;
        out->region_max = stats.region_max;
        out->entries = stats.entries;
        out->engine_nodes = stats.engine_nodes;
        out->max_entries = stats.max_entries;
        out->tombstones = stats.tombstones;
        out->queues = stats.queues;
        out->heap_used = stats.heap_used;
        out->heap_free = stats.heap_free;
        out->recoveries = stats.recoveries;
    });
}

void kshm_value_free(kshm_value_t* value) {
    if (value == nullptr) return;
    std::free(value->data);
    value->data = nullptr;
    value->len = 0;
}

void kshm_value_list_free(kshm_value_list_t* values) {
    if (values == nullptr) return;
    for (size_t i = 0; i < values->len; ++i) {
        std::free(values->items[i].data);
    }
    std::free(values->items);
    values->items = nullptr;
    values->len = 0;
}

void kshm_string_list_free(kshm_string_list_t* values) {
    if (values == nullptr) return;
    for (size_t i = 0; i < values->len; ++i) {
        std::free(values->items[i].data);
    }
    std::free(values->items);
    values->items = nullptr;
    values->len = 0;
}

} // extern "C"
