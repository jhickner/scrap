#include "quota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "text.h"
#include "vendor/cJSON.h"

#define QUOTA_MAX_BYTES (1u << 16)
#define QUOTA_BACKENDS  8

#define QUOTA_STALE_SECS (6 * 3600)

struct reading {
    char   backend[32];
    int    percent;
    long   resets_at;
    long   window_minutes;
    time_t at;
};

static struct reading readings[QUOTA_BACKENDS];
static int            nreadings;
static time_t         read_at;

static const char *path(void)
{
    static char p[4200];
    if (!p[0] && !path_config_file(p, sizeof p, "quota.json"))
        snprintf(p, sizeof p, "/tmp/mux-quota.json");
    return p;
}

static struct reading *slot_for(const char *backend)
{
    for (int i = 0; i < nreadings; i++)
        if (!strcmp(readings[i].backend, backend))
            return &readings[i];
    if (nreadings >= QUOTA_BACKENDS)
        return NULL;
    struct reading *r = &readings[nreadings++];
    memset(r, 0, sizeof *r);
    snprintf(r->backend, sizeof r->backend, "%s", backend);
    return r;
}

static void load(void)
{
    time_t now = time(NULL);
    if (read_at && now - read_at < 5)
        return;
    read_at = now;

    char *text = text_slurp(path(), QUOTA_MAX_BYTES, NULL);
    if (!text)
        return;
    cJSON *o = cJSON_Parse(text);
    free(text);
    if (!o)
        return;

    for (cJSON *e = o->child; e; e = e->next) {
        if (!cJSON_IsObject(e) || !e->string)
            continue;
        struct reading *r = slot_for(e->string);
        if (!r)
            continue;
        const cJSON *at = cJSON_GetObjectItem(e, "at");

        if (at && cJSON_IsNumber(at) && (time_t)at->valuedouble <= r->at)
            continue;
        const cJSON *p = cJSON_GetObjectItem(e, "percent");
        const cJSON *rs = cJSON_GetObjectItem(e, "resets_at");
        const cJSON *w = cJSON_GetObjectItem(e, "window_minutes");
        r->percent = p && cJSON_IsNumber(p) ? (int)p->valuedouble : 0;
        r->resets_at = rs && cJSON_IsNumber(rs) ? (long)rs->valuedouble : 0;
        r->window_minutes = w && cJSON_IsNumber(w) ? (long)w->valuedouble : 0;
        r->at = at && cJSON_IsNumber(at) ? (time_t)at->valuedouble : 0;
    }
    cJSON_Delete(o);
}

static void save(void)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return;
    for (int i = 0; i < nreadings; i++) {
        cJSON *e = cJSON_AddObjectToObject(o, readings[i].backend);
        if (!e)
            break;
        cJSON_AddNumberToObject(e, "percent", readings[i].percent);
        cJSON_AddNumberToObject(e, "resets_at", (double)readings[i].resets_at);
        cJSON_AddNumberToObject(e, "window_minutes", (double)readings[i].window_minutes);
        cJSON_AddNumberToObject(e, "at", (double)readings[i].at);
    }
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!text)
        return;

    char tmp[4300];
    snprintf(tmp, sizeof tmp, "%s.tmp", path());
    FILE *f = fopen(tmp, "wb");
    int   ok = f != NULL;
    if (ok && fprintf(f, "%s\n", text) < 0)
        ok = 0;
    if (f && fclose(f) != 0)
        ok = 0;
    free(text);
    if (!ok || rename(tmp, path()) != 0)
        unlink(tmp);
}

void quota_note(const char *backend, int percent, long resets_at,
                long window_minutes)
{
    if (!backend || !*backend || percent < 0 || percent > 100)
        return;
    load();

    struct reading *r = slot_for(backend);
    if (!r)
        return;
    if (r->percent == percent && r->resets_at == resets_at &&
        r->window_minutes == window_minutes)
        return;

    r->percent = percent;
    r->resets_at = resets_at > 0 ? resets_at : 0;
    r->window_minutes = window_minutes > 0 ? window_minutes : 0;
    r->at = time(NULL);
    save();
}

int quota_get(const char *backend, int *percent, long *resets_at)
{
    if (percent)
        *percent = 0;
    if (resets_at)
        *resets_at = 0;
    if (!backend || !*backend)
        return 0;
    load();

    for (int i = 0; i < nreadings; i++) {
        struct reading *r = &readings[i];
        if (strcmp(r->backend, backend))
            continue;
        if (!r->at || time(NULL) - r->at > QUOTA_STALE_SECS)
            return 0;

        int spent = r->percent;
        if (r->resets_at && time(NULL) >= r->resets_at)
            spent = 0;

        if (percent)
            *percent = spent;
        if (resets_at)
            *resets_at = r->resets_at;
        return 1;
    }
    return 0;
}

int quota_resets_in(const char *backend)
{
    long resets_at = 0;
    if (!quota_get(backend, NULL, &resets_at) || !resets_at)
        return -1;
    long left = resets_at - (long)time(NULL);
    return left > 0 ? (int)((left + 59) / 60) : 0;
}
