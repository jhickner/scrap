#include "sessionload.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "grokbottail.h"
#include "md.h"
#include "prompt.h"
#include "sessionlist.h"
#include "session.h"
#include "sessionview.h"
#include "transcript.h"
#include "toolstyle.h"
#include "ui.h"
#include "viewport.h"
#include "vendor/agents/backend.h"
#include "vendor/cJSON.h"

#define TURNS_MAX  400
#define LINE_MAX   (4 << 20)

static int is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int pi_find(const char *dir, const char *id, char *out, size_t size)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;

    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len < 7 || strcmp(e->d_name + len - 6, ".jsonl") != 0)
            continue;
        if (!strstr(e->d_name, id))
            continue;
        if ((size_t)snprintf(out, size, "%s/%s", dir, e->d_name) < size)
            found = is_file(out);
    }
    closedir(d);
    return found;
}

static int codex_find(const char *dir, const char *id, int depth, char *out,
                      size_t size)
{
    DIR *d = opendir(dir);
    if (!d)
        return 0;

    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;

        char path[4096];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >=
            sizeof path)
            continue;

        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode) && depth > 0) {
            found = codex_find(path, id, depth - 1, out, size);
            continue;
        }
        if (!S_ISREG(st.st_mode))
            continue;

        size_t name_n = strlen(e->d_name), id_n = strlen(id);
        if (name_n < id_n + 6 || strcmp(e->d_name + name_n - 6, ".jsonl") ||
            strncmp(e->d_name + name_n - 6 - id_n, id, id_n))
            continue;
        if ((size_t)snprintf(out, size, "%s", path) < size)
            found = 1;
    }
    closedir(d);
    return found;
}

int sessionload_path(const char *backend, const char *cwd, const char *id,
                     char *out, size_t size)
{
    char dir[2048];
    if (!id || !*id || strchr(id, '/'))
        return 0;
    if (!sessionlist_available(backend))
        return 0;
    if (!sessionlist_dir(backend, cwd, dir, sizeof dir))
        return 0;

    if (!strcmp(backend, "codex"))
        return codex_find(dir, id, 3, out, size);
    if (!strcmp(backend, "grok"))
        return (size_t)snprintf(out, size, "%s/%s/chat_history.jsonl", dir, id) < size &&
               is_file(out);
    if (!strcmp(backend, "pi"))
        return pi_find(dir, id, out, size);
    return (size_t)snprintf(out, size, "%s/%s.jsonl", dir, id) < size && is_file(out);
}

enum role {
    ROLE_NONE,
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_THINKING,
};

static enum role role_of(const char *name)
{
    if (!name)
        return ROLE_NONE;
    if (!strcmp(name, "user"))
        return ROLE_USER;
    if (!strcmp(name, "assistant"))
        return ROLE_ASSISTANT;
    if (!strcmp(name, "reasoning") || !strcmp(name, "thinking"))
        return ROLE_THINKING;
    return ROLE_NONE;
}

static enum role line_message(const cJSON *ev, const cJSON **content)
{
    *content = NULL;
    if (cJSON_IsTrue(cJSON_GetObjectItem(ev, "isMeta")) ||
        cJSON_IsTrue(cJSON_GetObjectItem(ev, "isSidechain")))
        return ROLE_NONE;

    const cJSON *body = ev;
    const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "type"));
    if (type && !strcmp(type, "response_item"))
        body = cJSON_GetObjectItem(ev, "payload");
    const cJSON *message = body ? cJSON_GetObjectItem(body, "message") : NULL;
    if (message)
        body = message;
    if (!body)
        return ROLE_NONE;
    const char  *role = cJSON_GetStringValue(cJSON_GetObjectItem(body, "role"));
    if (!role)
        role = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "type"));

    *content = cJSON_GetObjectItem(body, "content");
    return *content ? role_of(role) : ROLE_NONE;
}

static int user_text(const char *text, char *out, size_t size)
{
    if (!text || !*text)
        return 0;

    const char *open = strstr(text, "<user_query>");
    const char *end = NULL;
    if (open) {
        text = open + strlen("<user_query>");
        end = strstr(text, "</user_query>");
    }

    size_t n = end ? (size_t)(end - text) : strlen(text);
    while (n && (text[n - 1] == '\n' || text[n - 1] == ' '))
        n--;
    while (n && (*text == '\n' || *text == ' ')) {
        text++;
        n--;
    }
    if (!n || n >= size)
        n = n >= size ? size - 1 : n;
    if (!n)
        return 0;

    memcpy(out, text, n);
    out[n] = '\0';
    if (!strncmp(out, "<system-reminder>", 17) || !strncmp(out, "<user_info>", 11) ||
        !strncmp(out, "Caveat:", 7) || out[0] == '<')
        return 0;
    return 1;
}

