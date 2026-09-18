#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cmd.h"
#include "dispatch.h"
#include "orchevent.h"
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
static int            turn_running;
static int            render_n;
static int            send_n;
static int            echo_n;
static int            last_render_at = -1;
static int            last_send_at = -1;
static char           last_title[128];
static char           last_backend[32];
static char           last_model[64];
static char           last_cwd[256];
static char           last_echo[256];
static char           last_send[256];

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

const char *cmd_default_backend(void) { return "claude"; }
void        prompt_echo_message(const char *text)
{
    echo_n++;
    snprintf(last_echo, sizeof last_echo, "%s", text ? text : "");
}

static char spawned_id[64];
static int  spawned_open = 1;
static int  closed_at = -1;
static int  close_n;

struct session *workspace_current(void) { return &current_tab; }
struct session *workspace_at(int index)
{
    if (index == 0)
        return &current_tab;
    return index == spawn_at && spawned_open ? &spawned : NULL;
}
int workspace_index(void) { return 0; }
int workspace_index_of(const struct session *s)
{
    if (s == &current_tab)
        return 0;
    return s == &spawned && spawned_open ? spawn_at : -1;
}
int workspace_find_id(const char *id)
{
    if (id && *id && spawned_open && !strcmp(id, spawned_id))
        return spawn_at;
    return -1;
}
int  workspace_queued(int index) { (void)index; return 0; }
int  workspace_close(int index)
{
    close_n++;
    closed_at = index;
    if (index == spawn_at)
        spawned_open = 0;
    return 0;
}
void workspace_show(int index) { (void)index; }
void workspace_render(int index, void (*fn)(struct session *s, void *ud), void *ud)
{
    render_n++;
    last_render_at = index;
    if (fn)
        fn(workspace_at(index), ud);
}
int workspace_send(int index, const char *line, const char *shown)
{
    (void)shown;
    send_n++;
    last_send_at = index;
    snprintf(last_send, sizeof last_send, "%s", line ? line : "");
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
    if (s == &spawned && spawned_id[0])
        return spawned_id;
    return NULL;
}
const char *session_addr(const struct session *s)
{
    return s == &spawned ? "/tmp/mux-addr-fixture" : NULL;
}
int session_turn_running(const struct session *s)
{
    (void)s;
    return turn_running;
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
    turn_running = 0;
    render_n = 0;
    send_n = 0;
    echo_n = 0;
    last_render_at = last_send_at = -1;
    last_title[0] = last_backend[0] = last_model[0] = last_cwd[0] = '\0';
    last_echo[0] = last_send[0] = '\0';
    close_n = 0;
    closed_at = -1;
    memset(&spawned, 0, sizeof spawned);
}

