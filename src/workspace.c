#include "workspace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "activelog.h"
#include "block.h"
#include "chrome.h"
#include "cmd.h"
#include "gitinfo.h"
#include "prompt.h"
#include "session.h"
#include "sessionview.h"
#include "settings.h"
#include "sidechannel.h"
#include "sideroute.h"
#include "status.h"
#include "stamp.h"
#include "tabbar.h"
#include "text.h"
#include "tty.h"
#include "tg.h"
#include "relay.h"
#include "im.h"
#include "ui.h"
#include "viewport.h"
#include "voice.h"

#define PENDING_MAX 8

struct pending {
    char *line;
    char *shown;
    int   typed;
};

struct tab {
    struct session        *s;
    struct viewport_state *screen;
    struct pending         pending[PENDING_MAX];
    int                    npending;
    char                  *sticky;
    char                  *draft;
    int                    draft_cursor;
    int                    finished;
};

static struct tab tabs[WORKSPACE_MAX];
static int        ntabs;
static int        cur;
static int        safe;
static void     (*on_finish)(struct session *s);
static void     (*on_turn)(struct session *s);

int workspace_spawn_remote(const char *target, char *why, size_t size)
{
    char            here[4096];
    struct session *s = ntabs < WORKSPACE_MAX
                            ? workspace_prepare("claude", NULL, NULL,
                                                getcwd(here, sizeof here) ? here : NULL, NULL)
                            : NULL;
    if (!s || !session_set_remote(s, target) || !session_start(s) ||
        !session_remote_connected(s)) {
        const char *err = s ? session_last_error(s) : NULL;
        snprintf(why, size, "%s", err && *err ? err : "could not open that session");
        session_free(s);
        return -1;
    }
    int at = workspace_open(s);
    if (at < 0) {
        snprintf(why, size, "too many tabs");
        session_free(s);
    }
    return at;
}

static void follow(const struct session *s);

void workspace_on_finish(void (*fn)(struct session *s))
{
    on_finish = fn;
}

void workspace_on_turn(void (*fn)(struct session *s))
{
    on_turn = fn;
}

static void spin_follow(void)
{
    static const struct session *spinning;

    struct session *s = ntabs ? tabs[cur].s : NULL;
    const struct session *want = s && session_turn_running(s) ? s : NULL;

    if (want == spinning) {
        if (!want) {
            if (status_spinning())
                status_end();
            return;
        }
        session_spin_word(want);
        return;
    }

    if (spinning || status_spinning())
        status_end();
    spinning = want;
    if (!want)
        return;
    status_begin_at(session_turn_elapsed(want));
    session_spin_word(want);
}

int workspace_count(void) { return ntabs; }

void workspace_republish(void)
{
    for (int i = 0; i < ntabs; i++)
        session_republish(workspace_at(i));
}
int workspace_index(void) { return cur; }

struct session *workspace_current(void)
{
    return ntabs ? tabs[cur].s : NULL;
}

struct session *workspace_at(int index)
{
    return index >= 0 && index < ntabs ? tabs[index].s : NULL;
}

int workspace_index_of(const struct session *s)
{
    for (int i = 0; i < ntabs; i++)
        if (tabs[i].s == s)
            return i;
    return -1;
}

int workspace_begin(struct session *first, int safe_mode)
{
    safe = safe_mode;
    ntabs = 0;
    cur = 0;
    if (workspace_open(first) != 0)
        return 0;
    follow(first);
    workspace_log_active();
    return 1;
}

void workspace_end(void)
{
    for (int i = 0; i < ntabs; i++) {
        viewport_state_free(tabs[i].screen);
        session_free(tabs[i].s);
        for (int j = 0; j < tabs[i].npending; j++) {
            free(tabs[i].pending[j].line);
            free(tabs[i].pending[j].shown);
        }
        free(tabs[i].sticky);
        free(tabs[i].draft);
    }
    memset(tabs, 0, sizeof tabs);
    ntabs = 0;
}

