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

static int dir_path(char *out, size_t size)
{
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

static void echo_prompt(struct session *s, void *ud)
{
    (void)s;
    prompt_echo_message(ud);
}

static void close_slot(const char *dir, const char *base, const cJSON *target)
{
    int at = -1;
    if (cJSON_IsNumber(target) && target->valuedouble == target->valueint)
        at = workspace_at(target->valueint) ? target->valueint : -1;
    else if (cJSON_IsString(target))
        at = workspace_find_id(target->valuestring);

    struct session *s = workspace_at(at);
    if (!s) {
        reply(dir, base, "{\"error\": \"no such slot or session\"}");
        return;
    }
    if (at == workspace_index()) {
        reply(dir, base, "{\"error\": \"slot is in view\"}");
        return;
    }
    if (session_turn_running(s) || workspace_queued(at)) {
        reply(dir, base, "{\"error\": \"slot is busy\"}");
        return;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", 1);
    cJSON_AddNumberToObject(r, "slot", at);
    const char *id = session_id(s);
    if (id)
        cJSON_AddStringToObject(r, "session", id);
    char *json = cJSON_PrintUnformatted(r);
    workspace_close(at);
    reply(dir, base, json ? json : "{\"ok\":true}");
    free(json);
    cJSON_Delete(r);
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
        close_slot(dir, base, target);
        cJSON_Delete(o);
        return;
    }

    cJSON *send = cJSON_GetObjectItem(o, "send");
    if (send) {
        const char *line = cJSON_GetStringValue(send);
        cJSON *slot = cJSON_GetObjectItem(o, "slot");
        int at = cJSON_IsNumber(slot) ? slot->valueint : -1;
        if (!cJSON_IsNumber(slot) || slot->valuedouble != at || !workspace_at(at)) {
            reply(dir, base, "{\"error\": \"bad slot\"}");
        } else if (!line || !*line) {
            reply(dir, base, "{\"error\": \"bad line\"}");
        } else if (!workspace_send(at, line, NULL)) {
            reply(dir, base, "{\"error\": \"could not send line\"}");
        } else {
            reply(dir, base, "{\"ok\":true}");
        }
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

    workspace_show(workspace_index_of(was));
    const char *prompt = field(o, "prompt");
    if (prompt) {
        workspace_render(at, echo_prompt, (void *)prompt);
        workspace_send(at, prompt, NULL);
    }

    const char *id = session_id(workspace_at(at));
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "slot", at);
    if (id)
        cJSON_AddStringToObject(r, "session", id);
    char *json = cJSON_PrintUnformatted(r);
    reply(dir, base, json ? json : "{\"error\": \"oom\"}");
    free(json);
    cJSON_Delete(r);
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
