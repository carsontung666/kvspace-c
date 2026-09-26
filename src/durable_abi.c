/* KVSpace SHM ABI. */

/* macOS 需 _DARWIN_C_SOURCE 才暴露 usleep 等完整 Darwin API；_POSIX_C_SOURCE ≥200112
 * 才在 macOS 暴露 C99 的 snprintf（Linux 走 glibc 的 _DEFAULT_SOURCE）。 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "kvspace_shm.h"
#include "xvalue_head.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <limits.h>

#define KVSPACE_REF_INLINE 0
#define KVSPACE_REF_PTR 1
#define KVSPACE_REF_EXT 2
#define KVSPACE_KIND_CHAR_UTF8 "char/utf8"
#define KVSPACE_KIND_CHAR_ASCII "char/ascii"
#define KVSPACE_KIND_CHAR "char/utf32"
#define KVSPACE_KIND_BYTE "byte"
#define KVSPACE_KIND_BOOL "bool"
#define KVSPACE_KIND_INT64 "int64"
#define KVSPACE_KIND_FLOAT64 "float64"

int kvspaceTlvEncodeMode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                         const int32_t *dims, int32_t ndim, int32_t ref,
                         uint8_t ro, uint32_t vid, uint8_t **out,
                         uint32_t *out_len);

#define SHM_DEFAULT_SIZE (8ULL * 64 * 64 * 64 * 64)

/* Mirrors the public ABI. */
typedef struct {
    uint16_t headlen;
    uint8_t ref;
    uint8_t storetype;
    uint8_t ro;
    uint32_t vid;
    int32_t body_len;
    int32_t ndim;
    int32_t dims[8];
    char langtype[256];
    int32_t langtype_len;
    int32_t body_offset;
    uint64_t body_cap;
} kvspaceHead_t;

int kvspaceDecodeHead(const uint8_t *data, uint32_t data_len, kvspaceHead_t *out);

static int parse_shm_path(const char *dsn, char *out, size_t osz) {
    const char *sep = strstr(dsn, "://");
    if (!sep || strncmp(dsn, "shm", (size_t)(sep - dsn)) != 0)
        return -1;
    snprintf(out, osz, "%s", sep + 3);
    return 0;
}

void *kvspaceConnect(const char *dsn) {
    char path[1024];
    if (parse_shm_path(dsn, path, sizeof path) != 0)
        return NULL;
    return (void *)kvspaceShmOpen(path, SHM_DEFAULT_SIZE);
}

void kvspaceClose(void *h) {
    if (h)
        kvspaceShmClose((kvspace_t *)h);
}

/* 借用读：*out 指向 SHM 常驻映射（生命周期同该槽），调用方不得 free。 */
int kvspaceGet(void *h, const char *key, int resolve, uint8_t **out,
               uint32_t *out_len) {
    int32_t len = 0;
    uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, resolve, &len);
    if (!d || len <= 0) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }
    *out = d;
    *out_len = (uint32_t)len;
    return 0;
}

int kvspaceSetValue(void *h, const char *key, const uint8_t *value,
                    uint32_t value_len, uint8_t ro, uint32_t vid,
                    char *err, uint32_t err_cap) {
    int rc = value_len > INT32_MAX ? -2 :
             kvspaceShmSetValue((kvspace_t *)h, key, value,
                                (int32_t)value_len, ro, vid);
    if (rc == -2) {
        if (err && err_cap)
            snprintf(err, err_cap, "invalid XValue");
        return 1;
    }
    if (rc != 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "kvspace: set-value failed at %s", key ? key : "");
        return 1;
    }
    return 0;
}

int kvspaceResolveRef(void *h, const char *key, kvspaceRef_t *ref) {
    return kvspaceShmResolveRef((kvspace_t *)h, key, ref);
}

int kvspaceGetByRef(void *h, kvspaceRef_t *ref, const char *key_fallback,
                    uint8_t **out, uint32_t *out_len) {
    int32_t len = 0;
    uint8_t *d = kvspaceShmGetByRef((kvspace_t *)h, ref, key_fallback, &len);
    if (!d || len <= 0) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }
    *out = d;
    *out_len = (uint32_t)len;
    return 0;
}

int kvspaceSetPartByRef(void *h, kvspaceRef_t *ref, const char *key_fallback,
                        uint32_t offset, const uint8_t *buf, uint32_t buf_len,
                        char *err, uint32_t err_cap) {
    if (kvspaceShmSetPartByRef((kvspace_t *)h, ref, key_fallback, offset, buf,
                               buf_len) != 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "kvspace: set-part-by-ref failed");
        return 1;
    }
    return 0;
}

/* 指令边界回收读借用池：SHM 常驻映射，借用恒有效，no-op。 */
void kvspaceReadReset(void *h) { (void)h; }

