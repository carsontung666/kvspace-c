#include "xvalue.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 对齐 kvspace-durable 的 kindexp TLV 编解码。 */

static void wr_u8(uint8_t *dst, uint8_t v) { dst[0] = v; }
static void wr_u16(uint8_t *dst, uint16_t v) {
    dst[0] = (uint8_t)v;
    dst[1] = (uint8_t)(v >> 8);
}
static void wr_u32(uint8_t *dst, uint32_t v) {
    for (int i = 0; i < 4; i++)
        dst[i] = (uint8_t)(v >> (i * 8));
}
static void wr_u64(uint8_t *dst, uint64_t v) {
    for (int i = 0; i < 8; i++)
        dst[i] = (uint8_t)(v >> (i * 8));
}
static void wr_i8(uint8_t *dst, int8_t v) { wr_u8(dst, (uint8_t)v); }
static void wr_i16(uint8_t *dst, int16_t v) { wr_u16(dst, (uint16_t)v); }
static void wr_i32(uint8_t *dst, int32_t v) { wr_u32(dst, (uint32_t)v); }
static void wr_i64(uint8_t *dst, int64_t v) { wr_u64(dst, (uint64_t)v); }

static uint16_t rd_u16(const uint8_t *r) {
    return (uint16_t)((uint16_t)r[0] | ((uint16_t)r[1] << 8));
}
static uint32_t rd_u32(const uint8_t *r) {
    return (uint32_t)r[0] | ((uint32_t)r[1] << 8) | ((uint32_t)r[2] << 16) |
           ((uint32_t)r[3] << 24);
}

static int32_t header_array_len(int32_t ndim, const int32_t *dims) {
    if (ndim <= 0)
        return 1;
    int32_t n = 1;
    for (int i = 0; i < ndim; i++)
        n *= dims[i];
    return n;
}

#define X_HEAD_PREFIX \
    13 /* headlen(2)+ref(1)+storetype(1)+ro(1)+vid(4)+body_len(4) */

/* ARRAYND / index / extindex 携带 ndim+dims 物理字段。 */
static int store_has_dims(uint8_t st) {
    return st == KVSPACE_STORETYPE_ARRAYND || st == KVSPACE_STORETYPE_INDEX ||
           st == KVSPACE_STORETYPE_EXTINDEX;
}

int32_t kvspaceXvalueHeadLen(const xvalue_head_t *h) { return h->headlen; }

int32_t kvspaceXvalueHeadLenForLangtype(uint8_t storetype, const char *langtype,
                                        int32_t ndim) {
    if (!store_has_dims(storetype))
        ndim = 0;
    int32_t lt = langtype ? (int32_t)strlen(langtype) : 0;
    int32_t phys = store_has_dims(storetype) ? (1 + 4 * ndim) : 0;
    return X_HEAD_PREFIX + phys + lt;
}

void kvspaceXvalueWriteHead(uint8_t *dst, uint8_t ref, uint8_t storetype,
                            uint8_t ro, uint32_t vid, const char *langtype,
                            const int32_t *dims, int32_t ndim,
                            int32_t body_len) {
    if (!store_has_dims(storetype))
        ndim = 0;
    int32_t lt = langtype ? (int32_t)strlen(langtype) : 0;
    int32_t phys = store_has_dims(storetype) ? (1 + 4 * ndim) : 0;
    int32_t headlen = X_HEAD_PREFIX + phys + lt;
    wr_u16(dst, (uint16_t)headlen);
    dst[2] = ref;
    dst[3] = storetype;
    dst[4] = ro;                         /* ro */
    wr_u32(dst + 5, vid);                /* vid */
    wr_u32(dst + 9, (uint32_t)body_len); /* body_len */
    int32_t o = X_HEAD_PREFIX;
    if (store_has_dims(storetype)) {
        dst[o++] = (uint8_t)ndim;
        for (int i = 0; i < ndim; i++) {
            wr_u32(dst + o, (uint32_t)dims[i]);
            o += 4;
        }
    }
    if (lt > 0)
        memcpy(dst + o, langtype, (size_t)lt);
}

