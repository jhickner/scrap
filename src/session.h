
#ifndef SESSION_H
#define SESSION_H

#include <string.h>

#include "tty.h"
#include "vendor/agents/backend.h"

struct session;
struct transcript;
struct task;
struct tasktab;

typedef int (*session_key_fn)(void *ud, tty_event *ev);
void session_set_typeahead(session_key_fn fn, void *ud);

int session_poll_input(void);

struct session *session_new(const char *backend, const char *cwd, const char *model,
                            const char *effort);
void            session_free(struct session *s);

void            session_replay(struct session *s);

void session_set_quiet(struct session *s, int quiet);

typedef void (*session_event_fn)(void *ud, const backend_event *ev);
void session_set_observer(struct session *s, session_event_fn fn, void *ud);

/* process-wide hooks, called after the observer for every session's events */
typedef void (*session_listener_fn)(void *ud, struct session *s, const backend_event *ev);
int  session_add_listener(session_listener_fn fn, void *ud);
void session_remove_listener(session_listener_fn fn, void *ud);

#define SESSION_RECENT     4
#define SESSION_RECENT_MAX 160

int session_recent(const struct session *s, const char **out, int max);
int session_recent_seq(const struct session *s);

void session_set_system_extra(struct session *s, const char *text);

void session_set_abort_hook(struct session *s, int (*fn)(void *ud), void *ud);

void session_set_naming(struct session *s, int on);

/* the session this one was opened from; held until this one has an id of its
   own to hang it under */
void session_set_parent(struct session *s, const char *parent_id);

void session_set_thinking(struct session *s, int on);
int  session_thinking(const struct session *s);

void session_set_compact(struct session *s, int on);
int  session_compact(const struct session *s);

void session_set_customizations(struct session *s, int on);
void session_set_browser_login(struct session *s, int on);

void session_set_fork(struct session *s, int on);

/* why the last backend start failed; NULL when unknown */
const char *session_start_error(void);

int session_start(struct session *s);

/* Starts every session at once, off the main thread. Each is collected with
   session_start_wait(), which joins its connect and returns whether it came up;
   session_start_done() says whether that would return right away, and
   session_start_fd() is readable once a connect has finished. A session is
   untouchable until it has been collected. */
void session_start_batch(struct session **list, int n);
int  session_start_done(const struct session *s);
int  session_start_wait(struct session *s);
int  session_start_fd(void);
void session_start_drain(void);

int session_trust_project(struct session *s);
int session_take_trust_request(struct session *s);

struct session *session_set_drawing(struct session *s);

int session_turn(struct session *s, const char *text);

int  session_turn_begin(struct session *s, const char *text);
int  session_turn_running(const struct session *s);
int  session_turn_pump(struct session *s);
int  session_wake_fd(const struct session *s);
double session_turn_elapsed(const struct session *s);

#define SESSION_QUIET_SECONDS 60.0

double session_quiet(const struct session *s);

void session_turn_wait(struct session *s);

void session_interrupt(struct session *s);

int  session_busy(const struct session *s);

void session_set_unseen(struct session *s, int on);
int  session_unseen(const struct session *s);

int  session_idle_fd(const struct session *s);

int  session_idle_pump(struct session *s);
int  session_idle_busy(const struct session *s);

/* Background work this session started: the whole table, and the entry the
   event being dispatched changed, for an observer rendering it. */
const struct tasktab *session_tasks(const struct session *s);
const struct task    *session_task_change(const struct session *s);
int  session_task_repeat(const struct session *s);

/* Background work the agent has running with no turn in flight: an idle prompt
   with work outstanding is not a finished one. */
int session_work_count(const struct session *s);

/* The session's background work ended without waking the agent, and nothing is
   left to resume it. Nonzero once per stall, for a caller that can nudge. */
int  session_stalled(struct session *s);
int  session_stall_armed(const struct session *s);

int session_switch_backend(struct session *s, const char *backend);

const char *session_failed_prompt(const struct session *s);

int session_clear(struct session *s);

int session_set_cwd(struct session *s, const char *path);

int session_set_model(struct session *s, const char *model);

