
#ifndef BOARD_H
#define BOARD_H

#include <stddef.h>
#include <time.h>

#include "boardcfg.h"

struct board_card;

enum board_col {
    BOARD_NEW,
    BOARD_UNCLEAR,
    BOARD_BACKLOG,
    BOARD_STEP,
    BOARD_DONE,
    BOARD_COLS,
};

const char *board_col_name(enum board_col col);

const char *board_where(const struct board_card *c);

#define BOARD_ID_MAX    16
#define BOARD_TITLE_MAX 200
#define BOARD_WHO_MAX   16

struct board_note {
    time_t ts;
    char   who[BOARD_WHO_MAX];
    char  *text;
};

struct board_card {
    char           id[BOARD_ID_MAX];
    enum board_col col;
    char           step[BOARD_STEP_NAME];
    char           kind[16];
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

    char merge_into[128];
    char merge_from[48];
    char merge_to[48];

    char stuck[256];
    double cost_usd;
    long   tokens_in, tokens_out;

    time_t created, updated;

    struct board_note *log;
    int                log_n;
};

int board_cmp_col(const struct board_card *a, const struct board_card *b);

const char *board_path(void);

int  board_load(struct board_card **out);

unsigned long board_revision(void);

void board_free(struct board_card *cards, int n);

struct board_card *board_find(struct board_card *cards, int n, const char *id);

const char *board_said(const struct board_card *c, const char *who);

int board_add(const char *text, const char *cwd, char id_out[BOARD_ID_MAX]);

void board_title_of(const char *text, char *out, size_t size);

int board_update(const struct board_card *card);

int board_remove(const char *id);

int board_archive(int days);

int board_note(const char *id, const char *who, const char *text);

int board_move(const char *id, enum board_col col, const char *step,
               const char *who, const char *why);

int board_move_to(const char *id, const char *step, const char *who,
                  const char *why);

int board_move_back(const char *id, const char *step, const char *who,
                    const char *why);

int board_at(const struct board_card *c, const char *step);

void board_put(struct board_card *c, const char *name);

#endif