/* 由 base 种类名（+ndim）推 storetype——便利编码函数用；WriteNewPlace 直接收
 * storetype。 */
static int is_index_kind(const char *kind) {
    return strcmp(kind, KVSPACE_KIND_INDEX) == 0 ||
           strcmp(kind, KVSPACE_KIND_EXT_INDEX) == 0 ||
           strcmp(kind, KVSPACE_KIND_RWFUNC) == 0 ||
           strcmp(kind, KVSPACE_KIND_DEF_RWIR) == 0;
}
static uint8_t storetype_of(const char *kind, int32_t ndim) {
    if (!kind || !kind[0])
        return KVSPACE_STORETYPE_NONE;
    if (strcmp(kind, KVSPACE_KIND_EXT_INDEX) == 0)
        return KVSPACE_STORETYPE_EXTINDEX;
    if (is_index_kind(kind))
        return KVSPACE_STORETYPE_INDEX;
    if (ndim > 0)
        return KVSPACE_STORETYPE_ARRAYND;
    return KVSPACE_STORETYPE_ATOM;
}

/* 指针 head 的 storetype = 目标语义 storetype（据目标完整 kindexpr
 * 推；指针自身物理字段恒空）。 */
static uint8_t storetype_from_kindexpr(const char *kx) {
    if (!kx || !kx[0])
        return KVSPACE_STORETYPE_NONE;
    const char *base = kx;
    int has_dims = 0;
    if (*base == '[') {
        const char *p = strchr(base, ']');
        if (p) {
            base = p + 1;
            has_dims = 1;
        }
    }
    if (strcmp(base, KVSPACE_KIND_EXT_INDEX) == 0)
        return KVSPACE_STORETYPE_EXTINDEX;
    if (is_index_kind(base) || base[0] == '/' ||
        strstr(base, "\xC2\xB7")) /* · = U+00B7 */
        return KVSPACE_STORETYPE_INDEX;
    if (has_dims)
        return KVSPACE_STORETYPE_ARRAYND;
    return KVSPACE_STORETYPE_ATOM;
}

/* langtype 串（ARRAYND 含 [dims]，其余为裸种类名/路径）。 */
static int32_t build_langtype(char *buf, int32_t cap, const char *kind,
                              uint8_t storetype, const int32_t *dims,
                              int32_t ndim) {
    int32_t o = 0;
    if (storetype == KVSPACE_STORETYPE_ARRAYND && ndim > 0) {
        buf[o++] = '[';
        for (int i = 0; i < ndim; i++) {
            if (i > 0)
                buf[o++] = ',';
            o += snprintf(buf + o, (size_t)(cap - o), "%d", dims[i]);
        }
        buf[o++] = ']';
    }
    int32_t kl = (int32_t)strlen(kind);
    memcpy(buf + o, kind, (size_t)kl);
    return o + kl;
}

/* 核心编码：写三轴 head + body。dims 仅在 store_has_dims(storetype)
 * 时落物理字段。 */
static int32_t encode_head(uint8_t ref, uint8_t storetype, const char *langtype,
                           int32_t ro, uint32_t vid, const int32_t *dims,
                           int32_t ndim, const uint8_t *raw, int32_t raw_len,
                           uint8_t **out) {
    if (!store_has_dims(storetype))
        ndim = 0;
    int32_t lt = langtype ? (int32_t)strlen(langtype) : 0;
    int32_t phys = store_has_dims(storetype) ? (1 + 4 * ndim) : 0;
    int32_t headlen = X_HEAD_PREFIX + phys + lt;
    int32_t total = headlen + raw_len;
    uint8_t *buf = (uint8_t *)malloc((size_t)total);
    if (!buf)
        return -1;
    wr_u16(buf, (uint16_t)headlen);
    buf[2] = ref;
    buf[3] = storetype;
    buf[4] = (uint8_t)(ro ? 1 : 0);
    wr_u32(buf + 5, vid);
    wr_u32(buf + 9, (uint32_t)raw_len);
    int32_t o = X_HEAD_PREFIX;
    if (store_has_dims(storetype)) {
        buf[o++] = (uint8_t)ndim;
        for (int i = 0; i < ndim; i++) {
            wr_u32(buf + o, (uint32_t)dims[i]);
            o += 4;
        }
    }
    if (lt > 0)
        memcpy(buf + o, langtype, (size_t)lt);
    if (raw_len > 0 && raw)
        memcpy(buf + headlen, raw, (size_t)raw_len);
    *out = buf;
    return total;
}

