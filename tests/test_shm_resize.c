/* test_shm_resize [dir] — sbo head pool doubling, data x64 re-root, PUNCH_HOLE
 * (#15) */

#define _GNU_SOURCE
#include "kvspace_shm.h"
#include "xvalue_head.h"
#include "test_util.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DATA_2MB (8UL * 64 * 64 * 64)
#define DATA_128MB (8UL * 64 * 64 * 64 * 64)
#define HEAD_POOL_INIT (4UL * 1024 * 1024)
/* A value near 500 bytes consumes one L1 box; 14000 boxes grow the pool. */
#define HEAD_KEYS 14000
#define HEAD_VAL_RAW 360
/* 900KB = 29 slots of 32KB; the 2MB root has 64, so the third one re-roots. */
#define BIG_RAW (900 * 1024)

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

/* uint8 array, raw[j] = seed + j */
static int set_bytes(kvspace_t *kv, const char *key, int32_t n, int seed) {
  uint8_t *raw = malloc((size_t)n);
  for (int32_t j = 0; j < n; j++)
    raw[j] = (uint8_t)(seed + j);
  uint8_t *tlv;
  uint64_t dims[1] = {(uint64_t)n}, len = 0;
  if (kvspaceXhNewTensor(dims, 1, "uint8", raw, (uint64_t)n, &tlv, &len) != 0) {
    free(raw);
    return -1;
  }
  int rc = kvspaceShmSet(kv, key, tlv, (int32_t)len);
  free(tlv);
  free(raw);
  return rc;
}

/* 0 if present with the exact length and content */
static int check_bytes(kvspace_t *kv, const char *key, int32_t n, int seed) {
  int32_t len = 0;
  uint8_t *d = kvspaceShmGet(kv, key, 1, &len);
  if (!d || len <= 0)
    return 1;
  kvspaceXh h;
  if (kvspaceXhDecode(d, (uint64_t)len, &h) != 0 || h.content_len != (uint64_t)n)
    return 2;
  for (int32_t j = 0; j < n; j++)
    if (h.body[j] != (uint8_t)(seed + j))
      return 3;
  return 0;
}

static void head_key(char *buf, size_t n, int i) {
  snprintf(buf, n, "/h/%05d", i);
}

/* returns failure count */
static int fill_head(kvspace_t *kv) {
  char key[64];
  int fail = 0;
  for (int i = 0; i < HEAD_KEYS; i++) {
    head_key(key, sizeof key, i);
    fail += set_bytes(kv, key, HEAD_VAL_RAW, i) != 0;
  }
  return fail;
}

/* returns mismatch count over every step-th key */
static int check_head(kvspace_t *kv, int step) {
  char key[64];
  int bad = 0;
  for (int i = 0; i < HEAD_KEYS; i += step) {
    head_key(key, sizeof key, i);
    bad += check_bytes(kv, key, HEAD_VAL_RAW, i) != 0;
  }
  return bad;
}

static void t_head_grow(const char *dir) {
  printf("[head] %d values fill the 4MB box pool, grow, verify across reopen\n",
         HEAD_KEYS);
  char db[512], head[600], data[600];
  snprintf(db, sizeof db, "%s/hd", dir);
  snprintf(head, sizeof head, "%s.sbo.head", db);
  snprintf(data, sizeof data, "%s.sbo.data", db);
  kvspace_t *kv = kvspaceShmOpen(db, DATA_128MB);
  REQUIRE(kv != NULL, "open failed");
  off_t before = fsize(head);
  CHECK(before > (off_t)HEAD_POOL_INIT && before < (off_t)HEAD_POOL_INIT + 4096,
        "initial head size %ld", (long)before);

  double t0 = now_s();
  int fail = fill_head(kv);
  CHECK(fail == 0, "%d sets failed", fail);
  off_t after = fsize(head);
  CHECK(after >= 2 * before - 4096, "head file did not grow: %ld -> %ld",
        (long)before, (long)after);
  CHECK(fsize(data) == (off_t)DATA_128MB, "data grew spuriously: %ld",
        (long)fsize(data));
  printf("  head file %ld -> %ld bytes, %.2fs\n", (long)before, (long)after,
         now_s() - t0);
  int bad = check_head(kv, 3);
  CHECK(bad == 0, "%d values unreadable after head grow", bad);
  kvspaceShmClose(kv);

  kv = kvspaceShmOpen(db, 8);
  REQUIRE(kv != NULL, "reopen failed");
  CHECK(fsize(head) == after, "reopen changed head size");
  bad = check_head(kv, 5);
  CHECK(bad == 0, "%d values unreadable after reopen", bad);
  CHECK(set_bytes(kv, "/h/after", HEAD_VAL_RAW, 7) == 0, "set after reopen");
  CHECK(check_bytes(kv, "/h/after", HEAD_VAL_RAW, 7) == 0, "get after reopen");
  kvspaceShmClose(kv);
}

