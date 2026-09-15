#include "session.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "agenttabs.h"
#include "block.h"
#include "app.h"
#include "gitinfo.h"
#include "hooks.h"
#include "hud.h"
#include "image.h"
#include "livelist.h"
#include "restart.h"
#include "models.h"
#include "parent.h"
#include "sessionload.h"
#include "sessionprefs.h"
#include "sessionpresent.h"
#include "sessionview.h"
#include "viewport.h"
#include "settings.h"
#include "sidechannel.h"
#include "status.h"
#include "tasks.h"
#include "title.h"
#include "tg.h"
#include "relay.h"
#include "transcript.h"
#include "tty.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "text.h"
#include "vendor/cJSON.h"
#include "voice.h"

struct session {
    Backend *agent;
    char    *backend;
    char    *cwd;
    char    *workdir;
    char    *model;
    char    *effort;
    char    *resolved;
    char     id[128];
    char     title[128];
    char     stale_title[128];
    char     held_title[128];
    int      announce_title;
    int      retitle;
    int      named;
    double   named_at;
    char    *prompt;
    char    *last_reply;
    char    *failed_prompt;
    struct transcript transcript;
    char    *last_block;
    int      turns;
    double   cost_usd;
    long     tokens_in, tokens_out;
    /* The cache-read share of tokens_in. Broken out because it is billed at a
       fraction of fresh input, so the two move very differently. */
    long     tokens_cached;
    long     context_tokens;
    long     context_window;
    int      quiet;
    char    *system_extra;
    session_event_fn observer;
    void    *observer_ud;
    char     recent[SESSION_RECENT][SESSION_RECENT_MAX];
    int      recent_n;
    int    (*abort_hook)(void *ud);
    void    *abort_ud;
    int      skip_naming;
    char     parent[128];
    int      thinking;
    int      compact;
    int      customizations;
    int      no_browser_login;
    int      fork_session;
    char    *permission;
    char    *error_note;
    int      idle_busy;
    int      trust_requested;
    struct tasktab     tasks;
    const struct task *task_change;
    int      task_repeat;
    unsigned long spoke; /* events other than task reports, for stall_watch */
    double   work_at;    /* when the outstanding background work started */
    double   stall_at;   /* when the last outstanding task went quiet */
    int      stall_seen; /* work was outstanding at the previous pump */
    int      stall_told;
    volatile double heard_at;
    volatile int    tool_open;
    int      interrupted;
    int      unseen;

    pthread_t       thread;
    int             running;
    volatile int    connecting;
    pthread_t       start_th;
    int             start_threaded;
    volatile int    start_finished;
    int             start_ok;
    volatile int    finished;
    volatile int    abort_request;
    pthread_mutex_t lock;
    struct evcopy  *head, *tail;
    int             wake[2];
    char           *asked;
    char           *reply;
    backend_result  meta;
    double          started;
    double          status_at;   /* when the last status update was asked for */
    char           *status_last; /* its answer, so the next one adds to it */
    int             status_open;

    struct sessionpresent present;
};

struct evcopy {
    backend_event  ev;
    char          *text, *name, *input_json, *arg, *diff, *id, *parent;
    double         queued_at;
    struct evcopy *next;
};

/* The enqueue time of the event being rendered, so a listener can report how
   long the event sat between the backend thread and the main loop. */
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
static void retire(Backend *b);
static int dir_alive(const char *path);
static int ground_target(const char *gone, char *out, size_t size);

/* a line cut to fit can end mid-sequence */
static void clip_utf8(char *s)
{
    size_t i = 0, whole = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        size_t len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 :
                     (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
        for (size_t j = 1; j < len; j++)
            if (((unsigned char)s[i + j] & 0xc0) != 0x80) {
                s[whole] = '\0';
                return;
            }
        i += len;
        whole = i;
    }
    s[whole] = '\0';
}

/* What the session itself is doing. A subagent's work runs under a call that is
   already recorded, so counting it here would report the wrong actor. */