int32_t kvspaceXvalueEncode(const char *kind, const uint8_t *raw,
                            int32_t raw_len, const int32_t *dims, int32_t ndim,
                            uint8_t **out) {
    return kvspaceXvalueEncodeMode(kind, raw, raw_len, dims, ndim, 0, 0, 0, out);
}

int32_t kvspaceXvalueEncodeMode(const char *kind, const uint8_t *raw,
                                int32_t raw_len, const int32_t *dims,
                                int32_t ndim, int32_t ref, int32_t ro,
                                uint32_t vid, uint8_t **out) {
    if (!out || !kind)
        return -1;
    if (ndim < 0)
        ndim = 0;
    if (ndim > X_MAX_NDIM)
        return -1;
    if (raw_len < 0)
        raw_len = 0;
    uint8_t storetype = storetype_of(kind, ndim);
    char lt[256];
    int32_t ltl =
        build_langtype(lt, (int32_t)sizeof lt, kind, storetype, dims, ndim);
    lt[ltl] = 0;
    return encode_head((uint8_t)ref, storetype, lt, ro, vid, dims, ndim, raw,
                       raw_len, out);
}

/* 标量/一维便捷编码（内部）：array_len → dims。char/* 恒一维（含空串/单字符）；
 * 其余标量(≤1)=0 维、多元素=1 维。公开的 kvspaceXvalueEncode 只认 dims/ndim。
 */
static int32_t al_to_dims(const char *kind, int32_t array_len, int32_t *dims) {
    if (strncmp(kind, "char/", 5) == 0) {
        dims[0] = array_len < 0 ? 0 : array_len;
        return 1;
    }
    if (array_len > 1) {
        dims[0] = array_len;
        return 1;
    }
    return 0;
}

static int32_t encode_al(const char *kind, const uint8_t *raw, int32_t raw_len,
                         int32_t array_len, uint8_t **out) {
    int32_t dims[1];
    int32_t nd = al_to_dims(kind, array_len, dims);
    return kvspaceXvalueEncode(kind, raw, raw_len, dims, nd, out);
}

#define KVSPACE_HEAD64 64
static int looks_head64(const uint8_t *d, int32_t n) {
    if (!d || n < KVSPACE_HEAD64)
        return 0;
    if (d[1] > KVSPACE_STORETYPE_EXTINDEX || d[3] > X_MAX_NDIM)
        return 0;
    uint64_t bl = 0, cap = 0;
    for (int i = 0; i < 8; i++) {
        bl |= (uint64_t)d[8 + i] << (i * 8);
        cap |= (uint64_t)d[16 + i] << (i * 8);
    }
    if (bl > cap || (int32_t)KVSPACE_HEAD64 + (int32_t)bl > n)
        return 0;
    return 1;
}

