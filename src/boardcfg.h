
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

#define BOARD_STEP_NAME  32
#define BOARD_KIND_STEPS 12

enum board_runs {
    BOARD_RUNS_AGENT,
    BOARD_RUNS_WORKER,
    BOARD_RUNS_PERSON,
    BOARD_RUNS_COMMAND,
    BOARD_RUNS_MODES,
};

const char     *boardcfg_runs_name(enum board_runs runs);
enum board_runs boardcfg_runs_from_name(const char *name);

enum board_lock {
    BOARD_LOCK_NONE,
    BOARD_LOCK_REPO,
    BOARD_LOCK_MACHINE,
    BOARD_LOCKS,
};

const char     *boardcfg_lock_name(enum board_lock lock);
enum board_lock boardcfg_lock_from_name(const char *name);

struct board_role {
    char name[32];
    char job[32];
    char step[BOARD_STEP_NAME];
    char tier[8];
    int  skippable;

    enum board_runs runs;
    enum board_lock lock;

    char fail_marker[64];
    char fail_step[BOARD_STEP_NAME];
    char fail_prompt[32];
    char pass_label[32];
    char fail_label[32];
    int  over_files;
    int  over_lines;

    char  backend[32];
    char  model[128];
    char  effort[32];
    char *prompt;
};

#define BOARD_ROLES_MAX 16

#define BOARD_KINDS_MAX 16

struct board_kind {
    char  name[32];
    char *means;
    char *prompt;
    char *approval_prompt;
    char  next_kind[32];
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
    int sweep_every;
    int archive_after;
    int done_shown;
    int backlog_shown;

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

const struct board_role *boardcfg_for_step(const char *step);

const struct board_role *boardcfg_worker(void);

const struct board_role *boardcfg_for_backend(const char *job,
                                              const char *backend,
                                              const char *tier);

int boardcfg_steps(const char *const **out);

#define BOARDCFG_ARGV_MAX 9

int boardcfg_argv(const struct board_role *p, const char *prompt, char **out,
                  int max);

const char *boardcfg_serving(void);

const char *boardcfg_view(void);

int boardcfg_set_view(const char *view);

int boardcfg_set_serving(const char *backend);

/* both lists lead with "" for unpinned, and stop at max entries */
int boardcfg_backend_choices(const struct board_cfg *c, const char **out,
                             int max);

int boardcfg_tier_choices(const char **out, int max);

const struct board_backend *boardcfg_backend(const struct board_cfg *c,
                                             const char *name);

const struct board_kind *boardcfg_kind(const char *name);

int boardcfg_priority(const char *kind);

int boardcfg_kind_takes(const char *kind, const char *step);

const char *boardcfg_kind_step(const char *kind, int at);

int boardcfg_kind_step_at(const char *kind, const char *step);

void boardcfg_kind_steps(struct board_kind *k, const char *list);

void boardcfg_kind_steps_default(struct board_kind *k);

void boardcfg_kinds_block(char *out, size_t size);

char *boardcfg_expand(const char *text, const char *id);

void boardcfg_projects_block(char *out, size_t size);

#endif
