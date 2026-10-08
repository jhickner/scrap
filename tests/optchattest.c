#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <pthread.h>

#include "vendor/agents/backend.h"
#define OC_RETRY_MS 50
#define OPTCHAT_IMPLEMENTATION
#include "vendor/agents/core/optchat.h"

static int failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                          \
        }                                                                        \
    } while (0)

#define NODE 64
#define VIEW 1200

static void tiles(oc_mem *m)
{
    oc_ref *p;
    int  np  = oc_view(m, &p);
    long at  = 0;
    for (int k = 0; k < np; k++) {
        CHECK(p[k].i << p[k].l == at);
        at += 1L << p[k].l;
    }
    free(p);
    CHECK(at == oc_count(m));
}

static int covered(const oc_ref *old, int nold, oc_mem *m)
{
    oc_ref *p;
    int np = oc_view(m, &p), all = 1;
    for (int k = 0; k < nold && all; k++) {
        int ok = 0;
        for (int j = 0; j < np && !ok; j++)
            ok = p[j].l >= old[k].l && (old[k].i >> (p[j].l - old[k].l)) == p[j].i;
        all = ok;
    }
    free(p);
    return all;
}

static void compact(oc_mem *m)
{
    oc_ref due[16];
    int    n;
    while ((n = oc_due(m, due, 16)) > 0) {
        for (int k = 0; k < n; k++) {
            char s[NODE + 1];
            snprintf(s, sizeof s, "sum l%d i%ld %.*s", due[k].l, due[k].i, 40,
                     "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
            CHECK(oc_put(m, due[k].l, due[k].i, s));
        }
    }
}

static size_t common(const char *a, const char *b)
{
    size_t n = 0;
    while (a[n] && a[n] == b[n])
        n++;
    return n;
}

static pthread_mutex_t fmu = PTHREAD_MUTEX_INITIALIZER;
static int fcalls, fbad, fids, fretries, fmerges, fscale;

static char *fake_ask(Backend *b, const char *user)
{
    (void)b;
    pthread_mutex_lock(&fmu);
    int n     = ++fcalls;
    int retry = !strncmp(user, "Too long: your line is ", 23);
    fbad     += strstr(user, "not summarized yet") != NULL;
    fids     += strstr(user, "+1|") != NULL;
    fretries += retry;
    fmerges  += strstr(user, "Merge only the two lines below") != NULL;
    fscale   += !retry && strstr(user, "the length of this ruler; never more than 64:\n"
                                       "------------------------------------------------------\n\n") != NULL;
    pthread_mutex_unlock(&fmu);
    if (n == 7)
        return NULL;
    if (!retry && n % 5 == 0) {
        char *s = malloc(NODE + 28);
        strcpy(s, "talk: ");
        memset(s + 6, 'z', NODE + 20);
        strcpy(s + NODE + 26, "\n");
        return s;
    }
    return strdup("  talk: summary of a long stretch of messages ab \n");
}

static int  fake_reset(Backend *b) { (void)b; return 1; }
static void fake_close(Backend *b) { free(b); }

static Backend *fake_open(void *ud, const char *system)
{
    (void)ud;
    CHECK(strstr(system, "You write the memory of scrap"));
    Backend *b = calloc(1, sizeof *b);
    b->ask     = fake_ask;
    b->reset   = fake_reset;
    b->close   = fake_close;
    return b;
}

static long long started;

static int too_long(void *ud)
{
    (void)ud;
    return oc_ms() - started > 20000;
}

static void compactor_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, NODE, VIEW, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    oc_compactor *c = oc_compactor_start(m, 4, fake_open, NULL);
    CHECK(c);

    char msg[200];
    for (int i = 0; i < 400; i++) {
        int len = 70 + (i * 13) % 100;
        memset(msg, 'a' + i % 26, (size_t)len);
        msg[len] = '\0';
        CHECK(oc_append(m, i % 2 ? "echo" : "user", msg) == i);
    }
    started = oc_ms();
    CHECK(oc_settle(m, too_long, NULL));
    oc_ref due[4];
    while (oc_due(m, due, 4) && !too_long(NULL))
        usleep(10000);
    CHECK(oc_due(m, due, 4) == 0);
    CHECK(oc_view_size(m) <= VIEW);
    tiles(m);

    for (long i = 0; i < 400; i++)
        CHECK(oc_node(m, 0, i) && strlen(oc_node(m, 0, i)) <= NODE + NODE * 15 / 100);
    CHECK(oc_node(m, 8, 0));
    CHECK(fbad == 0);
    CHECK(fids == 0);
    CHECK(fretries > 0);
    CHECK(fmerges > 0);
    CHECK(fscale > 0);
    char e[512];
    CHECK(oc_compactor_error(c, e, sizeof e) >= 1);
    CHECK(strstr(e, "failed: no reply"));

    oc_compactor_stop(c);
    oc_close(m);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        failures++;
}

static char *fail_ask(Backend *b, const char *user)
{
    (void)b;
    (void)user;
    return strdup("API Error: 400 messages.0.content.3.cache_control.ttl: a ttl='1h' cache_control block must not come after a ttl='5m' cache_control block.");
}

static Backend *fail_open(void *ud, const char *system)
{
    (void)ud;
    (void)system;
    Backend *b = calloc(1, sizeof *b);
    b->ask     = fail_ask;
    b->reset   = fake_reset;
    b->close   = fake_close;
    return b;
}

static int three_seconds(void *ud)
{
    (void)ud;
    return oc_ms() - started > 3000;
}

/* A summary that keeps failing must not hold the turn: settle returns once
 * nothing is in flight and only failing jobs are left, unsettled. */
static void failing_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, NODE, VIEW, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    oc_compactor *c = oc_compactor_start(m, 4, fail_open, NULL);
    CHECK(c);
    char msg[200];
    memset(msg, 'q', 150);
    msg[150] = '\0';
    CHECK(oc_append(m, "user", msg) == 0);
    started = oc_ms();
    CHECK(oc_settle(m, three_seconds, NULL));
    CHECK(oc_ms() - started < 1000);
    CHECK(!oc_settled(m));
    char e[512];
    CHECK(oc_compactor_error(c, e, sizeof e) >= 1 && strstr(e, "summary 0+1 failed"));
    oc_compactor_stop(c);
    oc_close(m);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        failures++;
}

static int lines_in(const char *a, const char *b)
{
    int n = 0;
    for (; a < b; a++)
        n += *a == '\n';
    return n;
}

static int count_marks(const char *s)
{
    int n = 0;
    for (const char *p = s; (p = strstr(p, BACKEND_CACHE_MARK)); p++)
        n++;
    return n;
}