xvalue_head_t kvspaceXvalueDecodeHead(const uint8_t *data, int32_t data_len) {
    xvalue_head_t h = {0};
    if (!data || data_len < X_HEAD_PREFIX)
        return h;
    if (looks_head64(data, data_len) && rd_u16(data) != X_HEAD_PREFIX) {
        uint8_t st = data[1];
        uint8_t ndim = data[3];
        uint64_t bl = 0;
        for (int i = 0; i < 8; i++)
            bl |= (uint64_t)data[8 + i] << (i * 8);
        h.headlen = KVSPACE_HEAD64;
        h.ref = (int32_t)(int8_t)data[0];
        if (h.ref < 0)
            h.ref = KVSPACE_REF_EXT;
        h.storetype = st;
        h.ro = data[2] & 1;
        h.vid = rd_u32(data + 4);
        h.raw_len = (int32_t)bl;
        h.ndim = ndim;
        for (int i = 0; i < ndim && i < X_MAX_NDIM; i++)
            h.dims[i] = (int32_t)rd_u32(data + 28 + i * 4);
        h.langtype = "";
        h.langtype_len = 0;
        h.kind = "";
        h.kind_len = 0;
        h.raw = data + KVSPACE_HEAD64;
        h.array_len = store_has_dims(st) ? header_array_len(h.ndim, h.dims) : 1;
        return h;
    }
    h.headlen = rd_u16(data);
    h.ref = data[2];
    h.storetype = data[3];
    h.ro = data[4] & 0x01;
    h.vid = rd_u32(data + 5);
    h.raw_len = (int32_t)rd_u32(data + 9);
    if ((int32_t)h.headlen < X_HEAD_PREFIX || data_len < (int32_t)h.headlen)
        return h;
    int32_t o = X_HEAD_PREFIX;
    if (store_has_dims(h.storetype)) {
        h.ndim = data[o++];
        if (h.ndim > X_MAX_NDIM)
            h.ndim = X_MAX_NDIM;
        for (int i = 0; i < h.ndim; i++) {
            h.dims[i] = (int32_t)rd_u32(data + o);
            o += 4;
        }
    }
    h.langtype = (const char *)(data + o);
    h.langtype_len = (int32_t)h.headlen - o;
    if (h.langtype_len < 0)
        h.langtype_len = 0;
    /* kind = langtype 越过前导 [dims]（若有）。 */
    int32_t i = 0;
    if (i < h.langtype_len && h.langtype[i] == '[') {
        while (i < h.langtype_len && h.langtype[i] != ']')
            i++;
        if (i < h.langtype_len)
            i++;
    }
    h.kind = h.langtype + i;
    h.kind_len = h.langtype_len - i;
    if (data_len < (int32_t)h.headlen + h.raw_len)
        return h;
    h.raw = data + h.headlen;
    h.array_len =
        store_has_dims(h.storetype) ? header_array_len(h.ndim, h.dims) : 1;
    return h;
}

#define DEF_NEW_ARRAY(name, kind, T, elem_sz, wr_fn)             \
    int32_t kvspaceXvalueNew##name(const T *vals, int32_t count, \
                                   uint8_t **out) {              \
        if (!vals || count <= 0)                                 \
            return -1;                                           \
        int32_t raw_len = count * elem_sz;                       \
        uint8_t *raw = (uint8_t *)malloc((size_t)raw_len);       \
        if (!raw)                                                \
            return -1;                                           \
        for (int32_t i = 0; i < count; i++)                      \
            wr_fn(raw + i * elem_sz, vals[i]);                   \
        int32_t r = encode_al(kind, raw, raw_len, count, out);   \
        free(raw);                                               \
        return r;                                                \
    }

DEF_NEW_ARRAY(Bool, KVSPACE_KIND_BOOL, bool, 1, wr_u8)
DEF_NEW_ARRAY(Int8, KVSPACE_KIND_INT8, int8_t, 1, wr_i8)
DEF_NEW_ARRAY(Int16, KVSPACE_KIND_INT16, int16_t, 2, wr_i16)
DEF_NEW_ARRAY(Int32, KVSPACE_KIND_INT32, int32_t, 4, wr_i32)
DEF_NEW_ARRAY(Int64, KVSPACE_KIND_INT64, int64_t, 8, wr_i64)
DEF_NEW_ARRAY(Uint8, KVSPACE_KIND_UINT8, uint8_t, 1, wr_u8)
DEF_NEW_ARRAY(Uint16, KVSPACE_KIND_UINT16, uint16_t, 2, wr_u16)
DEF_NEW_ARRAY(Uint32, KVSPACE_KIND_UINT32, uint32_t, 4, wr_u32)
DEF_NEW_ARRAY(Uint64, KVSPACE_KIND_UINT64, uint64_t, 8, wr_u64)

int32_t kvspaceXvalueNewFloat32(const float *vals, int32_t count,
                                uint8_t **out) {
    if (!vals || count <= 0)
        return -1;
    int32_t raw_len = count * 4;
    uint8_t *raw = (uint8_t *)malloc((size_t)raw_len);
    if (!raw)
        return -1;
    for (int32_t i = 0; i < count; i++) {
        union {
            float f;
            uint32_t u;
        } c = {vals[i]};
        wr_u32(raw + i * 4, c.u);
    }
    int32_t r = encode_al(KVSPACE_KIND_FLOAT32, raw, raw_len, count, out);
    free(raw);
    return r;
}

