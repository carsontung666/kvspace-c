/*
 * kvspace.c — KVSpace SHM 存储引擎: ART 树 + slotsboxmalloc
 */

#define _GNU_SOURCE
/* header-only 模式：两个依赖库的实现体在本 TU 编译，不再链接它们的 .so。
 * blockmalloc 必须先于 slotsboxmalloc（后者在其实现体里调用前者）。 */
#define BLOCKMALLOC_IMPLEMENTATION
#include <blockmalloc/blockmalloc.h>
#define SBO_MIN_SHARD_BYTES (1ULL << 40)
#define SLOTSBOXMALLOC_IMPLEMENTATION
#include "slotsboxmalloc/slotsboxobj.h"
#include "kvspace_shm.h"
#include "xvalue_head.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KVS_MAGIC "kvspace-c.v5"
#define ART_PREFIX_MAX 10
/* ART block = one N4/N16 node; N48/N256 take a run of consecutive blocks.
   The pool only appends (nodes are never freed), so runs are bump-allocated. */
#define ART_BLOCK_SZ 104
#define ART_SLAB_INIT (64UL * 1024 * 1024)
/* blocks_init picks 30-bit ids only if the initial pool holds > 8191 blocks;
   a smaller pool would be capped at 16384 blocks forever. */
#define SBO_HEAD_POOL_INIT (4UL * 1024 * 1024)
/* Reserved VA per region: grow in place, base never moves.
   data grows x64 per step; 2^40 fits 8 * 64^6 = 512GB. */
#define REGION_RESERVE (1ULL << 38)
/* ART ids are 30-bit: reserve <= 2^30 blocks. */
#define ART_RESERVE (1ULL << 36)
#define DATA_RESERVE (1ULL << 40)
/* Values >= 64KB are level-3+ objects (32KB aligned): punch pages on delete. */
#define SBO_PUNCH_MIN (64UL * 1024)
#define WATCH_TABLE_SZ 256

static int reserved_meta_path(const char *key) {
    static const char root[] = "/.kvspace-meta";
    return strncmp(key, root, sizeof root - 1) == 0 &&
           (key[sizeof root - 1] == 0 || key[sizeof root - 1] == '/');
}

enum { ART_N4 = 0,
       ART_N16 = 1,
       ART_N48 = 2,
       ART_N256 = 3,
       ART_MOVED = 255 };

/* v5：{has_value, box_offset} 合并成单个 8B 对齐原子字，禁止裸位域访问。
 * val = bit0 has_value | bit1..63 box_offset（MOVED 节点用 box_offset 存新 id）。 */
typedef struct {
    uint8_t type, prefix[ART_PREFIX_MAX], prefix_len;
    uint16_t count;
    uint16_t _pad;
    uint64_t val;
} art_hdr_t;

#define AV_HAS(v) ((int)((v) & 1u))
#define AV_OFF(v) ((uint64_t)((v) >> 1))
#define AV_MAKE(has, off) ((((uint64_t)(off)) << 1) | ((has) ? 1ull : 0ull))
static inline uint64_t art_val_load(const art_hdr_t *h) {
    return __atomic_load_n(&h->val, __ATOMIC_ACQUIRE);
}
static inline void art_val_store(art_hdr_t *h, uint64_t v) {
    __atomic_store_n(&h->val, v, __ATOMIC_RELEASE);
}
static inline bool art_val_cas(art_hdr_t *h, uint64_t *exp, uint64_t nv) {
    return __atomic_compare_exchange_n(&h->val, exp, nv, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

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

_Static_assert(sizeof(art_n4_t) <= ART_BLOCK_SZ && sizeof(art_n16_t) <= ART_BLOCK_SZ,
               "N4/N16 must fit one ART block");

static int art_node_sz(int t) {
    return t == ART_N4    ? sizeof(art_n4_t)
           : t == ART_N16 ? sizeof(art_n16_t)
           : t == ART_N48 ? sizeof(art_n48_t)
                          : sizeof(art_n256_t);
}

/* 进程注册表槽：pid + starttime 双重判活（pid 复用），recovery_ack = 已确认的代。 */
#define P2_SLOT_N 64
/* CLAIMING：已 CAS 占位、pid/starttime 尚未写完；扫描者视为存活，避免认领竞态误回收。 */
enum { P2_SLOT_FREE = 0, P2_SLOT_MINE = 1, P2_SLOT_CLAIMING = 3 };
typedef struct {
    uint32_t pid;
    uint32_t state;
    uint64_t starttime;
    uint64_t recovery_ack;
    uint32_t quiesced;   /* Q2 声明（第 3 步接） */
    uint32_t claim_id;   /* open 时领取，1..(1<<30)-1 */
} p2_slot_t;

/* 统一垃圾环：每条 16B，off 恒为真实 box 偏移，状态编码在 meta。
 * meta = state:2 | claim_id:30（EMPTY=0；RESERVED/COMMITTED 带 claim）。 */
#define GARB_CAP (1u << 21)              /* 2,097,152 条 = 32MiB VA */
#define GARB_THRESHOLD (64ull << 20)     /* 触发一轮的垃圾字节 */
#define P2_BARRIER_MS_DEFAULT 200
#define GARB_MAX_CLAIM ((1u << 30) - 1u)
typedef struct {
    uint64_t off;
    uint32_t epoch;
    uint32_t meta;
} garb_ent_t;
enum { GE_EMPTY = 0, GE_RESERVED = 1, GE_COMMITTED = 2 };
#define GE_STATE(m) ((uint32_t)((m) & 3u))
#define GE_CLAIM(m) ((uint32_t)((m) >> 2))
#define GE_MAKE(st, id) ((((uint32_t)(id)) << 2) | (uint32_t)(st))

/* <path>           [kvspace_hdr_t][blocks_meta_t][ART slab ...]
 * <path>.sbo.head  [sbo meta ...]
 * <path>.sbo.data  [sbo data ...] */
typedef struct {
    char magic[12];
    int32_t pending_moved_old; /* Crash journal: node replaced in flight (-1 if none) */
    uint64_t art_slab_size, sbo_head_size, sbo_data_size;
    int32_t art_root;
    int32_t pending_moved_to;  /* Crash journal: clone/grown replacement id (-1 if none) */
    /* v3: 旁路 gen 表（节点 id 索引，8B/节点）。 */
    uint64_t gens_size;
    uint64_t gen_epoch;        /* 节点分配 epoch（原子递增） */
    uint32_t format_version;   /* = KVSPACE_FORMAT_VERSION */
    uint32_t recovery_epoch;   /* 屏障轮次（u32，模比较） */
    uint32_t ack_seq;          /* 跨进程 futex 字（第 3 步） */
    uint32_t barrier_state;
    uint64_t pending_moved_par; /* Crash journal: (uint64_t)(uint32_t)parent_id | ((uint64_t)byte << 32), or UINT64_MAX for root */
    uint64_t pending_unused;
    uint64_t garb_cap, garb_head, garb_tail;
    uint64_t committed_bytes;  /* 只由写者改（单写者契约，普通 add） */
    uint64_t freed_bytes;      /* 只由 leader 改 */
    uint64_t garb_freeing;
    uint64_t round_ok, round_aborted, leak_boxes;
    /* v6: Key-set generation. Incremented (release) whenever a key appears (insert/revive)
     * or disappears (delete/tombstone) to invalidate per-handle List caches across processes. */
    uint64_t list_gen;
    uint32_t claim_seq;        /* 30 位发号；耗尽 open 硬失败 */
    uint32_t write_lock;       /* Cross-process writer spinlock (PID-owned, crash-recoverable) */
    p2_slot_t slots[P2_SLOT_N];
} kvspace_hdr_t;

_Static_assert(sizeof(kvspace_hdr_t) == 2232, "kvspace_hdr_t size must stay 2232 bytes");
_Static_assert(offsetof(kvspace_hdr_t, art_slab_size) == 16, "art_slab_size offset unchanged");
_Static_assert(offsetof(kvspace_hdr_t, gens_size) == 48, "gens_size offset unchanged");
_Static_assert(offsetof(kvspace_hdr_t, garb_cap) == 96, "garb_cap offset unchanged");
_Static_assert(offsetof(kvspace_hdr_t, write_lock) == 180, "write_lock offset unchanged");

#define ART_OFF (sizeof(kvspace_hdr_t) + sizeof(blocks_meta_t))

/* blocks_init fixes the block-head width (30-bit ids) from the initial size. */
_Static_assert(ART_SLAB_INIT / (ART_BLOCK_SZ + 2) > 32767ULL / 4,
               "ART_SLAB_INIT too small: blockmalloc would pick 14-bit ids");
_Static_assert(ART_RESERVE / (ART_BLOCK_SZ + 8) <= (1ULL << 30),
               "ART_RESERVE too large for 30-bit block ids");
_Static_assert((ART_BLOCK_SZ + 8) % 8 == 0, "ART stride must keep 8B alignment");
_Static_assert(
    SBO_HEAD_POOL_INIT / (sizeof(sbo_box_t) + 2) > 32767ULL / 4,
    "SBO_HEAD_POOL_INIT too small: blockmalloc would pick 14-bit ids");

/* Head layout with root_slots == 1 (checked at open):
   [sbo_meta_t][blocks_meta_t][sbo_lock_t][pool]. Public structs only. */
#define SBO_HEAD_FIXED                                                         \
    (sizeof(sbo_meta_t) + sizeof(blocks_meta_t) + sizeof(sbo_lock_t))
static blocks_meta_t *sbo_pool(sbo_meta_t *m) {
    return sbo_pool_meta(m, 0);
}
static sbo_lock_t *sbo_plock(sbo_meta_t *m) {
    return sbo_pool_lock(m, 0);
}
static uint8_t *sbo_pmem(sbo_meta_t *m) {
    return sbo_pool_mem(m, 0);
}
static sbo_box_t *sbo_box(sbo_meta_t *m, int32_t id) {
    return (sbo_box_t *)(sbo_pmem(m) + (size_t)id * m->block_stride +
                         m->box_off);
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
    shm_region_t r_art, r_head, r_data, r_gen, r_garb;
    garb_ent_t *garb;             /* .garb 映射（GARB_CAP 条） */
    uint64_t gen_n;               /* gens_at 容量：本进程已映射的节点数 */
    uint64_t art_total_seen;      /* 上次同步时的 ART slab total_size */
    kvspace_hdr_t *hdr;
    uint64_t store_id;   /* 进程内句柄身份（P2 token 用） */
    int p2_slot;         /* 注册表槽；-1 = 未注册 */
    uint32_t pid;        /* open 时的 pid（冷路径判 fork） */
    uint32_t fork_gen;   /* fork 世代：子进程里全局 +1 → 热路径守卫 */
    uint32_t quiesced;   /* Q2 进程内镜像；claim 时 = 1 */
    pthread_t maint_thread;
    uint32_t maint_seq;  /* 进程内私有 futex 字 */
    uint32_t maint_started, maint_stop;  /* started: 0 未建, 1 运行, 2 创建失败 */
    uint32_t reclaim_unavailable;        /* 线程创建失败：环 3/4 满后写硬失败 */
    blocks_meta_t *art_meta;
    uint8_t *art_data;
    sbo_meta_t *sbo_meta;
    uint8_t *sbo_data;
    watch_t watches[WATCH_TABLE_SZ];
    pthread_mutex_t wlock;
    /* Per-handle List cache (see list_cached): ListLen + indexed ListAt scans only once.
     * Hits when (prefix, ex, resolve), resolved target, extindex mount, and hdr->list_gen
     * all match, producing identical results to a fresh scan. */
    pthread_mutex_t lc_mtx;
    char *lc_prefix;
    char *lc_target;    /* resolved target directory (equals prefix when resolve=0) */
    char *lc_ext;       /* extindex mount source when ex=1; "" if unmounted */
    char **lc_names;
    int32_t lc_count;
    uint8_t lc_ex, lc_res;
    uint64_t lc_gen;
};
/* Bump after key-set changes (insert/revive/delete) to invalidate List caches across processes.
 * Ptr/@ext value changes do not bump this since list_cached re-checks target and mount each call. */
static inline void list_gen_bump(kvspace_t *kv) {
    __atomic_add_fetch(&kv->hdr->list_gen, 1ull, __ATOMIC_RELEASE);
}

static void kv_repair_pending_move(kvspace_t *kv);

static inline void kv_write_lock(kvspace_t *kv) {
    uint32_t me = (uint32_t)sbo_self_pid();
    for (;;) {
        uint32_t e = 0;
        if (__atomic_compare_exchange_n(&kv->hdr->write_lock, &e, me, true,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return;
        for (int spin = 0; spin < 4096; spin++) {
            sbo_cpu_relax();
            if (__atomic_load_n(&kv->hdr->write_lock, __ATOMIC_RELAXED) == 0)
                break;
        }
        uint32_t owner = __atomic_load_n(&kv->hdr->write_lock, __ATOMIC_ACQUIRE);
        if (owner > 0 && owner != me && sbo_pid_dead((int)owner)) {
            if (__atomic_compare_exchange_n(&kv->hdr->write_lock, &owner, me,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_RELAXED)) {
                kv_repair_pending_move(kv);
                return;
            }
        }
    }
}

static inline void kv_write_unlock(kvspace_t *kv) {
    __atomic_store_n(&kv->hdr->write_lock, 0u, __ATOMIC_RELEASE);
}

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

/* ── P2: 旁路 gen 表（node id → {struct_gen, value_gen}）──────────── */
static void kv_sbo_free(kvspace_t *kv, uint64_t off);
static art_hdr_t *art_hdr(kvspace_t *kv, int32_t id);
static int32_t art_child(kvspace_t *kv, void *n, uint8_t b);
static int pfx_shared(const uint8_t *a, int al, const uint8_t *b, int bl);
static int32_t art_walk(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                        int d, int32_t *par, int *pard);
static int32_t art_follow(kvspace_t *kv, int32_t id);
static int32_t ref_leaf(kvspace_t *kv, kvspaceRef_t *ref, const char *key);

/* 每节点 8B：{struct_gen, value_gen}（v2.2 删 pin 后不再有 refcount 位）。 */
typedef struct {
    uint32_t struct_gen;
    uint32_t value_gen;
} kv_gens_t;

static uint64_t gen_cap(const kvspace_t *kv) {
    blocks_meta_t *m = kv->art_meta;
    uint64_t head = (uint64_t)m->sizeof_block_head;
    return m->total_size / (head + m->block_size);
}

static int32_t art_id_of(kvspace_t *kv, const void *p) {
    if (!p || !kv->art_data)
        return -1;
    return (int32_t)blockid_bydataoffset(kv->art_meta,
                                         (uint64_t)((const uint8_t *)p - kv->art_data));
}

static kv_gens_t *gens_at(kvspace_t *kv, int32_t id) {
    if (id < 0 || !kv->r_gen.base)
        return NULL;
    if ((uint64_t)id >= kv->gen_n)   /* gen_n 由 gens_grow / kv_sync 维护 */
        return NULL;
    return (kv_gens_t *)kv->r_gen.base + id;
}

static void gen_bump_struct(kvspace_t *kv, int32_t id) {
    kv_gens_t *g = gens_at(kv, id);
    if (g)
        __atomic_add_fetch(&g->struct_gen, 1u, __ATOMIC_ACQ_REL);
}

/* value_gen 静止态恒为偶数：就地写前后各 +1（奇=写中），整体替换/删除 +2。 */
static void gen_bump_value(kvspace_t *kv, int32_t id) {
    kv_gens_t *g = gens_at(kv, id);
    if (g)
        __atomic_add_fetch(&g->value_gen, 2u, __ATOMIC_ACQ_REL);
}

static int64_t gen_alloc_epoch(kvspace_t *kv) {
    return (int64_t)__atomic_add_fetch(&kv->hdr->gen_epoch, 1ull, __ATOMIC_ACQ_REL);
}

static int p2_maint_start(kvspace_t *kv);   /* 懒启动（阈值触发） */
static int p2_round(kvspace_t *kv);
static void p2_futex_wake_priv(uint32_t *addr);
static bool p2_claim_alive(kvspace_t *kv, uint32_t claim);
static int kv_sync(kvspace_t *kv);
static void list_cache_drop(kvspace_t *kv);

/* 阈值：生产固定 64MiB；测试构建可用 P2_TEST_GARB_THRESHOLD_MB 调低。 */
static uint64_t p2_garb_threshold(void) {
#ifdef P2_TEST_HOOKS
    const char *v = getenv("P2_TEST_GARB_THRESHOLD_MB");
    if (v) {
        unsigned long mb = strtoul(v, NULL, 10);
        if (mb)
            return (uint64_t)mb << 20;
    }
#endif
    return GARB_THRESHOLD;
}

/* ── 统一垃圾环：第 2 步只记不回收（drain 在第 3 步接）─────────────
 * reserve 必须发生在任何键值/节点变更之前；失败 = 写路径硬错误，无半写。
 * 每个 reserve 之后必须且只能走 commit 或 cancel。 */
static void garb_full_report(kvspace_t *kv) {
    static bool once;
    if (once)
        return;
    once = true;
    uint64_t h = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
    uint64_t t = __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE);
    uint32_t m = __atomic_load_n(&kv->garb[h & (GARB_CAP - 1)].meta,
                                 __ATOMIC_ACQUIRE);
    fprintf(stderr,
            "kvspace: garbage ring full (cap=%llu, head=%llu, tail=%llu, "
            "bytes=%llu): head entry #%llu state=%u claim=%u is holding "
            "the drain (no reclaim round yet)\n",
            (unsigned long long)kv->hdr->garb_cap, (unsigned long long)h,
            (unsigned long long)t,
            (unsigned long long)(kv->hdr->committed_bytes -
                                 kv->hdr->freed_bytes),
            (unsigned long long)h, GE_STATE(m), GE_CLAIM(m));
}

static int garb_reserve(kvspace_t *kv, uint64_t *idx) {
    uint32_t claim = kv->p2_slot >= 0 ? kv->hdr->slots[kv->p2_slot].claim_id : 0;
    if (!claim)
        return -1;
    uint64_t h = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
    uint64_t t = __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE);
    if (t - h >= kv->hdr->garb_cap - 1024 && kv->quiesced) {
        (void)p2_round(kv);
        h = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
        t = __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE);
    }
    if (t - h >= kv->hdr->garb_cap) {
        garb_full_report(kv);
        return -1;
    }
    if (kv->reclaim_unavailable &&
        kv->hdr->garb_cap - (t - h) < kv->hdr->garb_cap / 4) {
        garb_full_report(kv);        /* 线程建不起来：1/4 安全线硬失败 */
        return -1;
    }
    /* 单写者契约（kvspace-c #58）：entry/tail 只有写者写，普通 release store。
     * leader 只读；唯一的写者进程若死在 store 与 tail 之间，槽位停在 RESERVED
     * 且未发布（i == tail），下一个写者在这里回收死 claim（冷路径）。
     * 活 claim 占着 tail 槽 = 契约被违反 → 硬错误，不猜。 */
    garb_ent_t *e = &kv->garb[t & (GARB_CAP - 1)];
    uint32_t m = __atomic_load_n(&e->meta, __ATOMIC_ACQUIRE);
    if (m != GE_EMPTY) {
        if (GE_STATE(m) != GE_RESERVED || p2_claim_alive(kv, GE_CLAIM(m)))
            return -1;
        __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&e->meta, GE_MAKE(GE_RESERVED, claim), __ATOMIC_RELEASE);
    __atomic_store_n(&kv->hdr->garb_tail, t + 1, __ATOMIC_RELEASE);
    *idx = t;
    return 0;
}

/* bytes 由退役点在“不拿分配器锁”的前提下给出（ViewDel 用 token 缓存的 cap，
 * realloc 用已算出的 cap）；sbo_allocated_size 会取 sbo plock，不能进热路径。 */
static void garb_commit(kvspace_t *kv, uint64_t idx, uint64_t off,
                        uint64_t bytes_retired) {
    garb_ent_t *e = &kv->garb[idx & (GARB_CAP - 1)];
    uint32_t claim = GE_CLAIM(__atomic_load_n(&e->meta, __ATOMIC_ACQUIRE));
    e->off = off;
    e->epoch = __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE);
    __atomic_store_n(&e->meta, GE_MAKE(GE_COMMITTED, claim), __ATOMIC_RELEASE);
    uint64_t bytes = __atomic_add_fetch(&kv->hdr->committed_bytes,
                                        bytes_retired, __ATOMIC_RELEASE);
    uint64_t h = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
    uint64_t t = __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE);
    if (bytes - __atomic_load_n(&kv->hdr->freed_bytes, __ATOMIC_ACQUIRE) >=
            p2_garb_threshold() ||
        t - h >= kv->hdr->garb_cap / 4) {
        (void)p2_maint_start(kv);    /* 失败已置 reclaim_unavailable */
        __atomic_add_fetch(&kv->maint_seq, 1u, __ATOMIC_RELEASE);
        p2_futex_wake_priv(&kv->maint_seq);
    }
}

