
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
enum board_tier boardcfg_tier_or_med(const char *name);

struct board_level {
    char model[128];
    char effort[32];
};

#define BOARD_BACKENDS_MAX 8

struct board_backend {
    char               name[32];
    struct board_level level[BOARD_TIERS];
};

struct board_role {
    char  name[32];
    char  job[32];
    char  tier[8];
    char  step[16];
    int   skippable;
    char  backend[32];
    char  model[128];
    char  effort[32];
    char *prompt;
};

#define BOARD_ROLES_MAX 16

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
    char *approval_prompt;
    int   priority;

    unsigned steps;
};

struct board_cfg {
    char serving[32];

    int workers;
    int auto_pull;
    int auto_pick;
    int audit_files;
    int audit_lines;
    int sweep_every;
    int archive_after;

    char verify[256];
    char projects[512];

    struct board_kind kinds[BOARD_KINDS_MAX];
    int               kinds_n;

    struct board_role roles[BOARD_ROLES_MAX];
    int               roles_n;

    struct board_backend backends[BOARD_BACKENDS_MAX];
    int                  backends_n;
};

const struct board_cfg *boardcfg(void);

void boardcfg_reload(void);

int boardcfg_missing(char *out, size_t size);

struct board_cfg *boardcfg_copy(void);
void              boardcfg_free(struct board_cfg *c);

int boardcfg_set(const struct board_cfg *c);

const struct board_role *boardcfg_for_job(const char *job);

const struct board_role *boardcfg_for_step(enum board_step step);

const struct board_role *boardcfg_for_backend(const char *job,
                                              const char *backend);

#define BOARDCFG_ARGV_MAX 9

int boardcfg_argv(const struct board_role *p, const char *prompt, char **out,
                  int max);

const char *boardcfg_serving(void);

int boardcfg_set_serving(const char *backend);

const struct board_backend *boardcfg_backend(const struct board_cfg *c,
                                             const char *name);

const struct board_kind *boardcfg_kind(const char *name);

int boardcfg_priority(const char *kind);

int boardcfg_kind_takes(const char *kind, enum board_step step);

void boardcfg_kinds_block(char *out, size_t size);

char *boardcfg_expand(const char *text, const char *id);

void boardcfg_projects_block(char *out, size_t size);

#endif
