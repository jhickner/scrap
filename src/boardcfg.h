
#ifndef BOARDCFG_H
#define BOARDCFG_H

// What the board runs things with, kept beside the cards in
// ~/.config/mux/board.json and edited from the board itself rather than from
// the settings list: it is the board's own machinery, not a preference.
//
// Absent or half-written, the defaults below stand, so the board works before
// anything has been configured.

// Who runs what. Triage classifies, a worker builds, an audit reads a diff.
enum board_who {
    BOARD_WHO_TRIAGE,
    BOARD_WHO_WORKER,
    BOARD_WHO_AUDIT,
    BOARD_WHO,
};

const char *boardcfg_who_name(enum board_who who);

struct board_profile {
    char  backend[32];
    char  model[128];
    char  effort[32];
    char *prompt;       /* standing instructions; never NULL once loaded */
};

struct board_cfg {
    int workers;         /* how many may run at once */
    int usage_ceiling;   /* percent of quota above which nothing starts */
    int reset_hold;      /* minutes: a reset this close waits rather than delegates */
    int audit_files;     /* a diff over this many files is audited; 0 never */
    int audit_lines;
    int sweep_every;     /* cards done in a repo before a refactor sweep; 0 never */

    char delegation[256];   /* "claude,codex,grok": who takes over when quota runs out */

    // Run in the worktree before a card lands, and it does not land if this
    // fails. Empty means nothing is checked.
    char verify[256];

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

#endif
