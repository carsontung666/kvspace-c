#ifndef XVALUE_META_H
#define XVALUE_META_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int kvspaceMetaKey(const char *key, char **out);
int kvspaceMetaEncode(uint8_t ro, uint32_t vid, uint8_t **out, uint64_t *out_len);
int kvspaceMetaDecode(const uint8_t *data, uint64_t len, uint8_t *ro, uint32_t *vid);

#ifdef __cplusplus
}
#endif

#endif