static void note_recent(struct session *s, const backend_event *ev)
{
    char line[SESSION_RECENT_MAX];

    if (ev->parent && *ev->parent)
        return;

    if (ev->kind == BACKEND_EV_TOOL) {
        char what[1024] = "";
        view_tool_argument(ev, s->cwd, what, sizeof what);
        snprintf(line, sizeof line, "%s%s%s", ev->name ? ev->name : "tool",
                 what[0] ? "  " : "", what);
    } else if (ev->kind == BACKEND_EV_ASSISTANT && ev->text && *ev->text) {
        snprintf(line, sizeof line, "%s", ev->text);
    } else {
        return;
    }

    for (char *p = line; *p; p++)
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    clip_utf8(line);

    snprintf(s->recent[s->recent_n % SESSION_RECENT], SESSION_RECENT_MAX, "%s", line);
    s->recent_n++;
}

static void render_event(struct session *s, const backend_event *ev)
{
    /* Anything but a task report is the backend talking, which is what tells a
       stalled session from one whose work woke it. */
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

    note_recent(s, ev);

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

/* The turn's stream is the only sign the backend is still talking to its
 * provider: a CLI that has lost the network keeps its pipe open and retries in
 * silence. */
static void heard(struct session *s, const backend_event *ev)
{
    s->heard_at = now_seconds();
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
    if (!s || !s->agent || !s->agent->idle_fd || s->quiet)
        return -1;
    return s->agent->idle_fd(s->agent);
}

static const char *tabs_provider(const struct session *s)
{
    static char buf[32];
    const char *model = NULL;

    if (!s || !s->backend || strcmp(s->backend, "pi") != 0)
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
    agenttabs_publish(s, session_backend(s), status, tabs_provider(s));
    livelist_publish(s, status);
}

static void tab_busy(struct session *s, int busy)
{
    busy = busy ? 1 : 0;
    if (busy == s->idle_busy)
        return;
    s->idle_busy = busy;
    publish(s, busy ? "working" : "finished");
}

static void name_poll(struct session *s);

/* Background work outstanding with no turn in flight, and how long it has been
   there. A backend that counts its own is the authority: the table can be left
   holding a task whose end was never reported, which is the thing being watched
   for. */
int session_work_count(const struct session *s)
{
    if (!s || !s->agent)
        return 0;
    return s->agent->busy ? session_idle_busy(s) : tasks_pending(&s->tasks);
}

/* Work that outlives its turn is only over when the backend says so: the task
   ends, its result wakes the model, and that turn is the answer. A task torn
   down without one leaves the session at a prompt indistinguishable from an
   answered one, with nothing left to resume it. Note the moment the last one
   goes quiet so session_stalled() can time it. */
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
        /* the turn the work woke: its answer is what resumes the session */
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

/* Whether a stall is being timed, for a caller that has to come back for it:
   nothing else will wake the loop once the work has gone quiet. */
int session_stall_armed(const struct session *s)
{
    return s && s->stall_at && !s->stall_told &&
           settings_get_int(SETTING_TASK_STALL, TASK_STALL_DEFAULT) > 0;
}

/* Nonzero once per stall, for the caller that can do something about it. */
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

    if (!s->agent->idle_pump || s->quiet)
        return 0;

    if (!s->idle_busy) {
        sessionpresent_break(&s->present);
    }
    struct session *was = session_set_drawing(s);
    image_poll();
    unsigned long before = s->spoke;
    int busy = s->agent->idle_pump(s->agent) ? 1 : 0;
    sessionpresent_expire(&s->present, s->quiet);
    session_set_drawing(was);

    /* a turn still open, or one that opened and closed inside this pump */
    stall_watch(s, busy || s->spoke != before);
    tab_busy(s, busy);
    return busy;
}

int session_idle_busy(const struct session *s)
{
    if (!s || !s->agent || !s->agent->busy)
        return 0;
    return s->agent->busy(s->agent) ? 1 : 0;
}

static session_key_fn typeahead;
static void          *typeahead_ud;

void session_set_typeahead(session_key_fn fn, void *ud)
{
    typeahead = fn;
    typeahead_ud = ud;
}


static void set_id(struct session *s, const char *id);

static void usage_poll(struct session *s)
{
    if (!s || !s->agent || !s->agent->rate_limit)
        return;
    backend_rate_limit limit = {0};
    s->agent->rate_limit(s->agent, &limit);
    if (limit.available) {
        agenttabs_usage(s, limit.used_percent, limit.resets_at, limit.window_minutes);

    }
}