/* Point the session at a model, an effort and a directory in one backend
   replacement, rather than one for each. A directory it is already in keeps
   the conversation: the child is started on the session it resumes. */
int session_retarget(struct session *s, const char *model, const char *effort,
                     const char *cwd);

int session_set_effort(struct session *s, const char *effort);
const char *session_effort(const struct session *s);
int session_can_set_effort(const struct session *s);

int session_set_permission(struct session *s, const char *mode);
const char *session_permission(const struct session *s);

int         session_permission_count(void);
const char *session_permission_name(int index);
const char *session_permission_desc(int index);
int         session_permission_index(const char *mode);
int         session_permission_default(void);

int session_resume(struct session *s, const char *id);

void session_adopt_id(struct session *s, const char *id);

const char *session_title(const struct session *s);

enum session_rename {
    SESSION_RENAME_OK,
    SESSION_RENAME_NO_ID,
    SESSION_RENAME_BAD_NAME,
    SESSION_RENAME_NO_STORE,
    SESSION_RENAME_NO_SOURCE,
};

enum session_rename session_rename(struct session *s, const char *name);

const char *session_rename_error(enum session_rename why);

const char *session_model(const struct session *s);
const char *session_id(const struct session *s);

const char *session_saved_model(const char *backend);
const char *session_saved_effort(const char *backend);

const char *session_model_label(const struct session *s);

const char *session_effort_label(const struct session *s);

int session_can_resume(const struct session *s);
const char *session_cwd(const struct session *s);

const char *session_workdir(const struct session *s);
const char *session_backend(const struct session *s);

#define SESSION_ARGV_MAX 16
enum {
    SESSION_ARGV_CWD    = 1u << 0,
    SESSION_ARGV_RESUME = 1u << 1,
    SESSION_ARGV_SAFE   = 1u << 2,
    SESSION_ARGV_FORK   = 1u << 3,
};

static inline int mux_argv(char **out, int max, unsigned what,
                           const char *program, const char *backend,
                           const char *cwd, const char *model,
                           const char *effort, const char *id, int safe,
                           const char *prompt)
{
    int n = 0;
    if (program && n < max)
        out[n++] = (char *)program;
    if (n + 2 <= max) {
        out[n++] = (char *)"-b";
        out[n++] = (char *)(backend ? backend : "claude");
    }
    if ((what & SESSION_ARGV_CWD) && cwd && *cwd && n + 2 <= max) {
        out[n++] = (char *)"-C";
        out[n++] = (char *)cwd;
    }
    if (model && *model && strcmp(model, "default") && n + 2 <= max) {
        out[n++] = (char *)"-m";
        out[n++] = (char *)model;
    }
    if (effort && *effort && strcmp(effort, "default") && n + 2 <= max) {
        out[n++] = (char *)"-e";
        out[n++] = (char *)effort;
    }
    if (safe && n < max)
        out[n++] = (char *)"-s";
    if ((what & SESSION_ARGV_RESUME) && id && *id && n + 2 <= max) {
        out[n++] = (char *)"--session";
        out[n++] = (char *)id;
        if ((what & SESSION_ARGV_FORK) && n < max)
            out[n++] = (char *)"--fork";
    }
    if (prompt && n < max)
        out[n++] = (char *)prompt;
    if (n < max)
        out[n] = NULL;
    return n;
}

int session_argv(const struct session *s, char **out, int max, unsigned what);
const char *session_last_reply(const struct session *s);
/* the prompt of the running or most recent turn, and when it began */
const char *session_prompt(const struct session *s);
double      session_turn_started(const struct session *s);

/* When the event now being rendered was enqueued on the backend thread, or 0
   if it is being rendered inline. */
double      session_event_queued_at(void);
const struct transcript *session_transcript(const struct session *s);
const char *session_last_error(const struct session *s);
int         session_last_interrupted(const struct session *s);

double session_cost(const struct session *s);

long session_tokens_in(const struct session *s);
long session_tokens_out(const struct session *s);
long session_tokens_cached(const struct session *s);

int session_context_percent(const struct session *s);

long session_context_window(const struct session *s);

void session_spin_word(const struct session *s);

void session_report(const struct session *s);

#endif
