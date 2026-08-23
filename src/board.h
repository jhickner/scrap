
#ifndef BOARD_H
#define BOARD_H

#include <stddef.h>
#include <time.h>

// The card store: one JSONL file, machine-wide, shared by every mux on it.
// A card is a thought that has not been dealt with yet. Capture appends one
// and returns; everything that works out what it meant happens later.
//
// The store is not per-repo on purpose. A card thrown from a phone does not
// know which repo it belongs to, which is what triage is for, so `cwd` is a
// field on the card and the board filters by it.

enum board_col {
    BOARD_NEW,
    BOARD_UNCLEAR,
    BOARD_BACKLOG,
    BOARD_DOING,
    BOARD_REVIEW,
    BOARD_AUDIT,
    BOARD_MERGING,
    BOARD_DONE,
    BOARD_COLS,
};

const char    *board_col_name(enum board_col col);
enum board_col board_col_from_name(const char *name);

#define BOARD_ID_MAX    16
#define BOARD_TITLE_MAX 200
#define BOARD_WHO_MAX   16

struct board_note {
    time_t ts;
    char   who[BOARD_WHO_MAX];  /* triage | worker | you */
    char  *text;
};

struct board_card {
    char           id[BOARD_ID_MAX];
    enum board_col col;
    char           kind[16];        /* set by triage; empty until then */
    char           title[BOARD_TITLE_MAX];
    char          *body;            /* never NULL once loaded */
    char           cwd[4096];
    int            priority;

    // What runs it: empty falls back to the profile for its kind, and then to
    // the session's own defaults.
    char backend[32];
    char model[128];
    char effort[32];

    char   session[128];    /* the backend's id for the worker's conversation */
    char   worktree[4096];
    char   base[24];        /* the sha the worktree branched from */
    double cost_usd;

    time_t created, updated;

    struct board_note *log;
    int                log_n;
};

const char *board_path(void);

// Every card, newest first. The caller frees with board_free().
int  board_load(struct board_card **out);
void board_free(struct board_card *cards, int n);

// A card in a loaded array, by id. NULL when it is not there.
struct board_card *board_find(struct board_card *cards, int n, const char *id);

// Capture: appends `text` as a card in `new` and fills `id_out` with the id it
// was given. The first line becomes the title, the whole of it the body.
int board_add(const char *text, const char *cwd, char id_out[BOARD_ID_MAX]);

// Writes `card` back over the one with its id, under the lock, leaving every
// other card as it was found. Its `updated` is stamped for the caller.
int board_update(const struct board_card *card);

int board_remove(const char *id);

// Appends a line to a card's log without having to load and write it whole,
// which is what the worker reporting its own progress does.
int board_note(const char *id, const char *who, const char *text);

// Moves a card between columns, logging why where a reason is given.
int board_move(const char *id, enum board_col col, const char *who, const char *why);

#endif
