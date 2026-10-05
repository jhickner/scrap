
#ifndef CMD_H
#define CMD_H

#include <stddef.h>

#include "vendor/repl.h"

struct session;
struct pick_item;

const struct pick_item *cmd_model_choices(const char *backend, int *count);
const struct pick_item *cmd_backend_choices(int *count);
const struct pick_item *cmd_effort_choices(const char *backend, int *count);
const char             *cmd_default_backend(void);

const ReplCommand *cmd_completions(int *count);

enum cmd_result {
    CMD_NOT_A_COMMAND,
    CMD_HANDLED,
    CMD_QUIT,
};

enum cmd_result cmd_dispatch(struct session *s, const char *line);

enum cmd_result cmd_submit(struct session *s, const char *line);

int cmd_runs_mid_turn(const char *line);

int cmd_runs_live(const char *line);

int cmd_is_command(const char *line);

int cmd_self_echoes(const char *line);

void cmd_dispatch_live(struct session *s, const char *line);

void cmd_run_deferred(struct session *s);
void cmd_turn_done(struct session *s);

void cmd_forget_session(struct session *s);

int cmd_resume(struct session *s);

int copy_to_clipboard(const char *text);

void cmd_attach(const char *target);
int  cmd_attach_tab(const char *target, char *why, size_t size);

#endif
