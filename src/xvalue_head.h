/* Wire: [u8 pow][u8 kind][u64le a][u64le b][langtype]. headlen = 1<<pow, pow >= 5.
   langtype runs to the first 0, or to headlen-18 if none. Body follows the head.
   0 fixed-small, pow 5: a = b = 0, body width from langtype. Empty langtype is None.
   1 slack, pow 6: a = content bytes, b = capacity bytes, stored body is b bytes.
   2 fixed-large, pow 7: a = numel, b = esize, body = a * b.
   3 is rejected. */
#ifndef XVALUE_HEAD_H
#define XVALUE_HEAD_H

#include <stdint.h>

#define KVSPACE_XH_PREFIX 18u

#define KVSPACE_XH_FIXED_SMALL 0u
#define KVSPACE_XH_SLACK       1u
#define KVSPACE_XH_FIXED_LARGE 2u
#define KVSPACE_XH_EXT         3u

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
/* data_len and cap are bytes; character counts are derived. */
int kvspaceXhNewSlack(int elem, const uint8_t *data, uint64_t data_len, uint64_t cap,
                      uint8_t **out, uint64_t *out_len);
int kvspaceXhNewTensor(const uint64_t *dims, uint32_t ndim, const char *elem,
                       const uint8_t *raw, uint8_t **out, uint64_t *out_len);

#endif
