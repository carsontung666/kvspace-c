#include "xvalue_head.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put_u64(uint8_t *d, uint64_t v) {
    for (int i = 0; i < 8; i++)
        d[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_u64(const uint8_t *d) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= (uint64_t)d[i] << (8 * i);
    return v;
}

static int headlen_of(uint8_t pow, uint32_t *headlen) {
    if (pow < KVSPACE_XH_POW_SCALAR || pow > 31)
        return -1;
    *headlen = 1u << pow;
    return 0;
}

static int scalar_width(const uint8_t *s, uint32_t n) {
    static const struct {
        const char *name;
        uint8_t w;
    } tab[] = {
        {"bool", 1},    {"int8", 1},     {"int16", 2},    {"int32", 4},
        {"int64", 8},   {"uint8", 1},    {"uint16", 2},   {"uint32", 4},
        {"uint64", 8},  {"float8/e4m3", 1}, {"float8/e5m2", 1},
        {"float16", 2}, {"bfloat16", 2}, {"float32", 4},  {"float64", 8},
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        size_t L = strlen(tab[i].name);
        if (L == n && memcmp(s, tab[i].name, n) == 0)
            return tab[i].w;
    }
    return -1;
}

static int type_eq(const uint8_t *s, uint32_t n, const char *want) {
    size_t m = strlen(want);
    return n == m && memcmp(s, want, m) == 0;
}

static int short_width(const uint8_t *s, uint32_t n) {
    int width = scalar_width(s, n);
    if (width >= 0 || n == 0)
        return width >= 0 ? width : 0;
    if (type_eq(s, n, "def rwir"))
        return 5;
    if (type_eq(s, n, "time") || type_eq(s, n, "duration"))
        return 8;
    if (type_eq(s, n, "def struct") || type_eq(s, n, "lib") ||
        type_eq(s, n, "rwfunc") || s[0] == '/')
        return 0;
    for (uint32_t i = 0; i + 1 < n; i++)
        if (s[i] == 0xc2 && s[i + 1] == 0xb7)
            return 0;
    return -1;
}

static int min_pow(uint32_t type_len) {
    for (int pow = KVSPACE_XH_POW_SCALAR; pow <= 31; pow++)
        if ((uint64_t)type_len + KVSPACE_XH_PREFIX <= (1ULL << pow))
            return pow;
    return -1;
}

static const struct {
    const char *suf;
    uint8_t width;
} k_slack[] = {
    {"char/utf8", 0},
    {"byte", 1},
    {"char/utf32", 4},
    {"char/ascii", 1},
};

static int slack_count(size_t k, const uint8_t *data, uint64_t len, uint64_t *count) {
    uint8_t width = k_slack[k].width;
    if (width) {
        if (len % width)
            return -1;
        if (k == KVSPACE_XH_ASCII - KVSPACE_XH_UTF8) {
            for (uint64_t i = 0; i < len; i++)
                if (data[i] > 0x7f)
                    return -1;
        } else if (k == KVSPACE_XH_UTF32 - KVSPACE_XH_UTF8) {
            for (uint64_t i = 0; i < len; i += 4) {
                uint32_t cp = (uint32_t)data[i] | (uint32_t)data[i + 1] << 8 |
                              (uint32_t)data[i + 2] << 16 | (uint32_t)data[i + 3] << 24;
                if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
                    return -1;
            }
        }
        *count = len / width;
        return 0;
    }
    *count = 0;
    for (uint64_t i = 0; i < len; (*count)++) {
        uint8_t c = data[i++];
        if (c < 0x80)
            continue;
        unsigned tail = c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
        if (c < 0xc2 || c > 0xf4 || len - i < tail)
            return -1;
        uint8_t next = data[i];
        if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next > 0x9f) ||
            (c == 0xf0 && next < 0x90) || (c == 0xf4 && next > 0x8f))
            return -1;
        for (unsigned j = 0; j < tail; j++)
            if ((data[i++] & 0xc0) != 0x80)
                return -1;
    }
    return 0;
}

static int parse_u64(const uint8_t *s, uint32_t n, uint32_t *i, uint64_t *v) {
    if (*i >= n || s[*i] < '0' || s[*i] > '9')
        return -1;
    if (s[*i] == '0') {
        *v = 0;
        (*i)++;
        if (*i < n && s[*i] >= '0' && s[*i] <= '9')
            return -1;
        return 0;
    }
    uint64_t x = 0;
    while (*i < n && s[*i] >= '0' && s[*i] <= '9') {
        uint64_t d = (uint64_t)(s[*i] - '0');
        if (x > (UINT64_MAX - d) / 10)
            return -1;
        x = x * 10 + d;
        (*i)++;
    }
    *v = x;
    return 0;
}

