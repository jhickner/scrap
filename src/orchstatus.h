#ifndef ORCHSTATUS_H
#define ORCHSTATUS_H

#include <stddef.h>
#include <time.h>

struct orch_task {
    char project[128];
    char id[64];
    char desc[512];
    char status[32];
    char backend[32];
    char model[160];
    char session[160];
    char live_status[32];
    time_t created;
    time_t updated;
};

struct orchstatus_columns {
    int project;
    int task;
    int status;
    int agent;
    int age;
};

/* Load each .jsonl file and retain the last record for each (project, id).
   Done tasks are omitted unless include_done is nonzero. */
int orchstatus_load(const char *projects_dir, const char *live_dir,
                    int include_done, struct orch_task **out);

void orchstatus_age(char *out, size_t size, time_t then, time_t now);

void orchstatus_status(char *out, size_t size, const struct orch_task *t);
void orchstatus_agent(char *out, size_t size, const struct orch_task *t);

/* Size the five table columns to their content and fit them within columns,
   including the two-cell indent, four separators, and a one-cell right
   margin. The task column receives the remaining width. */
void orchstatus_columns(struct orchstatus_columns *out, int columns,
                        const struct orch_task *tasks, int count);

/* Copy the next line of in, broken at whitespace, of at most width bytes.
   Returns the remaining text, empty when done. */
const char *orchstatus_wrap(char *out, size_t size, const char *in, size_t width);

void orchstatus_cell(char *out, size_t size, const char *in, size_t width);

#endif
