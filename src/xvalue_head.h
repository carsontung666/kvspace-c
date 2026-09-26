/* Wire: [pow][flags][a:u64le][b:u64le][langtype][body]. Head length is 1 << pow.
   flags: low two bits are storage class; bit 2 marks a pointer. */
#ifndef XVALUE_HEAD_H
#define XVALUE_HEAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVSPACE_XH_PREFIX 18u

#define KVSPACE_XH_FIXED_SMALL 0u
#define KVSPACE_XH_SLACK       1u
#define KVSPACE_XH_FIXED_LARGE 2u
#define KVSPACE_XH_EXT         3u
#define KVSPACE_XH_PTR_FLAG    4u

#define KVSPACE_XH_POW_SCALAR 5u
#define KVSPACE_XH_POW_SLACK  6u
#define KVSPACE_XH_POW_TENSOR 7u

#define KVSPACE_XH_UTF8  1
#define KVSPACE_XH_BYTE  2
#define KVSPACE_XH_UTF32 3
#define KVSPACE_XH_ASCII 4

typedef struct kvspaceXh {
    uint8_t        pow;
    uint8_t        kind;
    uint64_t       a;
    uint64_t       b;
    const uint8_t *langtype;
    uint32_t       langtype_len;
    const uint8_t *body;
    uint64_t       content_len;
    uint64_t       body_cap;
    uint32_t       headlen;
    uint64_t       total;
} kvspaceXh;

/* 0 on success. -1 and *out cleared on failure. Bytes past total are ignored. */
int kvspaceXhDecode(const uint8_t *data, uint64_t len, kvspaceXh *out);

int kvspaceXhNewNone(uint8_t **out, uint64_t *out_len);
int kvspaceXhNewScalar(const char *langtype, const uint8_t *raw, uint32_t raw_len,
                       uint8_t **out, uint64_t *out_len);
int kvspaceXhNewShort(const char *langtype, const uint8_t *body, uint32_t body_len,
                      uint8_t **out, uint64_t *out_len);
/* data_len and cap are bytes; character counts are derived. */
int kvspaceXhNewSlack(int elem, const uint8_t *data, uint64_t data_len, uint64_t cap,
                      uint8_t **out, uint64_t *out_len);
int kvspaceXhNewTensor(const uint64_t *dims, uint32_t ndim, const char *elem,
                       const uint8_t *raw, uint64_t raw_len,
                       uint8_t **out, uint64_t *out_len);
int kvspaceXhNewPtr(const char *langtype, const char *path, uint64_t cap,
                    uint8_t **out, uint64_t *out_len);
int kvspaceXhNewExt(const char *langtype, const char *locator, uint64_t cap,
                    uint8_t **out, uint64_t *out_len);
int kvspaceXhNewCode(const char *langtype, const uint8_t *body, uint64_t body_len,
                     uint64_t cap, uint8_t **out, uint64_t *out_len);
int kvspaceXhReserve(uint8_t kind, const char *langtype, uint64_t body_len,
                     uint64_t cap, uint8_t **out, uint64_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