static int alloc_value(uint32_t headlen, uint64_t body_len, uint8_t **out, uint64_t *out_len) {
    if (body_len > UINT64_MAX - headlen)
        return -1;
    uint64_t n = (uint64_t)headlen + body_len;
    if (n > SIZE_MAX)
        return -1;
    uint8_t *p = (uint8_t *)calloc(1, (size_t)n);
    if (!p)
        return -1;
    *out = p;
    *out_len = n;
    return 0;
}

static int emit(uint8_t pow, uint8_t kind, uint64_t a, uint64_t b,
                const char *lang, uint32_t lang_len,
                const uint8_t *body, uint64_t copy_len, uint64_t store_len,
                uint8_t **out, uint64_t *out_len) {
    uint32_t headlen = 0;
    if (headlen_of(pow, &headlen) != 0 || lang_len > headlen - KVSPACE_XH_PREFIX ||
        copy_len > store_len)
        return -1;
    if (alloc_value(headlen, store_len, out, out_len) != 0)
        return -1;
    uint8_t *p = *out;
    p[0] = pow;
    p[1] = kind;
    put_u64(p + 2, a);
    put_u64(p + 10, b);
    if (lang_len)
        memcpy(p + KVSPACE_XH_PREFIX, lang, lang_len);
    if (copy_len)
        memcpy(p + headlen, body, (size_t)copy_len);
    return 0;
}

int kvspaceXhNewNone(uint8_t **out, uint64_t *out_len) {
    if (!out || !out_len)
        return -1;
    *out = NULL;
    return emit(KVSPACE_XH_POW_SCALAR, KVSPACE_XH_FIXED_SMALL, 0, 0, NULL, 0, NULL, 0, 0,
                out, out_len);
}

int kvspaceXhNewScalar(const char *langtype, const uint8_t *raw, uint32_t raw_len,
                       uint8_t **out, uint64_t *out_len) {
    if (!out || !out_len)
        return -1;
    *out = NULL;
    if (!langtype || !raw)
        return -1;
    uint32_t lt = (uint32_t)strlen(langtype);
    int width = scalar_width((const uint8_t *)langtype, lt);
    if (width < 0 || raw_len != (uint32_t)width ||
        (strcmp(langtype, "bool") == 0 && raw[0] > 1))
        return -1;
    return emit(KVSPACE_XH_POW_SCALAR, KVSPACE_XH_FIXED_SMALL, 0, 0, langtype, lt, raw,
                width, width, out, out_len);
}

static int decode_emitted(uint8_t **out, uint64_t *out_len) {
    kvspaceXh h;
    if (kvspaceXhDecode(*out, *out_len, &h) == 0)
        return 0;
    free(*out);
    *out = NULL;
    *out_len = 0;
    return -1;
}

int kvspaceXhNewShort(const char *langtype, const uint8_t *body, uint32_t body_len,
                      uint8_t **out, uint64_t *out_len) {
    if (!out || !out_len)
        return -1;
    *out = NULL;
    if (!langtype || (body_len && !body))
        return -1;
    size_t lt = strlen(langtype);
    if (lt > UINT32_MAX || short_width((const uint8_t *)langtype, (uint32_t)lt) !=
                               (int)body_len)
        return -1;
    int pow = min_pow((uint32_t)lt);
    if (pow < 0 || emit((uint8_t)pow, KVSPACE_XH_FIXED_SMALL, 0, 0,
                        langtype, (uint32_t)lt, body, body_len, body_len,
                        out, out_len) != 0)
        return -1;
    return decode_emitted(out, out_len);
}

int kvspaceXhNewSlack(int elem, const uint8_t *data, uint64_t data_len, uint64_t cap,
                      uint8_t **out, uint64_t *out_len) {
    if (!out || !out_len)
        return -1;
    *out = NULL;
    if (elem < KVSPACE_XH_UTF8 || elem > KVSPACE_XH_ASCII || cap < data_len ||
        cap > SIZE_MAX - (1u << KVSPACE_XH_POW_SLACK) || (data_len && !data))
        return -1;
    size_t k = (size_t)(elem - KVSPACE_XH_UTF8);
    uint64_t count = 0;
    if (slack_count(k, data, data_len, &count) != 0)
        return -1;
    char text[48];
    int kn = snprintf(text, sizeof text, "[%" PRIu64 "]%s", count, k_slack[k].suf);
    if (kn < 0 || (size_t)kn >= sizeof text)
        return -1;
    return emit(KVSPACE_XH_POW_SLACK, KVSPACE_XH_SLACK, data_len, cap, text, (uint32_t)kn,
                data, data_len, cap, out, out_len);
}

