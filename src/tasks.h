#ifndef TASKS_H
#define TASKS_H

#include <stddef.h>
#include <time.h>

#include "vendor/agents/backend.h"

#define TASKS_MAX 16

struct task {
    char   id[40];
    char   parent[40];
    char   desc[140];
    char   type[40];
    char   status[24];
    char   latest[240];
    char   cmd[240];
    time_t started, ended;
    int    repeats;
    int    inferred;
};

struct tasktab {
    struct task v[TASKS_MAX];
    int         n;
    int         seq;
    int         lifecycle;
    char        tag[16];
    struct {
        char id[64];
        char cmd[240];
    } shell[TASKS_MAX];
    int shell_next;
};

void tasks_reset(struct tasktab *t, const char *tag);

const struct task *tasks_note(struct tasktab *t, const backend_event *ev, int *repeat);

int tasks_done(const struct task *a);
int tasks_running(const struct tasktab *t);

int tasks_pending(const struct tasktab *t);
int tasks_count(const struct tasktab *t);

const struct task *tasks_at(const struct tasktab *t, int i);

const struct task *tasks_by_parent(const struct tasktab *t, const char *tool_use_id);

void tasks_label(const struct task *a, char *out, size_t size);

int tasks_drop(struct tasktab *t);

void tasks_line(const struct task *a, char *out, size_t size, size_t *cmd_at, size_t *cmd_len);

void tasks_duration(char *out, size_t size, long secs);

#endif