static int slot_for(const struct session *s)
{
    char mine[512];
    path_home_relative(session_cwd(s), mine, sizeof mine);

    int at = ntabs;
    for (int i = 0; i < ntabs; i++) {
        char dir[512];
        path_home_relative(session_cwd(tabs[i].s), dir, sizeof dir);
        if (strcmp(dir, mine) > 0) {
            at = i;
            break;
        }
    }
    return at;
}

int workspace_open(struct session *s)
{
    if (!s || ntabs >= WORKSPACE_MAX)
        return -1;
    struct viewport_state *screen = viewport_state_new();
    if (!screen)
        return -1;

    int at = slot_for(s);
    for (int i = ntabs; i > at; i--)
        tabs[i] = tabs[i - 1];
    ntabs++;
    if (at <= cur && ntabs > 1)
        cur++;

    memset(&tabs[at], 0, sizeof tabs[at]);
    tabs[at].s = s;
    tabs[at].screen = screen;
    if (at != cur)
        workspace_show(at);
    return at;
}

struct session *workspace_prepare(const char *backend, const char *model, const char *effort,
                                 const char *cwd, const char *id)
{
    if (!model || !strcmp(model, "default"))
        model = session_saved_model(backend);
    if (!effort || !strcmp(effort, "default"))
        effort = session_saved_effort(backend);

    struct session *s = session_new(backend, cwd, model, effort);
    if (!s)
        return NULL;

    session_set_customizations(s, !safe);
    session_set_thinking(s, settings_get_int(SETTING_THINKING, 1));
    session_set_compact(s, settings_get_int(SETTING_COMPACT, 0));
    session_set_permission(s, session_permission_name(
        settings_get_int(SETTING_PERMISSION, session_permission_default())));
    session_adopt_id(s, id);
    return s;
}

int workspace_spawn(const char *backend, const char *model, const char *effort,
                    const char *cwd, const char *id)
{
    return workspace_spawn_env(backend, model, effort, cwd, id, NULL);
}

int workspace_spawn_env(const char *backend, const char *model, const char *effort,
                        const char *cwd, const char *id, const char *const *env)
{
    if (ntabs >= WORKSPACE_MAX)
        return -1;

    struct session *s = workspace_prepare(backend, model, effort, cwd, id);
    if (!s)
        return -1;
    if ((env && !session_set_env(s, env)) || !session_start(s)) {
        session_free(s);
        return -1;
    }
    int at = workspace_open(s);
    if (at < 0)
        session_free(s);
    return at;
}

static void follow(const struct session *s)
{
    const char *dir = session_cwd(s);
    if (dir && *dir)
        (void)chdir(dir);
    status_set_note(session_title(s));
    prompt_rehome(dir);
    gitinfo_forget();
}

static void draft_save(int index)
{
    free(tabs[index].draft);
    tabs[index].draft = NULL;
    tabs[index].draft_cursor = 0;
    prompt_stash_draft(&tabs[index].draft, &tabs[index].draft_cursor);
}

void workspace_history_follow(void)
{
    static char loaded[64];
    if (!ntabs)
        return;
    const char *chain = session_chain(tabs[cur].s);
    if (!strcmp(chain, loaded))
        return;
    snprintf(loaded, sizeof loaded, "%s", chain);

    char path[4200];
    prompt_history_open(session_history_file(tabs[cur].s, path, sizeof path) ? path : NULL);
}

static void draft_load(int index)
{
    prompt_adopt_draft(tabs[index].draft, tabs[index].draft_cursor);
}

void workspace_log_active(void)
{
    char path[4400];
    if (ntabs && tty_focused() && path_config_file(path, sizeof path, "active"))
        activelog_add(path, session_chain(tabs[cur].s), 0);
}

void workspace_show(int index)
{
    if (index < 0 || index >= ntabs || index == cur)
        return;

    draft_save(cur);

    ui_flush();
    viewport_stash(tabs[cur].screen);
    viewport_adopt(tabs[index].screen);
    cur = index;

    block_forget();
    session_set_unseen(tabs[cur].s, 0);
    stamp_clear();
    status_sticky_prompt(tabs[cur].sticky);
    status_sticky_busy(session_busy(tabs[cur].s));
    draft_load(cur);
    follow(tabs[cur].s);
    spin_follow();
    viewport_forget();
    view_collapse(session_compact(tabs[cur].s));

    tg_refocus();
    relay_refocus();
    im_refocus();
    voice_refocus();
    workspace_log_active();
}

