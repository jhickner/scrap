#ifndef SESSIONPRESENT_H
#define SESSIONPRESENT_H

#include <stddef.h>

#include "filediff.h"
#include "sessionview.h"
#include "vendor/agents/backend.h"

struct task;
struct tasktab;
struct transcript;

#define SESSIONPRESENT_TASK_HOLD_MAX 4

struct sessionpresent {
    char  *streamed;
    size_t streamed_len, streamed_cap;
    char   task_hold[SESSIONPRESENT_TASK_HOLD_MAX][240];
    int    task_held;
    double task_held_at;
    int    call_open;
    struct turnview view;
    struct filediff_snapshot filediff;
};

void sessionpresent_free(struct sessionpresent *p);
void sessionpresent_turn_begin(struct sessionpresent *p);
void sessionpresent_turn_end(struct sessionpresent *p);

void sessionpresent_event(struct sessionpresent *p, const backend_event *ev,
                          const char *cwd, const struct tasktab *tasks,
                          const struct task *task_change, int thinking);
void sessionpresent_expire(struct sessionpresent *p, int quiet);
void sessionpresent_break(struct sessionpresent *p);

void sessionpresent_replay(const struct transcript *transcript);

void sessionpresent_spin(const char *backend, const char *effort, double quiet,
                         double quiet_threshold);

void sessionpresent_failure(const char *backend, const char *detail);
void sessionpresent_turn_result(struct sessionpresent *p, const char *backend,
                                const char *reply, const char *last_block,
                                const char *detail, const backend_result *meta,
                                int quiet);

void sessionpresent_footer(double elapsed, long tokens, long window, double cost,
                           const char *title);

struct sessionpresent_report {
    const char *backend;
    const char *model;
    const char *effort;
    const char *auth;
    const char *permission;
    const char *chat;
    const char *id;
    const char *parent;
    const char *cwd;
    int customizations;
    int compact;
    int turns;
    long context_tokens;
    long context_window;
    long tokens_in;
    long tokens_out;
    long tokens_cached;
    double cost;
};

void sessionpresent_report(const struct sessionpresent_report *r);

struct sessionpresent_tokens {
    char   backend[16];
    char   model[48];
    char   prompt[256];
    long   fresh, cache_write, cache_read, output;
    long   context;
    double cost;

    double rate_input, rate_cache_write, rate_cache_read, rate_output;
};

void sessionpresent_tokenomics(const struct sessionpresent_tokens *turns, int n,
                               long context_window);

#endif
