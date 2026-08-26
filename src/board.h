
#ifndef BOARD_H
#define BOARD_H

#include <stddef.h>
#include <time.h>

#include "boardcfg.h"

struct board_card;

#define BOARD_ID_MAX    16
#define BOARD_TITLE_MAX 200
#define BOARD_WHO_MAX   16

#define BOARD_ACTION_NAME 32
#define BOARD_QUEUE       12
#define BOARD_DONE_MAX    16

/* Where a card stands, read off its lists rather than stored: nothing run and
   nothing waiting is open, a queue is working, and an empty queue is your turn
   once anything has been run on it. Closed is the one bit the card carries. */
enum board_stand {
    BOARD_OPEN,
    BOARD_WORKING,
    BOARD_REVIEW,
    BOARD_CLOSED,
    BOARD_STANDS,
};

const char      *board_stand_name(enum board_stand stand);
enum board_stand board_stands(const struct board_card *c);

/* the action the card is on, or NULL when nothing is queued */
const char *board_step(const struct board_card *c);

struct board_note {
    time_t ts;
    char   who[BOARD_WHO_MAX];
    char  *text;
};

struct board_card {
    char id[BOARD_ID_MAX];
    int  closed;

    char queue[BOARD_QUEUE][BOARD_ACTION_NAME];
    int  queue_n;
    char done[BOARD_DONE_MAX][BOARD_ACTION_NAME];
    int  done_n;
    /* the queue was dropped rather than run out, so the card waits on you even
       with nothing in its history */
    int  stopped;
    char           title[BOARD_TITLE_MAX];
    char          *body;
    char           cwd[4096];
    int            priority;

    char backend[32];
    char backend_pin[32];
    char tier_pin[8];
    char model[128];
    char effort[32];

    char   session[128];
    char   worktree[4096];
    char   base[24];

    double cost_usd;
    long   tokens_in, tokens_out;

    time_t created, updated;

    struct board_note *log;
    int                log_n;
};

int board_cmp(const struct board_card *a, const struct board_card *b);

const char *board_path(void);

/* the file a card keeps under a directory of its own, named by its id */
int board_md_path(const char *dir, const char *id, char *out, size_t size);

int  board_load(struct board_card **out);

unsigned long board_revision(void);

void board_free(struct board_card *cards, int n);

struct board_card *board_find(struct board_card *cards, int n, const char *id);

int board_add(const char *text, const char *cwd, char id_out[BOARD_ID_MAX]);

void board_title_of(const char *text, char *out, size_t size);

/* the branch a card's work is on, named after its id */
void board_branch(const char *id, char *out, size_t size);

int board_update(const struct board_card *card);

/* the session id the card's worker reports, without touching the rest */
int board_set_session(const char *id, const char *session);

int board_pin(const char *id, const char *backend, const char *tier);

int board_remove(const char *id);

int board_archive(int days);

int board_note(const char *id, const char *who, const char *text);

int board_ran(const struct board_card *c, const char *action);

int board_queued(const char *id, const char *const *actions, int n);

/* Drop what is queued and stand the card in review: an action that did not
   pass takes the rest of its pipeline down with it, and waits to be told. */
int board_stopped(const char *id);

int board_took(const char *id, const char *action);

int board_close(const char *id);

#endif
