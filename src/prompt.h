
#ifndef PROMPT_H
#define PROMPT_H

#include "vendor/cJSON.h"

#include "tty.h"
#include "ui.h"
#include "vendor/repl.h"

struct prompt;

struct prompt *prompt_new(const ReplCommand *commands, int command_count);
void           prompt_free(struct prompt *p);

void prompt_history_open(struct prompt *p, const char *path);

void prompt_file_completion(struct prompt *p, const char *root);

void prompt_rehome(const char *root);

char *prompt_read(struct prompt *p);

struct prompt_key {
    const char *key;
    const char *desc;
};

const struct prompt_key *prompt_shortcuts(int *count);

int  prompt_live_key(void *ud, tty_event *ev);

int  prompt_input_rows(struct prompt *p, int cols);
void prompt_paint_input(struct prompt *p, int rows, int *caret_row, int *caret_col);

#define QUEUED_LINES 2

int  prompt_queued_rows(struct prompt *p, int cols);
void prompt_paint_queued(struct prompt *p, int room);

void prompt_set_queued_source(struct prompt *p, int (*count)(void *ud),
                              const char *(*at)(void *ud, int i),
                              char *(*take_last)(void *ud), void *ud);
int  prompt_busy(struct prompt *p);

void prompt_set_replay(struct prompt *p, void (*fn)(void *ud), void *ud);

void prompt_set_blank(struct prompt *p, void (*fn)(void *ud), void *ud);

typedef int (*prompt_live_fn)(void *ud, const char *line);
void prompt_set_live_command(struct prompt *p, prompt_live_fn fn, void *ud);

void prompt_set_echo_filter(struct prompt *p, int (*fn)(void *ud, const char *line),
                            void *ud);
int  prompt_echoes(struct prompt *p, const char *line);

void prompt_set_takeover(struct prompt *p, int (*pending)(void *ud), void (*run)(void *ud),
                         void *ud);

void prompt_set_cancel(struct prompt *p, int (*fn)(void *ud), void *ud);

void prompt_set_switcher(struct prompt *p, void (*fn)(void *ud), void *ud);

void prompt_set_board(struct prompt *p, void (*fn)(void *ud), void *ud);

void prompt_set_another(struct prompt *p, void (*fn)(void *ud), void *ud);

void prompt_set_collapse(struct prompt *p, void (*fn)(void *ud), void *ud);

void prompt_set_split(struct prompt *p, void (*fn)(void *ud, int quiet), void *ud);

void prompt_stop(struct prompt *p);

void prompt_set_restart(struct prompt *p, int (*pending)(void *ud), int (*run)(void *ud),
                        void *ud);

void prompt_restart_check(struct prompt *p);

void prompt_set_animate(struct prompt *p, int (*busy)(void *ud), void (*tick)(void *ud),
                        void *ud);

void prompt_set_idle(struct prompt *p, int (*fds)(void *ud, int *out, int max),
                     int (*render)(void *ud), int (*busy)(void *ud), void *ud);

char *prompt_take_queued(struct prompt *p);

void prompt_set_external(struct prompt *p, char *(*fn)(void *ud), void *ud);

int  prompt_line_was_external(struct prompt *p);

void prompt_echo_message(const char *text);

#define PROMPT_ECHO_KIND "echo"
void prompt_echo_load(const cJSON *st);

#endif
