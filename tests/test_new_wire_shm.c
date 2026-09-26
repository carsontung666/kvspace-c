#define _GNU_SOURCE
#include "kvspace_shm.h"
#include "xvalue_head.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s\n", #x); return 1; } } while (0)

int main(void) {
    char dir[] = "/tmp/kvspace-new-wire-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[128];
    snprintf(path, sizeof path, "%s/db", dir);
    kvspace_t *kv = kvspaceShmOpen(path, 8UL * 64 * 64 * 64 * 64);
    CHECK(kv != NULL);
    uint8_t *value = NULL;
    uint64_t n = 0;
    uint8_t raw[8] = {42};
    CHECK(kvspaceXhNewScalar("int64", raw, 8, &value, &n) == 0);
    CHECK(kvspaceShmSet(kv, "/x", value, (int32_t)n) == 0);
    free(value);
    int32_t got = 0;
    uint8_t *stored = kvspaceShmGet(kv, "/x", 0, &got);
    kvspaceXh head;
    CHECK(stored && got == 40 && kvspaceXhDecode(stored, got, &head) == 0 &&
          head.body[0] == 42);

    CHECK(kvspaceXhNewPtr("int64", "/x", 2, &value, &n) == 0);
    CHECK(kvspaceShmSet(kv, "/p", value, (int32_t)n) == 0);
    free(value);
    stored = kvspaceShmGet(kv, "/p", 1, &got);
    CHECK(stored && got == 40 && kvspaceXhDecode(stored, got, &head) == 0 &&
          head.body[0] == 42);

    CHECK(kvspaceXhNewShort("[int64]·int64", NULL, 0, &value, &n) == 0);
    CHECK(kvspaceShmSet(kv, "/m", value, (int32_t)n) == 0);
    free(value);
    CHECK(kvspaceXhNewScalar("int64", raw, 8, &value, &n) == 0);
    CHECK(kvspaceShmSet(kv, "/m·[0]", value, (int32_t)n) == 0);
    free(value);
    char **names = NULL;
    int32_t count = 0;
    CHECK(kvspaceShmList(kv, "/m·", false, 0, &names, &count) == 0 &&
          count == 1 && strcmp(names[0], "[0]") == 0);
    free(names[0]);
    free(names);
    CHECK(kvspaceShmCptree(kv, "/m", "/m-tree") == 0);
    CHECK(kvspaceShmCplist(kv, "/m", "/m-list") == 0);
    CHECK(kvspaceShmCptree(kv, "/m", "/m") == 0);
    CHECK(kvspaceShmCplist(kv, "/m", "/m") == 0);
    CHECK(kvspaceShmCptree(kv, "/m", "/m/child") != 0);
    CHECK(kvspaceShmCptree(kv, "/", "/under-root") != 0);
    for (int i = 0; i < 2; i++) {
        const char *base = i ? "/m-list" : "/m-tree";
        char member[32];
        snprintf(member, sizeof member, "%s·[0]", base);
        stored = kvspaceShmGet(kv, member, 0, &got);
        CHECK(stored && kvspaceXhDecode(stored, got, &head) == 0 &&
              head.body[0] == 42);
    }
    CHECK(kvspaceShmGet(kv, "/m-tree·", 0, &got) == NULL);
    CHECK(kvspaceShmGet(kv, "/m-list·", 0, &got) == NULL);

    CHECK(kvspaceXhNewShort("rwfunc", NULL, 0, &value, &n) == 0);
    CHECK(kvspaceShmSetValue(kv, "/fn/", value, (int32_t)n, 1, 7) == 0);
    free(value);
    CHECK(kvspaceShmMkindex(kv, "/fn/", 0) == 0);
    stored = kvspaceShmGet(kv, "/fn/", 0, &got);
    CHECK(stored && kvspaceXhDecode(stored, got, &head) == 0 &&
          head.langtype_len == 6 && memcmp(head.langtype, "rwfunc", 6) == 0);
    uint8_t ro = 0;
    uint32_t vid = 0;
    CHECK(kvspaceShmMetaGet(kv, "/fn/", &ro, &vid) == 0 && ro == 1 && vid == 7);

    CHECK(kvspaceShmMkindex(kv, "/lib/f/", 0) == 0);
    const uint8_t code[] = {0, 0, 0, 0, 0, 'x'};
    CHECK(kvspaceXhNewCode("rwir", code, sizeof code, sizeof code, &value, &n) == 0);
    CHECK(kvspaceShmSet(kv, "/lib/f/[1,-1]", value, (int32_t)n) == 0);
    CHECK(kvspaceShmSet(kv, "/lib/f/[1,0]", value, (int32_t)n) == 0);
    free(value);
    CHECK(kvspaceShmExtindex(kv, "/vthread/1/[1]/", "/lib/f/") == 0);
    CHECK(kvspaceXhNewPtr("int64", "/x", 2, &value, &n) == 0);
    CHECK(kvspaceShmSet(kv, "/vthread/1/[1]/[1,0]", value, (int32_t)n) != 0);
    CHECK(kvspaceShmSetValue(kv, "/vthread/1/[1]/[1,-1]", value,
                             (int32_t)n, 0, 0) == 0);
    free(value);
    stored = kvspaceShmGet(kv, "/vthread/1/[1]/[1,-1]", 0, &got);
    CHECK(stored && kvspaceXhDecode(stored, got, &head) == 0 &&
          head.kind == (KVSPACE_XH_SLACK | KVSPACE_XH_PTR_FLAG));

    uint8_t *body = NULL;
    CHECK(kvspaceShmWriteInPlace(kv, "/x", 0, 8, &body) == 0);
    body[0] = 43;
    stored = kvspaceShmGet(kv, "/x", 0, &got);
    CHECK(stored && kvspaceXhDecode(stored, got, &head) == 0 && head.body[0] == 43);

    CHECK(kvspaceShmWriteNewPlace(kv, "/fresh", 0, KVSPACE_XH_FIXED_SMALL,
                                  1, 19, "int64", 8, 8, &body) == 0);
    memcpy(body, raw, 8);
    stored = kvspaceShmGet(kv, "/fresh", 0, &got);
    CHECK(stored && kvspaceXhDecode(stored, got, &head) == 0 && head.body[0] == 42);
    CHECK(kvspaceShmMetaGet(kv, "/fresh", &ro, &vid) == 0 && ro == 1 && vid == 19);
    CHECK(kvspaceShmCp(kv, "/fresh", "/fresh-copy") == 0);
    CHECK(kvspaceShmMetaGet(kv, "/fresh-copy", &ro, &vid) == 0 && ro == 1 && vid == 19);
    CHECK(kvspaceShmCptree(kv, "/fresh", "/fresh-tree") == 0);
    CHECK(kvspaceShmMetaGet(kv, "/fresh-tree", &ro, &vid) == 0 && ro == 1 && vid == 19);

    static const uint8_t utf8[] = {0xc3, 0xa9, 'a'};
    CHECK(kvspaceShmWriteNewPlace(kv, "/text", 0, KVSPACE_XH_SLACK,
                                  0, 0, "[2]char/utf8", 3, 9, &body) == 0);
    memcpy(body, utf8, sizeof utf8);
    stored = kvspaceShmGet(kv, "/text", 0, &got);
    CHECK(stored && kvspaceXhDecode(stored, got, &head) == 0 && head.a == 3 &&
          head.b == 9 && got == 73 &&
          memcmp(head.body, utf8, sizeof utf8) == 0);
    kvspaceShmClose(kv);
    unlink(path);
    char extra[144];
    snprintf(extra, sizeof extra, "%s.sbo.head", path); unlink(extra);
    snprintf(extra, sizeof extra, "%s.sbo.data", path); unlink(extra);
    rmdir(dir);
    puts("ok");
    return 0;
}
