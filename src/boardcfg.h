
#ifndef BOARDCFG_H
#define BOARDCFG_H

#include <stddef.h>

// What the board runs things with, kept beside the cards in
// ~/.config/mux/board.json and edited from the board itself rather than from
// the settings list: it is the board's own machinery, not a preference.
//
// Absent or half-written, the defaults below stand, so the board works before
// anything has been configured.

// Who runs what. Triage classifies, a worker builds, an audit reads a diff, a
// sweep looks over a repo for what the others left behind, and a merge worker
// is sent in when the queue could not land a card on its own.
enum board_who {
    BOARD_WHO_TRIAGE,
    BOARD_WHO_WORKER,
    BOARD_WHO_AUDIT,
    BOARD_WHO_SWEEP,
    BOARD_WHO_MERGE,
    BOARD_WHO,
};

const char *boardcfg_who_name(enum board_who who);

struct board_profile {
    char  backend[32];
    char  model[128];
    char  effort[32];
    char *prompt;       /* standing instructions; never NULL once loaded */
};

// What a card can turn out to be. The classifier is told the name and what it
// means; a worker given a card of that kind is told the prompt. Kinds are
// configuration rather than code, so a board can have the classes its work
// actually falls into.
#define BOARD_KINDS_MAX 16

// What a card of a kind goes through after a worker has had it. A note being
// filed takes none of them and is done when the worker stops; work that has to
// land takes all four. Anything between is a matter of configuration.
enum board_step {
    BOARD_STEP_WORKTREE,    /* a worktree and a branch of its own */
    BOARD_STEP_REVIEW,      /* stops for a person to say yes */
    BOARD_STEP_AUDIT,       /* read by an auditor, if the diff is big enough */
    BOARD_STEP_MERGE,       /* through the merge queue */
    BOARD_STEPS,
};

const char     *boardcfg_step_name(enum board_step step);
enum board_step boardcfg_step_from_name(const char *name);

struct board_kind {
    char  name[32];
    char *means;    /* what the classifier is told this kind is; never NULL */
    char *prompt;   /* what a worker of this kind is told; never NULL */
    int   priority;

    unsigned steps;     /* a bit per enum board_step */
};

struct board_cfg {
    int workers;         /* how many may run at once */
    int usage_ceiling;   /* percent of quota above which nothing starts */
    int reset_hold;      /* minutes: a reset this close waits rather than delegates */
    int audit_files;     /* a diff over this many files is audited; 0 never */
    int audit_lines;
    int sweep_every;     /* cards done in a repo before a refactor sweep; 0 never */
    int archive_after;   /* days a done card stays on the board; 0 forever */

    char delegation[256];   /* "claude,codex,grok": who takes over when quota runs out */

    // Run in the worktree before a card lands, and it does not land if this
    // fails. Empty means nothing is checked.
    char verify[256];

    struct board_kind kinds[BOARD_KINDS_MAX];
    int               kinds_n;

    struct board_profile who[BOARD_WHO];
};

// The configuration, loaded once and cached. Never NULL.
const struct board_cfg *boardcfg(void);

// The same, to be changed and then written back. Editing without saving leaves
// the file alone but the cache changed, which is what a form that is escaped
// out of must not do -- copy, edit the copy, and hand it to boardcfg_set().
struct board_cfg *boardcfg_copy(void);
void              boardcfg_free(struct board_cfg *c);

// Takes the copy as the configuration and writes it out.
int boardcfg_set(const struct board_cfg *c);

const struct board_profile *boardcfg_for(enum board_who who);

// The kind by that name, or NULL when the configuration does not name one.
const struct board_kind *boardcfg_kind(const char *name);

// What a card of this kind starts out worth.
int boardcfg_priority(const char *kind);

// Whether a card of this kind takes this step. An unknown kind takes all of
// them, which is the careful way round.
int boardcfg_kind_takes(const char *kind, enum board_step step);

// The kinds as the classifier is told them: a list of names, then a line each
// saying what it means. Written into the triage prompt where {kinds} is.
void boardcfg_kinds_block(char *out, size_t size);

#endif