static void t_data_grow(const char *dir) {
  printf("[data] third 900KB value in a 2MB store re-roots to 128MB, old "
         "values intact, verify across reopen\n");
  char db[512], data[600], head[600];
  snprintf(db, sizeof db, "%s/dg", dir);
  snprintf(data, sizeof data, "%s.sbo.data", db);
  snprintf(head, sizeof head, "%s.sbo.head", db);
  kvspace_t *kv = kvspaceShmOpen(db, DATA_2MB);
  REQUIRE(kv != NULL, "open failed");
  CHECK(fsize(data) == (off_t)DATA_2MB, "initial data size %ld",
        (long)fsize(data));
  off_t head_before = fsize(head);

  CHECK(set_bytes(kv, "/small", 100, 1) == 0, "set /small");
  CHECK(set_bytes(kv, "/big/1", BIG_RAW, 11) == 0, "set /big/1");
  CHECK(set_bytes(kv, "/big/2", BIG_RAW, 22) == 0, "set /big/2");
  CHECK(fsize(data) == (off_t)DATA_2MB, "grew too early: %ld",
        (long)fsize(data));

  CHECK(set_bytes(kv, "/big/3", BIG_RAW, 33) == 0, "set /big/3 (grow)");
  CHECK(fsize(data) == (off_t)DATA_128MB, "data did not grow to 128MB: %ld",
        (long)fsize(data));
  CHECK(fsize(head) == head_before, "head grew spuriously: %ld -> %ld",
        (long)head_before, (long)fsize(head));
  printf("  data file %lu -> %ld bytes\n", DATA_2MB, (long)fsize(data));

  CHECK(check_bytes(kv, "/small", 100, 1) == 0, "/small after grow");
  CHECK(check_bytes(kv, "/big/1", BIG_RAW, 11) == 0, "/big/1 after grow");
  CHECK(check_bytes(kv, "/big/2", BIG_RAW, 22) == 0, "/big/2 after grow");
  CHECK(check_bytes(kv, "/big/3", BIG_RAW, 33) == 0, "/big/3 after grow");

  /* freed space in the old subtree is reused; 3MB fits the new root */
  CHECK(kvspaceShmDel(kv, "/big/1") == 0, "del /big/1");
  CHECK(set_bytes(kv, "/big/1b", BIG_RAW, 44) == 0, "set /big/1b");
  CHECK(set_bytes(kv, "/huge", 3 * 1024 * 1024, 55) == 0, "set /huge 3MB");
  CHECK(fsize(data) == (off_t)DATA_128MB, "unexpected second grow");
  CHECK(check_bytes(kv, "/huge", 3 * 1024 * 1024, 55) == 0, "/huge");
  kvspaceShmClose(kv);

  kv = kvspaceShmOpen(db, 8);
  REQUIRE(kv != NULL, "reopen failed");
  CHECK(fsize(data) == (off_t)DATA_128MB, "reopen changed data size");
  CHECK(check_bytes(kv, "/small", 100, 1) == 0, "/small after reopen");
  CHECK(check_bytes(kv, "/big/2", BIG_RAW, 22) == 0, "/big/2 after reopen");
  CHECK(check_bytes(kv, "/big/3", BIG_RAW, 33) == 0, "/big/3 after reopen");
  CHECK(check_bytes(kv, "/big/1b", BIG_RAW, 44) == 0, "/big/1b after reopen");
  CHECK(check_bytes(kv, "/huge", 3 * 1024 * 1024, 55) == 0,
        "/huge after reopen");
  CHECK(set_bytes(kv, "/big/4", BIG_RAW, 66) == 0, "set after reopen");
  CHECK(check_bytes(kv, "/big/4", BIG_RAW, 66) == 0, "get after reopen");
  kvspaceShmClose(kv);
}

