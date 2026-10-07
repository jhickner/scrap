#include "session.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "agenttabs.h"
#include "apicore.h"
#include "block.h"
#include "chain.h"
#include "app.h"
#include "gitinfo.h"
#include "grokbottail.h"
#include "hub.h"
#include "hud.h"
#include "image.h"
#include "intercom.h"
#include "livelist.h"
#include "remote.h"
#include "restart.h"
#include "models.h"
#include "parent.h"
#include "sessionaddr.h"
#include "sessionlist.h"
#include "sessionload.h"
#include "sessionprefs.h"
#include "sessionpresent.h"
#include "sessionview.h"
#include "viewport.h"
#include "settings.h"
#include "sidechannel.h"
#include "stream.h"
#include "status.h"
#include "tasks.h"
#include "title.h"
#include "tg.h"
#include "relay.h"
#include "im.h"
#include "transcript.h"
#include "tty.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "quota.h"
#include "text.h"
#include "vendor/cJSON.h"
#include "vncinset.h"
#include "voice.h"

struct session {
    Backend *agent;
    char    *remote;
    char    *backend;
    char    *cwd;
    char    *workdir;
    char    *model;
    char    *effort;
    char    *resolved;
    char     id[128];
    char     name[INTERCOM_NAME_MAX];
    char     chain[CHAIN_ID_MAX];
    char     fork_from[128];
    char    *addr;
    char   **env;
    backend_result last_result;
    int      quota_failed;
    int      autobackend_due;
    char     title[128];
    char     held_title[128];
    double   title_polled;
    char    *prompt;
    char    *last_reply;
    char    *failed_prompt;
    struct transcript transcript;
    int      transcript_loaded;
    char    *last_block;
    int      turns;
    int      saved;
    double   cost_usd;
    long     tokens_in, tokens_out;

    long     tokens_cached;
    struct sessionpresent_tokens *ledger;
    int      ledger_n, ledger_cap;
    long     context_tokens;
    long     context_window;
    int      autohandoff;
    int      autohandoffs;
    backend_rate_limit noted;
    int      quiet;
    char    *system_extra;
    char    *handoff;
    session_event_fn observer;
    void    *observer_ud;
    int    (*abort_hook)(void *ud);
    void    *abort_ud;
    int      thinking;
    int      compact;
    int      resetting;
    int      customizations;
    int      no_browser_login;
    int      fork_session;
    int      memory;
    int      fork_named;
    int      forked;
    char    *permission;
    char    *error_note;
    int      idle_busy;
    int      idle_pumping;
    int      trust_requested;
    struct tasktab     tasks;
    const struct task *task_change;
    int      task_repeat;
    unsigned long spoke;
    unsigned long heard;
    double   work_at;
    double   stall_at;
    int      stall_seen;
    int      stall_told;
    volatile double heard_at;
    volatile int    tool_open;
    int      interrupted;
    int      ask_open;
    int      unseen;
    char     tail_bot[128];
    struct grokbottail_mark tail_mark;
    struct vncinset *inset;

    pthread_t       thread;
    int             running;
    volatile int    connecting;
    pthread_t       start_th;
    int             start_threaded;
    volatile int    start_finished;
    int             start_ok;
    volatile int    finished;
    volatile int    abort_request;
    struct agent_job *job;
    int             subagent;
    double          idle_at;
    struct permission perm;
    volatile int    perm_state;
    int             perm_allow;
    pthread_mutex_t lock;
    struct evcopy  *head, *tail;
    int             wake[2];
    char           *asked;
    char           *reply;
    int             continuing;
    backend_result  meta;
    double          started;
    double          status_at;
    char           *status_last;
    int             status_open;
    char           *turn_log;
    size_t          turn_len;
    unsigned long   status_heard;

    struct sessionpresent present;
};

struct evcopy {
    backend_event  ev;
    char          *text, *name, *input_json, *arg, *diff, *id, *parent, *task_type;
    double         queued_at;
    struct evcopy *next;
};

static double rendering_queued_at;

double session_event_queued_at(void)
{
    return rendering_queued_at;
}

static void replace(char **slot, const char *value)
{
    free(*slot);
    *slot = value ? strdup(value) : NULL;
}

static int same_string(const char *a, const char *b)
{
    return a == b || (a && b && !strcmp(a, b));
}

static struct session *live;

struct session *session_set_drawing(struct session *s)
{
    struct session *was = live;
    live = s;
    return was;
}

#define LISTENERS_MAX 4
static struct { session_listener_fn fn; void *ud; } listeners[LISTENERS_MAX];
static void remember_model(const struct session *s);
static void charge_turn(struct session *s, const backend_result *m);
static void ledger_add(struct session *s, const backend_result *m, const char *prompt,
                       double cost);
static void retire(Backend *b);
static int dir_alive(const char *path);
static int ground_target(const char *gone, char *out, size_t size);
static int session_retarget(struct session *s, const char *model, const char *effort,
                            const char *cwd);

#define TURN_LOG_MAX  (24 * 1024)
#define TURN_LOG_CLIP 400
#define SIDE_VIEW_MAX (16 * 1024)

static void turn_log_add(struct session *s, const char *label, const char *text)
{
    if (!text || !*text)
        return;
    size_t n = strlen(text);
    int clipped = n > TURN_LOG_CLIP;
    if (clipped)
        n = TURN_LOG_CLIP;
    size_t want = s->turn_len + strlen(label) + n + 8;
    char *grown = realloc(s->turn_log, want);
    if (!grown)
        return;
    s->turn_log = grown;
    s->turn_len += (size_t)snprintf(s->turn_log + s->turn_len, want - s->turn_len,
                                    "%s%.*s%s\n", label, (int)n, text, clipped ? "..." : "");
    if (s->turn_len <= TURN_LOG_MAX)
        return;
    char *cut = memchr(s->turn_log + s->turn_len - TURN_LOG_MAX, '\n', TURN_LOG_MAX);
    size_t drop = cut ? (size_t)(cut + 1 - s->turn_log) : s->turn_len - TURN_LOG_MAX;
    memmove(s->turn_log, s->turn_log + drop, s->turn_len - drop + 1);
    s->turn_len -= drop;
}

static void turn_log_event(struct session *s, const backend_event *ev)
{
    if (ev->parent && *ev->parent)
        return;
    if (ev->kind == BACKEND_EV_ASSISTANT) {
        turn_log_add(s, "agent: ", ev->text);
    } else if (ev->kind == BACKEND_EV_TOOL) {
        char arg[512] = "";
        view_tool_argument(ev, s->cwd, arg, sizeof arg);
        char line[768];
        snprintf(line, sizeof line, "%s %s", ev->name ? ev->name : "tool", arg);
        turn_log_add(s, "tool call: ", line);
    } else if (ev->kind == BACKEND_EV_TOOL_RESULT) {
        turn_log_add(s, ev->failed ? "tool failed: " : "tool result: ",
                     ev->text && *ev->text ? ev->text : "(no output)");
    }
}

static void render_event(struct session *s, const backend_event *ev)
{
    s->heard++;
    turn_log_event(s, ev);
    if (ev->kind != BACKEND_EV_TASK)
        s->spoke++;

    if (ev->kind == BACKEND_EV_INIT) {
        replace(&s->resolved, ev->name);
        return;
    }

    if (ev->kind == BACKEND_EV_CWD) {
        if (ev->text && *ev->text)
            replace(&s->workdir, ev->text);
        return;
    }

    if (ev->kind == BACKEND_EV_TRUST) {
        s->trust_requested = 1;
        return;
    }

    if (ev->kind == BACKEND_EV_ASSISTANT && ev->text && *ev->text &&
        !(ev->parent && *ev->parent))
        replace(&s->last_block, ev->text);

    if (s->agent && s->agent->caps & BACKEND_CAP_TASKS)
        s->tasks.lifecycle = 1;
    s->task_change = tasks_note(&s->tasks, ev, &s->task_repeat);
    if (s->task_change && !tasks_done(s->task_change))
        s->stall_told = 0;

    if (s->observer)
        s->observer(s->observer_ud, ev);
    for (int i = 0; i < LISTENERS_MAX; i++)
        if (listeners[i].fn)
            listeners[i].fn(listeners[i].ud, s, ev);

    if (s->quiet || live != s)
        return;
    sessionpresent_event(&s->present, ev, s->cwd, &s->tasks, s->task_change,
                         s->thinking);
}

static void wake_write(struct session *s);

static char *dup_or_null(const char *s)
{
    return s ? strdup(s) : NULL;
}

static void evcopy_free(struct evcopy *e)
{
    free(e->text);
    free(e->name);
    free(e->input_json);
    free(e->arg);
    free(e->diff);
    free(e->id);
    free(e->parent);
    free(e->task_type);
    free(e);
}

static void enqueue(struct session *s, const backend_event *ev)
{
    struct evcopy *e = calloc(1, sizeof *e);
    if (!e)
        return;
    e->ev = *ev;
    e->ev.text = e->text = dup_or_null(ev->text);
    e->ev.name = e->name = dup_or_null(ev->name);
    e->ev.input_json = e->input_json = dup_or_null(ev->input_json);
    e->ev.arg = e->arg = dup_or_null(ev->arg);
    e->ev.diff = e->diff = dup_or_null(ev->diff);
    e->ev.id = e->id = dup_or_null(ev->id);
    e->ev.parent = e->parent = dup_or_null(ev->parent);
    e->ev.task_type = e->task_type = dup_or_null(ev->task_type);
    e->queued_at = now_seconds();

    pthread_mutex_lock(&s->lock);
    if (s->tail)
        s->tail->next = e;
    else
        s->head = e;
    s->tail = e;
    pthread_mutex_unlock(&s->lock);

    wake_write(s);
}

static struct evcopy *dequeue(struct session *s)
{
    pthread_mutex_lock(&s->lock);
    struct evcopy *e = s->head;
    if (e) {
        s->head = e->next;
        if (!s->head)
            s->tail = NULL;
    }
    pthread_mutex_unlock(&s->lock);
    return e;
}

static void queue_drop(struct session *s)
{
    struct evcopy *e;
    while ((e = dequeue(s)))
        evcopy_free(e);
}

static void heard(struct session *s, const backend_event *ev)
{
    s->heard_at = now_seconds();
    if (ev->kind == BACKEND_EV_ASSISTANT && s->tail_bot[0])
        s->tail_mark.ms = now_seconds() * 1000.0;
    if (ev->kind == BACKEND_EV_TOOL)
        s->tool_open = 1;
    else if (ev->kind == BACKEND_EV_TOOL_RESULT)
        s->tool_open = 0;
}

static void on_event(void *ud, const backend_event *ev)
{
    struct session *s = ud;
    heard(s, ev);
    if (s->running || s->connecting) {
        enqueue(s, ev);
        return;
    }
    render_event(s, ev);
}

int session_wake_fd(const struct session *s)
{
    return s ? s->wake[0] : -1;
}

int session_idle_fd(const struct session *s)
{
    if (!s || !s->agent || !s->agent->idle_fd || s->quiet || s->resetting)
        return -1;
    return s->agent->idle_fd(s->agent);
}

static const char *tabs_provider(const struct session *s)
{
    static char buf[32];
    const char *model = NULL;

    if (!s || !s->backend || (strcmp(s->backend, "pi") && strcmp(s->backend, "core")))
        return NULL;
    if (s->resolved && *s->resolved)
        model = s->resolved;
    else if (s->model && *s->model)
        model = s->model;
    if (!model)
        return NULL;
    const char *slash = strchr(model, '/');
    if (!slash || slash == model)
        return NULL;
    size_t n = (size_t)(slash - model);
    if (n >= sizeof buf)
        return NULL;
    memcpy(buf, model, n);
    buf[n] = '\0';
    return buf;
}

static void publish(const struct session *s, const char *status)
{
    if (s->remote)
        return;
    agenttabs_publish(s, session_backend(s), status, tabs_provider(s));
    livelist_publish(s, status);
}