static void garb_cancel(kvspace_t *kv, uint64_t idx) {
    __atomic_store_n(&kv->garb[idx & (GARB_CAP - 1)].meta, GE_EMPTY,
                     __ATOMIC_RELEASE);
}

/* 精确 key 的节点 id（即使 has_value==0 也返回；用于墓碑复活时清 DEAD）。 */
static int32_t p2_node_at(kvspace_t *kv, const char *key) {
    int klen = (int)strlen(key);
    int d = 0;
    int32_t nid = kv->hdr->art_root;
    while (nid >= 0) {
        art_hdr_t *h = art_hdr(kv, nid);
        if (!h || h->type == ART_MOVED)
            return -1;
        if (h->prefix_len) {
            int s = pfx_shared(h->prefix, h->prefix_len,
                               (const uint8_t *)key + d, klen - d);
            if (s != h->prefix_len && d + s != klen)
                return -1;
            d += s;
        }
        /* 键可能正好结束在 prefix_len==0 的分支节点上（如 k1 落在根 N4 的子叶），
         * 此时 d==klen，必须在此返回；否则会越界读 key[klen]。 */
        if (d == klen)
            return nid;
        nid = art_child(kv, h, (uint8_t)key[d]);
        d++;
    }
    return -1;
}

/* ── P2: 进程注册表 + 恢复屏障 ────────────────────────────────── */
/* /proc/<pid>/stat 第 22 字段 starttime（pid 复用判据）。macOS 无 /proc。
 * 返回 0=读到；1=进程不存在（ENOENT，判死）；2=不可判定（保守视为存活）。 */
static int p2_proc_starttime(uint32_t pid, uint64_t *out) {
#if defined(__linux__)
    char p[64];
    if (snprintf(p, sizeof p, "/proc/%u/stat", pid) >= (int)sizeof p)
        return 2;
    FILE *f = fopen(p, "r");
    if (!f)
        return errno == ENOENT ? 1 : 2;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (!n)
        return 2;
    buf[n] = 0;
    char *rp = strrchr(buf, ')');   /* comm 可含空格，字段从最后一个 ')' 起数 */
    if (!rp)
        return 2;
    char *s = rp + 1;
    int field = 3;                  /* ')' 之后第一个字段是 stat 第 3 字段 */
    while (*s) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        if (field == 3 && (*s == 'Z' || *s == 'X'))
            return 1;                   /* 僵尸/已退出：判死（否则会永久占位） */
        if (field == 22) {
            *out = strtoull(s, NULL, 10);
            return 0;
        }
        while (*s && *s != ' ')
            s++;
        field++;
    }
    return 2;
#else
    (void)pid;
    (void)out;
    return 2;                       /* 平台无 liveness：永不判死 */
#endif
}

static bool p2_slot_alive(const p2_slot_t *s) {
    uint64_t st = 0;
    uint32_t state = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    if (state == P2_SLOT_FREE)
        return false;
    if (state == P2_SLOT_CLAIMING)
        return true;
    int rc = p2_proc_starttime(s->pid, &st);
    if (rc == 1)
        return false;               /* ENOENT：进程已死 */
    if (rc != 0)
        return true;                /* 不可判定 → 保守视为存活 */
    return st == s->starttime;
}