static void marks_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, 0, 0, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    char msg[400];
    memset(msg, 'q', sizeof msg - 1);
    msg[sizeof msg - 1] = '\0';
    for (int i = 0; i < 300; i++)
        CHECK(oc_append(m, "user", msg) == i);
    char big[600];
    memset(big, 'w', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    long id = oc_append(m, "echo", big);
    CHECK(!oc_node(m, 0, id));

    pthread_mutex_lock(&m->mu);
    char *ctx = oc_context(m, (oc_ref){ 0, id });
    pthread_mutex_unlock(&m->mu);
    char *mark = strstr(ctx, BACKEND_CACHE_MARK), *close = strstr(ctx, "</chat>");
    CHECK(count_marks(ctx) == 1 && mark && mark[-1] == '\n' && close && mark < close);
    CHECK(mark && lines_in(ctx, mark) % OC_GRID == 1 && lines_in(mark, close) < OC_GRID);
    char *ask = strstr(ctx, "The chat above is context only. Compress only the message below");
    CHECK(close && ask && ask > close && strstr(ask, "include nothing said only in other messages, and no reason, comparison or number"));
    CHECK(ask && strstr(ask, "echo: www"));
    free(ctx);

    char *view = oc_render(m, 1), *plain = oc_render(m, 0);
    mark = strstr(view, BACKEND_CACHE_MARK);
    close = strstr(view, "</chat>");
    CHECK(count_marks(view) == 1 && mark && mark[-1] == '\n' && lines_in(view, mark) % OC_GRID == 1);
    CHECK(mark && lines_in(mark, close) < OC_GRID && !strstr(plain, BACKEND_CACHE_MARK));
    free(view);
    free(plain);
    oc_close(m);

    char dir2[] = "/tmp/optchattest.XXXXXX";
    if (!mkdtemp(dir2)) {
        failures++;
        return;
    }
    m = oc_open(dir2, 0, 0, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    long seen = 0;
    char *v = oc_render_from(m, 1, &seen);
    CHECK(!strcmp(v, "<chat>\n" BACKEND_CACHE_MARK "</chat>") && seen == 0);
    free(v);
    char s[32];
    for (int i = 0; i < 6; i++) {
        snprintf(s, sizeof s, "m%d", i);
        oc_append(m, "user", s);
    }
    v = oc_render_from(m, 1, &seen);
    CHECK(strstr(v, "3+1|user: m3\n" BACKEND_CACHE_MARK "4+1|") && count_marks(v) == 1 && seen == 4);
    free(v);
    for (int i = 6; i < 13; i++) {
        snprintf(s, sizeof s, "m%d", i);
        oc_append(m, "user", s);
    }
    v = oc_render_from(m, 1, &seen);
    CHECK(strstr(v, "3+1|user: m3\n" BACKEND_CACHE_MARK "4+1|") &&
          strstr(v, "11+1|user: m11\n" BACKEND_CACHE_MARK "12+1|") && count_marks(v) == 2 && seen == 12);
    free(v);
    v = oc_render_from(m, 1, &seen);
    CHECK(strstr(v, "11+1|user: m11\n" BACKEND_CACHE_MARK "12+1|") && count_marks(v) == 1 && seen == 12);
    free(v);
    v = oc_render_from(m, 0, &seen);
    CHECK(count_marks(v) == 0 && seen == 12);
    free(v);

    v = oc_render_turn(m);
    CHECK(count_marks(v) == 1 && m->seen == 12);
    free(v);
    oc_append(m, "user", "m13");
    oc_append(m, "user", "m14");
    oc_append(m, "user", "m15");
    oc_close(m);
    m = oc_open(dir2, 0, 0, err, sizeof err);
    CHECK(m && m->seen == 12);
    if (m) {
        v = oc_render_turn(m);
        CHECK(strstr(v, "11+1|user: m11\n" BACKEND_CACHE_MARK "12+1|") &&
              strstr(v, "15+1|user: m15\n" BACKEND_CACHE_MARK "</chat>") && count_marks(v) == 2 && m->seen == 16);
        free(v);
        oc_close(m);
    }
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s %s", dir, dir2);
    if (system(cmd))
        failures++;
}

static char *render_of(const char *dir, long view, oc_ref **parts, int *np, long *seen)
{
    char err[512];
    oc_mem *m = oc_open(dir, NODE, view, err, sizeof err);
    CHECK(m);
    if (!m)
        return strdup("");
    char *v = oc_render(m, 0);
    if (parts)
        *np = oc_view(m, parts);
    if (seen)
        *seen = m->seen;
    tiles(m);
    oc_close(m);
    return v;
}

static int same_parts(const oc_ref *a, int na, const oc_ref *b, int nb)
{
    if (na != nb)
        return 0;
    for (int k = 0; k < na; k++)
        if (a[k].l != b[k].l || a[k].i != b[k].i)
            return 0;
    return 1;
}

static void put_file(const char *dir, const char *leaf, const char *text)
{
    char path[600];
    snprintf(path, sizeof path, "%s/%s", dir, leaf);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static char *get_file(const char *dir, const char *leaf)
{
    char path[600];
    size_t len;
    snprintf(path, sizeof path, "%s/%s", dir, leaf);
    char *s = oc_slurp(path, &len);
    return s ? s : strdup("");
}

/* The view a restart opens is the one that was live, read from view.json, not
   one rebuilt from the log; a view.json that does not match the log is
   ignored, and one behind the log is caught up message by message. */
static void view_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, NODE, VIEW, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    char msg[200];
    for (int i = 0; i < 600; i++) {
        int len = 5 + (i * 37) % 150;
        memset(msg, 'a' + i % 26, (size_t)len);
        msg[len] = '\0';
        oc_append(m, i % 3 ? "tool" : "user", msg);
        if (i % 7 == 0)
            compact(m);
    }
    long seen = 0;
    free(oc_render_from(m, 1, &seen));
    m->seen = seen;
    m->dirty = 1;
    oc_save_view(m);
    oc_ref *live;
    int nlive = oc_view(m, &live);
    char *before = oc_render(m, 0);
    long live_seen = m->seen;
    oc_close(m);

    oc_ref *p1, *p2;
    int n1, n2;
    long s1, s2;
    char *r1 = render_of(dir, VIEW, &p1, &n1, &s1), *r2 = render_of(dir, VIEW, &p2, &n2, &s2);
    CHECK(!strcmp(before, r1) && !strcmp(r1, r2));
    CHECK(same_parts(live, nlive, p1, n1) && same_parts(p1, n1, p2, n2));
    CHECK(s1 == live_seen && s2 == live_seen && live_seen > 0);
    free(p1);
    free(p2);
    free(r2);

    char *saved = get_file(dir, "view.json");
    put_file(dir, "view.json", "{\"n\":999999,\"parts\":[[0,0]]}");
    char *rebuilt = render_of(dir, VIEW, NULL, NULL, &s2);
    CHECK(s2 == 0 && strcmp(rebuilt, before));
    free(rebuilt);
    put_file(dir, "view.json", "{\"n\":600,\"parts\":[[0,0],[0,2]]}");
    rebuilt = render_of(dir, VIEW, NULL, NULL, &s2);
    CHECK(s2 == 0);
    free(rebuilt);
    put_file(dir, "view.json", "not json");
    rebuilt = render_of(dir, VIEW, NULL, NULL, &s2);
    CHECK(s2 == 0);
    free(rebuilt);

    put_file(dir, "view.json", saved);
    m = oc_open(dir, NODE, VIEW, err, sizeof err);
    CHECK(m);
    oc_ref *after = NULL;
    int nafter = 0;
    if (m) {
        oc_append(m, "user", "one more");
        oc_append(m, "user", "and another");
        nafter = oc_view(m, &after);
        oc_close(m);
    }
    put_file(dir, "view.json", saved);
    oc_ref *p3;
    int n3;
    char *caught = render_of(dir, VIEW, &p3, &n3, &s2);
    CHECK(n3 >= 2 && p3[n3 - 1].l == 0 && p3[n3 - 1].i == 601 && p3[n3 - 2].l == 0 && p3[n3 - 2].i == 600);
    CHECK(same_parts(after, nafter, p3, n3) && s2 == live_seen);
    free(after);
    free(p3);
    free(caught);
    free(saved);
    free(live);
    free(before);
    free(r1);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        failures++;
}

static pthread_mutex_t smu = PTHREAD_MUTEX_INITIALIZER;
static int sactive, smax, sbad, scalls, soverlap, nfirst;
static char firsts[256][2048];
static int first_active[256];

static size_t chat_len(const char *user)
{
    const char *e = strstr(user, "</chat>");
    return e ? (size_t)(e - user) : strlen(user);
}

static char *slow_ask(Backend *b, const char *user)
{
    (void)b;
    pthread_mutex_lock(&smu);
    scalls++;
    sbad += strstr(user, "not summarized yet") != NULL;
    const char *mark = strstr(user, BACKEND_CACHE_MARK);
    size_t n = mark ? (size_t)(mark - user) : chat_len(user);
    int me = -1, k = 0;
    for (; k < nfirst; k++)
        if (strlen(firsts[k]) == n && !strncmp(firsts[k], user, n))
            break;
    if (k < nfirst)
        soverlap += first_active[k];
    else if (nfirst < 256 && n < sizeof firsts[0]) {
        me = nfirst++;
        snprintf(firsts[me], sizeof firsts[me], "%.*s", (int)n, user);
        first_active[me] = 1;
    }
    if (++sactive > smax)
        smax = sactive;
    pthread_mutex_unlock(&smu);
    usleep(40000);
    pthread_mutex_lock(&smu);
    sactive--;
    if (me >= 0)
        first_active[me] = 0;
    pthread_mutex_unlock(&smu);
    return strdup("talk: a summary line");
}

static Backend *slow_open(void *ud, const char *system)
{
    (void)ud;
    (void)system;
    Backend *b = calloc(1, sizeof *b);
    b->ask     = slow_ask;
    b->reset   = fake_reset;
    b->close   = fake_close;
    return b;
}

/* Up to OC_JOBS leaves are built at once: a leaf starts while fewer than
   OC_JOBS lines before it are unbuilt, and its context stops at the first
   unbuilt line. Calls that would write the same cache entry wait for the
   first, so only it writes. */
static void concurrent_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, NODE, VIEW, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    char msg[200];
    memset(msg, 'c', 150);
    msg[150] = '\0';
    for (int i = 0; i < 4; i++)
        oc_append(m, "user", "short");
    for (int i = 0; i < 40; i++)
        oc_append(m, "echo", msg);
    oc_ref due[64];
    int n = oc_due(m, due, 64), leaves = 0;
    for (int k = 0; k < n; k++)
        leaves += due[k].l == 0;
    CHECK(leaves == OC_JOBS && due[0].l == 0 && due[0].i == 4 && due[OC_JOBS - 1].i == 4 + OC_JOBS - 1);

    pthread_mutex_lock(&m->mu);
    char *ctx = oc_context(m, (oc_ref){ 0, 9 });
    pthread_mutex_unlock(&m->mu);
    CHECK(!strstr(ctx, "not summarized") && !strstr(ctx, "cccc\n") && strstr(ctx, "user: short"));
    free(ctx);

    oc_compactor *c = oc_compactor_start(m, OC_JOBS, slow_open, NULL);
    started = oc_ms();
    while (oc_pending(m) && !too_long(NULL))
        usleep(10000);
    long long took = oc_ms() - started;
    CHECK(!oc_pending(m));
    CHECK(smax >= OC_JOBS / 2);
    CHECK(sbad == 0);
    CHECK(soverlap == 0);
    fprintf(stderr, "concurrent: %d calls, at most %d at once, %lld ms\n", scalls, smax, took);
    oc_compactor_stop(c);
    oc_close(m);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        failures++;
}