static void tab_busy(struct session *s, int busy)
{
    busy = busy ? 1 : 0;
    if (busy == s->idle_busy)
        return;
    s->idle_busy = busy;
    if (!busy && !s->running && !tasks_pending(&s->tasks) && !s->remote && s->id[0]) {
        title_clear(s->id);
        s->title[0] = '\0';
        if (s == live)
            status_set_note(NULL);
    }
    publish(s, busy ? "working" : "finished");
}

static void name_poll(struct session *s);
static void status_update_tick(struct session *s);
static void side_drain(struct session *s);

int session_work_count(const struct session *s)
{
    if (!s || !s->agent)
        return 0;
    return s->agent->busy ? session_idle_busy(s) : tasks_pending(&s->tasks);
}

static void stall_watch(struct session *s, int awake)
{
    int work = session_work_count(s);

    if (work) {
        if (!s->work_at)
            s->work_at = now_seconds();
        s->stall_seen = 1;
        s->stall_at = 0;
        return;
    }
    s->work_at = 0;
    if (awake) {

        s->stall_seen = 0;
        s->stall_at = 0;
        return;
    }
    if (s->stall_seen) {
        s->stall_seen = 0;
        s->stall_at = now_seconds();
        tasks_drop(&s->tasks);
    }
}

int session_stalled(struct session *s)
{
    int wait = settings_get_int(SETTING_TASK_STALL, TASK_STALL_DEFAULT);

    if (!s || wait <= 0 || s->running || s->stall_told || !s->stall_at || !s->turns)
        return 0;
    if (now_seconds() - s->stall_at < wait)
        return 0;
    if (!s->heard_at || now_seconds() - s->heard_at < wait)
        return 0;

    s->stall_told = 1;
    s->stall_at = 0;
    return 1;
}

const struct tasktab *session_tasks(const struct session *s)
{
    return s ? &s->tasks : NULL;
}

const struct task *session_task_change(const struct session *s)
{
    return s ? s->task_change : NULL;
}

int session_task_repeat(const struct session *s)
{
    return s ? s->task_repeat : 0;
}

int session_idle_pump(struct session *s)
{
    if (!s || !s->agent)
        return 0;

    name_poll(s);

    if (!s->agent->idle_pump || s->quiet || s->resetting)
        return 0;
    /* A permission prompt raised by the backend's pump runs the UI loop, which
     * pumps every tab again; the backend is mid-read and must not be re-entered. */
    if (s->idle_pumping)
        return s->idle_busy;

    if (!s->idle_busy) {
        sessionpresent_break(&s->present);
    }
    struct session *was = session_set_drawing(s);
    image_poll();
    unsigned long before = s->spoke;
    s->idle_pumping = 1;
    int busy = s->agent->idle_pump(s->agent) ? 1 : 0;
    s->idle_pumping = 0;
    int continuation = s->agent->take_continuation &&
                       s->agent->take_continuation(s->agent);
    if (continuation && s->remote && remote_prompt(s->agent) && *remote_prompt(s->agent))
        sessionpresent_prompt(remote_prompt(s->agent));
    sessionpresent_expire(&s->present, s->quiet);
    session_set_drawing(was);

    stall_watch(s, busy || s->spoke != before);
    side_drain(s);
    status_update_tick(s);
    tab_busy(s, busy);
    if (continuation && session_turn_continue_begin(s))
        return 1;
    return busy;
}

int session_idle_busy(const struct session *s)
{
    if (!s || !s->agent || !s->agent->busy)
        return 0;
    return s->agent->busy(s->agent) ? 1 : 0;
}

const char *session_wake_owed(const struct session *s)
{
    if (!s || !s->agent || !s->agent->wake_owed)
        return NULL;
    return s->agent->wake_owed(s->agent);
}

static session_key_fn typeahead;
static void          *typeahead_ud;

void session_set_typeahead(session_key_fn fn, void *ud)
{
    typeahead = fn;
    typeahead_ud = ud;
}

static void set_id(struct session *s, const char *id);

static const char *quota_name(const struct session *s)
{
    return s->remote ? NULL : quota_provider(s->backend, session_model_label(s));
}

static int same_limit(const backend_rate_limit *a, const backend_rate_limit *b)
{
    return a->available == b->available && a->kind == b->kind &&
           a->used_percent == b->used_percent && a->resets_at == b->resets_at &&
           a->window_minutes == b->window_minutes && a->balance_usd == b->balance_usd;
}

void session_rate_limit(struct session *s, backend_rate_limit *out)
{
    memset(out, 0, sizeof *out);
    if (!s)
        return;
    if (s->agent && s->agent->rate_limit)
        s->agent->rate_limit(s->agent, out);
    const char *q = quota_name(s);
    if (!q)
        return;
    if (out->available && !same_limit(out, &s->noted)) {
        s->noted = *out;
        quota_note(q, out);
    }
    quota_read(q, out);
}

static int autobackend_ready(struct session *s)
{
    int at = settings_get_int(SETTING_AUTO_BACKEND, 0);
    if (!s || s->remote || !s->agent || !s->agent->rate_limit || at < 1 || at > 100 ||
        (strcmp(s->backend, "claude") && strcmp(s->backend, "codex") &&
         strcmp(s->backend, "grok")))
        return 0;
    backend_rate_limit limit;
    session_rate_limit(s, &limit);
    return limit.available && limit.kind == BACKEND_QUOTA_PERCENT &&
           limit.used_percent >= at &&
           (limit.resets_at <= 0 || limit.resets_at > (long)time(NULL));
}

static void autobackend_interrupt(struct session *s)
{
    if (s && !s->abort_request && autobackend_ready(s)) {
        s->autobackend_due = 1;
        s->abort_request = 1;
    }
}

int session_autobackend_due(const struct session *s)
{
    return s && s->autobackend_due;
}

static void usage_poll(struct session *s)
{
    backend_rate_limit limit;
    session_rate_limit(s, &limit);
    if (limit.available) {
        if (limit.kind == BACKEND_QUOTA_PERCENT)
            agenttabs_usage(s, limit.used_percent, limit.resets_at, limit.window_minutes);
        apicore_usage(session_backend(s), &limit);
    }
}

static int adopt_title(struct session *s)
{
    char found[sizeof s->title];
    if (!title_lookup(s->id, found, sizeof found) || !strcmp(found, s->title))
        return 0;
    snprintf(s->title, sizeof s->title, "%s", found);
    if (s == live)
        status_set_note(s->title);
    publish(s, s->idle_busy ? "working" : "finished");
    return 1;
}

static void name_poll(struct session *s)
{
    if (!s || !s->agent)
        return;

    if (!s->id[0]) {
        const char *id = s->agent->session_id(s->agent);
        if (!id)
            return;
        set_id(s, id);
    }

    double now = now_seconds();
    if (now - s->title_polled < 1.0)
        return;
    s->title_polled = now;
    adopt_title(s);
}

const char *session_rename_error(enum session_rename why)
{
    switch (why) {
    case SESSION_RENAME_NO_ID:
        return "this conversation has no id yet — it gets one when the first "
               "turn ends, and the name is filed under it";
    case SESSION_RENAME_BAD_NAME:
        return "a name has to be 1 to 80 characters once quotes, spaces and a "
               "trailing period come off";
    case SESSION_RENAME_NO_STORE:
        return "the name would not stay written — check ~/.config/" APP_NAME
               "/titles";
    case SESSION_RENAME_OK:
        break;
    }
    return "renamed";
}

enum session_rename session_rename(struct session *s, const char *name)
{
    if (!s)
        return SESSION_RENAME_NO_ID;

    if (!s->id[0]) {
        if (!title_clean(name, s->held_title, sizeof s->held_title))
            return SESSION_RENAME_BAD_NAME;
        snprintf(s->title, sizeof s->title, "%s", s->held_title);
        if (s == live)
            status_set_note(s->title);
        publish(s, s->idle_busy ? "working" : "finished");
        return SESSION_RENAME_OK;
    }

    char found[sizeof s->title];
    if (!title_set(s->id, name))
        return SESSION_RENAME_BAD_NAME;
    if (!title_lookup(s->id, found, sizeof found))
        return SESSION_RENAME_NO_STORE;
    adopt_title(s);
    return SESSION_RENAME_OK;
}

static int effort_is_off(const char *effort)
{
    return !effort || !*effort ||
           !strcmp(effort, "none") || !strcmp(effort, "off");
}

static const char *spin_effort(const struct session *s)
{
    if (s->effort && *s->effort &&
        strcmp(s->effort, "default") != 0 && strcmp(s->effort, "auto") != 0)
        return s->effort;
    if (s->agent && s->agent->effort) {
        const char *got = s->agent->effort(s->agent);
        if (got && *got && strcmp(got, "auto") != 0)
            return got;
    }
    return NULL;
}

static double session_quiet(const struct session *s)
{
    if (!s || !s->heard_at || s->tool_open)
        return 0;
    double quiet = now_seconds() - s->heard_at;
    return quiet > 0 ? quiet : 0;
}

static int session_poll_input(void)
{
    if (!tty_is_raw())
        return 0;

    int interrupt = 0;
    tty_event ev;
    while (tty_read(&ev, 0)) {
        status_touch();
        if (typeahead) {
            interrupt |= typeahead(typeahead_ud, &ev);
            continue;
        }
        if (ev.key == TK_TEXT)
            free(ev.text);
        if (ev.key == TK_ESCAPE || (ev.key == TK_CHAR && ev.cp == 3))
            interrupt = 1;
        if (ev.key == TK_EOF)
            interrupt = 1;
    }
    return interrupt;
}

static void status_update_done(void *ud, const char *answer)
{
    struct session *s = ud;
    s->status_open = 0;
    s->status_at = now_seconds();
    s->status_heard = s->heard;
    if (answer)
        replace(&s->status_last, answer);
}

static void side_drain(struct session *s)
{
    cJSON *n;
    while (s->remote && (n = remote_side_take(s->agent))) {
        const char *question = cJSON_GetStringValue(cJSON_GetObjectItem(n, "question"));
        const char *answer = cJSON_GetStringValue(cJSON_GetObjectItem(n, "answer"));
        int failed = cJSON_IsTrue(cJSON_GetObjectItem(n, "failed"));
        sidechannel_show(s, question, answer, failed);
        relay_btw(s, question, answer, failed);
        cJSON_Delete(n);
    }
}

static void status_update_tick(struct session *s)
{
    if (!s || s->remote || s->status_open ||
        (s != live && s != relay_session() && !stream_watched(s)))
        return;
    double since = s->status_at;
    if (!s->running) {
        if (!s->work_at)
            return;
        if (s->work_at > since)
            since = s->work_at;
    }
    int every = settings_get_int(SETTING_STATUS_INTERVAL, STATUS_INTERVAL_DEFAULT);
    if (every <= 0 || now_seconds() - since < every || s->heard == s->status_heard)
        return;
    s->status_at = now_seconds();
    if (sidechannel_status(s, s->status_last, status_update_done, s))
        s->status_open = 1;
}

static __thread struct session *owner;

static int (*permission_hook)(struct session *s, const struct permission *p);

void session_on_permission(int (*fn)(struct session *s, const struct permission *p))
{
    permission_hook = fn;
}

static void permission_clear(struct permission *p)
{
    free(p->tool);
    free(p->detail);
    free(p->about);
    *p = (struct permission){0};
}

static void permission_fill(struct permission *p, const backend_permission *req)
{
    static const char *const keys[] = {"command", "file_path", "notebook_path", "url",
                                       "pattern", "path"};
    const char *detail = NULL;
    cJSON *input = req->input_json ? cJSON_Parse(req->input_json) : NULL;
    for (int i = 0; input && !detail && i < COUNT(keys); i++) {
        const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(input, keys[i]));
        if (v && *v)
            detail = v;
    }
    const char *about = cJSON_GetStringValue(cJSON_GetObjectItem(input, "description"));
    if (!about || !*about)
        about = req->description;
    if (!detail)
        detail = req->path;

    permission_clear(p);
    p->tool = strdup(req->tool && *req->tool ? req->tool : "tool call");
    p->detail = detail && *detail ? strdup(detail) : NULL;
    p->about = about && *about ? strdup(about) : NULL;
    cJSON_Delete(input);
}