static int grow_text(char **dst, size_t *len, size_t *cap, const char *src)
{
    if (!src || !*src)
        return 1;
    size_t n = strlen(src);
    if (*len + n + 1 > *cap) {
        size_t next = *cap ? *cap : 256;
        while (next < *len + n + 1)
            next *= 2;
        char *grown = realloc(*dst, next);
        if (!grown)
            return 0;
        *dst = grown;
        *cap = next;
    }
    memcpy(*dst + *len, src, n + 1);
    *len += n;
    return 1;
}

static int content_plain(const cJSON *content, char **dst, size_t *len, size_t *cap)
{
    if (cJSON_IsString(content))
        return grow_text(dst, len, cap, content->valuestring);
    if (!cJSON_IsArray(content))
        return 1;

    const cJSON *block;
    cJSON_ArrayForEach(block, content) {
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
        const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(block, "text"));
        if (!kind || !body || !*body)
            continue;
        if (strcmp(kind, "text") && strcmp(kind, "input_text") &&
            strcmp(kind, "output_text"))
            continue;
        if (*len && !grow_text(dst, len, cap, "\n"))
            return 0;
        if (!grow_text(dst, len, cap, body))
            return 0;
    }
    return 1;
}

static char *user_from(const cJSON *content)
{
    char *plain = NULL;
    size_t len = 0, cap = 0;
    if (!content_plain(content, &plain, &len, &cap) || !plain) {
        free(plain);
        return NULL;
    }
    char *out = malloc(len + 1);
    if (!out) {
        free(plain);
        return NULL;
    }
    if (!user_text(plain, out, len + 1)) {
        free(plain);
        free(out);
        return NULL;
    }
    free(plain);
    return out;
}

static void draw_tool(const cJSON *block, const char *cwd)
{
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(block, "name"));
    if (!name)
        return;

    cJSON *input = cJSON_GetObjectItem(block, "input");
    char  *json = input ? cJSON_PrintUnformatted(input) : NULL;

    backend_event ev = {0};
    ev.kind = BACKEND_EV_TOOL;
    ev.name = name;
    ev.input_json = json;

    char arg[4096];
    view_tool_argument(&ev, cwd, arg, sizeof arg);
    view_keep_tool_call(name, arg, toolstyle_collapses(name, json, NULL));
    free(json);
}

static int draw_message(enum role role, const cJSON *content, const char *cwd,
                        int thinking)
{
    char text[8192];
    int  drew = 0;

    if (cJSON_IsString(content)) {
        if (role == ROLE_USER && user_text(content->valuestring, text, sizeof text)) {
            prompt_echo_message(text);
            return 1;
        }
        if (role == ROLE_ASSISTANT && *content->valuestring) {
            md_render_kept(content->valuestring, 0);
            return 1;
        }
        if (role == ROLE_THINKING && thinking && *content->valuestring) {
            view_keep_activity("\xe2\x9c\xbb", content->valuestring, UI_THINKING);
            return 1;
        }
        return 0;
    }

    const cJSON *block;
    cJSON_ArrayForEach(block, content) {
        const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
        const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(block, "text"));
        if (!kind)
            continue;

        if ((!strcmp(kind, "text") || !strcmp(kind, "input_text") ||
             !strcmp(kind, "output_text")) && body) {
            if (role == ROLE_USER) {
                if (user_text(body, text, sizeof text)) {
                    prompt_echo_message(text);
                    drew = 1;
                }
            } else if (*body) {
                md_render_kept(body, 0);
                drew = 1;
            }
        } else if (!strcmp(kind, "thinking") || !strcmp(kind, "reasoning")) {
            const char *reason = body ? body
                                      : cJSON_GetStringValue(
                                            cJSON_GetObjectItem(block, "thinking"));
            if (thinking && reason && *reason) {
                view_keep_activity("\xe2\x9c\xbb", reason, UI_THINKING);
                drew = 1;
            }
        } else if (!strcmp(kind, "tool_use")) {
            draw_tool(block, cwd);
            drew = 1;
        }
    }
    return drew;
}

