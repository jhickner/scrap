#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "activelog.h"
#include "chrome.h"
#include "im.h"
#include "cmd.h"
#include "gitinfo.h"
#include "prompt.h"
#include "restart.h"
#include "session.h"
#include "sessionview.h"
#include "stamp.h"
#include "intercom.h"
#include "sidechannel.h"
#include "sideroute.h"
#include "status.h"
#include "relay.h"
#include "tg.h"
#include "ui.h"
#include "viewport.h"
#include "voice.h"
#include "workspace.h"

void restart_shield_thread(void) {}

int  sidechannel_rows(void) { return 0; }
void sidechannel_paint(int budget) { (void)budget; }
void sidechannel_tick(void) {}
void sidechannel_poll(void) {}
int  sidechannel_busy(void) { return 0; }
void sidechannel_close_all(void) {}
int  sidechannel_fds(int *out, int max) { (void)out; (void)max; return 0; }
enum sideroute sideroute_classify(const char *running, const char *queued) { (void)running; (void)queued; return SIDEROUTE_QUEUE; }

void gitinfo_forget(void) {}
void tg_refocus(void) {}
void voice_refocus(void) {}
void tg_forget_session(struct session *s) { (void)s; }
void relay_forget_session(struct session *s) { (void)s; }
void im_refocus(void) {}
void im_forget_session(struct session *s) { (void)s; }
void cmd_forget_session(struct session *s) { (void)s; }
int cmd_is_command(const char *line) { (void)line; return 0; }
enum cmd_result cmd_submit(struct session *s, const char *line) { (void)s; (void)line; return CMD_NOT_A_COMMAND; }
void view_collapse(int on) { (void)on; }

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static int dump_count(const char *path, const char *needle)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char buf[4096];
    int n = 0;
    while (fgets(buf, sizeof buf, f))
        if (strstr(buf, needle))
            n++;
    fclose(f);
    return n;
}

struct session {
    int running;
    int work;
    int busy;
    const char *cwd;
    int finish;
};

static int quota_blocked;
int session_autobackend(struct session *s) { (void)s; return quota_blocked ? -1 : 0; }
int session_turn_running(const struct session *s) { return s && s->running; }
const char *session_prompt(const struct session *s) { (void)s; return NULL; }
const char *session_remote(const struct session *s) { (void)s; return NULL; }
void session_interrupt(struct session *s) { (void)s; }
int sidechannel_start(const struct session *s, const char *prompt, const char *label) { (void)s; (void)prompt; (void)label; return 0; }
int sidechannel_inbox(const struct session *s, const char *message, sidechannel_done done, void *ud) { (void)s; (void)message; (void)done; (void)ud; return 0; }
void sidechannel_cancel(sidechannel_done done, void *ud) { (void)done; (void)ud; }
void sidechannel_show(const struct session *owner, const char *question, const char *answer, int failed) { (void)owner; (void)question; (void)answer; (void)failed; }
int session_can_resume(const struct session *s) { (void)s; return 0; }
void intercom_reply(const char *from, const char *to, const char *text) { (void)from; (void)to; (void)text; }
int session_work_count(const struct session *s) { return s ? s->work : 0; }
int session_busy(const struct session *s) { return s && (s->busy || s->running); }
int session_compact(const struct session *s) { (void)s; return 0; }
const char *session_title(const struct session *s) { (void)s; return "tab"; }
const char *session_name(const struct session *s) { (void)s; return NULL; }
const char *session_chain(const struct session *s) { (void)s; return ""; }
int session_history_file(const struct session *s, char *out, size_t size) { (void)s; (void)out; (void)size; return 0; }
void stamp_show(void) {}
void stamp_clear(void) {}
void session_republish(const struct session *s) { (void)s; }
void activelog_add(const char *path, const char *id, int back) { (void)path; (void)id; (void)back; }
const char *session_cwd(const struct session *s) { return s && s->cwd ? s->cwd : "."; }
const char *session_id(const struct session *s) { (void)s; return NULL; }
const char *session_backend(const struct session *s) { (void)s; return "grok"; }
const char *session_model(const struct session *s) { (void)s; return "default"; }
const char *session_effort(const struct session *s) { (void)s; return "default"; }
const char *session_saved_model(const char *backend) { (void)backend; return NULL; }
const char *session_saved_effort(const char *backend) { (void)backend; return NULL; }
const char *session_permission_name(int index) { (void)index; return "bypass"; }
int session_permission_default(void) { return 0; }
int session_unseen(const struct session *s) { (void)s; return 0; }
void session_set_unseen(struct session *s, int on) { (void)s; (void)on; }
struct agent_job *session_agent_take(void) { return NULL; }
struct session *session_agent_open(struct agent_job *j) { (void)j; return NULL; }
const char *session_agent_task(const struct session *s) { (void)s; return NULL; }
void session_agent_fail(struct session *s, const char *why) { (void)s; (void)why; }
void session_agent_poll(struct session *s) { (void)s; }
void session_agent_started(struct session *s) { (void)s; }
double session_agent_idle(const struct session *s) { (void)s; return -1; }
void session_set_customizations(struct session *s, int on) { (void)s; (void)on; }
void session_set_thinking(struct session *s, int on) { (void)s; (void)on; }
void session_set_compact(struct session *s, int on) { (void)s; (void)on; }
int session_set_permission(struct session *s, const char *mode)
{
    (void)s;
    (void)mode;
    return 1;
}
void session_adopt_id(struct session *s, const char *id) { (void)s; (void)id; }
int session_start(struct session *s) { (void)s; return 1; }
int session_set_env(struct session *s, const char *const *env) { (void)s; (void)env; return 1; }
void session_free(struct session *s) { (void)s; }
struct session *session_new(const char *backend, const char *cwd, const char *model,
                            const char *effort)
{
    (void)backend;
    (void)cwd;
    (void)model;
    (void)effort;
    return NULL;
}
struct session *session_set_drawing(struct session *s)
{
    static struct session *live;
    struct session *was = live;
    live = s;
    return was;
}
int session_turn_begin(struct session *s, const char *text)
{
    (void)text;
    if (!s)
        return 0;
    s->running = 1;
    return 1;
}
int session_turn_pump(struct session *s)
{
    if (!s || !s->running)
        return 0;
    if (s->finish)
        s->running = 0;
    return 1;
}
void session_turn_wait(struct session *s) { (void)s; }
void session_replay(struct session *s) { (void)s; }
int session_permission_waiting(const struct session *s) { (void)s; return 0; }
int session_idle_pump(struct session *s) { (void)s; return 0; }
int session_wake_fd(const struct session *s) { (void)s; return -1; }
int session_idle_fd(const struct session *s) { (void)s; return -1; }
int session_stalled(struct session *s) { (void)s; return 0; }
const char *session_last_error(const struct session *s) { (void)s; return NULL; }
int session_set_remote(struct session *s, const char *target) { (void)s; (void)target; return 0; }
int session_remote_connected(const struct session *s) { (void)s; return 0; }
double session_turn_elapsed(const struct session *s) { (void)s; return 0; }
void session_spin_word(const struct session *s)
{
    (void)s;
    status_set_word("working");
}
const char *session_failed_prompt(const struct session *s) { (void)s; return NULL; }