int32_t kvspaceXvalueNewFloat64(const double *vals, int32_t count,
                                uint8_t **out) {
    if (!vals || count <= 0)
        return -1;
    int32_t raw_len = count * 8;
    uint8_t *raw = (uint8_t *)malloc((size_t)raw_len);
    if (!raw)
        return -1;
    for (int32_t i = 0; i < count; i++) {
        union {
            double f;
            uint64_t u;
        } c = {vals[i]};
        wr_u64(raw + i * 8, c.u);
    }
    int32_t r = encode_al(KVSPACE_KIND_FLOAT64, raw, raw_len, count, out);
    free(raw);
    return r;
}

/* ── 坐标段比较（对齐 durable coord.rs；memindex 矩阵规范排序与容器 dims
 * 派生共用）── */

int kvspaceCoordIsCoord(const char *name) {
    if (!name || name[0] != '[')
        return 0;
    size_t n = strlen(name);
    if (n < 3 || name[n - 1] != ']')
        return 0;
    for (size_t i = 1; i + 1 < n; i++)
        if (name[i] == '[' || name[i] == ']')
            return 0;
    return 1;
}

/* 解析整数坐标段，成功填充 coords 并返回维数；非整数返回 -1。 */
int kvspaceParseCoord(const char *name, int64_t *coords, int maxn) {
    if (!name || name[0] != '[')
        return -1;
    int n = 0;
    int64_t cur = 0;
    bool has = false;
    for (const char *p = name + 1; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            cur = cur * 10 + (*p - '0');
            has = true;
        } else if (*p == ',') {
            if (!has || n >= maxn)
                return -1;
            coords[n++] = cur;
            cur = 0;
            has = false;
        } else if (*p == ']') {
            if (!has || n >= maxn)
                return -1;
            coords[n++] = cur;
            return (p[1] == '\0') ? n : -1;
        } else {
            return -1;
        }
    }
    return -1;
}

int kvspaceCoordCmp(const char *a, const char *b) {
    int ia = kvspaceCoordIsCoord(a), ib = kvspaceCoordIsCoord(b);
    if (!ia && !ib)
        return strcmp(a, b);
    if (!ia)
        return 1; /* 非坐标段排后 */
    if (!ib)
        return -1;
    int64_t ca[8], cb[8];
    int na = kvspaceParseCoord(a, ca, 8);
    int nb = kvspaceParseCoord(b, cb, 8);
    if (na < 0 || nb < 0)
        return strcmp(a, b); /* 含小数/字符串坐标 → 字典序 */
    for (int i = 0; i < na && i < nb; i++) {
        if (ca[i] != cb[i])
            return ca[i] < cb[i] ? -1 : 1;
    }
    if (na != nb)
        return na < nb ? -1 : 1;
    return 0;
}

/* ── index / ptr / extindex：定宽排序矩阵 memindex（Go-slice cap/len，对齐
 * durable xvalue_index.rs）──
 * dims=[len,cap,M]：len=有效成员数、cap≥len=预留行数、M=成员 UTF-8
 * 字节最大长向上 8 对齐（行宽）； body=cap×M，前 len 行成员名 + NUL
 * 补齐（cmp_coord 有序），后 cap−len 行全 NUL。容量内增删 body 长度 恒 cap×M →
 * 就地覆写不重分配；满则 cap 翻倍。与 durable blob 逐字节一致，listat/listlen
 * O(1)。 */

static int32_t align8(int32_t n) { return (n + 7) & ~7; }

static int matrix_qsort_cmp(const void *a, const void *b) {
    return kvspaceCoordCmp(*(const char *const *)a, *(const char *const *)b);
}

/* children → (dims=[len,cap,M], body=malloc(cap×M))，encode
 * 侧规范排序；cap_hint/m_hint 为下限（只增）。 */
