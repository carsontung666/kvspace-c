/*
 * kvspace.c — KVSpace SHM 存储引擎: ART 树 + slotsboxmalloc
 */

#define _GNU_SOURCE
/* header-only 模式：两个依赖库的实现体在本 TU 编译，不再链接它们的 .so。
 * blockmalloc 必须先于 slotsboxmalloc（后者在其实现体里调用前者）。 */
#define BLOCKMALLOC_IMPLEMENTATION
#include <blockmalloc/blockmalloc.h>
#define SLOTSBOXMALLOC_IMPLEMENTATION
#include "slotsboxmalloc/slotsboxobj.h"
#include "kvspace_shm.h"
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KVS_MAGIC "kvspace-c.v2"
#define ART_PREFIX_MAX 10
#define ART_NODE_MAX_SZ 2112
#define ART_SLAB_INIT (256UL * 1024 * 1024)
/* blocks_init picks 30-bit ids only if the initial pool holds > 8191 blocks;
   a smaller pool would be capped at 16384 blocks forever. */
#define SBO_HEAD_POOL_INIT (4UL * 1024 * 1024)
/* Reserved VA per region: grow in place, base never moves.
   data grows x64 per step; 2^40 fits 8 * 64^6 = 512GB. */
#define REGION_RESERVE (1ULL << 38)
#define DATA_RESERVE (1ULL << 40)
/* Values >= 64KB are level-3+ objects (32KB aligned): punch pages on delete. */
#define SBO_PUNCH_MIN (64UL * 1024)
#define WATCH_TABLE_SZ 256

enum { ART_N4 = 0,
       ART_N16 = 1,
       ART_N48 = 2,
       ART_N256 = 3,
       ART_MOVED = 255 };

typedef struct {
    uint8_t type, prefix[ART_PREFIX_MAX], prefix_len;
    uint8_t has_value : 1;
    uint64_t box_offset : 63;
    uint16_t count;
} art_hdr_t;

typedef struct {
    art_hdr_t h;
    uint8_t keys[4];
    int32_t children[4];
} art_n4_t;
typedef struct {
    art_hdr_t h;
    uint8_t keys[16];
    int32_t children[16];
} art_n16_t;
typedef struct {
    art_hdr_t h;
    uint8_t index[256];
    int32_t children[48];
} art_n48_t;
typedef struct {
    art_hdr_t h;
    int32_t children[256];
} art_n256_t;

static int art_node_sz(int t) {
    return t == ART_N4    ? sizeof(art_n4_t)
           : t == ART_N16 ? sizeof(art_n16_t)
           : t == ART_N48 ? sizeof(art_n48_t)
                          : sizeof(art_n256_t);
}

/* <path>           [kvspace_hdr_t][blocks_meta_t][ART slab ...]
 * <path>.sbo.head  [sbo meta ...]
 * <path>.sbo.data  [sbo data ...] */
typedef struct {
    char magic[12];
    uint64_t art_slab_size, sbo_head_size, sbo_data_size;
    int32_t art_root;
} kvspace_hdr_t;

#define ART_OFF (sizeof(kvspace_hdr_t) + sizeof(blocks_meta_t))

/* blocks_init fixes the block-head width (30-bit ids) from the initial size. */
_Static_assert(ART_SLAB_INIT / (ART_NODE_MAX_SZ + 2) > 32767ULL / 4,
               "ART_SLAB_INIT too small: blockmalloc would pick 14-bit ids");
_Static_assert(REGION_RESERVE / (ART_NODE_MAX_SZ + 4) <= (1ULL << 30),
               "REGION_RESERVE too large for 30-bit block ids");
_Static_assert(
    SBO_HEAD_POOL_INIT / (sizeof(sbo_box_t) + 2) > 32767ULL / 4,
    "SBO_HEAD_POOL_INIT too small: blockmalloc would pick 14-bit ids");

/* Head layout with root_slots == 1 (checked at open):
   [sbo_meta_t][blocks_meta_t][sbo_lock_t][pool]. Public structs only. */
#define SBO_HEAD_FIXED                                                         \
    (sizeof(sbo_meta_t) + sizeof(blocks_meta_t) + sizeof(sbo_lock_t))
static blocks_meta_t *sbo_pool(sbo_meta_t *m) {
    return (blocks_meta_t *)(m + 1);
}
static sbo_lock_t *sbo_plock(sbo_meta_t *m) {
    return (sbo_lock_t *)(sbo_pool(m) + 1);
}
static uint8_t *sbo_pmem(sbo_meta_t *m) {
    return (uint8_t *)(sbo_plock(m) + 1);
}
static sbo_box_t *sbo_box(sbo_meta_t *m, int32_t id) {
    return (sbo_box_t *)(sbo_pmem(m) + (size_t)id * m->block_stride +
                         m->sizeof_block_head);
}

typedef struct {
    int fd;
    uint8_t *base;
    size_t mapped;
    size_t reserve;
} shm_region_t;

typedef struct {
    char key[256];
    pthread_cond_t cond;
    pthread_mutex_t mtx;
    uint8_t *val;
    int32_t val_len;
    bool ready;
} watch_t;

struct kvspace {
    shm_region_t r_art, r_head, r_data;
    kvspace_hdr_t *hdr;
    blocks_meta_t *art_meta;
    uint8_t *art_data;
    sbo_meta_t *sbo_meta;
    uint8_t *sbo_data;
    watch_t watches[WATCH_TABLE_SZ];
    pthread_mutex_t wlock;
};

/* ---- shm region ---- */
static int region_map(shm_region_t *r, size_t size) {
    if (size > r->reserve)
        return -1;
    if (size <= r->mapped)
        return 0;
    if (mmap(r->base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
             r->fd, 0) == MAP_FAILED)
        return -1;
    r->mapped = size;
    return 0;
}

/* create: size the file; open: check it. Then reserve VA and map. */
static int region_attach(shm_region_t *r, bool create, size_t size,
                         size_t reserve) {
    struct stat st;
    if (create ? ftruncate(r->fd, (off_t)size) != 0
               : fstat(r->fd, &st) != 0 || st.st_size < (off_t)size)
        return -1;
    r->reserve = size > reserve ? size : reserve;
    void *p = mmap(NULL, r->reserve, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    r->base = p;
    return region_map(r, size);
}

static void region_close(shm_region_t *r) {
    if (r->base)
        munmap(r->base, r->reserve);
    if (r->fd >= 0)
        close(r->fd);
}

/* Pick up growth done by another process (mid-op growth: kvspace#11). */
static int kv_sync(kvspace_t *kv) {
    if (region_map(&kv->r_art, ART_OFF + (size_t)kv->hdr->art_slab_size) != 0)
        return -1;
    if (region_map(&kv->r_head, (size_t)kv->hdr->sbo_head_size) != 0)
        return -1;
    return region_map(&kv->r_data, (size_t)kv->hdr->sbo_data_size);
}

/* blockmalloc only checks total_size; grow the file before publishing it. */
static int art_slab_grow(kvspace_t *kv) {
    size_t want = (size_t)kv->hdr->art_slab_size * 2;
    if (ART_OFF + want > kv->r_art.reserve ||
        ftruncate(kv->r_art.fd, (off_t)(ART_OFF + want)) != 0 ||
        region_map(&kv->r_art, ART_OFF + want) != 0)
        return -1;
    kv->hdr->art_slab_size = want;
    kv->art_meta->total_size = want;
    return 0;
}

/* ---- sbo growth (docs/shm-resize.md). Both grow functions run under
   sbo_plock, which excludes sbo_alloc/free/allocated_size in every process. */

/* Free list empty and no room to append. Unlocked calls are a hint only. */
static bool sbo_pool_full(sbo_meta_t *m) {
    blocks_meta_t *p = sbo_pool(m);
    if (p->free_next_id != -1)
        return false;
    uint64_t next_end = (uint64_t)block_offset(p, p->malloc_blocks) +
                        p->sizeof_block_head + p->block_size;
    return next_end > p->total_size;
}

/* Cap at what the block-head width can address: 14-bit or 30-bit ids. */
static int sbo_head_grow(kvspace_t *kv) {
    sbo_meta_t *m = kv->sbo_meta;
    blocks_meta_t *p = sbo_pool(m);
    uint64_t stride = p->sizeof_block_head + p->block_size;
    uint64_t cap = (1ULL << (p->sizeof_block_head == 2 ? 14 : 30)) * stride;
    uint64_t cur = p->total_size, want = cur * 2;
    if (want > cap)
        want = cap;
    if (SBO_HEAD_FIXED + want > kv->r_head.reserve)
        want = kv->r_head.reserve - SBO_HEAD_FIXED;
    if (want <= cur ||
        ftruncate(kv->r_head.fd, (off_t)(SBO_HEAD_FIXED + want)) != 0 ||
        region_map(&kv->r_head, SBO_HEAD_FIXED + (size_t)want) != 0)
        return -1;
    p->total_size = want;
    m->per_slot_meta = want;
    m->head_size = SBO_HEAD_FIXED + want;
    kv->hdr->sbo_head_size = m->head_size;
    return 0;
}

/* x64: move the root into a new block, make block 0 the level above with
   slot 0 pointing at it. Existing offsets stay valid (docs 2.2). */
static int sbo_data_grow(kvspace_t *kv) {
    sbo_meta_t *m = kv->sbo_meta;
    uint64_t cur = m->data_size, want = cur * 64;
    if (want > kv->r_data.reserve)
        return -1;
    if (sbo_pool_full(m) && sbo_head_grow(kv) != 0)
        return -1;
    if (ftruncate(kv->r_data.fd, (off_t)want) != 0 ||
        region_map(&kv->r_data, (size_t)want) != 0)
        return -1;
    int64_t nid = blocks_alloc(sbo_pool(m), sbo_pmem(m));
    if (nid < 0)
        return -1;
    sbo_box_t *root = sbo_box(m, 0), *b = sbo_box(m, (int32_t)nid);
    memcpy(b, root, sizeof *b);
    b->parent = 0;
    for (int i = 0; i < SBO_N; i++)
        if (root->slots[i].state == SBO_BOX)
            sbo_box(m, root->children[i])->parent = (int32_t)nid;
    uint8_t lvl = (uint8_t)(root->objlevel + 1);
    root->objlevel = lvl;
    root->box_boundary = 1;
    root->obj_boundary = SBO_N - 1;
    memset(root->free_bitmap, 0xFF, SBO_BITMAP_B);
    root->free_bitmap[0] &= 0xFE;
    for (int i = 0; i < SBO_N; i++) {
        root->slots[i].state = SBO_FREE;
        root->children[i] = -1;
    }
    root->slots[0].state = SBO_BOX;
    root->children[0] = (int32_t)nid;
    root->max_obj_cap = SBO_N - 1;
    root->child_max_cap = (sbo_usage_t){(uint8_t)(lvl + 1), 1};
    m->data_size = want;
    m->slot_bytes = want;
    kv->hdr->sbo_data_size = want;
    return 0;
}

/* Pool exhaustion is exact. Data exhaustion is not (sbo_alloc also fails on
   trylock contention), so re-root only after two failures at one capacity. */
static int kv_sbo_grow(kvspace_t *kv, bool failed, uint64_t *seen_data) {
    sbo_meta_t *m = kv->sbo_meta;
    sbo_lock(sbo_plock(m));
    int rc = 0;
    if (sbo_pool_full(m))
        rc = sbo_head_grow(kv);
    else if (failed && m->data_size == *seen_data)
        rc = sbo_data_grow(kv);
    else if (failed)
        *seen_data = m->data_size;
    sbo_unlock(sbo_plock(m));
    return rc;
}

/* A failed sbo_alloc burns one slot per level (box_boundary is not rolled
   back), so check the pool before allocating. */
static uint64_t kv_sbo_alloc(kvspace_t *kv, size_t n) {
    sbo_meta_t *m = kv->sbo_meta;
    uint64_t seen_data = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt && kv_sync(kv) != 0)
            return (uint64_t)-1;
        if (sbo_pool_full(m) && kv_sbo_grow(kv, false, &seen_data) != 0)
            return (uint64_t)-1;
        uint64_t off = sbo_alloc(m, n);
        if (off != (uint64_t)-1)
            return off;
        if (kv_sbo_grow(kv, true, &seen_data) != 0)
            return (uint64_t)-1;
    }
    return (uint64_t)-1;
}