void workspace_cycle(int delta)
{
    if (ntabs <= 1 || !delta)
        return;
    int at = (cur + delta) % ntabs;
    if (at < 0)
        at += ntabs;
    workspace_show(at);
}

static void sticky_set(int index, const char *line)
{
    free(tabs[index].sticky);
    tabs[index].sticky = line ? strdup(line) : NULL;
    if (index == cur)
        status_sticky_prompt(line);
}

#define BORROW_MAX 16

struct borrow {
    int tab;
    int hold;
    struct session *drawn;
};

static struct borrow borrows[BORROW_MAX];
static int           nborrow;

static struct borrow top(void)
{
    int d = nborrow < BORROW_MAX ? nborrow : BORROW_MAX;
    struct borrow b = {cur, 0, NULL};
    return d > 0 ? borrows[d - 1] : b;
}

static void enter_held(int index, int hold)
{
    struct borrow was = top();
    int to = index >= 0 && index < ntabs ? index : was.tab;

    if (nborrow < BORROW_MAX) {
        if (to != was.tab) {
            ui_flush();
            viewport_stash(tabs[was.tab].screen);
            viewport_adopt(tabs[to].screen);
        }
        borrows[nborrow].tab = to;
        borrows[nborrow].hold = hold || was.hold || to != cur;
        viewport_hold(borrows[nborrow].hold);

        borrows[nborrow].drawn =
            session_set_drawing(to < ntabs ? tabs[to].s : NULL);
    }
    nborrow++;
}

static void enter(int index)
{
    enter_held(index, 0);
}

static void leave(void)
{
    if (nborrow <= 0)
        return;
    nborrow--;
    if (nborrow >= BORROW_MAX)
        return;

    int from = borrows[nborrow].tab;
    struct borrow back = top();
    session_set_drawing(borrows[nborrow].drawn);
    if (from != back.tab) {
        ui_flush();
        viewport_stash(tabs[from].screen);
        viewport_adopt(tabs[back.tab].screen);
    }
    viewport_hold(back.hold);
}

void workspace_render(int index, void (*fn)(struct session *s, void *ud), void *ud)
{
    if (!fn || index < 0 || index >= ntabs)
        return;
    enter(index);
    fn(tabs[index].s, ud);
    ui_flush();
    leave();
}

int workspace_find_id(const char *id)
{
    if (!id || !*id)
        return -1;
    for (int i = 0; i < ntabs; i++) {
        const char *mine = session_id(tabs[i].s);
        if (mine && !strcmp(mine, id))
            return i;
    }
    return -1;
}

int workspace_dump(int index, const char *path)
{
    if (index < 0 || index >= ntabs)
        return 0;
    enter(index);
    int ok = viewport_dump(path);
    leave();
    return ok;
}

static void drop(int index)
{
    tg_forget_session(tabs[index].s);
    relay_forget_session(tabs[index].s);
    im_forget_session(tabs[index].s);
    cmd_forget_session(tabs[index].s);

    if (index == cur)
        viewport_stash(tabs[index].screen);
    viewport_state_free(tabs[index].screen);

    session_free(tabs[index].s);
    for (int i = 0; i < tabs[index].npending; i++) {
        free(tabs[index].pending[i].line);
        free(tabs[index].pending[i].shown);
    }
    tabs[index].npending = 0;
    free(tabs[index].sticky);
    tabs[index].sticky = NULL;
    free(tabs[index].draft);
    tabs[index].draft = NULL;

    for (int i = index; i + 1 < ntabs; i++)
        tabs[i] = tabs[i + 1];
    ntabs--;
    memset(&tabs[ntabs], 0, sizeof tabs[ntabs]);

    if (!ntabs)
        return;

    if (index < cur) {
        cur--;
    } else if (index == cur) {
        cur = index > 0 ? index - 1 : 0;

        viewport_adopt(tabs[cur].screen);
        block_forget();
        status_sticky_prompt(tabs[cur].sticky);
        status_sticky_busy(session_busy(tabs[cur].s));
        draft_load(cur);
        follow(tabs[cur].s);
        spin_follow();
        viewport_forget();
        tg_refocus();
        relay_refocus();
    im_refocus();
        voice_refocus();
    }
}

