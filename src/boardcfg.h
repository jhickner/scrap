
#ifndef BOARDCFG_H
#define BOARDCFG_H

#include <stddef.h>

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
    char *prompt;
};

#define BOARD_KINDS_MAX 16

enum board_step {
    BOARD_STEP_WORKTREE,
    BOARD_STEP_REVIEW,
    BOARD_STEP_AUDIT,
    BOARD_STEP_MERGE,
    BOARD_STEPS,
};

const char     *boardcfg_step_name(enum board_step step);
enum board_step boardcfg_step_from_name(const char *name);

struct board_kind {
    char  name[32];
    char *means;
    char *prompt;
    int   priority;

    unsigned steps;
};

struct board_cfg {
    int workers;
    int audit_files;
    int audit_lines;
    int sweep_every;
    int archive_after;

    char verify[256];

    struct board_kind kinds[BOARD_KINDS_MAX];
    int               kinds_n;

    struct board_profile who[BOARD_WHO];
};

const struct board_cfg *boardcfg(void);

struct board_cfg *boardcfg_copy(void);
void              boardcfg_free(struct board_cfg *c);

int boardcfg_set(const struct board_cfg *c);

const struct board_profile *boardcfg_for(enum board_who who);

const struct board_kind *boardcfg_kind(const char *name);

int boardcfg_priority(const char *kind);

int boardcfg_kind_takes(const char *kind, enum board_step step);

void boardcfg_kinds_block(char *out, size_t size);

#endif