/* Punch before free: once freed, another process may reuse the range. */
static void kv_sbo_free(kvspace_t *kv, uint64_t off) {
#if defined(__linux__)
    /* Linux 用 fallocate PUNCH_HOLE 归还磁盘块；macOS 无该 API（仅空间回收优化），跳过。 */
    uint64_t sz = sbo_allocated_size(kv->sbo_meta, off);
    if (sz >= SBO_PUNCH_MIN) {
        uint64_t pg = (uint64_t)sysconf(_SC_PAGESIZE);
        uint64_t a = (off + pg - 1) & ~(pg - 1), b = (off + sz) & ~(pg - 1);
        if (b > a)
            (void)fallocate(kv->r_data.fd,
                            FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                            (off_t)a, (off_t)(b - a));
    }
#endif
    sbo_free(kv->sbo_meta, off);
}

/* ---- helpers ---- */
static void *art_blk(kvspace_t *kv, int32_t id) {
    if (id < 0)
        return NULL;
    return kv->art_data + blockdata_offset(kv->art_meta, (uint64_t)id);
}
static int32_t art_balloc(kvspace_t *kv) {
    int64_t id = blocks_alloc(kv->art_meta, kv->art_data);
    if (id >= 0)
        return (int32_t)id;
    if (art_slab_grow(kv) != 0)
        return -1;
    return (int32_t)blocks_alloc(kv->art_meta, kv->art_data);
}
static art_hdr_t *art_hdr(kvspace_t *kv, int32_t id) {
    return (art_hdr_t *)art_blk(kv, id);
}

/* ---- prefix ---- */
static int pfx_shared(const uint8_t *a, int al, const uint8_t *b, int bl) {
    int n = al < bl ? al : bl;
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return i;
    return n;
}

/* ---- child lookup ---- */
static int32_t art_child(kvspace_t *kv, void *n, uint8_t b) {
    art_hdr_t *h = (art_hdr_t *)n;
    switch (h->type) {
    case ART_N4: {
        art_n4_t *x = n;
        for (int i = 0; i < (int)h->count; i++)
            if (x->keys[i] == b)
                return x->children[i];
        return -1;
    }
    case ART_N16: {
        art_n16_t *x = n;
        int lo = 0, hi = (int)h->count - 1;
        while (lo <= hi) {
            int m = (lo + hi) / 2;
            if (x->keys[m] == b)
                return x->children[m];
            if (x->keys[m] < b)
                lo = m + 1;
            else
                hi = m - 1;
        }
        return -1;
    }
    case ART_N48: {
        art_n48_t *x = n;
        uint8_t idx = x->index[b];
        return idx == 255 ? -1 : x->children[idx];
    }
    case ART_N256: {
        return ((art_n256_t *)n)->children[b];
    }
    }
    return -1;
}

static int32_t art_follow(kvspace_t *kv, int32_t id);

static int32_t art_walk(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                        int d, int32_t *par, int *pard) {
    while (nid >= 0) {
        art_hdr_t *h = art_hdr(kv, nid);
        if (!h || h->type == ART_MOVED)
            return -1;
        if (h->prefix_len) {
            int s = pfx_shared(h->prefix, h->prefix_len, key + d, klen - d);
            if (s != h->prefix_len) {
                if (d + s < klen)
                    return -1;
                if (s < h->prefix_len)
                    return -1;
            }
            d += h->prefix_len;
            if (d > klen)
                return -1;
        }
        if (d == klen)
            return h->has_value ? nid : -1;
        if (par)
            *par = nid;
        if (pard)
            *pard = d;
        nid = art_child(kv, h, key[d]);
        d++;
    }
    return -1;
}

/* ---- art_search ---- */
static int32_t art_find2(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                         int32_t *par, int *pard) {
    if (nid < 0 || !key)
        return -1;
    int32_t parent = -1;
    int parent_d = 0;
    int32_t id = art_walk(kv, nid, key, klen, 0, &parent, &parent_d);
    if (par)
        *par = parent;
    if (pard)
        *pard = parent_d;
    return id;
}
static int32_t art_find(kvspace_t *kv, int32_t nid, const uint8_t *key,
                        int klen) {
    return art_find2(kv, nid, key, klen, NULL, NULL);
}

/* Last path '/' or kvlang member '·' (U+00B7, utf-8 C2 B7). */
static int last_key_sep(const uint8_t *key, int klen, int *seplen) {
    int slash = -1, mid = -1;
    for (int i = 0; i < klen; i++) {
        if (key[i] == '/')
            slash = i;
        if (i + 1 < klen && key[i] == 0xC2 && key[i + 1] == 0xB7)
            mid = i;
    }
    if (mid > slash) {
        *seplen = 2;
        return mid;
    }
    if (slash > 0) {
        *seplen = 1;
        return slash;
    }
    *seplen = 0;
    return -1;
}

/* One walk: leaf plus the ancestor covering last '/' or '·' (after that
 * node's prefix). First match: node at the separator, not a deeper unique
 * prefix that swallowed it. */
static int32_t art_find_dir(kvspace_t *kv, int32_t nid, const uint8_t *key,
                            int klen, int last_sep, int seplen, int32_t *dirn,
                            int *dird) {
    int d = 0;
    int32_t dir = -1;
    int dd = 0;
    if (nid < 0 || !key)
        return -1;
    while (nid >= 0) {
        art_hdr_t *h = art_hdr(kv, nid);
        if (!h || h->type == ART_MOVED)
            return -1;
        int entry_d = d;
        if (h->prefix_len) {
            int s = pfx_shared(h->prefix, h->prefix_len, key + d, klen - d);
            if (s != h->prefix_len)
                return -1;
            d += h->prefix_len;
            if (d > klen)
                return -1;
        }
        if (dir < 0 && last_sep > 0 && seplen > 0 &&
            ((entry_d <= last_sep && last_sep < d) ||
             d == last_sep + seplen)) {
            dir = nid;
            dd = d;
        }
        if (d == klen) {
            if (dirn)
                *dirn = dir;
            if (dird)
                *dird = dd;
            return h->has_value ? nid : -1;
        }
        nid = art_child(kv, h, key[d]);
        d++;
    }
    return -1;
}
static art_hdr_t *art_search(kvspace_t *kv, int32_t nid, const uint8_t *key,
                             int klen) {
    int32_t id = art_find(kv, nid, key, klen);
    return id < 0 ? NULL : art_hdr(kv, id);
}
static int32_t art_follow(kvspace_t *kv, int32_t id) {
    for (int i = 0; i < 8 && id >= 0; i++) {
        art_hdr_t *h = art_hdr(kv, id);
        if (!h)
            return -1;
        if (h->type != ART_MOVED)
            return id;
        id = (int32_t)h->box_offset;
    }
    return -1;
}

/* ---- node create ---- */
static int32_t art_new_leaf(kvspace_t *kv, uint64_t off) {
    int32_t id = art_balloc(kv);
    if (id < 0)
        return -1;
    art_n4_t *x = art_blk(kv, id);
    memset(x, 0, sizeof(*x));
    x->h.type = ART_N4;
    x->h.has_value = 1;
    x->h.box_offset = off;
    return id;
}
static int32_t art_new_node(kvspace_t *kv, int t) {
    int32_t id = art_balloc(kv);
    if (id < 0)
        return -1;
    void *x = art_blk(kv, id);
    int sz = art_node_sz(t);
    memset(x, 0, sz);
    ((art_hdr_t *)x)->type = (uint8_t)t;
    if (t == ART_N48)
        memset(((art_n48_t *)x)->index, 255, 256);
    if (t == ART_N256) {
        art_n256_t *n = x;
        for (int i = 0; i < 256; i++)
            n->children[i] = -1;
    }
    return id;
}

/* ---- grow ---- */
static int32_t art_grow(kvspace_t *kv, void *on) {
    art_hdr_t *oh = on;
    int nt;
    if (oh->type == ART_N4)
        nt = ART_N16;
    else if (oh->type == ART_N16)
        nt = ART_N48;
    else if (oh->type == ART_N48)
        nt = ART_N256;
    else
        return -1;
    int32_t nid = art_new_node(kv, nt);
    if (nid < 0)
        return -1;
    art_hdr_t *nh = art_hdr(kv, nid);
    nh->prefix_len = oh->prefix_len;
    memcpy(nh->prefix, oh->prefix, oh->prefix_len);
    nh->has_value = oh->has_value;
    nh->box_offset = oh->box_offset;
    // copy children
    for (int i = 0; i < (int)oh->count; i++) {
        uint8_t b = 0;
        int32_t c = -1;
        switch (oh->type) {
        case ART_N4: {
            art_n4_t *x = on;
            b = x->keys[i];
            c = x->children[i];
            break;
        }
        case ART_N16: {
            art_n16_t *x = on;
            b = x->keys[i];
            c = x->children[i];
            break;
        }
        case ART_N48: {
            art_n48_t *x = on;
            c = x->children[i];
            for (int j = 0; j < 256; j++)
                if (x->index[j] == i) {
                    b = (uint8_t)j;
                    break;
                }
            break;
        }
        }
        switch (nt) {
        case ART_N16: {
            art_n16_t *x = (art_n16_t *)nh;
            x->keys[i] = b;
            x->children[i] = c;
            break;
        }
        case ART_N48: {
            art_n48_t *x = (art_n48_t *)nh;
            x->index[b] = (uint8_t)i;
            x->children[i] = c;
            break;
        }
        case ART_N256: {
            ((art_n256_t *)nh)->children[b] = c;
            break;
        }
        }
    }
    nh->count = oh->count;
    if (nt == ART_N16) {
        art_n16_t *x = (art_n16_t *)nh;
        for (int i = 0; i < (int)nh->count - 1; i++)
            for (int j = i + 1; j < (int)nh->count; j++)
                if (x->keys[i] > x->keys[j]) {
                    uint8_t tk = x->keys[i];
                    x->keys[i] = x->keys[j];
                    x->keys[j] = tk;
                    int32_t tc = x->children[i];
                    x->children[i] = x->children[j];
                    x->children[j] = tc;
                }
    }
    oh->type = ART_MOVED;
    oh->has_value = 0;
    oh->box_offset = (uint64_t)(uint32_t)nid;
    return nid;
}

/* ---- add child ---- */
static int art_add(kvspace_t *kv, void *n, uint8_t b, int32_t cid) {
    art_hdr_t *h = n;
    switch (h->type) {
    case ART_N4: {
        if (h->count >= 4)
            return -1;
        art_n4_t *x = n;
        x->keys[h->count] = b;
        x->children[h->count] = cid;
        h->count++;
        return 0;
    }
    case ART_N16: {
        if (h->count >= 16)
            return -1;
        art_n16_t *x = n;
        int p = (int)h->count;
        while (p > 0 && x->keys[p - 1] > b) {
            x->keys[p] = x->keys[p - 1];
            x->children[p] = x->children[p - 1];
            p--;
        }
        x->keys[p] = b;
        x->children[p] = cid;
        h->count++;
        return 0;
    }
    case ART_N48: {
        if (h->count >= 48)
            return -1;
        art_n48_t *x = n;
        int s = (int)h->count;
        x->index[b] = (uint8_t)s;
        x->children[s] = cid;
        h->count++;
        return 0;
    }
    case ART_N256: {
        art_n256_t *x = n;
        if (x->children[b] >= 0)
            return -1;
        x->children[b] = cid;
        h->count++;
        return 0;
    }
    }
    return -1;
}