/* Summary calls see a smaller view than turns: 16-32 KB, merged further from
   the turn view, so a long chat sends each summary a fraction of it. */
static void compaction_view_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, 0, 0, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    char msg[700], line[400];
    memset(msg, 'v', sizeof msg - 1);
    msg[sizeof msg - 1] = '\0';
    memset(line, 's', sizeof line - 1);
    line[sizeof line - 1] = '\0';
    long maxc = 0, minc = 1L << 30;
    for (int i = 0; i < 3000; i++) {
        oc_append(m, i % 2 ? "echo" : "user", msg);
        oc_ref due[64];
        int n;
        while ((n = oc_due(m, due, 64)) > 0)
            for (int k = 0; k < n; k++)
                CHECK(oc_put(m, due[k].l, due[k].i, line));
        CHECK(oc_view_size(m) <= OC_VIEW);
        if (i > 500) {
            pthread_mutex_lock(&m->mu);
            CHECK(m->csize <= OC_VIEW / 4);
            pthread_mutex_unlock(&m->mu);
            oc_append(m, "user", msg);
            pthread_mutex_lock(&m->mu);
            char *ctx = oc_context(m, (oc_ref){ 0, m->n - 1 });
            pthread_mutex_unlock(&m->mu);
            long len = (long)chat_len(ctx);
            if (len > maxc)
                maxc = len;
            if (len < minc)
                minc = len;
            CHECK(count_marks(ctx) == 1);
            CHECK(strstr(ctx, "Aim for about 427 bytes") && strstr(ctx, "never more than 512:\n"));
            free(ctx);
        }
    }
    CHECK(maxc <= OC_VIEW / 4 + 1000 && minc >= OC_VIEW / 8 - 1000);
    fprintf(stderr, "compaction view %ld-%ld bytes, turn view %ld\n", minc, maxc, oc_view_size(m));
    oc_close(m);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        failures++;
}

/* Ready merges come from a queue kept as nodes are built, not from a scan of
   the tree: after the leaves are in, every pair is due at once. */
static void ready_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, NODE, 100000, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    char msg[200];
    memset(msg, 'r', 150);
    msg[150] = '\0';
    for (int i = 0; i < 64; i++)
        oc_append(m, "user", msg);
    oc_ref due[128];
    int n, leaves = 0;
    while ((n = oc_due(m, due, 128)) > 0 && due[0].l == 0) {
        int batch = 0;
        for (int k = 0; k < n && due[k].l == 0; k++, leaves++, batch++)
            CHECK(oc_put(m, 0, due[k].i, "talk: a leaf of forty bytes, roughly...."));
        CHECK(batch <= OC_JOBS);
    }
    CHECK(leaves == 64 && n == 32);
    for (int k = 0; k < n; k++)
        CHECK(due[k].l == 1 && due[k].i == k);
    CHECK(m->nready == 32);
    for (int k = 0; k < n; k++)
        CHECK(oc_put(m, 1, due[k].i, "talk: a merged line of forty bytes......"));
    n = oc_due(m, due, 128);
    CHECK(n == 16 && due[0].l == 2 && m->nready == 16);
    oc_close(m);
    m = oc_open(dir, NODE, 100000, err, sizeof err);
    CHECK(m);
    if (m) {
        n = oc_due(m, due, 128);
        CHECK(n == 16 && due[0].l == 2);
        oc_close(m);
    }
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        failures++;
}

/* A user message over 30,000 bytes is logged as several messages in a row,
   whole; a tool result is clipped by its caller and never split. */
static void split_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, 0, 0, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    size_t len = 70000;
    char *big = malloc(len + 1);
    for (size_t k = 0; k < len; k++)
        big[k] = k % 100 == 99 ? '\n' : 'a' + (char)(k % 26);
    big[len] = '\0';
    CHECK(oc_append(m, "user", "first") == 0);
    CHECK(oc_append(m, "user", big) == 1);
    long n = oc_count(m);
    CHECK(n == 4);
    size_t at = 0;
    for (long i = 1; i < n; i++) {
        const char *t = oc_text(m, i);
        CHECK(!strcmp(oc_kind(m, i), "user") && strlen(t) <= OC_SPLIT && !strncmp(t, big + at, strlen(t)));
        at += strlen(t);
    }
    CHECK(at == len);
    CHECK(oc_append(m, "echo", big) == n && oc_count(m) == n + 1 && strlen(oc_text(m, n)) == len);
    free(big);
    oc_close(m);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        failures++;
}

static const char *script[OC_TRIES + 1];
static int         nscript;
static char        sent[OC_TRIES + 1][2048];

static char *script_ask(Backend *b, const char *user)
{
    (void)b;
    snprintf(sent[nscript], sizeof sent[nscript], "%s", user);
    return strdup(script[nscript++]);
}

static int resets;

static int count_reset(Backend *b)
{
    (void)b;
    resets++;
    return 1;
}

