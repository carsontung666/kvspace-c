#ifndef KVSPACE_COORD_H
#define KVSPACE_COORD_H
#include <stdint.h>
int kvspaceCoordIsCoord(const char *name);
int kvspaceParseCoord(const char *name, int64_t *coords, int maxn);
int kvspaceCoordCmp(const char *a, const char *b);
#endif
