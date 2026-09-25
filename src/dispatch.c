#include "dispatch.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cmd.h"
#include "prompt.h"
#include "session.h"
#include "text.h"
#include "vendor/cJSON.h"
#include "workspace.h"

#define POLL_MS      250
#define REQUEST_MAX  65536

#define ID_WAIT_MS   30000

#define PAIR_WINDOW  60
#define PAIR_CAP     6
#define PAIR_SLOTS   64

int dispatch_dir(char *out, size_t size)
{
    const char *env = getenv("MUX_DISPATCH_DIR");
    if (env && *env)
        return (size_t)snprintf(out, size, "%s", env) < size;
    return path_config_subdir(out, size, "dispatch");
}

static const char *field(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)o, key));
    return s && *s ? s : NULL;
}

static void reply(const char *dir, const char *base, const char *json)
{
    char tmp[4400], path[4400];
    size_t stem = strlen(base) - 4;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s/%.*s.res.tmp", dir, (int)stem, base) >= sizeof tmp)
        return;
    snprintf(path, sizeof path, "%s/%.*s.res", dir, (int)stem, base);

    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fputs(json, f);
    fputc('\n', f);
    fclose(f);
    rename(tmp, path);
}

static void reply_error(const char *dir, const char *base, const char *what, const char *id)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "error", what);
    if (id)
        cJSON_AddStringToObject(r, "session", id);
    char *json = cJSON_PrintUnformatted(r);
    reply(dir, base, json ? json : "{\"error\": \"oom\"}");
    free(json);
    cJSON_Delete(r);
}

static void echo_prompt(struct session *s, void *ud)
{
    (void)s;
    prompt_echo_message(ud);
}

static int index_of_id(const cJSON *target)
{
    if (!cJSON_IsString(target))
        return -1;
    const char *id = target->valuestring;
    if (!id || !*id)
        return -1;
    return workspace_find_id(id);
}

static void close_session(const char *dir, const char *base, const cJSON *target)
{
    if (!cJSON_IsString(target) || !target->valuestring || !*target->valuestring) {
        reply_error(dir, base, "close takes a session id", NULL);
        return;
    }
    const char *id = target->valuestring;
    int at = index_of_id(target);
    struct session *s = workspace_at(at);
    if (!s) {
        reply_error(dir, base, "no such session", id);
        return;
    }
    if (at == workspace_index()) {
        reply_error(dir, base, "session is in view", id);
        return;
    }
    if (session_turn_running(s) || workspace_queued(at)) {
        reply_error(dir, base, "session is busy", id);
        return;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    cJSON_AddStringToObject(r, "session", id);
    char *json = cJSON_PrintUnformatted(r);
    workspace_close(at);
    reply(dir, base, json ? json : "{\"ok\":true}");
    free(json);
    cJSON_Delete(r);
}

static struct {
    char   pair[200];
    double at;
} delivered[PAIR_SLOTS];

static int pair_allowed(const char *from, const char *to)
{
    char pair[200];
    snprintf(pair, sizeof pair, "%s>%s", from, to);
    double now = now_seconds();
    int    seen = 0, slot = 0;
    for (int i = 0; i < PAIR_SLOTS; i++) {
        if (now - delivered[i].at < PAIR_WINDOW && !strcmp(delivered[i].pair, pair))
            seen++;
        if (delivered[i].at < delivered[slot].at)
            slot = i;
    }
    if (seen >= PAIR_CAP)
        return 0;
    snprintf(delivered[slot].pair, sizeof delivered[slot].pair, "%s", pair);
    delivered[slot].at = now;
    return 1;
}

static int deliver(int at, const char *line, const char *shown)
{
    if (!session_turn_running(workspace_at(at)))
        workspace_render(at, echo_prompt, (void *)shown);
    return workspace_send(at, line, shown);
}

static void send_session(const char *dir, const char *base, const cJSON *o, const cJSON *send)
{
    const char *line = cJSON_GetStringValue(send);
    cJSON *target = cJSON_GetObjectItem((cJSON *)o, "session");
    if (!cJSON_IsString(target) || !target->valuestring || !*target->valuestring) {
        reply_error(dir, base, "send takes a session id", NULL);
        return;
    }
    const char *id = target->valuestring;
    int at = index_of_id(target);
    if (!workspace_at(at)) {
        reply_error(dir, base, "no such session", id);
        return;
    }
    if (!line || !*line) {
        reply_error(dir, base, "bad line", id);
        return;
    }

    const char *from = field(o, "from");
    if (from && !pair_allowed(from, id)) {
        reply_error(dir, base, "too many messages to this session in the last minute", id);
        return;
    }
    char *framed = from ? text_dsprintf("[from @%s] %s", from, line) : NULL;
    char *shown = from ? text_dsprintf("from @%s: %s", from, line) : NULL;
    int   sent = from ? framed && shown && deliver(at, framed, shown) : dispatch_send(at, line);
    free(framed);
    free(shown);
    if (!sent)
        reply_error(dir, base, "could not send line", id);
    else {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "ok", 1);
        cJSON_AddStringToObject(r, "session", id);
        char *json = cJSON_PrintUnformatted(r);
        reply(dir, base, json ? json : "{\"ok\":true}");
        free(json);
        cJSON_Delete(r);
    }
}

