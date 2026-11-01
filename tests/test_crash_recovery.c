/* Crash recovery and zero-tearing verification for kvspace-c shm:
 * 1) Dead and zombie process spinlock recovery across ART and SBO locks;
 * 2) Zero-tearing CoW overwrites under SIGKILL mid-SetValue (large strings / variable-length values);
 * 3) Crash safety across ART prefix splits, node growth (N4->N16->N48->N256), and SBO arena growth. */
#define _GNU_SOURCE
#include "kvspace_shm.h"
#include "test_util.h"
#include "xvalue_head.h"

#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    _Atomic uint64_t ops;
} ctl_t;

static int make_tagged_str(char ch, size_t len, uint8_t **out, uint64_t *out_len) {
    char *buf = malloc(len);
    if (!buf) return -1;
    memset(buf, ch, len);
    int rc = kvspaceXvalueHeadNewSlack((const uint8_t *)buf, len, len, "char/utf8", out, out_len);
    free(buf);
    return rc;
}

static void verify_not_torn(kvspace_t *kv, const char *key, size_t len_a, char ch_a,
                            size_t len_b, char ch_b) {
    int32_t rl = 0;
    uint8_t *raw = kvspaceShmGet(kv, key, 0, &rl);
    REQUIRE(raw != NULL && rl >= (int32_t)KVSPACE_XVALUE_HEAD_LEN + 2,
            "missing or truncated value on %s (rl=%d)", key, rl);
    kvspaceXvalueHead hd;
    memcpy(&hd, raw, sizeof hd);
    uint64_t blen = kvspaceXvalueHeadLen(&hd);
    const uint8_t *ltd = NULL;
    uint16_t ltl = kvspaceXvalueHeadLangtype(raw, blen, &ltd);
    REQUIRE(ltl == 9 && memcmp(ltd, "char/utf8", 9) == 0,
            "corrupted langtype attr on %s (ltl=%u)", key, ltl);
    const uint8_t *body = raw + KVSPACE_XVALUE_HEAD_LEN;
    bool ok_a = (blen == len_a);
    if (ok_a) {
        for (size_t i = 0; i < len_a; i++) {
            if (body[i] != (uint8_t)ch_a) { ok_a = false; break; }
        }
    }
    bool ok_b = (blen == len_b);
    if (ok_b) {
        for (size_t i = 0; i < len_b; i++) {
            if (body[i] != (uint8_t)ch_b) { ok_b = false; break; }
        }
    }
    REQUIRE(ok_a || ok_b, "torn value on %s (blen=%llu, first=0x%02x, last=0x%02x)",
            key, (unsigned long long)blen,
            blen ? body[0] : 0u, blen ? body[blen - 1] : 0u);
}