/* ---- remove child ---- */
static void art_rm(kvspace_t *kv, void *n, uint8_t b) {
    art_hdr_t *h = n;
    switch (h->type) {
    case ART_N4: {
        art_n4_t *x = n;
        for (int i = 0; i < (int)h->count; i++)
            if (x->keys[i] == b) {
                for (int j = i; j < (int)h->count - 1; j++) {
                    x->keys[j] = x->keys[j + 1];
                    x->children[j] = x->children[j + 1];
                }
                h->count--;
                return;
            }
        break;
    }
    case ART_N16: {
        art_n16_t *x = n;
        int lo = 0, hi = (int)h->count - 1, pos = -1;
        while (lo <= hi) {
            int m = (lo + hi) / 2;
            if (x->keys[m] == b) {
                pos = m;
                break;
            }
            if (x->keys[m] < b)
                lo = m + 1;
            else
                hi = m - 1;
        }
        if (pos < 0)
            return;
        for (int j = pos; j < (int)h->count - 1; j++) {
            x->keys[j] = x->keys[j + 1];
            x->children[j] = x->children[j + 1];
        }
        h->count--;
        break;
    }
    case ART_N48: {
        art_n48_t *x = n;
        uint8_t idx = x->index[b];
        if (idx == 255)
            return;
        x->index[b] = 255;
        int last = (int)h->count - 1;
        if (idx != last) {
            x->children[idx] = x->children[last];
            for (int j = 0; j < 256; j++)
                if (x->index[j] == last) {
                    x->index[j] = idx;
                    break;
                }
        }
        h->count--;
        break;
    }
    case ART_N256: {
        art_n256_t *x = n;
        x->children[b] = -1;
        h->count--;
        break;
    }
    }
}

/* ---- 叶子链：key[d..klen) 建链，末端叶子存 off（尾段超 ART_PREFIX_MAX 分级）
 * ---- */
static int32_t art_leaf_chain(kvspace_t *kv, const uint8_t *key, int klen,
                              int d, uint64_t off) {
    int rem = klen - d;
    if (rem <= ART_PREFIX_MAX) {
        int32_t id = art_new_leaf(kv, off);
        if (id < 0)
            return -1;
        art_n4_t *x = art_blk(kv, id);
        if (rem > 0)
            memcpy(x->h.prefix, key + d, (size_t)rem);
        x->h.prefix_len = (uint8_t)rem;
        return id;
    }
    int32_t id = art_new_node(kv, ART_N4);
    if (id < 0)
        return -1;
    art_hdr_t *x = art_hdr(kv, id);
    memcpy(x->prefix, key + d, ART_PREFIX_MAX);
    x->prefix_len = ART_PREFIX_MAX;
    int32_t sub = art_leaf_chain(kv, key, klen, d + ART_PREFIX_MAX + 1, off);
    if (sub < 0)
        return -1;
    art_add(kv, x, key[d + ART_PREFIX_MAX], sub);
    return id;
}

/* ---- insert ---- */
static int32_t art_ins(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                       int d, uint64_t off) {
    if (nid < 0)
        return art_leaf_chain(kv, key, klen, d, off);

    art_hdr_t *h = art_hdr(kv, nid);
    if (!h)
        return -1;
    int shared = pfx_shared(h->prefix, h->prefix_len, key + d, klen - d);
    int mpl = h->prefix_len < (klen - d) ? h->prefix_len : (klen - d);

    if (shared == mpl && (klen - d) < h->prefix_len) {
        /* key 在节点 prefix 内部耗尽（含 d==klen）：拆出 shared
         * 字节作新值，旧节点作 child */
        int32_t nn = art_new_node(kv, ART_N4);
        if (nn < 0)
            return -1;
        art_hdr_t *nh = art_hdr(kv, nn);
        nh->prefix_len = (uint8_t)shared;
        if (shared > 0)
            memcpy(nh->prefix, key + d, (size_t)shared);
        uint8_t ob = h->prefix[shared];
        h->prefix_len -= (uint8_t)(shared + 1);
        if (h->prefix_len > 0)
            memmove(h->prefix, h->prefix + shared + 1, h->prefix_len);
        art_add(kv, nh, ob, nid);
        nh->has_value = 1;
        nh->box_offset = off;
        return nn;
    }
    if (shared < mpl) { // prefix split
        int32_t nn = art_new_node(kv, ART_N4);
        if (nn < 0)
            return -1;
        art_hdr_t *nh = art_hdr(kv, nn);
        nh->prefix_len = (uint8_t)shared;
        memcpy(nh->prefix, key + d, shared);
        uint8_t ob = h->prefix[shared];
        h->prefix_len -= (uint8_t)(shared + 1);
        if (h->prefix_len > 0)
            memmove(h->prefix, h->prefix + shared + 1, h->prefix_len);
        art_add(kv, nh, ob, nid);
        int32_t leaf = art_leaf_chain(kv, key, klen, d + shared + 1, off);
        if (leaf < 0)
            return -1;
        art_add(kv, nh, key[d + shared], leaf);
        return nn;
    }
    d += h->prefix_len;
    if (d == klen) {
        if (h->has_value)
            h->box_offset = off;
        else {
            h->has_value = 1;
            h->box_offset = off;
        }
        return nid;
    }
    int32_t cid = art_child(kv, h, key[d]);
    if (cid >= 0) {
        int32_t nc = art_ins(kv, cid, key, klen, d + 1, off);
        if (nc < 0)
            return -1;
        if (nc != cid) {
            art_rm(kv, h, key[d]);
            art_add(kv, h, key[d], nc);
        }
        return nid;
    }
    int32_t leaf = art_leaf_chain(kv, key, klen, d + 1, off);
    if (leaf < 0)
        return -1;
    if ((h->type == ART_N4 && h->count >= 4) ||
        (h->type == ART_N16 && h->count >= 16) ||
        (h->type == ART_N48 && h->count >= 48)) {
        int32_t g = art_grow(kv, h);
        if (g < 0)
            return -1;
        art_add(kv, art_hdr(kv, g), key[d], leaf);
        return g;
    }
    art_add(kv, h, key[d], leaf);
    return nid;
}

/* ---- delete ---- */
static int32_t art_del(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                       int d, bool *del) {
    if (nid < 0)
        return -1;
    art_hdr_t *h = art_hdr(kv, nid);
    if (!h)
        return -1;
    if (h->prefix_len) {
        int s = pfx_shared(h->prefix, h->prefix_len, key + d, klen - d);
        if (s != h->prefix_len || d + h->prefix_len > klen)
            return nid;
        d += h->prefix_len;
    }
    if (d == klen) {
        if (!h->has_value)
            return nid;
        h->has_value = 0;
        kv_sbo_free(kv, h->box_offset);
        h->box_offset = 0;
        *del = true;
    } else {
        int32_t cid = art_child(kv, h, key[d]);
        if (cid >= 0)
            art_del(kv, cid, key, klen, d + 1, del);
    }
    return nid;
}

/* ---- path ---- */
static char *pjoin(const char *a, const char *b) {
    size_t al = strlen(a), bl = strlen(b);
    int sep = (al > 0 && a[al - 1] != '/') ? 1 : 0;
    char *r = malloc(al + sep + bl + 1);
    memcpy(r, a, al);
    if (sep)
        r[al] = '/';
    memcpy(r + al + sep, b, bl + 1);
    return r;
}
static void psplit(const char *k, char **p, char **n) {
    const char *s = strrchr(k, '/');
    if (!s || s == k) {
        *p = strdup("/");
        *n = strdup(s ? s + 1 : k);
        return;
    }
    size_t pl = (size_t)(s - k) + 1; /* 含尾 '/' */
    *p = malloc(pl + 1);
    memcpy(*p, k, pl);
    (*p)[pl] = '\0';
    *n = strdup(s + 1);
}
/* 非法目录前缀（不是 / 且不以 / 或 · 结尾）→ 返回非 0，对齐 durable
 * validate_dir。 */
static int bad_dir_prefix(const char *p) {
    if (!p || !p[0])
        return 1;
    if (strcmp(p, "/") == 0)
        return 0;
    size_t l = strlen(p);
    if (p[l - 1] == '/')
        return 0;
    if (l >= 2 && (unsigned char)p[l - 2] == 0xC2 &&
        (unsigned char)p[l - 1] == 0xB7)
        return 0;
    return 1;
}

static char *edir(const char *p) {
    size_t l = strlen(p);
    if (l > 0 && p[l - 1] == '/')
        return strdup(p);
    char *r = malloc(l + 2);
    memcpy(r, p, l);
    r[l] = '/';
    r[l + 1] = '\0';
    return r;
}
/* Covering node for pfx: *bpos is reconstructed length at arrival. */
static int32_t art_cover(kvspace_t *kv, int32_t nid, const uint8_t *pfx,
                         int plen, char *buf, int bcap, int *bpos) {
    int d = 0;
    if (nid < 0 || !pfx || plen < 0 || !buf || !bpos)
        return -1;
    while (nid >= 0) {
        art_hdr_t *h = art_hdr(kv, nid);
        if (!h)
            return -1;
        int pd = d, cover = 0;
        if (h->prefix_len) {
            int s = pfx_shared(h->prefix, h->prefix_len, pfx + d, plen - d);
            if (s < h->prefix_len) {
                if (d + s < plen)
                    return -1;
                cover = 1;
            } else
                d += h->prefix_len;
        }
        if (cover || d == plen) {
            if (pd >= bcap)
                return -1;
            if (pd)
                memcpy(buf, pfx, (size_t)pd);
            *bpos = pd;
            return nid;
        }
        nid = art_child(kv, h, pfx[d]);
        if (nid < 0)
            return -1;
        d++;
    }
    return -1;
}

/* ---- prefix scan: collect all keys under prefix into out[0..*n-1] ---- */
static void art_scan(kvspace_t *kv, int32_t nid, char *buf, int bpos, int bcap,
                     const char *pfx, int plen, char ***out, int32_t *n) {
    if (nid < 0 || *n >= 4096)
        return;
    art_hdr_t *h = art_hdr(kv, nid);
    if (!h)
        return;
    // write node's prefix into buf
    for (int i = 0; i < h->prefix_len && bpos < bcap; i++)
        buf[bpos++] = h->prefix[i];
    if (bpos >= bcap)
        return;
    // if this node has value, emit key
    if (h->has_value) {
        buf[bpos] = '\0';
        if (bpos >= plen && memcmp(buf, pfx, plen) == 0) {
            (*out)[*n] = strdup(buf);
            (*n)++;
        }
    }
    // recurse into children
    switch (h->type) {
    case ART_N4: {
        art_n4_t *x = (art_n4_t *)h;
        for (int i = 0; i < (int)h->count; i++) {
            if (bpos < bcap)
                buf[bpos] = x->keys[i];
            art_scan(kv, x->children[i], buf, bpos + (bpos < bcap ? 1 : 0), bcap, pfx,
                     plen, out, n);
        }
        break;
    }
    case ART_N16: {
        art_n16_t *x = (art_n16_t *)h;
        for (int i = 0; i < (int)h->count; i++) {
            if (bpos < bcap)
                buf[bpos] = x->keys[i];
            art_scan(kv, x->children[i], buf, bpos + (bpos < bcap ? 1 : 0), bcap, pfx,
                     plen, out, n);
        }
        break;
    }
    case ART_N48: {
        art_n48_t *x = (art_n48_t *)h;
        for (int i = 0; i < 256; i++)
            if (x->index[i] != 255) {
                if (bpos < bcap)
                    buf[bpos] = (uint8_t)i;
                art_scan(kv, x->children[x->index[i]], buf,
                         bpos + (bpos < bcap ? 1 : 0), bcap, pfx, plen, out, n);
            }
        break;
    }
    case ART_N256: {
        art_n256_t *x = (art_n256_t *)h;
        for (int i = 0; i < 256; i++)
            if (x->children[i] >= 0) {
                if (bpos < bcap)
                    buf[bpos] = (uint8_t)i;
                art_scan(kv, x->children[i], buf, bpos + (bpos < bcap ? 1 : 0), bcap,
                         pfx, plen, out, n);
            }
        break;
    }
    }
}