struct pending {
    char            base[256];
    struct session *s;
    struct timespec since;
};

static struct pending pendings[WORKSPACE_MAX];

static long since_ms(const struct timespec *then, const struct timespec *now)
{
    return (now->tv_sec - then->tv_sec) * 1000 + (now->tv_nsec - then->tv_nsec) / 1000000;
}

static void reply_spawn(const char *dir, const char *base, const char *id, const char *addr)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "session", id);
    if (addr && *addr)
        cJSON_AddStringToObject(r, "addr", addr);
    char *json = cJSON_PrintUnformatted(r);
    reply(dir, base, json ? json : "{\"error\": \"oom\"}");
    free(json);
    cJSON_Delete(r);
}

static void hold_spawn(const char *dir, const char *base, struct session *s)
{
    for (int i = 0; i < WORKSPACE_MAX; i++) {
        if (pendings[i].base[0])
            continue;
        if ((size_t)snprintf(pendings[i].base, sizeof pendings[i].base, "%s", base) >=
            sizeof pendings[i].base) {
            pendings[i].base[0] = '\0';
            break;
        }
        pendings[i].s = s;
        clock_gettime(CLOCK_MONOTONIC, &pendings[i].since);
        return;
    }
    reply_error(dir, base, "session id unavailable", NULL);
}

static void settle_pending(const char *dir)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (int i = 0; i < WORKSPACE_MAX; i++) {
        if (!pendings[i].base[0])
            continue;
        int at = workspace_index_of(pendings[i].s);
        const char *id = at >= 0 ? session_id(workspace_at(at)) : NULL;
        if (id)
            reply_spawn(dir, pendings[i].base, id, session_addr(workspace_at(at)));
        else if (at < 0)
            reply_error(dir, pendings[i].base, "session ended before it reported an id", NULL);
        else if (since_ms(&pendings[i].since, &now) < ID_WAIT_MS)
            continue;
        else
            reply_error(dir, pendings[i].base, "session id unavailable", NULL);
        pendings[i].base[0] = '\0';
        pendings[i].s = NULL;
    }
}

int dispatch_spawn(const char *backend, const char *model, const char *effort, const char *cwd,
                   const char *title, const char *resume, const char *const *env,
                   const char *prompt)
{
    struct session *was = workspace_current();
    char here[4096];
    if (!cwd && getcwd(here, sizeof here))
        cwd = here;
    int at = workspace_spawn_env(backend, model, effort, cwd, resume, env);
    if (at < 0)
        return -1;

    if (title) {
        char name[81];
        struct session *s = workspace_at(at);
        snprintf(name, sizeof name, "%s", title);
        if (session_rename(s, name) == SESSION_RENAME_OK)
            session_set_naming(s, 0);
    }

    workspace_show(workspace_index_of(was));
    at = workspace_index_of(workspace_at(at));
    if (prompt)
        dispatch_send(at, prompt);
    return at;
}

int dispatch_send(int at, const char *line)
{
    return deliver(at, line, line);
}

static void serve(const char *dir, const char *base, const char *text)
{
    cJSON *o = cJSON_Parse(text);
    if (!o) {
        reply(dir, base, "{\"error\": \"bad json\"}");
        return;
    }

    cJSON *target = cJSON_GetObjectItem(o, "close");
    if (target) {
        close_session(dir, base, target);
        cJSON_Delete(o);
        return;
    }

    cJSON *send = cJSON_GetObjectItem(o, "send");
    if (send) {
        send_session(dir, base, o, send);
        cJSON_Delete(o);
        return;
    }

    const char *backend = field(o, "backend");
    if (!backend)
        backend = cmd_default_backend();

    int at = dispatch_spawn(backend, field(o, "model"), field(o, "effort"), field(o, "cwd"),
                            field(o, "title"), field(o, "resume"), NULL, field(o, "prompt"));
    if (at < 0) {
        char out[300];
        snprintf(out, sizeof out, "{\"error\": \"could not start the %s CLI\"}", backend);
        reply(dir, base, out);
        cJSON_Delete(o);
        return;
    }

    const char *id = session_id(workspace_at(at));
    if (id)
        reply_spawn(dir, base, id, session_addr(workspace_at(at)));
    else
        hold_spawn(dir, base, workspace_at(at));
    cJSON_Delete(o);
}

void dispatch_poll(void)
{
    static struct timespec last;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000;
    if (last.tv_sec && ms >= 0 && ms < POLL_MS)
        return;
    last = now;

    char dir[4200];
    if (!dispatch_dir(dir, sizeof dir))
        return;

    settle_pending(dir);

    DIR *d = opendir(dir);
    if (!d)
        return;

    char prefix[32];
    int plen = snprintf(prefix, sizeof prefix, "%ld-", (long)getpid());

    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len <= (size_t)plen + 4 || strncmp(e->d_name, prefix, plen))
            continue;
        if (strcmp(e->d_name + len - 4, ".req"))
            continue;

        char path[4400];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= sizeof path)
            continue;
        char *text = text_slurp(path, REQUEST_MAX, NULL);
        unlink(path);
        if (!text)
            continue;
        serve(dir, e->d_name, text);
        free(text);
    }
    closedir(d);
}
