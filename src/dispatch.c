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
/* how long a spawn reply waits for the backend to report its session id */
#define ID_WAIT_MS   30000

static int dir_path(char *out, size_t size)
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

/* the only address the request API accepts: a backend session id */
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

    /* send_next echoes a queued line when its turn starts; idle has no such path */
    if (!session_turn_running(workspace_at(at)))
        workspace_render(at, echo_prompt, (void *)line);
    if (!workspace_send(at, line, NULL))
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

/* a spawn whose reply is held until its session id is known, so the caller
   never has to address the tab by position */
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

    struct session *was = workspace_current();
    int at = workspace_spawn(backend, field(o, "model"), field(o, "effort"),
                             field(o, "cwd"), NULL);
    if (at < 0) {
        char out[300];
        snprintf(out, sizeof out, "{\"error\": \"could not start the %s CLI\"}", backend);
        reply(dir, base, out);
        cJSON_Delete(o);
        return;
    }

    const char *title = field(o, "title");
    if (title) {
        char name[81];
        struct session *s = workspace_at(at);
        snprintf(name, sizeof name, "%s", title);
        if (session_rename(s, name) == SESSION_RENAME_OK)
            session_set_naming(s, 0);
    }

    workspace_show(workspace_index_of(was));
    const char *prompt = field(o, "prompt");
    if (prompt) {
        workspace_render(at, echo_prompt, (void *)prompt);
        workspace_send(at, prompt, NULL);
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
    if (!dir_path(dir, sizeof dir))
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