static void art_scan_pfx(kvspace_t *kv, const char *pfx, int plen, char *buf,
                         int bcap, char ***out, int32_t *n) {
    int bpos = 0;
    int32_t nid = art_cover(kv, kv->hdr->art_root, (const uint8_t *)pfx, plen,
                            buf, bcap, &bpos);
    if (nid < 0)
        return;
    art_scan(kv, nid, buf, bpos, bcap, pfx, plen, out, n);
}

/* ============ lifecycle ============ */
/* 8 * 64^k; s wraps to 0. */
static bool sbo_data_size_ok(size_t n) {
    for (uint64_t s = 8; s; s *= 64)
        if (s == n)
            return true;
    return false;
}

kvspace_t *kvspaceShmOpen(const char *path, size_t data_size) {
    if (!path)
        return NULL;
    char head_path[PATH_MAX], data_path[PATH_MAX];
    if (snprintf(head_path, sizeof head_path, "%s.sbo.head", path) >=
            (int)sizeof head_path ||
        snprintf(data_path, sizeof data_path, "%s.sbo.data", path) >=
            (int)sizeof data_path)
        return NULL;

    kvspace_t *kv = calloc(1, sizeof(*kv));
    if (!kv)
        return NULL;
    kv->r_art.fd = kv->r_head.fd = kv->r_data.fd = -1;

    /* validate before O_EXCL create: no empty file left behind */
    bool created = false;
    kv->r_art.fd = open(path, O_RDWR);
    if (kv->r_art.fd < 0) {
        if (!sbo_data_size_ok(data_size))
            goto fail;
        kv->r_art.fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
        if (kv->r_art.fd < 0)
            goto fail;
        created = true;
    }

    size_t art_slab, sbo_head;
    if (created) {
        art_slab = ART_SLAB_INIT;
        sbo_head = sbo_meta_size(data_size, SBO_HEAD_POOL_INIT);
    } else {
        kvspace_hdr_t tmp;
        if (pread(kv->r_art.fd, &tmp, sizeof tmp, 0) != (ssize_t)sizeof tmp ||
            memcmp(tmp.magic, KVS_MAGIC, sizeof(KVS_MAGIC) - 1) != 0)
            goto fail;
        art_slab = (size_t)tmp.art_slab_size;
        sbo_head = (size_t)tmp.sbo_head_size;
        data_size = (size_t)tmp.sbo_data_size;
    }

    /* O_TRUNC: sbo_init rejects a stale magic */
    int fl = created ? O_RDWR | O_CREAT | O_TRUNC : O_RDWR;
    if (region_attach(&kv->r_art, created, ART_OFF + art_slab,
                      REGION_RESERVE) != 0 ||
        (kv->r_head.fd = open(head_path, fl, 0644)) < 0 ||
        region_attach(&kv->r_head, created, sbo_head, REGION_RESERVE) != 0 ||
        (kv->r_data.fd = open(data_path, fl, 0644)) < 0 ||
        region_attach(&kv->r_data, created, data_size, DATA_RESERVE) != 0)
        goto fail;

    kv->hdr = (kvspace_hdr_t *)kv->r_art.base;
    kv->art_meta = (blocks_meta_t *)(kv->r_art.base + sizeof(kvspace_hdr_t));
    kv->art_data = kv->r_art.base + ART_OFF;
    kv->sbo_meta = (sbo_meta_t *)kv->r_head.base;
    kv->sbo_data = kv->r_data.base;

    if (created) {
        memset(kv->hdr, 0, sizeof(*kv->hdr));
        kv->hdr->art_slab_size = art_slab;
        kv->hdr->sbo_head_size = sbo_head;
        kv->hdr->sbo_data_size = data_size;
        kv->hdr->art_root = -1;
        if (blocks_init(kv->art_meta, art_slab, ART_NODE_MAX_SZ) != 0 ||
            sbo_init(kv->sbo_meta, sbo_head, data_size) != 0)
            goto fail;
        /* magic last: half-initialized file fails reopen */
        memcpy(kv->hdr->magic, KVS_MAGIC, sizeof(KVS_MAGIC) - 1);
    }
    /* growth code assumes the single-root pool layout (SBO_HEAD_FIXED) */
    if (kv->sbo_meta->root_slots != 1)
        goto fail;

    pthread_mutex_init(&kv->wlock, NULL);
    for (int i = 0; i < WATCH_TABLE_SZ; i++) {
        pthread_cond_init(&kv->watches[i].cond, NULL);
        pthread_mutex_init(&kv->watches[i].mtx, NULL);
    }
    return kv;

fail:
    region_close(&kv->r_art);
    region_close(&kv->r_head);
    region_close(&kv->r_data);
    free(kv);
    if (created) {
        unlink(path);
        unlink(head_path);
        unlink(data_path);
    }
    return NULL;
}
void kvspaceShmClose(kvspace_t *kv) {
    if (!kv)
        return;
    for (int i = 0; i < WATCH_TABLE_SZ; i++) {
        pthread_cond_destroy(&kv->watches[i].cond);
        pthread_mutex_destroy(&kv->watches[i].mtx);
        free(kv->watches[i].val);
    }
    pthread_mutex_destroy(&kv->wlock);
    region_close(&kv->r_art);
    region_close(&kv->r_head);
    region_close(&kv->r_data);
    free(kv);
}

/* ---- link resolve helpers ---- */
static int read_tlv(kvspace_t *kv, uint64_t off, uint8_t **out, int32_t *ol) {
    uint8_t *s =
        kv->sbo_data + off; /* box 内必含完整 TLV，用 xvalue 解码器算长度 */
    size_t sz = sbo_allocated_size(kv->sbo_meta, off);
    xvalue_head_t h =
        kvspaceXvalueDecodeHead(s, sz > INT32_MAX ? INT32_MAX : (int32_t)sz);
    *out = s; /* SHM pointer */
    if (h.langtype_len == 0) {
        *ol = 0;
        return 0;
    } /* None（空 langtype）→ len 0 */
    *ol = kvspaceXvalueHeadLen(&h) + h.raw_len;
    return 0;
}
/* link 解析：ptr（ref=1）与 @ext（ref=2）的 XValue 恒为叶子（spec 硬规则——指针/扩展键
 * 之后不可能再有成员，成员只挂在 target 上），故路径中任何 `/` 分隔的容器前缀都绝不会是
 * link；唯一可能是 ptr 的只有整键叶子本身。因此只需查整键一次：非 ptr 即完成，ptr 则顺链
 * 到 target（本身又是一个完整叶子键）再查，最多追 16 跳。叶子不存在→非链，原样返回（调用
 * 方另做 extindex/dir 兜底）。取代旧的「逐前缀 1~3 次 art_search + 追加 rest」全下降探测。 */
static void resolve_path(kvspace_t *kv, const char *path, char *out, int osz) {
    strncpy(out, path, osz - 1);
    out[osz - 1] = '\0';
    for (int depth = 0; depth < 16; depth++) {
        art_hdr_t *h =
            art_search(kv, kv->hdr->art_root, (const uint8_t *)out, (int)strlen(out));
        if (!h || !h->has_value)
            return;
        uint8_t *raw;
        int32_t rl;
        if (read_tlv(kv, h->box_offset, &raw, &rl) < 0)
            return;
        xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
        if (hh.ref != 1)
            return;
        int tl = hh.raw_len;
        if (tl <= 0 || tl >= osz)
            return;
        memcpy(out, hh.raw, tl);
        out[tl] = '\0';
    }
}

/* resolve_path 的融合版：解析 link 链的同时把终端叶子的值一并取出，令调用方省掉
 * 「解析后再 art_search + read_tlv」的整套重复下降。每跳只解一次 head（ref 判定与
 * TLV 长度共用），命中带值节点即返 fetched=1（*raw/*rl 就位）。语义与
 * resolve_path + art_search + read_tlv 逐字等价：未命中/无值 → 返回该节点、fetched=0，
 * 交调用方走 extindex/dir 兜底。 */
static art_hdr_t *resolve_fetch(kvspace_t *kv, const char *path, char *out, int osz,
                                uint8_t **raw, int32_t *rl, int *fetched) {
    *fetched = 0;
    strncpy(out, path, osz - 1);
    out[osz - 1] = '\0';
    for (int depth = 0; depth < 16; depth++) {
        art_hdr_t *h =
            art_search(kv, kv->hdr->art_root, (const uint8_t *)out, (int)strlen(out));
        if (!h || !h->has_value)
            return h;
        uint8_t *s = kv->sbo_data + h->box_offset;
        size_t sz = sbo_allocated_size(kv->sbo_meta, h->box_offset);
        xvalue_head_t hh =
            kvspaceXvalueDecodeHead(s, sz > INT32_MAX ? INT32_MAX : (int32_t)sz);
        int32_t tlvlen =
            hh.langtype_len == 0 ? 0 : kvspaceXvalueHeadLen(&hh) + hh.raw_len;
        int tl = hh.raw_len;
        if (hh.ref != 1 || tl <= 0 || tl >= osz) { /* 非 ptr 或无法续链：终端 */
            *raw = s;
            *rl = tlvlen;
            *fetched = 1;
            return h;
        }
        memcpy(out, hh.raw, tl);
        out[tl] = '\0';
    }
    return NULL;
}

/* memindex 定宽矩阵几何：n=dims[0]、m=dims[1]；返回矩阵起点（extindex 矩阵在
 * body 尾部， off=body_len−N*M；普通 index off=0）。head dims 由 DecodeHead 从
 * kindexpr 解出。 */
/* cap 增长：容量足够不变；空取 need；否则从旧容量翻倍覆盖 need（对齐 durable
 * grow_cap）。 */
static int32_t grow_cap(int32_t old_cap, int32_t need) {
    if (need <= old_cap)
        return old_cap;
    if (old_cap == 0)
        return need;
    int32_t c = old_cap;
    while (c < need)
        c *= 2;
    return c;
}

/* memindex 矩阵（dims=[len,cap,M]）：返回矩阵起点（跳过 extindex 的 ext_path
   头部 off=raw_len−cap*M）， 出参 *len=有效成员数=dims[0]、*m=行宽=dims[2]。 */
static const uint8_t *index_matrix(const xvalue_head_t *hh, int32_t *len,
                                   int32_t *m) {
    int32_t l = hh->ndim >= 1 && hh->dims[0] > 0 ? hh->dims[0] : 0;
    int32_t cap = hh->ndim >= 2 && hh->dims[1] > 0 ? hh->dims[1] : 0;
    int32_t mm = hh->ndim >= 3 && hh->dims[2] > 0 ? hh->dims[2] : 0;
    int32_t off = hh->raw_len - cap * mm;
    if (off < 0)
        off = 0;
    *len = l;
    *m = mm;
    return hh->raw + off;
}

/* 定宽矩阵 → malloc 成员名数组（每行去尾 NUL）。 */
static char **index_names(const xvalue_head_t *hh, int32_t *oc) {
    *oc = 0;
    int32_t n, m;
    const uint8_t *mat = index_matrix(hh, &n, &m);
    if (n <= 0)
        return NULL;
    char **names = malloc(sizeof(char *) * (size_t)n);
    if (!names)
        return NULL;
    for (int32_t i = 0; i < n; i++) {
        const char *row = (const char *)mat + (size_t)i * m;
        int32_t len = 0;
        while (len < m && row[len] != 0)
            len++;
        names[i] = strndup(row, (size_t)len);
    }
    *oc = n;
    return names;
}

