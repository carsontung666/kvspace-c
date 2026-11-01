/* P1b4-b1a：shm slot view 语义。
 * 覆盖 cap/body_cap 自洽、status 固定形状与容量、旧小 cap 槽拒绝、
 * content 越界拒绝、10 万次写指针稳定。 */

#define _GNU_SOURCE
#include "kvspace_shm.h"
#include "xvalue_head.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "failed: %s (line %d)\n", #x, __LINE__);           \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static uint8_t *slack(const char *s, uint64_t *n) {
    uint8_t *v = NULL;
    if (kvspaceXvalueHeadNewSlack( (const uint8_t *)s, strlen(s),
                          strlen(s), "", &v, n) != 0)
        return NULL;
    return v;
}

int main(void) {
    char dir[] = "/tmp/kvspace-slot-view-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[128];
    snprintf(path, sizeof path, "%s/db", dir);
    kvspace_t *kv = kvspaceShmOpen(path, 8UL * 64 * 64 * 64 * 64);
    CHECK(kv != NULL);

    /* 普通 SLACK 槽：cap/body_cap 自洽、content 不越界。 */
    uint64_t n = 0;
    uint8_t *v = slack("hello", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, "/a/s", v, (int32_t)n) == 0);
    free(v);
    kvspaceRef_t r;
    memset(&r, 0, sizeof r);
    CHECK(kvspaceShmResolveRef(kv, "/a/s", &r) == 0);
    kvspaceXvalueBodyView_t view;
    CHECK(kvspaceShmXvalueBodyView(kv, &r, "/a/s", &view) == 0);
    CHECK(view.head && view.body == view.head + view.headlen);
    CHECK(view.cap >= view.headlen && view.body_cap == view.cap - view.headlen);
    kvspaceXvalueHead xh;
    memcpy(&xh, view.head, sizeof xh);
    CHECK(kvspaceXvalueHeadLen(&xh) <= view.body_cap);

    /* key_fallback 必填：NULL → -1。 */
    kvspaceXvalueBodyView_t nv;
    CHECK(kvspaceShmXvalueBodyView(kv, &r, NULL, &nv) == -1);

    /* status: short / 16B values succeed via CoW >=80B boxes; identical overwrites reuse box. */
    const char *sk = "/vthread/7/\xE2\x80\xA5status";
    v = slack("init", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, sk, v, (int32_t)n) == 0);
    free(v);
    memset(&r, 0, sizeof r);
    CHECK(kvspaceShmResolveRef(kv, sk, &r) == 0);
    CHECK(kvspaceShmXvalueBodyView(kv, &r, sk, &view) == 0);
    CHECK(view.headlen == KVSPACE_XVALUE_HEAD_LEN);
    CHECK(view.body_cap >= 16);

    v = slack("running", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, sk, v, (int32_t)n) == 0);
    free(v);
    CHECK(kvspaceShmXvalueBodyView(kv, &r, sk, &view) == 0);
    uint8_t *head0 = view.head, *body0 = view.body;
    uint64_t cap0 = view.cap;
    CHECK(view.body_cap >= 16);

    for (int i = 0; i < 100000; i++) {
        v = slack("running", &n);
        CHECK(v != NULL);
        CHECK(kvspaceShmSet(kv, sk, v, (int32_t)n) == 0);
        free(v);
        kvspaceXvalueBodyView_t again;
        CHECK(kvspaceShmXvalueBodyView(kv, &r, sk, &again) == 0);
        CHECK(again.head == head0 && again.body == body0 && again.cap == cap0);
    }
    v = slack("0123456789abcdef", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, sk, v, (int32_t)n) == 0);
    free(v);
    v = slack("0123456789abcdefg", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, sk, v, (int32_t)n) != 0);
    free(v);
    uint8_t raw[8] = {1};
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, raw, 8, "", &v, &n) == 0);
    CHECK(kvspaceShmSet(kv, sk, v, (int32_t)n) != 0);
    free(v);

    /* status 槽（固定头 SLACK）：创建即 view 成功（类型在 attr key，不在 head）。 */
    const char *lk = "/vthread/8/\xE2\x80\xA5status";
    uint8_t *lbody = NULL;
    CHECK(kvspaceShmWriteNewPlace(kv, lk, 0, KVSPACE_XVALUE_ST_SLACK, 0, 0, "", &lbody) == 0);
    kvspaceRef_t lr;
    memset(&lr, 0, sizeof lr);
    CHECK(kvspaceShmResolveRef(kv, lk, &lr) == 0);
    kvspaceXvalueBodyView_t lv;
    CHECK(kvspaceShmXvalueBodyView(kv, &lr, lk, &lv) == 0);
    v = slack("init", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, lk, v, (int32_t)n) == 0);
    free(v);

    /* 非 SLACK 形态（SHORT）作 status：view -1、写入硬错。 */
    const char *wk = "/vthread/9/\xE2\x80\xA5status";
    uint8_t *wbody = NULL;
    CHECK(kvspaceShmWriteNewPlace(kv, wk, 0, KVSPACE_XVALUE_ST_SHORT, 0, 0, "", &wbody) == 0);
    kvspaceRef_t wr;
    memset(&wr, 0, sizeof wr);
    CHECK(kvspaceShmResolveRef(kv, wk, &wr) == 0);
    kvspaceXvalueBodyView_t wv;
    CHECK(kvspaceShmXvalueBodyView(kv, &wr, wk, &wv) == -1);
    v = slack("init", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, wk, v, (int32_t)n) != 0);
    free(v);

    /* status content 被篡改成 17：view -1。 */
    kvspaceRef_t s17;
    memset(&s17, 0, sizeof s17);
    CHECK(kvspaceShmResolveRef(kv, sk, &s17) == 0);
    kvspaceXvalueHead h17;
    memset(&h17, 0, sizeof h17);
    kvspaceXvalueHeadSetLen(&h17, 17);
    int32_t s17len = 0;
    uint8_t *s17raw = kvspaceShmGet(kv, sk, 0, &s17len);
    CHECK(s17raw != NULL && s17len >= (int32_t)(2 + sizeof h17));
    memcpy(s17raw + 2, &h17, sizeof h17);
    kvspaceXvalueBodyView_t v17;
    CHECK(kvspaceShmXvalueBodyView(kv, &s17, sk, &v17) == -1);
    CHECK(kvspaceShmXvalueBodyView(kv, &s17, NULL, &v17) == -1);

    /* 越界 content（head.len 被改大）：view -1。 */
    v = slack("hello", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, "/a/corrupt", v, (int32_t)n) == 0);
    free(v);
    kvspaceXvalueHead big;
    memset(&big, 0, sizeof big);
    kvspaceXvalueHeadSetLen(&big, 0xF4240); /* 1,000,000 ≫ box 物理容量 */
    kvspaceRef_t xr;
    memset(&xr, 0, sizeof xr);
    CHECK(kvspaceShmResolveRef(kv, "/a/corrupt", &xr) == 0);
    int32_t xrlen = 0;
    uint8_t *xrraw = kvspaceShmGet(kv, "/a/corrupt", 0, &xrlen);
    CHECK(xrraw != NULL && xrlen >= (int32_t)(2 + sizeof big));
    memcpy(xrraw + 2, &big, sizeof big);
    kvspaceRef_t cr;
    memset(&cr, 0, sizeof cr);
    CHECK(kvspaceShmResolveRef(kv, "/a/corrupt", &cr) == 0);
    kvspaceXvalueBodyView_t cv;
    CHECK(kvspaceShmXvalueBodyView(kv, &cr, "/a/corrupt", &cv) == -1);

    /* 不存在的 key → -1。 */
    kvspaceRef_t miss;
    memset(&miss, 0xff, sizeof miss);
    kvspaceXvalueBodyView_t mv;
    CHECK(kvspaceShmXvalueBodyView(kv, &miss, "/no/such", &mv) == -1);

    kvspaceShmClose(kv);
    printf("ok: slot view\n");
    return 0;
}
