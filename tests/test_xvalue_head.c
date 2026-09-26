#include "xvalue_head.h"
#include "xvalue_meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
        }                                                                      \
    } while (0)

static void test_none(void) {
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXhNewNone(&buf, &n) == 0, "none encode");
    CHECK(n == 32, "none len %llu", (unsigned long long)n);
    CHECK(buf[0] == 5, "pow");
    int rest = 0;
    for (int i = 1; i < 32; i++)
        rest |= buf[i];
    CHECK(rest == 0, "none head tail");
    kvspaceXh h;
    CHECK(kvspaceXhDecode(buf, n, &h) == 0, "none decode");
    CHECK(h.kind == KVSPACE_XH_FIXED_SMALL && h.langtype_len == 0, "none kind");
    CHECK(h.a == 0 && h.b == 0 && h.content_len == 0 && h.total == 32, "none fields");
    uint8_t zeros[32];
    memset(zeros, 0, sizeof zeros);
    CHECK(kvspaceXhDecode(zeros, 32, &h) != 0, "pow 0");
    CHECK(h.langtype_len == 0 && h.total == 0, "fail clears");
    free(buf);
}

static void test_int64(void) {
    uint8_t raw[8] = {0x0a, 0, 0, 0, 0, 0, 0, 0};
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXhNewScalar("int64", raw, 8, &buf, &n) == 0, "i64");
    CHECK(n == 40, "i64 len %llu", (unsigned long long)n);
    CHECK(buf[0] == 5 && buf[1] == 0, "i64 prefix");
    CHECK(memcmp(buf + 18, "int64", 5) == 0, "i64 lang");
    CHECK(buf[23] == 0, "i64 nul");
    CHECK(memcmp(buf + 32, raw, 8) == 0, "i64 body");
    kvspaceXh h;
    uint8_t padded[64] = {0};
    memcpy(padded, buf, (size_t)n);
    CHECK(kvspaceXhDecode(padded, sizeof padded, &h) == 0, "read 64");
    CHECK(h.total == 40 && h.content_len == 8 && h.body[0] == 0x0a, "ignore overrun");
    CHECK(kvspaceXhDecode(buf, 39, &h) != 0, "short body");
    free(buf);

    uint8_t b = 1;
    CHECK(kvspaceXhNewScalar("bool", &b, 1, &buf, &n) == 0, "bool");
    CHECK(n == 33 && buf[32] == 1, "bool body");
    buf[32] = 2;
    CHECK(kvspaceXhDecode(buf, n, &h) != 0, "invalid bool decode");
    free(buf);
    b = 2;
    CHECK(kvspaceXhNewScalar("bool", &b, 1, &buf, &n) != 0, "invalid bool encode");
    CHECK(kvspaceXhNewScalar("int64", raw, 4, &buf, &n) != 0, "bad width");
    CHECK(buf == NULL, "no buf");
}

static void test_slack(void) {
    const uint8_t hi[] = {'h', 'i'};
    uint8_t *buf = NULL;
    uint64_t n = 0;
    kvspaceXh h;
    uint8_t cps[8] = {0x41, 0, 0, 0, 0x16, 0x4e, 0, 0};
    CHECK(kvspaceXhNewSlack(KVSPACE_XH_UTF32, cps, sizeof cps, 8, &buf, &n) == 0, "utf32");
    CHECK(kvspaceXhDecode(buf, n, &h) == 0, "utf32 dec");
    CHECK(h.a == 8 && h.langtype_len == 13 && memcmp(h.langtype, "[2]char/utf32", 13) == 0,
          "utf32 lt");
    free(buf);

    CHECK(kvspaceXhNewSlack(KVSPACE_XH_UTF32, cps, 3, 8, &buf, &n) != 0,
          "partial utf32");
    CHECK(buf == NULL, "partial utf32 output");
    uint8_t surrogate[4] = {0, 0xd8, 0, 0};
    CHECK(kvspaceXhNewSlack(KVSPACE_XH_UTF32, surrogate, 4, 4, &buf, &n) != 0,
          "utf32 surrogate");
    uint8_t too_high[4] = {0, 0, 0x11, 0};
    CHECK(kvspaceXhNewSlack(KVSPACE_XH_UTF32, too_high, 4, 4, &buf, &n) != 0,
          "utf32 range");

    CHECK(kvspaceXhNewSlack(KVSPACE_XH_ASCII, hi, 2, 2, &buf, &n) == 0, "ascii");
    CHECK(kvspaceXhDecode(buf, n, &h) == 0 && h.a == 2, "ascii dec");
    CHECK(memcmp(h.langtype, "[2]char/ascii", h.langtype_len) == 0, "ascii lt");
    free(buf);
    uint8_t non_ascii = 0x80;
    CHECK(kvspaceXhNewSlack(KVSPACE_XH_ASCII, &non_ascii, 1, 1, &buf, &n) != 0,
          "ascii range");

    CHECK(kvspaceXhNewSlack(KVSPACE_XH_BYTE, hi, 2, 2, &buf, &n) == 0, "byte");
    CHECK(kvspaceXhDecode(buf, n, &h) == 0, "byte dec");
    CHECK(memcmp(h.langtype, "[2]byte", h.langtype_len) == 0, "byte lt");
    buf[2] = 9;
    CHECK(kvspaceXhDecode(buf, n, &h) != 0, "len > cap");
    free(buf);
}