/* 成员是否已在矩阵中：直扫定宽行 memcmp，零分配（成员密集写的幂等快路径）。 */
static int index_has_member(const xvalue_head_t *hh, const char *name) {
    int32_t n, m;
    const uint8_t *mat = index_matrix(hh, &n, &m);
    if (n <= 0 || m <= 0)
        return 0;
    size_t nl = strlen(name);
    if ((int32_t)nl > m)
        return 0;
    for (int32_t i = 0; i < n; i++) {
        const char *row = (const char *)mat + (size_t)i * m;
        if (memcmp(row, name, nl) == 0 && ((int32_t)nl == m || row[nl] == 0))
            return 1;
    }
    return 0;
}

/* 读 dir（尾斜杠目录键）的 extindex，返回 extpath（body 头部
 * [0..body_len−N*M]）；非 extindex 返回 0。 */
static int dir_ext_path(kvspace_t *kv, const char *dir, char *out, int osz) {
    out[0] = 0;
    art_hdr_t *h =
        art_search(kv, kv->hdr->art_root, (const uint8_t *)dir, (int)strlen(dir));
    if (!h || !h->has_value)
        return 0;
    uint8_t *raw;
    int32_t rl;
    if (read_tlv(kv, h->box_offset, &raw, &rl) < 0)
        return 0;
    xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
    if (hh.kind_len != (int32_t)strlen(KVSPACE_KIND_EXT_INDEX) ||
        memcmp(hh.kind, KVSPACE_KIND_EXT_INDEX, hh.kind_len) != 0)
        return 0;
    int32_t len, m;
    const uint8_t *mat = index_matrix(&hh, &len, &m);
    int32_t el = (int32_t)(mat - hh.raw);
    if (el < 0)
        el = 0;
    if (el >= osz)
        el = osz - 1;
    memcpy(out, hh.raw, (size_t)el);
    out[el] = 0;
    return out[0] ? 1 : 0;
}

/* ============ CRUD ============ */
uint8_t *kvspaceShmGet(kvspace_t *kv, const char *key, int resolve,
                       int32_t *ol) {
    if (!kv || !key || !ol)
        return NULL;
    *ol = 0;
    if (kv_sync(kv) != 0)
        return NULL;
    char kbuf[1024];
    uint8_t *raw;
    int32_t rl;
    int fetched = 0;
    art_hdr_t *h;
    if (resolve)
        h = resolve_fetch(kv, key, kbuf, sizeof(kbuf), &raw, &rl, &fetched);
    else {
        strncpy(kbuf, key, sizeof(kbuf) - 1);
        kbuf[sizeof(kbuf) - 1] = '\0';
        h = art_search(kv, kv->hdr->art_root, (const uint8_t *)kbuf,
                       (int)strlen(kbuf));
    }
    if (!h || !h->has_value) {
        /* extindex fallback：父目录是 extindex → 读 extpath + name */
        char *parent = NULL, *name = NULL;
        psplit(kbuf, &parent, &name);
        char extpath[1024];
        if (dir_ext_path(kv, parent, extpath, sizeof extpath)) {
            char *target = pjoin(extpath, name);
            art_hdr_t *eh = art_search(kv, kv->hdr->art_root, (const uint8_t *)target,
                                       (int)strlen(target));
            free(target);
            if (eh && eh->has_value) {
                uint8_t *raw;
                int32_t rl;
                if (read_tlv(kv, eh->box_offset, &raw, &rl) == 0) {
                    *ol = rl;
                    free(parent);
                    free(name);
                    return raw;
                }
            }
        }
        free(parent);
        free(name);
        return NULL;
    }
    if (!fetched && read_tlv(kv, h->box_offset, &raw, &rl) < 0)
        return NULL;
    *ol = rl;
    return raw;
}

/* gen>0: block_id is parent at depth gen; key must share that prefix. */
static int32_t ref_leaf(kvspace_t *kv, kvspaceRef_t *ref, const char *key) {
    int32_t id = art_follow(kv, (int32_t)ref->block_id);
    if (id < 0)
        return -1;
    ref->block_id = (uint32_t)id;
    if (ref->gen == 0)
        return id;
    if (!key)
        return -1;
    int klen = (int)strlen(key);
    int d = (int)ref->gen;
    if (d < 0 || d >= klen)
        return -1;
    art_hdr_t *h = art_hdr(kv, id);
    if (!h || h->type == ART_MOVED)
        return -1;
    int32_t cid = art_child(kv, h, (uint8_t)key[d]);
    if (cid < 0)
        return -1;
    return art_walk(kv, cid, (const uint8_t *)key, klen, d + 1, NULL, NULL);
}

int kvspaceShmResolveRef(kvspace_t *kv, const char *key, kvspaceRef_t *ref) {
    if (!kv || !key || !ref)
        return -1;
    memset(ref, 0, sizeof(*ref));
    if (kv_sync(kv) != 0)
        return -1;
    int klen = (int)strlen(key);
    int seplen = 0;
    int sep = last_key_sep((const uint8_t *)key, klen, &seplen);
    int dir_d = 0;
    int32_t dn = -1;
    int32_t id = art_find_dir(kv, kv->hdr->art_root, (const uint8_t *)key, klen,
                              sep, seplen, &dn, &dir_d);
    if (id < 0)
        return -1;
    ref->block_id = (uint32_t)id;
    ref->gen = 0;
    if (dn >= 0 && dir_d > 0) {
        ref->parent_id = (uint32_t)dn;
        ref->depth = (uint32_t)dir_d;
    } else {
        ref->parent_id = 0;
        ref->depth = 0;
    }
    return 0;
}

uint8_t *kvspaceShmGetByRef(kvspace_t *kv, kvspaceRef_t *ref,
                            const char *key_fallback, int32_t *ol) {
    if (!kv || !ref || !ol)
        return NULL;
    *ol = 0;
    if (kv_sync(kv) != 0)
        return NULL;
    int32_t id = ref_leaf(kv, ref, key_fallback);
    art_hdr_t *h = id >= 0 ? art_hdr(kv, id) : NULL;
    if (h && h->has_value) {
        uint8_t *raw;
        int32_t rl;
        if (read_tlv(kv, h->box_offset, &raw, &rl) == 0) {
            *ol = rl;
            return raw;
        }
    }
    /* gen>0: parent walk miss — do not full-Get; caller falls back. */
    if (ref->gen != 0)
        return NULL;
    if (!key_fallback)
        return NULL;
    uint8_t *raw = kvspaceShmGet(kv, key_fallback, 0, ol);
    if (raw)
        kvspaceShmResolveRef(kv, key_fallback, ref);
    return raw;
}

int kvspaceShmSetPartByRef(kvspace_t *kv, kvspaceRef_t *ref,
                           const char *key_fallback, uint32_t offset,
                           const uint8_t *buf, uint32_t buf_len) {
    if (!kv || !ref || !buf)
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    int32_t id = ref_leaf(kv, ref, key_fallback);
    art_hdr_t *h = id >= 0 ? art_hdr(kv, id) : NULL;
    if (!h || !h->has_value) {
        if (ref->gen != 0)
            return -1;
        if (!key_fallback)
            return -1;
        int32_t rl = 0;
        uint8_t *d = kvspaceShmGet(kv, key_fallback, 0, &rl);
        if (!d || offset + buf_len > (uint32_t)rl)
            return -1;
        memcpy(d + offset, buf, buf_len);
        kvspaceShmResolveRef(kv, key_fallback, ref);
        return 0;
    }
    uint8_t *raw;
    int32_t rl;
    if (read_tlv(kv, h->box_offset, &raw, &rl) < 0 ||
        offset + buf_len > (uint32_t)rl)
        return -1;
    memcpy(raw + offset, buf, buf_len);
    return 0;
}

/* ── 值/索引分离（方案2，对齐 kvspace-durable backend.rs） ────────── */
/* 坐标段工具见 xvalue.c：kvspaceCoordIsCoord / kvspaceParseCoord /
 * kvspaceCoordCmp。 */

/* kind 判定（kindexpr 非 NUL 终止）。 */
static int is_kind(const xvalue_head_t *h, const char *k) {
    int32_t kl = (int32_t)strlen(k);
    return h->kind_len == kl && memcmp(h->kind, k, (size_t)kl) == 0;
}

/* 原始落盘（不处理容器/member 语义，供内部调用，避免递归）。 */
static int shm_set_raw(kvspace_t *kv, const char *key, const uint8_t *val,
                       int32_t val_len) {
    art_hdr_t *old =
        art_search(kv, kv->hdr->art_root, (const uint8_t *)key, (int)strlen(key));
    if (old && old->has_value) {
        /* 同尺寸原地覆写：新值 ≤ 旧 box 容量时直接 memcpy，跳过 free+alloc，
           不改共享 buddy 树。容量读自共享 mmap，零进程内状态。 */
        uint64_t cap = sbo_allocated_size(kv->sbo_meta, old->box_offset);
        if ((uint64_t)val_len <= cap) {
            memcpy(kv->sbo_data + old->box_offset, val, (size_t)val_len);
            return 0;
        }
        kv_sbo_free(kv, old->box_offset);
    }
    uint64_t off = kv_sbo_alloc(kv, (size_t)val_len);
    if (off == (uint64_t)-1)
        return -1;
    memcpy(kv->sbo_data + off, val, val_len);
    kv->hdr->art_root = art_ins(kv, kv->hdr->art_root, (const uint8_t *)key,
                                (int)strlen(key), 0, off);
    return 0;
}

/* 去掉尾部分隔符（/ 或 ·），返回 malloc；根 "/" 保持 "/"。 */
static char *strip_dir_suf_alloc(const char *p) {
    size_t l = strlen(p);
    if (l == 0)
        return strdup(p);
    if (l >= 2 && (unsigned char)p[l - 2] == 0xC2 &&
        (unsigned char)p[l - 1] == 0xB7)
        return strndup(p, l - 2);
    if (p[l - 1] == '/') {
        if (l == 1)
            return strdup("/");
        return strndup(p, l - 1);
    }
    return strdup(p);
}

/* base + "·"（memindex 键）。 */
static char *memjoin(const char *base) {
    size_t l = strlen(base);
    char *r = malloc(l + 3);
    memcpy(r, base, l);
    r[l] = (char)0xC2;
    r[l + 1] = (char)0xB7;
    r[l + 2] = 0;
    return r;
}

/* 找字符串内最后一个 ·（U+00B7，2 字节），无则返回 NULL。 */
static const char *strrstr_mid(const char *s) {
    const char *last = NULL;
    for (const char *p = s; *p; p++)
        if ((unsigned char)p[0] == 0xC2 && (unsigned char)p[1] == 0xB7)
            last = p;
    return last;
}

/* 解析 key 的父目录/成员名（对齐 durable split_index，取末段最后一个
 * ·）。父目录含尾分隔符。 */