static int on_permission(void *ud, const backend_permission *req)
{
    struct session *s = ud;

    if (owner != s) {
        struct permission p = {0};
        permission_fill(&p, req);
        int allow = permission_hook && permission_hook(s, &p);
        permission_clear(&p);
        return allow;
    }

    pthread_mutex_lock(&s->lock);
    permission_fill(&s->perm, req);
    s->perm_allow = 0;
    s->perm_state = 1;
    pthread_mutex_unlock(&s->lock);
    wake_write(s);

    struct timespec tick = {0, 20 * 1000000L};
    while (s->perm_state == 1 && !s->abort_request)
        nanosleep(&tick, NULL);

    pthread_mutex_lock(&s->lock);
    int allow = s->perm_state == 2 && s->perm_allow;
    s->perm_state = 0;
    pthread_mutex_unlock(&s->lock);
    return allow;
}

const struct permission *session_permission_pending(struct session *s)
{
    return session_permission_waiting(s) ? &s->perm : NULL;
}

int session_permission_waiting(const struct session *s)
{
    return s && s->perm_state == 1;
}

void session_permission_answer(struct session *s, int allow)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->lock);
    if (s->perm_state == 1) {
        s->perm_allow = allow;
        s->perm_state = 2;
    }
    pthread_mutex_unlock(&s->lock);
}

static int abort_check(void)
{
    if (owner)
        return owner->abort_request;

    name_poll(live);
    usage_poll(live);
    if (live && !viewport_held())
        sessionpresent_spin(live->backend, spin_effort(live), session_quiet(live),
                            SESSION_QUIET_SECONDS);

    voice_pending();

    int interrupt = session_poll_input();
    if (live && live->abort_hook)
        interrupt |= live->abort_hook(live->abort_ud);
    relay_poll(live);
    if (!interrupt)
        autobackend_interrupt(live);
    if (live && live->abort_request)
        interrupt = 1;

    sidechannel_poll();
    sidechannel_tick();
    image_poll();
    status_tick();
    return interrupt;
}

static char *dup_model(const char *backend, const char *model)
{
    char slug[128];

    if (!model)
        return NULL;
    if (!strcmp(backend, "codex") && models_codex_slug(model, slug, sizeof slug))
        return strdup(slug);
    return strdup(model);
}

struct session *session_new(const char *backend, const char *cwd, const char *model,
                            const char *effort)
{
    struct session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->wake[0] = s->wake[1] = -1;
    pthread_mutex_init(&s->lock, NULL);
    s->backend = strdup(backend && *backend ? backend : "claude");
    if (!s->backend) {
        free(s);
        return NULL;
    }
    s->cwd = cwd ? strdup(cwd) : NULL;
    s->model = dup_model(s->backend, model);
    s->effort = effort ? strdup(effort) : NULL;
    s->thinking = 1;
    intercom_name_new(s->name, sizeof s->name);
    chain_new(s->chain, sizeof s->chain);
    char addr[4300];
    if (sessionaddr_alloc(addr, sizeof addr))
        s->addr = strdup(addr);
    tasks_reset(&s->tasks, s->backend);
    return s;
}

int session_preset_model(struct session *s, const char *model)
{
    if (!s || s->agent)
        return 0;
    char *m = dup_model(s->backend, model);
    if (model && !m)
        return 0;
    free(s->model);
    s->model = m;
    prefs_remember_choice("model", s->backend, s->model);
    return 1;
}

void session_replay(struct session *s)
{
    block_cleared();
    hud_print(s);
    if (!s)
        return;
    if (!s->remote && (s->id[0] || grokbottail_applies(s)) && sessionload_into(s))
        return;
    sessionpresent_replay(&s->transcript);
}

void session_free(struct session *s)
{
    if (!s)
        return;
    if (s->start_threaded) {
        pthread_join(s->start_th, NULL);
        s->start_threaded = 0;
    }
    livelist_forget(s);
    agenttabs_forget(s);
    sidechannel_forget(s);

    if (s->running) {
        if (s->remote)
            remote_detach(s->agent);
        else
            s->abort_request = 1;
        pthread_join(s->thread, NULL);
        s->running = 0;
        free(s->reply);
    }
    session_agent_fail(s, "the subagent's tab was closed");
    queue_drop(s);
    pthread_mutex_destroy(&s->lock);
    for (int i = 0; i < 2; i++)
        if (s->wake[i] >= 0)
            close(s->wake[i]);
    free(s->asked);
    retire(s->agent);
    free(s->backend);
    free(s->remote);
    free(s->cwd);
    free(s->workdir);
    free(s->model);
    free(s->effort);
    free(s->resolved);
    free(s->last_reply);
    free(s->ledger);
    free(s->failed_prompt);
    free(s->last_block);
    for (char **e = s->env; e && *e; e++)
        free(*e);
    free(s->env);
    sessionpresent_free(&s->present);
    free(s->prompt);
    free(s->status_last);
    free(s->turn_log);
    free(s->permission);
    permission_clear(&s->perm);
    free(s->error_note);
    free(s->system_extra);
    free(s->handoff);
    transcript_free(&s->transcript);
    sessionaddr_forget(s->addr);
    free(s->addr);
    vncinset_free(s->inset);
    if (live == s)
        live = NULL;
    free(s);
}

static const char *shunt_plugin_dir(const struct session *s)
{
    static char path[4300];

    if (!s->customizations || !settings_get_int(SETTING_SHUNT, 0))
        return NULL;

    char config[4096];
    if (!path_config_dir(config, sizeof config))
        return NULL;
    snprintf(path, sizeof path, "%s/plugins/shunt", config);
    return path;
}

static char *join_system(const char *const *parts, int n)
{
    size_t total = 0;
    for (int i = 0; i < n; i++)
        if (parts[i] && *parts[i])
            total += strlen(parts[i]) + 2;
    if (!total)
        return NULL;
    char *out = malloc(total + 1), *p = out;
    if (!out)
        return NULL;
    for (int i = 0; i < n; i++) {
        if (!parts[i] || !*parts[i])
            continue;
        if (p != out)
            p += sprintf(p, "\n\n");
        p += sprintf(p, "%s", parts[i]);
    }
    return out;
}

static char *session_system(const struct session *s, const char *handoff)
{
    const char *note = image_available()
        ? "This conversation is displayed in a terminal that renders images inline. "
          "To show the user an image, write a markdown image with an absolute local "
          "path — ![alt](/abs/path.png) — alone on its own line. PNG is drawn "
          "directly; other formats are converted first. Use this whenever an image "
          "would answer better than words: a render you just produced, a screenshot, "
          "a diagram, a photo the user asked about. This only displays the image to "
          "the user; it does not show it to you. To look at an image yourself, read "
          "it with the Read tool first, then write the markdown."
        : NULL;

    const char *view =
        "To present a long text file to the user, such as a plan, write it to disk "
        "and output `@view /abs/path.md` alone on its own line instead of repeating "
        "its contents. The user can open it in a full-screen pager; markdown is "
        "rendered.";

    const char *ask =
        "When you need the user to answer questions before you continue, end the "
        "reply with an `@ask` line followed by a numbered list of questions, each "
        "optionally followed by `- option` lines (`- option \xe2\x80\x94 description` "
        "to explain one), with no code fence. The user fills in a form and every "
        "question also takes a typed answer, so omit options for open questions. "
        "Their answers arrive as the next message, numbered to match.";

    char       *intercom = intercom_note(s->name);
    const char *parts[] = {note, view, ask, intercom, s->system_extra, handoff};
    char       *joined = join_system(parts, 6);
    free(intercom);
    return joined;
}


int session_set_env(struct session *s, const char *const *env)
{
    if (s->agent)
        return 0;
    size_t n = 0;
    while (env && env[n])
        n++;
    char **copy = calloc(n + 1, sizeof *copy);
    if (!copy)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!(copy[i] = strdup(env[i]))) {
            while (i)
                free(copy[--i]);
            free(copy);
            return 0;
        }
    for (char **e = s->env; e && *e; e++)
        free(*e);
    free(s->env);
    s->env = copy;
    return 1;
}

const backend_result *session_last_result(const struct session *s)
{
    return &s->last_result;
}

struct agent_job {
    struct agent_job *next;
    Backend          *child;
    char             *task, *cwd, *model, *effort, *backend;
    char             *report;
    char              name[INTERCOM_NAME_MAX];
    int               status, taken, started, refs;
};

#define AGENT_PENDING (-2)

static pthread_mutex_t   job_mu   = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    job_cond = PTHREAD_COND_INITIALIZER;
static struct agent_job *jobs;

static void job_unref(struct agent_job *j)
{
    if (--j->refs)
        return;
    free(j->task);
    free(j->cwd);
    free(j->model);
    free(j->backend);
    free(j->effort);
    free(j->report);
    free(j);
}

static void wake_write(struct session *s);
static void claim_name(struct session *s);

/* Fire and forget: returns once the subagent's tab is running its task, or
 * at once if the parent is interrupted first. The tab keeps going either way;
 * its task tells it how to report back, such as with scrap send. */
static int host_agent(void *ud, Backend *child, const char *task, const char *cwd, char **report)
{
    struct session   *s = ud;
    struct agent_job *j = calloc(1, sizeof *j);
    if (!j)
        return BACKEND_AGENT_DECLINED;
    j->child  = child;
    j->task   = strdup(task);
    j->cwd    = cwd ? strdup(cwd) : s->cwd ? strdup(s->cwd) : NULL;
    j->model  = s->model ? strdup(s->model) : NULL;
    j->effort = s->effort ? strdup(s->effort) : NULL;
    j->backend = strdup(s->backend);
    j->status = AGENT_PENDING;
    j->refs   = 2;

    pthread_mutex_lock(&job_mu);
    j->next = jobs;
    jobs    = j;
    pthread_mutex_unlock(&job_mu);
    wake_write(s);

    pthread_mutex_lock(&job_mu);
    while (j->status == AGENT_PENDING && !j->started && !s->abort_request) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&job_cond, &job_mu, &ts);
    }
    int status = j->status;
    if (status == AGENT_PENDING) {
        status  = BACKEND_AGENT_STARTED;
        *report = j->name[0] ? text_dsprintf("started subagent @%s in its own tab", j->name)
                             : strdup("started a subagent in its own tab");
    } else {
        *report   = j->report;
        j->report = NULL;
    }
    job_unref(j);
    pthread_mutex_unlock(&job_mu);
    return status;
}

struct agent_job *session_agent_take(void)
{
    pthread_mutex_lock(&job_mu);
    struct agent_job *j = jobs;
    if (j) {
        jobs     = j->next;
        j->taken = 1;
    }
    pthread_mutex_unlock(&job_mu);
    return j;
}

static void job_finish(struct agent_job *j, int status, const char *report)
{
    pthread_mutex_lock(&job_mu);
    if (j->status == AGENT_PENDING) {
        j->status = status;
        j->report = report ? strdup(report) : NULL;
    }
    job_unref(j);
    pthread_cond_broadcast(&job_cond);
    pthread_mutex_unlock(&job_mu);
}

struct session *session_agent_open(struct agent_job *j)
{
    struct session *s = session_new(j->backend, j->cwd, j->model, j->effort);
    if (!s) {
        j->child->close(j->child);
        job_finish(j, BACKEND_AGENT_FAILED, "could not open a tab for the subagent");
        return NULL;
    }
    s->agent       = j->child;
    s->job         = j;
    s->subagent    = 1;
    s->thinking    = settings_get_int(SETTING_THINKING, 1);
    s->compact     = settings_get_int(SETTING_COMPACT, 0);
    s->agent->set_event_cb(s->agent, on_event, s);
    s->agent->set_abort_check(s->agent, abort_check);
    claim_name(s);
    pthread_mutex_lock(&job_mu);
    snprintf(j->name, sizeof j->name, "%s", s->name);
    pthread_mutex_unlock(&job_mu);
    snprintf(s->title, sizeof s->title, "agent: %.100s", j->task);
    for (char *c = s->title; *c; c++)
        if (*c == '\n' || *c == '\t')
            *c = ' ';
    publish(s, "finished");
    return s;
}

const char *session_agent_task(const struct session *s)
{
    return s && s->job ? s->job->task : NULL;
}

