/*
 * kvspace.h — KVSpace C API
 *
 * file-backed mmap, ART 树索引 + slotsboxmalloc 变长存储.
 * 对齐 kvspace-go KVSpace 接口.
 */

#ifndef KVSPACE_H
#define KVSPACE_H

#include "coord.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct kvspace kvspace_t;

/* 对齐 kvspace/include/kvspace/kvspace.h：叶子 + 目录祖先。 */
typedef struct {
    uint32_t block_id;
    uint32_t gen;
    uint32_t parent_id;
    uint32_t depth;
} kvspaceRef_t;

/* ================================================================
 * 生命周期
 * ================================================================ */

// Files: <path>, <path>.sbo.head, <path>.sbo.data; all grow on demand.
// data_size = 8*64^k, create only, initial (not maximum) data size.
kvspace_t *kvspaceShmOpen(const char *path, size_t data_size);
void kvspaceShmClose(kvspace_t *kv);

/* ================================================================
 * 单点读写
 * ================================================================ */

// Get: resolve=1 穿透 link，resolve=0 返回 link 本身。
uint8_t *kvspaceShmGet(kvspace_t *kv, const char *key, int resolve,
                       int32_t *out_len);
int kvspaceShmMetaGet(kvspace_t *kv, const char *key, uint8_t *ro, uint32_t *vid);
int kvspaceShmMetaGetAt(kvspace_t *kv, const char *key, uint8_t *ro, uint32_t *vid);

int kvspaceShmResolveRef(kvspace_t *kv, const char *key, kvspaceRef_t *ref);
uint8_t *kvspaceShmGetByRef(kvspace_t *kv, kvspaceRef_t *ref,
                            const char *key_fallback, int32_t *out_len);
int kvspaceShmSetPartByRef(kvspace_t *kv, kvspaceRef_t *ref,
                           const char *key_fallback, uint32_t offset,
                           const uint8_t *buf, uint32_t buf_len);

// Set: 写入 value（TLV 编码的字节）。总是穿透 link 写入 target。
int kvspaceShmSet(kvspace_t *kv, const char *key, const uint8_t *val,
                  int32_t val_len);
int kvspaceShmSetValue(kvspace_t *kv, const char *key, const uint8_t *val,
                       int32_t val_len, uint8_t ro, uint32_t vid);

// 零拷贝写原语——返回 SHM 常驻 body 偏移指针供调用方直接写；写即持久，无收尾。
// WriteInPlace: key 必须已存在、body_len 必须等于原 body_len（同 kind
// 覆写）；否则返回非 0。
int kvspaceShmWriteInPlace(kvspace_t *kv, const char *key, int resolve,
                           int32_t body_len, uint8_t **body);
// WriteNewPlace: 按 (ref, storetype, ro, vid, langtype, body_len) 分配新 box、
// 写好 head、维护父索引，返回 body 指针。
int kvspaceShmWriteNewPlace(kvspace_t *kv, const char *key, uint8_t ref,
                            uint8_t storetype, uint8_t ro, uint32_t vid,
                            const char *langtype, int32_t body_len,
                            uint64_t body_cap,
                            uint8_t **body);
// ListLen: 只返回前缀下子项计数，无缓冲。
int kvspaceShmListLen(kvspace_t *kv, const char *prefix, bool expand_ext,
                      int resolve, int32_t *out_count);

/* ================================================================
 * 目录操作
 * ================================================================ */

// List: resolve=1 穿透 link 列出 target 子节点。
int kvspaceShmList(kvspace_t *kv, const char *prefix, bool expand_ext,
                   int resolve, char ***out_names, int32_t *out_count);

int kvspaceShmDel(kvspace_t *kv, const char *key);
int kvspaceShmDeltree(kvspace_t *kv, const char *prefix);
int kvspaceShmCp(kvspace_t *kv, const char *src,
                 const char *dst); // 单 key 拷贝
int kvspaceShmCptree(kvspace_t *kv, const char *src,
                     const char *dst); // 递归子树拷贝
int kvspaceShmCplist(kvspace_t *kv, const char *src,
                     const char *dst); // 浅拷贝：base 值 + 一层 · 成员，不递归
int kvspaceShmMkindex(
    kvspace_t *kv, const char *path,
    uint32_t capacity); // 递归创建目录，capacity 预留叶 index 容量（减少扩容）

/* ================================================================
 * ExtIndex
 * ================================================================ */

int kvspaceShmExtindex(kvspace_t *kv, const char *path, const char *extpath);
int kvspaceShmDelextindex(kvspace_t *kv, const char *path); // 移除 extindex

/* ================================================================
 * Watch / Notify
 * ================================================================ */

int kvspaceShmNotify(kvspace_t *kv, const char *key, const uint8_t *val,
                     int32_t val_len);

// 阻塞等待通知，timeout_ms 毫秒。返回 malloc TLV，超时返回 NULL.
uint8_t *kvspaceShmWatch(kvspace_t *kv, const char *key, int32_t timeout_ms,
                         int32_t *out_len);

#endif