static void shm_split_index(const char *key, char **parent, char **name,
                            bool *is_member) {
    *parent = NULL;
    *name = NULL;
    *is_member = false;
    const char *s = strrchr(key, '/');
    const char *last;
    size_t plen;
    if (!s || s == key) {
        plen = 1; /* 父前缀 "/" */
        last = s ? s + 1 : key;
    } else {
        plen = (size_t)(s - key) + 1; /* 含尾 '/' */
        last = s + 1;
    }
    const char *dot = strrstr_mid(last);
    if (dot && dot != last && dot[2]) {
        size_t prelen = (size_t)(dot - last);
        char *p = malloc(plen + prelen + 2 + 1);
        if (plen == 1 && (s == key)) {
            p[0] = '/';
            memcpy(p + 1, last, prelen + 2);
            p[plen + prelen + 2] = 0;
        } else {
            memcpy(p, key, plen);
            memcpy(p + plen, last, prelen + 2);
            p[plen + prelen + 2] = 0;
        }
        *parent = p;
        *name = strdup(dot + 2);
        *is_member = true;
    } else {
        char *p = malloc(plen + 1);
        if (plen == 1 && (s == key)) {
            p[0] = '/';
            p[1] = 0;
        } else {
            memcpy(p, key, plen);
            p[plen] = 0;
        }
        *parent = p;
        *name = strdup(last);
    }
}

/* 坐标段 → stringkeymap dims（对齐 durable grow_coord_dims，单成员）。 */
static void grow_coord_dims_one(const char *name, int32_t *dims,
                                int32_t *ndim) {
    int64_t coords[8];
    int n = kvspaceParseCoord(name, coords, 8);
    if (n < 0) {
        dims[0] = 1;
        *ndim = 1;
        return;
    }
    *ndim = n;
    for (int i = 0; i < n; i++)
        dims[i] = (int32_t)(coords[i] + 1);
}

/* 确保 memindex 存在（不存在 → 建空 index）。 */
static void ensure_memindex(kvspace_t *kv, const char *mem) {
    art_hdr_t *h =
        art_search(kv, kv->hdr->art_root, (const uint8_t *)mem, (int)strlen(mem));
    if (h && h->has_value)
        return;
    uint8_t *iv;
    int32_t ivl = kvspaceXvalueNewIndex(NULL, 0, &iv);
    shm_set_raw(kv, mem, iv, ivl);
    free(iv);
}

/* 向 memindex 追加成员名（幂等）。 */
static int add_child_index(kvspace_t *kv, const char *mem, const char *name) {
    char **names = NULL;
    int32_t nnames = 0;
    int32_t old_cap = 0, old_m = 0;
    art_hdr_t *h =
        art_search(kv, kv->hdr->art_root, (const uint8_t *)mem, (int)strlen(mem));
    if (h && h->has_value) {
        uint8_t *raw;
        int32_t rl;
        if (read_tlv(kv, h->box_offset, &raw, &rl) == 0) {
            xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
            if (hh.ref == 0 && is_kind(&hh, KVSPACE_KIND_EXT_INDEX))
                return 0; /* extindex：成员由 extpath 展开，不维护本地 childs */
            if (hh.ref == 0 && is_kind(&hh, KVSPACE_KIND_INDEX)) {
                if (index_has_member(&hh, name))
                    return 1; /* 已存在：零分配快路径，不物化 names、不重建矩阵 */
                names = index_names(&hh, &nnames);
                old_cap = hh.ndim >= 2 && hh.dims[1] > 0 ? hh.dims[1] : 0;
                old_m = hh.ndim >= 3 && hh.dims[2] > 0 ? hh.dims[2] : 0;
            }
        }
    }
    char **nn = realloc(names, sizeof(char *) * (size_t)(nnames + 1));
    if (!nn) {
        for (int32_t j = 0; j < nnames; j++)
            free(names[j]);
        free(names);
        return -1;
    }
    nn[nnames] = strdup(name);
    uint8_t *iv;
    int32_t ivl = kvspaceXvalueNewIndexGrow(
        (const char **)nn, nnames + 1, grow_cap(old_cap, nnames + 1), old_m, &iv);
    int rc = shm_set_raw(kv, mem, iv, ivl);
    free(iv);
    for (int32_t j = 0; j <= nnames; j++)
        free(nn[j]);
    free(nn);
    return rc;
}

/* 从 memindex 移除成员名（幂等）。 */
static int remove_child_index(kvspace_t *kv, const char *mem,
                              const char *name) {
    art_hdr_t *h =
        art_search(kv, kv->hdr->art_root, (const uint8_t *)mem, (int)strlen(mem));
    if (!h || !h->has_value)
        return 0;
    uint8_t *raw;
    int32_t rl;
    if (read_tlv(kv, h->box_offset, &raw, &rl) < 0)
        return 0;
    xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
    if (hh.ref != 0 || !is_kind(&hh, KVSPACE_KIND_INDEX))
        return 0;
    int32_t old_cap = hh.ndim >= 2 && hh.dims[1] > 0 ? hh.dims[1] : 0;
    int32_t old_m = hh.ndim >= 3 && hh.dims[2] > 0 ? hh.dims[2] : 0;
    int32_t nnames;
    char **names = index_names(&hh, &nnames);
    if (!names)
        return 0;
    int32_t j = 0;
    for (int32_t i = 0; i < nnames; i++) {
        if (strcmp(names[i], name) == 0) {
            free(names[i]);
            continue;
        }
        names[j++] = names[i];
    }
    if (j == nnames) {
        for (int32_t i = 0; i < j; i++)
            free(names[i]);
        free(names);
        return 0;
    }
    uint8_t *iv;
    int32_t ivl =
        kvspaceXvalueNewIndexGrow((const char **)names, j, old_cap, old_m, &iv);
    int rc = shm_set_raw(kv, mem, iv, ivl);
    free(iv);
    for (int32_t i = 0; i < j; i++)
        free(names[i]);
    free(names);
    return rc;
}

/* 写成员时沿父链逐层兜底容器值（leaf base + 中间层
 * stringkeymap）并注册成员（对齐 durable）。 parent 是尾 ·
 * 的成员父目录，name 是该成员名；逐层向上建容器值并注册成员到各自 memindex。 */
static void ensure_member_chain(kvspace_t *kv, char *parent, char *name) {
    char *dir = strdup(parent);
    char *child = strdup(name);
    for (;;) {
        char *base = strip_dir_suf_alloc(dir);
        art_hdr_t *ch = art_search(kv, kv->hdr->art_root, (const uint8_t *)base,
                                   (int)strlen(base));
        if (!ch || !ch->has_value) {
            if (kvspaceCoordIsCoord(child)) {
                int32_t dims[8];
                int32_t ndim;
                grow_coord_dims_one(child, dims, &ndim);
                uint8_t *mv;
                int32_t mvl =
                    kvspaceXvalueEncode(KVSPACE_KIND_MAP, NULL, 0, dims, ndim, &mv);
                shm_set_raw(kv, base, mv, mvl);
                free(mv);
            } else {
                int32_t odims[1] = { 0 };
                uint8_t *ov;
                int32_t ovl =
                    kvspaceXvalueEncode(KVSPACE_KIND_MAP, NULL, 0, odims, 1, &ov);
                shm_set_raw(kv, base, ov, ovl);
                free(ov);
            }
        }
        ensure_memindex(kv, dir);
        if (add_child_index(kv, dir, child) == 1) {
            /* 叶子成员已存在 → 祖先链早已建立，steady-state 写无需上溯 */
            free(base);
            free(dir);
            free(child);
            break;
        }
        char *pp = NULL, *pn = NULL;
        bool pm = false;
        shm_split_index(base, &pp, &pn, &pm);
        free(base);
        if (!pm) {
            free(pp);
            free(pn);
            free(dir);
            free(child);
            break; /* 父是层级目录（如 /），shm 不维护根 index */
        }
        free(dir);
        dir = pp;
        free(child);
        child = pn;
    }
}

/* 非容器写的父索引维护：member → ensure_member_chain；dir index/extindex →
   注册父 index。 kvspaceShmSet 与零拷贝 kvspaceShmWriteNewPlace
   共用，杜绝逻辑分叉。 */
static void shm_ensure_indexes(kvspace_t *kv, const char *kbuf,
                               const xvalue_head_t *hh) {
    char *parent = NULL, *name = NULL;
    bool is_member = false;
    shm_split_index(kbuf, &parent, &name, &is_member);
    if (is_member) {
        ensure_member_chain(kv, parent, name);
        free(parent);
        free(name);
        return;
    }
    free(parent);
    free(name);
    size_t l = strlen(kbuf);
    bool is_dir = (l > 0 && kbuf[l - 1] == '/') ||
                  (l >= 2 && (unsigned char)kbuf[l - 2] == 0xC2 &&
                   (unsigned char)kbuf[l - 1] == 0xB7);
    if (is_dir && hh->ref == 0 &&
        (is_kind(hh, KVSPACE_KIND_INDEX) ||
         is_kind(hh, KVSPACE_KIND_EXT_INDEX))) {
        char *strip = strip_dir_suf_alloc(kbuf);
        char *pp = NULL, *pn = NULL;
        bool pm = false;
        shm_split_index(strip, &pp, &pn, &pm);
        if (pn && pn[0]) {
            char *dn = pn;
            if (kbuf[l - 1] == '/') {
                size_t nl = strlen(pn);
                dn = malloc(nl + 2);
                memcpy(dn, pn, nl);
                dn[nl] = '/';
                dn[nl + 1] = 0;
            }
            add_child_index(kv, pp, dn);
            if (dn != pn)
                free(dn);
        }
        free(pp);
        free(pn);
        free(strip);
    }
}

/* langtype 前导 [dims] → 物理 dims（仅 ARRAYND 有意义）；无则 ndim=0。 */
static int32_t parse_langtype_dims(const char *lt, int32_t *dims) {
    int32_t nd = 0;
    if (lt && lt[0] == '[') {
        const char *p = lt + 1;
        while (*p && *p != ']' && nd < X_MAX_NDIM) {
            int32_t d = 0;
            while (*p >= '0' && *p <= '9')
                d = d * 10 + (*p++ - '0');
            dims[nd++] = d;
            if (*p == ',')
                p++;
        }
    }
    return nd;
}

/* 分配 box、就地写三轴 head（ref/storetype/langtype, body_len），art_ins
   挂树，返回 body 偏移指针。 已存在 key 先释放旧 box（新位置写=换
   box）。零拷贝写路径唯一分配点。 */
static int shm_alloc_head(kvspace_t *kv, const char *key, uint8_t ref,
                          uint8_t storetype, uint8_t ro, uint32_t vid,
                          const char *langtype, int32_t headlen,
                          int32_t body_len, uint8_t **body) {
    int32_t total = headlen + body_len;
    art_hdr_t *old =
        art_search(kv, kv->hdr->art_root, (const uint8_t *)key, (int)strlen(key));
    if (old && old->has_value)
        kv_sbo_free(kv, old->box_offset);
    uint64_t off = kv_sbo_alloc(kv, (size_t)total);
    if (off == (uint64_t)-1)
        return -1;
    int32_t dims[X_MAX_NDIM];
    int32_t ndim = parse_langtype_dims(langtype, dims);
    kvspaceXvalueWriteHead(kv->sbo_data + off, ref, storetype, ro, vid, langtype,
                           dims, ndim, body_len);
    kv->hdr->art_root = art_ins(kv, kv->hdr->art_root, (const uint8_t *)key,
                                (int)strlen(key), 0, off);
    *body = kv->sbo_data + off + headlen;
    return 0;
}