int workspace_close(int index)
{
    if (index < 0 || index >= ntabs)
        return ntabs;
    drop(index);
    return ntabs;
}

int workspace_fds(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < ntabs && n < max; i++) {
        int fd = session_turn_running(tabs[i].s) ? session_wake_fd(tabs[i].s)
                                                 : session_idle_fd(tabs[i].s);
        if (fd >= 0)
            out[n++] = fd;
    }
    return n;
}

static void send_next(int index, int hold);

static const char STALL_PROMPT[] =
    "The background work you started here ended without reporting back, so no "
    "turn was run for it. Check what those tasks left behind and carry on from "
    "there.";

static void nudge_stalled(int index, int hold, int screen)
{
    struct tab *t = &tabs[index];

    if (!screen || chrome_modal_active() || t->npending || session_turn_running(t->s))
        return;
    if (!session_stalled(t->s))
        return;

    enter_held(index, hold);
    viewport_item_begin(VIEWPORT_ROWS(1, 1));
    ui_note("background tasks ended without a reply, continuing");
    viewport_item_end();
    ui_flush();
    leave();

    workspace_send(index, STALL_PROMPT, "continuing from the stalled tasks");
}

static void settle_finished(int index, int hold)
{
    if (!tabs[index].finished || chrome_modal_active())
        return;
    tabs[index].finished = 0;

    struct session *s = tabs[index].s;
    enter_held(index, hold);
    if (on_finish)
        on_finish(s);
    leave();
    send_next(index, hold);
}

static int pump(int hold, int screen)
{
    int busy = 0;

    hold = hold || chrome_modal_active();
    for (int i = 0; i < ntabs; i++) {
        struct session *s = tabs[i].s;
        int running = session_turn_running(s);

        enter_held(i, hold);
        if (running)
            busy |= session_turn_pump(s);
        else
            busy |= session_idle_pump(s) ? 1 : 0;
        leave();

        if (!running)
            nudge_stalled(i, hold, screen);

        if (running && !session_turn_running(s)) {
            tabs[i].finished = 1;
            if (i != cur)
                session_set_unseen(s, 1);
            else if (screen && !tty_focused())
                stamp_show();
        }
        settle_finished(i, hold);
    }

    if (screen) {
        if (ntabs) {
            status_set_note(session_title(tabs[cur].s));
            status_sticky_busy(session_busy(tabs[cur].s));
        }
        spin_follow();
        if (tabbar_stale())
            viewport_touch();
    }
    return busy;
}

int workspace_pump(void)
{
    return pump(0, 1);
}

int workspace_pump_quiet(void)
{
    int busy = pump(1, 1);

    chrome_paint();
    return busy;
}

int workspace_drain(void)
{
    return pump(1, 0);
}

void workspace_settle(struct session *s)
{
    int at = workspace_index_of(s);
    if (at < 0 || !session_turn_running(s))
        return;

    enter(at);
    session_turn_wait(s);
    tabs[at].finished = 0;
    if (on_finish)
        on_finish(s);
    leave();
    send_next(at, 0);
    spin_follow();
}

void workspace_wait_turn(int index)
{
    if (index < 0 || index >= ntabs)
        return;
    struct session *s = tabs[index].s;
    if (!session_turn_running(s))
        return;

    enter(index);
    session_turn_wait(s);
    tabs[index].finished = 0;
    if (on_finish)
        on_finish(s);
    leave();
}

int workspace_busy(void)
{
    for (int i = 0; i < ntabs; i++)
        if (session_busy(tabs[i].s))
            return 1;
    return 0;
}