static int adopt_title(struct session *s)
{
    char found[sizeof s->title];
    if (!title_lookup(s->id, found, sizeof found))
        return 0;
    if (s->stale_title[0] && !strcmp(found, s->stale_title))
        return 0;
    s->stale_title[0] = '\0';
    snprintf(s->title, sizeof s->title, "%s", found);
    status_set_note(s->title);
    publish(s, s->idle_busy ? "working" : "finished");
    if (s->announce_title) {
        s->announce_title = 0;
        viewport_item_begin(VIEWPORT_ROWS(1, 1));
        ui_note("renamed to %s", s->title);
        viewport_item_end();
        ui_flush();
    }
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

    /* a rename that landed before the conversation had an id */
    if (s->id[0] && s->held_title[0]) {
        title_set(s->id, s->held_title);
        s->held_title[0] = '\0';
        s->named = 1;
        s->retitle = 0;
        adopt_title(s);
    }

    if (s->title[0])
        return;

    if (!s->named) {
        if (s->skip_naming || !s->prompt)
            return;
        const char *model = s->resolved && *s->resolved ? s->resolved : s->model;
        title_request(s->id, s->backend, model, s->cwd, s->prompt, s->last_reply);
        s->named = 1;
        s->retitle = 0;
        s->named_at = now_seconds();
        return;
    }

    double now = now_seconds();
    if (now - s->named_at < 1.0)
        return;
    s->named_at = now;

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
    case SESSION_RENAME_NO_SOURCE:
        return "nothing to name it from yet — the model names it from a turn";
    case SESSION_RENAME_OK:
        break;
    }
    return "renamed";
}

enum session_rename session_rename(struct session *s, const char *name)
{
    if (!s)
        return SESSION_RENAME_NO_ID;

    /* the id arrives when the first turn ends and the name is filed under it;
       until then the session wears it and name_poll files it */
    if (!s->id[0] && name && *name) {
        if (!title_clean(name, s->held_title, sizeof s->held_title))
            return SESSION_RENAME_BAD_NAME;
        snprintf(s->title, sizeof s->title, "%s", s->held_title);
        s->stale_title[0] = '\0';
        s->named = 1;
        s->retitle = 0;
        status_set_note(s->title);
        publish(s, s->idle_busy ? "working" : "finished");
        return SESSION_RENAME_OK;
    }

    if (!s->id[0])
        return SESSION_RENAME_NO_ID;

    if (name && *name) {
        if (!title_set(s->id, name))
            return SESSION_RENAME_BAD_NAME;
        s->stale_title[0] = '\0';
        if (!adopt_title(s))
            return SESSION_RENAME_NO_STORE;
        s->named = 1;
        s->retitle = 0;
        return SESSION_RENAME_OK;
    }

    if (!s->prompt)
        return SESSION_RENAME_NO_SOURCE;

    const char *model = s->resolved && *s->resolved ? s->resolved : s->model;
    title_request(s->id, s->backend, model, s->cwd, s->prompt, s->last_reply);
    snprintf(s->stale_title, sizeof s->stale_title, "%s", s->title);
    s->title[0] = '\0';
    s->named = 1;
    s->retitle = 0;
    s->named_at = now_seconds();
    s->announce_title = 1;
    status_set_note(NULL);
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

/* A tool the backend is still running explains any amount of silence; nothing
 * else does. */
double session_quiet(const struct session *s)
{
    if (!s || !s->heard_at || s->tool_open)
        return 0;
    double quiet = now_seconds() - s->heard_at;
    return quiet > 0 ? quiet : 0;
}

int session_poll_input(void)
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
    if (answer)
        replace(&s->status_last, answer);
}

/* Every status interval of a running turn, fork a side turn that says what the
   agent has done since the last update. */
static void status_update_tick(struct session *s)
{
    if (!s || !s->running || s->status_open)
        return;
    int every = settings_get_int(SETTING_STATUS_INTERVAL, STATUS_INTERVAL_DEFAULT);
    if (every <= 0 || now_seconds() - s->status_at < every)
        return;
    s->status_at = now_seconds();
    if (sidechannel_status(s, s->status_last, status_update_done, s))
        s->status_open = 1;
}

/* The session whose work this thread is running: set on every thread but the
   window's own, so the abort check knows not to draw or read from it. */
static __thread struct session *owner;

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
    interrupt |= relay_poll(live);
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
    tasks_reset(&s->tasks, s->backend);
    return s;
}

