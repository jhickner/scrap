#include "orchevent.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "orchtask.h"
#include "text.h"
#include "vendor/cJSON.h"

#define EVENT_MAX 500
#define FILE_MAX  (64u << 10)

static int events_dir(char *out, size_t size)
{
    char root[4200];
    if (!orchtask_root(root, sizeof root))
        return 0;
    if ((size_t)snprintf(out, size, "%s/events", root) >= size)
        return 0;
    if (mkdir(out, 0700) != 0 && errno != EEXIST)
        return 0;
    return 1;
}

static const char *str(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive((cJSON *)o, key));
    return s ? s : "";
}

static void copy(char *out, size_t size, const char *s)
{
    snprintf(out, size, "%s", s ? s : "");
}

static int suffix_json(const char *name)
{
    size_t n = strlen(name);
    return n > 5 && !strcmp(name + n - 5, ".json");
}

static int read_event(const char *dir, const char *name, struct orch_event *out)
{
    char path[4400];
    if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, name) >= sizeof path)
        return 0;
    char *text = text_slurp(path, FILE_MAX, NULL);
    if (!text)
        return 0;
    cJSON *o = cJSON_Parse(text);
    free(text);
    if (!o)
        return 0;

    memset(out, 0, sizeof *out);
    copy(out->task, sizeof out->task, str(o, "task"));
    copy(out->session, sizeof out->session, str(o, "session"));
    copy(out->notify, sizeof out->notify, str(o, "notify"));
    copy(out->reason, sizeof out->reason, str(o, "reason"));
    out->ts = (long)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(o, "ts"));
    cJSON_Delete(o);

    copy(out->id, sizeof out->id, name);
    char *dot = strrchr(out->id, '.');
    if (dot)
        *dot = '\0';
    return out->id[0] != '\0';
}

static int order(const void *a, const void *b)
{
    const struct orch_event *x = a, *y = b;
    if (x->ts != y->ts)
        return x->ts < y->ts ? -1 : 1;
    return strcmp(x->id, y->id);
}

int orchevent_pending(struct orch_event **out)
{
    *out = NULL;
    char dir[4200];
    if (!events_dir(dir, sizeof dir))
        return -1;
    DIR *d = opendir(dir);
    if (!d)
        return -1;

    struct orch_event *v = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < EVENT_MAX) {
        if (!suffix_json(e->d_name))
            continue;
        if (n == cap) {
            int next = cap ? cap * 2 : 16;
            struct orch_event *p = realloc(v, (size_t)next * sizeof *p);
            if (!p)
                break;
            v = p;
            cap = next;
        }
        if (read_event(dir, e->d_name, &v[n]))
            n++;
    }
    closedir(d);
    qsort(v, (size_t)n, sizeof *v, order);
    *out = v;
    return n;
}

static int already_pending(const char *task, const char *reason)
{
    struct orch_event *v = NULL;
    int n = orchevent_pending(&v);
    int hit = 0;
    for (int i = 0; i < n && !hit; i++)
        hit = !strcmp(v[i].task, task) && !strcmp(v[i].reason, reason);
    free(v);
    return hit;
}

struct writer {
    const char *json;
};

static int write_json(FILE *f, void *ud)
{
    const struct writer *w = ud;
    return fprintf(f, "%s\n", w->json) > 0;
}

int orchevent_emit(const char *task, const char *session, const char *notify,
                   const char *reason)
{
    if (!task || !*task || !reason || !*reason)
        return 0;
    char dir[4200];
    if (!events_dir(dir, sizeof dir))
        return 0;
    if (already_pending(task, reason))
        return 1;

    cJSON *o = cJSON_CreateObject();
    if (!o)
        return 0;
    long now = (long)time(NULL);
    cJSON_AddStringToObject(o, "task", task);
    cJSON_AddStringToObject(o, "session", session ? session : "");
    cJSON_AddStringToObject(o, "notify", notify ? notify : "");
    cJSON_AddStringToObject(o, "reason", reason);
    cJSON_AddNumberToObject(o, "ts", (double)now);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!json)
        return 0;

    /* the name carries the order and keeps two instances from colliding */
    char path[4600];
    snprintf(path, sizeof path, "%s/%ld-%ld-%s-%s.json", dir, now, (long)getpid(),
             task, reason);
    struct writer w = {json};
    int ok = text_spit(path, write_json, &w);
    free(json);
    return ok;
}

int orchevent_ack(const char *id)
{
    char dir[4200], path[4400];
    if (!id || !*id || strchr(id, '/') || !events_dir(dir, sizeof dir))
        return 0;
    if ((size_t)snprintf(path, sizeof path, "%s/%s.json", dir, id) >= sizeof path)
        return 0;
    return unlink(path) == 0;
}