static void copy_test(void)
{
    Backend b = { .ask = script_ask, .reset = count_reset };
    char    longline[NODE + 40], cut[NODE + 1], said[64], ruler[NODE + 3];
    snprintf(longline, sizeof longline, "talk: %0*d", NODE + 30, 0);
    memcpy(cut, longline + 20, NODE - 20);
    cut[NODE - 20] = '\0';
    ruler[0] = '\n';
    memset(ruler + 1, '-', NODE);
    strcpy(ruler + NODE + 1, "\n");
    script[0] = longline;
    script[1] = cut;
    script[2] = "talk: short line";
    char *line = oc_build(&b, NODE, "prompt", NULL, NULL, 1, NULL, NULL);
    CHECK(nscript == 3);
    CHECK(line && !strcmp(line, "talk: short line"));
    CHECK(resets == 1);
    CHECK(!strcmp(sent[0], "prompt"));
    snprintf(said, sizeof said, "Too long: your line is %zu bytes, over the %d-byte limit", strlen(longline), NODE);
    CHECK(!strncmp(sent[1], said, strlen(said)) && strstr(sent[1], ruler) && !strstr(sent[1], "prompt"));
    CHECK(!strncmp(sent[2], "Rejected: ", 10) && !strstr(sent[2], "prompt"));
    free(line);

    char marked[NODE + 20];
    snprintf(marked, sizeof marked, "%.*s| \xe2\x86\x90 LIMIT", NODE - 20, longline);
    nscript = 0;
    script[1] = marked;
    line = oc_build(&b, NODE, "prompt", NULL, NULL, 1, NULL, NULL);
    CHECK(nscript == 3);
    CHECK(line && !strcmp(line, "talk: short line"));
    free(line);

    /* An API error is a failed try, never a line. */
    const char *error = "API Error: 400 A maximum of 4 blocks with cache_control may be provided. Found 5.";
    nscript = 0;
    script[0] = error;
    line = oc_build(&b, NODE, "prompt", "user: source", NULL, 1, NULL, NULL);
    CHECK(nscript == 1 && !line);
    free(line);
    nscript = 0;
    script[0] = "talk: a line far too long";
    script[1] = error;
    line = oc_build(&b, 10, "prompt", "user: source", NULL, 1, NULL, NULL);
    CHECK(nscript == 2 && !line);
    free(line);

    nscript = 0;
    script[0] = ".h oc_fit fragment| \xe2\x86\x90 LIMIT";
    script[1] = "<line>tool: fixed retry.</line></br>";
    line = oc_build(&b, NODE, "prompt", NULL, NULL, 1, NULL, NULL);
    CHECK(nscript == 2 && line && !strcmp(line, "tool: fixed retry."));
    free(line);

    /* Tails of a cut line are rejected; stray closing tags are dropped. */
    nscript = 0;
    script[0] = "utput yet).";
    script[1] = "n reviewed..";
    script[2] = "echo: tests pass.</> user: ship it</input>";
    line = oc_build(&b, NODE, "prompt", NULL, NULL, 1, NULL, NULL);
    CHECK(nscript == 3 && line && !strcmp(line, "echo: tests pass. user: ship it"));
    free(line);
}

/* Over-long tries keep the shortest, and a line too long every time is cut
   on a word boundary, so the node is always built. */
static void cut_test(void)
{
    Backend b = { .ask = script_ask, .reset = count_reset };
    char    tries[OC_TRIES][NODE * 3];
    for (int t = 0; t < OC_TRIES; t++) {
        snprintf(tries[t], sizeof tries[t], "talk: try%d", t);
        while (strlen(tries[t]) < (size_t)(NODE * 2 - (t == 2 ? 20 : 0)))
            strcat(tries[t], " words");
        script[t] = tries[t];
    }
    nscript = 0;
    resets  = 0;
    char *line = oc_build(&b, NODE, "prompt", "user: source", NULL, 1, NULL, NULL);
    CHECK(nscript == OC_TRIES && resets == 1);
    CHECK(line && strlen(line) <= NODE && !strncmp(line, "talk: try2 words", 16));
    CHECK(line && !strncmp(tries[2], line, strlen(line)) && tries[2][strlen(line)] == ' ');
    free(line);

    for (int t = 0; t < OC_TRIES; t++)
        script[t] = "no tag here";
    nscript = 0;
    line = oc_build(&b, NODE, "prompt", "user: source", NULL, 1, NULL, NULL);
    CHECK(nscript == OC_ATTEMPTS && line && !strcmp(line, "user: source"));
    free(line);

    char s[200] = "x";
    for (int k = 0; k < 40; k++)
        strcat(s, "\xc3\xa9");
    oc_cut(s, 20);
    CHECK(strlen(s) == 19 && !strncmp(s, "x\xc3\xa9", 3));
}

/* The most due pair is the one that ended longest ago in its own line size:
   (T - last) / 2^l, as the gist and Taelin's push have it. At T=10 with the
   view 0+4, 4+4, 8+1, 9+1, push merges 8-9, not 0-7. */
static void due_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, 8, 100000, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    for (int i = 0; i < 10; i++)
        CHECK(oc_append(m, "user", "a message too long to be its own line") == i);
    for (int i = 0; i < 10; i++)
        CHECK(oc_put(m, 0, i, "aaaaa"));
    for (int i = 0; i < 5; i++)
        CHECK(oc_put(m, 1, i, "bbbbb"));
    CHECK(oc_put(m, 2, 0, "ccccc") && oc_put(m, 2, 1, "ccccc") && oc_put(m, 3, 0, "ddddd"));
    pthread_mutex_lock(&m->mu);
    oc_ref view[] = { { 2, 0 }, { 2, 1 }, { 0, 8 }, { 0, 9 } };
    memcpy(m->parts, view, sizeof view);
    m->np        = 4;
    m->view      = 30;
    m->shrinking = 1;
    oc_fit(m);
    CHECK(m->np == 3 && m->parts[0].l == 2 && m->parts[1].l == 2 && m->parts[2].l == 1 && m->parts[2].i == 4);
    pthread_mutex_unlock(&m->mu);
    oc_close(m);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        failures++;
}

static char *long_ask(Backend *b, const char *user)
{
    (void)b;
    (void)user;
    char s[NODE * 4] = "user: always";
    while (strlen(s) < NODE * 3)
        strcat(s, " too long");
    return strdup(s);
}

static Backend *long_open(void *ud, const char *system)
{
    (void)ud;
    (void)system;
    Backend *b = calloc(1, sizeof *b);
    b->ask     = long_ask;
    b->reset   = fake_reset;
    b->close   = fake_close;
    return b;
}

/* A model that is always too long still gets its nodes built, not retried. */
static void long_reply_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, NODE, VIEW, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    oc_compactor *c = oc_compactor_start(m, 2, long_open, NULL);
    CHECK(c);
    char msg[200];
    memset(msg, 'q', 150);
    msg[150] = '\0';
    CHECK(oc_append(m, "user", msg) == 0);
    CHECK(oc_append(m, "user", msg) == 1);
    started = oc_ms();
    CHECK(oc_settle(m, three_seconds, NULL));
    CHECK(oc_settled(m));
    oc_ref due[4];
    while (oc_due(m, due, 4) && !three_seconds(NULL))
        usleep(10000);
    CHECK(oc_node(m, 0, 0) && strlen(oc_node(m, 0, 0)) <= NODE && !strncmp(oc_node(m, 0, 0), "user: always too", 16));
    CHECK(oc_node(m, 1, 0));
    char e[512];
    CHECK(oc_compactor_error(c, e, sizeof e) == 0);
    oc_compactor_stop(c);
    oc_close(m);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        failures++;
}

/* A reply a little over the limit is kept; a retry is asked only past
   ~15% over, and says how long the line was. */