void session_agent_started(struct session *s)
{
    if (!s || !s->job)
        return;
    pthread_mutex_lock(&job_mu);
    s->job->started = 1;
    pthread_cond_broadcast(&job_cond);
    pthread_mutex_unlock(&job_mu);
}

void session_agent_fail(struct session *s, const char *why)
{
    if (!s || !s->job)
        return;
    job_finish(s->job, BACKEND_AGENT_FAILED, why);
    s->job = NULL;
}

void session_agent_poll(struct session *s)
{
    if (!s || !s->subagent)
        return;
    if (s->running) {
        s->idle_at = 0;
        return;
    }
    if (s->job) {
        const backend_result *m = &s->last_result;
        int status = m->interrupted ? BACKEND_AGENT_INTERRUPTED
                     : m->is_error || !s->last_reply ? BACKEND_AGENT_FAILED
                                                     : BACKEND_AGENT_DONE;
        job_finish(s->job, status, s->last_reply);
        s->job = NULL;
    }
    if (!s->idle_at)
        s->idle_at = now_seconds();
}

double session_agent_idle(const struct session *s)
{
    return s && s->subagent && !s->running && s->idle_at ? now_seconds() - s->idle_at : -1;
}

static Backend *agent(struct session *s)
{
    if (s->agent)
        return s->agent;
    backend_opts o = {0};
    o.name = s->backend;
    o.cwd = s->cwd;
    o.session_file = s->addr;
    o.model = s->model;
    o.effort = s->effort;
    o.allow_customizations = s->customizations;
    o.permission_mode = s->permission;
    o.fork_session = s->fork_session;
    o.memory = s->memory;
    o.no_browser_login = s->no_browser_login;
    o.chrome = settings_get_int(SETTING_CHROME, 0);
    o.plugin_dir = shunt_plugin_dir(s);
    o.env = (const char *const *)s->env;
    char exe[PATH_MAX];
    if (s->memory && hub_self_path(exe, sizeof exe))
        o.memory_relay = exe;
    o.skip_quota_read = !strcmp(s->backend, "grok") ||
                        (!strcmp(s->backend, "codex") && quota_fresh("codex"));

    char *joined = s->remote ? NULL : session_system(s, s->handoff);
    o.system = joined;
    s->agent = s->remote ? remote_open(s->remote) : backend_open_ex(&o);
    free(joined);
    if (s->agent) {
        const char *q = quota_name(s);
        if (q)
            quota_want(q);
        s->agent->set_event_cb(s->agent, on_event, s);
        s->agent->set_abort_check(s->agent, abort_check);
        if (s->agent->set_permission_cb)
            s->agent->set_permission_cb(s->agent, on_permission, s);
        if (s->agent->set_agent_host)
            s->agent->set_agent_host(s->agent, host_agent, s);
    }
    return s->agent;
}

static void remember_window(const struct session *s);

static void *retire_thread(void *arg)
{
    Backend *b = arg;
    b->close(b);
    return NULL;
}

static void retire(Backend *b)
{
    if (!b)
        return;

    b->set_event_cb(b, NULL, NULL);
    b->set_abort_check(b, NULL);

    pthread_t t;
    if (pthread_create(&t, NULL, retire_thread, b) != 0)
        b->close(b);
    else
        pthread_join(t, NULL);
}

static char start_error[512];

const char *session_start_error(void)
{
    return start_error[0] ? start_error : NULL;
}

static char *carried_handoff(const struct session *s)
{
    struct transcript disk = {0};
    const struct transcript *src = &s->transcript;
    if (s->id[0] && sessionload_fill(&disk, s->backend, s->cwd, s->id))
        src = &disk;
    char *handoff = transcript_handoff(src, 128 * 1024, s->id[0] ? s->id : NULL);
    transcript_free(&disk);
    if (s->failed_prompt && *s->failed_prompt) {
        char *full = text_dsprintf("%s\nThe most recent user request failed before completion:\n%s\n",
                                  handoff ? handoff : "", s->failed_prompt);
        if (full) {
            free(handoff);
            handoff = full;
        }
    }
    return handoff;
}

static int switch_backend(struct session *s, const char *backend,
                          const char *model, const char *effort, int quota)
{
    if (!s || !backend || !*backend || s->running)
        return 0;
    if (strcmp(s->backend, backend) == 0)
        return 1;

    char *handoff = carried_handoff(s);
    char *joined = session_system(s, handoff);
    backend_opts o = {0};
    o.name = backend;
    o.model = model;
    o.effort = effort;
    o.system = joined;
    o.cwd = s->cwd;
    o.allow_customizations = s->customizations;
    o.permission_mode = s->permission;
    o.no_browser_login = s->no_browser_login;
    o.chrome = settings_get_int(SETTING_CHROME, 0);
    o.plugin_dir = shunt_plugin_dir(s);
    o.env = (const char *const *)s->env;
    Backend *replacement = backend_open_ex(&o);
    free(joined);
    if (!replacement) {
        free(handoff);
        return 0;
    }

    if (replacement->set_permission_cb)
        replacement->set_permission_cb(replacement, on_permission, s);
    if (!replacement->start(replacement, NULL) ||
        (quota > 0 && replacement->connect && !replacement->connect(replacement))) {
        const char *why = replacement->last_error ? replacement->last_error(replacement) : NULL;
        snprintf(start_error, sizeof start_error, "%s", why ? why : "");
        replacement->close(replacement);
        free(handoff);
        return 0;
    }
    if (quota > 0) {
        backend_rate_limit limit = {0};
        if (replacement->rate_limit)
            replacement->rate_limit(replacement, &limit);
        if (!limit.available || limit.used_percent >= quota ||
            (limit.resets_at > 0 && limit.resets_at <= (long)time(NULL))) {
            replacement->close(replacement);
            free(handoff);
            return 0;
        }
    }
    replacement->set_event_cb(replacement, on_event, s);
    replacement->set_abort_check(replacement, abort_check);

    Backend *previous = s->agent;
    s->agent = replacement;
    s->quota_failed = 0;
    free(s->handoff);
    s->handoff = handoff;
    replace(&s->backend, backend);
    replace(&s->model, model);
    replace(&s->effort, effort);
    replace(&s->resolved, NULL);

    if (s->title[0] && !s->held_title[0])
        snprintf(s->held_title, sizeof s->held_title, "%s", s->title);
    s->id[0] = '\0';
    const char *id = replacement->session_id(replacement);
    if (id)
        set_id(s, id);
    s->turns = 0;
    s->cost_usd = 0;
    s->tokens_in = s->tokens_out = s->tokens_cached = 0;
    s->context_tokens = 0;
    s->context_window = 0;
    retire(previous);
    const char *q = quota_name(s);
    if (q)
        quota_want(q);
    return 1;
}

int session_switch_backend(struct session *s, const char *backend)
{
    return switch_backend(s, backend, NULL, NULL, 0);
}

void session_set_quiet(struct session *s, int quiet) { s->quiet = quiet; }

const struct transcript *session_transcript(struct session *s)
{
    if (!s)
        return NULL;
    if (!s->transcript_loaded && !s->remote && s->id[0]) {
        s->transcript_loaded = 1;
        struct transcript disk = {0};
        if (sessionload_fill(&disk, s->backend, s->cwd, s->id) > 0) {
            transcript_free(&s->transcript);
            s->transcript = disk;
        } else {
            transcript_free(&disk);
        }
    }
    return &s->transcript;
}

int session_add_listener(session_listener_fn fn, void *ud)
{
    for (int i = 0; i < LISTENERS_MAX; i++) {
        if (!listeners[i].fn) {
            listeners[i].fn = fn;
            listeners[i].ud = ud;
            return 1;
        }
    }
    return 0;
}

void session_remove_listener(session_listener_fn fn, void *ud)
{
    for (int i = 0; i < LISTENERS_MAX; i++)
        if (listeners[i].fn == fn && listeners[i].ud == ud)
            listeners[i].fn = NULL;
}

void session_set_observer(struct session *s, session_event_fn fn, void *ud)
{
    s->observer = fn;
    s->observer_ud = ud;
}

void session_set_system_extra(struct session *s, const char *text)
{
    replace(&s->system_extra, text);
}

void session_set_abort_hook(struct session *s, int (*fn)(void *ud), void *ud)
{
    s->abort_hook = fn;
    s->abort_ud = ud;
}

void session_set_thinking(struct session *s, int on) { s->thinking = on; }

int session_thinking(const struct session *s) { return s->thinking; }

void session_set_compact(struct session *s, int on) { s->compact = on; }

int session_compact(const struct session *s) { return s->compact; }

void session_set_customizations(struct session *s, int on) { s->customizations = on; }
void session_set_browser_login(struct session *s, int on) { s->no_browser_login = !on; }

void session_set_fork(struct session *s, int on) { s->fork_session = on; }

static void claim_name(struct session *s)
{
    char other[128];
    if (!s->name[0] || !livelist_name_holder(s, s->name, other, sizeof other))
        return;
    char fresh[INTERCOM_NAME_MAX];
    intercom_name_next(s->name, fresh, sizeof fresh);
    snprintf(s->name, sizeof s->name, "%s", fresh);
}

static int history_file(const char *key, char *out, size_t size)
{
    char dir[4096];
    return key && *key && path_config_subdir(dir, sizeof dir, "history") &&
           (size_t)snprintf(out, size, "%s/%s", dir, key) < size;
}

static void copy_history(const char *from, const char *to)
{
    char   src[4200], dst[4200];
    size_t n = 0;
    char  *data = history_file(from, src, sizeof src) && history_file(to, dst, sizeof dst)
                      ? text_slurp(src, 1 << 20, &n)
                      : NULL;
    FILE  *f = data ? fopen(dst, "w") : NULL;
    if (f) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
    free(data);
}

static void fork_chain(struct session *s, const char *parent_id)
{
    char parent[CHAIN_ID_MAX];
    snprintf(s->fork_from, sizeof s->fork_from, "%s", parent_id);
    chain_new(s->chain, sizeof s->chain);
    if (chain_find(parent_id, parent, sizeof parent)) {
        chain_copy(parent, parent_id, s->chain, s->name);
        copy_history(parent, s->chain);
    }
}

static void track_chain(struct session *s)
{
    char have[CHAIN_ID_MAX], name[INTERCOM_NAME_MAX];
    if (!strcmp(s->id, s->fork_from))
        return;
    s->fork_from[0] = '\0';
    if (intercom_name_of(s->id, name, sizeof name) && chain_find(s->id, have, sizeof have)) {
        snprintf(s->chain, sizeof s->chain, "%s", have);
        snprintf(s->name, sizeof s->name, "%s", name);
        return;
    }
    chain_add(s->chain, s->name, s->backend, s->cwd, s->id);
}

static void set_id(struct session *s, const char *id)
{
    int changed = strcmp(s->id, id) != 0;
    snprintf(s->id, sizeof s->id, "%s", id);
    sessionaddr_write(s->addr, s->id);
    if (changed && !s->remote) {
        track_chain(s);
        claim_name(s);
    }
    if (changed) {
        if (s->held_title[0]) {
            title_set(s->id, s->held_title);
            s->held_title[0] = '\0';
        } else {
            s->title[0] = '\0';
        }
        title_lookup(s->id, s->title, sizeof s->title);
        if (!s->remote && s->context_tokens <= 0) {
            long tokens, window;
            if (sessionload_context(s->backend, s->cwd, s->id, &tokens, &window)) {
                s->context_tokens = tokens;
                if (window > 0 && s->context_window <= 0)
                    s->context_window = window;
                hud_refresh(s);
            }
        }
        status_set_note(s->title);
        publish(s, s->idle_busy ? "working" : "finished");
    }
    agenttabs_forget_hook(id);
}

static const char *saved_id(const struct session *s)
{
    return s->id[0] && s->saved ? s->id : NULL;
}