void session_replay(struct session *s)
{
    block_cleared();
    hud_print(s);
    if (!s)
        return;
    if (s->id[0] && sessionload_into(s))
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
        s->abort_request = 1;
        pthread_join(s->thread, NULL);
        s->running = 0;
        free(s->reply);
    }
    queue_drop(s);
    pthread_mutex_destroy(&s->lock);
    for (int i = 0; i < 2; i++)
        if (s->wake[i] >= 0)
            close(s->wake[i]);
    free(s->asked);
    retire(s->agent);
    free(s->backend);
    free(s->cwd);
    free(s->workdir);
    free(s->model);
    free(s->effort);
    free(s->resolved);
    free(s->last_reply);
    free(s->failed_prompt);
    free(s->last_block);
    sessionpresent_free(&s->present);
    free(s->prompt);
    free(s->status_last);
    free(s->permission);
    free(s->error_note);
    free(s->system_extra);
    transcript_free(&s->transcript);
    if (live == s)
        live = NULL;
    free(s);
}

/* Path of the shunt plugin, or NULL when the setting is off or there is no
   config dir. The plugin's hooks only run when customizations are on, since
   --safe-mode loads a plugin but will not execute its hooks. */
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

static char *on_hook(void *ud, int hook, const char *tool, const char *input_json)
{
    (void)ud;
    return hooks_context(hook, tool, input_json);
}