static void over_test(void)
{
    Backend b = { .ask = script_ask, .reset = count_reset };
    long    ok = NODE + NODE * 15 / 100;
    char    near[NODE + 40], far[NODE + 40];
    memset(near, 'n', (size_t)ok);
    memcpy(near, "talk: ", 6);
    near[ok] = '\0';
    memset(far, 'f', (size_t)ok + 1);
    memcpy(far, "talk: ", 6);
    far[ok + 1] = '\0';
    char said[64], over[64];
    snprintf(over, sizeof over, "Too long: your line is %ld bytes, over the %d-byte limit", ok, NODE);
    nscript = 0;
    script[0] = near;
    script[1] = "talk: shorter";
    char *line = oc_build(&b, NODE, "prompt", NULL, NULL, 1, NULL, NULL);
    CHECK(nscript == 2 && line && !strcmp(line, "talk: shorter"));
    CHECK(!strncmp(sent[1], over, strlen(over)));
    free(line);

    nscript = 0;
    script[0] = near;
    script[1] = far;
    line = oc_build(&b, NODE, "prompt", NULL, NULL, 1, NULL, NULL);
    CHECK(nscript == 2 && line && !strcmp(line, near));
    free(line);

    nscript = 0;
    script[0] = near;
    script[1] = "user: shorter but wrong";
    line = oc_build(&b, NODE, "prompt", NULL, "talk", 0, NULL, NULL);
    CHECK(nscript == 2 && line && !strcmp(line, near));
    free(line);

    nscript = 0;
    script[0] = near;
    script[1] = "API Error: 529 overloaded";
    line = oc_build(&b, NODE, "prompt", NULL, NULL, 1, NULL, NULL);
    CHECK(nscript == 2 && line && !strcmp(line, near));
    free(line);

    for (int t = 0; t < OC_TRIES; t++)
        script[t] = "talk: never right; user: x";
    script[OC_ATTEMPTS - 1] = near;
    nscript = 0;
    line = oc_build(&b, NODE, "prompt", "talk: source", "talk", 0, NULL, NULL);
    CHECK(nscript == OC_ATTEMPTS + 1 && line && !strcmp(line, near));
    free(line);

    snprintf(said, sizeof said, "Too long: your line is %ld bytes", ok + 1);
    nscript = 0;
    script[0] = far;
    script[1] = near;
    line = oc_build(&b, NODE, "prompt", NULL, NULL, 1, NULL, NULL);
    CHECK(nscript == 2 && line && !strcmp(line, near));
    CHECK(!strncmp(sent[1], said, strlen(said)));
    free(line);
}

/* Lines over the stated limit store, render, reload and zoom whole, at the
   real 512-byte size: nothing holds a line in a node-sized buffer. */
static void long_line_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, OC_NODE, 0, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    char msg[700], line[3][581];
    memset(msg, 'm', sizeof msg - 1);
    msg[sizeof msg - 1] = '\0';
    for (int k = 0; k < 3; k++) {
        memset(line[k], 'A' + k, 579);
        line[k][0] = '0' + k;
        line[k][579] = 'Z';
        line[k][580] = '\0';
    }
    for (int i = 0; i < 4; i++)
        CHECK(oc_append(m, "echo", msg) == i);
    CHECK(oc_put(m, 0, 0, line[0]) && oc_put(m, 0, 1, line[1]));
    CHECK(oc_put(m, 0, 2, line[0]) && oc_put(m, 0, 3, line[1]));
    CHECK(oc_put(m, 1, 0, line[2]) && oc_put(m, 1, 1, line[2]));
    CHECK(oc_put(m, 2, 0, line[0]));
    for (int pass = 0; pass < 2; pass++) {
        char want[1400];
        char *z = oc_zoom(m, 0, 2);
        snprintf(want, sizeof want, "0+1|%s\n1+1|%s", line[0], line[1]);
        CHECK(!strcmp(z, want));
        free(z);
        z = oc_zoom(m, 0, 4);
        snprintf(want, sizeof want, "0+2|%s\n2+2|%s", line[2], line[2]);
        CHECK(!strcmp(z, want));
        free(z);
        char *v = oc_render(m, 0);
        CHECK(strstr(v, line[0]) || strstr(v, line[2]));
        free(v);
        oc_close(m);
        m = oc_open(dir, OC_NODE, 0, err, sizeof err);
        CHECK(m);
        if (!m)
            return;
    }
    oc_close(m);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        failures++;
}

static void wipe(const char *dir)
{
    const char *subs[] = { "/main", "/tree", "" };
    for (int k = 0; k < 3; k++) {
        char path[600];
        snprintf(path, sizeof path, "%s%s", dir, subs[k]);
        DIR *d = opendir(path);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            char f[1200];
            snprintf(f, sizeof f, "%s/%s", path, e->d_name);
            unlink(f);
        }
        if (d)
            closedir(d);
        CHECK(rmdir(path) == 0);
    }
}

static void rules_test(void)
{
    Backend b = { .ask = script_ask, .reset = count_reset };
    nscript   = 0;
    script[0] = "user: asked to restart";
    script[1] = "talk: restarting (user: restart after dumpco)";
    script[2] = "talk: restarting; told the user: done";
    char *line = oc_build(&b, NODE, "prompt", "talk: source", "talk", 0, NULL, NULL);
    CHECK(nscript == 3 && line && !strcmp(line, script[2]));
    CHECK(!strncmp(sent[1], "Rejected: this is a talk message, so the line must start with \"talk: \"", 69));
    CHECK(!strncmp(sent[2], "Rejected: no message in your stretch is the user's", 50));
    free(line);

    nscript   = 0;
    script[0] = "talk: done; user: ship it";
    line      = oc_build(&b, NODE, "prompt", NULL, NULL, 1, NULL, NULL);
    CHECK(nscript == 1 && line && !strcmp(line, script[0]));
    free(line);

    nscript   = 0;
    script[0] = "work: [from @w] tests pass";
    line      = oc_build(&b, NODE, "prompt", NULL, "work", 0, NULL, NULL);
    CHECK(nscript == 1 && line && !strcmp(line, script[0]));
    free(line);

    for (int t = 0; t < OC_TRIES; t++)
        script[t] = "user: never right";
    nscript = 0;
    line    = oc_build(&b, NODE, "prompt", "talk: source", "talk", 0, NULL, NULL);
    CHECK(nscript == OC_ATTEMPTS && line && !strcmp(line, "talk: never right"));
    free(line);

    nscript   = 0;
    script[0] = "user: explains his understanding of the register";
    script[1] = "user: explains their understanding of the register";
    line      = oc_build(&b, NODE, "prompt", NULL, "user", 1, NULL, NULL);
    CHECK(nscript == 2 && line && !strcmp(line, script[1]));
    CHECK(!strncmp(sent[1], "Rejected: call the user \"the user\" or they/them", 47));
    free(line);

    for (int t = 0; t < OC_TRIES; t++)
        script[t] = t == 2 ? "user: he wants it" : "user: he wants it now";
    nscript = 0;
    line    = oc_build(&b, NODE, "prompt", "user: source", "user", 1, NULL, NULL);
    CHECK(nscript == OC_ATTEMPTS && line && !strcmp(line, "user: he wants it"));
    free(line);

    CHECK(oc_user_item("talk: ok (user: restart after dumpco)"));
    CHECK(oc_user_item("User: hi"));
    CHECK(oc_user_item("echo: x; user/talk: agreed"));
    CHECK(!oc_user_item("talk: told the user: done"));
    CHECK(!oc_user_item("tool: Bash `superuser:x`"));
    CHECK(!oc_user_item("echo: user count is 5"));

    CHECK(oc_gendered("user: explains his view"));
    CHECK(oc_gendered("talk: met Tom; he agrees"));
    CHECK(oc_gendered("talk: Tom agrees. user: he emails"));
    CHECK(oc_gendered("user: He wants it"));
    CHECK(!oc_gendered("user: met Tom, who said he agrees"));
    CHECK(!oc_gendered("user: told of talk with Tom about ontologies, his conclusion: keep them"));
    CHECK(!oc_gendered("work: [from @w] says his tests pass"));
    CHECK(!oc_gendered("echo: audit lists he/his pronouns"));
    CHECK(!oc_gendered("talk: the word \"her\" appears; the theme is there"));
    CHECK(strstr(OC_COMPACT, "never he/she/his/her") && strstr(OC_COMPACT, "status block in a reply"));

    nscript   = 0;
    script[0] = "echo: recalled @maxiscrap-2 review (cat of the review file)";
    script[1] = "echo: review file: 3 findings, all fixed";
    line      = oc_build(&b, NODE, "prompt", NULL, "echo", 0, NULL, NULL);
    CHECK(nscript == 2 && line && !strcmp(line, script[1]));
    CHECK(!strncmp(sent[1], "Rejected: this echo is the output of a tool other than zoom", 59));
    free(line);

    nscript   = 0;
    script[0] = "talk: plan; echo: recalled 12 (plan)";
    line      = oc_build(&b, NODE, "prompt", NULL, NULL, 0, NULL, NULL);
    CHECK(nscript == 1 && line && !strcmp(line, script[0]));
    free(line);

    CHECK(oc_recalled("echo: recalled 5"));
    CHECK(oc_recalled("echo:Recalled the brief"));
    CHECK(oc_recalled("talk: ok; echo: recalled x"));
    CHECK(!oc_recalled("echo: recalledness"));
    CHECK(!oc_recalled("echo: the file recalled earlier"));
    CHECK(!oc_recalled("talk: echo recalled"));
    CHECK(strstr(OC_COMPACT, "Only an echo of a zoom call is a recall") && strstr(OC_COMPACT, "never call it \"recalled\""));
    CHECK(strstr(OC_COMPACT, "Add no reason, comparison,") && strstr(OC_COMPACT, "not what a result went up or down from"));
}

