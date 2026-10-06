#include "quota.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "filelock.h"
#include "text.h"
#include "vendor/agents/core/core.h"
#include "vendor/cJSON.h"

#define QUOTA_CLAIM 90

enum { FETCH_IDLE, FETCH_RUNNING, FETCH_FAILED };

struct record {
    backend_rate_limit r;
    long               fetched_at;
    long               claimed_until;
};

struct slot {
    const char   *name;
    int           timed;
    struct record rec;
    long          checked;
    long          wanted;
    long          due;
    atomic_int    fetch;
};

static struct slot slots[] = {
    {.name = "claude"},
    {.name = "codex"},
    {.name = "grok", .timed = 1},
    {.name = "openrouter", .timed = 1},
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static struct slot *find(const char *name)
{
    for (size_t i = 0; name && i < sizeof slots / sizeof *slots; i++)
        if (!strcmp(slots[i].name, name))
            return &slots[i];
    return NULL;
}

const char *quota_provider(const char *backend, const char *model)
{
    if (!backend)
        return NULL;
    if (!strcmp(backend, "claude") || !strcmp(backend, "codex") || !strcmp(backend, "grok"))
        return find(backend)->name;
    if ((!strcmp(backend, "core") || !strcmp(backend, "pi")) && model &&
        !strncmp(model, "openrouter/", 11))
        return "openrouter";
    return NULL;
}

static int path_of(const struct slot *s, char *out, size_t size)
{
    char dir[4096];
    return path_config_subdir(dir, sizeof dir, "quota/") &&
           (size_t)snprintf(out, size, "%s%s.json", dir, s->name) < size;
}

static long num(const cJSON *j, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(j, key);
    return cJSON_IsNumber(v) ? (long)v->valuedouble : 0;
}

static void load(const char *path, struct record *out)
{
    memset(out, 0, sizeof *out);
    char  *text = text_slurp(path, 1 << 16, NULL);
    cJSON *j = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!j)
        return;
    const cJSON *bal = cJSON_GetObjectItem(j, "balance_usd");
    const cJSON *pct = cJSON_GetObjectItem(j, "used_percent");
    if (cJSON_IsNumber(bal)) {
        out->r.available = 1;
        out->r.kind = BACKEND_QUOTA_BALANCE;
        out->r.balance_usd = bal->valuedouble;
    } else if (cJSON_IsNumber(pct)) {
        out->r.available = 1;
        out->r.kind = BACKEND_QUOTA_PERCENT;
        out->r.used_percent = (int)pct->valuedouble;
        out->r.resets_at = num(j, "resets_at");
        out->r.window_minutes = num(j, "window_minutes");
    }
    out->fetched_at = num(j, "fetched_at");
    out->claimed_until = num(j, "claimed_until");
    cJSON_Delete(j);
}

static void store(const char *path, const struct record *rec)
{
    cJSON *j = cJSON_CreateObject();
    if (rec->r.available && rec->r.kind == BACKEND_QUOTA_BALANCE) {
        cJSON_AddNumberToObject(j, "balance_usd", rec->r.balance_usd);
    } else if (rec->r.available) {
        cJSON_AddNumberToObject(j, "used_percent", rec->r.used_percent);
        cJSON_AddNumberToObject(j, "resets_at", (double)rec->r.resets_at);
        cJSON_AddNumberToObject(j, "window_minutes", (double)rec->r.window_minutes);
    }
    cJSON_AddNumberToObject(j, "fetched_at", (double)rec->fetched_at);
    if (rec->claimed_until)
        cJSON_AddNumberToObject(j, "claimed_until", (double)rec->claimed_until);
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    char tmp[4300];
    FILE *f = text && (size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) < sizeof tmp
                  ? fopen(tmp, "w") : NULL;
    if (f) {
        int ok = fputs(text, f) >= 0;
        ok &= fclose(f) == 0;
        if (!ok || rename(tmp, path) != 0)
            unlink(tmp);
    }
    free(text);
}

static void refresh(struct slot *s)
{
    long now = (long)time(NULL);
    char path[4200];
    if (s->checked == now || !path_of(s, path, sizeof path))
        return;
    s->checked = now;
    load(path, &s->rec);
}

int quota_fresh(const char *provider)
{
    struct slot *s = find(provider);
    if (!s)
        return 0;
    pthread_mutex_lock(&mu);
    refresh(s);
    int fresh = s->rec.fetched_at && (long)time(NULL) - s->rec.fetched_at < QUOTA_EVERY;
    pthread_mutex_unlock(&mu);
    return fresh;
}