static Backend *agent(struct session *s)
{
    if (s->agent)
        return s->agent;
    backend_opts o = {0};
    o.name = s->backend;
    o.cwd = s->cwd;
    o.model = s->model;
    o.effort = s->effort;
    o.allow_customizations = s->customizations;
    o.permission_mode = s->permission;
    o.fork_session = s->fork_session;
    o.no_browser_login = s->no_browser_login;
    o.chrome = settings_get_int(SETTING_CHROME, 0);
    o.plugin_dir = shunt_plugin_dir(s);
    backend_hook hooks[HOOKS_MAX];
    hooks_load();
    o.hooks = hooks;
    o.hook_count = hooks_backend(hooks, HOOKS_MAX);

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

    char *joined = NULL;
    if (note && s->system_extra) {
        size_t n = strlen(note) + strlen(s->system_extra) + 3;
        if ((joined = malloc(n)))
            snprintf(joined, n, "%s\n\n%s", note, s->system_extra);
    }
    o.system = joined ? joined : s->system_extra ? s->system_extra : note;
    s->agent = backend_open_ex(&o);
    free(joined);
    if (s->agent) {
        s->agent->set_event_cb(s->agent, on_event, s);
        s->agent->set_abort_check(s->agent, abort_check);
        if (s->agent->set_hook_cb)
            s->agent->set_hook_cb(s->agent, on_hook, s);
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

/* Closing a backend stops its child. Do that on another thread in case close
   joins a reader that would deadlock here, then join so quit reaps the CLI
   before the process exits and closes its stdout pipe. */
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

int session_switch_backend(struct session *s, const char *backend)
{
    if (!s || !backend || !*backend || s->running)
        return 0;
    if (strcmp(s->backend, backend) == 0)
        return 1;

    struct transcript disk = {0};
    const struct transcript *src = &s->transcript;
    if (s->id[0] && sessionload_fill(&disk, s->backend, s->cwd, s->id))
        src = &disk;
    char *handoff = transcript_handoff(src, 128 * 1024, s->id[0] ? s->id : NULL);
    transcript_free(&disk);
    backend_opts o = {0};
    o.name = backend;
    o.system = handoff;
    o.cwd = s->cwd;
    o.allow_customizations = s->customizations;
    o.permission_mode = s->permission;
    o.no_browser_login = s->no_browser_login;
    o.chrome = settings_get_int(SETTING_CHROME, 0);
    o.plugin_dir = shunt_plugin_dir(s);
    backend_hook hooks[HOOKS_MAX];
    hooks_load();
    o.hooks = hooks;
    o.hook_count = hooks_backend(hooks, HOOKS_MAX);
    Backend *replacement = backend_open_ex(&o);
    free(handoff);
    if (!replacement)
        return 0;
    if (replacement->set_hook_cb)
        replacement->set_hook_cb(replacement, on_hook, s);

    if (!replacement->start(replacement, NULL)) {
        replacement->close(replacement);
        return 0;
    }
    replacement->set_event_cb(replacement, on_event, s);
    replacement->set_abort_check(replacement, abort_check);

    Backend *previous = s->agent;
    s->agent = replacement;
    replace(&s->backend, backend);
    replace(&s->model, NULL);
    replace(&s->effort, NULL);
    replace(&s->resolved, NULL);
    s->id[0] = '\0';
    const char *id = replacement->session_id(replacement);
    if (id) {
        snprintf(s->id, sizeof s->id, "%s", id);
        agenttabs_forget_hook(id);
    }
    s->turns = 0;
    s->cost_usd = 0;
    s->tokens_in = s->tokens_out = s->tokens_cached = 0;
    s->context_tokens = 0;
    s->context_window = 0;
    retire(previous);
    return 1;
}

void session_set_quiet(struct session *s, int quiet) { s->quiet = quiet; }

const struct transcript *session_transcript(const struct session *s)
{
    return s ? &s->transcript : NULL;
}

int session_recent_seq(const struct session *s)
{
    return s ? s->recent_n : 0;
}

int session_recent(const struct session *s, const char **out, int max)
{
    if (!s || max <= 0)
        return 0;
    int have = s->recent_n < SESSION_RECENT ? s->recent_n : SESSION_RECENT;
    if (have > max)
        have = max;

    int n = 0;
    for (int i = have; i > 0; i--)
        out[n++] = s->recent[(s->recent_n - i) % SESSION_RECENT];
    return n;
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

void session_set_naming(struct session *s, int on) { s->skip_naming = !on; }

void session_set_parent(struct session *s, const char *parent_id)
{
    snprintf(s->parent, sizeof s->parent, "%s", parent_id ? parent_id : "");
    if (s->id[0])
        parent_set(s->id, s->parent);
}

void session_set_thinking(struct session *s, int on) { s->thinking = on; }

int session_thinking(const struct session *s) { return s->thinking; }

void session_set_compact(struct session *s, int on) { s->compact = on; }

int session_compact(const struct session *s) { return s->compact; }

void session_set_customizations(struct session *s, int on) { s->customizations = on; }
void session_set_browser_login(struct session *s, int on) { s->no_browser_login = !on; }

void session_set_fork(struct session *s, int on) { s->fork_session = on; }

static void set_id(struct session *s, const char *id)
{
    int changed = strcmp(s->id, id) != 0;
    snprintf(s->id, sizeof s->id, "%s", id);
    if (changed) {
        if (s->parent[0])
            parent_set(s->id, s->parent);
        if (s->held_title[0]) {
            title_set(s->id, s->held_title);
            s->held_title[0] = '\0';
            s->named = 1;
            s->retitle = 0;
        } else {
            s->title[0] = '\0';
            s->stale_title[0] = '\0';
            s->announce_title = 0;
            s->named = 0;
        }
        if (!s->retitle)
            title_lookup(s->id, s->title, sizeof s->title);
        status_set_note(s->title);
        publish(s, s->idle_busy ? "working" : "finished");
    }
    agenttabs_forget_hook(id);
}

/* A child is handed its directory, model, effort and permission mode on its
   command line, so anything that changes one of them starts a replacement
   rather than telling the running child and restarting it. The session keeps
   the old backend until the new one is up, and lets go of it on a thread. */
static int restart(struct session *s, const char *resume_id)
{
    if (s->running)
        return 0;

    Backend *previous = s->agent;
    s->agent = NULL;

    Backend *b = agent(s);
    if (!b || !b->start(b, resume_id)) {
        if (b) {
            b->set_event_cb(b, NULL, NULL);
            b->set_abort_check(b, NULL);
            b->close(b);
        }
        s->agent = previous;
        return 0;
    }
    retire(previous);
    tasks_reset(&s->tasks, s->backend);
    s->stall_seen = s->stall_told = 0;
    s->stall_at = 0;

    if (resume_id && resume_id != s->id)
        set_id(s, resume_id);

    const char *id = b->session_id(b);
    if (id)
        set_id(s, id);
    return 1;
}

static int connect_agent(struct session *s)
{
    char next[4096];
    if (!dir_alive(s->cwd) && ground_target(s->cwd, next, sizeof next))
        replace(&s->cwd, next);
    return restart(s, s->id[0] ? s->id : NULL);
}

int session_start(struct session *s)
{
    if (!connect_agent(s))
        return 0;
    publish(s, "finished");
    return 1;
}

/* a CLI takes seconds to boot and resume, so the sessions a window comes back
   with connect at once, off the main thread: their events queue, and the live
   list is written back here when each is collected. */
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

    /* a backend can poll the abort check all through its handshake, and that
       check draws and reads the terminal. Only the thread the window runs on
       may do that: claim the session so this one answers and nothing else. */
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
    return restart(s, s->id[0] ? s->id : NULL);
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
    {"auto", "approves the safe calls, refuses the rest"},
    {"acceptEdits", "edits without asking, refuses the rest"},
    {"dontAsk", "refuses anything that would ask"},
    {"manual", "refuses everything not pre-allowed"},
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
    if (restart(s, s->id[0] ? s->id : NULL)) {
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

void session_adopt_id(struct session *s, const char *id)
{
    if (s && id && *id)
        set_id(s, id);
}

#define RESET_BLOCK   (1 << 0)
#define RESET_WORKDIR (1 << 1)

/* what a conversation counted, said and was called does not carry into the
   next one */
static void reset_turns(struct session *s, int flags)
{
    s->turns = 0;
    s->cost_usd = 0;
    s->tokens_in = s->tokens_out = s->tokens_cached = 0;
    s->context_tokens = 0;
    if (flags & RESET_WORKDIR)
        replace(&s->workdir, NULL);
    replace(&s->last_reply, NULL);
    replace(&s->failed_prompt, NULL);
    if (flags & RESET_BLOCK)
        replace(&s->last_block, NULL);
    transcript_clear(&s->transcript);
}

int session_resume(struct session *s, const char *id)
{
    if (!restart(s, id))
        return 0;
    reset_turns(s, 0);
    return 1;
}

/* A directory change is a conversation of its own. */
static void started_over(struct session *s)
{
    reset_turns(s, RESET_BLOCK | RESET_WORKDIR);
    s->title[0] = '\0';
    s->stale_title[0] = '\0';
    s->held_title[0] = '\0';
    s->announce_title = 0;
    s->retitle = 1;
    s->named = 0;
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

    return session_retarget(s, s->model, s->effort, path);
}

/* Setting a model, an effort and a directory one at a time restarts the child
   for each. This asks for all three at once: one replacement, started with
   what it needs on its command line. */
int session_retarget(struct session *s, const char *model, const char *effort,
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

    if (!restart(s, moved || !s->id[0] ? NULL : s->id)) {
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
    if (moved)
        started_over(s);
    publish(s, s->idle_busy ? "working" : "finished");
    return 1;
}

int session_clear(struct session *s)
{
    if (!s->agent || !s->agent->reset(s->agent))
        return 0;
    reset_turns(s, RESET_BLOCK);
    tasks_reset(&s->tasks, s->backend);

    s->title[0] = '\0';
    s->stale_title[0] = '\0';
    s->held_title[0] = '\0';
    s->announce_title = 0;
    s->retitle = 1;
    s->named = 0;
    status_set_note(NULL);

    const char *id = s->agent->session_id(s->agent);
    if (id)
        set_id(s, id);
    else
        s->id[0] = '\0';
    return 1;
}

static void update_title(struct session *s)
{
    if (!s->id[0] || s->title[0])
        return;
    if (!s->named) {
        name_poll(s);
        return;
    }
    adopt_title(s);
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
        if (restart(s, s->id[0] ? s->id : NULL)) {
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
                session_warn(s, "%s is gone — restarted in %s with a fresh context",
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

static int turn_ready(struct session *s, const char *text)
{
    replace(&s->error_note, NULL);
    if (session_reground(s))
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
    replace(&s->status_last, NULL);
    s->tool_open = 0;
    s->idle_busy = 1;
    s->stall_told = 0;
    s->stall_at = 0;
    s->interrupted = 0;
    s->abort_request = 0;
    publish(s, "working");
}

static int turn_finish(struct session *s, char *reply, const backend_result *meta,
                       double elapsed)
{
    const backend_result m = *meta;
    const char *text = s->prompt ? s->prompt : "";
    s->heard_at = 0;
    sessionpresent_turn_end(&s->present);
    const char *id = s->agent->session_id(s->agent);
    if (id)
        set_id(s, id);

    if (!reply) {
        replace(&s->failed_prompt, text);
        s->idle_busy = 0;
        publish(s, "errored");

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
    s->interrupted = m.interrupted;

    if (*reply)
        replace(&s->last_reply, reply);
    if (m.is_error) {
        replace(&s->failed_prompt, text);
    } else {
        transcript_add(&s->transcript, s->backend, text, reply, m.interrupted);
        replace(&s->failed_prompt, NULL);
    }
    free(reply);

    s->turns++;
    charge_turn(s, &m);
    s->tokens_in += m.input_tokens + m.cache_read_tokens + m.cache_creation_tokens;
    s->tokens_out += m.output_tokens;
    s->tokens_cached += m.cache_read_tokens;

    if (m.context_window > 0)
        s->context_window = m.context_window;
    if (m.context_tokens > 0)
        s->context_tokens = m.context_tokens;

    tab_busy(s, session_idle_busy(s));

    gitinfo_forget();

    update_title(s);
    remember_model(s);
    remember_window(s);
    if (!s->quiet)
        sessionpresent_footer(elapsed, s->context_tokens, s->context_window,
                              s->cost_usd, s->title);
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

    return turn_finish(s, reply, &meta, elapsed);
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
    s->reply = s->agent->ask_ex(s->agent, s->asked, &s->meta);
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
    if (s && s->running)
        s->abort_request = 1;
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
        if (s == live)
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
        poll(&p, 1, -1);
    }
}

const char *session_title(const struct session *s)
{
    return s && s->title[0] ? s->title : NULL;
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

/* A backend that prices its own turns reports the session total; one that
 * reports only tokens, as codex does, is priced from the model catalog. Cache
 * writes need no term: codex counts them inside the input it reports. */
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

    /* Pi otherwise consults its own mutable setting on every new process. Once
       mux has observed the model pi selected, make that explicit for later mux
       sessions until the user chooses another model here. */
    if (!strcmp(s->backend, "pi") && (!s->model || !*s->model) && id && *id)
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
    if (!session_can_set_effort(s))
        return NULL;
    const char *effort = spin_effort(s);
    return effort_is_off(effort) || !strcmp(effort, "default") ? NULL : effort;
}

int session_can_set_effort(const struct session *s)
{
    return s->agent && (s->agent->caps & BACKEND_CAP_EFFORT);
}

const char *session_id(const struct session *s) { return s->id[0] ? s->id : NULL; }

int session_can_resume(const struct session *s)
{
    return s->agent && (s->agent->caps & BACKEND_CAP_RESUME);
}

const char *session_cwd(const struct session *s) { return s->cwd; }
const char *session_workdir(const struct session *s)
{
    return s->workdir ? s->workdir : s->cwd;
}
const char *session_backend(const struct session *s) { return s->backend; }

int session_argv(const struct session *s, char **out, int max, unsigned what)
{
    const char *id = session_id(s);
    int resume = (what & SESSION_ARGV_RESUME) && id && session_can_resume(s);
    return mux_argv(out, max, resume ? what : (what & ~SESSION_ARGV_RESUME),
                    NULL, session_backend(s), session_cwd(s), session_model(s),
                    session_effort(s), id,
                    (what & SESSION_ARGV_SAFE) && s && !s->customizations, NULL);
}

int session_last_interrupted(const struct session *s) { return s ? s->interrupted : 0; }

const char *session_last_error(const struct session *s)
{
    if (s && s->error_note)
        return s->error_note;
    return s && s->agent ? s->agent->last_error(s->agent) : NULL;
}
const char *session_last_reply(const struct session *s) { return s->last_reply; }
const char *session_prompt(const struct session *s) { return s ? s->prompt : NULL; }
double session_turn_started(const struct session *s) { return s ? s->started : 0; }
const char *session_failed_prompt(const struct session *s)
{
    return s ? s->failed_prompt : NULL;
}

double session_cost(const struct session *s)
{
    return s ? s->cost_usd : 0;
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
    if (used <= 0 || window <= 0)
        return -1;
    int percent = (int)(used * 100 / window);
    return percent > 100 ? 100 : percent;
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
    if (tg_label() || relay_label())
        snprintf(chat, sizeof chat, "%s%s%s", tg_label() ? tg_label() : "",
                 tg_label() && relay_label() ? ", " : "",
                 relay_label() ? relay_label() : "");

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