/* child attaches at 2MB / 4MB pool; parent re-roots and grows the pool;
   child then reads the new area and allocates from the grown pool */
static void t_cross_process(const char *dir) {
  printf("[xproc] one process re-roots and grows the pool, another syncs and "
         "reads/writes\n");
  char db[512], head[600];
  snprintf(db, sizeof db, "%s/xp", dir);
  snprintf(head, sizeof head, "%s.sbo.head", db);
  kvspace_t *kv = kvspaceShmOpen(db, DATA_2MB);
  REQUIRE(kv != NULL, "open failed");
  CHECK(set_bytes(kv, "/seed", 16, 1) == 0, "seed");

  int to_child[2], to_parent[2];
  REQUIRE(pipe(to_child) == 0 && pipe(to_parent) == 0, "pipe");
  pid_t pid = fork();
  REQUIRE(pid >= 0, "fork");
  if (pid == 0) {
    close(to_child[1]);
    close(to_parent[0]);
    kvspace_t *ckv = kvspaceShmOpen(db, DATA_2MB);
    if (!ckv)
      _exit(10);
    if (check_bytes(ckv, "/seed", 16, 1) != 0)
      _exit(11);
    char c = 'r';
    if (write(to_parent[1], &c, 1) != 1)
      _exit(12);
    if (read(to_child[0], &c, 1) != 1)
      _exit(13);
    if (check_bytes(ckv, "/big/3", BIG_RAW, 33) != 0)
      _exit(14);
    if (check_bytes(ckv, "/big/1", BIG_RAW, 11) != 0)
      _exit(15);
    char key[64];
    head_key(key, sizeof key, HEAD_KEYS - 1);
    if (check_bytes(ckv, key, HEAD_VAL_RAW, HEAD_KEYS - 1) != 0)
      _exit(16);
    if (set_bytes(ckv, "/from-child", BIG_RAW, 99) != 0)
      _exit(17);
    for (int i = 0; i < 100; i++) {
      snprintf(key, sizeof key, "/c/%03d", i);
      if (set_bytes(ckv, key, HEAD_VAL_RAW, i) != 0)
        _exit(18);
    }
    kvspaceShmClose(ckv);
    _exit(0);
  }
  close(to_child[0]);
  close(to_parent[1]);
  char c;
  CHECK(read(to_parent[0], &c, 1) == 1 && c == 'r', "child ready");

  CHECK(set_bytes(kv, "/big/1", BIG_RAW, 11) == 0, "set /big/1");
  CHECK(set_bytes(kv, "/big/2", BIG_RAW, 22) == 0, "set /big/2");
  CHECK(set_bytes(kv, "/big/3", BIG_RAW, 33) == 0, "set /big/3");
  off_t head_before = fsize(head);
  int fail = fill_head(kv);
  CHECK(fail == 0, "%d sets failed", fail);
  CHECK(fsize(head) > head_before, "parent did not grow head pool");
  c = 'g';
  CHECK(write(to_child[1], &c, 1) == 1, "signal child");

  int status = 0;
  CHECK(waitpid(pid, &status, 0) == pid, "waitpid");
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
        "child exit status %d (signal %d)",
        WIFEXITED(status) ? WEXITSTATUS(status) : -1,
        WIFSIGNALED(status) ? WTERMSIG(status) : 0);
  CHECK(check_bytes(kv, "/from-child", BIG_RAW, 99) == 0,
        "parent sees child's write");
  CHECK(check_bytes(kv, "/c/099", HEAD_VAL_RAW, 99) == 0,
        "parent sees child's small writes");
  kvspaceShmClose(kv);
}