static char *read_res(const char *dir, const char *id)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%ld-%s.res", dir, (long)getpid(), id);
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    static char buf[512];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void expect_res(const char *dir, const char *id, const char *needle, const char *what)
{
    const char *res = read_res(dir, id);
    if (!res || !strstr(res, needle)) {
        fprintf(stderr, "  reply for %s: %s\n", id, res ? res : "(none)");
        fail(what);
    }
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
    if (render_n != 1 || echo_n != 1 || strcmp(last_echo, "do the thing"))
        fail("spawn echoes the prompt");
    if (read_res(dir, "titled"))
        fail("spawn reply waits for the session id");

    /* the backend reports its id; the held reply carries it */
    snprintf(spawned_id, sizeof spawned_id, "sess-1");
    poll_once();
    expect_res(dir, "titled", "\"session\":\"sess-1\"", "spawn reply carries the session id");
    expect_res(dir, "titled", "\"addr\":\"/tmp/mux-addr-fixture\"",
               "spawn reply carries the session address file");
    if (strstr(read_res(dir, "titled"), "slot"))
        fail("spawn reply has no slot");

    reset_case();
    drop_req(dir, "plain", "{\"backend\":\"claude\",\"prompt\":\"hello\"}");
    poll_once();
    if (spawned_n != 1)
        fail("spawn without title");
    if (renamed || naming_calls)
        fail("omitted title leaves auto-titling");
    expect_res(dir, "plain", "\"session\":\"sess-1\"", "known id replies at once");

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

    reset_case();
    drop_req(dir, "idle-send", "{\"send\":\"idle follow-up\",\"session\":\"sess-1\"}");
    poll_once();
    if (send_n != 1 || last_send_at != spawn_at || strcmp(last_send, "idle follow-up"))
        fail("idle send delivers the line by id");
    if (render_n != 1 || last_render_at != spawn_at || echo_n != 1 ||
        strcmp(last_echo, "idle follow-up"))
        fail("idle send echoes once as a user turn");
    expect_res(dir, "idle-send", "\"ok\":true", "send by id replies ok");

    reset_case();
    turn_running = 1;
    drop_req(dir, "busy-send", "{\"send\":\"queued follow-up\",\"session\":\"sess-1\"}");
    poll_once();
    if (send_n != 1 || last_send_at != spawn_at || strcmp(last_send, "queued follow-up"))
        fail("busy send still delivers the line");
    if (render_n || echo_n)
        fail("busy send does not echo; send_next does when the turn starts");

    reset_case();
    drop_req(dir, "unknown-send", "{\"send\":\"lost\",\"session\":\"sess-nope\"}");
    poll_once();
    if (send_n)
        fail("an unknown id sends nothing");
    expect_res(dir, "unknown-send", "no such session", "an unknown id is an error");

    reset_case();
    drop_req(dir, "unaddressed-send", "{\"send\":\"lost\"}");
    poll_once();
    if (send_n)
        fail("a send with no id sends nothing");
    expect_res(dir, "unaddressed-send", "send takes a session id",
               "a send with no id is an error");

    reset_case();
    drop_req(dir, "slot-send", "{\"send\":\"lost\",\"slot\":1}");
    poll_once();
    if (send_n)
        fail("a slot is not an address for send");
    expect_res(dir, "slot-send", "send takes a session id", "a slot send is an error");

    reset_case();
    drop_req(dir, "slot-close", "{\"close\":1}");
    poll_once();
    if (close_n)
        fail("a slot is not an address for close");
    expect_res(dir, "slot-close", "close takes a session id", "a slot close is an error");

    reset_case();
    drop_req(dir, "unknown-close", "{\"close\":\"sess-nope\"}");
    poll_once();
    if (close_n)
        fail("an unknown id closes nothing");
    expect_res(dir, "unknown-close", "no such session", "an unknown close id is an error");

    reset_case();
    drop_req(dir, "close-id", "{\"close\":\"sess-1\"}");
    poll_once();
    if (close_n != 1 || closed_at != spawn_at)
        fail("close by id closes that session");
    expect_res(dir, "close-id", "\"session\":\"sess-1\"", "close reply names the session");

    /* the session is gone now: further requests for its id must not land */
    reset_case();
    drop_req(dir, "dead-send", "{\"send\":\"too late\",\"session\":\"sess-1\"}");
    drop_req(dir, "dead-close", "{\"close\":\"sess-1\"}");
    poll_once();
    if (send_n || close_n)
        fail("a finished session takes nothing");
    expect_res(dir, "dead-send", "no such session", "send to a finished session is an error");
    expect_res(dir, "dead-close", "no such session", "close of a finished session is an error");


    /* --- a watched spawn reports its ending ------------------------------ */

    char orchroot[] = "/tmp/mux-dispatch-orch-XXXXXX";
    if (!mkdtemp(orchroot))
        fail("orchestrator temp dir");
    setenv("ORCHESTRATOR_DIR", orchroot, 1);

    reset_case();
    spawned_open = 1;
    snprintf(spawned_id, sizeof spawned_id, "sess-idle");
    drop_req(dir, "watched",
             "{\"prompt\":\"go\",\"task\":\"t-idle\",\"notify\":\"sess-orch\"}");
    poll_once();
    struct orch_event *ev = NULL;
    if (orchevent_pending(&ev) != 0)
        fail("a worker that has not run yet has not finished");
    free(ev);

    turn_running = 1;
    poll_once();
    if (orchevent_pending(&ev) != 0)
        fail("a worker mid-turn has not finished");
    free(ev);

    turn_running = 0;
    poll_once();
    int n = orchevent_pending(&ev);
    if (n != 1)
        fail("going idle after a turn reports the completion");
    else if (strcmp(ev[0].task, "t-idle") || strcmp(ev[0].reason, "idle") ||
             strcmp(ev[0].notify, "sess-orch") || strcmp(ev[0].session, "sess-idle"))
        fail("the completion names the task, the worker and who to tell");
    free(ev);

    /* a second idle turn is the same worker, not a second completion */
    turn_running = 1;
    poll_once();
    turn_running = 0;
    poll_once();
    if (orchevent_pending(&ev) != 1)
        fail("one completion per watched spawn");
    free(ev);

    /* a worker that dies without finishing a turn is still reported */
    reset_case();
    spawned_open = 1;
    snprintf(spawned_id, sizeof spawned_id, "sess-lost");
    drop_req(dir, "lost",
             "{\"prompt\":\"go\",\"task\":\"t-lost\",\"notify\":\"sess-orch\"}");
    poll_once();
    spawned_open = 0;
    poll_once();
    n = orchevent_pending(&ev);
    if (n != 2)
        fail("a worker that ends without a turn is reported");
    else {
        int saw = 0;
        for (int i = 0; i < n; i++)
            saw |= !strcmp(ev[i].task, "t-lost") && !strcmp(ev[i].reason, "exit");
        if (!saw)
            fail("the lost worker is reported as an exit");
    }
    free(ev);

    /* a spawn that asked for nothing reports nothing */
    reset_case();
    spawned_open = 1;
    snprintf(spawned_id, sizeof spawned_id, "sess-quiet");
    drop_req(dir, "unwatched", "{\"prompt\":\"go\",\"task\":\"t-quiet\"}");
    poll_once();
    turn_running = 1;
    poll_once();
    turn_running = 0;
    spawned_open = 0;
    poll_once();
    n = orchevent_pending(&ev);
    for (int i = 0; i < n; i++)
        if (!strcmp(ev[i].task, "t-quiet"))
            fail("an unwatched spawn is not reported");
    free(ev);

    {
        char path[512];
        const char *ids[] = {"titled", "plain", "empty", "long", "idle-send", "busy-send",
                             "unknown-send", "unaddressed-send", "slot-send", "slot-close",
                             "unknown-close", "close-id", "dead-send", "dead-close",
                             "watched", "lost", "unwatched", NULL};
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
