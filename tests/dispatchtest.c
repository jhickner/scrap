#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cmd.h"
#include "dispatch.h"
#include "prompt.h"
#include "session.h"
#include "workspace.h"

struct session {
    char title[128];
    int  skip_naming;
};

static struct session spawned;
static struct session current_tab;
static int            spawn_at = 1;
static int            spawned_n;
static int            renamed;
static int            naming_calls;
static char           last_title[128];
static char           last_backend[32];
static char           last_model[64];
static char           last_cwd[256];

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

const char *cmd_default_backend(void) { return "claude"; }
void        prompt_echo_message(const char *text) { (void)text; }

struct session *workspace_current(void) { return &current_tab; }
struct session *workspace_at(int index)
{
    return index == spawn_at ? &spawned : NULL;
}
int workspace_index(void) { return 0; }
int workspace_index_of(const struct session *s)
{
    return s == &current_tab ? 0 : spawn_at;
}
int  workspace_find_id(const char *id) { (void)id; return -1; }
int  workspace_queued(int index) { (void)index; return 0; }
int  workspace_close(int index) { (void)index; return 0; }
void workspace_show(int index) { (void)index; }
void workspace_render(int index, void (*fn)(struct session *s, void *ud), void *ud)
{
    (void)index;
    (void)fn;
    (void)ud;
}
int workspace_send(int index, const char *line, const char *shown)
{
    (void)index;
    (void)line;
    (void)shown;
    return 1;
}

int workspace_spawn(const char *backend, const char *model, const char *effort,
                    const char *cwd, const char *id)
{
    (void)effort;
    (void)id;
    spawned_n++;
    memset(&spawned, 0, sizeof spawned);
    snprintf(last_backend, sizeof last_backend, "%s", backend ? backend : "");
    snprintf(last_model, sizeof last_model, "%s", model ? model : "");
    snprintf(last_cwd, sizeof last_cwd, "%s", cwd ? cwd : "");
    return spawn_at;
}

const char *session_id(const struct session *s)
{
    (void)s;
    return NULL;
}
int session_turn_running(const struct session *s)
{
    (void)s;
    return 0;
}

enum session_rename session_rename(struct session *s, const char *name)
{
    renamed++;
    snprintf(last_title, sizeof last_title, "%s", name ? name : "");
    if (s && name)
        snprintf(s->title, sizeof s->title, "%s", name);
    return SESSION_RENAME_OK;
}

void session_set_naming(struct session *s, int on)
{
    naming_calls++;
    if (s)
        s->skip_naming = !on;
}

static void reset_case(void)
{
    spawned_n = 0;
    renamed = 0;
    naming_calls = 0;
    last_title[0] = last_backend[0] = last_model[0] = last_cwd[0] = '\0';
    memset(&spawned, 0, sizeof spawned);
}

static void drop_req(const char *dir, const char *id, const char *json)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%ld-%s.req", dir, (long)getpid(), id);
    FILE *f = fopen(path, "w");
    if (!f) {
        fail("write request");
        return;
    }
    fputs(json, f);
    fclose(f);
}

static void poll_once(void)
{
    struct timespec ts = {0, 300 * 1000 * 1000L};
    nanosleep(&ts, NULL);
    dispatch_poll();
}

int main(void)
{
    char dir[] = "/tmp/mux-dispatch-XXXXXX";
    if (!mkdtemp(dir)) {
        perror("dispatchtest: mkdtemp");
        return 1;
    }
    setenv("MUX_DISPATCH_DIR", dir, 1);
    unsetenv("TMUX_PANE");

    drop_req(dir, "titled",
             "{\"backend\":\"grok\",\"model\":\"grok-4.6\",\"cwd\":\"/work\","
             "\"prompt\":\"do the thing\",\"title\":\"name dispatched worker tabs\"}");
    poll_once();
    if (spawned_n != 1)
        fail("spawn with title");
    if (strcmp(last_backend, "grok") || strcmp(last_model, "grok-4.6") ||
        strcmp(last_cwd, "/work"))
        fail("spawn fields");
    if (renamed != 1 || strcmp(last_title, "name dispatched worker tabs"))
        fail("title is the session name");
    if (naming_calls != 1 || !spawned.skip_naming)
        fail("dispatched title is not auto-replaced");

    reset_case();
    drop_req(dir, "plain", "{\"backend\":\"claude\",\"prompt\":\"hello\"}");
    poll_once();
    if (spawned_n != 1)
        fail("spawn without title");
    if (renamed || naming_calls)
        fail("omitted title leaves auto-titling");

    reset_case();
    drop_req(dir, "empty", "{\"title\":\"\",\"prompt\":\"hello\"}");
    poll_once();
    if (spawned_n != 1)
        fail("spawn with empty title");
    if (renamed || naming_calls)
        fail("empty title is ignored");

    reset_case();
    {
        char json[256];
        char long_title[100];
        memset(long_title, 'x', 90);
        long_title[90] = '\0';
        snprintf(json, sizeof json, "{\"title\":\"%s\"}", long_title);
        drop_req(dir, "long", json);
        poll_once();
        if (renamed != 1 || strlen(last_title) != 80)
            fail("a long title is clipped to 80");
    }

    {
        char path[512];
        const char *ids[] = {"titled", "plain", "empty", "long", NULL};
        for (int i = 0; ids[i]; i++) {
            snprintf(path, sizeof path, "%s/%ld-%s.res", dir, (long)getpid(), ids[i]);
            unlink(path);
        }
    }
    rmdir(dir);
    if (failures) {
        fprintf(stderr, "dispatchtest: %d failed\n", failures);
        return 1;
    }
    printf("dispatchtest: ok\n");
    return 0;
}