static void test_cow_no_tearing_and_crash_churn(void) {
    char dir[] = "/tmp/kvspace-crash-XXXXXX";
    REQUIRE(mkdtemp(dir) != NULL, "mkdtemp");
    char path[256];
    snprintf(path, sizeof path, "%s/db", dir);

    ctl_t *ctl = mmap(NULL, sizeof(*ctl), PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    REQUIRE(ctl != MAP_FAILED, "mmap ctl");
    atomic_store(&ctl->ops, 0);

    /* Start with a tiny 512B SBO data arena so SBO data grow + head grow + ART splits
     * are exercised repeatedly under SIGKILL. */
    kvspace_t *kv = kvspaceShmOpen(path, 512);
    REQUIRE(kv != NULL, "initial ShmOpen(512)");

    uint8_t *va = NULL, *vb = NULL;
    uint64_t na = 0, nb = 0;
    REQUIRE(make_tagged_str('A', 300, &va, &na) == 0, "make A");
    REQUIRE(make_tagged_str('B', 950, &vb, &nb) == 0, "make B");

    for (int i = 0; i < 8; i++) {
        char k[64];
        snprintf(k, sizeof k, "/anchor/%d", i);
        REQUIRE(kvspaceShmSet(kv, k, va, (int32_t)na) == 0, "seed anchor %d", i);
    }
    REQUIRE(kvspaceShmSet(kv, "/hot/toggle", va, (int32_t)na) == 0, "seed toggle");
    kvspaceShmClose(kv);

    /* 25 rounds of SIGKILLing a writer mid-churn (both before and after waitpid). */
    for (int round = 0; round < 25; round++) {
        uint64_t base_ops = atomic_load_explicit(&ctl->ops, memory_order_relaxed);
        pid_t pid = fork();
        REQUIRE(pid >= 0, "fork");
        if (pid == 0) {
            kvspace_t *wkv = kvspaceShmOpen(path, 512);
            if (!wkv) _exit(2);
            uint64_t step = 0;
            for (;;) {
                const uint8_t *v = (step & 1) ? vb : va;
                int32_t vl = (int32_t)((step & 1) ? nb : na);
                (void)kvspaceShmSet(wkv, "/hot/toggle", v, vl);
                char dk[96];
                /* Exercise ART N4 -> N16 -> N48 -> N256 + prefix splits + Del. */
                snprintf(dk, sizeof dk, "/churn/r%d/b%03u/sub_%03u",
                         round & 3, (unsigned)(step & 255u), (unsigned)((step * 7u) & 63u));
                (void)kvspaceShmSet(wkv, dk, v, vl);
                if ((step & 7u) == 7u)
                    (void)kvspaceShmDel(wkv, dk);
                step++;
                atomic_fetch_add_explicit(&ctl->ops, 1, memory_order_relaxed);
            }
        }

        uint64_t target = (uint64_t)(40 + (round * 37) % 200);
        for (int wait_it = 0; wait_it < 2000; wait_it++) {
            if (atomic_load_explicit(&ctl->ops, memory_order_relaxed) - base_ops >= target)
                break;
            struct timespec ts = {0, 100000};
            nanosleep(&ts, NULL);
        }
        kill(pid, SIGKILL);

        /* Odd rounds: reopen and mutate BEFORE waitpid so zombie ('Z') takeover is exercised! */
        if (round & 1) {
            for (int z = 0; z < 200; z++) {
                char sp[64];
                snprintf(sp, sizeof sp, "/proc/%d/stat", (int)pid);
                FILE *f = fopen(sp, "r");
                if (!f) break;
                char buf[256];
                size_t r = fread(buf, 1, sizeof(buf) - 1, f);
                fclose(f);
                buf[r] = 0;
                char *rp = strrchr(buf, ')');
                if (rp && (rp[2] == 'Z' || rp[2] == 'X')) break;
                struct timespec ts = {0, 500000};
                nanosleep(&ts, NULL);
            }
        } else {
            int st = 0;
            waitpid(pid, &st, 0);
        }

        kvspace_t *rkv = kvspaceShmOpen(path, 512);
        REQUIRE(rkv != NULL, "reopen after SIGKILL round %d", round);
        verify_not_torn(rkv, "/hot/toggle", 300, 'A', 950, 'B');
        for (int i = 0; i < 8; i++) {
            char k[64];
            snprintf(k, sizeof k, "/anchor/%d", i);
            verify_not_torn(rkv, k, 300, 'A', 300, 'A');
        }
        /* Perform post-crash write + List to verify locks and ART/SBO trees are healthy. */
        char probe[64];
        snprintf(probe, sizeof probe, "/recovered/%d", round);
        CHECK(kvspaceShmSet(rkv, probe, (round & 1) ? vb : va,
                            (int32_t)((round & 1) ? nb : na)) == 0,
              "post-crash Set round %d", round);
        char **names = NULL;
        int32_t cnt = 0;
        CHECK(kvspaceShmList(rkv, "/anchor/", false, 0, &names, &cnt) == 0 && cnt == 8,
              "post-crash List /anchor/ round %d (cnt=%d)", round, cnt);
        for (int32_t i = 0; i < cnt; i++) free(names[i]);
        free(names);
        kvspaceShmClose(rkv);

        if (round & 1) {
            int st = 0;
            waitpid(pid, &st, 0);
        }
    }

    free(va);
    free(vb);
    munmap(ctl, sizeof(*ctl));
}

static void test_concurrent_writers_and_pc_cap(void) {
    char dir[] = "/tmp/kvspace-mpwrite-XXXXXX";
    REQUIRE(mkdtemp(dir) != NULL, "mkdtemp mp");
    char path[256];
    snprintf(path, sizeof path, "%s/db", dir);

    kvspace_t *kv = kvspaceShmOpen(path, 8UL * 64 * 64 * 64 * 64);
    REQUIRE(kv != NULL, "mp ShmOpen");

    /* Verify /vthread/<vid>/‥pc gets >= 1024B body_cap while doing CoW replacement. */
    uint8_t *p1 = NULL, *p2 = NULL;
    uint64_t np1 = 0, np2 = 0;
    const char *pck = "/vthread/42/\xE2\x80\xA5pc";
    REQUIRE(kvspaceXvalueHeadNewSlack((const uint8_t *)"/vthread/42/[1]/[1,0]", 21, 21, "", &p1, &np1) == 0, "p1");
    REQUIRE(kvspaceXvalueHeadNewSlack((const uint8_t *)"/vthread/42/[1]/[2]/[1,0]", 25, 25, "", &p2, &np2) == 0, "p2");
    REQUIRE(kvspaceShmSet(kv, pck, p1, (int32_t)np1) == 0, "set pck 1");
    kvspaceRef_t pr;
    memset(&pr, 0, sizeof pr);
    REQUIRE(kvspaceShmResolveRef(kv, pck, &pr) == 0, "resolve pck");
    kvspaceXvalueBodyView_t pv1, pv2;
    REQUIRE(kvspaceShmXvalueBodyView(kv, &pr, pck, &pv1) == 0, "view pck 1");
    CHECK(pv1.body_cap >= 1024, "pc body_cap >= 1024 (got %llu)", (unsigned long long)pv1.body_cap);
    REQUIRE(kvspaceShmSet(kv, pck, p2, (int32_t)np2) == 0, "set pck 2");
    REQUIRE(kvspaceShmXvalueBodyView(kv, &pr, pck, &pv2) == 0, "view pck 2");
    CHECK(pv2.body_cap >= 1024 && pv2.head != pv1.head, "pc CoW new box + body_cap >= 1024");
    free(p1);
    free(p2);

    /* Seed status + scalar counter; fork a background CoW writer and verify concurrent Del/Set never fails. */
    const char *stk = "/vthread/42/\xE2\x80\xA5status";
    uint8_t *sv = NULL, *iv = NULL, *va = NULL, *vb = NULL;
    uint64_t nsv = 0, niv = 0, na = 0, nb = 0;
    uint64_t zero = 0;
    REQUIRE(kvspaceXvalueHeadNewSlack((const uint8_t *)"running", 7, 16, "", &sv, &nsv) == 0, "sv");
    REQUIRE(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, (const uint8_t *)&zero, 8, "int64", &iv, &niv) == 0, "iv");
    REQUIRE(make_tagged_str('X', 200, &va, &na) == 0, "mp va");
    REQUIRE(make_tagged_str('Y', 600, &vb, &nb) == 0, "mp vb");
    REQUIRE(kvspaceShmSet(kv, stk, sv, (int32_t)nsv) == 0, "seed status");
    REQUIRE(kvspaceShmSet(kv, "/cnt/i", iv, (int32_t)niv) == 0, "seed cnt");

    pid_t bg = fork();
    REQUIRE(bg >= 0, "fork bg");
    if (bg == 0) {
        kvspace_t *bkv = kvspaceShmOpen(path, 8UL * 64 * 64 * 64 * 64);
        if (!bkv) _exit(2);
        for (uint64_t i = 1;; i++) {
            uint8_t *ci = NULL;
            uint64_t nci = 0;
            if (kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, (const uint8_t *)&i, 8, "int64", &ci, &nci) == 0) {
                (void)kvspaceShmSet(bkv, "/cnt/i", ci, (int32_t)nci);
                free(ci);
            }
            const uint8_t *cv = (i & 1) ? vb : va;
            int32_t cvl = (int32_t)((i & 1) ? nb : na);
            (void)kvspaceShmSet(bkv, "/bg/cow", cv, cvl);
        }
    }

    struct timespec ts = {0, 2000000};
    nanosleep(&ts, NULL);
    for (int iter = 0; iter < 50; iter++) {
        CHECK(kvspaceShmDel(kv, stk) == 0, "concurrent Del status iter %d", iter);
        int32_t gl = 0;
        CHECK(kvspaceShmGet(kv, stk, 0, &gl) == NULL, "status deleted iter %d", iter);
        CHECK(kvspaceShmSet(kv, stk, sv, (int32_t)nsv) == 0, "concurrent Set status iter %d", iter);
    }
    kill(bg, SIGKILL);
    int st = 0;
    waitpid(bg, &st, 0);

    /* Verify write_lock is recoverable if bg died holding it. */
    CHECK(kvspaceShmSet(kv, stk, sv, (int32_t)nsv) == 0, "post-kill Set status");
    kvspaceShmClose(kv);
    free(sv);
    free(iv);
    free(va);
    free(vb);
}

