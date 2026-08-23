
#ifndef CMD_H
#define CMD_H

#include "vendor/repl.h"

struct session;
struct pick_item;

const struct pick_item *cmd_model_choices(const char *backend, int *count);
const struct pick_item *cmd_effort_choices(const char *backend, int *count);

const ReplCommand *cmd_completions(int *count);

enum cmd_result {
    CMD_NOT_A_COMMAND,
    CMD_HANDLED,
    CMD_QUIT,
};

enum cmd_result cmd_dispatch(struct session *s, const char *line);

int cmd_runs_mid_turn(const char *line);

int cmd_is_command(const char *line);
int cmd_is_quit(const char *line);

int cmd_self_echoes(const char *line);

void cmd_dispatch_live(struct session *s, const char *line);

void cmd_run_deferred(struct session *s);

void cmd_forget_session(struct session *s);

int cmd_resume(struct session *s);

#endif