int kvspaceShmSet(kvspace_t *kv, const char *key, const uint8_t *val,
                  int32_t val_len) {
    if (!kv || !key)
        return -1;
    if (!val && val_len > 0)
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    if (val_len <= 0) {
        /* None → 写 1 字节空 kind TLV（sbo 不支持 0 字节），读时 read_tlv 判 None
         * 返 len 0。 */
        static const uint8_t none_tlv[1] = {0};
        val = none_tlv;
        val_len = 1;
    }
    char kbuf[1024];
    resolve_path(kv, key, kbuf, sizeof(kbuf)); // always resolve through link
    if (strstr(kbuf, "//"))
        return -1;

    /* extindex 写保护：父是只读扩展层、本地无同名节点但扩展层有 → 禁止写（对齐
     * durable backend.rs / fs）。以「父是否 extindex」为唯一首闸——非 extindex（如
     * 全部 /lib 直写）父读一次即放行，不触碰整键查找，热路径开销与 redis 对齐。 */
    {
        char *pp = NULL, *nn = NULL;
        psplit(kbuf, &pp, &nn);
        char extpath[1024];
        if (pp && nn && dir_ext_path(kv, pp, extpath, sizeof extpath)) {
            art_hdr_t *lh = art_search(kv, kv->hdr->art_root,
                                       (const uint8_t *)kbuf, (int)strlen(kbuf));
            if (!lh || !lh->has_value) {
                char *tgt = pjoin(extpath, nn);
                art_hdr_t *eh = art_search(kv, kv->hdr->art_root,
                                           (const uint8_t *)tgt, (int)strlen(tgt));
                free(tgt);
                if (eh && eh->has_value) {
                    free(pp);
                    free(nn);
                    return -1;
                }
            }
        }
        free(pp);
        free(nn);
    }

    xvalue_head_t hh = kvspaceXvalueDecodeHead(val, val_len);

    /* 目录 kind（index/extindex）必须落在目录键（尾 / 或 ·）。 */
    if (hh.ref == 0 && (is_kind(&hh, KVSPACE_KIND_INDEX) ||
                        is_kind(&hh, KVSPACE_KIND_EXT_INDEX))) {
        size_t l = strlen(kbuf);
        bool is_dir = (l > 0 && kbuf[l - 1] == '/') ||
                      (l >= 2 && (unsigned char)kbuf[l - 2] == 0xC2 &&
                       (unsigned char)kbuf[l - 1] == 0xB7);
        if (!is_dir)
            return -1;
    }

    /* 容器值（stringkeymap）：值写 p（无后缀、body 空、dims/ro/vid
     * 保留）， memindex p· 写空 index（成员由后续 add_child 维护）；对齐 durable
     * set() 的 Obj/Map 分支。 */
    if (hh.ref == 0 &&
        is_kind(&hh, KVSPACE_KIND_MAP)) {
        char *base = strip_dir_suf_alloc(kbuf);
        if (!base || !base[0]) {
            free(base);
            return -1;
        }
        char *kind = strndup(hh.kind, hh.kind_len);
        uint8_t *cv;
        int32_t cvl = kvspaceXvalueEncodeMode(kind, NULL, 0, hh.dims, hh.ndim, 0,
                                              hh.ro, hh.vid, &cv);
        int rc = shm_set_raw(kv, base, cv, cvl);
        free(cv);
        free(kind);
        if (rc < 0) {
            free(base);
            return -1;
        }
        char *mem = memjoin(base);
        uint8_t *iv;
        int32_t ivl = kvspaceXvalueNewIndex(NULL, 0, &iv);
        rc = shm_set_raw(kv, mem, iv, ivl);
        free(iv);
        free(mem);
        /* 注册 base 为其父 memindex 成员（嵌套容器）。 */
        char *pp = NULL, *pn = NULL;
        bool pm = false;
        shm_split_index(base, &pp, &pn, &pm);
        if (pm)
            add_child_index(kv, pp, pn);
        free(pp);
        free(pn);
        free(base);
        return rc;
    }

    /* 成员/目录索引维护（与 WriteNewPlace 共用），随后落盘。 */
    shm_ensure_indexes(kv, kbuf, &hh);
    return shm_set_raw(kv, kbuf, val, val_len);
}

int kvspaceShmWriteInPlace(kvspace_t *kv, const char *key, int resolve,
                           int32_t body_len, uint8_t **body) {
    if (!kv || !key || !body || body_len < 0)
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    char kbuf[1024];
    uint8_t *raw;
    int32_t rl;
    int fetched = 0;
    art_hdr_t *h;
    if (resolve)
        h = resolve_fetch(kv, key, kbuf, sizeof(kbuf), &raw, &rl, &fetched);
    else {
        strncpy(kbuf, key, sizeof(kbuf) - 1);
        kbuf[sizeof(kbuf) - 1] = '\0';
        h = art_search(kv, kv->hdr->art_root, (const uint8_t *)kbuf,
                       (int)strlen(kbuf));
    }
    if (!h || !h->has_value)
        return -1;
    if ((!fetched && read_tlv(kv, h->box_offset, &raw, &rl) < 0) || rl <= 0)
        return -1; /* None 或读失败 → 强制走 NewPlace */
    xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
    if (hh.raw_len != body_len)
        return -1; /* 前置条件：同 body_len（同 kind 覆写） */
    *body = raw + kvspaceXvalueHeadLen(&hh);
    return 0;
}

int kvspaceShmWriteNewPlace(kvspace_t *kv, const char *key, uint8_t ref,
                            uint8_t storetype, uint8_t ro, uint32_t vid,
                            const char *langtype, int32_t body_len,
                            uint8_t **body) {
    if (!kv || !key || !langtype || !body || body_len < 0)
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    char kbuf[1024];
    strncpy(kbuf, key, sizeof(kbuf) - 1);
    kbuf[sizeof(kbuf) - 1] = '\0'; /* 写键本身，不穿透 link——显式解引用由 runtime 掌控 */
    if (strstr(kbuf, "//"))
        return -1;

    int32_t hdims[X_MAX_NDIM];
    int32_t hndim = parse_langtype_dims(langtype, hdims);
    int32_t headlen = kvspaceXvalueHeadLenForLangtype(storetype, langtype, hndim);
    uint8_t hbuf[512];
    if (headlen > (int32_t)sizeof(hbuf))
        return -1;
    kvspaceXvalueWriteHead(hbuf, ref, storetype, ro, vid, langtype, hdims, hndim, 0);
    xvalue_head_t hh = kvspaceXvalueDecodeHead(hbuf, headlen);

    size_t l = strlen(kbuf);
    bool is_dir = (l > 0 && kbuf[l - 1] == '/') ||
                  (l >= 2 && (unsigned char)kbuf[l - 2] == 0xC2 &&
                   (unsigned char)kbuf[l - 1] == 0xB7);
    if (hh.ref == 0 &&
        (is_kind(&hh, KVSPACE_KIND_INDEX) ||
         is_kind(&hh, KVSPACE_KIND_EXT_INDEX)) &&
        !is_dir)
        return -1;

    /* 容器值（stringkeymap，body 恒空）：base 空 box + 空 memindex +
     * 注册父。 */
    if (hh.ref == 0 &&
        is_kind(&hh, KVSPACE_KIND_MAP)) {
        if (body_len != 0)
            return -1;
        char *base = strip_dir_suf_alloc(kbuf);
        if (!base || !base[0]) {
            free(base);
            return -1;
        }
        char *mem = memjoin(base);
        uint8_t *iv;
        int32_t ivl = kvspaceXvalueNewIndex(NULL, 0, &iv);
        if (ivl > 0) {
            shm_set_raw(kv, mem, iv, ivl);
            free(iv);
        }
        free(mem);
        char *pp = NULL, *pn = NULL;
        bool pm = false;
        shm_split_index(base, &pp, &pn, &pm);
        if (pm)
            add_child_index(kv, pp, pn);
        free(pp);
        free(pn);
        int rc =
            shm_alloc_head(kv, base, ref, storetype, ro, vid, langtype, headlen, 0, body);
        free(base);
        return rc;
    }

    shm_ensure_indexes(kv, kbuf, &hh);
    return shm_alloc_head(kv, kbuf, ref, storetype, ro, vid, langtype, headlen,
                          body_len, body);
}

int kvspaceShmListLen(kvspace_t *kv, const char *prefix, bool ex, int resolve,
                      int32_t *out_count) {
    char **names;
    int32_t count;
    if (kvspaceShmList(kv, prefix, ex, resolve, &names, &count) != 0) {
        *out_count = 0;
        return -1;
    }
    for (int32_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
    *out_count = count;
    return 0;
}

int kvspaceShmDel(kvspace_t *kv, const char *key) {
    if (!kv || !key)
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    char kbuf[1024];
    strncpy(kbuf, key, sizeof(kbuf) - 1);
    kbuf[sizeof(kbuf) - 1] = '\0'; /* 删键本身，不穿透 link——显式解引用由 runtime 掌控 */
    /* memindex 成员删除 → 同步从 p· 的 index 移除。 */
    char *parent = NULL, *name = NULL;
    bool is_member = false;
    shm_split_index(kbuf, &parent, &name, &is_member);
    if (is_member)
        remove_child_index(kv, parent, name);
    free(parent);
    free(name);
    bool d = false;
    kv->hdr->art_root = art_del(kv, kv->hdr->art_root, (const uint8_t *)kbuf,
                                (int)strlen(kbuf), 0, &d);
    return d ? 0 : -1;
}

int kvspaceShmDeltree(kvspace_t *kv, const char *prefix) {
    if (!kv || !prefix)
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    // if prefix itself is a link, only delete the link
    art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)prefix,
                              (int)strlen(prefix));
    if (h && h->has_value) {
        uint8_t *raw;
        int32_t rl;
        read_tlv(kv, h->box_offset, &raw, &rl);
        xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
        if (hh.ref == 1) {
            return kvspaceShmDel(kv, prefix);
        }
    }
    char *e = edir(prefix); // ensure trailing / for listing children
    char **ns;
    int32_t nc;
    kvspaceShmList(kv, e, false, 1, &ns, &nc);
    for (int i = 0; i < nc; i++) {
        char *c = pjoin(e, ns[i]);
        kvspaceShmDeltree(kv, c);
        free(c);
    }
    for (int i = 0; i < nc; i++)
        free(ns[i]);
    free(ns);

    /* 成员目录 marker（prefix·，U+00B7）：成员 key =
       prefix·<name>，直接拼接后递归删除。 slash 版 e=prefix/ 覆盖不到 ·
       成员（json 的 stringkeymap 落盘形态）。 */
    size_t pl = strlen(prefix);
    char *m = malloc(pl + 3);
    memcpy(m, prefix, pl);
    m[pl] = (char)0xC2;
    m[pl + 1] = (char)0xB7;
    m[pl + 2] = 0;
    char **ms;
    int32_t mc;
    kvspaceShmList(kv, m, false, 1, &ms, &mc);
    for (int i = 0; i < mc; i++) {
        size_t ml = strlen(m), nl = strlen(ms[i]);
        char *c = malloc(ml + nl + 1);
        memcpy(c, m, ml);
        memcpy(c + ml, ms[i], nl + 1);
        kvspaceShmDeltree(kv, c);
        free(c);
    }
    for (int i = 0; i < mc; i++)
        free(ms[i]);
    free(ms);
    kvspaceShmDel(kv, m);
    free(m);

    kvspaceShmDel(kv, prefix);
    if (strcmp(e, prefix) != 0)
        kvspaceShmDel(kv, e);
    free(e);
    return 0;
}

/* 成员目录 marker（p·，U+00B7）。 */
static char *memdir(const char *p) {
    size_t l = strlen(p);
    char *r = malloc(l + 3);
    memcpy(r, p, l);
    r[l] = (char)0xC2;
    r[l + 1] = (char)0xB7;
    r[l + 2] = 0;
    return r;
}

/* 单 key 原样拷贝（Set 可能移动 slab，先拷出）。 */
int kvspaceShmCp(kvspace_t *kv, const char *src, const char *dst) {
    if (!kv || !src || !dst)
        return -1;
    int32_t rl;
    uint8_t *raw = kvspaceShmGet(kv, src, 0, &rl);
    if (!raw || rl <= 0)
        return -1;
    uint8_t *tmp = malloc((size_t)rl);
    memcpy(tmp, raw, (size_t)rl);
    int rc = kvspaceShmSet(kv, dst, tmp, rl);
    free(tmp);
    return rc;
}