/* 定位读：*out 指向 SHM 常驻映射内 [offset, offset+len)（借用，不得 free）。
 * 越界/空 → *out=NULL、out_len=0。 */
int kvspaceGetPart(void *h, const char *key, uint32_t offset, uint32_t len,
                    uint8_t **out, uint32_t *out_len) {
    int32_t total = 0;
    uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &total);
    if (!d || total <= 0 || offset >= (uint32_t)total) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }
    uint32_t avail = (uint32_t)total - offset;
    *out = d + offset;
    *out_len = len < avail ? len : avail;
    return 0;
}

/* 定位写：就地 memcpy buf 到 SHM 映射内 [offset, offset+buf_len)（key 须已存在、不改结构）。 */
int kvspaceSetPart(void *h, const char *key, uint32_t offset,
                    const uint8_t *buf, uint32_t buf_len, char *err,
                    uint32_t err_cap) {
    int32_t total = 0;
    uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &total);
    if (!d || total < 0 || offset + buf_len > (uint32_t)total) {
        if (err && err_cap)
            snprintf(err, err_cap, "kvspace: set-part out of range at %s", key);
        return 1;
    }
    memcpy(d + offset, buf, buf_len);
    return 0;
}

/* 读 head：解码值前缀的三正交轴 head（不取 body）。空/不存在 → 返回 1。 */
int kvspaceGetHead(void *h, const char *key, kvspaceHead_t *out) {
    int32_t total = 0;
    uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &total);
    if (!d || total <= 0)
        return 1;
    if (kvspaceDecodeHead(d, (uint32_t)total, out) != 0)
        return 1;
    return kvspaceShmMetaGet((kvspace_t *)h, key, &out->ro, &out->vid) == 0 ? 0 : 1;
}

/* 就地写：返回原 box body 偏移指针；前置条件不满足 → 非 0 + err。 */
int kvspaceWriteInPlace(void *h, const char *key, int resolve,
                        uint32_t body_len, uint8_t **body, char *err,
                        uint32_t err_cap) {
    if (kvspaceShmWriteInPlace((kvspace_t *)h, key, resolve, (int32_t)body_len,
                               body) != 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "kvspace: write-in-place rejected at %s", key);
        return 1;
    }
    return 0;
}

/* 新位置写：按 (ref, storetype, ro, vid, langtype, body_len) 分配 box、写 head，
 * 返回 body 偏移指针。 */
int kvspaceWriteNewPlace(void *h, const char *key, uint8_t ref,
                         uint8_t storetype, uint8_t ro, uint32_t vid,
                         const char *langtype, uint32_t body_len, uint64_t body_cap,
                         uint8_t **body, char *err, uint32_t err_cap) {
    if (kvspaceShmWriteNewPlace((kvspace_t *)h, key, ref, storetype, ro, vid,
                                langtype, (int32_t)body_len, body_cap, body) != 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "kvspace: write-new-place failed at %s", key);
        return 1;
    }
    return 0;
}

/* 只返回前缀下子项计数，无缓冲、无需释放。 */
int kvspaceListLen(void *h, const char *prefix, int expand_ext, int resolve,
                   int32_t *out_count) {
    return kvspaceShmListLen((kvspace_t *)h, prefix, expand_ext != 0, resolve,
                             out_count);
}

/* 索引取项：把前缀下第 idx 个直接子项名写进调用方自备缓冲 buf（容量
   buf_cap），*out_len 置该名长度（不含 NUL）。库侧零状态。idx 越界或缓冲不足 →
   返回非 0（缓冲不足时 *out_len 仍为所需长度，不静默截断）。配合 kvspaceListLen
   遍历。 */