static void adopt_remote(struct session *s, Backend *b)
{
    const cJSON *h = remote_history(b);
    const char  *backend = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)h, "backend"));
    const char  *model = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)h, "model"));
    if (backend && *backend)
        replace(&s->backend, backend);
    replace(&s->model, model && *model ? model : NULL);
    replace(&s->resolved, model && *model ? model : NULL);
    transcript_clear(&s->transcript);
    cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem((cJSON *)h, "turns"))
    {
        const char *user = cJSON_GetStringValue(cJSON_GetObjectItem(t, "user"));
        const char *reply = cJSON_GetStringValue(cJSON_GetObjectItem(t, "assistant"));
        transcript_add(&s->transcript, s->backend, user ? user : "", reply ? reply : "",
                       cJSON_IsTrue(cJSON_GetObjectItem(t, "interrupted")));
    }
    snprintf(s->title, sizeof s->title, "%s", s->remote);
}

static int restart(struct session *s, const char *resume_id)
{
    if (s->running)
        return 0;

    Backend *previous = s->agent;
    s->agent = NULL;
    start_error[0] = '\0';
    char parent[INTERCOM_NAME_MAX];
    if (s->remote)
        ;
    else if (resume_id && s->fork_session && !s->fork_named &&
        intercom_name_of(resume_id, parent, sizeof parent)) {
        intercom_name_next(parent, s->name, sizeof s->name);
        s->fork_named = 1;
    } else if (resume_id && !s->fork_named)
        intercom_name_of(resume_id, s->name, sizeof s->name);
    if (!s->remote)
        claim_name(s);

    char path[4096];
    if (resume_id && !s->remote && sessionlist_available(s->backend) &&
        !sessionload_path(s->backend, s->cwd, resume_id, path, sizeof path))
        resume_id = NULL;

    if (s->memory && previous) {
        retire(previous);
        previous = NULL;
    }
    Backend *b = agent(s);
    if (!b || !b->start(b, resume_id)) {
        if (b) {
            const char *why = b->last_error ? b->last_error(b) : NULL;
            snprintf(start_error, sizeof start_error, "%s", why ? why : "");
            b->set_event_cb(b, NULL, NULL);
            b->set_abort_check(b, NULL);
            b->close(b);
        }
        s->agent = previous;
        return 0;
    }
    retire(previous);
    if (s->remote)
        adopt_remote(s, b);
    tasks_reset(&s->tasks, s->backend);
    s->stall_seen = s->stall_told = 0;
    s->stall_at = 0;

    if (resume_id && s->fork_session && !s->forked && !s->remote) {
        s->forked = 1;
        fork_chain(s, resume_id);
    }
    if (resume_id && resume_id != s->id)
        set_id(s, resume_id);

    const char *id = b->session_id(b);
    if (id)
        set_id(s, id);
    s->saved = resume_id && !strcmp(s->id, resume_id);

    if (!strcmp(s->backend, "grokbot") && b->model && b->model(b)) {
        if (!s->model)
            replace(&s->model, b->model(b));
        snprintf(s->title, sizeof s->title, "%s", b->model(b));
        status_set_note(s->title);
    }
    return 1;
}

struct restart_job {
    struct session *s;
    const char     *resume_id;
    int             ok;
    int             done;
    pthread_mutex_t mu;
};

static int restart_job_done(struct restart_job *j)
{
    pthread_mutex_lock(&j->mu);
    int done = j->done;
    pthread_mutex_unlock(&j->mu);
    return done;
}

static void *restart_job_run(void *ud)
{
    struct restart_job *j = ud;
    owner = j->s;
    restart_shield_thread();
    int ok = restart(j->s, j->resume_id);
    pthread_mutex_lock(&j->mu);
    j->ok = ok;
    j->done = 1;
    pthread_mutex_unlock(&j->mu);
    return NULL;
}

static void start_word(const struct session *s, char *out, size_t size)
{
    snprintf(out, size, "Starting %s\xe2\x80\xa6", s->model ? s->model : "Grok Bot");
}

static int restart_spun(struct session *s, const char *resume_id)
{
    if (strcmp(s->backend, "grokbot"))
        return restart(s, resume_id);
    struct restart_job j = {.s = s, .resume_id = resume_id};
    pthread_mutex_init(&j.mu, NULL);
    pthread_t t;
    if (pthread_create(&t, NULL, restart_job_run, &j) != 0) {
        j.ok = restart(s, resume_id);
    } else {
        char word[128];
        start_word(s, word, sizeof word);
        int owned = status_work_begin(word);
        struct timespec slice = {0, SPIN_FRAME_MS * 1000000L};
        while (!restart_job_done(&j)) {
            nanosleep(&slice, NULL);
            status_tick();
        }
        pthread_join(t, NULL);
        status_work_end(owned);
    }
    pthread_mutex_destroy(&j.mu);
    return j.ok;
}

static int connect_agent(struct session *s)
{
    char next[4096];
    if (!dir_alive(s->cwd) && ground_target(s->cwd, next, sizeof next))
        replace(&s->cwd, next);
    return restart(s, saved_id(s));
}

int session_start(struct session *s)
{
    if (!connect_agent(s))
        return 0;
    publish(s, "finished");
    return 1;
}

static int start_pipe[2] = {-1, -1};

static void start_notify(void)
{
    if (start_pipe[1] < 0)
        return;
    char byte = 1;
    ssize_t w = write(start_pipe[1], &byte, 1);
    (void)w;
}

static void *start_thread(void *ud)
{
    struct session *s = ud;

    owner = s;

    restart_shield_thread();
    s->start_ok = connect_agent(s);
    s->start_finished = 1;
    start_notify();
    return NULL;
}

int session_start_fd(void)
{
    if (start_pipe[0] < 0 && pipe(start_pipe) == 0) {
        for (int i = 0; i < 2; i++) {
            fcntl(start_pipe[i], F_SETFD, FD_CLOEXEC);
            fcntl(start_pipe[i], F_SETFL, O_NONBLOCK);
        }
    }
    return start_pipe[0];
}

void session_start_drain(void)
{
    if (start_pipe[0] < 0)
        return;
    char buf[256];
    while (read(start_pipe[0], buf, sizeof buf) > 0)
        ;
}

void session_start_batch(struct session **list, int n)
{
    if (!list || n <= 0)
        return;

    session_start_fd();
    for (int i = 0; i < n; i++) {
        struct session *s = list[i];
        if (!s)
            continue;
        s->connecting = 1;
        s->start_finished = 0;
        s->start_ok = 0;
        s->start_threaded = pthread_create(&s->start_th, NULL, start_thread, s) == 0;
        if (!s->start_threaded) {
            s->start_ok = connect_agent(s);
            s->start_finished = 1;
        }
    }
}

int session_start_done(const struct session *s)
{
    return s && (!s->start_threaded || s->start_finished);
}

int session_start_wait(struct session *s)
{
    if (!s)
        return 0;
    if (s->start_threaded) {
        if (!strcmp(s->backend, "grokbot") && !s->start_finished) {
            char word[128];
            start_word(s, word, sizeof word);
            int owned = status_work_begin(word);
            struct timespec slice = {0, SPIN_FRAME_MS * 1000000L};
            while (!s->start_finished) {
                nanosleep(&slice, NULL);
                status_tick();
            }
            status_work_end(owned);
        }
        pthread_join(s->start_th, NULL);
        s->start_threaded = 0;
    }
    s->connecting = 0;
    if (s->start_ok)
        publish(s, "finished");
    return s->start_ok;
}

int session_trust_project(struct session *s)
{
    Backend *b = agent(s);
    if (!b || !b->trust_project || !b->trust_project(b, s->cwd))
        return 0;
    return restart(s, saved_id(s));
}

int session_take_trust_request(struct session *s)
{
    if (!s || !s->trust_requested)
        return 0;
    s->trust_requested = 0;
    return 1;
}

int session_set_model(struct session *s, const char *model)
{
    if (!session_retarget(s, model, s ? s->effort : NULL, s ? s->cwd : NULL))
        return 0;
    prefs_remember_choice("model", s->backend, s->model);
    return 1;
}

int session_set_effort(struct session *s, const char *effort)
{
    Backend *b = agent(s);
    if (!b || !(b->caps & BACKEND_CAP_EFFORT))
        return 0;

    if (!s->running && (b->caps & BACKEND_CAP_LIVE_EFFORT) && b->set_effort) {
        if (!b->set_effort(b, effort))
            return 0;
        replace(&s->effort, effort);
        prefs_remember_choice("effort", s->backend, s->effort);
        return 1;
    }

    if (!session_retarget(s, s->model, effort, s->cwd))
        return 0;
    prefs_remember_choice("effort", s->backend, s->effort);
    return 1;
}

static const struct {
    const char *name;
    const char *desc;
} PERMISSIONS[] = {
    {"bypassPermissions", "never refuses a tool call"},
    {"auto", "approves the safe calls, asks about the rest"},
    {"acceptEdits", "edits without asking, asks about the rest"},
    {"dontAsk", "refuses anything that would ask"},
    {"manual", "asks about everything not pre-allowed"},
    {"plan", "read-only: research and propose, no changes"},
};
#define PERMISSION_COUNT (COUNT(PERMISSIONS))
#define PERMISSION_DEFAULT 1

int session_permission_count(void) { return PERMISSION_COUNT; }
int session_permission_default(void) { return PERMISSION_DEFAULT; }

const char *session_permission_name(int index)
{
    return (index >= 0 && index < PERMISSION_COUNT) ? PERMISSIONS[index].name : NULL;
}

const char *session_permission_desc(int index)
{
    return (index >= 0 && index < PERMISSION_COUNT) ? PERMISSIONS[index].desc : NULL;
}

int session_permission_index(const char *mode)
{
    if (!mode)
        return -1;
    for (int i = 0; i < PERMISSION_COUNT; i++)
        if (strcmp(PERMISSIONS[i].name, mode) == 0)
            return i;
    return -1;
}

const char *session_permission(const struct session *s)
{
    return (s && s->permission) ? s->permission : PERMISSIONS[PERMISSION_DEFAULT].name;
}

static int swap_and_restart(struct session *s, char **slot, const char *next)
{
    char *previous = *slot;
    *slot = next ? strdup(next) : NULL;
    if (next && !*slot) {
        *slot = previous;
        return 0;
    }
    if (restart(s, saved_id(s))) {
        free(previous);
        return 1;
    }
    free(*slot);
    *slot = previous;
    return 0;
}

int session_set_permission(struct session *s, const char *mode)
{
    if (!s->agent) {
        replace(&s->permission, mode);
        return 1;
    }
    return swap_and_restart(s, &s->permission, mode);
}

int session_memory(const struct session *s) { return s->memory; }

char *session_memory_view(struct session *s)
{
    return s->agent && s->agent->memory_view ? s->agent->memory_view(s->agent) : NULL;
}

long session_memory_pending(struct session *s)
{
    if (!s || s->remote || !s->agent || !s->agent->memory_pending)
        return -1;
    return s->agent->memory_pending(s->agent);
}

int session_set_memory(struct session *s, int on)
{
    int previous = s->memory;
    s->memory = on;
    if (!s->agent || restart(s, on ? NULL : (s->id[0] ? s->id : NULL)))
        return 1;
    s->memory = previous;
    return 0;
}

void session_adopt_chain(struct session *s, const char *chain)
{
    if (!s || !chain || !*chain || strlen(chain) >= sizeof s->chain)
        return;
    for (const char *p = chain; *p; p++)
        if (!isxdigit((unsigned char)*p))
            return;
    snprintf(s->chain, sizeof s->chain, "%s", chain);
}

void session_adopt_id(struct session *s, const char *id)
{
    if (s && id && *id) {
        set_id(s, id);
        s->saved = 1;
    }
}

#define RESET_BLOCK   (1 << 0)
#define RESET_WORKDIR (1 << 1)

static void reset_turns(struct session *s, int flags)
{
    s->turns = 0;
    replace(&s->handoff, NULL);
    s->cost_usd = 0;
    s->tokens_in = s->tokens_out = s->tokens_cached = 0;
    s->ledger_n = 0;
    s->context_tokens = 0;
    if (flags & RESET_WORKDIR)
        replace(&s->workdir, NULL);
    replace(&s->last_reply, NULL);
    replace(&s->failed_prompt, NULL);
    if (flags & RESET_BLOCK)
        replace(&s->last_block, NULL);
    transcript_clear(&s->transcript);
    s->transcript_loaded = 0;
}