static int punch_supported(const char *dir) {
#if !defined(__linux__)
  (void)dir;
  return 0; /* macOS 无 fallocate PUNCH_HOLE */
#else
  char p[600];
  snprintf(p, sizeof p, "%s/punch-probe", dir);
  int fd = open(p, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    return 0;
  int ok = ftruncate(fd, 1 << 20) == 0 &&
           fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0,
                     1 << 20) == 0;
  close(fd);
  unlink(p);
  return ok;
#endif
}

static void t_punch(const char *dir) {
  printf("[punch] deleting a 1MB value releases the data file's pages\n");
  if (!punch_supported(dir)) {
    printf("  (filesystem lacks PUNCH_HOLE, skipped)\n");
    return;
  }
  char db[512], data[600];
  snprintf(db, sizeof db, "%s/pn", dir);
  snprintf(data, sizeof data, "%s.sbo.data", db);
  kvspace_t *kv = kvspaceShmOpen(db, DATA_2MB);
  REQUIRE(kv != NULL, "open failed");
  off_t base = fphys(data);
  CHECK(set_bytes(kv, "/v", BIG_RAW, 5) == 0, "set /v");
  off_t used = fphys(data);
  CHECK(used - base >= (off_t)BIG_RAW - 65536, "phys after write %ld -> %ld",
        (long)base, (long)used);
  CHECK(kvspaceShmDel(kv, "/v") == 0, "del /v");
  off_t freed = fphys(data);
  CHECK(freed <= base + 65536, "phys after del %ld (base %ld)", (long)freed,
        (long)base);
  printf("  phys %ld -> %ld -> %ld bytes\n", (long)base, (long)used,
         (long)freed);
  /* growing overwrite goes through free+alloc: old box punched, new readable */
  CHECK(set_bytes(kv, "/w", BIG_RAW, 6) == 0, "set /w");
  CHECK(set_bytes(kv, "/w", BIG_RAW + 65536, 7) == 0, "overwrite /w bigger");
  CHECK(check_bytes(kv, "/w", BIG_RAW + 65536, 7) == 0, "/w after overwrite");
  kvspaceShmClose(kv);
}

/* growth code addresses the single-root pool layout; anything else is refused
 */
static void t_bad_root_slots(const char *dir) {
  printf("[root_slots] head file with root_slots != 1 is rejected\n");
  char db[512], head[600];
  snprintf(db, sizeof db, "%s/rs", dir);
  snprintf(head, sizeof head, "%s.sbo.head", db);
  kvspace_t *kv = kvspaceShmOpen(db, DATA_2MB);
  REQUIRE(kv != NULL, "open failed");
  kvspaceShmClose(kv);
  /* sbo_meta_t: magic[16] head_size data_size slot_bytes per_slot_meta -> 48 */
  int fd = open(head, O_RDWR);
  REQUIRE(fd >= 0, "open head");
  uint8_t one = 0;
  REQUIRE(pread(fd, &one, 1, 48) == 1 && one == 1, "root_slots byte is %u",
          one);
  uint8_t two = 2;
  CHECK(pwrite(fd, &two, 1, 48) == 1, "pwrite");
  CHECK(kvspaceShmOpen(db, DATA_2MB) == NULL, "root_slots=2 accepted");
  CHECK(pwrite(fd, &one, 1, 48) == 1, "restore");
  close(fd);
  kv = kvspaceShmOpen(db, DATA_2MB);
  CHECK(kv != NULL, "reopen after restore");
  kvspaceShmClose(kv);
}

int main(int argc, char **argv) {
  char tmpl[] = "/tmp/kvspace-resize-XXXXXX";
  const char *dir = argc > 1 ? argv[1] : mkdtemp(tmpl);
  if (!dir) {
    perror("mkdtemp");
    return 2;
  }

  t_data_grow(dir);
  t_cross_process(dir);
  t_punch(dir);
  t_bad_root_slots(dir);
  t_head_grow(dir);

  if (argc <= 1) {
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
      fprintf(stderr, "cleanup failed: %s\n", dir);
  }
  printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
  return failures ? 1 : 0;
}