static char *user_ask(Backend *b, const char *user)
{
    (void)b;
    (void)user;
    return strdup("user: asked something");
}

static Backend *user_open(void *ud, const char *system)
{
    (void)ud;
    (void)system;
    Backend *b = calloc(1, sizeof *b);
    b->ask     = user_ask;
    b->reset   = fake_reset;
    b->close   = fake_close;
    return b;
}

static void kind_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, NODE, 100000, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    char msg[200];
    memset(msg, 'k', 150);
    msg[150] = '\0';
    CHECK(oc_append(m, "talk", msg) == 0);
    CHECK(oc_append(m, "echo", msg) == 1);
    CHECK(oc_append(m, "user", msg) == 2);
    snprintf(msg, sizeof msg, "[from @w] %0140d", 0);
    CHECK(oc_append(m, "user", msg) == 3);
    oc_compactor *c = oc_compactor_start(m, 2, user_open, NULL);
    started = oc_ms();
    oc_ref due[4];
    while ((oc_due(m, due, 4) || !oc_node(m, 2, 0)) && !too_long(NULL))
        usleep(10000);
    oc_compactor_stop(c);
    CHECK(oc_node(m, 0, 0) && !strcmp(oc_node(m, 0, 0), "talk: asked something"));
    CHECK(oc_node(m, 0, 1) && !strcmp(oc_node(m, 0, 1), "echo: asked something"));
    CHECK(oc_node(m, 0, 2) && !strcmp(oc_node(m, 0, 2), "user: asked something"));
    CHECK(oc_node(m, 0, 3) && !strcmp(oc_node(m, 0, 3), "work: asked something"));
    CHECK(oc_node(m, 1, 0) && !strcmp(oc_node(m, 1, 0), "talk: asked something\necho: asked something"));
    CHECK(oc_node(m, 1, 1) && !strcmp(oc_node(m, 1, 1), "user: asked something\nwork: asked something"));
    CHECK(oc_node(m, 2, 0) && !strcmp(oc_node(m, 2, 0), "user: asked something"));
    oc_close(m);
    wipe(dir);
}

static void recall_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, 0, 0, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    char talk[800], echo[900];
    snprintf(talk, sizeof talk, "Plan for the eval suite:\n\n- first build the harness, then  the runner %0600d", 0);
    CHECK(oc_append(m, "user", "hi") == 0);
    CHECK(oc_append(m, "talk", talk) == 1);
    snprintf(echo, sizeof echo, "message 1 (talk):\n%s", talk);
    CHECK(oc_append(m, "echo", echo) == 2);
    const char *r = oc_node(m, 0, 2);
    CHECK(r && !strcmp(r, "echo: recalled 1 (Plan for the eval suite: - first build the harness)"));
    CHECK(r && strlen(r) < 100 && r[strlen(r) - 1] == ')');
    CHECK(oc_append(m, "echo", "0+1|user: hi\n1+1|talk: Plan for the eval suite") == 3);
    CHECK(oc_node(m, 0, 3) && !strcmp(oc_node(m, 0, 3), "echo: recalled 0+2 (hi)"));
    CHECK(oc_append(m, "echo", "1840+0|talk: old style") == 4);
    CHECK(oc_node(m, 0, 4) && !strcmp(oc_node(m, 0, 4), "echo: recalled 1840 (old style)"));
    CHECK(oc_append(m, "echo", "No line 1+2.") == 5);
    CHECK(oc_node(m, 0, 5) && !strcmp(oc_node(m, 0, 5), "echo: No line 1+2."));
    CHECK(oc_append(m, "echo", "message 3 (bogus):\nx") == 6);
    CHECK(oc_node(m, 0, 6) && !strcmp(oc_node(m, 0, 6), "echo: message 3 (bogus):\nx"));
    CHECK(oc_append(m, "tool", "message 1 (talk):\nx") == 7);
    CHECK(oc_node(m, 0, 7) && !strcmp(oc_node(m, 0, 7), "tool: message 1 (talk):\nx"));
    oc_ref due[8];
    CHECK(oc_due(m, due, 8) == 1 && due[0].l == 0 && due[0].i == 1);
    CHECK(oc_append(m, "echo", "3008+32|talk: wrote the brief, started the runs (bench query). user:\n3040+32|x") == 8);
    r = oc_node(m, 0, 8);
    CHECK(r && !strcmp(r, "echo: recalled 3008+64 (wrote the brief, started the runs (bench query))"));
    CHECK(r && !oc_user_item(r));
    CHECK(oc_append(m, "echo", "3200+4|talk: Yes. The roadmap is the six phases (P0 to P5) in the order the user approved; tool: read plan; echo (08:43): ok") == 9);
    r = oc_node(m, 0, 9);
    CHECK(r && !strcmp(r, "echo: recalled 3200+4 (Yes. The roadmap is the six phases (P0 to P5) in the order the user)"));
    CHECK(r && strlen(r) < 100 && !oc_user_item(r) && !strstr(r, "talk:") && !strstr(r, "echo ("));
    char quoted[600];
    snprintf(quoted, sizeof quoted, "message 2 (talk):\nuser/talk: agreed on the plan, then talk (09:12): drafted the full spec with every module named, "
                                    "its inputs and outputs listed, (and the tests %0200d", 0);
    CHECK(oc_append(m, "echo", quoted) == 10);
    r = oc_node(m, 0, 10);
    CHECK(r && !strcmp(r, "echo: recalled 2 (agreed on the plan, then drafted the full spec with every module named)"));
    CHECK(r && strlen(r) < 100);
    char *s = strdup("talk: did X (user: asked Y); echo (08:43): ok; Users: 5; superuser: x");
    oc_detag(s);
    CHECK(!strcmp(s, "did X (asked Y); ok; Users: 5; superuser: x"));
    free(s);
    oc_close(m);
    wipe(dir);
}

static int shape_ok(const oc_mem *m)
{
    for (int k = 0; k + 1 < m->np; k++)
        if (m->parts[k + 1].l > m->parts[k].l)
            return 0;
    return 1;
}