int kvspaceXhNewTensor(const uint64_t *dims, uint32_t ndim, const char *elem,
                       const uint8_t *raw, uint64_t raw_len,
                       uint8_t **out, uint64_t *out_len) {
    if (!out || !out_len)
        return -1;
    *out = NULL;
    if (!dims || ndim == 0 || ndim > 64 || !elem)
        return -1;
    uint32_t el = (uint32_t)strlen(elem);
    int width = scalar_width((const uint8_t *)elem, el);
    if (width < 0)
        return -1;
    uint64_t numel = 1;
    for (uint32_t i = 0; i < ndim; i++) {
        if (dims[i] != 0 && numel > UINT64_MAX / dims[i])
            return -1;
        numel *= dims[i];
    }
    if (numel > UINT64_MAX / (uint64_t)width)
        return -1;
    uint64_t bytes = numel * (uint64_t)width;
    if (raw_len != bytes || (bytes > 0 && !raw))
        return -1;
    char text[111];
    size_t o = 0;
    text[o++] = '[';
    for (uint32_t i = 0; i < ndim; i++) {
        if (i && o < sizeof text)
            text[o++] = ',';
        int kn = snprintf(text + o, sizeof text - o, "%" PRIu64, dims[i]);
        if (kn < 0 || (size_t)kn >= sizeof text - o)
            return -1;
        o += (size_t)kn;
    }
    if (o + 1 + el >= sizeof text)
        return -1;
    text[o++] = ']';
    memcpy(text + o, elem, el);
    o += el;
    return emit(KVSPACE_XH_POW_TENSOR, KVSPACE_XH_FIXED_LARGE, numel, (uint64_t)width, text,
                (uint32_t)o, raw, bytes, bytes, out, out_len);
}

static int new_locator(uint8_t kind, const char *langtype, const char *locator,
                       uint64_t cap, uint8_t **out, uint64_t *out_len) {
    if (!out || !out_len)
        return -1;
    *out = NULL;
    if (!langtype || !locator)
        return -1;
    size_t lt = strlen(langtype);
    size_t locator_len = strlen(locator);
    if (lt > UINT32_MAX || cap < locator_len)
        return -1;
    int pow = min_pow((uint32_t)lt);
    if (pow < 0 || emit((uint8_t)pow, kind,
                        locator_len, cap, langtype, (uint32_t)lt,
                        (const uint8_t *)locator, locator_len, cap,
                        out, out_len) != 0)
        return -1;
    return decode_emitted(out, out_len);
}

int kvspaceXhNewPtr(const char *langtype, const char *path, uint64_t cap,
                    uint8_t **out, uint64_t *out_len) {
    return new_locator(KVSPACE_XH_SLACK | KVSPACE_XH_PTR_FLAG,
                       langtype, path, cap, out, out_len);
}

int kvspaceXhNewExt(const char *langtype, const char *locator, uint64_t cap,
                    uint8_t **out, uint64_t *out_len) {
    return new_locator(KVSPACE_XH_EXT, langtype, locator, cap, out, out_len);
}

int kvspaceXhNewCode(const char *langtype, const uint8_t *body, uint64_t body_len,
                     uint64_t cap, uint8_t **out, uint64_t *out_len) {
    if (!out || !out_len)
        return -1;
    *out = NULL;
    if (!langtype || (body_len && !body) || cap < body_len)
        return -1;
    size_t lt = strlen(langtype);
    if (lt > UINT32_MAX ||
        (!type_eq((const uint8_t *)langtype, (uint32_t)lt, "rwir") &&
         !type_eq((const uint8_t *)langtype, (uint32_t)lt, "def langtype") &&
         !type_eq((const uint8_t *)langtype, (uint32_t)lt, "rwfunc")))
        return -1;
    if (emit(KVSPACE_XH_POW_SCALAR, KVSPACE_XH_SLACK, body_len, cap,
             langtype, (uint32_t)lt, body, body_len, cap,
             out, out_len) != 0)
        return -1;
    return decode_emitted(out, out_len);
}