int kvspaceListAt(void *h, const char *prefix, int expand_ext, int resolve,
                  int32_t idx, uint8_t *buf, uint32_t buf_cap,
                  uint32_t *out_len) {
    *out_len = 0;
    char **names = NULL;
    int32_t count = 0;
    if (kvspaceShmList((kvspace_t *)h, prefix, expand_ext != 0, resolve, &names,
                       &count) != 0)
        return -1;
    int rc = -1;
    if (idx >= 0 && idx < count) {
        size_t l = strlen(names[idx]);
        *out_len = (uint32_t)l;
        if (buf && l <= buf_cap) {
            memcpy(buf, names[idx], l);
            rc = 0;
        }
    }
    for (int32_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
    return rc;
}

int kvspaceDel(void *h, const char *const *keys, uint32_t nkeys, char *err,
               uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    for (uint32_t i = 0; i < nkeys; i++)
        kvspaceShmDel((kvspace_t *)h, keys[i]);
    return 0;
}

int kvspaceDelTree(void *h, const char *prefix, char *err, uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmDeltree((kvspace_t *)h, prefix);
}

int kvspaceCp(void *h, const char *src, const char *dst, char *err,
              uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmCp((kvspace_t *)h, src, dst);
}

int kvspaceCpTree(void *h, const char *src, const char *dst, char *err,
                  uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmCptree((kvspace_t *)h, src, dst);
}

int kvspaceCpList(void *h, const char *src, const char *dst, char *err,
                  uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmCplist((kvspace_t *)h, src, dst);
}

int kvspaceMkindex(void *h, const char *path, uint32_t capacity, char *err,
                   uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmMkindex((kvspace_t *)h, path, capacity);
}

int kvspaceMkindexExt(void *h, const char *path, const char *ext_path,
                      char *err, uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmExtindex((kvspace_t *)h, path, ext_path);
}

int kvspaceRmindexExt(void *h, const char *path, char *err, uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmDelextindex((kvspace_t *)h, path);
}

int kvspaceClear(void *h, char *err, uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmDeltree((kvspace_t *)h, "/");
}

static int finish(uint8_t *value, uint64_t len, uint8_t **out, uint32_t *out_len) {
    if (!value || len > UINT32_MAX) {
        free(value);
        return 1;
    }
    *out = value;
    *out_len = (uint32_t)len;
    return 0;
}

static int locator(int ext, const char *type, const uint8_t *raw, uint32_t len,
                   uint8_t **out, uint32_t *out_len) {
    if (!raw || memchr(raw, 0, len))
        return 1;
    char *path = malloc((size_t)len + 1);
    if (!path)
        return 1;
    memcpy(path, raw, len);
    path[len] = 0;
    uint8_t *value = NULL;
    uint64_t total = 0;
    int rc = ext ? kvspaceXhNewExt(type, path, len, &value, &total) :
                   kvspaceXhNewPtr(type, path, len, &value, &total);
    free(path);
    return rc == 0 ? finish(value, total, out, out_len) : 1;
}

int kvspaceTlvEncode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                     const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len) {
    return kvspaceTlvEncodeMode(kind, raw, raw_len, dims, ndim, 0, 0, 0, out, out_len);
}

int kvspaceTlvEncodeMode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                         const int32_t *dims, int32_t ndim, int32_t ref, uint8_t ro, uint32_t vid,
                         uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len || !kind || ndim < 0 || ndim > 8 ||
        (ndim && !dims) || (raw_len && !raw) || ro || vid)
        return 1;
    *out = NULL;
    *out_len = 0;
    if (ref == KVSPACE_REF_PTR || ref == KVSPACE_REF_EXT)
        return locator(ref == KVSPACE_REF_EXT, kind, raw, raw_len, out, out_len);
    if (ref != KVSPACE_REF_INLINE)
        return 1;

    uint8_t *value = NULL;
    uint64_t total = 0;
    int rc = -1;
    if ((kind[0] == 0 || strcmp(kind, "None") == 0) && raw_len == 0 && ndim == 0) {
        rc = kvspaceXhNewNone(&value, &total);
    } else if (strcmp(kind, KVSPACE_KIND_CHAR_UTF8) == 0 ||
               strcmp(kind, KVSPACE_KIND_CHAR_ASCII) == 0 ||
               strcmp(kind, KVSPACE_KIND_CHAR) == 0 ||
               strcmp(kind, KVSPACE_KIND_BYTE) == 0) {
        int elem = strcmp(kind, KVSPACE_KIND_CHAR_UTF8) == 0 ? KVSPACE_XH_UTF8 :
                   strcmp(kind, KVSPACE_KIND_CHAR_ASCII) == 0 ? KVSPACE_XH_ASCII :
                   strcmp(kind, KVSPACE_KIND_CHAR) == 0 ? KVSPACE_XH_UTF32 :
                   KVSPACE_XH_BYTE;
        rc = kvspaceXhNewSlack(elem, raw, raw_len, raw_len, &value, &total);
    } else if (ndim > 0) {
        uint64_t shape[8];
        for (int32_t i = 0; i < ndim; i++) {
            if (dims[i] < 0)
                return 1;
            shape[i] = (uint64_t)dims[i];
        }
        rc = kvspaceXhNewTensor(shape, (uint32_t)ndim, kind, raw, raw_len,
                                &value, &total);
    } else if (strcmp(kind, "rwir") == 0 || strcmp(kind, "def langtype") == 0 ||
               (strcmp(kind, "rwfunc") == 0 && raw_len != 0)) {
        rc = kvspaceXhNewCode(kind, raw, raw_len, raw_len, &value, &total);
    } else if (kvspaceXhNewScalar(kind, raw, raw_len, &value, &total) == 0) {
        rc = 0;
    } else {
        rc = kvspaceXhNewShort(kind, raw, raw_len, &value, &total);
    }
    return rc == 0 ? finish(value, total, out, out_len) : 1;
}