int session_resume(struct session *s, const char *id)
{
    if (!restart(s, id))
        return 0;
    reset_turns(s, 0);
    return 1;
}

static void moved_over(struct session *s)
{
    replace(&s->workdir, NULL);
    if (s->title[0] && !s->held_title[0])
        snprintf(s->held_title, sizeof s->held_title, "%s", s->title);
    s->id[0] = '\0';
    const char *id = s->agent->session_id(s->agent);
    if (id)
        set_id(s, id);
    s->context_tokens = 0;
    gitinfo_forget();
}

static void started_over(struct session *s)
{
    reset_turns(s, RESET_BLOCK | RESET_WORKDIR);
    s->title[0] = '\0';
    s->held_title[0] = '\0';
    s->autohandoff = AUTOHANDOFF_IDLE;
    status_set_note(NULL);

    s->id[0] = '\0';
    const char *id = s->agent->session_id(s->agent);
    if (id)
        set_id(s, id);
    gitinfo_forget();
}

int session_set_cwd(struct session *s, const char *path)
{
    if (!s || !path || !*path)
        return 0;
    if (s->cwd && strcmp(s->cwd, path) == 0)
        return 1;

    char *was = s->handoff;
    s->handoff = carried_handoff(s);
    if (!session_retarget(s, s->model, s->effort, path)) {
        free(s->handoff);
        s->handoff = was;
        return 0;
    }
    free(was);
    return 1;
}

static int session_retarget(struct session *s, const char *model, const char *effort,
                            const char *cwd)
{
    if (!s)
        return 0;

    const char *want = cwd && *cwd ? cwd : s->cwd;
    if (!want)
        return 0;

    int moved = !s->cwd || strcmp(s->cwd, want) != 0;
    char *next_model = dup_model(s->backend, model && *model ? model : NULL);
    char *next_effort = effort && *effort ? strdup(effort) : NULL;
    char *next_cwd = strdup(want);
    if ((model && *model && !next_model) || (effort && *effort && !next_effort) ||
        !next_cwd) {
        free(next_model);
        free(next_effort);
        free(next_cwd);
        return 0;
    }

    if (!moved && same_string(s->model, next_model) &&
        same_string(s->effort, next_effort)) {
        free(next_model);
        free(next_effort);
        free(next_cwd);
        return 1;
    }

    char *was_model = s->model, *was_effort = s->effort, *was_cwd = s->cwd;
    s->model = next_model;
    s->effort = next_effort;
    s->cwd = next_cwd;

    if (!restart_spun(s, moved || s->handoff ? NULL : saved_id(s))) {
        free(s->model);
        free(s->effort);
        free(s->cwd);
        s->model = was_model;
        s->effort = was_effort;
        s->cwd = was_cwd;
        return 0;
    }
    free(was_model);
    free(was_effort);
    free(was_cwd);

    replace(&s->resolved, NULL);
    if (moved && !s->handoff)
        started_over(s);
    else if (moved)
        moved_over(s);
    publish(s, s->idle_busy ? "working" : "finished");
    return 1;
}

int session_clear(struct session *s)
{
    if (!s->agent)
        return 0;
    s->resetting = 1;
    int ok = s->agent->reset(s->agent);
    s->resetting = 0;
    if (!ok)
        return 0;
    reset_turns(s, RESET_BLOCK);
    tasks_reset(&s->tasks, s->backend);

    s->title[0] = '\0';
    s->held_title[0] = '\0';
    status_set_note(NULL);

    const char *id = s->agent->session_id(s->agent);
    if (id)
        set_id(s, id);
    else
        s->id[0] = '\0';
    return 1;
}

int session_clear_history(struct session *s)
{
    if (!session_clear(s))
        return 0;
    chain_cut(s->chain, s->id[0] ? s->id : NULL);
    return 1;
}

const char *session_chain(const struct session *s)
{
    return s ? s->chain : "";
}

int session_history_file(const struct session *s, char *out, size_t size)
{
    char legacy[4200];
    if (!history_file(s->chain, out, size))
        return 0;
    if (access(out, F_OK) != 0 && history_file(s->name, legacy, sizeof legacy))
        rename(legacy, out);
    return 1;
}

static int dir_alive(const char *path)
{
    struct stat st;
    return path && *path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int nearest_live_dir(const char *path, char *out, size_t size)
{
    snprintf(out, size, "%s", path);
    for (char *slash; (slash = strrchr(out, '/')); ) {
        if (slash == out)
            out[1] = '\0';
        else
            *slash = '\0';
        if (dir_alive(out))
            return 1;
        if (slash == out)
            return 0;
    }
    return 0;
}

static int main_worktree(const char *near, char *out, size_t size)
{
    char quoted[4200];
    if (!text_shell_quote(near, quoted, sizeof quoted))
        return 0;

    char cmd[4300];
    if (snprintf(cmd, sizeof cmd,
                 "git -C %s rev-parse --path-format=absolute --git-common-dir 2>/dev/null",
                 quoted) >= (int)sizeof cmd)
        return 0;

    FILE *f = popen(cmd, "r");
    if (!f)
        return 0;
    char line[4096] = "";
    char *got = fgets(line, sizeof line, f);
    pclose(f);
    if (!got)
        return 0;
    line[strcspn(line, "\n")] = '\0';

    char *slash = strrchr(line, '/');
    if (!slash || slash == line || strcmp(slash + 1, ".git") != 0)
        return 0;
    *slash = '\0';
    if (!dir_alive(line))
        return 0;
    snprintf(out, size, "%s", line);
    return 1;
}

static int ground_target(const char *gone, char *out, size_t size)
{
    char near[4096];
    if (!nearest_live_dir(gone, near, sizeof near))
        return 0;
    if (main_worktree(near, out, size))
        return 1;
    if (!strcmp(near, "/")) {
        const char *home = getenv("HOME");
        if (dir_alive(home)) {
            snprintf(out, size, "%s", home);
            return 1;
        }
    }
    snprintf(out, size, "%s", near);
    return 1;
}

__attribute__((format(printf, 2, 3)))
static void session_warn(struct session *s, const char *fmt, ...)
{
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);

    backend_event ev = {.kind = BACKEND_EV_WARNING, .text = text};
    struct session *was = session_set_drawing(s);
    on_event(s, &ev);
    session_set_drawing(was);
}

static void shorten(const char *dir, char *out, size_t size)
{
    path_home_relative(dir, out, size);
    if (!*out)
        snprintf(out, size, "%s", dir ? dir : "?");
}

enum { GROUND_LOST, GROUND_OK, GROUND_MOVED };

static int session_reground(struct session *s)
{
    const char *dir = s->workdir ? s->workdir : s->cwd;
    if (dir_alive(dir))
        return GROUND_OK;

    char gone[4096], shown[512];
    snprintf(gone, sizeof gone, "%s", dir ? dir : "");
    shorten(gone, shown, sizeof shown);

    if (dir_alive(s->cwd)) {
        replace(&s->workdir, NULL);
        if (restart(s, saved_id(s))) {
            char home[512];
            shorten(s->cwd, home, sizeof home);
            session_warn(s, "%s is gone — restarted in %s", shown, home);
            return GROUND_MOVED;
        }
    } else {
        char next[4096], moved[512];
        if (ground_target(gone, next, sizeof next)) {
            shorten(next, moved, sizeof moved);
            if (session_set_cwd(s, next)) {
                session_warn(s, "%s is gone — restarted in %s",
                             shown, moved);
                return GROUND_MOVED;
            }
        }
    }

    char note[700];
    snprintf(note, sizeof note,
             "the working directory %s no longer exists and the session could not "
             "be restarted elsewhere", shown);
    replace(&s->error_note, note);
    session_warn(s, "%s", note);
    return GROUND_LOST;
}

int session_autobackend(struct session *s)
{
    int at = settings_get_int(SETTING_AUTO_BACKEND, 0);
    if (!s || s->remote || s->running || !s->agent || !s->agent->rate_limit)
        return 0;
    int failed = s->quota_failed;
    int due = s->autobackend_due;
    s->quota_failed = 0;
    s->autobackend_due = 0;
    if (at < 1 || at > 100)
        return 0;
    static const char *const order[] = {"claude", "codex", "grok"};
    int current = -1;
    for (int i = 0; i < 3; i++)
        if (!strcmp(s->backend, order[i]))
            current = i;
    if (current < 0)
        return 0;
    backend_rate_limit limit;
    session_rate_limit(s, &limit);
    if (!failed && !due && !autobackend_ready(s))
        return 0;
    for (int i = current + 1; i < 3; i++) {
        const char *model, *effort;
        models_autobackend(s->backend, session_model_label(s), session_effort(s),
                          order[i], &model, &effort);
        if (switch_backend(s, order[i], model, effort, at)) {
            s->autohandoff = AUTOHANDOFF_IDLE;
            session_warn(s, "auto-backend: switched to %s (%s, %s)", order[i], model, effort);
            usage_poll(s);
            return 1;
        }
    }
    replace(&s->error_note, "auto-backend stopped: no next backend has known quota below the limit");
    if (failed)
        session_warn(s, "auto-backend stopped after quota error: no next backend has known quota below %d%%", at);
    else
        session_warn(s, "auto-backend stopped at %d%%: no next backend has known quota below %d%%",
                     limit.used_percent, at);
    return -1;
}

static int turn_ready(struct session *s, const char *text)
{
    replace(&s->error_note, NULL);
    if ((s->remote || session_reground(s)) && session_autobackend(s) >= 0)
        return 1;
    replace(&s->failed_prompt, text);
    return 0;
}

static void turn_prepare(struct session *s, const char *text)
{
    replace(&s->last_block, NULL);
    sessionpresent_turn_begin(&s->present);
    replace(&s->prompt, text);
    s->started = now_seconds();
    s->heard_at = s->started;
    s->status_at = s->started;
    s->status_heard = s->heard;
    replace(&s->status_last, NULL);
    free(s->turn_log);
    s->turn_log = NULL;
    s->turn_len = 0;
    s->tool_open = 0;
    s->idle_busy = 1;
    s->stall_told = 0;
    s->stall_at = 0;
    s->interrupted = 0;
    s->autobackend_due = 0;
    s->abort_request = 0;
    publish(s, "working");
}

static void continuation_fallback(struct session *s)
{
    s->stall_seen = 0;
    s->stall_told = 0;
    s->stall_at = now_seconds();
    s->heard_at = s->stall_at;
}

static int quota_error(const char *error)
{
    static const char *const phrases[] = {
        "rate_limit", "rate limit", "usage_limit", "usage limit",
        "quota exceeded", "quota exhausted", "insufficient_quota",
        "exceeded your current quota", "quota_exceeded",
        "insufficient credits", "credit limit", "out of credits",
        "you've hit your", "you have hit your", "spend limit"
    };
    if (error)
        for (size_t i = 0; i < sizeof phrases / sizeof *phrases; i++)
            if (strcasestr(error, phrases[i]))
                return 1;
    return 0;
}