static void merge_stop_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, 8, 100000, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    for (int i = 0; i < 8; i++)
        CHECK(oc_append(m, "user", "a message too long to be its own line") == i);
    for (int i = 0; i < 8; i++)
        CHECK(oc_put(m, 0, i, "aaaaa"));
    CHECK(oc_put(m, 1, 2, "bbbbb") && oc_put(m, 1, 3, "bbbbb"));
    pthread_mutex_lock(&m->mu);
    m->view      = 30;
    m->shrinking = 1;
    oc_fit(m);
    CHECK(m->np == 8 && m->parts[0].l == 0);
    pthread_mutex_unlock(&m->mu);
    CHECK(oc_put(m, 1, 0, "bbbbb"));
    pthread_mutex_lock(&m->mu);
    CHECK(m->np == 7 && m->parts[0].l == 1 && m->parts[0].i == 0 && m->parts[1].l == 0 && m->parts[1].i == 2);
    pthread_mutex_unlock(&m->mu);
    CHECK(oc_put(m, 1, 1, "bbbbb"));
    pthread_mutex_lock(&m->mu);
    CHECK(m->np == 5 && m->parts[2].l == 1 && m->parts[2].i == 2 && m->parts[3].l == 0 && shape_ok(m));
    pthread_mutex_unlock(&m->mu);
    CHECK(oc_put(m, 2, 0, "ccccc"));
    pthread_mutex_lock(&m->mu);
    CHECK(m->np == 3 && m->parts[0].l == 2 && m->parts[2].l == 1 && m->parts[2].i == 3 && shape_ok(m));
    pthread_mutex_unlock(&m->mu);
    oc_close(m);
    wipe(dir);

    char dir2[] = "/tmp/optchattest.XXXXXX";
    if (!mkdtemp(dir2)) {
        failures++;
        return;
    }
    m = oc_open(dir2, NODE, 4000, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    char msg[200];
    memset(msg, 'p', 150);
    msg[150] = '\0';
    for (int i = 0; i < 300; i++)
        oc_append(m, "user", msg);
    oc_ref due[16];
    int n;
    while ((n = oc_due(m, due, 16)) > 0 && due[0].l == 0)
        for (int k = 0; k < n && due[k].l == 0; k++)
            CHECK(oc_put(m, 0, due[k].i, "talk: a leaf of forty bytes, roughly...."));
    oc_close(m);
    char path[600];
    snprintf(path, sizeof path, "%s/view.json", dir2);
    CHECK(unlink(path) == 0);
    m = oc_open(dir2, NODE, 4000, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    pthread_mutex_lock(&m->mu);
    CHECK(m->np == 300 && shape_ok(m));
    pthread_mutex_unlock(&m->mu);
    int bad = 0, puts = 0;
    while ((n = oc_due(m, due, 16)) > 0) {
        for (int k = 0; k < n; k++) {
            CHECK(oc_put(m, due[k].l, due[k].i, "talk: a merged line of forty bytes......"));
            puts++;
            pthread_mutex_lock(&m->mu);
            bad += !shape_ok(m);
            pthread_mutex_unlock(&m->mu);
        }
    }
    CHECK(bad == 0 && puts > 100);
    CHECK(oc_view_size(m) <= 4000);
    tiles(m);
    oc_close(m);
    wipe(dir2);
}

static void context_cap_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, 0, 0, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    char msg[700], leaf[401];
    memset(msg, 'c', sizeof msg - 1);
    msg[sizeof msg - 1] = '\0';
    memset(leaf, 'l', sizeof leaf - 1);
    memcpy(leaf, "talk: ", 6);
    leaf[sizeof leaf - 1] = '\0';
    for (int i = 0; i < 600; i++) {
        oc_append(m, "echo", msg);
        CHECK(oc_put(m, 0, i, leaf));
    }
    pthread_mutex_lock(&m->mu);
    CHECK(m->np == 600);
    long maxc = 0;
    for (long i = 0; i < 600; i += 7) {
        char *ctx = oc_context(m, (oc_ref){ 0, i });
        long len = (long)chat_len(ctx);
        if (len > maxc)
            maxc = len;
        CHECK(count_marks(ctx) == 1);
        free(ctx);
    }
    char *ctx = oc_context(m, (oc_ref){ 1, 299 });
    long last = (long)chat_len(ctx);
    free(ctx);
    pthread_mutex_unlock(&m->mu);
    CHECK(maxc <= OC_VIEW / 4 + 1000);
    CHECK(last >= OC_VIEW / 8 && last <= OC_VIEW / 4 + 1000);
    fprintf(stderr, "blocked compaction context max %ld bytes (last %ld) over 600 built leaves\n", maxc, last);
    oc_close(m);
    wipe(dir);
}

static void repair_test(void)
{
    char *s = oc_unuser(strdup("talk: did X (user: asked Y); user: said Z; echo: ok"));
    CHECK(!strcmp(s, "talk: did X; echo: ok"));
    free(s);
    s = oc_unuser(strdup("talk: ran it\nuser: thanks; tool: Bash `superuser:x`"));
    CHECK(!strcmp(s, "talk: ran it; tool: Bash `superuser:x`"));
    free(s);

    Backend     b = { .ask = script_ask, .reset = count_reset };
    const char *how;
    int         left = OC_ATTEMPTS;
    for (int t = 0; t < OC_TRIES; t++)
        script[t] = "user: restart it; tool: ran restart (user: after dumpco)";
    nscript    = 0;
    char *line = oc_build(&b, NODE, "prompt", "talk: source", "talk", 0, &left, &how);
    CHECK(nscript == OC_ATTEMPTS && left == 0 && how && !strcmp(how, "repair"));
    CHECK(line && !strcmp(line, "talk: restart it; tool: ran restart"));
    CHECK(line && oc_lead(line, "talk") && !oc_user_item(line));
    free(line);

    for (int t = 0; t < OC_TRIES; t++)
        script[t] = "user: said ship it; echo: tests pass";
    left    = OC_ATTEMPTS;
    nscript = 0;
    line    = oc_build(&b, NODE, "prompt", "echo: source", NULL, 0, &left, &how);
    CHECK(nscript == OC_ATTEMPTS && line && !strcmp(line, "echo: tests pass") && how && !strcmp(how, "repair"));
    free(line);

    for (int t = 0; t < OC_TRIES; t++)
        script[t] = "echo: recalled review brief+report";
    left    = OC_ATTEMPTS;
    nscript = 0;
    line    = oc_build(&b, NODE, "prompt", "echo: source", "echo", 0, &left, &how);
    CHECK(nscript == OC_ATTEMPTS && line && !strcmp(line, "echo: review brief+report") && how && !strcmp(how, "repair"));
    free(line);

    for (int t = 0; t < OC_TRIES; t++)
        script[t] = "user: only the user's words";
    left    = OC_ATTEMPTS;
    nscript = 0;
    line    = oc_build(&b, NODE, "prompt", "echo: source", NULL, 0, &left, &how);
    CHECK(nscript == OC_ATTEMPTS && line && !strcmp(line, "echo: source") && how && !strcmp(how, "source"));
    free(line);

    left    = 0;
    nscript = 0;
    resets  = 0;
    line    = oc_build(NULL, NODE, "prompt", "echo: source", NULL, 0, &left, &how);
    CHECK(nscript == 0 && resets == 0 && line && !strcmp(line, "echo: source") && how && !strcmp(how, "source"));
    free(line);

    script[0] = "user: wrong";
    script[1] = "API Error: 529 overloaded";
    left      = OC_ATTEMPTS;
    nscript   = 0;
    line      = oc_build(&b, NODE, "prompt", "talk: source", "talk", 0, &left, &how);
    CHECK(nscript == 2 && !line && left == OC_ATTEMPTS - 1);
}

static pthread_mutex_t cmu = PTHREAD_MUTEX_INITIALIZER;
static int             ccalls, cflaky;

static char *cap_ask(Backend *b, const char *user)
{
    (void)b;
    (void)user;
    pthread_mutex_lock(&cmu);
    int k = ccalls++;
    pthread_mutex_unlock(&cmu);
    if (cflaky && k % 2)
        return strdup("API Error: 529 overloaded");
    return strdup("user: asked about the long running build on the server");
}