static void test_utf8(void) {
    static const struct {
        const char *data;
        uint8_t len, count;
    } cases[] = {
        {"", 0, 0},
        {"hi", 2, 2},
        {"\xe4\xbd\xa0\xe5\xa5\xbd", 6, 2},
        {"\xf0\x9f\x98\x80", 4, 1},
        {"A\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80", 10, 4},
        {"A\0B", 3, 3},
        {"\xc2\x80\xdf\xbf", 4, 2},
        {"\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80\xef\xbf\xbf", 12, 4},
        {"\xf0\x90\x80\x80\xf4\x8f\xbf\xbf", 8, 2},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        uint8_t wire[80] = {6, 1};
        wire[2] = cases[i].len;
        wire[10] = 16;
        memcpy(wire + 18, "[0]char/utf8", 12);
        wire[19] += cases[i].count;
        memcpy(wire + 64, cases[i].data, cases[i].len);
        uint8_t *buf = NULL;
        uint64_t n = 0;
        CHECK(kvspaceXhNewSlack(KVSPACE_XH_UTF8, (const uint8_t *)cases[i].data,
                               cases[i].len, 16, &buf, &n) == 0, "utf8 encode %zu", i);
        CHECK(buf && n == sizeof wire && memcmp(buf, wire, sizeof wire) == 0,
              "utf8 wire %zu", i);
        free(buf);
        kvspaceXh h;
        CHECK(kvspaceXhDecode(wire, sizeof wire, &h) == 0, "utf8 fixture %zu", i);
        CHECK(h.content_len == cases[i].len && h.body_cap == 16 && h.total == 80,
              "utf8 lengths %zu", i);
        wire[19]++;
        CHECK(kvspaceXhDecode(wire, sizeof wire, &h) != 0, "utf8 count %zu", i);
        CHECK(h.body == NULL && h.total == 0, "utf8 failure clears %zu", i);
        wire[19]--;
        memset(wire + 64 + cases[i].len, 0xff, 16 - cases[i].len);
        CHECK(kvspaceXhDecode(wire, sizeof wire, &h) == 0, "utf8 slack %zu", i);
        for (uint64_t cut = 64; cut < sizeof wire; cut++)
            CHECK(kvspaceXhDecode(wire, cut, &h) != 0, "utf8 truncation %zu", i);
        if (cases[i].len) {
            CHECK(kvspaceXhNewSlack(KVSPACE_XH_UTF8, (const uint8_t *)cases[i].data,
                                   cases[i].len, cases[i].len - 1, &buf, &n) != 0,
                  "utf8 capacity %zu", i);
            CHECK(buf == NULL, "utf8 capacity output %zu", i);
        }
        wire[2] = wire[10] = 255;
        CHECK(kvspaceXhDecode(wire, sizeof wire, &h) != 0, "utf8 body bounds %zu", i);
    }
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXhNewSlack(KVSPACE_XH_UTF8, NULL, 0, 0, &buf, &n) == 0, "empty utf8");
    kvspaceXh h;
    CHECK(kvspaceXhDecode(buf, n, &h) == 0 && h.content_len == 0, "empty utf8 decode");
    free(buf);
    CHECK(kvspaceXhNewSlack(KVSPACE_XH_UTF8, NULL, 1, 1, &buf, &n) != 0, "null utf8");
}