static int turn_finish(struct session *s, char *reply, const backend_result *meta,
                       double elapsed)
{
    const backend_result m = *meta;
    s->last_result = m;
    s->interrupted = m.interrupted || s->abort_request;
    s->quota_failed = !s->interrupted && (!reply || m.is_error) &&
        (quota_error(m.subtype) || quota_error(s->agent->last_error(s->agent)) ||
         (m.is_error && quota_error(reply)));
    int continuing = s->continuing;
    s->continuing = 0;
    const char *text = s->prompt ? s->prompt : "";
    s->heard_at = 0;
    sessionpresent_turn_end(&s->present);
    const char *id = s->agent->session_id(s->agent);
    if (id)
        set_id(s, id);

    if (!reply) {
        replace(&s->failed_prompt, continuing ? NULL : text);
        s->idle_busy = 0;
        publish(s, "errored");
        if (continuing)
            continuation_fallback(s);

        if (session_reground(s) == GROUND_MOVED)
            replace(&s->error_note,
                    "the working directory was deleted mid-turn; the session has "
                    "been restarted");
        const char *detail = session_last_error(s);
        if (s->quiet) {
            if (detail)
                fprintf(stderr, "%s: %s\n", s->backend, detail);
            else
                fprintf(stderr, "the %s process stopped responding\n", s->backend);
            return 0;
        }
        sessionpresent_failure(s->backend, detail);
        return 0;
    }

    const char *tail = *reply ? reply : (s->last_block ? s->last_block : "");
    const char *detail = NULL;
    if ((s->quiet && !*tail) || (!s->quiet && !*reply && m.is_error))
        detail = s->agent->last_error(s->agent);
    sessionpresent_turn_result(&s->present, s->backend, reply, s->last_block,
                               detail, &m, s->quiet);

    if (*reply)
        replace(&s->last_reply, reply);
    if (m.is_error) {
        replace(&s->failed_prompt, continuing ? NULL : text);
        if (continuing)
            continuation_fallback(s);
    } else if (!continuing || s->remote) {
        transcript_add(&s->transcript, s->backend, text, reply, m.interrupted);
        replace(&s->failed_prompt, NULL);
    } else {
        replace(&s->failed_prompt, NULL);
    }
    free(reply);

    s->turns++;
    if (!m.is_error || m.interrupted)
        s->saved = 1;
    replace(&s->handoff, NULL);
    double cost_before = s->cost_usd;
    charge_turn(s, &m);
    ledger_add(s, &m, text, s->cost_usd - cost_before);
    s->tokens_in += m.input_tokens + m.cache_read_tokens + m.cache_creation_tokens;
    s->tokens_out += m.output_tokens;
    s->tokens_cached += m.cache_read_tokens;

    const char *quota = quota_name(s);
    if (quota)
        quota_want(quota);

    if (m.context_window > 0)
        s->context_window = m.context_window;
    if (m.context_tokens > 0)
        s->context_tokens = m.context_tokens;

    tab_busy(s, session_idle_busy(s));

    gitinfo_forget();

    if (!s->remote) {
        adopt_title(s);
        remember_model(s);
        remember_window(s);
    }
    if (!s->quiet)
        sessionpresent_footer(elapsed, s->context_tokens, s->context_window,
                              s->cost_usd, m.input_tokens, m.cache_read_tokens,
                              m.cache_creation_tokens, s->title);
    hud_refresh(s);
    return 1;
}

int session_turn(struct session *s, const char *text)
{
    if (!s->agent || s->running || !turn_ready(s, text))
        return 0;

    turn_prepare(s, text);
    struct session *was = session_set_drawing(s);
    if (!s->quiet) {
        sessionpresent_spin(s->backend, spin_effort(s), session_quiet(s),
                            SESSION_QUIET_SECONDS);
        status_begin();
    }

    backend_result meta = {0};
    char *reply = s->agent->ask_ex(s->agent, text, &meta);
    usage_poll(s);
    double elapsed = now_seconds() - s->started;
    session_set_drawing(was);
    if (!s->quiet)
        status_end();

    int ok = turn_finish(s, reply, &meta, elapsed);
    int switched = s->interrupted && !s->autobackend_due ? 0 : session_autobackend(s);
    if (switched > 0)
        return session_turn(s, "continue");
    return switched < 0 ? 0 : ok;
}

static void wake_write(struct session *s)
{
    if (s->wake[1] < 0)
        return;
    char byte = 1;
    ssize_t w = write(s->wake[1], &byte, 1);
    (void)w;
}

static void wake_drain(struct session *s)
{
    if (s->wake[0] < 0)
        return;
    char buf[256];
    while (read(s->wake[0], buf, sizeof buf) > 0)
        ;
}

static int wake_open(struct session *s)
{
    if (s->wake[0] >= 0)
        return 1;
    if (pipe(s->wake) != 0)
        return 0;
    for (int i = 0; i < 2; i++) {
        fcntl(s->wake[i], F_SETFL, fcntl(s->wake[i], F_GETFL, 0) | O_NONBLOCK);
        fcntl(s->wake[i], F_SETFD, FD_CLOEXEC);
    }
    return 1;
}

static void *turn_thread(void *ud)
{
    struct session *s = ud;
    owner = s;

    restart_shield_thread();

    memset(&s->meta, 0, sizeof s->meta);
    s->reply = s->continuing
        ? s->agent->continue_ex(s->agent, &s->meta)
        : s->agent->ask_ex(s->agent, s->asked, &s->meta);
    s->finished = 1;
    wake_write(s);
    return NULL;
}

int session_turn_begin(struct session *s, const char *text)
{
    if (!s || !s->agent || s->running || !wake_open(s) || !turn_ready(s, text))
        return 0;

    turn_prepare(s, text);
    replace(&s->asked, text);
    s->reply = NULL;
    s->finished = 0;
    s->running = 1;

    if (pthread_create(&s->thread, NULL, turn_thread, s) != 0) {
        s->running = 0;
        s->idle_busy = 0;
        replace(&s->failed_prompt, text);
        publish(s, "errored");
        return 0;
    }
    return 1;
}

int session_turn_continue_begin(struct session *s)
{
    if (!s || !s->agent || !s->agent->continue_ex || s->running || !wake_open(s)) {
        if (s)
            continuation_fallback(s);
        return 0;
    }

    int switched = session_autobackend(s);
    if (switched < 0) {
        continuation_fallback(s);
        return 0;
    }
    if (switched > 0)
        return session_turn_begin(s, "continue");
    turn_prepare(s, s->remote ? remote_prompt(s->agent) : NULL);
    replace(&s->asked, NULL);
    s->reply = NULL;
    s->finished = 0;
    s->continuing = 1;
    s->running = 1;

    if (pthread_create(&s->thread, NULL, turn_thread, s) != 0) {
        s->running = 0;
        s->continuing = 0;
        s->idle_busy = 0;
        continuation_fallback(s);
        publish(s, "errored");
        return 0;
    }
    return 1;
}

int session_turn_running(const struct session *s)
{
    return s && s->running;
}

double session_turn_elapsed(const struct session *s)
{
    return s && s->running ? now_seconds() - s->started : 0;
}

void session_interrupt(struct session *s)
{
    if (s && s->running) {
        s->autobackend_due = 0;
        s->abort_request = 1;
    }
}

int session_stop_task(struct session *s, const char *task_id)
{
    if (!s || !s->agent || !s->agent->stop_task)
        return -1;
    return s->agent->stop_task(s->agent, task_id);
}

void session_set_unseen(struct session *s, int on)
{
    on = on ? 1 : 0;
    if (!s || s->unseen == on)
        return;
    s->unseen = on;
    publish(s, session_busy(s) ? "working" : "finished");
}

int session_unseen(const struct session *s) { return s ? s->unseen : 0; }

void session_republish(const struct session *s)
{
    if (s)
        publish(s, session_busy(s) ? "working" : "finished");
}

int session_in_turn(const struct session *s)
{
    if (!s)
        return 0;
    return s->running || (s->agent && s->agent->turn_open && s->agent->turn_open(s->agent));
}

int session_busy(const struct session *s)
{
    if (!s)
        return 0;
    return s->running || session_idle_busy(s);
}

static void drain_events(struct session *s)
{
    struct evcopy *e;
    struct session *was = session_set_drawing(s);
    while ((e = dequeue(s))) {
        rendering_queued_at = e->queued_at;
        render_event(s, &e->ev);
        rendering_queued_at = 0;
        evcopy_free(e);
    }
    image_poll();
    sessionpresent_expire(&s->present, s->quiet);
    session_set_drawing(was);
}

int session_turn_pump(struct session *s)
{
    if (!s || !s->running)
        return 0;

    wake_drain(s);
    drain_events(s);
    name_poll(s);
    usage_poll(s);

    if (!s->finished) {
        autobackend_interrupt(s);
        if (!s->abort_request && session_autohandoff_ready(s)) {
            s->autohandoff = AUTOHANDOFF_DUE;
            s->abort_request = 1;
        }
        side_drain(s);
        status_update_tick(s);
        return 1;
    }

    pthread_join(s->thread, NULL);
    s->running = 0;

    drain_events(s);

    char *reply = s->reply;
    s->reply = NULL;
    turn_finish(s, reply, &s->meta, now_seconds() - s->started);
    return 0;
}

void session_turn_wait(struct session *s)
{
    while (s && s->running) {
        if (!session_turn_pump(s))
            break;
        int fd = session_wake_fd(s);
        if (fd < 0) {
            struct timespec ts = {0, 20 * 1000000L};
            nanosleep(&ts, NULL);
            continue;
        }
        struct pollfd p = {.fd = fd, .events = POLLIN};
        poll(&p, 1, 200);
    }
}

int session_set_remote(struct session *s, const char *target)
{
    replace(&s->remote, target);
    if (s->remote)
        s->name[0] = '\0';
    return s->remote != NULL;
}

const char *session_remote(const struct session *s)
{
    return s ? s->remote : NULL;
}

const char *session_remote_field(const struct session *s, const char *key)
{
    if (!s || !s->remote || !s->agent)
        return NULL;
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(remote_history(s->agent), key));
    return v && *v ? v : NULL;
}

int session_remote_btw(struct session *s, const char *prompt)
{
    return s && s->remote && agent(s) && remote_btw(s->agent, prompt);
}

int session_remote_connected(const struct session *s)
{
    return s && s->remote && remote_connected(s->agent);
}

const char *session_title(const struct session *s)
{
    return s && s->title[0] ? s->title : NULL;
}

const char *session_name(const struct session *s)
{
    return s ? s->name : "";
}

int session_set_name(struct session *s, const char *name)
{
    if (!intercom_name_valid(name) || intercom_name_taken(name, s->chain))
        return 0;
    snprintf(s->name, sizeof s->name, "%s", name);
    chain_set_name(s->chain, s->name);
    publish(s, s->idle_busy ? "working" : "finished");
    return 1;
}

const char *session_model(const struct session *s)
{
    if (s->resolved && *s->resolved)
        return s->resolved;
    if (s->model && *s->model)
        return s->model;
    return "default";
}

const char *session_effort(const struct session *s)
{
    return s->effort && *s->effort ? s->effort : "default";
}

const char *session_saved_model(const char *backend)
{
    return prefs_saved_choice("model", backend);
}

const char *session_saved_effort(const char *backend)
{
    return prefs_saved_choice("effort", backend);
}

static void ledger_add(struct session *s, const backend_result *m, const char *prompt,
                       double cost)
{
    if (s->ledger_n == s->ledger_cap) {
        int cap = s->ledger_cap ? s->ledger_cap * 2 : 32;
        void *grown = realloc(s->ledger, (size_t)cap * sizeof *s->ledger);
        if (!grown)
            return;
        s->ledger = grown;
        s->ledger_cap = cap;
    }
    struct sessionpresent_tokens *t = &s->ledger[s->ledger_n++];
    memset(t, 0, sizeof *t);
    const char *model = session_model_label(s);
    snprintf(t->backend, sizeof t->backend, "%s", s->backend);
    const char *shown = models_short_name(s->backend, model);
    snprintf(t->model, sizeof t->model, "%s", shown ? shown : "");

    const char *body = prompt ? prompt : "";
    if (!strncmp(body, VOICE_PREAMBLE, sizeof VOICE_PREAMBLE - 1))
        body += sizeof VOICE_PREAMBLE - 1;
    size_t at = 0, cap = sizeof t->prompt - sizeof "\xe2\x80\xa6";
    while (*body == ' ' || *body == '\n' || *body == '\t' || *body == '\r')
        body++;
    for (; *body && at < cap; body++) {
        char c = *body == '\n' || *body == '\t' || *body == '\r' ? ' ' : *body;
        if (c == ' ' && at && t->prompt[at - 1] == ' ')
            continue;
        t->prompt[at++] = c;
    }
    if (*body) {
        if (((unsigned char)*body & 0xC0) == 0x80) {
            while (at > 0 && ((unsigned char)t->prompt[at - 1] & 0xC0) == 0x80)
                at--;
            if (at > 0)
                at--;
        }
        memcpy(t->prompt + at, "\xe2\x80\xa6", 3);
        at += 3;
    }
    t->prompt[at] = '\0';
    t->fresh = m->input_tokens;
    t->cache_write = m->cache_creation_tokens;
    t->cache_read = m->cache_read_tokens;
    t->output = m->output_tokens;
    t->context = m->context_tokens;
    t->cost = cost;
    struct model_rates rates;
    if (models_rates(s->backend, model, &rates)) {
        t->rate_input = rates.input;
        t->rate_cache_write = rates.cache_write;
        t->rate_cache_read = rates.cache_read;
        t->rate_output = rates.output;
    }
}