int quota_read(const char *provider, backend_rate_limit *out)
{
    struct slot *s = find(provider);
    if (!s)
        return 0;
    pthread_mutex_lock(&mu);
    refresh(s);
    int ok = s->rec.r.available;
    if (ok)
        *out = s->rec.r;
    pthread_mutex_unlock(&mu);
    return ok;
}

void quota_note(const char *provider, const backend_rate_limit *r)
{
    struct slot *s = find(provider);
    char path[4200];
    if (!s || !r->available || !path_of(s, path, sizeof path))
        return;
    pthread_mutex_lock(&mu);
    int lock = filelock_acquire(path, LOCK_EX);
    struct record rec;
    load(path, &rec);
    rec.r = *r;
    rec.fetched_at = (long)time(NULL);
    store(path, &rec);
    filelock_release(lock);
    s->rec = rec;
    s->checked = rec.fetched_at;
    pthread_mutex_unlock(&mu);
}

void quota_want(const char *provider)
{
    struct slot *s = find(provider);
    if (!s || !s->timed)
        return;
    pthread_mutex_lock(&mu);
    refresh(s);
    long now = (long)time(NULL);
    if (!s->wanted)
        s->wanted = now;
    long at = s->rec.fetched_at + QUOTA_EVERY;
    if (at < now)
        at = now;
    if (!s->due || at < s->due)
        s->due = at;
    pthread_mutex_unlock(&mu);
}

static void fetch_grok(backend_rate_limit *r)
{
    backend_opts o = {.name = "grok", .ephemeral = 1, .disable_tools = 1};
    Backend *b = backend_open_ex(&o);
    if (!b)
        return;
    if (b->start(b, NULL) && b->connect && b->connect(b) && b->rate_limit)
        b->rate_limit(b, r);
    b->close(b);
}

static void *fetch_thread(void *ud)
{
    struct slot *s = ud;
    backend_rate_limit r = {0};
    double usd;
    if (!strcmp(s->name, "openrouter") && core_agent_openrouter_balance(&usd)) {
        r.available = 1;
        r.kind = BACKEND_QUOTA_BALANCE;
        r.balance_usd = usd;
    } else if (!strcmp(s->name, "grok")) {
        fetch_grok(&r);
    }
    char path[4200];
    if (path_of(s, path, sizeof path)) {
        int lock = filelock_acquire(path, LOCK_EX);
        struct record rec;
        load(path, &rec);
        if (r.available) {
            rec.r = r;
            rec.fetched_at = (long)time(NULL);
        }
        rec.claimed_until = 0;
        store(path, &rec);
        filelock_release(lock);
    }
    atomic_store(&s->fetch, r.available ? FETCH_IDLE : FETCH_FAILED);
    return NULL;
}

static void start_fetch(struct slot *s, long now)
{
    char path[4200];
    if (!path_of(s, path, sizeof path))
        return;
    int lock = filelock_acquire(path, LOCK_EX);
    struct record rec;
    load(path, &rec);
    if (rec.fetched_at >= s->wanted || rec.claimed_until > now) {
        filelock_release(lock);
        s->rec = rec;
        s->due = rec.fetched_at >= s->wanted ? 0 : rec.claimed_until;
        if (!s->due)
            s->wanted = 0;
        return;
    }
    rec.claimed_until = now + QUOTA_CLAIM;
    store(path, &rec);
    filelock_release(lock);

    atomic_store(&s->fetch, FETCH_RUNNING);
    pthread_t t;
    if (pthread_create(&t, NULL, fetch_thread, s) != 0) {
        atomic_store(&s->fetch, FETCH_FAILED);
        return;
    }
    pthread_detach(t);
    s->due = 0;
    s->wanted = 0;
}

void quota_tick(void)
{
    long now = (long)time(NULL);
    pthread_mutex_lock(&mu);
    for (size_t i = 0; i < sizeof slots / sizeof *slots; i++) {
        struct slot *s = &slots[i];
        int state = atomic_load(&s->fetch);
        if (state == FETCH_RUNNING)
            continue;
        if (state == FETCH_FAILED) {
            atomic_store(&s->fetch, FETCH_IDLE);
            s->wanted = s->wanted ? s->wanted : now;
            s->due = now + QUOTA_EVERY;
            continue;
        }
        if (!s->due || now < s->due)
            continue;
        s->checked = 0;
        refresh(s);
        if (s->rec.fetched_at >= s->wanted) {
            s->due = s->wanted = 0;
        } else if (now - s->rec.fetched_at < QUOTA_EVERY) {
            s->due = s->rec.fetched_at + QUOTA_EVERY;
        } else {
            start_fetch(s, now);
        }
    }
    pthread_mutex_unlock(&mu);
}