static const char *draft_line(struct prompt *p)
{
    (void)p;
    static char buf[1024];
    char *text;
    prompt_stash_draft(&text, NULL);
    snprintf(buf, sizeof buf, "%s", text ? text : "");
    free(text);
    return buf;
}

static int draft_cursor(struct prompt *p)
{
    (void)p;
    char *text;
    int   cursor;
    prompt_stash_draft(&text, &cursor);
    free(text);
    return cursor;
}

int main(void)
{
    setenv("COLUMNS", "80", 1);
    setenv("LINES", "24", 1);

    ui_init();
    viewport_begin();

    struct prompt *prompt = prompt_new(NULL, 0);
    chrome_bind(prompt);

    int saved = dup(STDOUT_FILENO);
    int null = open("/dev/null", O_WRONLY);
    if (saved >= 0 && null >= 0)
        dup2(null, STDOUT_FILENO);
    if (null >= 0)
        close(null);

    struct session a = {0}, b = {0};
    if (!workspace_begin(&a, 0))
        fail("open the first tab");
    else if (workspace_open(&b) < 0)
        fail("open a second tab");
    else {
        workspace_show(0);
        if (status_spinning())
            fail("idle tabs do not start a spinner");

        status_begin();
        if (!status_spinning())
            fail("a blocking turn starts a spinner");

        workspace_show(1);
        if (status_spinning())
            fail("an idle tab does not keep another tab's spinner");

        a.running = 1;
        workspace_show(0);
        if (!status_spinning())
            fail("the working tab shows its spinner");

        workspace_show(1);
        if (status_spinning())
            fail("switching back to the idle tab ends the spinner");

        workspace_pump();
        if (status_spinning())
            fail("a working tab behind this one does not keep the spinner");

        b.work = 1;
        workspace_show(1);
        workspace_pump();
        if (status_spinning())
            fail("background work does not take the input spinner");
        b.work = 0;
        a.running = 0;

        workspace_show(0);
        prompt_adopt_draft("alpha", 5);
        workspace_show(1);
        {
            const char *got = draft_line(prompt);
            if (got && *got)
                fail("the other tab does not show this tab's unsent input");
        }
        prompt_adopt_draft("beta", 4);
        workspace_show(0);
        {
            const char *got = draft_line(prompt);
            if (!got || strcmp(got, "alpha"))
                fail("unsent input comes back with its tab");
            if (draft_cursor(prompt) != 5)
                fail("the caret comes back where it was");
        }
        workspace_show(1);
        {
            const char *got = draft_line(prompt);
            if (!got || strcmp(got, "beta"))
                fail("each tab keeps its own unsent input");
        }
        workspace_show(0);
        prompt_adopt_draft("alpha\nmore", 5);
        workspace_show(1);
        {
            const char *got = draft_line(prompt);
            if (!got || strcmp(got, "beta"))
                fail("a multi-line draft stays on its tab");
        }
        workspace_show(0);
        {
            const char *got = draft_line(prompt);
            if (!got || strcmp(got, "alpha\nmore"))
                fail("a multi-line draft comes back");
            if (draft_cursor(prompt) != 5)
                fail("a multi-line caret comes back");
        }
        if (a.running)
            fail("restoring unsent input does not submit it");

        if (workspace_count() != 2)
            fail("two tabs stay open");
        workspace_show(1);
        if (workspace_close(1) != 1)
            fail("close drops a tab");
        if (workspace_count() != 1 || workspace_current() != &a)
            fail("the remaining tab is the one left");
        {
            const char *got = draft_line(prompt);
            if (!got || strcmp(got, "alpha\nmore"))
                fail("closing a tab restores the remaining draft");
        }
        if (workspace_close(0) != 0)
            fail("close drops the last tab");
        if (workspace_count() || workspace_current())
            fail("no tab remains");
    }

    struct session z = {0, 0, 0, "/z", 0}, m = {0, 0, 0, "/m", 0}, q = {0, 0, 0, "/a", 0};
    if (!workspace_begin(&z, 0))
        fail("open the first tab of the sorted set");
    else if (workspace_open(&m) != 0 || workspace_open(&q) != 0)
        fail("a tab lands in its directory's place");
    else {
        if (workspace_at(0) != &q || workspace_at(1) != &m || workspace_at(2) != &z)
            fail("tabs read in directory order");
        if (workspace_current() != &q)
            fail("the new tab is the current one");
        workspace_show(workspace_index_of(&z));
        if (workspace_open(&b) != 0 || workspace_current() != &b)
            fail("opening ahead of the current tab keeps its place");
        if (workspace_index_of(&z) != 3)
            fail("the tab behind the new one moves along");
    }
    while (workspace_count())
        workspace_close(0);

    {
        struct session idle = {0}, busy = {0};
        char dump[] = "/tmp/scrap-ws-echo-XXXXXX";
        int dfd = mkstemp(dump);
        if (dfd < 0)
            fail("temp dump");
        else
            close(dfd);

        if (!workspace_begin(&idle, 0) || workspace_open(&busy) < 0)
            fail("open tabs for send echo");
        else {
            if (!workspace_send(0, "idle-dispatch-line", NULL))
                fail("idle send");
            if (!workspace_dump(0, dump) || dump_count(dump, "idle-dispatch-line"))
                fail("idle workspace_send does not echo; the caller does");

            busy.running = 1;
            if (!workspace_send(1, "queued-dispatch-line", NULL))
                fail("queue a line while a turn is running");
            if (workspace_queued(1) != 1)
                fail("the line is pending");
            if (!workspace_dump(1, dump) || dump_count(dump, "queued-dispatch-line"))
                fail("a queued line is not echoed until its turn starts");

            quota_blocked = 1;
            busy.finish = 1;
            workspace_pump();
            if (busy.running || workspace_queued(1) != 1 ||
                strcmp(workspace_pending_at(1, 0), "queued-dispatch-line"))
                fail("a quota stop preserves the queued request");
            quota_blocked = 0;
            busy.running = 1;
            busy.finish = 0;

            if (!workspace_send(1, "second-line", NULL) || !workspace_send(1, "third-line", NULL))
                fail("queue two more lines");
            if (workspace_dequeue(1, "missing-line") || !workspace_dequeue(1, "second-line"))
                fail("dequeue removes only a matching line");
            if (workspace_queued(1) != 2 || strcmp(workspace_pending_at(1, 1), "third-line"))
                fail("dequeue keeps the order of the rest");
            workspace_dequeue(1, "third-line");

            busy.finish = 1;
            workspace_pump();
            if (workspace_queued(1))
                fail("the queued line starts when the turn ends");
            if (!workspace_dump(1, dump) || dump_count(dump, "queued-dispatch-line") != 1)
                fail("a queued line is echoed once when its turn starts");
        }
        unlink(dump);
        while (workspace_count())
            workspace_close(0);
    }

    workspace_end();
    chrome_bind(NULL);
    prompt_free(prompt);
    viewport_end();

    if (saved >= 0) {
        fflush(stdout);
        dup2(saved, STDOUT_FILENO);
        close(saved);
    }

    if (failures)
        return 1;
    puts("workspacetest: ok");
    return 0;
}
