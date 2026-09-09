#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "chrome.h"
#include "cmd.h"
#include "gitinfo.h"
#include "prompt.h"
#include "restart.h"
#include "session.h"
#include "sessionview.h"
#include "sidechannel.h"
#include "status.h"
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

void gitinfo_forget(void) {}
void tg_refocus(void) {}
void voice_refocus(void) {}
void voice_commit(struct session *s) { (void)s; }
void tg_forget_session(struct session *s) { (void)s; }
void cmd_forget_session(struct session *s) { (void)s; }
void view_collapse(int on) { (void)on; }

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

struct session {
    int running;
    int work;
    int busy;
    const char *cwd;
};

int session_turn_running(const struct session *s) { return s && s->running; }
int session_work_count(const struct session *s) { return s ? s->work : 0; }
int session_busy(const struct session *s) { return s && (s->busy || s->running); }
int session_compact(const struct session *s) { (void)s; return 0; }
const char *session_title(const struct session *s) { (void)s; return "tab"; }
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
void session_set_customizations(struct session *s, int on) { (void)s; (void)on; }
void session_set_thinking(struct session *s, int on) { (void)s; (void)on; }
void session_set_compact(struct session *s, int on) { (void)s; (void)on; }
int session_set_permission(struct session *s, const char *mode)
{
    (void)s;
    (void)mode;
    return 1;
}
void session_set_system_extra(struct session *s, const char *text) { (void)s; (void)text; }
void session_adopt_id(struct session *s, const char *id) { (void)s; (void)id; }
int session_start(struct session *s) { (void)s; return 1; }
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
int session_turn_pump(struct session *s) { return s && s->running; }
void session_turn_wait(struct session *s) { (void)s; }
int session_idle_pump(struct session *s) { (void)s; return 0; }
int session_wake_fd(const struct session *s) { (void)s; return -1; }
int session_idle_fd(const struct session *s) { (void)s; return -1; }
int session_stall_armed(const struct session *s) { (void)s; return 0; }
int session_stalled(struct session *s) { (void)s; return 0; }
double session_turn_elapsed(const struct session *s) { (void)s; return 0; }
void session_spin_word(const struct session *s)
{
    (void)s;
    status_set_word("working");
}
const char *session_failed_prompt(const struct session *s) { (void)s; return NULL; }

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

        if (workspace_count() != 2)
            fail("two tabs stay open");
        if (workspace_close(1) != 1)
            fail("close drops a tab");
        if (workspace_count() != 1 || workspace_current() != &a)
            fail("the remaining tab is the one left");
        if (workspace_close(0) != 0)
            fail("close drops the last tab");
        if (workspace_count() || workspace_current())
            fail("no tab remains");
    }

    struct session z = {0, 0, 0, "/z"}, m = {0, 0, 0, "/m"}, q = {0, 0, 0, "/a"};
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