static void test_sbo_data_grow_split_recovery_and_status_cow(void) {
    char dir[] = "/tmp/kvspace-growsplit-XXXXXX";
    REQUIRE(mkdtemp(dir) != NULL, "mkdtemp growsplit");
    char path[256];
    snprintf(path, sizeof path, "%s/db", dir);

    /* 512B initial SBO arena; seed two keys, then crash inside sbo_data_grow
     * right after sbo_box_format(root) and before root->children[0] = nid. */
    kvspace_t *kv = kvspaceShmOpen(path, 512);
    REQUIRE(kv != NULL, "ShmOpen(512)");
    uint8_t *va = NULL, *vb = NULL;
    uint64_t na = 0, nb = 0;
    REQUIRE(make_tagged_str('K', 120, &va, &na) == 0, "make K");
    REQUIRE(make_tagged_str('G', 400, &vb, &nb) == 0, "make G");
    REQUIRE(kvspaceShmSet(kv, "/seed/0", va, (int32_t)na) == 0, "seed 0");
    REQUIRE(kvspaceShmSet(kv, "/seed/1", va, (int32_t)na) == 0, "seed 1");
    kvspaceShmClose(kv);

    pid_t gpid = fork();
    REQUIRE(gpid >= 0, "fork grower");
    if (gpid == 0) {
        setenv("P2_TEST_DIE_GROW_SPLIT", "1", 1);
        kvspace_t *gkv = kvspaceShmOpen(path, 512);
        if (!gkv) _exit(2);
        (void)kvspaceShmSet(gkv, "/grow/trigger", vb, (int32_t)nb);
        _exit(0);
    }
    int gst = 0;
    waitpid(gpid, &gst, 0);
    CHECK(WIFEXITED(gst) && WEXITSTATUS(gst) == 99,
          "grower exited via P2_TEST_DIE_GROW_SPLIT (st=%d)", gst);

    /* Reopen and verify pre-existing keys survived the interrupted root split
     * and subsequent allocations succeed cleanly. */
    kvspace_t *rkv = kvspaceShmOpen(path, 512);
    REQUIRE(rkv != NULL, "reopen after interrupted sbo_data_grow");
    verify_not_torn(rkv, "/seed/0", 120, 'K', 120, 'K');
    verify_not_torn(rkv, "/seed/1", 120, 'K', 120, 'K');
    CHECK(kvspaceShmSet(rkv, "/grow/after", vb, (int32_t)nb) == 0,
          "post-grow-crash Set");
    verify_not_torn(rkv, "/grow/after", 400, 'G', 400, 'G');

    /* Verify /vthread/<vid>/..status transitions ("running" <-> "done") never tear under SIGKILL. */
    const char *stk = "/vthread/99/\xE2\x80\xA5status";
    uint8_t *s_run = NULL, *s_done = NULL;
    uint64_t n_run = 0, n_done = 0;
    REQUIRE(kvspaceXvalueHeadNewSlack((const uint8_t *)"running", 7, 16, "char/utf8", &s_run, &n_run) == 0, "s_run");
    REQUIRE(kvspaceXvalueHeadNewSlack((const uint8_t *)"done", 4, 16, "char/utf8", &s_done, &n_done) == 0, "s_done");
    REQUIRE(kvspaceShmSet(rkv, stk, s_run, (int32_t)n_run) == 0, "seed status running");
    kvspaceShmClose(rkv);

    for (int r = 0; r < 10; r++) {
        pid_t spid = fork();
        REQUIRE(spid >= 0, "fork status writer");
        if (spid == 0) {
            kvspace_t *skv = kvspaceShmOpen(path, 512);
            if (!skv) _exit(2);
            for (uint64_t i = 0;; i++) {
                const uint8_t *sv = (i & 1) ? s_done : s_run;
                int32_t svl = (int32_t)((i & 1) ? n_done : n_run);
                (void)kvspaceShmSet(skv, stk, sv, svl);
            }
        }
        struct timespec ts = {0, 1500000};
        nanosleep(&ts, NULL);
        kill(spid, SIGKILL);
        int sst = 0;
        waitpid(spid, &sst, 0);

        kvspace_t *ckv = kvspaceShmOpen(path, 512);
        REQUIRE(ckv != NULL, "reopen status round %d", r);
        int32_t srl = 0;
        uint8_t *sraw = kvspaceShmGet(ckv, stk, 0, &srl);
        REQUIRE(sraw != NULL && srl >= (int32_t)KVSPACE_XVALUE_HEAD_LEN + 2,
                "status readable round %d", r);
        kvspaceXvalueHead shd;
        memcpy(&shd, sraw, sizeof shd);
        uint64_t slen = kvspaceXvalueHeadLen(&shd);
        const uint8_t *sbody = sraw + KVSPACE_XVALUE_HEAD_LEN;
        bool ok_st = (slen == 7 && memcmp(sbody, "running", 7) == 0) ||
                     (slen == 4 && memcmp(sbody, "done", 4) == 0);
        CHECK(ok_st, "status torn round %d (slen=%llu)", r, (unsigned long long)slen);
        kvspaceShmClose(ckv);
    }

    free(s_run);
    free(s_done);
    free(va);
    free(vb);
}