static void test_invalid_utf8(void) {
    static const char *cases[] = {
        "\x80", "\xbf", "\xff", "\xc0\xaf", "\xc1\xbf", "\xe0\x80\x80",
        "\xed\xa0\x80", "\xf0\x80\x80\x80", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80",
        "\xc2", "\xe2\x82", "\xf0\x9f\x98", "\xc2\x41", "\xe2\x28\xa1", "\xf0\x90\x41\x80",
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        size_t len = strlen(cases[i]);
        uint8_t *buf = NULL;
        uint64_t n = 0;
        CHECK(kvspaceXhNewSlack(KVSPACE_XH_UTF8, (const uint8_t *)cases[i], len, 16,
                               &buf, &n) != 0, "invalid utf8 encode %zu", i);
        CHECK(buf == NULL, "invalid utf8 output %zu", i);
        free(buf);
        uint8_t wire[80] = {6, 1};
        wire[2] = (uint8_t)len;
        wire[10] = 16;
        memcpy(wire + 18, "[1]char/utf8", 12);
        memcpy(wire + 64, cases[i], len);
        kvspaceXh h;
        CHECK(kvspaceXhDecode(wire, sizeof wire, &h) != 0, "invalid utf8 decode %zu", i);
    }
}

static void test_tensor(void) {
    uint64_t dims[2] = {2, 3};
    uint8_t raw[24];
    memset(raw, 0xab, sizeof raw);
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXhNewTensor(dims, 2, "float32", raw, sizeof raw,
                             &buf, &n) == 0, "td");
    uint8_t *bad = NULL;
    uint64_t bad_len = 0;
    CHECK(kvspaceXhNewTensor(dims, 2, "float32", raw, sizeof raw - 1,
                             &bad, &bad_len) != 0, "tensor body length");
    CHECK(n == 128 + 24, "td len %llu", (unsigned long long)n);
    CHECK(buf[0] == 7 && buf[1] == 2, "td kind");
    kvspaceXh h;
    CHECK(kvspaceXhDecode(buf, n, &h) == 0, "td dec");
    CHECK(h.a == 6 && h.b == 4 && h.content_len == 24, "td fields");
    CHECK(h.langtype_len == 12 && memcmp(h.langtype, "[2,3]float32", 12) == 0, "td lt");
    CHECK(memcmp(h.body, raw, 24) == 0, "td body");
    buf[2] = 5;
    CHECK(kvspaceXhDecode(buf, n, &h) != 0, "numel mismatch");
    free(buf);

    uint8_t ext[32];
    memset(ext, 0, sizeof ext);
    ext[0] = 5;
    ext[1] = KVSPACE_XH_EXT;
    CHECK(kvspaceXhDecode(ext, sizeof ext, &h) != 0, "ext");
}

static void test_metadata(void) {
    char *key = NULL;
    CHECK(kvspaceMetaKey("/a", &key) == 0 &&
          strcmp(key, "/.kvspace-meta/2f61") == 0, "metadata key");
    free(key);
    uint8_t *encoded = NULL;
    uint64_t len = 0;
    CHECK(kvspaceMetaEncode(1, 0x12345678, &encoded, &len) == 0,
          "metadata encode");
    uint8_t ro = 0;
    uint32_t vid = 0;
    CHECK(kvspaceMetaDecode(encoded, len, &ro, &vid) == 0 &&
          ro == 1 && vid == 0x12345678, "metadata decode");
    encoded[64] = 2;
    CHECK(kvspaceMetaDecode(encoded, len, &ro, &vid) != 0,
          "metadata bool");
    free(encoded);
}

static void test_reserve(void) {
    uint8_t *value = NULL;
    uint64_t len = 0;
    kvspaceXh h;
    const uint8_t utf8[] = {0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80};
    CHECK(kvspaceXhReserve(KVSPACE_XH_SLACK, "[2]char/utf8", 7, 10,
                           &value, &len) == 0, "reserve utf8");
    memcpy(value + 64, utf8, sizeof utf8);
    CHECK(kvspaceXhDecode(value, len, &h) == 0 && h.a == 7 && h.b == 10,
          "reserved utf8 decodes after fill");
    free(value);
    CHECK(kvspaceXhReserve(KVSPACE_XH_FIXED_LARGE, "[2,3]float32", 23, 23,
                           &value, &len) != 0, "reserve wrong tensor width");
    CHECK(kvspaceXhReserve(KVSPACE_XH_FIXED_LARGE, "[2,3]float32", 24, 24,
                           &value, &len) == 0, "reserve tensor");
    free(value);
    CHECK(kvspaceXhReserve(KVSPACE_XH_SLACK, "[1]char/utf8\xff", 1, 1,
                           &value, &len) != 0, "reserve invalid langtype");
}

