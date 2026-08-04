#include "kvspace/kshm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s (%s)\n",                 \
                    __FILE__, __LINE__, #condition, kshm_last_error());         \
            return EXIT_FAILURE;                                               \
        }                                                                      \
    } while (0)

int main(void) {
    char name[128];
    const int written = snprintf(
        name, sizeof(name), "/kvspace_c_api_%ld", (long)getpid());
    CHECK(written > 0 && (size_t)written < sizeof(name));
    (void)kshm_destroy(name);

    kshm_options_t options;
    kshm_options_init(&options);
    CHECK(options.struct_size == sizeof(options));
    CHECK(options.engine_id == KSHM_ENGINE_HASH_BOX);
    options.max_entries = 64;
    options.max_queues = 8;
    kshm_t *handle = kshm_create_with_options(name, &options);
    CHECK(handle != NULL);

    const uint8_t encoded[] = {
        6, 's', 't', 'r', 'i', 'n', 'g',
        2, 0, 0, 0,
        2, 0, 0, 0,
        'h', 'i'};
    const kshm_kv_t pair = {"/c/value", encoded, sizeof(encoded)};
    CHECK(kshm_set(handle, &pair, 1) == KSHM_OK);
    const kshm_kv_t none_pair = {"/c/none", NULL, 0};
    CHECK(kshm_set(handle, &none_pair, 1) == KSHM_OK);
    const kshm_kv_t bad_directory = {"/c/not-index/", encoded, sizeof(encoded)};
    CHECK(kshm_set(handle, &bad_directory, 1) ==
          KSHM_ERR_INVALID_DIRECTORY_VALUE);

    const uint8_t noncanonical_bool[] = {
        4, 'b', 'o', 'o', 'l',
        1, 0, 0, 0,
        1, 0, 0, 0,
        2};
    const kshm_kv_t bad_pair = {
        "/c/bad", noncanonical_bool, sizeof(noncanonical_bool)};
    CHECK(kshm_set(handle, &bad_pair, 1) == KSHM_ERR_INVALID_VALUE);

    const uint8_t encoded_int64[] = {
        5, 'i', 'n', 't', '6', '4',
        1, 0, 0, 0,
        8, 0, 0, 0,
        42, 0, 0, 0, 0, 0, 0, 0};
    const kshm_kv_t partial_pairs[] = {
        {"/c/partial", encoded_int64, sizeof(encoded_int64)},
        {"/c/bad-partial", noncanonical_bool, sizeof(noncanonical_bool)}};
    CHECK(kshm_set(handle, partial_pairs, 2) == KSHM_ERR_INVALID_VALUE);

    const char *partial_key[] = {"partial"};
    kshm_value_list_t partial_value = {0};
    CHECK(kshm_get(handle, "/c/", partial_key, 1, &partial_value) == KSHM_OK);
    CHECK(partial_value.len == 1);
    CHECK(partial_value.items[0].len == sizeof(encoded_int64));
    CHECK(memcmp(partial_value.items[0].data,
                 encoded_int64, sizeof(encoded_int64)) == 0);
    kshm_value_list_free(&partial_value);

    const uint8_t go_time[] = {
        4, 't', 'i', 'm', 'e',
        1, 0, 0, 0,
        9, 0, 0, 0,
        1, 2, 3, 4, 5, 6, 7, 8, 9};
    const kshm_kv_t time_pair = {
        "/c/time", go_time, sizeof(go_time)};
    CHECK(kshm_set(handle, &time_pair, 1) == KSHM_OK);

    const char *keys[] = {"value", "time", "missing"};
    kshm_value_list_t values = {0};
    CHECK(kshm_get(handle, "/c/", keys, 3, &values) == KSHM_OK);
    CHECK(values.len == 3);
    CHECK(values.items[0].len == sizeof(encoded));
    CHECK(memcmp(values.items[0].data, encoded, sizeof(encoded)) == 0);
    CHECK(values.items[1].len == sizeof(go_time));
    CHECK(memcmp(values.items[1].data, go_time, sizeof(go_time)) == 0);
    CHECK(values.items[2].data == NULL && values.items[2].len == 0);
    kshm_value_list_free(&values);

    kshm_string_list_t names = {0};
    CHECK(kshm_list(handle, "/c/", 1, &names) == KSHM_OK);
    CHECK(names.len == 4);
    CHECK(names.items[0].len == 5);
    CHECK(strcmp(names.items[0].data, "value") == 0);
    CHECK(names.items[1].len == 4);
    CHECK(strcmp(names.items[1].data, "none") == 0);
    CHECK(names.items[2].len == 7);
    CHECK(strcmp(names.items[2].data, "partial") == 0);
    CHECK(names.items[3].len == 4);
    CHECK(strcmp(names.items[3].data, "time") == 0);
    kshm_string_list_free(&names);

    CHECK(kshm_notify(handle, "/c/none-event", NULL, 0) == KSHM_OK);
    kshm_value_t none_event = {0};
    CHECK(kshm_watch(handle, "/c/none-event", 1, &none_event) == KSHM_OK);
    CHECK(none_event.data == NULL && none_event.len == 0);
    kshm_value_free(&none_event);

    CHECK(kshm_mkindex(handle, "/target/") == KSHM_OK);
    CHECK(kshm_link(handle, "/target/", "/mount/") == KSHM_OK);
    CHECK(kshm_ext_index(handle, "/mount/", "/target/") ==
          KSHM_ERR_NOT_DIRECTORY);

    kshm_close(handle);
    CHECK(kshm_attach_with_engine(name, KSHM_ENGINE_ART_BOX) == NULL);
    handle = kshm_attach_with_engine(name, KSHM_ENGINE_HASH_BOX);
    CHECK(handle != NULL);
    CHECK(kshm_clear(handle) == KSHM_OK);
    CHECK(kshm_compact(handle) == KSHM_OK);
    kshm_stats_t stats = {0};
    CHECK(kshm_stats(handle, &stats) == KSHM_OK);
    CHECK(stats.engine_id == KSHM_ENGINE_HASH_BOX);
    CHECK(stats.entries == 0);
    kshm_close(handle);
    CHECK(kshm_destroy(name) == KSHM_OK);
    CHECK(kshm_attach(name) == NULL);
    CHECK(strlen(kshm_last_error()) != 0);

    const uint32_t tree_engines[] = {
        KSHM_ENGINE_ART_BUMP,
        KSHM_ENGINE_ART_BOX,
        KSHM_ENGINE_TRIE_BOX};
    for (size_t i = 0; i < sizeof(tree_engines) / sizeof(tree_engines[0]); ++i) {
        options.engine_id = tree_engines[i];
        handle = kshm_create_with_options(name, &options);
        CHECK(handle != NULL);
        CHECK(kshm_set(handle, &pair, 1) == KSHM_OK);
        kshm_value_list_t tree_value = {0};
        const char *tree_key[] = {"value"};
        CHECK(kshm_get(handle, "/c/", tree_key, 1, &tree_value) == KSHM_OK);
        CHECK(tree_value.len == 1);
        CHECK(tree_value.items[0].len == sizeof(encoded));
        CHECK(memcmp(tree_value.items[0].data, encoded, sizeof(encoded)) == 0);
        kshm_value_list_free(&tree_value);
        kshm_stats_t tree_stats = {0};
        CHECK(kshm_stats(handle, &tree_stats) == KSHM_OK);
        CHECK(tree_stats.engine_id == tree_engines[i]);
        CHECK(tree_stats.engine_nodes > 0);
        CHECK(kshm_disconnect(handle) == KSHM_OK);
        CHECK(kshm_clear(handle) == KSHM_ERR_DISCONNECTED);
        CHECK(kshm_compact(handle) == KSHM_ERR_DISCONNECTED);
        CHECK(kshm_disconnect(handle) == KSHM_OK);
        kshm_close(handle);
        CHECK(kshm_destroy(name) == KSHM_OK);
    }
    return EXIT_SUCCESS;
}