static int draw_codex_tool(const cJSON *ev, const char *cwd)
{
    const char *outer = cJSON_GetStringValue(cJSON_GetObjectItem(ev, "type"));
    if (!outer || strcmp(outer, "response_item"))
        return 0;

    const cJSON *item = cJSON_GetObjectItem(ev, "payload");
    const char *type = item
        ? cJSON_GetStringValue(cJSON_GetObjectItem(item, "type"))
        : NULL;
    if (!type || (strcmp(type, "custom_tool_call") &&
                  strcmp(type, "function_call")))
        return 0;

    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
    const char *input = cJSON_GetStringValue(cJSON_GetObjectItem(item, "input"));
    if (!input)
        input = cJSON_GetStringValue(cJSON_GetObjectItem(item, "arguments"));

    backend_event tool = {.kind = BACKEND_EV_TOOL,
                          .name = name ? name : "tool",
                          .arg = input ? input : ""};
    char arg[4096];
    view_tool_argument(&tool, cwd, arg, sizeof arg);
    view_keep_tool_call(tool.name, arg, toolstyle_collapses(tool.name, NULL, arg));
    return 1;
}

/* counts the turns in the file and reports where the last TURNS_MAX of them
   start, so the replay reads the tail rather than the whole file again */
static int count_turns(FILE *f, long *from)
{
    char   *line = NULL;
    size_t  cap = 0;
    ssize_t n;
    int     turns = 0;
    long    at[TURNS_MAX];
    long    here = ftell(f);

    while ((n = getline(&line, &cap, f)) > 0) {
        long next = ftell(f);
        if (n <= LINE_MAX) {
            cJSON *ev = cJSON_ParseWithLength(line, (size_t)n);
            if (ev) {
                const cJSON *content = NULL;
                if (line_message(ev, &content) != ROLE_NONE)
                    at[turns++ % TURNS_MAX] = here;
                cJSON_Delete(ev);
            }
        }
        here = next;
    }
    free(line);

    *from = turns > TURNS_MAX ? at[(turns - TURNS_MAX) % TURNS_MAX] : 0;
    return turns;
}

int sessionload_into(const struct session *s)
{
    if (grokbottail_applies(s))
        return grokbottail_show((struct session *)s, GROKBOTTAIL_DEFAULT, 0) > 0;
    const char *id = s ? session_id(s) : NULL;
    if (!id)
        return 0;
    return sessionload_replay(session_backend(s), session_cwd(s), id,
                              session_thinking(s));
}

int sessionload_replay(const char *backend, const char *cwd, const char *id,
                       int thinking)
{
    char path[4096];
    if (!sessionload_path(backend, cwd, id, path, sizeof path))
        return 0;

    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    long from = 0;
    int  turns = count_turns(f, &from);
    int  skip = turns > TURNS_MAX ? turns - TURNS_MAX : 0;
    if (fseek(f, from, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    if (skip) {
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_bar(ui_style(UI_DIM), "\xe2\x80\xa6 %d earlier turns not shown", skip);
        viewport_item_end();
    }

    char   *line = NULL;
    size_t  cap = 0;
    ssize_t n;
    int     drawn = 0;

    while ((n = getline(&line, &cap, f)) > 0) {
        if (n > LINE_MAX)
            continue;
        cJSON *ev = cJSON_ParseWithLength(line, (size_t)n);
        if (!ev)
            continue;

        const cJSON *content = NULL;
        enum role role = line_message(ev, &content);
        if (role != ROLE_NONE)
            drawn += draw_message(role, content, cwd, thinking);
        else
            drawn += draw_codex_tool(ev, cwd);
        cJSON_Delete(ev);
    }
    free(line);
    fclose(f);
    ui_flush();
    return drawn;
}

int sessionload_fill(struct transcript *t, const char *backend, const char *cwd,
                     const char *id)
{
    char path[4096];
    if (!t || !sessionload_path(backend, cwd, id, path, sizeof path))
        return 0;

    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    char   *line = NULL;
    size_t  cap = 0;
    ssize_t n;
    char   *user = NULL, *assistant = NULL;
    size_t  alen = 0, acap = 0;
    size_t  before = t->count;

    while ((n = getline(&line, &cap, f)) > 0) {
        if (n > LINE_MAX)
            continue;
        cJSON *ev = cJSON_ParseWithLength(line, (size_t)n);
        if (!ev)
            continue;

        const cJSON *content = NULL;
        enum role role = line_message(ev, &content);
        if (role == ROLE_USER) {
            char *got = user_from(content);
            if (got) {
                if (user && assistant)
                    transcript_add(t, backend, user, assistant, 0);
                free(user);
                free(assistant);
                user = got;
                assistant = NULL;
                alen = acap = 0;
            }
        } else if (role == ROLE_ASSISTANT && user) {
            content_plain(content, &assistant, &alen, &acap);
        }
        cJSON_Delete(ev);
    }
    if (user && assistant && *assistant)
        transcript_add(t, backend, user, assistant, 0);
    free(user);
    free(assistant);
    free(line);
    fclose(f);
    return (int)(t->count - before);
}