void session_tokenomics(const struct session *s)
{
    sessionpresent_tokenomics(s->ledger, s->ledger_n, s->context_window);
}

static void charge_turn(struct session *s, const backend_result *m)
{
    if (m->cost_usd > 0) {
        s->cost_usd = m->cost_usd;
        return;
    }

    struct model_rates rates;
    if (!models_rates(s->backend, session_model_label(s), &rates))
        return;

    s->cost_usd += ((double)m->input_tokens * rates.input +
                    (double)m->cache_read_tokens * rates.cache_read +
                    (double)m->cache_creation_tokens * rates.cache_write +
                    (double)m->output_tokens * rates.output) / 1e6;
}

static const char *backend_model(const struct session *s)
{
    if (!s->agent || !s->agent->model)
        return NULL;
    const char *got = s->agent->model(s->agent);
    return got && *got ? got : NULL;
}

static void remember_model(const struct session *s)
{
    const char *id = s->resolved && *s->resolved ? s->resolved : backend_model(s);
    prefs_remember_resolved_model(s->backend, s->model, id);

    if ((!strcmp(s->backend, "pi") || !strcmp(s->backend, "core")) &&
        (!s->model || !*s->model) && id && *id)
        prefs_remember_choice("model", s->backend, id);
}

const char *session_model_label(const struct session *s)
{
    if (s->resolved && *s->resolved)
        return s->resolved;

    if (s->agent && s->agent->model) {
        const char *got = s->agent->model(s->agent);
        if (got && *got)
            return got;
    }
    const char *cached = prefs_resolved_model(s->backend, s->model);
    if (cached)
        return cached;
    if (s->model && *s->model)
        return s->model;
    return "default";
}

static void remember_window(const struct session *s)
{
    prefs_remember_window(s->backend, session_model_label(s), s->context_window);
}

long session_context_window(const struct session *s)
{
    if (s->context_window > 0)
        return s->context_window;
    if (s->agent && s->agent->usage) {
        long used = 0, window = 0;
        s->agent->usage(s->agent, &used, &window);
        if (window > 0)
            return window;
    }
    return prefs_window(s->backend, session_model_label(s));
}

const char *session_effort_label(const struct session *s)
{
    if (s->remote || !session_can_set_effort(s))
        return NULL;
    const char *effort = spin_effort(s);
    return effort_is_off(effort) || !strcmp(effort, "default") ? NULL : effort;
}

int session_can_set_effort(const struct session *s)
{
    return s->agent && (s->agent->caps & BACKEND_CAP_EFFORT);
}

const char *session_id(const struct session *s) { return s->id[0] ? s->id : NULL; }

const char *session_addr(const struct session *s) { return s && s->addr ? s->addr : NULL; }

int session_can_resume(const struct session *s)
{
    return s->agent && (s->agent->caps & BACKEND_CAP_RESUME);
}

char *session_side_context(const struct session *s)
{
    const char *id = session_id(s);
    if (s->remote || (!s->memory && id && *id && session_can_resume(s)))
        return NULL;

    char *view = s->memory && s->agent && s->agent->memory_view
                     ? s->agent->memory_view(s->agent) : NULL;
    const char *tail = view;
    if (view && strlen(view) > SIDE_VIEW_MAX) {
        tail = view + strlen(view) - SIDE_VIEW_MAX;
        const char *nl = strchr(tail, '\n');
        if (nl)
            tail = nl + 1;
    }

    const char *prompt = s->running || s->turn_log ? s->prompt : NULL;
    size_t want = 512 + (tail ? strlen(tail) : 0) + (prompt ? strlen(prompt) : 0) +
                  s->turn_len;
    char *out = malloc(want);
    if (!out) {
        free(view);
        return NULL;
    }
    size_t n = 0;
    if (tail && *tail)
        n += (size_t)snprintf(out + n, want - n,
                              "The most recent part of the conversation, one summary "
                              "line per message, oldest first:\n\n%s\n\n", tail);
    if (prompt && *prompt)
        n += (size_t)snprintf(out + n, want - n, "The user's message this turn:\n\n%s\n\n",
                              prompt);
    if (s->turn_log && *s->turn_log)
        n += (size_t)snprintf(out + n, want - n,
                              "What the agent has done this turn so far, oldest first:\n\n%s\n",
                              s->turn_log);
    free(view);
    if (!n) {
        free(out);
        return NULL;
    }
    return out;
}

int session_restorable(const struct session *s)
{
    const char *id = session_id(s);
    return s->remote || s->memory || (id && *id && session_can_resume(s));
}

int session_subagent(const struct session *s) { return s && s->subagent; }

const char *session_cwd(const struct session *s)
{
    const char *remote = session_remote_field(s, "cwd");
    return remote ? remote : s->cwd;
}
const char *session_workdir(const struct session *s)
{
    return s->workdir ? s->workdir : session_cwd(s);
}
const char *session_backend(const struct session *s)
{
    const char *remote = session_remote_field(s, "backend");
    return remote ? remote : s->backend;
}

void session_address(const struct session *s, char *out, size_t size)
{
    if (s && s->remote)
        snprintf(out, size, "%s", s->remote);
    else
        snprintf(out, size, "@%s", s ? s->name : "");
}

int session_tail_mark(const struct session *s, const char *bot,
                      struct grokbottail_mark *out)
{
    if (!s || !bot || strcmp(s->tail_bot, bot))
        return 0;
    *out = s->tail_mark;
    return 1;
}

void session_set_tail_mark(struct session *s, const char *bot,
                           const struct grokbottail_mark *mark)
{
    if (!s || !bot)
        return;
    snprintf(s->tail_bot, sizeof s->tail_bot, "%s", bot);
    s->tail_mark = *mark;
}

struct vncinset *session_inset(struct session *s, int create)
{
    if (!s)
        return NULL;
    if (!s->inset && create)
        s->inset = vncinset_new(s->model);
    return s->inset;
}

int session_argv(const struct session *s, char **out, int max, unsigned what)
{
    if (s && s->remote) {
        int n = 0;
        if ((what & SESSION_ARGV_CWD) && s->cwd && n + 2 <= max) {
            out[n++] = (char *)"-C";
            out[n++] = s->cwd;
        }
        if (n + 2 <= max) {
            out[n++] = (char *)"--attach";
            out[n++] = s->remote;
        }
        if (n < max)
            out[n] = NULL;
        return n;
    }
    const char *id = session_id(s);
    int resume = (what & SESSION_ARGV_RESUME) && id && session_can_resume(s);
    int n = scrap_argv(out, max, resume ? what : (what & ~SESSION_ARGV_RESUME),
                       NULL, session_backend(s), session_cwd(s), session_model(s),
                       session_effort(s), id,
                       (what & SESSION_ARGV_SAFE) && s && !s->customizations, NULL);
    if (s && s->memory && n + 1 < max) {
        out[n++] = (char *)"--memory";
        out[n] = NULL;
    }
    if (s && !resume && (what & SESSION_ARGV_NAME) && s->name[0] && n + 2 < max) {
        out[n++] = (char *)"--name";
        out[n++] = (char *)s->name;
        out[n] = NULL;
        if (s->chain[0] && n + 2 < max) {
            out[n++] = (char *)"--chain";
            out[n++] = (char *)s->chain;
            out[n] = NULL;
        }
    }
    return n;
}

int session_last_interrupted(const struct session *s) { return s ? s->interrupted : 0; }

const char *session_last_error(const struct session *s)
{
    if (s && s->error_note)
        return s->error_note;
    return s && s->agent ? s->agent->last_error(s->agent) : NULL;
}
const char *session_last_reply(const struct session *s) { return s->last_reply; }
const char *session_last_block(const struct session *s) { return s->last_block; }
int session_ask_open(const struct session *s) { return s && s->ask_open; }
void session_set_ask_open(struct session *s, int on) { if (s) s->ask_open = on; }
const char *session_prompt(const struct session *s) { return s ? s->prompt : NULL; }
double session_turn_started(const struct session *s) { return s ? s->started : 0; }
const char *session_failed_prompt(const struct session *s)
{
    return s ? s->failed_prompt : NULL;
}

long session_tokens_in(const struct session *s)
{
    return s ? s->tokens_in : 0;
}

long session_tokens_out(const struct session *s)
{
    return s ? s->tokens_out : 0;
}

long session_tokens_cached(const struct session *s)
{
    return s ? s->tokens_cached : 0;
}

int session_context_percent(const struct session *s)
{
    long used = s->context_tokens, window = s->context_window;

    if (s->agent && s->agent->usage) {
        long live_used = 0, live_window = 0;
        s->agent->usage(s->agent, &live_used, &live_window);
        if (live_used > 0)
            used = live_used;
        if (live_window > 0)
            window = live_window;
    }
    if (window <= 0)
        window = session_context_window(s);
    if (used <= 0 || window <= 0)
        return -1;
    int percent = (int)(used * 100 / window);
    return percent > 100 ? 100 : percent;
}

int session_autohandoff(const struct session *s)
{
    return s->autohandoff;
}

void session_autohandoff_set(struct session *s, int state)
{
    if (state == AUTOHANDOFF_SEEDED)
        s->autohandoffs++;
    else if (state == AUTOHANDOFF_IDLE)
        s->autohandoffs = 0;
    s->autohandoff = state;
}

int session_autohandoff_ready(const struct session *s)
{
    int at = settings_get_int(SETTING_AUTO_HANDOFF, 0);
    if (at <= 0 || s->remote || s->autohandoffs >= AUTOHANDOFF_RUN_MAX)
        return 0;
    if (s->autohandoff != AUTOHANDOFF_IDLE && s->autohandoff != AUTOHANDOFF_SEEDED)
        return 0;
    return session_context_percent(s) >= at;
}

static const char *auth_description(const struct session *s)
{
    const char *source = s->agent ? s->agent->auth_source(s->agent) : NULL;
    if (!source)
        return NULL;
    if (strcmp(source, "none") == 0)
        return "subscription login";
    return source;
}

void session_spin_word(const struct session *s)
{
    if (!s)
        return;
    sessionpresent_spin(s->backend, spin_effort(s), session_quiet(s),
                        SESSION_QUIET_SECONDS);
}

void session_report(const struct session *s)
{
    const char *auth = auth_description(s);
    char parent[200] = "";
    if (s->id[0]) {
        char up[128];
        if (parent_of(s->id, up, sizeof up)) {
            if (!title_lookup(up, parent, sizeof parent))
                snprintf(parent, sizeof parent, "%s", up);
        }
    }

    char chat[160] = "";
    const char *chats[] = {tg_label(), relay_label(), im_label()};
    size_t      nchat = 0;
    for (size_t i = 0; i < sizeof chats / sizeof *chats; i++)
        if (chats[i] && nchat < sizeof chat)
            nchat += (size_t)snprintf(chat + nchat, sizeof chat - nchat, "%s%s",
                                      nchat ? ", " : "", chats[i]);

    struct sessionpresent_report r = {
        .backend = s->backend,
        .model = session_model(s),
        .effort = session_can_set_effort(s) ? session_effort(s) : NULL,
        .auth = auth,
        .permission = !strcmp(s->backend, "claude") ? session_permission(s) : NULL,
        .chat = chat[0] ? chat : NULL,
        .id = s->id[0] ? s->id : NULL,
        .parent = parent[0] ? parent : NULL,
        .cwd = s->cwd,
        .customizations = s->customizations,
        .compact = s->compact,
        .turns = s->turns,
        .context_tokens = s->context_tokens,
        .context_window = s->context_window,
        .tokens_in = s->tokens_in,
        .tokens_out = s->tokens_out,
        .tokens_cached = s->tokens_cached,
        .cost = s->cost_usd,
    };
    sessionpresent_report(&r);
}