/* 递归拷贝：镜像 kvspaceShmDeltree 的遍历（/ 子节点 + · 成员），逐 key
   原样复制。 index 由 ART 前缀扫描派生，故写入 dst 各 key
   即自动重建目录；extindex marker 一并复制。 */
static int cptree_rec(kvspace_t *kv, const char *src, const char *dst) {
    {
        int32_t rl;
        uint8_t *raw = kvspaceShmGet(kv, src, 0, &rl);
        if (raw && rl > 0) {
            uint8_t *tmp = malloc((size_t)rl);
            memcpy(tmp, raw, (size_t)rl);
            kvspaceShmSet(kv, dst, tmp, rl);
            free(tmp);
        }
    }
    char *es = edir(src), *ed = edir(dst);
    char **ns;
    int32_t nc;
    kvspaceShmList(kv, es, false, 1, &ns, &nc);
    for (int i = 0; i < nc; i++) {
        char *cs = pjoin(es, ns[i]), *cd = pjoin(ed, ns[i]);
        cptree_rec(kv, cs, cd);
        free(cs);
        free(cd);
    }
    for (int i = 0; i < nc; i++)
        free(ns[i]);
    free(ns);
    free(es);
    free(ed);

    char *ms = memdir(src), *md = memdir(dst);
    kvspaceShmCp(kv, ms,
                 md); /* memindex marker 值（extindex marker / map dims）本身。 */
    char **mms;
    int32_t mc;
    kvspaceShmList(kv, ms, false, 1, &mms, &mc);
    for (int i = 0; i < mc; i++) {
        size_t msl = strlen(ms), mdl = strlen(md), nl = strlen(mms[i]);
        char *cs = malloc(msl + nl + 1);
        memcpy(cs, ms, msl);
        memcpy(cs + msl, mms[i], nl + 1);
        char *cd = malloc(mdl + nl + 1);
        memcpy(cd, md, mdl);
        memcpy(cd + mdl, mms[i], nl + 1);
        cptree_rec(kv, cs, cd);
        free(cs);
        free(cd);
    }
    for (int i = 0; i < mc; i++)
        free(mms[i]);
    free(mms);
    free(ms);
    free(md);
    return 0;
}

int kvspaceShmCptree(kvspace_t *kv, const char *src, const char *dst) {
    if (!kv || !src || !dst)
        return -1;
    kvspaceShmDeltree(kv, dst); /* 覆盖语义：先清 dst 子树。 */
    return cptree_rec(kv, src, dst);
}

/* 浅拷贝：base 值 + 一层 · 成员（不遍历 / 子节点、不递归成员子树）。用于单
 * struct/扁平容器。 */
int kvspaceShmCplist(kvspace_t *kv, const char *src, const char *dst) {
    if (!kv || !src || !dst)
        return -1;
    kvspaceShmDeltree(kv, dst); /* 覆盖语义：先清 dst 子树。 */
    kvspaceShmCp(kv, src, dst); /* base 值。 */
    char *ms = memdir(src), *md = memdir(dst);
    kvspaceShmCp(kv, ms, md); /* memindex marker（成员名单 / map dims）。 */
    char **mms;
    int32_t mc;
    kvspaceShmList(kv, ms, false, 1, &mms, &mc);
    size_t msl = strlen(ms), mdl = strlen(md);
    for (int i = 0; i < mc; i++) {
        size_t nl = strlen(mms[i]);
        char *cs = malloc(msl + nl + 1);
        memcpy(cs, ms, msl);
        memcpy(cs + msl, mms[i], nl + 1);
        char *cd = malloc(mdl + nl + 1);
        memcpy(cd, md, mdl);
        memcpy(cd + mdl, mms[i], nl + 1);
        kvspaceShmCp(kv, cs, cd); /* 成员值，单 key，不递归。 */
        free(cs);
        free(cd);
        free(mms[i]);
    }
    free(mms);
    free(ms);
    free(md);
    return 0;
}

int kvspaceShmMkindex(kvspace_t *kv, const char *path, uint32_t capacity) {
    if (!kv || !path)
        return -1;
    char *d = edir(path);
    uint8_t *v;
    int32_t vl = kvspaceXvalueNewIndexGrow(NULL, 0, (int32_t)capacity, 0, &v);
    int r = kvspaceShmSet(kv, d, v, vl);
    free(v);
    free(d);
    return r;
}

/* 读 memindex（p·）的成员名：index 定宽矩阵 body 是成员名唯一权威，encode
 * 侧已按 cmp_coord 规范排序（坐标 row-major
 * 数值序、非坐标字典序），读侧原样返回矩阵行序即有序。 仅 p·（尾
 * ·）目录命中；slash 目录（p/）不自动维护 index，调用方回退 ART scan。 */
static int read_index_names(kvspace_t *kv, const char *dir, char ***on,
                            int32_t *oc) {
    *on = NULL;
    *oc = 0;
    size_t dl = strlen(dir);
    if (dl < 2 || !((unsigned char)dir[dl - 2] == 0xC2 &&
                    (unsigned char)dir[dl - 1] == 0xB7))
        return 0; /* 仅 memindex（p·） */
    art_hdr_t *h =
        art_search(kv, kv->hdr->art_root, (const uint8_t *)dir, (int)dl);
    if (!h || !h->has_value)
        return 0;
    uint8_t *raw;
    int32_t rl;
    if (read_tlv(kv, h->box_offset, &raw, &rl) < 0)
        return 0;
    xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
    if (hh.ref != 0 || !is_kind(&hh, KVSPACE_KIND_INDEX))
        return 0;
    *on = index_names(&hh, oc);
    return 1;
}

/* 提取直接成员名长度：到第一个 / 或 ·（U+00B7，2 字节）为止。 */
static int child_name_len(const char *rest, int restlen) {
    for (int i = 0; i < restlen; i++) {
        if (rest[i] == '/')
            return i;
        if (i + 1 < restlen && (unsigned char)rest[i] == 0xC2 &&
            (unsigned char)rest[i + 1] == 0xB7)
            return i;
    }
    return restlen;
}

int kvspaceShmList(kvspace_t *kv, const char *prefix, bool ex, int resolve,
                   char ***on, int32_t *oc) {
    if (!kv || !prefix || !on || !oc)
        return -1;
    *on = NULL;
    *oc = 0;
    if (kv_sync(kv) != 0 || bad_dir_prefix(prefix))
        return -1;
    const char *pfx = prefix;
    char tbuf[1024];
    if (resolve) {
        resolve_path(kv, prefix, tbuf, sizeof(tbuf));
        pfx = tbuf;
    }
    /* memindex（p·）：读 index body 成员名（唯一权威）；stringkeymap 按坐标
     * row-major 升序。读到 0 个名字时回落到 ART scan（空 memindex 不是「没有子项」）。 */
    if (read_index_names(kv, pfx, on, oc) && *oc > 0)
        return 0;
    int plen = (int)strlen(pfx);
    char **out = malloc(sizeof(char *) * 4096);
    int32_t n = 0;
    char buf[2048];
    memset(buf, 0, sizeof(buf));
    if (kv->hdr->art_root < 0)
        return 0;
    art_scan_pfx(kv, pfx, plen, buf, (int)sizeof(buf), &out, &n);
    // filter: only direct children (one level below prefix)
    char **filt = malloc(sizeof(char *) * n);
    int32_t fn = 0;
    for (int i = 0; i < n; i++) {
        const char *k = out[i];
        int kl = (int)strlen(k);
        if (kl <= plen)
            continue;
        // extract the name segment immediately after prefix
        const char *rest = k + plen;
        int nlen = child_name_len(rest, kl - plen);
        if (nlen == 0)
            continue;
        // dedup
        bool dup = false;
        for (int j = 0; j < fn; j++)
            if (strncmp(filt[j], rest, nlen) == 0 && filt[j][nlen] == '\0') {
                dup = true;
                break;
            }
        if (!dup) {
            filt[fn] = strndup(rest, nlen);
            fn++;
        }
    }
    for (int i = 0; i < n; i++)
        free(out[i]);
    free(out);

    /* extindex 展开：ex 且 prefix 是 extindex → 追加 extpath 的直接子项。 */
    if (ex) {
        char extpath[1024];
        char *d = edir(pfx);
        if (dir_ext_path(kv, d, extpath, sizeof extpath)) {
            char **eo = malloc(sizeof(char *) * 4096);
            int32_t en = 0;
            char ebuf[2048];
            memset(ebuf, 0, sizeof ebuf);
            int el = (int)strlen(extpath);
            art_scan_pfx(kv, extpath, el, ebuf, (int)sizeof ebuf, &eo, &en);
            filt = realloc(filt, sizeof(char *) * (size_t)(fn + en));
            for (int i = 0; i < en; i++) {
                const char *k = eo[i];
                int kl = (int)strlen(k);
                if (kl <= el)
                    continue;
                const char *rest = k + el;
                int nlen = child_name_len(rest, kl - el);
                if (nlen == 0)
                    continue;
                bool dup = false;
                for (int j = 0; j < fn; j++)
                    if (strncmp(filt[j], rest, (size_t)nlen) == 0 &&
                        filt[j][nlen] == '\0') {
                        dup = true;
                        break;
                    }
                if (!dup) {
                    filt[fn] = strndup(rest, (size_t)nlen);
                    fn++;
                }
            }
            for (int i = 0; i < en; i++)
                free(eo[i]);
            free(eo);
        }
        free(d);
    }

    *on = filt;
    *oc = fn;
    return 0;
}

int kvspaceShmExtindex(kvspace_t *kv, const char *p, const char *ep) {
    uint8_t *v;
    int32_t vl = kvspaceXvalueNewExtindex(ep, NULL, 0, &v);
    int r = kvspaceShmSet(kv, p, v, vl);
    free(v);
    return r;
}
int kvspaceShmDelextindex(kvspace_t *kv, const char *p) {
    return kvspaceShmDel(kv, p);
}

/* ============ Watch/Notify ============ */
static int wslot(const char *k) {
    uint32_t h = 5381;
    for (const char *p = k; *p; p++)
        h = ((h << 5) + h) + (uint8_t)*p;
    return (int)(h % WATCH_TABLE_SZ);
}
int kvspaceShmNotify(kvspace_t *kv, const char *k, const uint8_t *v,
                     int32_t vl) {
    if (!kv || !k)
        return -1;
    int s = wslot(k);
    pthread_mutex_lock(&kv->wlock);
    watch_t *w = &kv->watches[s];
    if (w->key[0] && strcmp(w->key, k) == 0) {
        pthread_mutex_lock(&w->mtx);
        free(w->val);
        w->val = v ? memcpy(malloc(vl), v, vl) : NULL;
        w->val_len = vl;
        w->ready = true;
        pthread_cond_signal(&w->cond);
        pthread_mutex_unlock(&w->mtx);
    }
    pthread_mutex_unlock(&kv->wlock);
    return 0;
}
uint8_t *kvspaceShmWatch(kvspace_t *kv, const char *k, int32_t to,
                         int32_t *ol) {
    if (!kv || !k || !ol)
        return NULL;
    *ol = 0;
    int s = wslot(k);
    pthread_mutex_lock(&kv->wlock);
    watch_t *w = &kv->watches[s];
    strncpy(w->key, k, 255);
    w->key[255] = '\0';
    w->ready = false;
    free(w->val);
    w->val = NULL;
    pthread_mutex_unlock(&kv->wlock);
    pthread_mutex_lock(&w->mtx);
    if (!w->ready) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += to / 1000;
        ts.tv_nsec += (to % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&w->cond, &w->mtx, &ts);
    }
    uint8_t *r = NULL;
    if (w->ready && w->val) {
        r = w->val;
        *ol = w->val_len;
        w->val = NULL;
    }
    w->ready = false;
    pthread_mutex_unlock(&w->mtx);
    return r;
}
