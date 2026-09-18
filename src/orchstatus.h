#ifndef ORCHSTATUS_H
#define ORCHSTATUS_H

#include <stddef.h>
#include <time.h>

#include "orchtask.h"

/* The /tasks table: how the orchestrator's tasks are laid out on a terminal.
   The records themselves come from orchtask, which is their only reader and
   writer; nothing here goes near a file. */

struct orchstatus_columns {
    int project;
    int id;
    int task;
    int status;
    int agent;
    int age;
};

/* Group open tasks by project, most recently changed first within each. With
   include_closed, order everything by recency alone: a finished task is
   interesting for when it finished, not for whose project it was. */
void orchstatus_sort(struct orch_rec *recs, int n, int include_closed);

/* The state column: the status, the worker's live state when it has one, and
   the count of follow-ups waiting for that worker. */
void orchstatus_status(char *out, size_t size, const struct orch_rec *t);

/* The task column, before wrapping: the description, marked when the task is a
   checkpoint, since a checkpoint holds up its project until it is reviewed. */
void orchstatus_task(char *out, size_t size, const struct orch_rec *t);
void orchstatus_agent(char *out, size_t size, const struct orch_rec *t);

/* Size the six table columns to their content and fit them within columns,
   including the two-cell indent, five separators, and a one-cell right
   margin. The task column receives the remaining width. */
void orchstatus_columns(struct orchstatus_columns *out, int columns,
                        const struct orch_rec *tasks, int count);

/* Copy the next line of in, broken at whitespace, of at most width bytes.
   Returns the remaining text, empty when done. */
const char *orchstatus_wrap(char *out, size_t size, const char *in, size_t width);

void orchstatus_cell(char *out, size_t size, const char *in, size_t width);

#endif