static uint8_t *build_matrix(const char **children, int32_t count,
                             int32_t cap_hint, int32_t m_hint, int32_t *len_out,
                             int32_t *cap_out, int32_t *m_out, int32_t *bl) {
    const char **c =
        (const char **)malloc(sizeof(char *) * (size_t)(count > 0 ? count : 1));
    int32_t len = 0;
    for (int32_t i = 0; i < count; i++)
        if (children[i])
            c[len++] = children[i];
    if (len > 1)
        qsort((void *)c, (size_t)len, sizeof(char *), matrix_qsort_cmp);
    int32_t maxl = 0;
    for (int32_t i = 0; i < len; i++) {
        int32_t l = (int32_t)strlen(c[i]);
        if (l > maxl)
            maxl = l;
    }
    int32_t m = align8(maxl);
    if (m_hint > m)
        m = m_hint;
    int32_t cap = len;
    if (cap_hint > cap)
        cap = cap_hint;
    int32_t total = cap * m;
    uint8_t *body = (uint8_t *)calloc((size_t)(total > 0 ? total : 1), 1);
    for (int32_t i = 0; i < len; i++)
        memcpy(body + (size_t)i * m, c[i], strlen(c[i]));
    free((void *)c);
    *len_out = len;
    *cap_out = cap;
    *m_out = m;
    *bl = total;
    return body;
}

int32_t kvspaceXvalueNewIndexGrow(const char **children, int32_t count,
                                  int32_t cap_hint, int32_t m_hint,
                                  uint8_t **out) {
    int32_t len, cap, m, bl;
    uint8_t *body =
        build_matrix(children, count, cap_hint, m_hint, &len, &cap, &m, &bl);
    int32_t dims[3] = {len, cap, m};
    int32_t r = kvspaceXvalueEncode(KVSPACE_KIND_INDEX, body, bl, dims, 3, out);
    free(body);
    return r;
}

int32_t kvspaceXvalueNewIndex(const char **children, int32_t count,
                              uint8_t **out) {
    return kvspaceXvalueNewIndexGrow(children, count, 0, 0, out);
}

/* 指针（ref=PTR）：langtype = target_kindexpr（目标完整
 * kindexpr、无前缀），storetype = 目标语义 storetype，物理字段恒空；body = 目标
 * key 路径。 */
int32_t kvspaceXvalueNewPtr(const char *target_kindexpr, const char *target,
                            uint8_t **out) {
    if (!target || !target_kindexpr || !out)
        return -1;
    uint8_t st = storetype_from_kindexpr(target_kindexpr);
    return encode_head(KVSPACE_REF_PTR, st, target_kindexpr, 0, 0, NULL, 0,
                       (const uint8_t *)target, (int32_t)strlen(target), out);
}

/* extindex：body = 头部变长 ext_path + 尾部 cap×M 矩阵（childs，cmp_coord
 * 有序）；dims=[len,cap,M]。 ext_path 置头部：帧生命周期内一次写定、childs 才
 * churn，矩阵起点 off=body_len−cap*M 稳定。 */
int32_t kvspaceXvalueNewExtindexGrow(const char *extpath, const char **children,
                                     int32_t count, int32_t cap_hint,
                                     int32_t m_hint, uint8_t **out) {
    if (!extpath)
        return -1;
    int32_t len, cap, m, mbl;
    uint8_t *mat =
        build_matrix(children, count, cap_hint, m_hint, &len, &cap, &m, &mbl);
    size_t el = strlen(extpath);
    int32_t bl = (int32_t)el + mbl;
    uint8_t *body = (uint8_t *)malloc((size_t)(bl > 0 ? bl : 1));
    memcpy(body, extpath, el);
    memcpy(body + el, mat, (size_t)mbl);
    int32_t dims[3] = {len, cap, m};
    int32_t r =
        kvspaceXvalueEncode(KVSPACE_KIND_EXT_INDEX, body, bl, dims, 3, out);
    free(mat);
    free(body);
    return r;
}

int32_t kvspaceXvalueNewExtindex(const char *extpath, const char **children,
                                 int32_t count, uint8_t **out) {
    return kvspaceXvalueNewExtindexGrow(extpath, children, count, 0, 0, out);
}