static void test_art_n48_and_setpart_cow_no_tearing(void) {
    char dir[] = "/tmp/kvspace-n48part-XXXXXX";
    REQUIRE(mkdtemp(dir) != NULL, "mkdtemp n48part");
    char path[256];
    snprintf(path, sizeof path, "%s/db", dir);

    kvspace_t *kv = kvspaceShmOpen(path, 8UL * 64 * 64 * 64 * 64);
    REQUIRE(kv != NULL, "n48part ShmOpen");

    /* Insert 80 sibling keys under one prefix to exercise N4 -> N16 -> N48 -> N256 under SIGKILL churn. */
    uint8_t *va = NULL, *vb = NULL;
    uint64_t na = 0, nb = 0;
    REQUIRE(make_tagged_str('A', 64, &va, &na) == 0, "n48 va");
    REQUIRE(make_tagged_str('B', 64, &vb, &nb) == 0, "n48 vb");
    for (int i = 0; i < 20; i++) {
        char k[64];
        snprintf(k, sizeof k, "/fan/%c", (char)(33 + i));
        REQUIRE(kvspaceShmSet(kv, k, va, (int32_t)na) == 0, "seed fan %d", i);
    }

    /* Seed a 64-byte SLACK value and verify SetPartByRef >8B never tears under SIGKILL. */
    uint8_t *sp0 = NULL;
    uint64_t nsp0 = 0;
    char bodyA[64], bodyB[64];
    memset(bodyA, 'P', sizeof bodyA);
    memset(bodyB, 'Q', sizeof bodyB);
    REQUIRE(kvspaceXvalueHeadNewSlack((const uint8_t *)bodyA, 64, 64, "", &sp0, &nsp0) == 0, "sp0");
    REQUIRE(kvspaceShmSet(kv, "/part/64", sp0, (int32_t)nsp0) == 0, "seed /part/64");
    free(sp0);
    kvspaceShmClose(kv);

    for (int r = 0; r < 8; r++) {
        pid_t pid = fork();
        REQUIRE(pid >= 0, "fork n48part");
        if (pid == 0) {
            kvspace_t *wkv = kvspaceShmOpen(path, 8UL * 64 * 64 * 64 * 64);
            if (!wkv) _exit(2);
            kvspaceRef_t pref = {0};
            (void)kvspaceShmResolveRef(wkv, "/part/64", &pref);
            for (int step = 0;; step++) {
                int idx = 20 + (step % 60);
                char k[64];
                snprintf(k, sizeof k, "/fan/%c", (char)(33 + idx));
                (void)kvspaceShmSet(wkv, k, (step & 1) ? vb : va, (int32_t)na);
                const char *pb = (step & 1) ? bodyB : bodyA;
                (void)kvspaceShmSetPartByRef(wkv, &pref, "/part/64",
                                             KVSPACE_XVALUE_HEAD_LEN,
                                             (const uint8_t *)pb, 64);
            }
        }
        struct timespec ts = {0, 1500000};
        nanosleep(&ts, NULL);
        kill(pid, SIGKILL);
        int st = 0;
        waitpid(pid, &st, 0);

        kvspace_t *ckv = kvspaceShmOpen(path, 8UL * 64 * 64 * 64 * 64);
        REQUIRE(ckv != NULL, "reopen n48part %d", r);
        for (int i = 0; i < 20; i++) {
            char k[64];
            snprintf(k, sizeof k, "/fan/%c", (char)(33 + i));
            verify_not_torn(ckv, k, 64, 'A', 64, 'A');
        }
        int32_t prl = 0;
        uint8_t *praw = kvspaceShmGet(ckv, "/part/64", 0, &prl);
        REQUIRE(praw != NULL && prl >= (int32_t)KVSPACE_XVALUE_HEAD_LEN + 64,
                "get /part/64 round %d", r);
        const uint8_t *pbody = praw + KVSPACE_XVALUE_HEAD_LEN;
        bool allP = memcmp(pbody, bodyA, 64) == 0;
        bool allQ = memcmp(pbody, bodyB, 64) == 0;
        CHECK(allP || allQ, "/part/64 torn on round %d", r);
        kvspaceShmClose(ckv);
    }

    free(va);
    free(vb);
}

int main(void) {
    test_cow_no_tearing_and_crash_churn();
    test_concurrent_writers_and_pc_cap();
    test_sbo_data_grow_split_recovery_and_status_cow();
    test_art_n48_and_setpart_cow_no_tearing();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("crash_recovery: ok");
    return 0;
}