static Backend *cap_open(void *ud, const char *system)
{
    (void)ud;
    (void)system;
    Backend *b = calloc(1, sizeof *b);
    b->ask     = cap_ask;
    b->reset   = fake_reset;
    b->close   = fake_close;
    return b;
}

static int calls(void)
{
    pthread_mutex_lock(&cmu);
    int k = ccalls;
    pthread_mutex_unlock(&cmu);
    return k;
}

static int occurrences(const char *s, const char *needle)
{
    int k = 0;
    for (const char *p = s; p && (p = strstr(p, needle)); p++)
        k++;
    return k;
}

static void cap_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, NODE, 100, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    char msg[200];
    memset(msg, 'k', 150);
    msg[150] = '\0';
    CHECK(oc_append(m, "talk", msg) == 0);
    CHECK(oc_append(m, "echo", msg) == 1);
    ccalls          = 0;
    cflaky          = 0;
    oc_compactor *c = oc_compactor_start(m, 2, cap_open, NULL);
    started         = oc_ms();
    while (!oc_node(m, 1, 0) && !too_long(NULL))
        usleep(10000);
    usleep(200000);
    oc_compactor_stop(c);
    const char *a = oc_node(m, 0, 0), *e = oc_node(m, 0, 1), *top = oc_node(m, 1, 0);
    CHECK(a && oc_lead(a, "talk") && !oc_user_item(a));
    CHECK(e && oc_lead(e, "echo") && !oc_user_item(e));
    CHECK(top && strlen(top) <= NODE && oc_tagged(top) && !oc_user_item(top));
    CHECK(calls() == 3 * OC_ATTEMPTS);
    oc_ref *parts;
    int     np = oc_view(m, &parts);
    CHECK(np == 1 && parts[0].l == 1 && parts[0].i == 0);
    free(parts);
    char *log = get_file(dir, "usage.jsonl");
    CHECK(occurrences(log, "\"fallback\":\"repair\"") == 2 && occurrences(log, "\"fallback\":\"source\"") == 1);
    free(log);
    oc_close(m);
    wipe(dir);
}

static void cap_passes_test(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir)) {
        failures++;
        return;
    }
    oc_mem *m = oc_open(dir, NODE, VIEW, err, sizeof err);
    CHECK(m);
    if (!m)
        return;
    char msg[200];
    memset(msg, 'k', 150);
    msg[150] = '\0';
    CHECK(oc_append(m, "talk", msg) == 0);
    ccalls          = 0;
    cflaky          = 1;
    oc_compactor *c = oc_compactor_start(m, 1, cap_open, NULL);
    started         = oc_ms();
    while (!oc_node(m, 0, 0) && !too_long(NULL))
        usleep(10000);
    usleep(300000);
    oc_compactor_stop(c);
    CHECK(oc_node(m, 0, 0) && !strcmp(oc_node(m, 0, 0), "talk: asked about the long running build on the server"));
    CHECK(calls() == 2 * OC_ATTEMPTS - 1);
    char *log = get_file(dir, "usage.jsonl");
    CHECK(occurrences(log, "\"fallback\":\"repair\",\"l\":0,\"i\":0,\"tries\":3") == 1);
    free(log);
    oc_close(m);
    wipe(dir);
}

int main(void)
{
    char dir[] = "/tmp/optchattest.XXXXXX";
    char err[512];
    if (!mkdtemp(dir))
        return 1;

    oc_mem *m = oc_open(dir, NODE, VIEW, err, sizeof err);
    CHECK(m);
    if (!m)
        return 1;
    CHECK(!oc_open(dir, NODE, VIEW, err, sizeof err));
    CHECK(strstr(err, "in use"));

    CHECK(oc_append(m, "user", "hi") == 0);
    CHECK(!strcmp(oc_node(m, 0, 0), "user: hi"));
    CHECK(oc_append(m, "talk", "hello") == 1);
    CHECK(!strcmp(oc_node(m, 1, 0), "user: hi\ntalk: hello"));
    CHECK(oc_settled(m));

    char longmsg[300];
    memset(longmsg, 'y', sizeof longmsg - 1);
    longmsg[sizeof longmsg - 1] = '\0';
    CHECK(oc_append(m, "echo", longmsg) == 2);
    CHECK(!oc_node(m, 0, 2));
    CHECK(!oc_settled(m));
    char *v = oc_render(m, 0);
    CHECK(strstr(v, "2+1|(not summarized yet: zoom it)"));
    free(v);
    oc_ref due[4];
    CHECK(oc_due(m, due, 4) == 1 && due[0].l == 0 && due[0].i == 2);
    compact(m);
    CHECK(oc_settled(m));

    char *z = oc_zoom(m, 2, 1);
    CHECK(!strncmp(z, "message 2 (echo):\nyyy", 21));
    free(z);
    z = oc_zoom(m, 0, 2);
    CHECK(!strcmp(z, "0+1|user: hi\n1+1|talk: hello"));
    free(z);
    z = oc_zoom(m, 1, 2);
    CHECK(!strcmp(z, "No line 1+2."));
    free(z);

    char  *prev     = NULL;
    size_t shared   = 0, renders = 0, whole = 0;
    for (int i = 3; i < 3000; i++) {
        oc_ref *old;
        int     np = oc_view(m, &old);

        char msg[200];
        int  len = 5 + (i * 37) % 150;
        memset(msg, 'a' + i % 26, (size_t)len);
        msg[len] = '\0';
        CHECK(oc_append(m, i % 3 ? "tool" : "user", msg) == i);
        compact(m);

        CHECK(oc_settled(m));
        CHECK(oc_view_size(m) <= VIEW);
        CHECK(covered(old, np, m));
        tiles(m);
        free(old);

        char *cur = oc_render(m, 0);
        if (prev && i > 1000) {
            shared += common(prev, cur);
            renders++;
            /* Merges come in batches, so most renders only add lines
               after the previous one and its cached prefix holds. */
            whole += common(prev, cur) >= strlen(prev) - strlen("</chat>");
        }
        free(prev);
        prev = cur;
    }
    CHECK(renders && shared / renders > VIEW / 3);
    CHECK(renders && whole * 2 > renders);
    fprintf(stderr, "avg shared prefix %zu of ~%d bytes; whole prefix kept in %zu of %zu renders\n",
            renders ? shared / renders : 0, VIEW, whole, renders);
    free(prev);

    long n = oc_count(m);
    char *before = strdup(oc_node(m, 5, 3)), *live = oc_render(m, 0);
    oc_close(m);

    char path[600];
    snprintf(path, sizeof path, "%s/main/torn.jsonl", dir);
    FILE *f = fopen(path, "w");
    fputs("{\"i\":99999,\"kind\":\"us", f);
    fclose(f);

    m = oc_open(dir, NODE, VIEW, err, sizeof err);
    CHECK(m);
    if (m) {
        CHECK(oc_count(m) == n);
        CHECK(oc_skipped(m) == 1);
        CHECK(!strcmp(oc_node(m, 5, 3), before));
        char *reopened = oc_render(m, 0);
        CHECK(!strcmp(reopened, live));
        free(reopened);
        CHECK(oc_settled(m));
        CHECK(oc_view_size(m) <= VIEW);
        tiles(m);
        CHECK(oc_append(m, "user", "after reopen") == n);
        oc_close(m);
    }
    free(before);
    free(live);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        failures++;
    compactor_test();
    failing_test();
    marks_test();
    view_test();
    concurrent_test();
    compaction_view_test();
    ready_test();
    split_test();
    copy_test();
    over_test();
    long_line_test();
    cut_test();
    due_test();
    long_reply_test();
    rules_test();
    kind_test();
    recall_test();
    merge_stop_test();
    context_cap_test();
    repair_test();
    cap_test();
    cap_passes_test();
    CHECK(strstr(OC_COMPACT, "\"message N (kind):\"") && strstr(OC_COMPACT, "a recall"));
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    return failures != 0;
}