static void test_container_ptr_code(void) {
    kvspaceXh h;
    uint8_t *encoded = NULL;
    uint64_t encoded_len = 0;
    uint8_t map[64] = {6, KVSPACE_XH_FIXED_SMALL};
    const char *map_type = "[int64]\xc2\xb7[]char/utf32";
    memcpy(map + 18, map_type, strlen(map_type));
    CHECK(kvspaceXhDecode(map, sizeof map, &h) == 0, "map head");
    CHECK(h.langtype_len == strlen(map_type) && h.content_len == 0,
          "map fields");
    CHECK(kvspaceXhNewShort(map_type, NULL, 0, &encoded, &encoded_len) == 0,
          "map encode");
    CHECK(encoded_len == sizeof map && memcmp(encoded, map, sizeof map) == 0,
          "map wire");
    free(encoded);

    uint8_t def[37] = {5, KVSPACE_XH_FIXED_SMALL};
    memcpy(def + 18, "def rwir", 8);
    CHECK(kvspaceXhDecode(def, sizeof def, &h) == 0, "def rwir head");
    CHECK(h.content_len == 5 && h.total == sizeof def, "def rwir body");
    CHECK(kvspaceXhNewShort("def rwir", def + 32, 5, &encoded, &encoded_len) == 0,
          "def rwir encode");
    CHECK(encoded_len == sizeof def && memcmp(encoded, def, sizeof def) == 0,
          "def rwir wire");
    free(encoded);

    uint8_t time_raw[8] = {1};
    CHECK(kvspaceXhNewShort("time", time_raw, 8, &encoded, &encoded_len) == 0,
          "time encode");
    CHECK(kvspaceXhDecode(encoded, encoded_len, &h) == 0 &&
          h.content_len == 8 && h.headlen == 32, "time head");
    free(encoded);

    uint8_t func[32] = {5, KVSPACE_XH_FIXED_SMALL};
    memcpy(func + 18, "rwfunc", 6);
    CHECK(kvspaceXhDecode(func, sizeof func, &h) == 0 && h.content_len == 0,
          "rwfunc directory");
    CHECK(kvspaceXhNewShort("rwfunc", NULL, 0, &encoded, &encoded_len) == 0,
          "rwfunc directory encode");
    CHECK(encoded_len == sizeof func && memcmp(encoded, func, sizeof func) == 0,
          "rwfunc directory wire");
    free(encoded);

    uint8_t anchor[40] = {5, KVSPACE_XH_SLACK};
    anchor[2] = 5;
    anchor[10] = 8;
    memcpy(anchor + 18, "rwfunc", 6);
    CHECK(kvspaceXhDecode(anchor, sizeof anchor, &h) == 0 &&
          h.content_len == 5 && h.body_cap == 8, "rwfunc anchor");
    CHECK(kvspaceXhNewCode("rwfunc", anchor + 32, 5, 8,
                           &encoded, &encoded_len) == 0, "rwfunc anchor encode");
    CHECK(encoded_len == sizeof anchor && memcmp(encoded, anchor, sizeof anchor) == 0,
          "rwfunc anchor wire");
    free(encoded);
    const uint8_t call_body[] = {0, 0, 0, 0, 0, 'f'};
    CHECK(kvspaceXhNewCode("rwfunc", call_body, sizeof call_body, sizeof call_body,
                           &encoded, &encoded_len) == 0, "rwfunc call encode");
    CHECK(kvspaceXhDecode(encoded, encoded_len, &h) == 0 &&
          h.content_len == sizeof call_body &&
          memcmp(h.body, call_body, sizeof call_body) == 0, "rwfunc call wire");
    free(encoded);
    anchor[2] = 4;
    CHECK(kvspaceXhDecode(anchor, sizeof anchor, &h) != 0, "short rwfunc anchor");

    uint8_t ptr[40] = {5, KVSPACE_XH_SLACK | KVSPACE_XH_PTR_FLAG};
    ptr[2] = 5;
    ptr[10] = 8;
    memcpy(ptr + 18, "int64", 5);
    memcpy(ptr + 32, "/varx", 5);
    CHECK(kvspaceXhDecode(ptr, sizeof ptr, &h) == 0, "pointer head");
    CHECK(h.content_len == 5 && h.body_cap == 8 && h.kind == 5,
          "pointer fields");
    CHECK(kvspaceXhNewPtr("int64", "/varx", 8, &encoded, &encoded_len) == 0,
          "pointer encode");
    CHECK(encoded_len == sizeof ptr && memcmp(encoded, ptr, sizeof ptr) == 0,
          "pointer wire");
    free(encoded);
    ptr[1] |= 0x80;
    CHECK(kvspaceXhDecode(ptr, sizeof ptr, &h) != 0, "reserved pointer bit");
    ptr[1] &= 0x7f;
    ptr[2] = 9;
    CHECK(kvspaceXhDecode(ptr, sizeof ptr, &h) != 0, "pointer length");
    ptr[2] = 5;
    ptr[32] = 0;
    CHECK(kvspaceXhDecode(ptr, sizeof ptr, &h) != 0, "pointer NUL");

    uint8_t ext[40] = {5, KVSPACE_XH_EXT};
    ext[2] = 5;
    ext[10] = 8;
    memcpy(ext + 18, "int64", 5);
    memcpy(ext + 32, "s3://", 5);
    CHECK(kvspaceXhDecode(ext, sizeof ext, &h) == 0 &&
          h.kind == KVSPACE_XH_EXT && h.content_len == 5 && h.body_cap == 8,
          "external locator");
    CHECK(kvspaceXhNewExt("int64", "s3://", 8, &encoded, &encoded_len) == 0,
          "external encode");
    CHECK(encoded_len == sizeof ext && memcmp(encoded, ext, sizeof ext) == 0,
          "external wire");
    free(encoded);
    ext[32] = 0;
    CHECK(kvspaceXhDecode(ext, sizeof ext, &h) != 0, "external NUL");
    ext[32] = 0xff;
    CHECK(kvspaceXhDecode(ext, sizeof ext, &h) != 0, "external UTF-8");
    ext[32] = 's';
    ext[2] = 0;
    CHECK(kvspaceXhDecode(ext, sizeof ext, &h) != 0, "empty external locator");

    uint8_t rwir[40] = {5, KVSPACE_XH_SLACK};
    rwir[2] = 8;
    rwir[10] = 8;
    memcpy(rwir + 18, "rwir", 4);
    CHECK(kvspaceXhDecode(rwir, sizeof rwir, &h) == 0, "rwir head");
    CHECK(h.content_len == 8 && h.total == sizeof rwir, "rwir body");
    CHECK(kvspaceXhNewCode("rwir", rwir + 32, 8, 8, &encoded, &encoded_len) == 0,
          "rwir encode");
    CHECK(encoded_len == sizeof rwir && memcmp(encoded, rwir, sizeof rwir) == 0,
          "rwir wire");
    free(encoded);
    rwir[2] = 4;
    CHECK(kvspaceXhDecode(rwir, sizeof rwir, &h) != 0, "short rwir body");

    uint8_t def_type[35] = {5, KVSPACE_XH_SLACK};
    def_type[2] = def_type[10] = 3;
    memcpy(def_type + 18, "def langtype", 12);
    memcpy(def_type + 32, "int", 3);
    CHECK(kvspaceXhDecode(def_type, sizeof def_type, &h) == 0,
          "def langtype head");
    CHECK(kvspaceXhNewCode("def langtype", (const uint8_t *)"int", 3, 3,
                           &encoded, &encoded_len) == 0,
          "def langtype encode");
    CHECK(encoded_len == sizeof def_type &&
          memcmp(encoded, def_type, sizeof def_type) == 0,
          "def langtype wire");
    free(encoded);
}

int main(void) {
    test_none();
    test_int64();
    test_slack();
    test_utf8();
    test_invalid_utf8();
    test_tensor();
    test_metadata();
    test_reserve();
    test_container_ptr_code();
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
