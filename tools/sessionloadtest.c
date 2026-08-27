#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "md.h"
#include "prompt.h"
#include "session.h"
#include "sessionload.h"
#include "sessionlist.h"
#include "sessionview.h"
#include "toolstyle.h"
#include "ui.h"
#include "viewport.h"

static int failures;
static int users;
static int assistants;
static int tools;

static void expect(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

int sessionlist_available(const char *backend)
{
    (void)backend;
    return 0;
}

int sessionlist_dir(const char *backend, const char *cwd, char *out, size_t size)
{
    (void)backend;
    (void)cwd;
    (void)out;
    (void)size;
    return 0;
}

const char *session_id(const struct session *s) { (void)s; return NULL; }
const char *session_backend(const struct session *s) { (void)s; return NULL; }
const char *session_cwd(const struct session *s) { (void)s; return NULL; }
int session_thinking(const struct session *s) { (void)s; return 0; }

void prompt_echo_message(const char *text)
{
    if (text && !strcmp(text, "do the work"))
        users++;
}

void md_render_kept(const char *text, int own)
{
    (void)own;
    if (text && !strcmp(text, "work is done"))
        assistants++;
}

void view_tool_argument(const backend_event *ev, const char *cwd, char *out,
                        size_t size)
{
    (void)cwd;
    snprintf(out, size, "%s", ev->arg ? ev->arg : "");
}

void view_keep_tool_call(const char *name, const char *arg, int collapses)
{
    (void)arg;
    (void)collapses;
    if (name && !strcmp(name, "exec"))
        tools++;
}

void view_keep_activity(const char *marker, const char *text, enum ui_role role)
{
    (void)marker;
    (void)text;
    (void)role;
}

int toolstyle_collapses(const char *name, const char *input_json, const char *arg)
{
    (void)name;
    (void)input_json;
    (void)arg;
    return 0;
}

const char *ui_style(enum ui_role role)
{
    (void)role;
    return "";
}
void ui_bar(const char *style, const char *fmt, ...)
{
    (void)style;
    (void)fmt;
}
void ui_flush(void) {}
unsigned viewport_item_begin(const struct viewport_entry *entry)
{
    (void)entry;
    return 0;
}
void viewport_item_end(void) {}

int main(void)
{
    char root[] = "/tmp/mux-sessionload-XXXXXX";
    expect(mkdtemp(root) != NULL, "temporary directory");
    setenv("CODEX_HOME", root, 1);

    char sessions[1024], year[1024], month[1024], day[1024], path[1200];
    snprintf(sessions, sizeof sessions, "%s/sessions", root);
    snprintf(year, sizeof year, "%s/2026", sessions);
    snprintf(month, sizeof month, "%s/08", year);
    snprintf(day, sizeof day, "%s/27", month);
    mkdir(sessions, 0700);
    mkdir(year, 0700);
    mkdir(month, 0700);
    mkdir(day, 0700);

    const char *id = "01a0456d-28c5-77c3-9ea8-c813360da444";
    snprintf(path, sizeof path, "%s/rollout-2026-08-27T15-52-56-%s.jsonl", day,
             id);
    FILE *f = fopen(path, "w");
    expect(f != NULL, "rollout fixture");
    if (f) {
        fputs("{\"type\":\"response_item\",\"payload\":{\"type\":\"message\","
              "\"role\":\"user\",\"content\":[{\"type\":\"input_text\","
              "\"text\":\"do the work\"}]}}\n", f);
        fputs("{\"type\":\"response_item\",\"payload\":{\"type\":\"message\","
              "\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\","
              "\"text\":\"work is done\"}]}}\n", f);
        fputs("{\"type\":\"response_item\",\"payload\":{"
              "\"type\":\"custom_tool_call\",\"name\":\"exec\","
              "\"input\":\"run it\"}}\n", f);
        fclose(f);
    }

    char found[4096];
    expect(sessionload_path("codex", "/worktree", id, found, sizeof found) &&
               !strcmp(found, path),
           "codex rollout path");
    expect(sessionload_replay("codex", "/worktree", id, 0) == 3,
           "codex replay count");
    expect(users == 1, "codex user message");
    expect(assistants == 1, "codex assistant message");
    expect(tools == 1, "codex tool call");

    unlink(path);
    rmdir(day);
    rmdir(month);
    rmdir(year);
    rmdir(sessions);
    rmdir(root);

    if (!failures)
        printf("sessionloadtest: all checks passed\n");
    return failures != 0;
}