static void send_next(int index, int hold)
{
    struct tab *t = &tabs[index];
    if (!t->npending || session_turn_running(t->s))
        return;

    struct pending p = t->pending[0];
    for (int i = 1; i < t->npending; i++)
        t->pending[i - 1] = t->pending[i];
    t->npending--;

    enter_held(index, hold);
    sticky_set(index, p.shown ? p.shown : p.line);

    prompt_echo_message(p.shown ? p.shown : p.line);
    if (on_turn)
        on_turn(t->s);
    session_turn_begin(t->s, p.line);
    leave();
    free(p.line);
    free(p.shown);
    spin_follow();
}

int workspace_send(int index, const char *line, const char *shown)
{
    if (index < 0 || index >= ntabs || !line || !*line)
        return 0;
    struct tab *t = &tabs[index];

    if (session_turn_running(t->s)) {
        if (t->npending >= PENDING_MAX)
            return 0;
        struct pending p = {strdup(line), shown ? strdup(shown) : NULL, 0};
        if (!p.line || (shown && !p.shown)) {
            free(p.line);
            free(p.shown);
            return 0;
        }
        t->pending[t->npending++] = p;
        return 1;
    }

    sticky_set(index, shown ? shown : line);
    enter(index);
    if (on_turn)
        on_turn(t->s);
    int ok = session_turn_begin(t->s, line);
    leave();
    spin_follow();
    return ok;
}

static int join(char **dst, const char *text)
{
    size_t n = strlen(*dst) + strlen(text) + 3;
    char  *j = malloc(n);
    if (!j)
        return 0;
    snprintf(j, n, "%s\n\n%s", *dst, text);
    free(*dst);
    *dst = j;
    return 1;
}

int workspace_send_typed(int index, const char *text, const char *full)
{
    if (index < 0 || index >= ntabs || !text || !*text)
        return 0;
    struct tab *t = &tabs[index];
    if (!session_turn_running(t->s))
        return workspace_send(index, full ? full : text, full ? text : NULL);

    if (!session_remote(t->s) && sideroute_independent(session_prompt(t->s), text)) {
        char label[4096];
        snprintf(label, sizeof label, "/btw %s", text);
        if (sidechannel_start(t->s, text, label))
            return 1;
    }

    struct pending *last = t->npending ? &t->pending[t->npending - 1] : NULL;
    if (last && last->typed)
        return join(&last->line, text) && (!last->shown || join(&last->shown, text));

    if (!workspace_send(index, full ? full : text, full ? text : NULL))
        return 0;
    t->pending[t->npending - 1].typed = 1;
    return 1;
}

int workspace_queued(int index)
{
    return index >= 0 && index < ntabs ? tabs[index].npending : 0;
}

const char *workspace_pending_at(int index, int i)
{
    if (index < 0 || index >= ntabs || i < 0 || i >= tabs[index].npending)
        return NULL;
    const struct pending *p = &tabs[index].pending[i];
    return p->shown ? p->shown : p->line;
}

int workspace_dequeue(int index, const char *line)
{
    if (index < 0 || index >= ntabs || !line)
        return 0;
    struct tab *t = &tabs[index];
    for (int i = 0; i < t->npending; i++) {
        if (strcmp(t->pending[i].line, line))
            continue;
        free(t->pending[i].line);
        free(t->pending[i].shown);
        for (int j = i + 1; j < t->npending; j++)
            t->pending[j - 1] = t->pending[j];
        t->npending--;
        return 1;
    }
    return 0;
}

char *workspace_unqueue(int index)
{
    if (index < 0 || index >= ntabs || !tabs[index].npending)
        return NULL;
    struct pending *p = &tabs[index].pending[--tabs[index].npending];

    char *back = p->shown ? p->shown : p->line;
    if (p->shown)
        free(p->line);
    p->line = p->shown = NULL;
    return back;
}

const char *workspace_status(const struct session *s)
{
    if (!s)
        return "finished";
    if (session_busy(s))
        return "working";
    if (session_failed_prompt(s))
        return "errored";
    return "finished";
}