static int parse_slack(const uint8_t *s, uint32_t n, const uint8_t *body, uint64_t len) {
    if (n < 3 || s[0] != '[')
        return -1;
    uint32_t i = 1;
    uint64_t logical = 0;
    if (parse_u64(s, n, &i, &logical) != 0 || i >= n || s[i] != ']')
        return -1;
    i++;
    uint32_t sl = n - i;
    for (size_t k = 0; k < sizeof k_slack / sizeof k_slack[0]; k++) {
        size_t L = strlen(k_slack[k].suf);
        if (L != sl || memcmp(s + i, k_slack[k].suf, L) != 0)
            continue;
        uint64_t count = 0;
        return slack_count(k, body, len, &count) == 0 && count == logical ? 0 : -1;
    }
    return -1;
}

static int parse_tensor(const uint8_t *s, uint32_t n, uint64_t *numel, int *width) {
    if (n < 4 || s[0] != '[')
        return -1;
    uint32_t i = 1;
    uint64_t prod = 1;
    int any = 0;
    while (i < n && s[i] != ']') {
        uint64_t d = 0;
        if (parse_u64(s, n, &i, &d) != 0)
            return -1;
        if (d != 0 && prod > UINT64_MAX / d)
            return -1;
        prod *= d;
        any = 1;
        if (i >= n)
            return -1;
        if (s[i] == ',') {
            i++;
            if (i >= n || s[i] == ']')
                return -1;
            continue;
        }
        if (s[i] != ']')
            return -1;
    }
    if (!any || i >= n || s[i] != ']')
        return -1;
    i++;
    int w = scalar_width(s + i, n - i);
    if (w < 0)
        return -1;
    *numel = prod;
    *width = w;
    return 0;
}

static int slack_langtype(const uint8_t *s, uint32_t n) {
    if (n < 3 || s[0] != '[')
        return -1;
    uint32_t i = 1;
    uint64_t count = 0;
    if (parse_u64(s, n, &i, &count) != 0 || i >= n || s[i++] != ']')
        return -1;
    for (size_t k = 0; k < sizeof k_slack / sizeof k_slack[0]; k++) {
        size_t len = strlen(k_slack[k].suf);
        if (n - i == len && memcmp(s + i, k_slack[k].suf, len) == 0)
            return 0;
    }
    return -1;
}

int kvspaceXhReserve(uint8_t kind, const char *langtype, uint64_t body_len,
                     uint64_t cap, uint8_t **out, uint64_t *out_len) {
    if (!out || !out_len)
        return -1;
    *out = NULL;
    *out_len = 0;
    if (!langtype)
        return -1;
    size_t lt = strlen(langtype);
    if (lt > UINT32_MAX)
        return -1;
    const uint8_t *type = (const uint8_t *)langtype;
    uint64_t type_chars = 0;
    if (slack_count(0, type, (uint64_t)lt, &type_chars) != 0)
        return -1;
    uint64_t a = 0, b = 0, reserve = cap;
    int pow = -1;
    if (kind == KVSPACE_XH_FIXED_SMALL) {
        int width = short_width(type, (uint32_t)lt);
        if (width < 0 || body_len != (uint64_t)width || cap != body_len)
            return -1;
        pow = min_pow((uint32_t)lt);
    } else if (kind == KVSPACE_XH_SLACK) {
        int code = type_eq(type, (uint32_t)lt, "rwir") ||
                   type_eq(type, (uint32_t)lt, "def langtype") ||
                   type_eq(type, (uint32_t)lt, "rwfunc");
        if ((!code && slack_langtype(type, (uint32_t)lt) != 0) ||
            (code && ((type_eq(type, (uint32_t)lt, "rwir") && body_len < 5) ||
                      (type_eq(type, (uint32_t)lt, "rwfunc") && body_len < 5))))
            return -1;
        pow = code ? KVSPACE_XH_POW_SCALAR : KVSPACE_XH_POW_SLACK;
        a = body_len;
        b = cap;
    } else if (kind == KVSPACE_XH_FIXED_LARGE) {
        int width = 0;
        if (parse_tensor(type, (uint32_t)lt, &a, &width) != 0 ||
            a > UINT64_MAX / (uint64_t)width ||
            body_len != a * (uint64_t)width || cap != body_len)
            return -1;
        b = (uint64_t)width;
        pow = KVSPACE_XH_POW_TENSOR;
        reserve = body_len;
    } else if (kind == KVSPACE_XH_EXT ||
               kind == (KVSPACE_XH_SLACK | KVSPACE_XH_PTR_FLAG)) {
        if (!lt || !body_len)
            return -1;
        pow = min_pow((uint32_t)lt);
        a = body_len;
        b = cap;
    } else {
        return -1;
    }
    if (pow < 0 || body_len > cap)
        return -1;
    return emit((uint8_t)pow, kind, a, b, langtype, (uint32_t)lt,
                NULL, 0, reserve, out, out_len);
}