/* open：先回收死槽，再 CAS 认领；仍满才失败（反复崩溃不耗光槽）。 */
static int p2_reg_claim(kvspace_t *kv) {
    uint64_t st = 0;
    if (p2_proc_starttime((uint32_t)getpid(), &st) != 0)
        st = 0;
    uint32_t me = (uint32_t)getpid();
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        uint32_t v = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
        if (v == P2_SLOT_CLAIMING && s->pid == 0) {
            /* 认领者死在 CAS 与写 pid 之间：pid 仍为 0，可回收 */
            __atomic_compare_exchange_n(&s->state, &v, P2_SLOT_FREE, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            continue;
        }
        if (v != P2_SLOT_FREE && !p2_slot_alive(s))
            __atomic_compare_exchange_n(&s->state, &v, P2_SLOT_FREE, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        uint32_t expect = P2_SLOT_FREE;
        if (!__atomic_compare_exchange_n(&s->state, &expect, P2_SLOT_CLAIMING, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        uint32_t cid = __atomic_add_fetch(&kv->hdr->claim_seq, 1u,
                                          __ATOMIC_ACQ_REL);
        if (cid > GARB_MAX_CLAIM) {          /* 不静默回绕 */
            __atomic_store_n(&s->state, P2_SLOT_FREE, __ATOMIC_RELEASE);
            return -3;
        }
        s->pid = me;
        s->starttime = st;
        s->quiesced = 1;   /* 新进程还没有 token：天然静默；runtime 用 view 前显式 Quiesce(0) */
        s->claim_id = cid;
        __atomic_store_n(&s->recovery_ack,
                         __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE),
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s->state, P2_SLOT_MINE, __ATOMIC_RELEASE);
        return i;
    }
    return -2;                      /* 占满：调用方拒绝 open，不静默扩容 */
}

static void p2_reg_release(kvspace_t *kv) {
    if (kv->p2_slot < 0)
        return;
    if (kv->pid != (uint32_t)getpid()) {   /* fork 子进程：句柄无效，不动父进程的槽 */
        kv->p2_slot = -1;
        return;
    }
    p2_slot_t *s = &kv->hdr->slots[kv->p2_slot];
    uint32_t expect = P2_SLOT_MINE;
    __atomic_compare_exchange_n(&s->state, &expect, P2_SLOT_FREE, false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    kv->p2_slot = -1;
}

/* 每次 kvspaceView* 入口：屏障代变化 → 记本地代并 ack（token 由 _pad 校验失效）。 */
static void p2_ack_slot(kvspace_t *kv, uint32_t ep) {
    if (kv->p2_slot < 0)
        return;
    __atomic_store_n(&kv->hdr->slots[kv->p2_slot].recovery_ack, ep,
                     __ATOMIC_RELEASE);
    __atomic_add_fetch(&kv->hdr->ack_seq, 1u, __ATOMIC_RELEASE);
#if defined(__linux__)
    /* 没有 leader 在等就不要进内核：热路径每 64 条指令一次 ack，
     * 无条件 FUTEX_WAKE 是每次写系统调用。漏唤醒最坏退化为 5ms 分片。 */
    if (__atomic_load_n(&kv->hdr->barrier_state, __ATOMIC_ACQUIRE))
        syscall(SYS_futex, &kv->hdr->ack_seq, FUTEX_WAKE, INT_MAX, NULL, NULL,
                0);
#endif
}

/* 表随 ART slab 增长（容量按当前 block 配置重算）。 */
#define GEN_RESERVE (8ULL << 30)    /* 8GiB VA: one gens entry per ART block id */
static int gens_grow(kvspace_t *kv) {
    uint64_t want = gen_cap(kv) * sizeof(kv_gens_t);
    if (want > GEN_RESERVE)
        return -1;
    kv->gen_n = want / sizeof(kv_gens_t);
    kv->art_total_seen = kv->art_meta->total_size;
    if (want <= kv->r_gen.mapped)
        return 0;
    if (!kv->r_gen.base) {
        void *p = mmap(NULL, GEN_RESERVE, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED)
            return -1;
        kv->r_gen.base = p;
        kv->r_gen.reserve = GEN_RESERVE;
    }
    if (ftruncate(kv->r_gen.fd, (off_t)want) != 0)
        return -1;
    if (region_map(&kv->r_gen, (size_t)want) != 0)
        return -1;
    kv->hdr->gens_size = want;
    return 0;
}

/* ── P2: 版本化 view token ─────────────────────────────────────── */
/* fork 检测不能每 op 调 getpid()（本机 glibc 不缓存，一次 ~150ns）。
 * 用 pthread_atfork 的子进程钩子把全局世代 +1；热路径只比两个 u32。 */
static uint32_t g_fork_gen;
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;
static void p2_atfork_child(void) {
    __atomic_add_fetch(&g_fork_gen, 1u, __ATOMIC_ACQ_REL);
}
static void p2_atfork_install(void) {
    (void)pthread_atfork(NULL, NULL, p2_atfork_child);
}

#define P2_FORK_GUARD(kv)                                                     \
    ((kv)->fork_gen != __atomic_load_n(&g_fork_gen, __ATOMIC_ACQUIRE))
#define P2_VIEW_GUARD(kv) ((kv)->quiesced || P2_FORK_GUARD(kv))

/* 固定头直读辅助：校验 value 是合法 8 字节头 + body 长度自洽；产出头。 */
static int head_of(const uint8_t *val, int32_t val_len, kvspaceXvalueHead *out) {
    if (!val || val_len < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    memcpy(out, val, sizeof *out);
    uint64_t l = kvspaceXvalueHeadLen(out);
    if (out->storetype > KVSPACE_XVALUE_ST_EXT ||
        out->ptr > KVSPACE_XVALUE_PTR_EXT)
        return -1;
    if ((uint64_t)KVSPACE_XVALUE_HEAD_LEN + l > (uint64_t)val_len)
        return -1;
    return 0;
}

/* Write validation: head_of + complete attr region (attr_len >= 2 and within val_len).
 * read_value derives total length from 8+len+attr_len without storing val_len, so
 * values with an incomplete attr region would be unreadable if accepted. */
static int value_ok(const uint8_t *val, int32_t val_len,
                    kvspaceXvalueHead *out) {
    if (head_of(val, val_len, out) != 0)
        return -1;
    uint64_t l = kvspaceXvalueHeadLen(out);
    if ((uint64_t)val_len < KVSPACE_XVALUE_HEAD_LEN + l + 2)
        return -1;
    uint64_t alen = kvspaceXvalueHeadAttrLen(val, l);
    if (alen < 2 || alen > (uint64_t)val_len - KVSPACE_XVALUE_HEAD_LEN - l)
        return -1;
    return 0;
}

int kvspaceShmViewFromRef(kvspace_t *kv, const kvspaceRef_t *ref,
                           const char *key_fallback, kvspaceViewToken_t *tok) {
    (void)key_fallback;
    if (!kv || !ref || !tok || ref->gen != 0 || P2_VIEW_GUARD(kv))
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    int32_t id = art_follow(kv, (int32_t)ref->block_id);
    art_hdr_t *h = id >= 0 ? art_hdr(kv, id) : NULL;
    if (!h)
        return -1;
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return -1;
    uint64_t cap = sbo_allocated_size(kv->sbo_meta, AV_OFF(hv));
    if (cap < 1)
        return -1;
    uint8_t *raw = kv->sbo_data + AV_OFF(hv);
    kvspaceXvalueHead w;
    if (head_of(raw, (int32_t)cap, &w) != 0 ||
        w.ptr != KVSPACE_XVALUE_PTR_INLINE)
        return -1;
    kv_gens_t *g = gens_at(kv, id);
    if (!g)
        return -1;
    uint32_t gen = __atomic_load_n(&g->struct_gen, __ATOMIC_ACQUIRE);
    tok->head = raw;
    tok->body = raw + KVSPACE_XVALUE_HEAD_LEN;
    tok->headlen = KVSPACE_XVALUE_HEAD_LEN;
    tok->value_len = (uint32_t)kvspaceXvalueHeadLen(&w);
    tok->cap = cap;
    tok->body_cap = cap - KVSPACE_XVALUE_HEAD_LEN;
    tok->struct_gen = gen;
    tok->value_gen = __atomic_load_n(&g->value_gen, __ATOMIC_ACQUIRE);
    tok->store_id = kv->store_id;
    tok->node_id = (uint32_t)id;
    tok->_pad = __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE);
    return 0;
}

int kvspaceShmViewResolve(kvspace_t *kv, const char *key, kvspaceViewToken_t *tok) {
    kvspaceRef_t ref;
    if (!kv || !key || !tok || P2_VIEW_GUARD(kv) ||
        kvspaceShmResolveRef(kv, key, &ref) != 0)
        return -1;
    return kvspaceShmViewFromRef(kv, &ref, key, tok);
}

/* 前缀解析：path 的最后一个字节落在哪个节点；depth = 该节点压缩前缀被 path 覆盖的字节数
 * （可能 < prefix_len，即边界落在前缀内部）。用于 short 键（整键就是一个节点的前缀）。 */
int kvspaceShmResolvePrefix(kvspace_t *kv, const char *path, kvspaceRef_t *ref) {
    if (!kv || !path || !ref || P2_VIEW_GUARD(kv))
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    memset(ref, 0, sizeof(*ref));
    int klen = (int)strlen(path);
    int d = 0;
    int32_t nid = kv->hdr->art_root;
    while (nid >= 0) {
        art_hdr_t *h = art_hdr(kv, nid);
        if (!h || h->type == ART_MOVED)
            return -1;
        int s = 0;
        if (h->prefix_len) {
            s = pfx_shared(h->prefix, h->prefix_len,
                           (const uint8_t *)path + d, klen - d);
            if (s != h->prefix_len && d + s != klen)
                return -1;
            d += s;
        }
        /* path may end at a node with prefix_len == 0 (e.g. "/vthread/1/" next to
         * "/vthread/10/"); return here before reading path[klen]. */
        if (d == klen) {
            ref->block_id = (uint32_t)nid;
            ref->depth = (uint32_t)s;       /* bytes of this node's prefix covered by path */
            return 0;
        }
        nid = art_child(kv, h, (uint8_t)path[d]);
        d++;
    }
    return -1;
}

/* 前缀形态：parent_ref 的 block_id/depth 指向目录节点；只走 rel。 */
int kvspaceShmViewResolveFrom(kvspace_t *kv, const kvspaceRef_t *parent,
                              const char *rel, kvspaceViewToken_t *tok) {
    if (!kv || !parent || !rel || !tok || P2_VIEW_GUARD(kv))
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    int32_t pid = art_follow(kv, (int32_t)parent->block_id);
    art_hdr_t *ph = pid >= 0 ? art_hdr(kv, pid) : NULL;
    if (!ph)
        return -1;
    /* 边界落在节点压缩前缀内部时，先把该节点前缀的剩余字节与 rel 对掉。 */
    const uint8_t *p = (const uint8_t *)rel;
    int rem = (int)strlen(rel);
    int32_t covered = (int32_t)parent->depth;
    if (covered < 0 || covered > (int32_t)ph->prefix_len)
        return -1;
    while (covered < (int32_t)ph->prefix_len && rem > 0) {
        if (p[0] != ph->prefix[covered])
            return -1;
        p++; rem--; covered++;
    }
    int32_t id;
    if (rem == 0) {
        id = pid;                        /* 空 rel = 前缀自身 */
    } else {
        int32_t cid = art_child(kv, ph, p[0]);
        id = art_walk(kv, cid, p, rem, 1, NULL, NULL);
    }
    if (id < 0)
        return -1;
    kvspaceRef_t leaf = {0};
    leaf.block_id = (uint32_t)id;
    leaf.gen = 0;
    return kvspaceShmViewFromRef(kv, &leaf, rel, tok);
}

int kvspaceShmViewCheck(kvspace_t *kv, const kvspaceViewToken_t *tok) {
    if (!kv || !tok || P2_VIEW_GUARD(kv))
        return -1;
    if (tok->store_id != kv->store_id)
        return 1;
    if (tok->_pad != __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE))
        return 1;
    kv_gens_t *g = gens_at(kv, (int32_t)tok->node_id);
    if (!g)
        return 1;
    if (__atomic_load_n(&g->struct_gen, __ATOMIC_ACQUIRE) != tok->struct_gen)
        return 1;
    art_hdr_t *h = art_hdr(kv, (int32_t)tok->node_id);
    if (!h)
        return 1;
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return 1;
    if (tok->head != kv->sbo_data + AV_OFF(hv))
        return 1;
    return 0;
}

static inline bool shm_write_body_atomic(uint8_t *dst, const uint8_t *src,
                                         uint32_t len) {
    if (len == 0)
        return true;
    if (len == 1) {
        __atomic_store_n(dst, src[0], __ATOMIC_RELEASE);
        return true;
    }
    if (len == 2 && (((uintptr_t)dst & 1u) == 0)) {
        uint16_t v;
        memcpy(&v, src, 2);
        __atomic_store_n((uint16_t *)dst, v, __ATOMIC_RELEASE);
        return true;
    }
    if (len == 4 && (((uintptr_t)dst & 3u) == 0)) {
        uint32_t v;
        memcpy(&v, src, 4);
        __atomic_store_n((uint32_t *)dst, v, __ATOMIC_RELEASE);
        return true;
    }
    if (len == 8 && (((uintptr_t)dst & 7u) == 0)) {
        uint64_t v;
        memcpy(&v, src, 8);
        __atomic_store_n((uint64_t *)dst, v, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

static inline bool shm_can_write_atomic(const uint8_t *dst, uint32_t len) {
    return len <= 1 ||
           (len == 2 && (((uintptr_t)dst & 1u) == 0)) ||
           (len == 4 && (((uintptr_t)dst & 3u) == 0)) ||
           (len == 8 && (((uintptr_t)dst & 7u) == 0));
}

/* Hot-path write: in-place same-length scalar body write. Returns 0 on success, 1 to fall back.
 * Only single-instruction aligned 1/2/4/8-byte writes are done in place; wider writes fall back
 * to CoW SetValue so SIGKILL cannot tear multi-word payloads. */
int kvspaceShmViewWrite(kvspace_t *kv, kvspaceViewToken_t *tok,
                        const uint8_t *data, uint32_t len) {
    if (!kv || !tok || (len && !data) || P2_VIEW_GUARD(kv))
        return -1;
    if (!shm_can_write_atomic(tok->body, len))
        return 1;
    kv_write_lock(kv);
    if (kvspaceShmViewCheck(kv, tok) != 0) {
        kv_write_unlock(kv);
        return 1;
    }
    kvspaceXvalueHead cur;
    memcpy(&cur, tok->head, sizeof cur);
    if ((uint64_t)len != kvspaceXvalueHeadLen(&cur) || len > tok->body_cap) {
        kv_write_unlock(kv);
        return 1;
    }
    kv_gens_t *g = gens_at(kv, (int32_t)tok->node_id);
    if (!g) {
        kv_write_unlock(kv);
        return 1;
    }
    (void)shm_write_body_atomic(tok->body, data, len);
    __atomic_add_fetch(&g->value_gen, 2u, __ATOMIC_ACQ_REL);
    tok->value_len = len;
    kv_write_unlock(kv);
    return 0;
}

/* 热路径删：tombstone + value_gen++（box 释放走延迟队列）。 */
int kvspaceShmViewDel(kvspace_t *kv, kvspaceViewToken_t *tok, const char *key) {
    if (!kv || P2_VIEW_GUARD(kv))
        return -1;
    if (kvspaceShmViewCheck(kv, tok) != 0)
        return 1;
    kv_gens_t *g = gens_at(kv, (int32_t)tok->node_id);
    art_hdr_t *h = art_hdr(kv, (int32_t)tok->node_id);
    if (!g || !h)
        return 1;
    /* 叶子级逻辑墓碑：CAS 一次原子清零 has_value+box；不 bump struct_gen。 */
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return 1;
    kv_write_lock(kv);
    hv = art_val_load(h);
    if (!AV_HAS(hv)) {
        kv_write_unlock(kv);
        return 1;
    }
    uint64_t gidx;
    if (garb_reserve(kv, &gidx) != 0) {
        kv_write_unlock(kv);
        return -1;                      /* 表满：硬错误，未变更 */
    }
#ifdef P2_TEST_HOOKS
    {
        const char *v = getenv("P2_TEST_HOLD_DELCAS_MS");
        if (v && atoi(v) > 0)
            usleep((useconds_t)atoi(v) * 1000);
    }
#endif
    if (!art_val_cas(h, &hv, 0)) {
        garb_cancel(kv, gidx);
        kv_write_unlock(kv);
        return 1;
    }
    __atomic_add_fetch(&g->value_gen, 2u, __ATOMIC_ACQ_REL); /* 静止态保持偶数 */
    garb_commit(kv, gidx, AV_OFF(hv), tok->cap);
    kv_write_unlock(kv);
    list_gen_bump(kv);                  /* bump after key removal */
    (void)key;
    return 0;
}

/* ── 第 3 步：屏障轮 / 维护线程 / Q1-Q2 ─────────────────────────── */

static long long p2_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int p2_barrier_ms(void) {
    const char *v = getenv("P2_BARRIER_MS");
    int ms = v ? atoi(v) : P2_BARRIER_MS_DEFAULT;
    return ms < 1 ? 1 : (ms > 5000 ? 5000 : ms);
}

#if defined(__linux__)
static void p2_futex_wait(uint32_t *addr, uint32_t val, int ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    syscall(SYS_futex, addr, FUTEX_WAIT, val, &ts, NULL, 0);
}
static void p2_futex_wake(uint32_t *addr) {
    syscall(SYS_futex, addr, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}
static void p2_futex_wait_priv(uint32_t *addr, uint32_t val, int ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, &ts, NULL, 0);
}
static void p2_futex_wake_priv(uint32_t *addr) {
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
}
#else
static void p2_futex_wait(uint32_t *a, uint32_t v, int ms) { (void)a; (void)v; usleep((useconds_t)ms * 1000); }
static void p2_futex_wake(uint32_t *a) { (void)a; }
static void p2_futex_wait_priv(uint32_t *a, uint32_t v, int ms) { (void)a; (void)v; usleep((useconds_t)ms * 1000); }
static void p2_futex_wake_priv(uint32_t *a) { (void)a; }
#endif

/* 选举字 = state:2 | claim_id:30（与环条目同一编码），单词 CAS，无 CLAIMING 过渡态。 */
enum { P2_BARRIER_FREE = 0, P2_BARRIER_OWNED = 1 };

static uint32_t p2_my_claim(kvspace_t *kv) {
    return kv->p2_slot >= 0 ? kv->hdr->slots[kv->p2_slot].claim_id : 0;
}

static bool p2_claim_alive(kvspace_t *kv, uint32_t claim) {
    if (!claim)
        return false;
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != P2_SLOT_MINE)
            continue;
        if (__atomic_load_n(&s->claim_id, __ATOMIC_ACQUIRE) != claim)
            continue;
        return p2_slot_alive(s);
    }
    return false;
}

static bool p2_elect(kvspace_t *kv) {
    uint32_t mine = p2_my_claim(kv);
    if (!mine)
        return false;
    uint32_t own = GE_MAKE(P2_BARRIER_OWNED, mine);
    for (;;) {
        uint32_t cur = __atomic_load_n(&kv->hdr->barrier_state, __ATOMIC_ACQUIRE);
        if (cur == GE_MAKE(P2_BARRIER_FREE, 0)) {
            if (__atomic_compare_exchange_n(&kv->hdr->barrier_state, &cur, own,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
                return true;
            continue;
        }
        if (GE_STATE(cur) != P2_BARRIER_OWNED)
            return false;                 /* 非法状态：不猜 */
        if (GE_CLAIM(cur) == mine)
            return false;                 /* 本进程已有 leader 线程在跑 */
        if (p2_claim_alive(kv, GE_CLAIM(cur)))
            return false;                 /* 活着的 owner：跳过，不等待 */
        if (__atomic_compare_exchange_n(&kv->hdr->barrier_state, &cur, own, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;                  /* 死者接管 */
    }
}

static void p2_release_busy(kvspace_t *kv) {
    uint32_t mine = GE_MAKE(P2_BARRIER_OWNED, p2_my_claim(kv));
    __atomic_compare_exchange_n(&kv->hdr->barrier_state, &mine,
                                GE_MAKE(P2_BARRIER_FREE, 0), false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* 每槽满足：state != MINE || recovery_ack == r || quiesced。
 * 只在未满足且未判死的槽上读 /proc；判死结果本轮缓存。 */
static bool p2_wait_acks(kvspace_t *kv, uint32_t r, int deadline_ms) {
    uint8_t dead[P2_SLOT_N] = {0};
    long long t0 = p2_now_ms();
    for (;;) {
        uint32_t seq = __atomic_load_n(&kv->hdr->ack_seq, __ATOMIC_ACQUIRE);
        bool missing = false;
        int first = -1;
        for (int i = 0; i < P2_SLOT_N; i++) {
            p2_slot_t *s = &kv->hdr->slots[i];
            if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != P2_SLOT_MINE)
                continue;
            if (__atomic_load_n(&s->recovery_ack, __ATOMIC_ACQUIRE) == r)
                continue;
            if (__atomic_load_n(&s->quiesced, __ATOMIC_ACQUIRE))
                continue;
            if (!dead[i]) {
                uint64_t ts = 0;
                int rc = p2_proc_starttime(s->pid, &ts);
                if (rc == 1 || (rc == 0 && ts != s->starttime))
                    dead[i] = 1;
            }
            if (dead[i])
                continue;
            missing = true;
            if (first < 0)
                first = i;
        }
        if (!missing)
            return true;
        long long left = (long long)deadline_ms - (p2_now_ms() - t0);
        if (left <= 0 || __atomic_load_n(&kv->maint_stop, __ATOMIC_ACQUIRE)) {
            if (first >= 0) {
                p2_slot_t *s = &kv->hdr->slots[first];
                fprintf(stderr,
                        "kvspace: barrier round %u abandoned (waited %dms): "
                        "slot %d pid=%u start=%llu ack=%llu quiesced=%u\n",
                        r, deadline_ms, first, s->pid,
                        (unsigned long long)s->starttime,
                        (unsigned long long)__atomic_load_n(&s->recovery_ack,
                                                            __ATOMIC_ACQUIRE),
                        __atomic_load_n(&s->quiesced, __ATOMIC_ACQUIRE));
            }
            return false;
        }
        p2_futex_wait(&kv->hdr->ack_seq, seq, (int)(left < 5 ? left : 5));
    }
}

/* 严格 FIFO：EMPTY 推进；RESERVED 活 claim → 停，死 claim → 取消+审计；
 * COMMITTED 且 epoch < r 才释放（cap 在 free 之前取）。 */
static int p2_drain(kvspace_t *kv, uint32_t r) {
    int freed = 0;
    if (kv_sync(kv) != 0)
        return 0;
    uint64_t i = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
    for (;;) {
        /* 环尾之外是未写条目：到 tail 必须停，否则会把 head 推过 tail
         * （tail-head 下溢后 reserve 永久失败）。 */
        if (i >= __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE))
            goto out;
        garb_ent_t *e = &kv->garb[i & (GARB_CAP - 1)];
        uint32_t m = __atomic_load_n(&e->meta, __ATOMIC_ACQUIRE);
        uint32_t st = GE_STATE(m);
        if (st == GE_EMPTY) {
            __atomic_store_n(&kv->hdr->garb_head, i + 1, __ATOMIC_RELEASE);
            i++;
            continue;
        }
        if (st == GE_RESERVED) {
            if (p2_claim_alive(kv, GE_CLAIM(m)))
                goto out;
            __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
            __atomic_add_fetch(&kv->hdr->leak_boxes, 1, __ATOMIC_ACQ_REL);
            fprintf(stderr,
                    "kvspace: garbage entry %llu reserved by dead claim %u: "
                    "leaked\n", (unsigned long long)i, GE_CLAIM(m));
            __atomic_store_n(&kv->hdr->garb_head, i + 1, __ATOMIC_RELEASE);
            i++;
            continue;
        }
        if (st != GE_COMMITTED)
            goto out;                 /* 非法状态：停，不猜 */
        if (!((int32_t)(r - __atomic_load_n(&e->epoch, __ATOMIC_ACQUIRE)) > 0))
            goto out;                 /* 未到轮次 */
        uint64_t off = __atomic_load_n(&e->off, __ATOMIC_ACQUIRE);
        if (off >= kv->r_data.mapped && kv_sync(kv) != 0)
            goto out;
        uint64_t cap = sbo_allocated_size(kv->sbo_meta, off);  /* free 之前取 */
        __atomic_store_n(&kv->hdr->garb_freeing, off, __ATOMIC_RELEASE);
        __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
        __atomic_store_n(&kv->hdr->garb_head, i + 1, __ATOMIC_RELEASE);
#ifdef P2_TEST_HOOKS
        {   /* 崩溃窗口：head 已推进、box 未 free —— 只漏不双 free */
            const char *v = getenv("P2_TEST_HOLD_DRAIN_MS");
            if (v && atoi(v) > 0)
                usleep((useconds_t)atoi(v) * 1000);
        }
#endif
        kv_sbo_free(kv, off);
        __atomic_store_n(&kv->hdr->freed_bytes,
                         kv->hdr->freed_bytes + cap, __ATOMIC_RELEASE);
        i++;
        freed++;
    }
out:
    __atomic_store_n(&kv->hdr->garb_freeing, 0, __ATOMIC_RELEASE);
    return freed;
}

/* 一轮：选举 → 清崩溃标记 → 推进 epoch → 等 ack → drain → 回收死槽。
 * 返回释放条数；-1 = 别人持有；-2 = 超时放弃（零 free）。 */
static int p2_round(kvspace_t *kv) {
#if !defined(__linux__)
    (void)kv;
    return 0;                             /* 无 /proc：不推进、不回收 */
#else
    if (!kv || kv->hdr->art_root < 0)
        return 0;
    if (kv->pid != (uint32_t)getpid())
        return -1;                        /* fork 子进程：句柄无效 */
    if (!p2_elect(kv))
        return -1;
    /* 别的进程可能扩过 ART/SBO/gen：drain 前必须把本进程映射同步到当前尺寸，
     * 否则 sbo_allocated_size / kv_sbo_free 会越界访问。 */
    if (kv_sync(kv) != 0) {
        p2_release_busy(kv);
        return -1;
    }
    uint64_t pending = __atomic_exchange_n(&kv->hdr->garb_freeing, 0,
                                           __ATOMIC_ACQ_REL);
    if (pending) {
        __atomic_add_fetch(&kv->hdr->leak_boxes, 1, __ATOMIC_ACQ_REL);
        fprintf(stderr, "kvspace: previous leader died freeing box %llu: "
                        "leaked\n", (unsigned long long)pending);
    }
    uint32_t r = __atomic_add_fetch(&kv->hdr->recovery_epoch, 1u,
                                    __ATOMIC_ACQ_REL);
    if (!p2_wait_acks(kv, r, p2_barrier_ms())) {
        __atomic_add_fetch(&kv->hdr->round_aborted, 1, __ATOMIC_ACQ_REL);
        p2_release_busy(kv);
        return -2;
    }
    int freed = p2_drain(kv, r);
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        uint32_t v = P2_SLOT_MINE;
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != P2_SLOT_MINE)
            continue;
        if (p2_slot_alive(s))
            continue;
        __atomic_compare_exchange_n(&s->state, &v, P2_SLOT_FREE, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    __atomic_add_fetch(&kv->hdr->round_ok, 1, __ATOMIC_ACQ_REL);
    p2_release_busy(kv);
    return freed;
#endif
}

static void *p2_maint(void *arg) {
    kvspace_t *kv = arg;
    while (!__atomic_load_n(&kv->maint_stop, __ATOMIC_ACQUIRE)) {
        uint32_t seq = __atomic_load_n(&kv->maint_seq, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&kv->maint_stop, __ATOMIC_ACQUIRE))
            break;
        p2_futex_wait_priv(&kv->maint_seq, seq, 50);
        if (__atomic_load_n(&kv->maint_stop, __ATOMIC_ACQUIRE))
            break;
        uint64_t h = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
        uint64_t t = __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE);
        if (kv->hdr->committed_bytes -
                    __atomic_load_n(&kv->hdr->freed_bytes, __ATOMIC_ACQUIRE) <
                p2_garb_threshold() &&
            t - h < kv->hdr->garb_cap / 4)
            continue;
        p2_round(kv);                 /* 内含选举；忙/超时都直接返回 */
    }
    return NULL;
}

/* 懒启动：open 不建线程；未回收垃圾越过 64MiB 时才建（open / commit 后检查）。
 * 每个进程最多一个；创建失败置 reclaim_unavailable 并由写路径在环 3/4 满时硬失败。 */
static int p2_maint_start(kvspace_t *kv) {
#if !defined(__linux__)
    (void)kv;
    return 0;                        /* macOS：不回收，无线程 */
#else
    uint32_t st = 0;
    if (!__atomic_compare_exchange_n(&kv->maint_started, &st, 1u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return st == 2u ? -1 : 0;    /* 已启动 / 已知失败 */
    if (kv->pid != (uint32_t)getpid()) {
        __atomic_store_n(&kv->maint_started, 0u, __ATOMIC_RELEASE);
        return 0;                    /* fork 子进程不建 */
    }
    if (pthread_create(&kv->maint_thread, NULL, p2_maint, kv) != 0) {
        __atomic_store_n(&kv->maint_started, 2u, __ATOMIC_RELEASE);
        kv->reclaim_unavailable = 1;
        fprintf(stderr, "kvspace: maintenance thread create failed; reclaim "
                        "unavailable (writes hard-fail when the ring is 3/4 "
                        "full)\n");
        return -1;
    }
    return 0;
#endif
}

static void p2_maint_stop(kvspace_t *kv) {
    if (kv->pid != (uint32_t)getpid()) {   /* fork 子进程里没有该线程 */
        __atomic_store_n(&kv->maint_started, 0u, __ATOMIC_RELEASE);
        return;
    }
    if (__atomic_load_n(&kv->maint_started, __ATOMIC_ACQUIRE) != 1u)
        return;                            /* 未启动 / 创建失败 */
    __atomic_store_n(&kv->maint_stop, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&kv->maint_seq, 1u, __ATOMIC_RELEASE);
    p2_futex_wake_priv(&kv->maint_seq);
    pthread_join(kv->maint_thread, NULL);
    __atomic_store_n(&kv->maint_started, 0u, __ATOMIC_RELEASE);
}

/* Q1：op 边界 ack。0 = 已 ack；1 = epoch 变了（先作废 token 再来）；-1 = 非法。 */
int kvspaceShmViewAck(kvspace_t *kv, uint32_t *epoch) {
    if (!kv || !epoch || kv->p2_slot < 0 || P2_FORK_GUARD(kv))
        return -1;
    uint32_t cur = __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE);
    if (*epoch != cur) {
        *epoch = cur;
        return 1;
    }
    p2_ack_slot(kv, cur);
    return 0;
}

/* Q2：显式静默声明。on 只接受 0/1；幂等；状态变化唤醒 leader。
 * 前置：调用线程已作废本进程全部 view token。声明期间 View* 一律 -1。 */
int kvspaceShmViewQuiesce(kvspace_t *kv, int on) {
    if (!kv || (on != 0 && on != 1) || kv->p2_slot < 0 || P2_FORK_GUARD(kv))
        return -1;
    p2_slot_t *s = &kv->hdr->slots[kv->p2_slot];
    if (__atomic_load_n(&s->quiesced, __ATOMIC_ACQUIRE) != (uint32_t)on) {
        __atomic_store_n(&s->quiesced, (uint32_t)on, __ATOMIC_RELEASE);
        __atomic_add_fetch(&kv->hdr->ack_seq, 1u, __ATOMIC_RELEASE);
        if (__atomic_load_n(&kv->hdr->barrier_state, __ATOMIC_ACQUIRE))
            p2_futex_wake(&kv->hdr->ack_seq);
    }
    kv->quiesced = (uint32_t)on;
    return 0;
}

#ifdef P2_TEST_HOOKS
void kvspaceShmViewRoundStats(kvspace_t *kv, uint64_t *ok, uint64_t *aborted,
                              uint64_t *leaks) {
    *ok = kv ? __atomic_load_n(&kv->hdr->round_ok, __ATOMIC_ACQUIRE) : 0;
    *aborted = kv ? __atomic_load_n(&kv->hdr->round_aborted, __ATOMIC_ACQUIRE) : 0;
    *leaks = kv ? __atomic_load_n(&kv->hdr->leak_boxes, __ATOMIC_ACQUIRE) : 0;
}

uint64_t kvspaceShmViewGarbBytes(kvspace_t *kv) {
    return kv && kv->hdr
               ? kv->hdr->committed_bytes -
                     __atomic_load_n(&kv->hdr->freed_bytes, __ATOMIC_ACQUIRE)
               : 0;
}

/* 对齐验收：块头 8、stride 8 倍数，且每个块的 val（偏移 16）不跨 64B 行。
 * 返回 -1 = 通过；>=0 = 第一个坏块 id；-2 = 块头宽度不是 8。 */
int64_t kvspaceShmArtAlignCheck(kvspace_t *kv) {
    if (!kv || !kv->art_meta)
        return -2;
    blocks_meta_t *m = kv->art_meta;
    if (m->sizeof_block_head != 8)
        return -2;
    uint64_t stride = (uint64_t)m->sizeof_block_head + m->block_size;
    if (stride % 8)
        return -2;
    uint64_t n = m->total_size / stride;
    for (uint64_t id = 0; id < n; id++) {
        uint64_t off = blockdata_offset(m, id);
        uint64_t val = off + offsetof(art_hdr_t, val);
        if (off % 8 || val % 8 || (val & 63) > 56)
            return (int64_t)id;
    }
    return -1;
}

/* 旧 store 拒绝测试：把 hdr 的 format_version 改成指定值（只测试用）。 */
void kvspaceShmTestSetFormatVersion(kvspace_t *kv, uint32_t v) {
    if (kv && kv->hdr)
        kv->hdr->format_version = v;
}
#endif

/* 同步强制一轮：返回释放条数；-1 = 别人持有；-2 = 超时放弃（零 free）。 */
int kvspaceShmViewSweep(kvspace_t *kv) {
    if (!kv || kv->hdr->art_root < 0)
        return 0;
    return p2_round(kv);
}

int kvspaceShmViewRefresh(kvspace_t *kv, kvspaceViewToken_t *tok, const char *key) {
    kvspaceViewToken_t t;
    if (kvspaceShmViewResolve(kv, key, &t) != 0)
        return -1;
    *tok = t;
    return 0;
}

static void kv_reconcile_sizes(kvspace_t *kv) {
    struct stat st;
    uint64_t art_tot = __atomic_load_n(&kv->art_meta->total_size, __ATOMIC_ACQUIRE);
    uint64_t hdr_art = __atomic_load_n(&kv->hdr->art_slab_size, __ATOMIC_ACQUIRE);
    if (art_tot > hdr_art &&
        fstat(kv->r_art.fd, &st) == 0 &&
        (uint64_t)st.st_size >= ART_OFF + art_tot) {
        __atomic_store_n(&kv->hdr->art_slab_size, art_tot, __ATOMIC_RELEASE);
    }
    sbo_meta_t *m = kv->sbo_meta;
    if (m->root_slots == 1) {
        blocks_meta_t *p = sbo_pool(m);
        uint64_t ptot = __atomic_load_n(&p->total_size, __ATOMIC_ACQUIRE);
        uint64_t psm = __atomic_load_n(&m->per_slot_meta, __ATOMIC_ACQUIRE);
        uint64_t pm = ptot > psm ? ptot : psm;
        uint64_t hdr_head = __atomic_load_n(&kv->hdr->sbo_head_size, __ATOMIC_ACQUIRE);
        if (SBO_HEAD_FIXED + pm > hdr_head &&
            fstat(kv->r_head.fd, &st) == 0 &&
            (uint64_t)st.st_size >= SBO_HEAD_FIXED + pm) {
            __atomic_store_n(&m->per_slot_meta, pm, __ATOMIC_RELEASE);
            __atomic_store_n(&m->max_blocks, (uint32_t)(pm / m->block_stride),
                             __ATOMIC_RELEASE);
            __atomic_store_n(&kv->hdr->sbo_head_size, SBO_HEAD_FIXED + pm,
                             __ATOMIC_RELEASE);
        }
        uint8_t cur_shift = __atomic_load_n(&m->slot_shift, __ATOMIC_ACQUIRE);
        uint64_t mdata0 = __atomic_load_n(&m->data_size, __ATOMIC_ACQUIRE);
        uint8_t want_lvl =
            (uint8_t)(sbo_align_to((1ULL << cur_shift) >> 3).level - 1);
        sbo_box_t *root = sbo_box(m, 0);
        uint32_t gnid = __atomic_load_n(&sbo_plock(m)->grow_nid, __ATOMIC_ACQUIRE);
        uint8_t root_lvl = __atomic_load_n(&root->objlevel, __ATOMIC_ACQUIRE);
        if ((gnid != 0 || root_lvl == (uint8_t)(want_lvl + 1)) &&
            cur_shift + SBO_N_BITS < 64) {
            uint64_t want_data = 1ULL << (cur_shift + SBO_N_BITS);
            if (fstat(kv->r_data.fd, &st) == 0 &&
                (uint64_t)st.st_size >= want_data) {
                sbo_lock_shard(m, 0);
                sbo_unlock(sbo_plock(m));
            }
        } else if (cur_shift >= 9 && cur_shift < 64 &&
                   (mdata0 >> cur_shift) != 1) {
            uint64_t want_data = 1ULL << cur_shift;
            if (fstat(kv->r_data.fd, &st) == 0 &&
                (uint64_t)st.st_size >= want_data) {
                sbo_lock_shard(m, 0);
                sbo_unlock(sbo_plock(m));
            }
        }
        uint64_t mdata = __atomic_load_n(&m->data_size, __ATOMIC_ACQUIRE);
        uint64_t hdr_data = __atomic_load_n(&kv->hdr->sbo_data_size, __ATOMIC_ACQUIRE);
        if (mdata > hdr_data &&
            fstat(kv->r_data.fd, &st) == 0 &&
            (uint64_t)st.st_size >= mdata) {
            __atomic_store_n(&kv->hdr->sbo_data_size, mdata, __ATOMIC_RELEASE);
        }
    }
}

static int kv_sync(kvspace_t *kv) {
    kv_reconcile_sizes(kv);
    size_t art_sz = (size_t)__atomic_load_n(&kv->hdr->art_slab_size, __ATOMIC_ACQUIRE);
    if (region_map(&kv->r_art, ART_OFF + art_sz) != 0)
        return -1;
    /* Grow local gens mapping when another process expanded the ART slab. */
    if (__atomic_load_n(&kv->art_meta->total_size, __ATOMIC_ACQUIRE) !=
            kv->art_total_seen &&
        gens_grow(kv) != 0)
        return -1;
    size_t head_sz = (size_t)__atomic_load_n(&kv->hdr->sbo_head_size, __ATOMIC_ACQUIRE);
    if (region_map(&kv->r_head, head_sz) != 0)
        return -1;
    size_t data_sz = (size_t)__atomic_load_n(&kv->hdr->sbo_data_size, __ATOMIC_ACQUIRE);
    return region_map(&kv->r_data, data_sz);
}

/* blockmalloc only checks total_size; grow the file before publishing it. */
static int art_slab_grow(kvspace_t *kv) {
    size_t want = (size_t)__atomic_load_n(&kv->hdr->art_slab_size, __ATOMIC_ACQUIRE) * 2;
    if (ART_OFF + want > kv->r_art.reserve ||
        ftruncate(kv->r_art.fd, (off_t)(ART_OFF + want)) != 0 ||
        region_map(&kv->r_art, ART_OFF + want) != 0)
        return -1;
    __atomic_store_n(&kv->art_meta->total_size, want, __ATOMIC_RELEASE);
    __atomic_store_n(&kv->hdr->art_slab_size, want, __ATOMIC_RELEASE);
    return 0;
}

/* ---- sbo growth (docs/shm-resize.md). Both grow functions run under
   sbo_plock, which excludes sbo_alloc/free/allocated_size in every process. */

/* Free list empty and no room to append. Unlocked calls are a hint only. */
#ifdef BLOCKMALLOC_COUNT_MASK
#define BM_MALLOC_BLOCKS(m) blocks_malloc_count(m)
#define BM_HEAD_SIZE(m)     blocks_head_size(m)
#else
#define BM_MALLOC_BLOCKS(m) ((uint64_t)(m)->malloc_blocks)
#define BM_HEAD_SIZE(m)     ((uint64_t)(m)->sizeof_block_head)
#endif
static bool sbo_pool_full(sbo_meta_t *m) {
    blocks_meta_t *p = sbo_pool(m);
    if (p->free_next_id != -1)
        return false;
    uint64_t next_end = (uint64_t)block_offset(p, BM_MALLOC_BLOCKS(p)) +
                        BM_HEAD_SIZE(p) + p->block_size;
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
        want = (kv->r_head.reserve - SBO_HEAD_FIXED) & ~(uint64_t)7;
    if (want <= cur ||
        ftruncate(kv->r_head.fd, (off_t)(SBO_HEAD_FIXED + want)) != 0 ||
        region_map(&kv->r_head, SBO_HEAD_FIXED + (size_t)want) != 0)
        return -1;
    __atomic_store_n(&p->total_size, want, __ATOMIC_RELEASE);
    __atomic_store_n(&m->per_slot_meta, want, __ATOMIC_RELEASE);
    __atomic_store_n(&m->max_blocks, (uint32_t)(want / m->block_stride),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&kv->hdr->sbo_head_size, SBO_HEAD_FIXED + want,
                     __ATOMIC_RELEASE);
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
    b->parent_slot = 0;
    b->self_id = (int32_t)nid;
    uint64_t mm = root->child_mask;
    while (mm) {
        int i = __builtin_ctzll(mm);
        mm &= mm - 1;
        sbo_box(m, root->children[i])->parent = (int32_t)nid;
    }
    sbo_lock_t *lk = sbo_plock(m);
    uint8_t old_lvl = b->objlevel;
    if (old_lvl >= 1 && old_lvl <= 3 && lk->hot_id1[old_lvl - 1] == 1)
        lk->hot_id1[old_lvl - 1] = (uint32_t)nid + 1;
    /* Journal nid before mutating root so sbo_repair_shard can finish the split on crash. */
    __atomic_store_n(&lk->grow_nid, (uint32_t)nid, __ATOMIC_RELEASE);
    uint8_t lvl = (uint8_t)(old_lvl + 1);
    sbo_box_format(root, lvl, -1, 255, 0);
#ifdef P2_TEST_HOOKS
    if (getenv("P2_TEST_DIE_GROW_SPLIT"))
        _exit(99);
#endif
    root->children[0] = (int32_t)nid;
    root->states[0] = SBO_BOX;
    root->free_mask = ~1ULL;
    root->child_mask = 1ULL;
    uint16_t b_comb = sbo_combined(b);
    root->child_cap[0] = b_comb;
    root->child_max_enc = b_comb;
    root->child_max_count = 1;
    root->max_obj_cap = SBO_MULT_MAX;
    root->hint_child = 0;
    __atomic_store_n(&m->slot_shift, (uint8_t)__builtin_ctzll(want),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&m->data_size, want, __ATOMIC_RELEASE);
    __atomic_store_n(&kv->hdr->sbo_data_size, want, __ATOMIC_RELEASE);
    __atomic_store_n(&lk->grow_nid, 0u, __ATOMIC_RELEASE);
    return 0;
}

/* Pool exhaustion is exact. Data exhaustion is not (sbo_alloc also fails on
   trylock contention), so re-root only after two failures at one capacity. */
static int kv_sbo_grow(kvspace_t *kv, bool failed, uint64_t *seen_data) {
    sbo_meta_t *m = kv->sbo_meta;
    sbo_lock_shard(m, 0);
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
/* Bump-allocate k consecutive blocks; the node spans them (interior heads unused). */
/* blockmalloc packs sizeof_block_head:4 and malloc_blocks:60 into a single u64 word
 * between block_size and used_blocks. Lock-free readers (art_blk / art_id_of) read this
 * word concurrently, so writers holding meta->lock must update the whole word atomically
 * (via blocks_set_malloc_count when available, or whole-word atomic store fallback). */
_Static_assert(offsetof(blocks_meta_t, used_blocks) ==
                   offsetof(blocks_meta_t, block_size) + 2 * sizeof(uint64_t),
               "blockmalloc: sizeof_block_head/malloc_blocks must share one u64 word");
#ifdef BLOCKMALLOC_COUNT_MASK
static inline void bm_set_malloc_blocks(blocks_meta_t *m, uint64_t n) {
    blocks_set_malloc_count(m, n);
}
#else
#define BM_WORD_OFF (offsetof(blocks_meta_t, block_size) + sizeof(uint64_t))
static unsigned bm_count_shift(void) {
    blocks_meta_t t;
    memset(&t, 0, sizeof t);
    t.malloc_blocks = 1;
    uint64_t w;
    memcpy(&w, (const char *)&t + BM_WORD_OFF, sizeof w);
    return w ? (unsigned)__builtin_ctzll(w) : 64u;
}
/* Caller holds m->lock. */
static void bm_set_malloc_blocks(blocks_meta_t *m, uint64_t n) {
    uint64_t *w = (uint64_t *)((char *)m + BM_WORD_OFF);
    const unsigned sh = bm_count_shift();
    const uint64_t mask = ((1ULL << 60) - 1) << sh;
    uint64_t old = __atomic_load_n(w, __ATOMIC_RELAXED);
    __atomic_store_n(w, (old & ~mask) | ((n << sh) & mask), __ATOMIC_RELEASE);
}
#endif
/* Verify whole-word bitfield layout at open time. */
static bool bm_layout_ok(void) {
    blocks_meta_t t;
    memset(&t, 0, sizeof t);
    t.sizeof_block_head = 8;
    t.malloc_blocks = 5;
    t.used_blocks = 7;
    bm_set_malloc_blocks(&t, 0x0123456789ABCDEull);
    return t.sizeof_block_head == 8 && t.malloc_blocks == 0x0123456789ABCDEull &&
           t.block_size == 0 && t.used_blocks == 7;
}
static int64_t art_bump(kvspace_t *kv, uint32_t k) {
    blocks_meta_t *m = kv->art_meta;
    if (spin_lock_ex(&m->lock))
        m->used_blocks = BM_MALLOC_BLOCKS(m);
    uint64_t id = BM_MALLOC_BLOCKS(m);
    if ((uint64_t)block_offset(m, id + k) > m->total_size) {
        spin_unlock(&m->lock);
        return -1;
    }
    setblock_t(m, kv->art_data + block_offset(m, id), 1, -1);
    bm_set_malloc_blocks(m, id + k);
    m->used_blocks += k;
    spin_unlock(&m->lock);
    return (int64_t)id;
}
static int32_t art_balloc(kvspace_t *kv, int t) {
    const uint64_t head = BM_HEAD_SIZE(kv->art_meta);
    const uint64_t stride = head + kv->art_meta->block_size;
    const uint32_t k = (uint32_t)(((uint64_t)art_node_sz(t) +
                                   head + stride - 1) /
                                  stride);
    int64_t id = art_bump(kv, k);
    while (id < 0) {
        if (art_slab_grow(kv) != 0 || gens_grow(kv) != 0)
            return -1;
        id = art_bump(kv, k);
    }
    if (id >= 0) {
        kv_gens_t *g = gens_at(kv, (int32_t)id);
        if (g) {
            /* 新 id 的 gen 取全局 epoch，保证被复用的 id 不会误命中旧 token。 */
            __atomic_store_n(&g->struct_gen,
                             (uint32_t)gen_alloc_epoch(kv), __ATOMIC_RELEASE);
            __atomic_store_n(&g->value_gen, 0u, __ATOMIC_RELEASE);
        }
    }
    return (int32_t)id;
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
            /* s == prefix_len implies d + prefix_len <= klen */
            if (pfx_shared(h->prefix, h->prefix_len, key + d, klen - d) !=
                h->prefix_len)
                return -1;
            d += h->prefix_len;
        }
        if (d == klen)
            return AV_HAS(art_val_load(h)) ? nid : -1;
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
            return AV_HAS(art_val_load(h)) ? nid : -1;
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
        id = (int32_t)AV_OFF(art_val_load(h));
    }
    return -1;
}

/* ---- node create ---- */
static int32_t art_new_leaf(kvspace_t *kv, uint64_t off) {
    int32_t id = art_balloc(kv, ART_N4);
    if (id < 0)
        return -1;
    art_n4_t *x = art_blk(kv, id);
    memset(x, 0, sizeof(*x));
    x->h.type = ART_N4;
    art_val_store(&x->h, AV_MAKE(1, off));
    return id;
}
static int32_t art_new_node(kvspace_t *kv, int t) {
    int32_t id = art_balloc(kv, t);
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

static void art_mark_moved(kvspace_t *kv, int32_t old_id, int32_t new_id) {
    if (old_id < 0 || new_id < 0)
        return;
    art_hdr_t *oh = art_hdr(kv, old_id);
    if (!oh)
        return;
    gen_bump_struct(kv, new_id);
    gen_bump_struct(kv, old_id);
    art_val_store(oh, AV_MAKE(0, (uint32_t)new_id));
    __atomic_store_n(&oh->type, (uint8_t)ART_MOVED, __ATOMIC_RELEASE);
}

static inline void kv_journal_move_begin(kvspace_t *kv, int32_t old_id,
                                         int32_t new_id, int32_t parent_id,
                                         uint8_t edge) {
    if (old_id < 0 || new_id < 0)
        return;
    uint64_t par = parent_id < 0
                       ? UINT64_MAX
                       : ((uint64_t)(uint32_t)parent_id | ((uint64_t)edge << 32));
    __atomic_store_n(&kv->hdr->pending_moved_to, new_id, __ATOMIC_RELAXED);
    __atomic_store_n(&kv->hdr->pending_moved_par, par, __ATOMIC_RELAXED);
    __atomic_store_n(&kv->hdr->pending_moved_old, old_id, __ATOMIC_RELEASE);
}

static inline void kv_journal_move_end(kvspace_t *kv) {
    __atomic_store_n(&kv->hdr->pending_moved_old, -1, __ATOMIC_RELEASE);
}

/* Complete art_mark_moved if a dead writer crashed after swapping parent/root
 * pointer to the new subtree but before marking old_id as ART_MOVED. */
static void kv_repair_pending_move(kvspace_t *kv) {
    int32_t old_id = __atomic_load_n(&kv->hdr->pending_moved_old, __ATOMIC_ACQUIRE);
    if (old_id < 0)
        return;
    int32_t new_id = __atomic_load_n(&kv->hdr->pending_moved_to, __ATOMIC_RELAXED);
    uint64_t par = __atomic_load_n(&kv->hdr->pending_moved_par, __ATOMIC_RELAXED);
    if (kv_sync(kv) == 0 && new_id >= 0) {
        art_hdr_t *oh = art_hdr(kv, old_id);
        if (oh && oh->type != ART_MOVED) {
            bool unlinked = false;
            if (par == UINT64_MAX) {
                unlinked = (__atomic_load_n(&kv->hdr->art_root, __ATOMIC_ACQUIRE) != old_id);
            } else {
                int32_t pid = (int32_t)(uint32_t)par;
                uint8_t edge = (uint8_t)(par >> 32);
                art_hdr_t *ph = art_hdr(kv, pid);
                if (ph && ph->type != ART_MOVED)
                    unlinked = (art_child(kv, ph, edge) != old_id);
            }
            if (unlinked)
                art_mark_moved(kv, old_id, new_id);
        }
    }
    __atomic_store_n(&kv->hdr->pending_moved_old, -1, __ATOMIC_RELEASE);
}

static int32_t art_clone_node_shortened(kvspace_t *kv, int32_t oid, int drop_prefix) {
    art_hdr_t *oh = art_hdr(kv, oid);
    if (!oh || drop_prefix > (int)oh->prefix_len)
        return -1;
    int t = oh->type;
    int32_t cid = art_new_node(kv, t);
    if (cid < 0)
        return -1;
    oh = art_hdr(kv, oid);
    void *dst = art_blk(kv, cid);
    memcpy(dst, oh, (size_t)art_node_sz(t));
    art_hdr_t *ch = (art_hdr_t *)dst;
    uint8_t rem = (uint8_t)(oh->prefix_len - (uint8_t)drop_prefix);
    ch->prefix_len = rem;
    if (rem > 0)
        memmove(ch->prefix, oh->prefix + drop_prefix, rem);
    return cid;
}

static int art_replace_child(kvspace_t *kv, void *n, uint8_t b, int32_t nc) {
    gen_bump_struct(kv, art_id_of(kv, n));
    art_hdr_t *h = n;
    switch (h->type) {
    case ART_N4: {
        art_n4_t *x = n;
        for (int i = 0; i < (int)h->count; i++)
            if (x->keys[i] == b) {
                __atomic_store_n(&x->children[i], nc, __ATOMIC_RELEASE);
                return 0;
            }
        return -1;
    }
    case ART_N16: {
        art_n16_t *x = n;
        int lo = 0, hi = (int)h->count - 1;
        while (lo <= hi) {
            int m = (lo + hi) / 2;
            if (x->keys[m] == b) {
                __atomic_store_n(&x->children[m], nc, __ATOMIC_RELEASE);
                return 0;
            }
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
        if (idx == 255)
            return -1;
        __atomic_store_n(&x->children[idx], nc, __ATOMIC_RELEASE);
        return 0;
    }
    case ART_N256: {
        art_n256_t *x = n;
        __atomic_store_n(&x->children[b], nc, __ATOMIC_RELEASE);
        return 0;
    }
    }
    return -1;
}

/* ---- grow ---- */
static uint64_t art_n48_used_mask(const art_n48_t *x) {
    uint64_t used = 0;
    for (int i = 0; i < 256; i++) {
        uint8_t idx = __atomic_load_n(&x->index[i], __ATOMIC_RELAXED);
        if (idx < 48)
            used |= 1ULL << idx;
    }
    return used;
}

static int32_t art_grow(kvspace_t *kv, int32_t oid) {
    art_hdr_t *oh = art_hdr(kv, oid);
    if (!oh)
        return -1;
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
    oh = art_hdr(kv, oid);
    void *on = oh;
    art_hdr_t *nh = art_hdr(kv, nid);
    nh->prefix_len = oh->prefix_len;
    memcpy(nh->prefix, oh->prefix, oh->prefix_len);
    art_val_store(nh, art_val_load(oh));
    if (oh->type == ART_N48) {
        art_n48_t *x = on;
        art_n256_t *dst = (art_n256_t *)nh;
        uint16_t cnt = 0;
        for (int j = 0; j < 256; j++) {
            uint8_t idx = x->index[j];
            if (idx < 48) {
                dst->children[j] = x->children[idx];
                cnt++;
            }
        }
        nh->count = cnt;
        return nid;
    }
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
    return nid;
}

/* ---- add child ---- */
static int art_add(kvspace_t *kv, void *n, uint8_t b, int32_t cid) {
    gen_bump_struct(kv, art_id_of(kv, n)); /* P2: 子集变化 */
    art_hdr_t *h = n;
    switch (h->type) {
    case ART_N4: {
        if (h->count >= 4)
            return -1;
        art_n4_t *x = n;
        uint16_t c = h->count;
        x->children[c] = cid;
        x->keys[c] = b;
        __atomic_store_n(&h->count, (uint16_t)(c + 1), __ATOMIC_RELEASE);
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
        x->children[p] = cid;
        x->keys[p] = b;
        __atomic_store_n(&h->count, (uint16_t)(h->count + 1), __ATOMIC_RELEASE);
        return 0;
    }
    case ART_N48: {
        art_n48_t *x = n;
        if (x->index[b] < 48)
            return -1;
        uint64_t used = art_n48_used_mask(x);
        int cnt = __builtin_popcountll(used);
        if (cnt >= 48) {
            __atomic_store_n(&h->count, 48u, __ATOMIC_RELEASE);
            return -1;
        }
        int s = __builtin_ctzll(~used);
        x->children[s] = cid;
        __atomic_store_n(&x->index[b], (uint8_t)s, __ATOMIC_RELEASE);
        __atomic_store_n(&h->count, (uint16_t)(cnt + 1), __ATOMIC_RELEASE);
        return 0;
    }
    case ART_N256: {
        art_n256_t *x = n;
        if (x->children[b] >= 0)
            return -1;
        uint16_t cnt = 0;
        for (int i = 0; i < 256; i++)
            if (x->children[i] >= 0)
                cnt++;
        __atomic_store_n(&x->children[b], cid, __ATOMIC_RELEASE);
        __atomic_store_n(&h->count, (uint16_t)(cnt + 1), __ATOMIC_RELEASE);
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
        uint64_t used = art_n48_used_mask(x);
        int cnt = __builtin_popcountll(used);
        if (cnt > 0) {
            int last = 63 - __builtin_clzll(used);
            if (idx < last) {
                x->children[idx] = x->children[last];
                for (int j = 0; j < 256; j++)
                    if (x->index[j] == last) {
                        x->index[j] = idx;
                        break;
                    }
            }
        }
        h->count = (uint16_t)cnt;
        break;
    }
    case ART_N256: {
        art_n256_t *x = n;
        x->children[b] = -1;
        if (h->count > 0)
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
                       int d, uint64_t off, int32_t *moved_old, int32_t *moved_to) {
    *moved_old = -1;
    *moved_to = -1;
    if (nid < 0)
        return art_leaf_chain(kv, key, klen, d, off);

    art_hdr_t *h = art_hdr(kv, nid);
    if (!h)
        return -1;
    int shared = pfx_shared(h->prefix, h->prefix_len, key + d, klen - d);
    int mpl = h->prefix_len < (klen - d) ? h->prefix_len : (klen - d);

    if (shared == mpl && (klen - d) < h->prefix_len) {
        /* Prefix split where new key ends inside h->prefix: clone h untouched. */
        uint8_t ob = h->prefix[shared];
        int32_t copy_id = art_clone_node_shortened(kv, nid, shared + 1);
        if (copy_id < 0)
            return -1;
        int32_t nn = art_new_node(kv, ART_N4);
        if (nn < 0)
            return -1;
        art_hdr_t *nh = art_hdr(kv, nn);
        nh->prefix_len = (uint8_t)shared;
        if (shared > 0)
            memcpy(nh->prefix, key + d, (size_t)shared);
        art_add(kv, nh, ob, copy_id);
        art_val_store(nh, AV_MAKE(1, off));
        *moved_old = nid;
        *moved_to = copy_id;
        return nn;
    }
    if (shared < mpl) { // prefix split
        uint8_t ob = h->prefix[shared];
        int32_t copy_id = art_clone_node_shortened(kv, nid, shared + 1);
        if (copy_id < 0)
            return -1;
        int32_t nn = art_new_node(kv, ART_N4);
        if (nn < 0)
            return -1;
        int32_t leaf = art_leaf_chain(kv, key, klen, d + shared + 1, off);
        if (leaf < 0)
            return -1;
        art_hdr_t *nh = art_hdr(kv, nn);
        nh->prefix_len = (uint8_t)shared;
        if (shared > 0)
            memcpy(nh->prefix, key + d, (size_t)shared);
        art_add(kv, nh, ob, copy_id);
        art_add(kv, nh, key[d + shared], leaf);
        *moved_old = nid;
        *moved_to = copy_id;
        return nn;
    }
    d += h->prefix_len;
    if (d == klen) {
        art_val_store(h, AV_MAKE(1, off));
        return nid;
    }
    int32_t cid = art_child(kv, h, key[d]);
    if (cid >= 0) {
        int32_t sub_old = -1, sub_to = -1;
        int32_t nc = art_ins(kv, cid, key, klen, d + 1, off, &sub_old, &sub_to);
        if (nc < 0)
            return -1;
        if (nc != cid) {
            h = art_hdr(kv, nid);
            kv_journal_move_begin(kv, sub_old, sub_to, nid, key[d]);
            if (art_replace_child(kv, h, key[d], nc) != 0) {
                kv_journal_move_end(kv);
                return -1;
            }
            art_mark_moved(kv, sub_old, sub_to);
            kv_journal_move_end(kv);
        }
        return nid;
    }
    int32_t leaf = art_leaf_chain(kv, key, klen, d + 1, off);
    if (leaf < 0)
        return -1;
    h = art_hdr(kv, nid);
    bool n48_full = h->type == ART_N48 &&
                    __builtin_popcountll(art_n48_used_mask((const art_n48_t *)h)) >= 48;
    if ((h->type == ART_N4 && h->count >= 4) ||
        (h->type == ART_N16 && h->count >= 16) ||
        n48_full) {
        int32_t g = art_grow(kv, nid);
        if (g < 0)
            return -1;
        art_add(kv, art_hdr(kv, g), key[d], leaf);
        *moved_old = nid;
        *moved_to = g;
        return g;
    }
    if (h->type == ART_N16 && h->count > 0) {
        art_n16_t *x = (art_n16_t *)h;
        if (key[d] < x->keys[h->count - 1]) {
            /* CoW N16 mid-array insert so live sorted keys[] never shifts in place. */
            int32_t copy_id = art_clone_node_shortened(kv, nid, 0);
            if (copy_id < 0)
                return -1;
            art_add(kv, art_hdr(kv, copy_id), key[d], leaf);
            *moved_old = nid;
            *moved_to = copy_id;
            return copy_id;
        }
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
        if (pfx_shared(h->prefix, h->prefix_len, key + d, klen - d) !=
            h->prefix_len)
            return nid;
        d += h->prefix_len;
    }
    if (d == klen) {
        uint64_t hv = art_val_load(h);
        if (!AV_HAS(hv))
            return nid;
        uint64_t gidx;
        if (garb_reserve(kv, &gidx) != 0)
            return -1;                  /* 表满：硬错误，未变更 */
        if (!art_val_cas(h, &hv, 0)) {
            garb_cancel(kv, gidx);
            return nid;
        }
        gen_bump_value(kv, art_id_of(kv, h)); /* P2: 值删 */
        gen_bump_struct(kv, art_id_of(kv, h)); /* P2: 槽消失，view 失效 */
        garb_commit(kv, gidx, AV_OFF(hv),
                    sbo_allocated_size(kv->sbo_meta, AV_OFF(hv)));
        *del = true;
    } else {
        int32_t cid = art_child(kv, h, key[d]);
        if (cid >= 0) {
            int32_t rc = art_del(kv, cid, key, klen, d + 1, del);
            if (rc < 0)
                return rc;
        }
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

typedef struct {
    char **keys;
    int32_t count;
    int32_t cap;
} keyscan_t;

static void keyscan_free(keyscan_t *scan) {
    for (int32_t i = 0; i < scan->count; i++)
        free(scan->keys[i]);
    free(scan->keys);
    scan->keys = NULL;
    scan->count = scan->cap = 0;
}

static int keyscan_add_n(keyscan_t *scan, const char *key, size_t len) {
    if (scan->count == INT32_MAX)
        return -1;
    if (scan->count == scan->cap) {
        int32_t cap = scan->cap > INT32_MAX / 2 ? INT32_MAX :
                      scan->cap ? scan->cap * 2 : 64;
        char **keys = realloc(scan->keys, (size_t)cap * sizeof *keys);
        if (!keys)
            return -1;
        scan->keys = keys;
        scan->cap = cap;
    }
    char *copy = strndup(key, len);
    if (!copy)
        return -1;
    scan->keys[scan->count++] = copy;
    return 0;
}

/* Direct children end at '/' or U+00B7. */
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

/* Deleted values leave ART nodes behind. */
static int art_has_live_value(kvspace_t *kv, int32_t nid) {
    if (nid < 0)
        return 0;
    art_hdr_t *h = art_hdr(kv, nid);
    if (!h)
        return -1;
    if (AV_HAS(art_val_load(h)))
        return 1;
    const int32_t *children;
    int count = h->count;
    switch (h->type) {
    case ART_N4:
        children = ((art_n4_t *)h)->children;
        break;
    case ART_N16:
        children = ((art_n16_t *)h)->children;
        break;
    case ART_N48: {
        art_n48_t *x = (art_n48_t *)h;
        for (int i = 0; i < 256; i++) {
            uint8_t idx = x->index[i];
            if (idx >= 48)
                continue;
            int live = art_has_live_value(kv, x->children[idx]);
            if (live != 0)
                return live;
        }
        return 0;
    }
    case ART_N256:
        children = ((art_n256_t *)h)->children;
        count = 256;
        break;
    default:
        return 0;
    }
    for (int i = 0; i < count; i++) {
        int live = art_has_live_value(kv, children[i]);
        if (live != 0)
            return live;
    }
    return 0;
}

static int art_scan_children(kvspace_t *kv, int32_t nid, char *buf, int bpos, int bcap,
                             int plen, keyscan_t *scan) {
    if (nid < 0)
        return 0;
    art_hdr_t *h = art_hdr(kv, nid);
    if (!h || h->prefix_len > bcap - bpos - 1)
        return -1;
    /* Recheck the last parent byte for a split U+00B7. */
    int start = bpos > plen + 1 ? bpos - plen - 2 : 0;
    for (int i = 0; i < h->prefix_len; i++)
        buf[bpos++] = h->prefix[i];
    buf[bpos] = 0;
    if (bpos > plen) {
        const char *rest = buf + plen;
        int restlen = bpos - plen;
        int len = start + child_name_len(rest + start, restlen - start);
        /* Stop at the child boundary. */
        if (len < restlen) {
            if (len == 0)
                return 0;
            int live = art_has_live_value(kv, nid);
            if (live <= 0)
                return live;
            if (rest[len] == '/' && len + 1 == restlen &&
                AV_HAS(art_val_load(h)))
                len++;
            return keyscan_add_n(scan, rest, (size_t)len);
        }
        if (AV_HAS(art_val_load(h)) &&
            keyscan_add_n(scan, rest, (size_t)len) != 0)
            return -1;
    }
    switch (h->type) {
    case ART_N4: {
        art_n4_t *x = (art_n4_t *)h;
        for (int i = 0; i < (int)h->count; i++) {
            if (bpos + 1 >= bcap)
                return -1;
            buf[bpos] = x->keys[i];
            if (art_scan_children(kv, x->children[i], buf, bpos + 1, bcap,
                                  plen, scan) != 0)
                return -1;
        }
        break;
    }
    case ART_N16: {
        art_n16_t *x = (art_n16_t *)h;
        for (int i = 0; i < (int)h->count; i++) {
            if (bpos + 1 >= bcap)
                return -1;
            buf[bpos] = x->keys[i];
            if (art_scan_children(kv, x->children[i], buf, bpos + 1, bcap,
                                  plen, scan) != 0)
                return -1;
        }
        break;
    }
    case ART_N48: {
        art_n48_t *x = (art_n48_t *)h;
        for (int i = 0; i < 256; i++) {
            if (x->index[i] == 255)
                continue;
            if (bpos + 1 >= bcap)
                return -1;
            buf[bpos] = (uint8_t)i;
            if (art_scan_children(kv, x->children[x->index[i]], buf, bpos + 1,
                                  bcap, plen, scan) != 0)
                return -1;
        }
        break;
    }
    case ART_N256: {
        art_n256_t *x = (art_n256_t *)h;
        for (int i = 0; i < 256; i++) {
            if (x->children[i] < 0)
                continue;
            if (bpos + 1 >= bcap)
                return -1;
            buf[bpos] = (uint8_t)i;
            if (art_scan_children(kv, x->children[i], buf, bpos + 1, bcap,
                                  plen, scan) != 0)
                return -1;
        }
        break;
    }
    }
    return 0;
}

static int art_scan_pfx(kvspace_t *kv, const char *pfx, int plen, char *buf,
                        int bcap, keyscan_t *scan) {
    if (plen >= bcap)
        return -1;
    int bpos = 0;
    int32_t nid = art_cover(kv, kv->hdr->art_root, (const uint8_t *)pfx,
                            plen, buf, bcap, &bpos);
    if (nid < 0)
        return 0;
    return art_scan_children(kv, nid, buf, bpos, bcap, plen, scan);
}

/* Collect all full keys with values under nid (buf[0, bpos) holds the reconstructed prefix).
 * Unlike art_scan_children, does not truncate at child boundaries or hide system names (used by DelTree). */
static int art_collect_keys(kvspace_t *kv, int32_t nid, char *buf, int bpos,
                            int bcap, keyscan_t *scan) {
    if (nid < 0)
        return 0;
    art_hdr_t *h = art_hdr(kv, nid);
    if (!h || h->prefix_len > bcap - bpos - 1)
        return -1;
    for (int i = 0; i < h->prefix_len; i++)
        buf[bpos++] = h->prefix[i];
    if (AV_HAS(art_val_load(h)) && keyscan_add_n(scan, buf, (size_t)bpos) != 0)
        return -1;
    const uint8_t *keys = NULL;
    const int32_t *children = NULL;
    int n = 0;
    switch (h->type) {
    case ART_N4:
        keys = ((art_n4_t *)h)->keys;
        children = ((art_n4_t *)h)->children;
        n = h->count;
        break;
    case ART_N16:
        keys = ((art_n16_t *)h)->keys;
        children = ((art_n16_t *)h)->children;
        n = h->count;
        break;
    case ART_N48: {
        art_n48_t *x = (art_n48_t *)h;
        for (int i = 0; i < 256; i++) {
            if (x->index[i] == 255)
                continue;
            if (bpos + 1 >= bcap)
                return -1;
            buf[bpos] = (char)i;
            if (art_collect_keys(kv, x->children[x->index[i]], buf, bpos + 1,
                                 bcap, scan) != 0)
                return -1;
        }
        return 0;
    }
    case ART_N256: {
        art_n256_t *x = (art_n256_t *)h;
        for (int i = 0; i < 256; i++) {
            if (x->children[i] < 0)
                continue;
            if (bpos + 1 >= bcap)
                return -1;
            buf[bpos] = (char)i;
            if (art_collect_keys(kv, x->children[i], buf, bpos + 1, bcap,
                                 scan) != 0)
                return -1;
        }
        return 0;
    }
    default:
        return 0;
    }
    for (int i = 0; i < n; i++) {
        if (bpos + 1 >= bcap)
            return -1;
        buf[bpos] = (char)keys[i];
        if (art_collect_keys(kv, children[i], buf, bpos + 1, bcap, scan) != 0)
            return -1;
    }
    return 0;
}

/* Collect all keys with values starting with pfx (including system variables). */
static int art_scan_keys(kvspace_t *kv, const char *pfx, int plen,
                         keyscan_t *scan) {
    char buf[4096];
    if (plen >= (int)sizeof buf)
        return -1;
    int bpos = 0;
    int32_t nid = art_cover(kv, kv->hdr->art_root, (const uint8_t *)pfx, plen,
                            buf, (int)sizeof buf, &bpos);
    if (nid < 0)
        return 0;
    return art_collect_keys(kv, nid, buf, bpos, (int)sizeof buf, scan);
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
    if (!bm_layout_ok()) {
        fprintf(stderr, "kvspace: unexpected blockmalloc blocks_meta_t layout\n");
        return NULL;
    }
    char head_path[PATH_MAX], data_path[PATH_MAX], gen_path[PATH_MAX],
        garb_path[PATH_MAX];
    if (snprintf(head_path, sizeof head_path, "%s.sbo.head", path) >=
            (int)sizeof head_path ||
        snprintf(data_path, sizeof data_path, "%s.sbo.data", path) >=
            (int)sizeof data_path ||
        snprintf(gen_path, sizeof gen_path, "%s.gen", path) >=
            (int)sizeof gen_path ||
        snprintf(garb_path, sizeof garb_path, "%s.garb", path) >=
            (int)sizeof garb_path)
        return NULL;

    kvspace_t *kv = calloc(1, sizeof(*kv));
    if (!kv)
        return NULL;
    kv->p2_slot = -1;
    kv->r_art.fd = kv->r_head.fd = kv->r_data.fd = kv->r_gen.fd =
        kv->r_garb.fd = -1;

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
        /* Check format_version before region_attach: header size changes across versions,
         * so an older store would otherwise fail in region_attach without printing a diagnostic. */
        if (tmp.format_version != KVSPACE_FORMAT_VERSION) {
            fprintf(stderr, "kvspace: unsupported store format (version=%u, "
                            "expected %u): %s; rebuild the shm store\n",
                    tmp.format_version, KVSPACE_FORMAT_VERSION, path);
            goto fail;
        }
        art_slab = (size_t)tmp.art_slab_size;
        sbo_head = (size_t)tmp.sbo_head_size;
        data_size = (size_t)tmp.sbo_data_size;
    }

    /* O_TRUNC: sbo_init rejects a stale magic */
    int fl = created ? O_RDWR | O_CREAT | O_TRUNC : O_RDWR;
    if (region_attach(&kv->r_art, created, ART_OFF + art_slab,
                      ART_RESERVE) != 0 ||
        (kv->r_head.fd = open(head_path, fl, 0644)) < 0 ||
        region_attach(&kv->r_head, created, sbo_head, REGION_RESERVE) != 0 ||
        (kv->r_data.fd = open(data_path, fl, 0644)) < 0 ||
        region_attach(&kv->r_data, created, data_size, DATA_RESERVE) != 0 ||
        (kv->r_gen.fd = open(gen_path, fl, 0644)) < 0 ||
        (kv->r_garb.fd = open(garb_path, fl, 0644)) < 0 ||
        region_attach(&kv->r_garb, created,
                      (size_t)GARB_CAP * sizeof(garb_ent_t),
                      (size_t)GARB_CAP * sizeof(garb_ent_t)) != 0)
        goto fail;

    kv->hdr = (kvspace_hdr_t *)kv->r_art.base;
    kv->art_meta = (blocks_meta_t *)(kv->r_art.base + sizeof(kvspace_hdr_t));
    kv->art_data = kv->r_art.base + ART_OFF;
    kv->sbo_meta = (sbo_meta_t *)kv->r_head.base;
    kv->sbo_data = kv->r_data.base;
    kv->garb = (garb_ent_t *)kv->r_garb.base;

    /* 每进程单调句柄序号：close 后重开即使 malloc 复用同一 kv 地址，旧 token 也失效。 */
    {
        static uint32_t store_seq;
        kv->store_id = ((uint64_t)getpid() << 32) | (uint64_t)++store_seq;
    }

    if (created) {
        memset(kv->hdr, 0, sizeof(*kv->hdr));
        kv->hdr->format_version = KVSPACE_FORMAT_VERSION;
        kv->hdr->garb_cap = GARB_CAP;
        kv->hdr->art_slab_size = art_slab;
        kv->hdr->sbo_head_size = sbo_head;
        kv->hdr->sbo_data_size = data_size;
        kv->hdr->art_root = -1;
        kv->hdr->pending_moved_old = -1;
        kv->hdr->pending_moved_to = -1;
        kv->hdr->pending_moved_par = UINT64_MAX;
        if (blocks_init(kv->art_meta, art_slab, ART_BLOCK_SZ) != 0 ||
            sbo_init(kv->sbo_meta, sbo_head, data_size) != 0)
            goto fail;
        /* blocks_init 按容量选 2/4/8 字节块头。4 字节头 + 8 对齐的块大小使
         * 节点地址交替落在 8k+4，art_hdr_t.val（偏移 16）跨 64B 行：x86
         * locked CAS 变成 split lock（实测 ~7k tick/次，del p95 26k）。
         * 节点地址必须 8 字节对齐（原子字不可跨行），故强制 8 字节头。 */
        kv->art_meta->sizeof_block_head = 8;
        if ((kv->art_meta->sizeof_block_head +
             kv->art_meta->block_size) % 8 != 0)
            goto fail;
        /* magic last: half-initialized file fails reopen */
        memcpy(kv->hdr->magic, KVS_MAGIC, sizeof(KVS_MAGIC) - 1);
    }
    if (!created &&
        (kv->hdr->format_version != KVSPACE_FORMAT_VERSION ||
         kv->art_meta->sizeof_block_head != 8 ||
         kv->hdr->garb_cap != GARB_CAP)) {
        fprintf(stderr, "kvspace: unsupported store format (version=%u, "
                        "garb_cap=%llu); rebuild the shm store\n",
                kv->hdr->format_version, (unsigned long long)kv->hdr->garb_cap);
        goto fail;
    }
    if (gens_grow(kv) != 0)   /* P2: gen 表在 blocks_init 之后 */
        goto fail;

    /* growth code assumes the single-root pool layout (SBO_HEAD_FIXED) */
    if (kv->sbo_meta->root_slots != 1)
        goto fail;
    if (!created && kv_sync(kv) != 0)
        goto fail;
    if (!created && __atomic_load_n(&kv->hdr->pending_moved_old, __ATOMIC_ACQUIRE) >= 0) {
        kv_write_lock(kv);
        kv_repair_pending_move(kv);
        kv_write_unlock(kv);
    }

    /* P2: 注册表认领。占满（N=64）拒绝 open，错误明确，不静默扩容。 */
    kv->p2_slot = p2_reg_claim(kv);
    if (kv->p2_slot == -2) {
        fprintf(stderr, "kvspace: too many processes, N=%d\n", P2_SLOT_N);
        goto fail;
    }
    if (kv->p2_slot == -3) {
        fprintf(stderr, "kvspace: claim_id exhausted (2^30 opens): "
                        "rebuild the store\n");
        goto fail;
    }
    if (kv->p2_slot < 0)
        goto fail;
    kv->pid = (uint32_t)getpid();
    (void)pthread_once(&g_atfork_once, p2_atfork_install);
    kv->fork_gen = __atomic_load_n(&g_fork_gen, __ATOMIC_ACQUIRE);
    kv->quiesced = 1;               /* claim 时 slot->quiesced=1，二者一致 */
    if (kv->hdr->committed_bytes -
            __atomic_load_n(&kv->hdr->freed_bytes, __ATOMIC_ACQUIRE) >=
            p2_garb_threshold() &&
        p2_maint_start(kv) != 0)
        goto fail;                  /* 已有大量垃圾且建不了线程：open 硬失败 */

    pthread_mutex_init(&kv->wlock, NULL);
    pthread_mutex_init(&kv->lc_mtx, NULL);
    for (int i = 0; i < WATCH_TABLE_SZ; i++) {
        pthread_cond_init(&kv->watches[i].cond, NULL);
        pthread_mutex_init(&kv->watches[i].mtx, NULL);
    }
    return kv;

fail:
    p2_maint_stop(kv);
    if (kv->p2_slot >= 0 && kv->hdr)
        p2_reg_release(kv);
    region_close(&kv->r_art);
    region_close(&kv->r_head);
    region_close(&kv->r_data);
    region_close(&kv->r_gen);
    region_close(&kv->r_garb);
    free(kv);
    if (created) {
        unlink(path);
        unlink(head_path);
        unlink(data_path);
        unlink(gen_path);
        unlink(garb_path);
    }
    return NULL;
}
/* 除自己外是否还有存活进程（macOS 无 /proc：只有 FREE 槽才算“不在”）。 */
static bool p2_other_alive(kvspace_t *kv) {
    for (int i = 0; i < P2_SLOT_N; i++) {
        if (i == kv->p2_slot)
            continue;
        p2_slot_t *s = &kv->hdr->slots[i];
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != P2_SLOT_MINE)
            continue;
        if (p2_slot_alive(s))
            return true;
    }
    return false;
}

void kvspaceShmClose(kvspace_t *kv) {
    if (!kv)
        return;
    /* 最后一个存活进程负责清场：Linux 跑一次机会性 round；macOS 直接清环。 */
    if (kv->pid == (uint32_t)getpid() && kv->p2_slot >= 0 &&
        !p2_other_alive(kv)) {
#if defined(__linux__)
        __atomic_store_n(&kv->hdr->slots[kv->p2_slot].quiesced, 1u,
                         __ATOMIC_RELEASE);
        (void)p2_round(kv);
#else
        uint64_t h = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
        uint64_t t = __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE);
        for (; h < t; h++) {
            garb_ent_t *e = &kv->garb[h & (GARB_CAP - 1)];
            uint32_t m = __atomic_load_n(&e->meta, __ATOMIC_ACQUIRE);
            if (GE_STATE(m) == GE_COMMITTED) {
                uint64_t off = __atomic_load_n(&e->off, __ATOMIC_ACQUIRE);
                uint64_t cap = sbo_allocated_size(kv->sbo_meta, off);
                __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
                kv_sbo_free(kv, off);
                __atomic_store_n(&kv->hdr->freed_bytes,
                                 kv->hdr->freed_bytes + cap, __ATOMIC_RELEASE);
            } else {
                __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
            }
        }
        __atomic_store_n(&kv->hdr->garb_head, t, __ATOMIC_RELEASE);
#endif
    }
    p2_maint_stop(kv);
    p2_reg_release(kv);
    for (int i = 0; i < WATCH_TABLE_SZ; i++) {
        pthread_cond_destroy(&kv->watches[i].cond);
        pthread_mutex_destroy(&kv->watches[i].mtx);
        free(kv->watches[i].val);
    }
    pthread_mutex_destroy(&kv->wlock);
    list_cache_drop(kv);
    pthread_mutex_destroy(&kv->lc_mtx);
    region_close(&kv->r_art);
    region_close(&kv->r_head);
    region_close(&kv->r_data);
    region_close(&kv->r_gen);
    region_close(&kv->r_garb);
    free(kv);
}

/* ---- link resolve helpers ---- */
/* Parse full value length = head + body + attr within [s, s+bound) (empty attr = 2). */
static int parse_value(const uint8_t *s, uint64_t bound, int32_t *ol) {
    if (bound > INT32_MAX)
        bound = INT32_MAX;
    kvspaceXvalueHead wire;
    if (head_of(s, (int32_t)bound, &wire) != 0)
        return -1;
    uint64_t l = kvspaceXvalueHeadLen(&wire);
    if (bound < KVSPACE_XVALUE_HEAD_LEN + l + 2)
        return -1;
    uint64_t alen = kvspaceXvalueHeadAttrLen(s, l);
    if (alen < 2 || alen > bound - KVSPACE_XVALUE_HEAD_LEN - l)
        return -1;
    *ol = (int32_t)(KVSPACE_XVALUE_HEAD_LEN + l + alen);
    return 0;
}

/* Write path: bound by physical box capacity (takes allocator lock) so a corrupted
 * header cannot cause caller writes to overflow into adjacent boxes. */
static int read_value(kvspace_t *kv, uint64_t off, uint8_t **out, int32_t *ol) {
    *out = kv->sbo_data + off;
    return parse_value(*out, sbo_allocated_size(kv->sbo_meta, off), ol);
}

/* Read-only path: bound by mapped data region without taking the allocator lock.
 * sbo_allocated_size contends on the cross-process spinlock on every read; since value_ok
 * already ensures 8+len+attr_len fits in the box at write time, both bounds agree on valid values. */
static uint64_t ro_bound(const kvspace_t *kv, uint64_t off) {
    uint64_t m = kv->r_data.mapped;
    return off < m ? m - off : 0;
}
static int read_value_ro(kvspace_t *kv, uint64_t off, uint8_t **out,
                         int32_t *ol) {
    *out = kv->sbo_data + off;
    return parse_value(*out, ro_bound(kv, off), ol);
}
/* Link resolution: ptr (ref=1) and @ext (ref=2) XValues are always leaves (never intermediate
 * `/` prefixes), so only the full leaf key can be a link. Follow at most 16 hops, fetching
 * the terminal leaf value at the same time (fetched=1) to avoid a second art_search + read_value.
 * Missing/valueless leaf returns fetched=0 for extindex/dir fallback. Caller ensures strlen(path) < osz. */
static art_hdr_t *resolve_fetch(kvspace_t *kv, const char *path, char *out, int osz,
                                uint8_t **raw, int32_t *rl, int *fetched) {
    *fetched = 0;
    strncpy(out, path, osz - 1);
    out[osz - 1] = '\0';
    for (int depth = 0; depth < 16; depth++) {
        art_hdr_t *h =
            art_search(kv, kv->hdr->art_root, (const uint8_t *)out, (int)strlen(out));
        if (!h)
            return h;
        uint64_t hv = art_val_load(h);
        if (!AV_HAS(hv))
            return h;
        uint8_t *s = kv->sbo_data + AV_OFF(hv);
        uint64_t sz = ro_bound(kv, AV_OFF(hv));
        kvspaceXvalueHead wire;
        if (head_of(s, sz > INT32_MAX ? INT32_MAX : (int32_t)sz, &wire) == 0) {
            uint64_t l3 = kvspaceXvalueHeadLen(&wire);
            if (wire.ptr != KVSPACE_XVALUE_PTR_PTR || l3 >= (uint64_t)osz) {
                if (parse_value(s, sz, rl) != 0)
                    return NULL;
                *raw = s;
                *fetched = 1;
                return h;
            }
            memcpy(out, s + KVSPACE_XVALUE_HEAD_LEN, (size_t)l3);
            out[l3] = 0;
            continue;
        }
        return NULL;
    }
    return NULL;
}

/* Resolve path only (used by List). */
static void resolve_path(kvspace_t *kv, const char *path, char *out, int osz) {
    uint8_t *raw;
    int32_t rl;
    int fetched;
    (void)resolve_fetch(kv, path, out, osz, &raw, &rl, &fetched);
}

/* Read the extension locator stored at a directory key. */
static int dir_ext_path(kvspace_t *kv, const char *dir, char *out, int osz) {
    out[0] = 0;
    art_hdr_t *h =
        art_search(kv, kv->hdr->art_root, (const uint8_t *)dir, (int)strlen(dir));
    if (!h)
        return 0;
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return 0;
    uint8_t *raw;
    int32_t rl;
    if (read_value_ro(kv, AV_OFF(hv), &raw, &rl) < 0)
        return 0;
    kvspaceXvalueHead wire;
    if (head_of(raw, rl, &wire) != 0 ||
        wire.ptr != KVSPACE_XVALUE_PTR_EXT)
        return 0;
    uint64_t l4 = kvspaceXvalueHeadLen(&wire);
    if (l4 == 0 || l4 >= (uint64_t)osz)
        return 0;
    memcpy(out, raw + KVSPACE_XVALUE_HEAD_LEN, (size_t)l4);
    out[l4] = 0;
    return 1;
}

/* ============ CRUD ============ */
uint8_t *kvspaceShmGet(kvspace_t *kv, const char *key, int resolve,
                       int32_t *ol) {
    if (!kv || !key || !ol)
        return NULL;
    *ol = 0;
    if (reserved_meta_path(key))
        return NULL;
    if (kv_sync(kv) != 0)
        return NULL;
    char kbuf[1024];
    /* Set rejects >= 1024-byte keys; do not truncate and look up a shorter key. */
    if (strlen(key) >= sizeof kbuf)
        return NULL;
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
    uint64_t hv = h ? art_val_load(h) : 0;
    if (!h || !AV_HAS(hv)) {
        /* extindex fallback：父目录是 extindex → 读 extpath + name */
        char *parent = NULL, *name = NULL;
        psplit(kbuf, &parent, &name);
        char extpath[1024];
        if (dir_ext_path(kv, parent, extpath, sizeof extpath)) {
            char *target = pjoin(extpath, name);
            art_hdr_t *eh = art_search(kv, kv->hdr->art_root, (const uint8_t *)target,
                                       (int)strlen(target));
            free(target);
            uint64_t ehv = eh ? art_val_load(eh) : 0;
            if (eh && AV_HAS(ehv)) {
                uint8_t *raw;
                int32_t rl;
                if (read_value_ro(kv, AV_OFF(ehv), &raw, &rl) == 0) {
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
    if (!fetched && read_value_ro(kv, AV_OFF(hv), &raw, &rl) < 0)
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
    if (d < 0 || d > klen)   /* P2: d == klen 表示前缀节点就是全部路径 */
        return -1;
    if (d == klen)
        return art_follow(kv, (int32_t)ref->block_id);
    art_hdr_t *h = art_hdr(kv, id);
    if (!h || h->type == ART_MOVED)
        return -1;
    int32_t cid = art_child(kv, h, (uint8_t)key[d]);
    if (cid < 0)
        return -1;
    return art_walk(kv, cid, (const uint8_t *)key, klen, d + 1, NULL, NULL);
}

int kvspaceShmResolveRef(kvspace_t *kv, const char *key, kvspaceRef_t *ref) {
    if (!kv || !key || !ref || reserved_meta_path(key))
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
    uint64_t hv = h ? art_val_load(h) : 0;
    if (h && AV_HAS(hv)) {
        uint8_t *raw;
        int32_t rl;
        if (read_value_ro(kv, AV_OFF(hv), &raw, &rl) == 0) {
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

/* Tear-free partial overwrite on leaf node id (caller holds kv_write_lock).
 * Aligned 1/2/4/8-byte writes use a single store; wider writes allocate a new
 * SBO box, copy-and-patch raw[0..rl), atomically swap h->val, and retire the old box. */
static int shm_patch_leaf_locked(kvspace_t *kv, int32_t id, art_hdr_t *h,
                                 uint64_t old_off, uint64_t cap,
                                 uint8_t *raw, int32_t rl,
                                 uint32_t offset, const uint8_t *buf,
                                 uint32_t buf_len) {
    if ((uint64_t)offset + buf_len > (uint64_t)rl || (uint64_t)rl > cap)
        return -1;
    if (buf_len == 0)
        return 0;
    if (shm_write_body_atomic(raw + offset, buf, buf_len)) {
        gen_bump_value(kv, id);
        return 0;
    }
    uint64_t gidx = 0;
    if (garb_reserve(kv, &gidx) != 0)
        return -1;
    size_t alloc_len = (size_t)cap > (size_t)rl ? (size_t)cap : (size_t)rl;
    uint64_t new_off = kv_sbo_alloc(kv, alloc_len);
    if (new_off == (uint64_t)-1) {
        garb_cancel(kv, gidx);
        return -1;
    }
    /* Recompute pointers in case kv_sbo_alloc grew .sbo.data / ART mappings. */
    h = art_hdr(kv, id);
    if (!h) {
        garb_cancel(kv, gidx);
        kv_sbo_free(kv, new_off);
        return -1;
    }
    const uint8_t *src = kv->sbo_data + old_off;
    uint8_t *dst = kv->sbo_data + new_off;
    memcpy(dst, src, (size_t)rl);
    memcpy(dst + offset, buf, buf_len);
    art_val_store(h, AV_MAKE(1, new_off));
    gen_bump_struct(kv, id);
    gen_bump_value(kv, id);
    garb_commit(kv, gidx, old_off, cap);
    return 0;
}

int kvspaceShmSetPartByRef(kvspace_t *kv, kvspaceRef_t *ref,
                           const char *key_fallback, uint32_t offset,
                           const uint8_t *buf, uint32_t buf_len) {
    if (!kv || !ref || !buf)
        return -1;
    kv_write_lock(kv);
    if (kv_sync(kv) != 0) {
        kv_write_unlock(kv);
        return -1;
    }
    int32_t id = ref_leaf(kv, ref, key_fallback);
    art_hdr_t *h = id >= 0 ? art_hdr(kv, id) : NULL;
    uint64_t hv = h ? art_val_load(h) : 0;
    if (h && AV_HAS(hv) && ref->gen == 0) {
        uint64_t data_size = kv->hdr->sbo_data_size;
        uint64_t off = AV_OFF(hv);
        if (off <= data_size && data_size - off >= KVSPACE_XVALUE_HEAD_LEN) {
            uint8_t *hs = kv->sbo_data + off;
            kvspaceXvalueHead w;
            memcpy(&w, hs, sizeof w);
            if (w.storetype == KVSPACE_XVALUE_ST_SLACK &&
                w.ptr == KVSPACE_XVALUE_PTR_INLINE) {
                uint64_t len = kvspaceXvalueHeadLen(&w);
                uint64_t cap = sbo_allocated_size(kv->sbo_meta, off);
                if (cap <= data_size - off && len <= cap - KVSPACE_XVALUE_HEAD_LEN &&
                    (uint64_t)offset <= KVSPACE_XVALUE_HEAD_LEN + len &&
                    (uint64_t)buf_len <=
                        KVSPACE_XVALUE_HEAD_LEN + len - (uint64_t)offset) {
                    if (shm_write_body_atomic(hs + offset, buf, buf_len)) {
                        gen_bump_value(kv, id);
                        kv_write_unlock(kv);
                        return 0;
                    }
                    int32_t rl = 0;
                    if (parse_value(hs, cap, &rl) == 0) {
                        int rc = shm_patch_leaf_locked(kv, id, h, off, cap, hs,
                                                       rl, offset, buf, buf_len);
                        kv_write_unlock(kv);
                        return rc;
                    }
                }
            }
        }
    }
    if (!h || !AV_HAS(hv)) {
        if (ref->gen != 0 || !key_fallback) {
            kv_write_unlock(kv);
            return -1;
        }
        int32_t rl = 0;
        uint8_t *d = kvspaceShmGet(kv, key_fallback, 0, &rl);
        if (!d) {
            kv_write_unlock(kv);
            return -1;
        }
        uint64_t off = (uint64_t)(d - kv->sbo_data);
        uint64_t cap = sbo_allocated_size(kv->sbo_meta, off);
        if (parse_value(d, cap, &rl) != 0 ||
            (uint64_t)offset + buf_len > (uint64_t)rl ||
            kvspaceShmResolveRef(kv, key_fallback, ref) != 0) {
            kv_write_unlock(kv);
            return -1;
        }
        int32_t lid = (int32_t)ref->block_id;
        art_hdr_t *lh = art_hdr(kv, lid);
        if (!lh || !AV_HAS(art_val_load(lh))) {
            kv_write_unlock(kv);
            return -1;
        }
        int rc = shm_patch_leaf_locked(kv, lid, lh, off, cap, d, rl, offset,
                                       buf, buf_len);
        kv_write_unlock(kv);
        return rc;
    }
    uint64_t off = AV_OFF(hv);
    uint64_t cap = sbo_allocated_size(kv->sbo_meta, off);
    uint8_t *raw = kv->sbo_data + off;
    int32_t rl = 0;
    if (parse_value(raw, cap, &rl) < 0 ||
        (uint64_t)offset + buf_len > (uint64_t)rl) {
        kv_write_unlock(kv);
        return -1;
    }
    int rc = shm_patch_leaf_locked(kv, id, h, off, cap, raw, rl, offset, buf,
                                   buf_len);
    kv_write_unlock(kv);
    return rc;
}

/* Partial write by key (follows links and extindex fallback like Get with resolve=1).
 * Re-bounds by physical box capacity under write_lock before writing. */
int kvspaceShmSetPart(kvspace_t *kv, const char *key, uint32_t offset,
                      const uint8_t *buf, uint32_t buf_len) {
    if (!kv || !key || (buf_len && !buf))
        return -1;
    kv_write_lock(kv);
    int32_t rl = 0;
    uint8_t *d = kvspaceShmGet(kv, key, 1, &rl);
    if (!d) {
        kv_write_unlock(kv);
        return -1;
    }
    uint64_t off = (uint64_t)(d - kv->sbo_data);
    uint64_t cap = sbo_allocated_size(kv->sbo_meta, off);
    if (parse_value(d, cap, &rl) != 0 ||
        (uint64_t)offset + buf_len > (uint64_t)rl) {
        kv_write_unlock(kv);
        return -1;
    }
    art_hdr_t *th = NULL;
    char kbuf[1024];
    uint8_t *rraw;
    int32_t rrl;
    int rfetched = 0;
    th = resolve_fetch(kv, key, kbuf, sizeof kbuf, &rraw, &rrl, &rfetched);
    if (!th || !AV_HAS(art_val_load(th)) || AV_OFF(art_val_load(th)) != off) {
        /* Target lives under extindex mount; resolve its backing leaf. */
        char *parent = NULL, *name = NULL;
        psplit(kbuf, &parent, &name);
        char extpath[1024];
        th = NULL;
        if (parent && name && dir_ext_path(kv, parent, extpath, sizeof extpath)) {
            char *target = pjoin(extpath, name);
            th = art_search(kv, kv->hdr->art_root, (const uint8_t *)target,
                            (int)strlen(target));
            free(target);
        }
        free(parent);
        free(name);
    }
    if (!th || !AV_HAS(art_val_load(th))) {
        kv_write_unlock(kv);
        return -1;
    }
    int32_t tid = art_id_of(kv, th);
    int rc = shm_patch_leaf_locked(kv, tid, th, off, cap, d, rl, offset, buf,
                                   buf_len);
    kv_write_unlock(kv);
    return rc;
}

/* /vthread/<vid>/..status: maximum content length and minimum physical box capacity. */
#define KVSPACE_STATUS_MAX_LEN 16u
#define KVSPACE_STATUS_MIN_CAP 80u
/* /vthread/<vid>/..pc: minimum physical box capacity (8B head + 1024B body + 2B attr). */
#define KVSPACE_PC_MIN_CAP 1034u

static int is_vthread_status_key(const char *key) {
    static const char pfx[] = "/vthread/";
    static const char suf[] = "/\xE2\x80\xA5status";
    if (!key)
        return 0;
    size_t n = strlen(key), sl = sizeof suf - 1;
    return n > sl && strncmp(key, pfx, sizeof pfx - 1) == 0 &&
           memcmp(key + n - sl, suf, sl) == 0;
}

static int is_vthread_pc_key(const char *key) {
    static const char pfx[] = "/vthread/";
    static const char suf[] = "/\xE2\x80\xA5pc";
    if (!key)
        return 0;
    size_t n = strlen(key), sl = sizeof suf - 1;
    return n > sl && strncmp(key, pfx, sizeof pfx - 1) == 0 &&
           memcmp(key + n - sl, suf, sl) == 0;
}

int kvspaceShmXvalueBodyView(kvspace_t *kv, const kvspaceRef_t *ref,
                       const char *key_fallback, kvspaceXvalueBodyView_t *out) {
    /* status 识别依赖 key_fallback；view 一律要求非 NULL（runtime 必须传）。 */
    if (!kv || !ref || !out || !key_fallback)
        return -1;
    if (ref->gen != 0)
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    kvspaceRef_t r = *ref;
    int32_t id = ref_leaf(kv, &r, key_fallback);
    art_hdr_t *h = id >= 0 ? art_hdr(kv, id) : NULL;
    if (!h)
        return -1;
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return -1;
    uint8_t *raw = NULL;
    int32_t rl = 0;
    if (read_value(kv, AV_OFF(hv), &raw, &rl) < 0 || !raw)
        return -1;
    kvspaceXvalueHead wire;
    if (head_of(raw, rl, &wire) != 0)
        return -1;
    /* ptr/ext 槽不可视：没有稳定的 head/body 语义。 */
    if (wire.ptr != KVSPACE_XVALUE_PTR_INLINE)
        return -1;
    uint64_t cap = sbo_allocated_size(kv->sbo_meta, AV_OFF(hv));
    uint64_t l5 = kvspaceXvalueHeadLen(&wire);
    if (cap < KVSPACE_XVALUE_HEAD_LEN ||
        l5 > cap - KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    /* status: only expose canonical slots (SLACK, len <= 16) with sufficient physical capacity;
       langtype ([n]char/utf8) lives in the inline attr region and is not checked here. */
    if (is_vthread_status_key(key_fallback)) {
        if (wire.storetype != KVSPACE_XVALUE_ST_SLACK ||
            cap < KVSPACE_STATUS_MIN_CAP ||
            l5 > KVSPACE_STATUS_MAX_LEN)
            return -1;
    }
    out->head = raw;
    out->headlen = KVSPACE_XVALUE_HEAD_LEN;
    out->body = raw + KVSPACE_XVALUE_HEAD_LEN;
    out->cap = cap;
    out->body_cap = cap - KVSPACE_XVALUE_HEAD_LEN;
    return 0;
}

/* Check if an overwrite can safely update ONLY an aligned 1/2/4/8-byte scalar
 * body in place (with identical 8-byte head and identical trailing attr). */
static bool can_atomic_scalar_overwrite(const uint8_t *old_raw, uint64_t cap,
                                        const uint8_t *val, int32_t val_len,
                                        uint32_t *out_blen) {
    if (val_len < (int32_t)KVSPACE_XVALUE_HEAD_LEN + 2 || (uint64_t)val_len > cap)
        return false;
    if (memcmp(old_raw, val, KVSPACE_XVALUE_HEAD_LEN) != 0)
        return false;
    kvspaceXvalueHead hd;
    memcpy(&hd, val, sizeof hd);
    if (hd.ptr != KVSPACE_XVALUE_PTR_INLINE)
        return false;
    uint64_t blen = kvspaceXvalueHeadLen(&hd);
    if (!(blen == 1 || blen == 2 || blen == 4 || blen == 8))
        return false;
    const uint8_t *dst = old_raw + KVSPACE_XVALUE_HEAD_LEN;
    if (((uintptr_t)dst & (uintptr_t)(blen - 1)) != 0)
        return false;
    uint64_t env_off = KVSPACE_XVALUE_HEAD_LEN + blen;
    if ((uint64_t)val_len < env_off + 2)
        return false;
    size_t attr_len = (size_t)((uint64_t)val_len - env_off);
    if (memcmp(old_raw + env_off, val + env_off, attr_len) != 0)
        return false;
    *out_blen = (uint32_t)blen;
    return true;
}

/* Raw local write (internal helper without container/member semantics).
 * reserve: minimum physical box capacity (for WriteNewPlace body_cap); 0 uses val_len.
 * Caller holds kv_write_lock(kv). */
static int shm_set_local_locked(kvspace_t *kv, const char *key, const uint8_t *val,
                                int32_t val_len, art_hdr_t *old, size_t reserve) {
    size_t alloc_len = (size_t)val_len;
    if (reserve > alloc_len)
        alloc_len = reserve;
    if (is_vthread_status_key(key) && alloc_len < KVSPACE_STATUS_MIN_CAP)
        alloc_len = KVSPACE_STATUS_MIN_CAP;
    if (is_vthread_pc_key(key) && alloc_len < KVSPACE_PC_MIN_CAP)
        alloc_len = KVSPACE_PC_MIN_CAP;
    uint64_t ohv = old ? art_val_load(old) : 0;
    bool retiring = old && AV_HAS(ohv);
    uint64_t gidx = 0;
    uint64_t cap = 0;
    if (retiring) {
        cap = sbo_allocated_size(kv->sbo_meta, AV_OFF(ohv));
        if (reserve > 0 && (uint64_t)alloc_len <= cap) {
            /* Reserved-capacity WriteNewPlace: caller fills body after return. */
            kv_gens_t *g = gens_at(kv, art_id_of(kv, old));
            if (g)
                __atomic_add_fetch(&g->value_gen, 1u, __ATOMIC_ACQ_REL);
            memcpy(kv->sbo_data + AV_OFF(ohv), val, (size_t)val_len);
            if (g)
                __atomic_add_fetch(&g->value_gen, 1u, __ATOMIC_ACQ_REL);
            return 0;
        }
        const uint8_t *old_raw = kv->sbo_data + AV_OFF(ohv);
        int32_t old_rl = 0;
        if (reserve == 0 && (uint64_t)alloc_len <= cap &&
            parse_value(old_raw, cap, &old_rl) == 0 && old_rl == val_len &&
            memcmp(old_raw, val, (size_t)val_len) == 0) {
            return 0;
        }
        uint32_t sblen = 0;
        if (reserve == 0 && (uint64_t)alloc_len <= cap &&
            can_atomic_scalar_overwrite(old_raw, cap, val, val_len, &sblen)) {
            /* Tear-free single-instruction scalar overwrite with unchanged head+attr. */
            (void)shm_write_body_atomic(
                kv->sbo_data + AV_OFF(ohv) + KVSPACE_XVALUE_HEAD_LEN,
                val + KVSPACE_XVALUE_HEAD_LEN, sblen);
            gen_bump_value(kv, art_id_of(kv, old));
            return 0;
        }
        /* CoW box replacement: reserve garbage slot before mutating state. */
        if (garb_reserve(kv, &gidx) != 0)
            return -1;
#ifdef P2_TEST_HOOKS
        {
            const char *v = getenv("P2_TEST_HOLD_RESERVE_MS");
            if (v && atoi(v) > 0)
                usleep((useconds_t)atoi(v) * 1000);
        }
        if (getenv("P2_TEST_FAIL_ALLOC")) {
            garb_cancel(kv, gidx);
            return -1;
        }
#endif
    }
    uint64_t off = kv_sbo_alloc(kv, alloc_len);
    if (off == (uint64_t)-1) {
        if (retiring)
            garb_cancel(kv, gidx);
        return -1;
    }
    memcpy(kv->sbo_data + off, val, val_len);
#ifdef P2_TEST_HOOKS
    if (retiring && getenv("P2_TEST_FAIL_ARTINS")) {
        garb_cancel(kv, gidx);
        kv_sbo_free(kv, off);
        return -1;
    }
#endif
    int32_t moved_old = -1, moved_to = -1;
    int32_t old_root = kv->hdr->art_root;
    int32_t root = art_ins(kv, old_root, (const uint8_t *)key,
                           (int)strlen(key), 0, off, &moved_old, &moved_to);
    if (root < 0) {
        if (retiring)
            garb_cancel(kv, gidx);
        kv_sbo_free(kv, off);          /* 新 box 从未被引用 */
        return -1;
    }
    if (root != old_root) {
        kv_journal_move_begin(kv, moved_old, moved_to, -1, 0);
        __atomic_store_n(&kv->hdr->art_root, root, __ATOMIC_RELEASE);
        art_mark_moved(kv, moved_old, moved_to);
        kv_journal_move_end(kv);
    }
    {   /* 换 box / 墓碑复活：旧 token 全部 STALE */
        int32_t nid = p2_node_at(kv, key);
        kv_gens_t *g = gens_at(kv, nid);
        if (g) {
            gen_bump_struct(kv, nid);
            gen_bump_value(kv, nid);
        }
    }
    if (retiring)
        garb_commit(kv, gidx, AV_OFF(ohv), cap);
    else
        list_gen_bump(kv);   /* new key or tombstone revive: bump after art_ins */
    return 0;
}

static int shm_set_raw(kvspace_t *kv, const char *key, const uint8_t *val,
                       int32_t val_len, size_t reserve) {
    kv_write_lock(kv);
    if (kv_sync(kv) != 0) {
        kv_write_unlock(kv);
        return -1;
    }
    art_hdr_t *old = art_search(kv, kv->hdr->art_root, (const uint8_t *)key,
                                (int)strlen(key));
    int rc = shm_set_local_locked(kv, key, val, val_len, old, reserve);
    kv_write_unlock(kv);
    return rc;
}

static int frame_operand_ptr(const char *key, const kvspaceXvalueHead *wire) {
    if (wire->ptr != KVSPACE_XVALUE_PTR_PTR ||
        strncmp(key, "/vthread/", 9) != 0)
        return 0;
    const char *slot = strrchr(key, '/');
    if (!slot || slot[1] != '[')
        return 0;
    char *end;
    long row = strtol(slot + 2, &end, 10);
    if (row <= 0 || *end++ != ',')
        return 0;
    long col = strtol(end, &end, 10);
    return col != 0 && *end++ == ']' && *end == '\0';
}

static int shm_set_validated_locked(kvspace_t *kv, const char *key,
                                    const uint8_t *val, int32_t val_len,
                                    const kvspaceXvalueHead *wire) {
    char kbuf[1024];
    if (strlen(key) >= sizeof kbuf || strstr(key, "//") ||
        reserved_meta_path(key))
        return -1;
    strcpy(kbuf, key);

    art_hdr_t *local = art_search(kv, kv->hdr->art_root,
                                   (const uint8_t *)kbuf, (int)strlen(kbuf));
    /* Existing local values already shadow the extension. */
    uint64_t lhv = local ? art_val_load(local) : 0;
    if (!local || !AV_HAS(lhv)) {
        char *pp = NULL, *nn = NULL;
        psplit(kbuf, &pp, &nn);
        char extpath[1024];
        if (pp && nn && dir_ext_path(kv, pp, extpath, sizeof extpath)) {
            char *tgt = pjoin(extpath, nn);
            art_hdr_t *eh = art_search(kv, kv->hdr->art_root,
                                       (const uint8_t *)tgt, (int)strlen(tgt));
            free(tgt);
            if (eh && AV_HAS(art_val_load(eh)) &&
                !frame_operand_ptr(kbuf, wire)) {
                free(pp);
                free(nn);
                return -1;
            }
        }
        free(pp);
        free(nn);
    }

    /* status slot: fixed shape (SLACK, len <= 16) and physical capacity >= minimum.
     * langtype ([n]char/utf8) lives in the inline attr region and is not checked here. */
    if (is_vthread_status_key(kbuf)) {
        if (wire->storetype != KVSPACE_XVALUE_ST_SLACK ||
            kvspaceXvalueHeadLen(wire) > KVSPACE_STATUS_MAX_LEN)
            return -1;
        if (local && AV_HAS(lhv)) {
            uint8_t *old_raw = kv->sbo_data + AV_OFF(lhv);
            uint64_t old_sz = sbo_allocated_size(kv->sbo_meta, AV_OFF(lhv));
            kvspaceXvalueHead old;
            /* 旧 store 的不合规 status 槽（形状/容量任一不符）：
             * 不自动升级、不 realloc——写入硬错，需重建/重置。 */
            if (head_of(old_raw, (int32_t)old_sz, &old) != 0 ||
                old.storetype != KVSPACE_XVALUE_ST_SLACK ||
                kvspaceXvalueHeadLen(&old) > KVSPACE_STATUS_MAX_LEN ||
                old_sz < KVSPACE_STATUS_MIN_CAP)
                return -1;
        }
    }
    return shm_set_local_locked(kv, kbuf, val, val_len, local, 0);
}

static int shm_set_validated(kvspace_t *kv, const char *key,
                            const uint8_t *val, int32_t val_len,
                            const kvspaceXvalueHead *wire) {
    kv_write_lock(kv);
    if (kv_sync(kv) != 0) {
        kv_write_unlock(kv);
        return -1;
    }
    int rc = shm_set_validated_locked(kv, key, val, val_len, wire);
    kv_write_unlock(kv);
    return rc;
}

int kvspaceShmSet(kvspace_t *kv, const char *key, const uint8_t *val,
                  int32_t val_len) {
    if (!kv || !key || !val || val_len <= 0)
        return -1;
    kvspaceXvalueHead wire;
    if (value_ok(val, val_len, &wire) != 0)
        return -1;
    return shm_set_validated(kv, key, val, val_len, &wire);
}

int kvspaceShmSetValue(kvspace_t *kv, const char *key, const uint8_t *val,
                       int32_t val_len) {
    kvspaceXvalueHead head;
    if (!kv || !key || !val || val_len <= 0 ||
        value_ok(val, val_len, &head) != 0)
        return -2;
    return shm_set_validated(kv, key, val, val_len, &head);
}

int kvspaceShmSetValueByRef(kvspace_t *kv, kvspaceRef_t *ref,
                            const char *key_fallback, const uint8_t *val,
                            int32_t val_len) {
    kvspaceXvalueHead head;
    if (!kv || !ref || !key_fallback || !val || val_len <= 0 ||
        value_ok(val, val_len, &head) != 0)
        return -2;
    if (strlen(key_fallback) >= 1024 || strstr(key_fallback, "//") ||
        reserved_meta_path(key_fallback))
        return -1;
    kv_write_lock(kv);
    if (kv_sync(kv) != 0) {
        kv_write_unlock(kv);
        return -1;
    }
    int32_t id = ref_leaf(kv, ref, key_fallback);
    art_hdr_t *h = id >= 0 ? art_hdr(kv, id) : NULL;
    if (h && AV_HAS(art_val_load(h))) {
        int rc = shm_set_local_locked(kv, key_fallback, val, val_len, h, 0);
        kv_write_unlock(kv);
        return rc;
    }
    /* ref 过期或尚无值：退回完整按 key 写（含 extindex 检查），成功后重解析。 */
    int rc = shm_set_validated_locked(kv, key_fallback, val, val_len, &head);
    kv_write_unlock(kv);
    if (rc == 0)
        kvspaceShmResolveRef(kv, key_fallback, ref);
    return rc;
}

int kvspaceShmWriteInPlace(kvspace_t *kv, const char *key, int resolve,
                           int32_t body_len, uint8_t **body) {
    if (!kv || !key || !body || body_len < 0 || reserved_meta_path(key))
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    char kbuf[1024];
    if (strlen(key) >= sizeof kbuf)
        return -1;                  /* reject rather than truncating to another key */
    uint8_t *raw;
    int32_t rl;
    int fetched = 0;
    art_hdr_t *h;
    if (resolve)
        h = resolve_fetch(kv, key, kbuf, sizeof(kbuf), &raw, &rl, &fetched);
    else {
        strcpy(kbuf, key);
        h = art_search(kv, kv->hdr->art_root, (const uint8_t *)kbuf,
                       (int)strlen(kbuf));
    }
    if (!h)
        return -1;
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return -1;
    /* Re-bound by physical box capacity before returning writable pointer. */
    (void)fetched;
    if (read_value(kv, AV_OFF(hv), &raw, &rl) < 0 || rl <= 0)
        return -1; /* None or read failure -> require WriteNewPlace */
    kvspaceXvalueHead wire;
    if (head_of(raw, rl, &wire) == 0) {
        if (kvspaceXvalueHeadLen(&wire) != (uint64_t)body_len)
            return -1;
        *body = raw + KVSPACE_XVALUE_HEAD_LEN;
        return 0;
    }
    return -1;
}

int kvspaceShmWriteNewPlace(kvspace_t *kv, const char *key, uint8_t ref,
                            uint8_t storetype, int32_t body_len,
                            uint64_t body_cap, const char *langtype,
                            uint8_t **body) {
    if (!kv || !key || !body || body_len < 0)
        return -1;
    if (body_cap > INT32_MAX - 32)
        return -1;
    *body = NULL;
    if (kv_sync(kv) != 0)
        return -1;
    size_t key_len = strlen(key);
    if (key_len >= 1024 || strstr(key, "//") || reserved_meta_path(key))
        return -1;
    uint8_t ptr;
    if (ref == 0 && storetype <= KVSPACE_XVALUE_ST_EXT)
        ptr = KVSPACE_XVALUE_PTR_INLINE;
    else if (ref == 1 && storetype == KVSPACE_XVALUE_ST_SLACK)
        ptr = KVSPACE_XVALUE_PTR_PTR;
    else if (ref == 2 && storetype == KVSPACE_XVALUE_ST_EXT)
        ptr = KVSPACE_XVALUE_PTR_EXT;
    else
        return -1;
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXvalueHeadReserve(storetype, (uint64_t)body_len,
                                 body_cap, langtype, &value, &total) != 0)
        return -1;
    if (total > INT32_MAX) {
        free(value);
        return -1;
    }
    /* Reserve produces an inline header; patch ptr byte for ref=1/2. */
    kvspaceXvalueHead hd;
    memcpy(&hd, value, sizeof hd);
    hd.ptr = ptr;
    memcpy(value, &hd, sizeof hd);
    /* Fixed 8-byte header (body offset = 8) + attr at 8+body_len in a single write.
     * Reserve physical box capacity (8 + body_cap + attr) so body can grow in place. */
    uint64_t attr_len = total - KVSPACE_XVALUE_HEAD_LEN - (uint64_t)body_len;
    size_t reserve = (size_t)(KVSPACE_XVALUE_HEAD_LEN + body_cap + attr_len);
    int rc = shm_set_raw(kv, key, value, (int32_t)total, reserve);
    free(value);
    if (rc != 0)
        return -1;
    art_hdr_t *node = art_search(kv, kv->hdr->art_root,
                                 (const uint8_t *)key, (int)key_len);
    if (!node)
        return -1;
    uint64_t nv = art_val_load(node);
    if (!AV_HAS(nv))
        return -1;
    *body = kv->sbo_data + AV_OFF(nv) + KVSPACE_XVALUE_HEAD_LEN;
    return 0;
}

/* Delete a single key directly (without following links) and without bumping
 * list_gen. Returns 1 = deleted, 0 = missing, -1 = hard error (tree unchanged).
 * Caller has called kv_sync, verified strlen(key) < 1024, and holds kv_write_lock(kv). */
static int shm_del_key_locked(kvspace_t *kv, const char *key) {
    bool d = false;
    int32_t root = art_del(kv, kv->hdr->art_root, (const uint8_t *)key,
                           (int)strlen(key), 0, &d);
    if (root < 0)
        return -1;
    __atomic_store_n(&kv->hdr->art_root, root, __ATOMIC_RELEASE);
    return d ? 1 : 0;
}

int kvspaceShmDel(kvspace_t *kv, const char *key) {
    if (!kv || !key)
        return -1;
    if (strlen(key) >= 1024)
        return -1;                  /* Reject instead of truncating into another key */
    kv_write_lock(kv);
    if (kv_sync(kv) != 0) {
        kv_write_unlock(kv);
        return -1;
    }
    /* Delete the key itself without following links; explicit deref belongs to runtime */
    int rc = shm_del_key_locked(kv, key);
    kv_write_unlock(kv);
    if (rc != 1)
        return -1;
    list_gen_bump(kv);              /* Key disappeared: bump after CAS */
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

/* Whether key belongs to base's subtree: base itself, or base followed by '/'
 * (hierarchy), U+00B7 (member), or U+2025 (runtime/attr key). Empty base = root
 * (all keys starting with '/'). Matches kvspace-durable store::is_subtree_key. */
static int subtree_key(const char *base, size_t bl, const char *key) {
    if (bl == 0)
        return key[0] == '/';
    if (strncmp(key, base, bl) != 0)
        return 0;
    const unsigned char *r = (const unsigned char *)key + bl;
    return r[0] == 0 || r[0] == '/' || (r[0] == 0xC2 && r[1] == 0xB7) ||
           (r[0] == 0xE2 && r[1] == 0x80 && r[2] == 0xA5);
}

static int is_ptr_key(kvspace_t *kv, const char *key) {
    art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)key,
                              (int)strlen(key));
    uint64_t hv = h ? art_val_load(h) : 0;
    if (!h || !AV_HAS(hv))
        return 0;
    uint8_t *raw = NULL;
    int32_t rl = 0;
    kvspaceXvalueHead wire;
    return read_value(kv, AV_OFF(hv), &raw, &rl) == 0 && rl > 0 &&
           head_of(raw, rl, &wire) == 0 && wire.ptr == KVSPACE_XVALUE_PTR_PTR;
}

/* Delete the entire subtree under prefix. A single trailing '/' or U+00B7 on
 * prefix normalizes to base ("/d/" and "/d<U+00B7>" both delete "/d"; "/" is root).
 * Deletes base, base/..., base<U+00B7>..., and base<U+2025>... (including hidden
 * U+2025 runtime keys). Does not follow links (POSIX rm semantics): if prefix
 * or base is a pointer, only the pointer is deleted. */
int kvspaceShmDeltree(kvspace_t *kv, const char *prefix) {
    if (!kv || !prefix || reserved_meta_path(prefix))
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    size_t pl = strlen(prefix);
    if (pl >= 1024)
        return -1;                  /* Reject instead of truncating into another prefix */
    char base[1024];
    memcpy(base, prefix, pl + 1);
    size_t bl = pl;
    if (strcmp(prefix, "/") == 0)
        bl = 0;
    else if (bl && base[bl - 1] == '/')
        bl--;
    else if (bl >= 2 && (unsigned char)base[bl - 2] == 0xC2 &&
             (unsigned char)base[bl - 1] == 0xB7)
        bl -= 2;
    base[bl] = 0;
    if (bl && reserved_meta_path(base))
        return -1;
    /* prefix itself is a link: delete only the link. */
    if (pl && is_ptr_key(kv, prefix))
        return kvspaceShmDel(kv, prefix);
    if (bl && bl != pl && is_ptr_key(kv, base)) {
        int rc = kvspaceShmDel(kv, base);
        kvspaceShmDel(kv, prefix);  /* Companion key such as "/p/", may not exist */
        return rc;
    }
    keyscan_t keys = {0};
    if (art_scan_keys(kv, bl ? base : "/", bl ? (int)bl : 1, &keys) != 0) {
        keyscan_free(&keys);
        return -1;
    }
    int rc = 0, any = 0;
    kv_write_lock(kv);
    if (kv_sync(kv) != 0) {
        kv_write_unlock(kv);
        keyscan_free(&keys);
        return -1;
    }
    for (int32_t i = 0; i < keys.count; i++) {
        const char *k = keys.keys[i];
        if (!subtree_key(base, bl, k) || reserved_meta_path(k) ||
            strlen(k) >= 1024)
            continue;
        int d = shm_del_key_locked(kv, k);
        if (d < 0)
            rc = -1;
        else if (d)
            any = 1;
    }
    kv_write_unlock(kv);
    keyscan_free(&keys);
    if (any)
        list_gen_bump(kv);          /* Keys disappeared: bump once after batch delete */
    return rc;
}

/* Copy one value before Set can move its slab. */
int kvspaceShmCp(kvspace_t *kv, const char *src, const char *dst) {
    if (!kv || !src || !dst)
        return -1;
    int32_t rl;
    uint8_t *raw = kvspaceShmGet(kv, src, 0, &rl);
    if (!raw || rl <= 0)
        return -1;
    uint8_t *tmp = malloc((size_t)rl);
    if (!tmp)
        return -1;
    memcpy(tmp, raw, (size_t)rl);
    int rc = kvspaceShmSet(kv, dst, tmp, rl);
    free(tmp);
    return rc;
}

/* Copy values and physical children recursively. */
static int cptree_rec(kvspace_t *kv, const char *src, const char *dst) {
    {
        int32_t rl;
        uint8_t *raw = kvspaceShmGet(kv, src, 0, &rl);
        if (raw && rl > 0) {
            if (kvspaceShmCp(kv, src, dst) != 0)
                return -1;
        }
    }
    char *es = edir(src), *ed = edir(dst);
    char **ns;
    int32_t nc;
    if (kvspaceShmList(kv, es, false, 1, &ns, &nc) != 0) {
        free(es);
        free(ed);
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < nc; i++) {
        char *cs = pjoin(es, ns[i]), *cd = pjoin(ed, ns[i]);
        if (!cs || !cd || cptree_rec(kv, cs, cd) != 0)
            rc = -1;
        free(cs);
        free(cd);
        if (rc != 0)
            break;
    }
    for (int i = 0; i < nc; i++)
        free(ns[i]);
    free(ns);
    free(es);
    free(ed);
    if (rc != 0)
        return -1;

    char *ms = memdir(src), *md = memdir(dst);
    char **mms;
    int32_t mc;
    if (kvspaceShmList(kv, ms, false, 1, &mms, &mc) != 0) {
        free(ms);
        free(md);
        return -1;
    }
    for (int i = 0; i < mc; i++) {
        size_t msl = strlen(ms), mdl = strlen(md), nl = strlen(mms[i]);
        char *cs = malloc(msl + nl + 1);
        char *cd = malloc(mdl + nl + 1);
        if (!cs || !cd) {
            rc = -1;
        } else {
            memcpy(cs, ms, msl);
            memcpy(cs + msl, mms[i], nl + 1);
            memcpy(cd, md, mdl);
            memcpy(cd + mdl, mms[i], nl + 1);
            if (cptree_rec(kv, cs, cd) != 0)
                rc = -1;
        }
        free(cs);
        free(cd);
        if (rc != 0)
            break;
    }
    for (int i = 0; i < mc; i++)
        free(mms[i]);
    free(mms);
    free(ms);
    free(md);
    return rc;
}

int kvspaceShmCptree(kvspace_t *kv, const char *src, const char *dst) {
    if (!kv || !src || !dst || reserved_meta_path(src) ||
        reserved_meta_path(dst))
        return -1;
    size_t sl = strlen(src);
    while (sl > 1 && src[sl - 1] == '/')
        sl--;
    if (sl == 1 && src[0] == '/' && dst[0] == '/')
        return dst[1] ? -1 : 0;
    if (strncmp(src, dst, sl) == 0) {
        if (dst[sl] == 0 || (dst[sl] == '/' && dst[sl + 1] == 0))
            return 0;
        if (dst[sl] == '/' ||
            ((unsigned char)dst[sl] == 0xc2 &&
             (unsigned char)dst[sl + 1] == 0xb7))
            return -1;
    }
    kvspaceShmDeltree(kv, dst);
    return cptree_rec(kv, src, dst);
}

/* Copy the base value and direct member values. */
int kvspaceShmCplist(kvspace_t *kv, const char *src, const char *dst) {
    if (!kv || !src || !dst || reserved_meta_path(src) ||
        reserved_meta_path(dst))
        return -1;
    if (strcmp(src, dst) == 0)
        return 0;
    kvspaceShmDeltree(kv, dst);
    int32_t rl;
    uint8_t *raw = kvspaceShmGet(kv, src, 0, &rl);
    if (raw && rl > 0 && kvspaceShmCp(kv, src, dst) != 0)
        return -1;
    char *ms = memdir(src), *md = memdir(dst);
    char **mms;
    int32_t mc;
    if (kvspaceShmList(kv, ms, false, 1, &mms, &mc) != 0) {
        free(ms);
        free(md);
        return -1;
    }
    size_t msl = strlen(ms), mdl = strlen(md);
    int rc = 0;
    for (int i = 0; i < mc; i++) {
        size_t nl = strlen(mms[i]);
        char *cs = malloc(msl + nl + 1);
        char *cd = malloc(mdl + nl + 1);
        if (!cs || !cd) {
            rc = -1;
        } else {
            memcpy(cs, ms, msl);
            memcpy(cs + msl, mms[i], nl + 1);
            memcpy(cd, md, mdl);
            memcpy(cd + mdl, mms[i], nl + 1);
            if (kvspaceShmCp(kv, cs, cd) != 0)
                rc = -1;
        }
        free(cs);
        free(cd);
        if (rc != 0)
            break;
    }
    for (int i = 0; i < mc; i++)
        free(mms[i]);
    free(mms);
    free(ms);
    free(md);
    return rc;
}

int kvspaceShmMkindex(kvspace_t *kv, const char *path, uint32_t capacity) {
    if (!kv || !path)
        return -1;
    (void)capacity;
    char *d = edir(path);
    int32_t existing_len = 0;
    if (kvspaceShmGet(kv, d, 0, &existing_len)) {
        free(d);
        return 0;
    }
    uint8_t *v = NULL;
    uint64_t vl = 0;
    int r = kvspaceXvalueHeadNewNone(&v, &vl);
    if (r == 0)
        r = kvspaceShmSetValue(kv, d, v, (int32_t)vl);
    free(v);
    free(d);
    return r;
}

static int child_cmp(const void *a, const void *b) {
    const char *x = *(const char *const *)a;
    const char *y = *(const char *const *)b;
    size_t nx = strlen(x), ny = strlen(y);
    char *tx = nx && x[nx - 1] == '/' ? strndup(x, nx - 1) : NULL;
    char *ty = ny && y[ny - 1] == '/' ? strndup(y, ny - 1) : NULL;
    int c = kvspaceCoordCmp(tx ? tx : x, ty ? ty : y);
    free(tx);
    free(ty);
    return c ? c : strcmp(x, y);
}

static int same_child(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    if (na && a[na - 1] == '/') na--;
    if (nb && b[nb - 1] == '/') nb--;
    return na == nb && memcmp(a, b, na) == 0;
}

/* Resolve List input: prefix -> target directory (following links when resolve=1)
 * and mounted extindex source ext (when ex=1 and mounted; "" otherwise).
 * Returns 0 = scan, 1 = reserved path (empty result), -1 = error.
 * target buffer >= 4096 bytes, ext buffer >= 1024 bytes. */
static int list_target(kvspace_t *kv, const char *prefix, bool ex, int resolve,
                       char *target, size_t tcap, char *ext, size_t ecap) {
    ext[0] = 0;
    if (reserved_meta_path(prefix))
        return 1;
    if (kv_sync(kv) != 0 || bad_dir_prefix(prefix))
        return -1;
    if (resolve) {
        if (strlen(prefix) >= 1024)
            return -1;              /* Reject instead of truncating into another dir */
        resolve_path(kv, prefix, target, 1024);
    } else {
        size_t l = strlen(prefix);
        if (l >= tcap)
            return -1;
        memcpy(target, prefix, l + 1);
    }
    if (reserved_meta_path(target))
        return 1;
    if (ex) {
        char *dir = edir(target);
        if (!dir)
            return -1;
        if (!dir_ext_path(kv, dir, ext, (int)ecap))
            ext[0] = 0;
        free(dir);
    }
    return 0;
}
/* Scan direct children of target (and mounted ext): sort, deduplicate (prefer
 * directory entry on name collision), and hide runtime entries. */
static int list_scan(kvspace_t *kv, const char *pfx, const char *ext,
                     char ***on, int32_t *oc) {
    keyscan_t children = {0};
    char buf[4096];
    if (kv->hdr->art_root >= 0 &&
        art_scan_pfx(kv, pfx, (int)strlen(pfx), buf, (int)sizeof buf, &children) != 0)
        goto fail;
    if (ext[0] && art_scan_pfx(kv, ext, (int)strlen(ext), buf, (int)sizeof buf,
                               &children) != 0)
        goto fail;
    if (children.count > 1)
        qsort(children.keys, (size_t)children.count, sizeof *children.keys,
              child_cmp);
    int32_t unique = 0;
    for (int32_t i = 0; i < children.count; i++) {
        if (strcmp(pfx, "/") == 0 &&
            strcmp(children.keys[i], ".kvspace-meta") == 0) {
            free(children.keys[i]);
            continue;
        }
        /* Child names containing U+2025 (\xE2\x80\xA5) are runtime system keys
           (e.g. status, pc); hide from List to match the durable backend. */
        if (strstr(children.keys[i], "\xE2\x80\xA5")) {
            free(children.keys[i]);
            continue;
        }
        if (unique && same_child(children.keys[unique - 1], children.keys[i])) {
            if (children.keys[i][strlen(children.keys[i]) - 1] == '/') {
                free(children.keys[unique - 1]);
                children.keys[unique - 1] = children.keys[i];
                continue;
            }
            free(children.keys[i]);
            continue;
        }
        children.keys[unique++] = children.keys[i];
    }
    children.count = unique;
    *on = children.keys;
    *oc = children.count;
    return 0;
fail:
    keyscan_free(&children);
    return -1;
}
int kvspaceShmList(kvspace_t *kv, const char *prefix, bool ex, int resolve,
                   char ***on, int32_t *oc) {
    if (!kv || !prefix || !on || !oc)
        return -1;
    *on = NULL;
    *oc = 0;
    char target[4096], ext[1024];
    int rc = list_target(kv, prefix, ex, resolve, target, sizeof target, ext,
                         sizeof ext);
    if (rc != 0)
        return rc > 0 ? 0 : -1;
    return list_scan(kv, target, ext, on, oc);
}
/* Caller holds lc_mtx (or handle is unshared, e.g. Close). */
static void list_cache_drop(kvspace_t *kv) {
    for (int32_t i = 0; i < kv->lc_count; i++)
        free(kv->lc_names[i]);
    free(kv->lc_names);
    free(kv->lc_prefix);
    free(kv->lc_target);
    free(kv->lc_ext);
    kv->lc_names = NULL;
    kv->lc_prefix = kv->lc_target = kv->lc_ext = NULL;
    kv->lc_count = 0;
}
/* Cached name list for ListLen/ListAt; caller holds lc_mtx. On return 0,
 * kv->lc_names/lc_count hold the result. A hit requires matching (prefix, ex,
 * resolve), unchanged resolved target/ext, and unchanged list_gen (read before
 * resolve/scan so any concurrent key-set mutation forces a rescan next time). */
static int list_cached(kvspace_t *kv, const char *prefix, bool ex, int resolve) {
    uint64_t g = __atomic_load_n(&kv->hdr->list_gen, __ATOMIC_ACQUIRE);
    char target[4096], ext[1024];
    int rc = list_target(kv, prefix, ex, resolve, target, sizeof target, ext,
                         sizeof ext);
    if (rc < 0)
        return -1;
    if (rc > 0) {
        list_cache_drop(kv);
        return 0;
    }
    if (kv->lc_prefix && kv->lc_gen == g && kv->lc_ex == (uint8_t)ex &&
        kv->lc_res == (uint8_t)(resolve != 0) &&
        strcmp(kv->lc_prefix, prefix) == 0 &&
        strcmp(kv->lc_target, target) == 0 && strcmp(kv->lc_ext, ext) == 0)
        return 0;
    list_cache_drop(kv);
    char **names = NULL;
    int32_t count = 0;
    if (list_scan(kv, target, ext, &names, &count) != 0)
        return -1;
    char *p = strdup(prefix), *t = strdup(target), *e = strdup(ext);
    if (!p || !t || !e) {
        free(p);
        free(t);
        free(e);
        for (int32_t i = 0; i < count; i++)
            free(names[i]);
        free(names);
        return -1;
    }
    kv->lc_prefix = p;
    kv->lc_target = t;
    kv->lc_ext = e;
    kv->lc_names = names;
    kv->lc_count = count;
    kv->lc_ex = (uint8_t)ex;
    kv->lc_res = (uint8_t)(resolve != 0);
    kv->lc_gen = g;
    return 0;
}
int kvspaceShmListLen(kvspace_t *kv, const char *prefix, bool ex, int resolve,
                      int32_t *out_count) {
    if (!out_count)
        return -1;
    *out_count = 0;
    if (!kv || !prefix)
        return -1;
    pthread_mutex_lock(&kv->lc_mtx);
    int rc = list_cached(kv, prefix, ex, resolve);
    if (rc == 0)
        *out_count = kv->lc_count;
    pthread_mutex_unlock(&kv->lc_mtx);
    return rc;
}
/* Copy the idx-th direct child name into buf (without NUL). *out_len is set to
 * the name length; returns non-zero if out of range or buf is too small. Shares
 * cache with ListLen so ListLen + indexed ListAt scans the directory once. */
int kvspaceShmListAt(kvspace_t *kv, const char *prefix, bool ex, int resolve,
                     int32_t idx, uint8_t *buf, uint32_t buf_cap,
                     uint32_t *out_len) {
    if (!out_len)
        return -1;
    *out_len = 0;
    if (!kv || !prefix)
        return -1;
    pthread_mutex_lock(&kv->lc_mtx);
    int rc = list_cached(kv, prefix, ex, resolve);
    if (rc == 0) {
        rc = -1;
        if (idx >= 0 && idx < kv->lc_count) {
            size_t l = strlen(kv->lc_names[idx]);
            *out_len = (uint32_t)l;
            if (buf && l <= buf_cap) {
                memcpy(buf, kv->lc_names[idx], l);
                rc = 0;
            }
        }
    }
    pthread_mutex_unlock(&kv->lc_mtx);
    return rc;
}
int kvspaceShmExtindex(kvspace_t *kv, const char *p, const char *ep,
                       char *err, uint32_t err_cap) {
    if (!kv || !p || !ep)
        return -1;
    /* Match durable ext_index: path and ext_path must be directory keys ('/' or U+00B7 suffix). */
    if (bad_dir_prefix(p) || bad_dir_prefix(ep)) {
        if (err && err_cap)
            snprintf(err, err_cap,
                     "kvspace: index must end with /: ExtIndex path=%s extpath=%s",
                     p, ep);
        return -1;
    }
    int32_t source_len = 0;
    uint8_t *source = kvspaceShmGet(kv, ep, 0, &source_len);
    if (!source || source_len <= 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "ExtIndex target missing: %s", ep);
        return -1;
    }
    /* source 类型在 ep 值 attr 区（明文事实源）；ext 句柄同样携带。 */
    char lt[256];
    const uint8_t *ltd = NULL;
    uint16_t ltl = 0;
    if (source_len >= (int32_t)KVSPACE_XVALUE_HEAD_LEN) {
        kvspaceXvalueHead sh;
        memcpy(&sh, source, sizeof sh);
        if (sh.storetype <= KVSPACE_XVALUE_ST_EXT &&
            sh.ptr <= KVSPACE_XVALUE_PTR_EXT) {
            uint64_t sbody = kvspaceXvalueHeadLen(&sh);
            if (sbody <= (uint64_t)source_len - KVSPACE_XVALUE_HEAD_LEN)
                ltl = kvspaceXvalueHeadLangtype(source, sbody, &ltd);
        }
    }
    if (ltl > (uint16_t)(sizeof lt - 1))
        ltl = (uint16_t)(sizeof lt - 1);
    if (ltl)
        memcpy(lt, ltd, ltl);
    lt[ltl] = 0;
    uint8_t *v = NULL;
    uint64_t vl = 0;
    int r = kvspaceXvalueHeadNewExt((const uint8_t *)ep, (uint32_t)strlen(ep),
                                    lt, &v, &vl);
    if (r == 0)
        r = kvspaceShmSetValue(kv, p, v, (int32_t)vl);
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
