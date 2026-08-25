
#ifndef BOARDCFG_H
#define BOARDCFG_H

#include <stddef.h>

enum board_tier {
    BOARD_TIER_LOW,
    BOARD_TIER_MED,
    BOARD_TIER_HIGH,
    BOARD_TIERS,
};

const char     *boardcfg_tier_name(enum board_tier tier);
enum board_tier boardcfg_tier_from_name(const char *name);

struct board_level {
    char model[128];
    char effort[32];
};

#define BOARD_BACKENDS_MAX 8

struct board_backend {
    char               name[32];
    struct board_level level[BOARD_TIERS];
};

#define BOARD_STEP_NAME  32
#define BOARD_KIND_STEPS 12

#define BOARD_NEEDS 4

/* Where an action runs: the card's worktree, or the checkout it came from.
   merge and anything after it act on the main branch, not on a branch. */
enum board_in {
    BOARD_IN_WORKTREE,
    BOARD_IN_REPO,
    BOARD_INS,
};

struct board_action {
    char name[32];
    char tier[8];

    enum board_in where;
    char          needs[BOARD_NEEDS][32];
    int           needs_n;
    int           on_capture;

    char fail_marker[64];

    char  backend[32];
    char  model[128];
    char  effort[32];
    char *prompt;
};

#define BOARD_ACTIONS_MAX 16

#define BOARD_KINDS_MAX 16

struct board_kind {
    char  name[32];
    char *means;
    int   priority;

    char steps[BOARD_KIND_STEPS][BOARD_STEP_NAME];
    int  steps_n;
};

struct board_cfg {
    char serving[32];
    char view[8];

    int workers;
    int auto_pull;
    int auto_pick;
    int archive_after;
    int done_shown;
    int backlog_shown;

    char projects[512];

    struct board_kind kinds[BOARD_KINDS_MAX];
    int               kinds_n;

    struct board_action actions[BOARD_ACTIONS_MAX];
    int                 actions_n;

    struct board_backend backends[BOARD_BACKENDS_MAX];
    int                  backends_n;
};

struct board_default;

/* Kinds and actions are compiled in. Standing a different table in is for tests;
   passing NULL goes back to the one the binary was built with. */
void boardcfg_defaults(const struct board_default *table, int n);

const struct board_cfg *boardcfg(void);

void boardcfg_reload(void);

int boardcfg_missing(char *out, size_t size);

struct board_cfg *boardcfg_copy(void);
void              boardcfg_free(struct board_cfg *c);

int boardcfg_set(const struct board_cfg *c);

const struct board_action *boardcfg_action(const char *name);

int boardcfg_actions(const char **out, int max);

/* the action, with the model and effort the backend and tier ask for */
const struct board_action *boardcfg_for_backend(const char *name,
                                                const char *backend,
                                                const char *tier);

#define BOARDCFG_ARGV_MAX 9

int boardcfg_argv(const struct board_action *p, const char *prompt, char **out,
                  int max);

const char *boardcfg_serving(void);

const char *boardcfg_view(void);

int boardcfg_set_view(const char *view);

int boardcfg_set_serving(const char *backend);

/* both lists lead with "" for unpinned, and stop at max entries */
int boardcfg_backend_choices(const struct board_cfg *c, const char **out,
                             int max);

int boardcfg_tier_choices(const char **out, int max);

const struct board_kind *boardcfg_kind(const char *name);

int boardcfg_priority(const char *kind);

const char *boardcfg_kind_step(const char *kind, int at);

int boardcfg_kind_step_at(const char *kind, const char *step);

void boardcfg_kinds_block(char *out, size_t size);

void boardcfg_projects_block(char *out, size_t size);

#endif
