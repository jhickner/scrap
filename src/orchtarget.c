#include "orchtarget.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text.h"
#include "vendor/cJSON.h"
#include "workspace.h"

#define LIVE_MAX 400
#define RECORD_MAX (256u << 10)

static int live_dir(char *out, size_t size)
{
    const char *env = getenv("MUX_LIVE_DIR");
    if (env && *env)
        return (size_t)snprintf(out, size, "%s", env) < size;
    return path_config_subdir(out, size, "live");
}

static int alive(long pid)
{
    if (pid <= 0)
        return 0;
    if (pid == (long)getpid())
        return 1;
    /* EPERM means it is there and owned by somebody else */
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

static int suffix_json(const char *name)
{
    size_t n = strlen(name);
    return n > 5 && !strcmp(name + n - 5, ".json");
}

/* Every live record, parsed. fn may stop the walk by returning 0. */
static void each_live(int (*fn)(const cJSON *rec, void *ud), void *ud)
{
    char dir[4200];
    if (!live_dir(dir, sizeof dir))
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) && n < LIVE_MAX) {
        if (!suffix_json(e->d_name))
            continue;
        char path[4400];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= sizeof path)
            continue;
        char *text = text_slurp(path, RECORD_MAX, NULL);
        if (!text)
            continue;
        cJSON *rec = cJSON_Parse(text);
        free(text);
        if (rec) {
            n++;
            int go = cJSON_IsObject(rec) ? fn(rec, ud) : 1;
            cJSON_Delete(rec);
            if (!go)
                break;
        }
    }
    closedir(d);
}

static long rec_pid(const cJSON *rec)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)rec, "pid");
    return cJSON_IsNumber(v) ? (long)cJSON_GetNumberValue(v) : 0;
}

static const char *rec_str(const cJSON *rec, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive((cJSON *)rec, key));
    return s ? s : "";
}

struct tally {
    struct orch_instance *v;
    int n, cap;
};

static int count_one(const cJSON *rec, void *ud)
{
    struct tally *t = ud;
    long pid = rec_pid(rec);
    if (!alive(pid))
        return 1;
    long ts = (long)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive((cJSON *)rec, "ts"));
    for (int i = 0; i < t->n; i++)
        if (t->v[i].pid == pid) {
            t->v[i].sessions++;
            if (ts > t->v[i].ts)
                t->v[i].ts = ts;
            return 1;
        }
    if (t->n == t->cap) {
        int next = t->cap ? t->cap * 2 : 8;
        struct orch_instance *p = realloc(t->v, (size_t)next * sizeof *p);
        if (!p)
            return 0;
        t->v = p;
        t->cap = next;
    }
    t->v[t->n++] = (struct orch_instance){.pid = pid, .sessions = 1, .ts = ts};
    return 1;
}

static int by_recent(const void *a, const void *b)
{
    const struct orch_instance *x = a, *y = b;
    if (x->ts != y->ts)
        return x->ts > y->ts ? -1 : 1;
    return x->pid > y->pid ? -1 : 1;
}

int orchtarget_instances(struct orch_instance **out)
{
    struct tally t = {0};
    each_live(count_one, &t);
    qsort(t.v, (size_t)t.n, sizeof *t.v, by_recent);
    *out = t.v;
    return t.n;
}

int orchtarget_has_room(const struct orch_instance *v)
{
    return v && v->sessions < WORKSPACE_MAX;
}

long orchtarget_pick(long prefer_pid)
{
    struct orch_instance *v = NULL;
    int n = orchtarget_instances(&v);
    long pick = 0;

    for (int i = 0; i < n && !pick; i++)
        if (v[i].pid == prefer_pid && orchtarget_has_room(&v[i]))
            pick = v[i].pid;
    for (int i = 0; i < n && !pick; i++)
        if (orchtarget_has_room(&v[i]))
            pick = v[i].pid;

    free(v);
    return pick;
}

struct hunt {
    const char *want;
    long        pid;
    char        status[32];
};

static int match_session(const cJSON *rec, void *ud)
{
    struct hunt *h = ud;
    if (strcmp(rec_str(rec, "id"), h->want))
        return 1;
    long pid = rec_pid(rec);
    if (!alive(pid))
        return 1;
    h->pid = pid;
    snprintf(h->status, sizeof h->status, "%s", rec_str(rec, "status"));
    return 0;
}

int orchtarget_session_status(const char *session, char *out, size_t size, long *pid)
{
    if (pid)
        *pid = 0;
    if (out && size)
        *out = '\0';
    if (!session || !*session)
        return 0;
    struct hunt h = {.want = session};
    each_live(match_session, &h);
    if (!h.pid)
        return 0;
    if (pid)
        *pid = h.pid;
    if (out && size)
        snprintf(out, size, "%s", h.status);
    return 1;
}

struct decorate {
    struct orch_rec *recs;
    int              n;
};

static int decorate_one(const cJSON *rec, void *ud)
{
    struct decorate *d = ud;
    const char *session = rec_str(rec, "id");
    const char *status = rec_str(rec, "status");
    if (!*session || !*status || !alive(rec_pid(rec)))
        return 1;
    for (int i = 0; i < d->n; i++)
        if (d->recs[i].session[0] && !strcmp(d->recs[i].session, session))
            snprintf(d->recs[i].live, sizeof d->recs[i].live, "%s", status);
    return 1;
}

void orchtarget_apply_live(struct orch_rec *recs, int n)
{
    if (!recs || n <= 0)
        return;
    for (int i = 0; i < n; i++)
        recs[i].live[0] = '\0';
    struct decorate d = {recs, n};
    each_live(decorate_one, &d);
}

long orchtarget_session_pid(const char *session)
{
    long pid = 0;
    orchtarget_session_status(session, NULL, 0, &pid);
    return pid;
}
