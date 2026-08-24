
#ifndef BOARDWORK_H
#define BOARDWORK_H

#include <stddef.h>

#include "boardcfg.h"
#include "boardflow.h"

struct board_card;
struct session;

void boardwork_finished(struct session *s);

int boardwork_start(const struct board_card *c, char *why, int size);

int boardwork_rejoin(const struct board_card *c, char *why, int size);

int boardwork_blocked(const struct board_card *c, char *why, int size);

int boardwork_running(void);

int boardwork_serve(int *waiting);

const char *boardwork_card_of(const struct session *s);

void boardwork_worktree_of(const char *root, const char *id, char *out,
                           size_t size);

void boardwork_branch_of(const char *id, char *out, size_t size);

#define BOARDWORK_TIDY_MAX 16384

int boardwork_tidy_step(const char *root, const char *tree, const char *branch,
                        char *out, size_t size);

int boardwork_tab(const char *id);

const char *boardwork_step_job(const char *id);

int boardwork_sweep_pump(void);

int boardwork_sweep_now(const char *cwd, char *why, int size);

int boardwork_hold(const char *id, struct session *s, const char *job,
                   const char *step, enum board_runs runs);

void boardwork_leave(const struct board_card *c);

void boardwork_let_go(const char *id);

void boardwork_halt(const char *id);

int boardwork_poll(void);

int boardwork_release(const struct board_card *c);

void boardwork_discard(const struct board_card *c);

int boardwork_pump(void);

int boardwork_approve(const struct board_card *c, int audit);

int boardwork_send_back(const struct board_card *c, const char *why);

int boardwork_reject(const struct board_card *c, const char *why);

void boardwork_spoke_to(struct session *s);

int boardwork_feedback(const struct board_card *c, const char *text);

#endif
