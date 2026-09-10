
#ifndef PROMPT_H
#define PROMPT_H

#include "tty.h"
#include "vendor/repl.h"

typedef struct cJSON cJSON;
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

void prompt_set_replay(struct prompt *p, void (*fn)(void *ud), void *ud);

void prompt_set_blank(struct prompt *p, void (*fn)(void *ud), void *ud);

typedef int (*prompt_live_fn)(void *ud, const char *line);
void prompt_set_live_command(struct prompt *p, prompt_live_fn fn, void *ud);

void prompt_set_echo_filter(struct prompt *p, int (*fn)(void *ud, const char *line),
                            void *ud);

void prompt_set_takeover(struct prompt *p, int (*pending)(void *ud), void (*run)(void *ud),
                         void *ud);

void prompt_set_cancel(struct prompt *p, int (*fn)(void *ud), void *ud);

void prompt_set_switcher(struct prompt *p, void (*fn)(void *ud), void *ud);

/* what a click on the transcript opens, given the 1-based screen row and
   column */
void prompt_set_click(struct prompt *p, int (*fn)(void *ud, int row, int col),
                      void *ud);

void prompt_set_board(struct prompt *p, void (*fn)(void *ud), void *ud);

/* space on an empty prompt */
void prompt_set_mic(struct prompt *p, void (*fn)(void *ud), void *ud);

void prompt_set_another(struct prompt *p, void (*fn)(void *ud), void *ud);

void prompt_set_cycle(struct prompt *p, void (*fn)(void *ud, int delta), void *ud);

void prompt_set_collapse(struct prompt *p, void (*fn)(void *ud), void *ud);

void prompt_set_split(struct prompt *p, void (*fn)(void *ud, int quiet), void *ud);

void prompt_stop(struct prompt *p);

void prompt_set_restart(struct prompt *p, int (*pending)(void *ud), int (*run)(void *ud),
                        void *ud);

void prompt_restart_check(struct prompt *p);

void prompt_set_animate(struct prompt *p, int (*busy)(void *ud), void (*tick)(void *ud),
                        void *ud);

/* How often the idle hooks are run when nothing will wake the loop for them */
#define PROMPT_IDLE_POLL_MS 500

/* fds/render are the wait-and-drain pair; poll reports work whose next step is
   due at a time rather than on an fd, and is what bounds the wait. */
void prompt_set_idle(struct prompt *p, int (*fds)(void *ud, int *out, int max),
                     int (*render)(void *ud), int (*poll)(void *ud), void *ud);

char *prompt_take_queued(struct prompt *p);

void prompt_set_external(struct prompt *p, char *(*fn)(void *ud), void *ud);
/* the live transcription in the input, edited and submitted like typed text;
   "" removes it */
void prompt_set_preview(struct prompt *p, const char *text);
void prompt_insert(struct prompt *p, const char *text);
const char *prompt_line(struct prompt *p);
/* 1 when the prompt should show the listen mark */
void prompt_set_listen(struct prompt *p, int (*fn)(void *ud), void *ud);

int  prompt_line_was_external(struct prompt *p);
/* 1 when the submitted line held the transcription preview */
int  prompt_line_had_preview(struct prompt *p);
/* keep the line as it stands and stop tracking it as a preview */
void prompt_release_preview(struct prompt *p);

void prompt_echo_message(const char *text);

#define PROMPT_ECHO_KIND "echo"
void prompt_echo_load(const cJSON *st);

#endif