static int parse_dims(const char *type, int32_t *dims, int32_t *ndim) {
    *ndim = 0;
    if (type[0] != '[')
        return 0;
    const char *p = type + 1;
    for (;;) {
        if (*ndim == 8 || *p < '0' || *p > '9')
            return -1;
        uint64_t n = 0;
        do {
            n = n * 10 + (uint64_t)(*p++ - '0');
            if (n > INT32_MAX)
                return -1;
        } while (*p >= '0' && *p <= '9');
        dims[(*ndim)++] = (int32_t)n;
        if (*p == ']')
            return 0;
        if (*p++ != ',')
            return -1;
    }
}

int kvspaceDecodeHead(const uint8_t *data, uint32_t data_len, kvspaceHead_t *out) {
    if (!out)
        return 1;
    memset(out, 0, sizeof *out);
    kvspaceXh h;
    if (kvspaceXhDecode(data, data_len, &h) != 0 || h.total != data_len ||
        h.headlen > UINT16_MAX || h.content_len > INT32_MAX ||
        h.langtype_len > sizeof out->langtype - 1)
        return 1;
    out->headlen = (uint16_t)h.headlen;
    out->ref = h.kind & KVSPACE_XH_PTR_FLAG ? KVSPACE_REF_PTR :
               h.kind == KVSPACE_XH_EXT ? KVSPACE_REF_EXT : KVSPACE_REF_INLINE;
    out->storetype = h.kind & 3;
    out->body_len = (int32_t)h.content_len;
    out->body_offset = (int32_t)h.headlen;
    out->body_cap = h.body_cap;
    out->langtype_len = (int32_t)h.langtype_len;
    memcpy(out->langtype, h.langtype, h.langtype_len);
    if (h.kind == KVSPACE_XH_FIXED_LARGE ||
        (h.kind == KVSPACE_XH_SLACK && h.langtype_len && h.langtype[0] == '[')) {
        if (parse_dims(out->langtype, out->dims, &out->ndim) != 0)
            return 1;
    }
    return 0;
}

int kvspaceNewPtr(const char *target_kindexpr, const char *target,
                  uint8_t **out, uint32_t *out_len) {
    if (!target_kindexpr || !target || !out || !out_len)
        return 1;
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXhNewPtr(target_kindexpr, target, strlen(target), &value, &total) != 0)
        return 1;
    return finish(value, total, out, out_len);
}

int kvspaceNewChar(const uint8_t *bytes, uint32_t len, uint8_t **out, uint32_t *out_len) {
    return kvspaceTlvEncode(KVSPACE_KIND_CHAR_UTF8, bytes, len, NULL, 0, out, out_len);
}

int kvspaceNewBool(uint8_t v, uint8_t **out, uint32_t *out_len) {
    uint8_t b = v ? 1 : 0;
    return kvspaceTlvEncode(KVSPACE_KIND_BOOL, &b, 1, NULL, 0, out, out_len);
}

int kvspaceNewInt64(int64_t v, uint8_t **out, uint32_t *out_len) {
    uint8_t raw[8];
    for (int i = 0; i < 8; i++)
        raw[i] = (uint8_t)((uint64_t)v >> (8 * i));
    return kvspaceTlvEncode(KVSPACE_KIND_INT64, raw, 8, NULL, 0, out, out_len);
}

int kvspaceNewFloat64(double v, uint8_t **out, uint32_t *out_len) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    uint8_t raw[8];
    for (int i = 0; i < 8; i++)
        raw[i] = (uint8_t)(bits >> (8 * i));
    return kvspaceTlvEncode(KVSPACE_KIND_FLOAT64, raw, 8, NULL, 0, out, out_len);
}

int kvspaceWatch(void *h, const char *key, const uint8_t *target,
                 uint32_t target_len, uint64_t tick_ns, uint8_t **out,
                 uint32_t *out_len) {
    if (!out || !out_len)
        return 1;
    *out = NULL;
    *out_len = 0;
    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int32_t len = 0;
        uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &len);
        if (d && (uint32_t)len == target_len &&
            memcmp(d, target, target_len) == 0) {
            *out = d;
            *out_len = (uint32_t)len;
            return 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &tn);
        uint64_t elapsed = (uint64_t)(tn.tv_sec - t0.tv_sec) * 1000000000ULL +
                           (uint64_t)(tn.tv_nsec - t0.tv_nsec);
        if (elapsed >= tick_ns)
            return 0;
        usleep(1000);
    }
}
