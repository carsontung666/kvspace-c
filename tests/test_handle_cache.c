#define _GNU_SOURCE
#include "kvspace_shm.h"
#include "xvalue_head.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DATA_SIZE (8UL * 64 * 64)
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      failures++;                                                              \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
    }                                                                          \
  } while (0)

static int failures;

static int set_i64(kvspace_t *kv, const char *key, int64_t v) {
  uint8_t *tlv;
  uint64_t n = 0;
  if (kvspaceXhNewScalar("int64", (const uint8_t *)&v, 8, &tlv, &n) != 0)
    return -1;
  int rc = kvspaceShmSet(kv, key, tlv, (int32_t)n);
  free(tlv);
  return rc;
}

static int64_t get_i64(const uint8_t *d, int32_t len) {
  if (!d || len <= 0)
    return -999;
  kvspaceXh h;
  if (kvspaceXhDecode(d, (uint64_t)len, &h) != 0 || h.content_len != 8)
    return -999;
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v |= (uint64_t)h.body[i] << (i * 8);
  return (int64_t)v;
}

static uint64_t nsec(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

int main(void) {
  char dir[] = "/tmp/kvs-ref-XXXXXX";
  if (!mkdtemp(dir))
    return 1;
  char path[256];
  snprintf(path, sizeof path, "%s/s", dir);
  kvspace_t *kv = kvspaceShmOpen(path, DATA_SIZE);
  if (!kv) {
    fprintf(stderr, "open failed\n");
    return 1;
  }

  CHECK(set_i64(kv, "/a", 1) == 0);
  kvspaceRef_t ref;
  CHECK(kvspaceShmResolveRef(kv, "/a", &ref) == 0);
  int32_t len = 0;
  uint8_t *d = kvspaceShmGetByRef(kv, &ref, "/a", &len);
  CHECK(get_i64(d, len) == 1);
  uint32_t id0 = ref.block_id;

  CHECK(set_i64(kv, "/a", 42) == 0);
  d = kvspaceShmGetByRef(kv, &ref, "/a", &len);
  CHECK(get_i64(d, len) == 42);
  CHECK(ref.block_id == id0);

  CHECK(set_i64(kv, "/n", 7) == 0);
  kvspaceRef_t nr;
  CHECK(kvspaceShmResolveRef(kv, "/n", &nr) == 0);
  for (int i = 0; i < 5; i++) {
    char k[8];
    snprintf(k, sizeof k, "/n%d", i);
    CHECK(set_i64(kv, k, i) == 0);
  }
  d = kvspaceShmGetByRef(kv, &nr, "/n", &len);
  CHECK(get_i64(d, len) == 7);

  kvspaceShmDel(kv, "/a");
  len = 0;
  d = kvspaceShmGetByRef(kv, &ref, NULL, &len);
  CHECK(d == NULL);
  CHECK(set_i64(kv, "/a", 9) == 0);
  d = kvspaceShmGetByRef(kv, &ref, "/a", &len);
  CHECK(get_i64(d, len) == 9);

  CHECK(set_i64(kv, "/p", 100) == 0);
  kvspaceRef_t pr;
  CHECK(kvspaceShmResolveRef(kv, "/p", &pr) == 0);
  d = kvspaceShmGet(kv, "/p", 0, &len);
  CHECK(d && len > 8);
  kvspaceXh hh;
  CHECK(kvspaceXhDecode(d, (uint64_t)len, &hh) == 0);
  int64_t nv = 200;
  uint8_t raw[8];
  for (int i = 0; i < 8; i++)
    raw[i] = (uint8_t)((uint64_t)nv >> (8 * i));
  CHECK(kvspaceShmSetPartByRef(kv, &pr, "/p",
                               hh.headlen, raw,
                               8) == 0);
  d = kvspaceShmGetByRef(kv, &pr, "/p", &len);
  CHECK(get_i64(d, len) == 200);

  for (int i = 0; i < 64; i++) {
    char k[16];
    snprintf(k, sizeof k, "/sib/%02d", i);
    CHECK(set_i64(kv, k, i + 100) == 0);
  }
  for (int i = 0; i < 64; i++) {
    char k[16];
    snprintf(k, sizeof k, "/sib/%02d", i);
    int32_t l = 0;
    d = kvspaceShmGet(kv, k, 0, &l);
    CHECK(get_i64(d, l) == i + 100);
  }
  CHECK(set_i64(kv, "/other", 1) == 0);
  {
    int32_t l = 0;
    d = kvspaceShmGet(kv, "/sib/00", 0, &l);
    CHECK(get_i64(d, l) == 100);
    d = kvspaceShmGet(kv, "/other", 0, &l);
    CHECK(get_i64(d, l) == 1);
    kvspaceRef_t sr;
    CHECK(set_i64(kv, "/frm/a", 1) == 0);
    CHECK(set_i64(kv, "/frm/i", 2) == 0);
    CHECK(set_i64(kv, "/frm/n", 3) == 0);
    CHECK(kvspaceShmResolveRef(kv, "/frm/a", &sr) == 0);
    CHECK(sr.depth > 0 && sr.parent_id != 0);
    d = kvspaceShmGetByRef(kv, &sr, "/frm/a", &l);
    CHECK(get_i64(d, l) == 1);
    {
      kvspaceRef_t pr = { sr.parent_id, sr.depth, 0, 0 };
      d = kvspaceShmGetByRef(kv, &pr, "/frm/i", &l);
      CHECK(get_i64(d, l) == 2);
      CHECK(get_i64(d, l) != 1);
      d = kvspaceShmGetByRef(kv, &pr, "/frm/n", &l);
      CHECK(get_i64(d, l) == 3);
      CHECK(get_i64(d, l) != 1);
    }
    CHECK(set_i64(kv, "/map/[1]", 11) == 0);
    CHECK(set_i64(kv, "/map/[2]", 22) == 0);
    CHECK(set_i64(kv, "/map/[3]", 33) == 0);
    {
      kvspaceRef_t mr;
      CHECK(kvspaceShmResolveRef(kv, "/map/[1]", &mr) == 0);
      CHECK(mr.depth > 0 && mr.parent_id != 0);
      kvspaceRef_t mp = { mr.parent_id, mr.depth, 0, 0 };
      d = kvspaceShmGetByRef(kv, &mp, "/map/[2]", &l);
      CHECK(get_i64(d, l) == 22);
      CHECK(get_i64(d, l) != 11);
      d = kvspaceShmGetByRef(kv, &mp, "/map/[3]", &l);
      CHECK(get_i64(d, l) == 33);
      CHECK(get_i64(d, l) != 11);
    }
    {
      kvspaceRef_t sr0;
      CHECK(kvspaceShmResolveRef(kv, "/sib/00", &sr0) == 0);
      CHECK(sr0.depth > 0);
      kvspaceRef_t sp = { sr0.parent_id, sr0.depth, 0, 0 };
      for (int i = 0; i < 64; i++) {
        char k[16];
        snprintf(k, sizeof k, "/sib/%02d", i);
        int32_t l2 = 0;
        d = kvspaceShmGetByRef(kv, &sp, k, &l2);
        CHECK(get_i64(d, l2) == i + 100);
      }
    }
    {
      const int N = 200;
      for (int i = 0; i < N; i++) {
        char k[128];
        snprintf(k, sizeof k, "/vthread/vt0/[0]/h/[%d]",
                 (int)((unsigned)i * 2654435761u % 100003));
        CHECK(set_i64(kv, k, i) == 0);
      }
      char k0[128];
      snprintf(k0, sizeof k0, "/vthread/vt0/[0]/h/[%d]", 0);
      kvspaceRef_t hr;
      CHECK(kvspaceShmResolveRef(kv, k0, &hr) == 0);
      CHECK(hr.depth > 0 && hr.parent_id != 0);
      kvspaceRef_t hp = { hr.parent_id, hr.depth, 0, 0 };
      for (int i = 0; i < N; i++) {
        char k[128];
        snprintf(k, sizeof k, "/vthread/vt0/[0]/h/[%d]",
                 (int)((unsigned)i * 2654435761u % 100003));
        int32_t l2 = 0;
        d = kvspaceShmGetByRef(kv, &hp, k, &l2);
        CHECK(get_i64(d, l2) == i);
        CHECK(get_i64(d, l2) != i + 1);
      }
    }
    /* kvlang map slots use member '·', not '/'. */
    {
      const int N = 200;
#define MSEP "\xC2\xB7"
      for (int i = 0; i < N; i++) {
        char k[128];
        snprintf(k, sizeof k, "/vthread/vt0/[0]" MSEP "h" MSEP "[%d]",
                 (int)((unsigned)i * 2654435761u % 100003));
        CHECK(set_i64(kv, k, i) == 0);
      }
      char k0[128];
      snprintf(k0, sizeof k0, "/vthread/vt0/[0]" MSEP "h" MSEP "[0]");
      kvspaceRef_t hr;
      CHECK(kvspaceShmResolveRef(kv, k0, &hr) == 0);
      CHECK(hr.depth > 0 && hr.parent_id != 0);
      kvspaceRef_t hp = { hr.parent_id, hr.depth, 0, 0 };
      int hits = 0;
      for (int i = 0; i < N; i++) {
        char k[128];
        snprintf(k, sizeof k, "/vthread/vt0/[0]" MSEP "h" MSEP "[%d]",
                 (int)((unsigned)i * 2654435761u % 100003));
        int32_t l2 = 0;
        d = kvspaceShmGetByRef(kv, &hp, k, &l2);
        if (get_i64(d, l2) == i)
          hits++;
        CHECK(get_i64(d, l2) == i);
        CHECK(get_i64(d, l2) != i + 1);
      }
      CHECK(hits == N);
      for (int i = 0; i < 32; i++) {
        char k[128];
        snprintf(k, sizeof k, "/vthread/vt0/[0]" MSEP "arr" MSEP "[%d]", i);
        CHECK(set_i64(kv, k, i + 50) == 0);
      }
      snprintf(k0, sizeof k0, "/vthread/vt0/[0]" MSEP "arr" MSEP "[0]");
      CHECK(kvspaceShmResolveRef(kv, k0, &hr) == 0);
      hp.block_id = hr.parent_id;
      hp.gen = hr.depth;
      for (int i = 0; i < 32; i++) {
        char k[128];
        snprintf(k, sizeof k, "/vthread/vt0/[0]" MSEP "arr" MSEP "[%d]", i);
        int32_t l2 = 0;
        d = kvspaceShmGetByRef(kv, &hp, k, &l2);
        CHECK(get_i64(d, l2) == i + 50);
      }
      /* kv.get/kv.set map slots are unbracketed: base·42 */
      for (int i = 0; i < N; i++) {
        char k[128];
        snprintf(k, sizeof k, "/vthread/vt0/[0]" MSEP "m" MSEP "%d",
                 (int)((unsigned)i * 2654435761u % 100003));
        CHECK(set_i64(kv, k, i + 1000) == 0);
      }
      snprintf(k0, sizeof k0, "/vthread/vt0/[0]" MSEP "m" MSEP "0");
      CHECK(kvspaceShmResolveRef(kv, k0, &hr) == 0);
      CHECK(hr.depth > 0 && hr.parent_id != 0);
      hp.block_id = hr.parent_id;
      hp.gen = hr.depth;
      hits = 0;
      for (int i = 0; i < N; i++) {
        char k[128];
        snprintf(k, sizeof k, "/vthread/vt0/[0]" MSEP "m" MSEP "%d",
                 (int)((unsigned)i * 2654435761u % 100003));
        int32_t l2 = 0;
        d = kvspaceShmGetByRef(kv, &hp, k, &l2);
        if (get_i64(d, l2) == i + 1000)
          hits++;
        CHECK(get_i64(d, l2) == i + 1000);
      }
      CHECK(hits == N);
#undef MSEP
    }
  }

  const int N = 200000;
  CHECK(set_i64(kv, "/hot", 1) == 0);
  kvspaceRef_t hr;
  CHECK(kvspaceShmResolveRef(kv, "/hot", &hr) == 0);
  uint64_t t0 = nsec();
  for (int i = 0; i < N; i++) {
    int32_t l = 0;
    kvspaceShmGet(kv, "/hot", 0, &l);
  }
  uint64_t t1 = nsec();
  for (int i = 0; i < N; i++) {
    int32_t l = 0;
    kvspaceShmGetByRef(kv, &hr, "/hot", &l);
  }
  uint64_t t2 = nsec();
  double ns_get = (double)(t1 - t0) / N;
  double ns_ref = (double)(t2 - t1) / N;
  printf("Get %.1f ns/op  GetByRef %.1f ns/op  ratio %.2f\n", ns_get, ns_ref,
         ns_ref > 0 ? ns_get / ns_ref : 0);

  const char *fk[] = {"/frm/a", "/frm/i", "/frm/n"};
  const int NF = 3;
  const int NR = 50000;
  uint64_t t3 = nsec();
  for (int r = 0; r < NR; r++) {
    int32_t l = 0;
    kvspaceShmGet(kv, fk[r % NF], 0, &l);
  }
  uint64_t t4 = nsec();
  kvspaceRef_t sr, pref;
  CHECK(kvspaceShmResolveRef(kv, fk[0], &sr) == 0);
  CHECK(sr.depth > 0);
  pref.block_id = sr.parent_id;
  pref.gen = sr.depth;
  pref.parent_id = 0;
  pref.depth = 0;
  uint64_t t5 = nsec();
  for (int r = 0; r < NR; r++) {
    int32_t l = 0;
    kvspaceShmGetByRef(kv, &pref, fk[r % NF], &l);
  }
  uint64_t t6 = nsec();
  printf("Get sibling %.1f ns/op  GetByRef sibling %.1f ns/op  ratio %.2f\n",
         (double)(t4 - t3) / NR, (double)(t6 - t5) / NR,
         (t6 - t5) > 0 ? (double)(t4 - t3) / (double)(t6 - t5) : 0);

  kvspaceShmClose(kv);
  return failures ? 1 : 0;
}
