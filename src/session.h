
#ifndef SESSION_H
#define SESSION_H

#include "tty.h"
#include "vendor/agents/backend.h"

struct session;

typedef int (*session_key_fn)(void *ud, tty_event *ev);
void session_set_typeahead(session_key_fn fn, void *ud);

int session_poll_input(void);

struct session *session_new(const char *backend, const char *cwd, const char *model,
                            const char *effort);
void            session_free(struct session *s);

void            session_replay(struct session *s);

void session_set_quiet(struct session *s, int quiet);

void session_set_silent(struct session *s, int silent);

typedef void (*session_event_fn)(void *ud, const backend_event *ev);
void session_set_observer(struct session *s, session_event_fn fn, void *ud);

#define SESSION_RECENT     4
#define SESSION_RECENT_MAX 160

int session_recent(const struct session *s, const char **out, int max);
int session_recent_seq(const struct session *s);

void session_set_system_extra(struct session *s, const char *text);

void session_set_abort_hook(struct session *s, int (*fn)(void *ud), void *ud);

void session_set_naming(struct session *s, int on);

void session_set_thinking(struct session *s, int on);
int  session_thinking(const struct session *s);

void session_set_compact(struct session *s, int on);
int  session_compact(const struct session *s);

void session_set_customizations(struct session *s, int on);
void session_set_browser_login(struct session *s, int on);

void session_set_fork(struct session *s, int on);

int session_start(struct session *s);

int session_trust_project(struct session *s);
int session_take_trust_request(struct session *s);

struct session *session_set_drawing(struct session *s);

int session_turn(struct session *s, const char *text);

int  session_turn_begin(struct session *s, const char *text);
int  session_turn_running(const struct session *s);
int  session_turn_pump(struct session *s);
int  session_wake_fd(const struct session *s);
double session_turn_elapsed(const struct session *s);

void session_turn_wait(struct session *s);

void session_interrupt(struct session *s);

int  session_busy(const struct session *s);

void session_set_unseen(struct session *s, int on);
int  session_unseen(const struct session *s);

int  session_idle_fd(const struct session *s);

int  session_idle_pump(struct session *s);
int  session_idle_busy(const struct session *s);

int session_switch_backend(struct session *s, const char *backend);

const char *session_failed_prompt(const struct session *s);

int session_clear(struct session *s);

int session_set_cwd(struct session *s, const char *path);

int session_set_model(struct session *s, const char *model);

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

const char *session_model_short(const struct session *s, const char *model);

const char *session_effort_label(const struct session *s);

int session_can_resume(const struct session *s);
const char *session_cwd(const struct session *s);

const char *session_workdir(const struct session *s);
const char *session_backend(const struct session *s);

#define SESSION_ARGV_MAX 11
enum {
    SESSION_ARGV_CWD    = 1u << 0,
    SESSION_ARGV_RESUME = 1u << 1,
    SESSION_ARGV_SAFE   = 1u << 2,
};
int session_argv(const struct session *s, char **out, int max, unsigned what);
const char *session_last_reply(const struct session *s);
const char *session_last_error(const struct session *s);
int         session_last_interrupted(const struct session *s);

double session_cost(const struct session *s);

int session_context_percent(const struct session *s);

long session_context_window(const struct session *s);

void session_spin_word(const struct session *s);

void session_report(const struct session *s);

#endif
