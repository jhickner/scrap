#ifndef ORCHTASK_H
#define ORCHTASK_H

#include <stddef.h>
#include <stdio.h>
#include <time.h>

#include "vendor/cJSON.h"

/* The orchestrator's state, read and written as code rather than by hand.
   State lives in ~/.config/orchestrator, or $ORCHESTRATOR_DIR when set:

     registry.json          project name -> cwd, with aliases
     projects/<name>.jsonl  append-only task log, newest record per id wins
     results/<task>.json    a worker's completion signal
     log.jsonl              dispatch/result/reconcile events

   Project files stay append-only: a status change appends a whole record with
   the same id, carrying every field the previous one had. Each write takes an
   exclusive lock on the file it touches, so the subsystem and a command line
   run cannot interleave. */

#define ORCHTASK_ID_MAX 64

struct orch_rec {
    char   project[128];
    char   id[ORCHTASK_ID_MAX];
    char   desc[512];
    char   klass[32];
    char   status[32];
    char   backend[32];
    char   model[160];
    char   session[160];
    char   worktree[256];
    long   pid;        /* the instance the task was asked for, 0 when unknown */
    int    checkpoint;
    int    pending;    /* queued follow-ups */
    time_t created;
    time_t updated;
    /* not part of the record: what the live session directory says about this
       task's worker right now, filled in by orchtarget_apply_live */
    char   live[32];
};

int orchtask_root(char *out, size_t size);

/* The owner lock for this store. One orchestrator per store rather than one
   per machine: a test or a scratch run pointed at its own $ORCHESTRATOR_DIR
   gets its own lock, and cannot collide with the real one. */
int orchtask_lockpath(char *out, size_t size);

/* Project names, sorted. Caller frees the array and each name. */
int orchtask_projects(char ***out);

/* cwd of a project, resolving an exact name first and then an alias, both
   case-insensitively. Fills the real name when real is not NULL. */
int orchtask_project_cwd(const char *name, char *cwd, size_t cwd_size,
                         char *real, size_t real_size);

/* Newest record per id. include_closed keeps done and cancelled. Pass NULL for
   project to load every project. Caller frees. */
int orchtask_load(const char *project, int include_closed, struct orch_rec **out);

/* The newest raw record for one id, so a caller can edit and re-append it
   without dropping fields it does not know about. Caller cJSON_Deletes it.
   Fills project_out with the project it was found in. */
cJSON *orchtask_find(const char *project, const char *id,
                     char *project_out, size_t size);

/* Append rec to the project log, stamping updated. rec is not freed. */
int orchtask_append(const char *project, cJSON *rec);

/* A new queued task. Fills id_out with the id it chose. */
int orchtask_create(const char *project, const char *desc, const char *klass,
                    int checkpoint, long pid, const char *notes,
                    char *id_out, size_t id_size);

/* Re-append the newest record for id with status replaced, plus any of the
   optional fields that are not NULL. Refuses a move out of done. */
int orchtask_set_status(const char *project, const char *id, const char *status,
                        const char *backend, const char *model,
                        const char *session, const char *worktree, long pid);

/* Queue a follow-up instruction for the task's live worker. */
int orchtask_add_pending(const char *project, const char *id, const char *text);

/* Take the first queued follow-up, if any. Returns 0 when there is none. */
int orchtask_take_pending(const char *project, const char *id, char *out, size_t size);

int orchtask_note(const char *project, const char *id, const char *note);

/* A worker's result file: read it, and take it (read then delete). Caller
   cJSON_Deletes. */
cJSON *orchtask_result_read(const char *id);
int    orchtask_result_drop(const char *id);

/* One line in log.jsonl. detail may be NULL. */
void orchtask_log(const char *ev, const char *project, const char *id,
                  const char *detail);

/* A status that is not waiting on anything: done or cancelled. */
int orchtask_closed(const char *status);

/* "45s", "3d": how long ago, at a glance. */
void orchtask_age(char *out, size_t size, time_t then, time_t now);

/* A status the orchestrator still has work to do about. */
int orchtask_open(const char *status);

void orchtask_print_table(FILE *f, const struct orch_rec *recs, int n, time_t now);
void orchtask_print_json(FILE *f, const struct orch_rec *recs, int n);
void orchtask_print_one(FILE *f, const cJSON *rec);

#endif
