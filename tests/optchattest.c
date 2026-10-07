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
    int retry = strstr(user, BACKEND_BLOCK_MARK "\n\nYour line was") != NULL;
    fbad     += strstr(user, "not summarized yet") != NULL;
    fids     += strstr(user, "+1|") != NULL;
    fretries += retry;
    fmerges  += strstr(user, "Merge only the two lines below") != NULL;
    fscale   += !retry && strstr(user, "For scale only, a fictional line unrelated to this chat, exactly 64 bytes:\nuser: greenhouse") != NULL;
    pthread_mutex_unlock(&fmu);
    if (n == 7)
        return NULL;
    if (!retry && n % 5 == 0) {
        char *s = malloc(NODE + 22);
        memset(s, 'z', NODE + 20);
        strcpy(s + NODE + 20, "\n");
        return s;
    }
    return strdup("  summary of a long stretch of messages ab \n");
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
        CHECK(oc_node(m, 0, i) && strlen(oc_node(m, 0, i)) <= NODE);
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
    int    count = 0;
    size_t limits[] = { 50000, 80000, 100000 };
    for (char *p = ctx; (p = strstr(p, BACKEND_CACHE_MARK)); p++) {
        CHECK(count < 4);
        if (count < 3)
            CHECK((size_t)(p - ctx) <= limits[count] && (size_t)(p - ctx) > limits[count] - 600);
        else
            CHECK(!strncmp(p + 1, "</chat>", 7));
        CHECK(p[-1] == '\n');
        count++;
    }
    CHECK(count == 4);
    char *close = strstr(ctx, "</chat>"), *ask = strstr(ctx, "The chat above is context only. Compress only the message below");
    CHECK(close && ask && ask > close && strstr(ask, "include nothing said only in other messages"));
    CHECK(ask && strstr(ask, "echo: www"));
    free(ctx);
    char *view = oc_render(m, 1), *plain = oc_render(m, 0);
    count = 0;
    for (char *p = view; (p = strstr(p, BACKEND_CACHE_MARK)); p++, count++)
        CHECK(p[-1] == '\n' && (size_t)(p - view) <= limits[count]);
    CHECK(count == 3 && !strstr(plain, BACKEND_CACHE_MARK));
    free(view);
    free(plain);

    /* A mark goes after the line that ended the previous render, and only
       when lines came after it. */
    long seen = 0;
    char *first = oc_render_from(m, 0, &seen);
    CHECK(seen == id + 1 && !strstr(first, BACKEND_CACHE_MARK));
    free(first);
    oc_append(m, "user", "next");
    char *next = oc_render_from(m, 0, &seen), *mark = strstr(next, BACKEND_CACHE_MARK);
    char want[32];
    snprintf(want, sizeof want, "%ld+1|user: next\n</chat>", id + 1);
    CHECK(mark && mark[-1] == '\n' && !strcmp(mark + 1, want) && seen == id + 2);
    free(next);
    char *again = oc_render_from(m, 0, &seen);
    CHECK(!strstr(again, BACKEND_CACHE_MARK) && seen == id + 2);
    free(again);
    oc_close(m);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        failures++;
}

static const char *script[4];
static int         nscript;
static char        sent[4][2048];

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
    char    longline[NODE + 40], cut[NODE + 1];
    memset(longline, 'k', sizeof longline - 1);
    longline[sizeof longline - 1] = '\0';
    memcpy(cut, longline, NODE);
    cut[NODE - 3] = '\0';
    script[0] = longline;
    script[1] = cut;
    script[2] = "short line";
    char *line = oc_build(&b, NODE, "prompt");
    CHECK(nscript == 3);
    CHECK(line && !strcmp(line, "short line"));
    CHECK(strstr(sent[2], "instead of copying the cut"));
    /* Retries are fresh asks: reset, the prompt, then the cut in its own block. */
    CHECK(resets == 3);
    CHECK(!strncmp(sent[1], "prompt" BACKEND_BLOCK_MARK, 7) && strstr(sent[1], "| \xe2\x86\x90 LIMIT"));
    free(line);

    char marked[NODE + 20];
    snprintf(marked, sizeof marked, "%.*s| \xe2\x86\x90 LIMIT", NODE - 20, longline);
    nscript = 0;
    script[1] = marked;
    line = oc_build(&b, NODE, "prompt");
    CHECK(nscript == 3);
    CHECK(line && !strcmp(line, "short line"));
    free(line);

    /* An API error is a failed try, never a line. */
    const char *error = "API Error: 400 A maximum of 4 blocks with cache_control may be provided. Found 5.";
    nscript = 0;
    script[0] = error;
    line = oc_build(&b, NODE, "prompt");
    CHECK(nscript == 1 && !line);
    free(line);
    nscript = 0;
    script[0] = "a line far too long";
    script[1] = error;
    line = oc_build(&b, 10, "prompt");
    CHECK(nscript == 2 && line && !strcmp(line, "a line far too long"));
    free(line);
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
    CHECK(!strncmp(z, "2+0|echo: yyy", 13));
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
    char *before = strdup(oc_node(m, 5, 3));
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
        CHECK(oc_settled(m));
        CHECK(oc_view_size(m) <= VIEW);
        tiles(m);
        CHECK(oc_append(m, "user", "after reopen") == n);
        oc_close(m);
    }
    free(before);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        failures++;
    compactor_test();
    failing_test();
    marks_test();
    copy_test();
    CHECK(strlen(OC_SCALE) == OC_NODE);
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    return failures != 0;
}
