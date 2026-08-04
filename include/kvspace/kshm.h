#ifndef KVSPACE_KSHM_H
#define KVSPACE_KSHM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque process-local handle for one shared-memory KVSpace mapping. */
typedef struct kshm kshm_t;

typedef enum kshm_status {
    KSHM_OK = 0,
    KSHM_ERR_INVALID_ARGUMENT = 1,
    KSHM_ERR_INVALID_PATH = 2,
    KSHM_ERR_INVALID_VALUE = 3,
    KSHM_ERR_DISCONNECTED = 4,
    KSHM_ERR_CAPACITY = 5,
    KSHM_ERR_CORRUPT = 6,
    KSHM_ERR_VERSION = 7,
    KSHM_ERR_EXISTS = 8,
    KSHM_ERR_NOT_FOUND = 9,
    KSHM_ERR_RESOLVE = 10,
    KSHM_ERR_EXT_WRITE = 11,
    KSHM_ERR_EXT_DELETE = 12,
    KSHM_ERR_EXT_CASCADE = 13,
    KSHM_ERR_EXT_TARGET = 14,
    KSHM_ERR_EXT_COLLISION = 15,
    KSHM_ERR_LINK_TYPE = 16,
    KSHM_ERR_LINK_EXISTS = 17,
    KSHM_ERR_NOT_DIRECTORY = 18,
    KSHM_ERR_ENGINE_MISMATCH = 19,
    KSHM_ERR_UNSUPPORTED_ENGINE = 20,
    KSHM_ERR_INVALID_DIRECTORY_VALUE = 21,
    KSHM_ERR_INTERNAL = 255
} kshm_status_t;

typedef enum kshm_engine {
    KSHM_ENGINE_ART_BUMP = 1,
    KSHM_ENGINE_ART_BOX = 2,
    KSHM_ENGINE_HASH_BOX = 3,
    KSHM_ENGINE_TRIE_BOX = 4
} kshm_engine_t;

typedef struct kshm_options {
    uint32_t struct_size;
    uint32_t engine_id;
    uint64_t initial_size;
    uint64_t max_size;
    uint64_t max_entries;
    uint64_t max_queues;
    uint32_t permissions;
} kshm_options_t;

/* A complete Go-compatible XValue TLV frame. None is {NULL, 0}. */
typedef struct kshm_value {
    uint8_t *data;
    size_t len;
} kshm_value_t;

typedef struct kshm_value_list {
    kshm_value_t *items;
    size_t len;
} kshm_value_list_t;

typedef struct kshm_kv {
    const char *key;
    const uint8_t *value;
    size_t value_len;
} kshm_kv_t;

typedef struct kshm_string {
    char *data;
    size_t len;
} kshm_string_t;

typedef struct kshm_string_list {
    kshm_string_t *items;
    size_t len;
} kshm_string_list_t;

typedef struct kshm_stats_result {
    uint32_t engine_id;
    uint32_t reserved;
    uint64_t region_size;
    uint64_t region_max;
    uint64_t entries;
    uint64_t engine_nodes;
    uint64_t max_entries;
    uint64_t tombstones;
    uint64_t queues;
    uint64_t heap_used;
    uint64_t heap_free;
    uint64_t recoveries;
} kshm_stats_t;

/* Fill options with the library defaults. */
void kshm_options_init(kshm_options_t *options);

/*
 * Names without an embedded slash use POSIX shm ("name" or "/name").
 * Absolute names containing another slash use a regular mmap-backed file.
 * The size-only calls use default limits; use *_with_options to configure all
 * fixed capacities. On failure they return NULL and set kshm_last_error().
 */
kshm_t *kshm_create(const char *name, size_t initial_size);
kshm_t *kshm_create_with_options(
    const char *name, const kshm_options_t *options);
kshm_t *kshm_attach(const char *name);
kshm_t *kshm_attach_with_engine(const char *name, kshm_engine_t expected_engine);
kshm_t *kshm_open(const char *name, size_t initial_size);
kshm_t *kshm_open_with_options(
    const char *name, const kshm_options_t *options);
/* Close the mapped client and wake watchers without freeing the handle. */
int kshm_disconnect(kshm_t *handle);
void kshm_close(kshm_t *handle);
int kshm_destroy(const char *name);

/* Last error for the calling thread; valid until its next kshm call. */
const char *kshm_last_error(void);

int kshm_get(
    kshm_t *handle,
    const char *prefix,
    const char *const *keys,
    size_t nkeys,
    kshm_value_list_t *out);
int kshm_set(kshm_t *handle, const kshm_kv_t *pairs, size_t count);
int kshm_list(
    kshm_t *handle,
    const char *prefix,
    int expand_ext,
    kshm_string_list_t *out);
int kshm_del(
    kshm_t *handle, const char *const *keys, size_t count);
int kshm_del_tree(kshm_t *handle, const char *prefix);
int kshm_notify(
    kshm_t *handle,
    const char *key,
    const uint8_t *value,
    size_t value_len);
/* timeout_ms == 0 waits indefinitely; a timeout returns KSHM_OK + None. */
int kshm_watch(
    kshm_t *handle,
    const char *key,
    int64_t timeout_ms,
    kshm_value_t *out);
int kshm_mkindex(kshm_t *handle, const char *path);
int kshm_link(kshm_t *handle, const char *target, const char *link_path);
int kshm_ext_index(kshm_t *handle, const char *path, const char *ext_path);
int kshm_unlink(kshm_t *handle, const char *path);
int kshm_clear(kshm_t *handle);
/* Reclaim unreachable append-only payloads (effective for ART-bump). */
int kshm_compact(kshm_t *handle);
int kshm_stats(kshm_t *handle, kshm_stats_t *out);

void kshm_value_free(kshm_value_t *value);
void kshm_value_list_free(kshm_value_list_t *values);
void kshm_string_list_free(kshm_string_list_t *values);

#ifdef __cplusplus
}
#endif

#endif