int kvspaceXhDecode(const uint8_t *data, uint64_t len, kvspaceXh *out) {
    if (!out)
        return -1;
    memset(out, 0, sizeof *out);
    if (!data || len < KVSPACE_XH_PREFIX)
        return -1;
    uint8_t pow = data[0];
    uint32_t headlen = 0;
    if (headlen_of(pow, &headlen) != 0 || len < headlen)
        return -1;
    uint8_t kind = data[1];
    if (kind & ~(KVSPACE_XH_PTR_FLAG | 3u))
        return -1;
    uint64_t a = get_u64(data + 2);
    uint64_t b = get_u64(data + 10);
    const uint8_t *lt = data + KVSPACE_XH_PREFIX;
    uint32_t region = headlen - KVSPACE_XH_PREFIX;
    uint32_t ltlen = 0;
    while (ltlen < region && lt[ltlen] != 0)
        ltlen++;
    uint64_t type_chars = 0;
    if (slack_count(0, lt, ltlen, &type_chars) != 0)
        return -1;

    uint64_t content = 0;
    uint64_t cap = 0;
    if (kind == KVSPACE_XH_FIXED_SMALL) {
        if (pow != min_pow(ltlen) || a != 0 || b != 0)
            return -1;
        int width = short_width(lt, ltlen);
        if (width < 0)
            return -1;
        content = (uint64_t)width;
        cap = content;
    } else if (kind == KVSPACE_XH_SLACK) {
        int code = type_eq(lt, ltlen, "rwir") ||
                   type_eq(lt, ltlen, "def langtype") ||
                   type_eq(lt, ltlen, "rwfunc");
        if (pow != (code ? KVSPACE_XH_POW_SCALAR : KVSPACE_XH_POW_SLACK) ||
            a > b)
            return -1;
        content = a;
        cap = b;
    } else if (kind == KVSPACE_XH_EXT ||
               kind == (KVSPACE_XH_SLACK | KVSPACE_XH_PTR_FLAG)) {
        if (pow != min_pow(ltlen) || a == 0 || a > b || ltlen == 0)
            return -1;
        content = a;
        cap = b;
    } else if (kind == KVSPACE_XH_FIXED_LARGE) {
        if (pow != KVSPACE_XH_POW_TENSOR)
            return -1;
        uint64_t numel = 0;
        int width = 0;
        if (parse_tensor(lt, ltlen, &numel, &width) != 0 || numel != a ||
            (uint64_t)width != b)
            return -1;
        if (b != 0 && a > UINT64_MAX / b)
            return -1;
        content = a * b;
        cap = content;
    } else {
        return -1;
    }
    if (cap > len - headlen)
        return -1;
    if (kind == KVSPACE_XH_FIXED_SMALL && type_eq(lt, ltlen, "bool") &&
        data[headlen] > 1)
        return -1;
    if (kind == KVSPACE_XH_SLACK) {
        if (type_eq(lt, ltlen, "rwir") || type_eq(lt, ltlen, "def langtype") ||
            type_eq(lt, ltlen, "rwfunc")) {
            if (pow != KVSPACE_XH_POW_SCALAR ||
                (type_eq(lt, ltlen, "rwir") && content < 5) ||
                (type_eq(lt, ltlen, "rwfunc") && content < 5))
                return -1;
        } else if (parse_slack(lt, ltlen, data + headlen, content) != 0) {
            return -1;
        }
    } else if (kind == KVSPACE_XH_EXT ||
               kind == (KVSPACE_XH_SLACK | KVSPACE_XH_PTR_FLAG)) {
        if (memchr(data + headlen, 0, (size_t)content))
            return -1;
        uint64_t path_chars = 0;
        if (slack_count(0, data + headlen, content, &path_chars) != 0)
            return -1;
    }

    out->pow = pow;
    out->kind = kind;
    out->a = a;
    out->b = b;
    out->langtype = lt;
    out->langtype_len = ltlen;
    out->body = data + headlen;
    out->content_len = content;
    out->body_cap = cap;
    out->headlen = headlen;
    out->total = (uint64_t)headlen + cap;
    return 0;
}
