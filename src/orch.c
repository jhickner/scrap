#include "orch.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "filelock.h"
#include "orchevent.h"
#include "orchtarget.h"
#include "orchtask.h"
#include "prompt.h"
#include "session.h"
#include "settings.h"
#include "text.h"
#include "vendor/cJSON.h"
#include "workspace.h"

#define POLL_MS         250
#define REQUEST_MAX     65536
/* how long a dispatch waits for the instance it picked to report a session id.
   mux holds that reply for up to 30 seconds itself. */
#define DISPATCH_WAIT_MS 40000
/* a reply that misses that deadline still carries a live worker, so the handoff
   keeps watching for it this long and records it when it lands. */
#define DISPATCH_LATE_MS 300000
#define RECONCILE_DEFAULT_MINUTES 20
/* how often a tab that could not be closed yet is tried again */
#define REAP_RETRY_SECONDS 5

static int             owner_lock = -1;
static char            start_error[256];
/* The session the orchestrator speaks to. Held as an id as well as a pointer:
   a restart replaces the session object, and a bare pointer would dangle, leave
   nobody to tell, and pile up undeliverable events for good. */
static struct session *told;
static char            told_id[128];
static time_t          next_reconcile;
/* what the task store looked like at the last sweep for tabs to close, and
   when to sweep again for one that would not close yet */
static long            reap_sig;
static time_t          reap_retry;

static void told_remember(struct session *s)
{
    told = s;
    const char *id = s ? session_id(s) : NULL;
    if (id && *id)
        snprintf(told_id, sizeof told_id, "%s", id);
}

/* The id is only known once the backend reports it, so the pointer is the fast
   path and the id is what survives a restart re-creating the session. */
static struct session *told_find(void)
{
    if (told && workspace_index_of(told) >= 0) {
        const char *id = session_id(told);
        if (id && *id && strcmp(told_id, id))
            snprintf(told_id, sizeof told_id, "%s", id);
        return told;
    }
    if (!told_id[0])
        return NULL;
    for (int i = 0; i < workspace_count(); i++) {
        struct session *s = workspace_at(i);
        const char *id = s ? session_id(s) : NULL;
        if (id && !strcmp(id, told_id)) {
            told = s;
            return s;
        }
    }
    return NULL;
}

const char *orch_start_error(void)
{
    return start_error[0] ? start_error : NULL;
}

const char *orch_label(void)
{
    return owner_lock >= 0 ? "orchestrator" : NULL;
}

struct session *orch_session(void)
{
    return told_find();
}

void orch_forget_session(struct session *s)
{
    if (told != s)
        return;
    told = NULL;
    /* a closed tab is gone for good; a restart is handled by told_find */
    told_id[0] = '\0';
}

static int claim_orchestrator(void)
{
    char path[4300];
    if (!orchtask_lockpath(path, sizeof path)) {
        snprintf(start_error, sizeof start_error,
                 "could not name the orchestrator lock");
        fprintf(stderr, APP_NAME ": %s\n", start_error);
        return 0;
    }
    owner_lock = filelock_acquire(path, LOCK_EX | LOCK_NB);
    if (owner_lock >= 0)
        return 1;
    if (errno == EWOULDBLOCK || errno == EAGAIN)
        snprintf(start_error, sizeof start_error,
                 "orchestrator is already enabled by another mux instance");
    else
        snprintf(start_error, sizeof start_error,
                 "could not claim orchestrator for this mux instance: %s", strerror(errno));
    fprintf(stderr, APP_NAME ": %s\n", start_error);
    return 0;
}

/* ---- speaking to the orchestrator session ------------------------------- */

static void echo_line(struct session *s, void *ud)
{
    (void)s;
    prompt_echo_message(ud);
}

/* Returns 0 when there is nobody to tell, which leaves the caller's event
   unacked so it comes round again. */
static int tell(const char *line)
{
    struct session *t = told_find();
    if (!t || !line || !*line)
        return 0;
    int at = workspace_index_of(t);
    if (at < 0)
        return 0;
    if (!session_turn_running(t))
        workspace_render(at, echo_line, (void *)line);
    return workspace_send(at, line, NULL);
}

/* ---- reconcile ----------------------------------------------------------- */

static const char *result_status(const cJSON *res)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive((cJSON *)res,
                                                                         "status"));
    return s && *s ? s : "done";
}

static const char *result_summary(const cJSON *res)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive((cJSON *)res,
                                                                         "summary"));
    return s ? s : "";
}

/* A result file means the worker finished: a worker reporting done leaves the
   task at review, since done also means merged, and that is the user's call. */
static int fold_result(const struct orch_rec *rec)
{
    cJSON *res = orchtask_result_read(rec->id);
    if (!res)
        return 0;

    /* status and summary are owned by res, so res outlives every use of them. */
    const char *status = result_status(res);
    const char *summary = result_summary(res);
    const char *next = strcmp(status, "done") ? "failed" : "review";
    char line[1200];
    snprintf(line, sizeof line,
             "orchestrator: task %s in %s reported %s. %s",
             rec->id, rec->project, status, summary);

    orchtask_set_summary(rec->project, rec->id, summary);
    if (!orchtask_set_status(rec->project, rec->id, next, NULL, NULL, NULL, NULL, 0)) {
        cJSON_Delete(res);
        return 0;
    }
    orchtask_result_drop(rec->id);
    orchtask_log("result", rec->project, rec->id, status);
    cJSON_Delete(res);
    tell(line);
    return 1;
}

/* A dispatched task with no result and no live session anywhere: the worker
   died. Say so rather than leaving it dispatched forever. */
static int fold_lost(const struct orch_rec *rec)
{
    if (strcmp(rec->status, "dispatched") || !rec->session[0])
        return 0;
    if (orchtarget_session_pid(rec->session))
        return 0;

    orchtask_note(rec->project, rec->id, "worker ended without writing a result");
    if (!orchtask_set_status(rec->project, rec->id, "failed", NULL, NULL, NULL, NULL, 0))
        return 0;
    orchtask_log("stuck", rec->project, rec->id, "no result, no live session");

    char line[600];
    snprintf(line, sizeof line,
             "orchestrator: task %s in %s ended without a result and its session is gone. "
             "Ask whether to redispatch.",
             rec->id, rec->project);
    tell(line);
    return 1;
}

int orch_reconcile(void)
{
    struct orch_rec *recs = NULL;
    int n = orchtask_load(NULL, 0, &recs);
    int changed = 0;
    for (int i = 0; i < n; i++) {
        if (fold_result(&recs[i]))
            changed++;
        else if (fold_lost(&recs[i]))
            changed++;
    }
    free(recs);
    next_reconcile = time(NULL) + 60 * settings_get_int(SETTING_ORCH_RECONCILE,
                                                        RECONCILE_DEFAULT_MINUTES);
    return changed;
}

/* ---- completion events --------------------------------------------------- */

/* A completion event means the worker stopped. Reconciling is this subsystem's
   job, not the session's: fold_result and fold_lost already speak, with the
   summary, so the event itself says nothing and costs the session no turn.
   An event is acked once its task has left dispatched -- folded -- or dropped
   when the task is unknown; anything else is retried on the next poll. */
static void drain_events(void)
{
    struct orch_event *v = NULL;
    int n = orchevent_pending(&v);
    if (n > 0)
        orch_reconcile();

    for (int i = 0; i < n; i++) {
        cJSON *rec = orchtask_find(NULL, v[i].task, NULL, 0);
        if (!rec) {
            /* an event for work this state knows nothing about: drop it rather
               than retrying it forever */
            orchevent_ack(v[i].id);
            continue;
        }
        const char *st = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rec, "status"));
        if (!st || strcmp(st, "dispatched"))
            orchevent_ack(v[i].id);
        cJSON_Delete(rec);
    }
    free(v);
}

/* ---- request files ------------------------------------------------------- */

static int requests_dir(char *out, size_t size)
{
    char root[4200];
    if (!orchtask_root(root, sizeof root))
        return 0;
    if ((size_t)snprintf(out, size, "%s/requests", root) >= size)
        return 0;
    if (mkdir(out, 0700) != 0 && errno != EEXIST)
        return 0;
    return 1;
}

static void reply(const char *dir, const char *base, const char *json)
{
    char tmp[4400], path[4400];
    size_t stem = strlen(base) - 4;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s/%.*s.res.tmp", dir, (int)stem, base) >= sizeof tmp)
        return;
    snprintf(path, sizeof path, "%s/%.*s.res", dir, (int)stem, base);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fputs(json, f);
    fputc('\n', f);
    fclose(f);
    rename(tmp, path);
}

static void reply_error(const char *dir, const char *base, const char *what)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", what);
    char *json = cJSON_PrintUnformatted(o);
    reply(dir, base, json ? json : "{\"error\":\"oom\"}");
    free(json);
    cJSON_Delete(o);
}

static const char *field(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive((cJSON *)o, key));
    return s && *s ? s : NULL;
}

/* A dispatch in flight: the worker request has gone to the instance we picked,
   and its session id has not come back yet. */
struct handoff {
    char            base[256];   /* the orchestrator request to answer */
    char            muxbase[256];/* the mux dispatch request we wrote */
    char            muxdir[4200];
    char            task[64];
    char            project[128];
    char            backend[32];
    char            model[160];
    long            pid;
    struct timespec since;
    int             busy;
    int             late;   /* the caller has been answered; still watching */
};

static struct handoff handoffs[WORKSPACE_MAX];

static int mux_dispatch_dir(char *out, size_t size)
{
    const char *env = getenv("MUX_DISPATCH_DIR");
    if (env && *env)
        return (size_t)snprintf(out, size, "%s", env) < size;
    return path_config_subdir(out, size, "dispatch");
}

static long since_ms(const struct timespec *then, const struct timespec *now)
{
    return (now->tv_sec - then->tv_sec) * 1000 + (now->tv_nsec - then->tv_nsec) / 1000000;
}

/* Hand the worker to the instance that will host it: the dispatch protocol is
   already instance-addressed, since the request file is named <pid>-<key>. */
static void serve_dispatch(const char *dir, const char *base, const cJSON *o)
{
    const char *task = field(o, "task");
    const char *prompt = field(o, "prompt");
    if (!task || !prompt) {
        reply_error(dir, base, "dispatch takes a task and a prompt");
        return;
    }

    char project[128];
    cJSON *rec = orchtask_find(field(o, "project"), task, project, sizeof project);
    if (!rec) {
        reply_error(dir, base, "no such task");
        return;
    }
    const char *had = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rec, "status"));
    const char *was = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rec, "session"));
    char status[32], session[160];
    snprintf(status, sizeof status, "%s", had ? had : "");
    snprintf(session, sizeof session, "%s", was ? was : "");
    long origin = (long)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(rec, "pid"));
    cJSON_Delete(rec);

    /* two workers on one task is not hypothetical: it has happened, and both
       wrote the same worktree. a task already out with a live worker stays out. */
    if (!strcmp(status, "dispatched") && session[0] && orchtarget_session_pid(session)) {
        reply_error(dir, base, "task is already dispatched to a live worker");
        return;
    }

    long pid = orchtarget_pick(origin);
    if (!pid) {
        reply_error(dir, base, "no mux instance with room: the task stays queued");
        return;
    }

    struct handoff *h = NULL;
    for (int i = 0; i < WORKSPACE_MAX && !h; i++)
        if (!handoffs[i].busy)
            h = &handoffs[i];
    if (!h) {
        reply_error(dir, base, "too many dispatches in flight");
        return;
    }

    memset(h, 0, sizeof *h);
    if (!mux_dispatch_dir(h->muxdir, sizeof h->muxdir)) {
        reply_error(dir, base, "no dispatch directory");
        return;
    }
    snprintf(h->base, sizeof h->base, "%s", base);
    snprintf(h->muxbase, sizeof h->muxbase, "%ld-%s.req", pid, task);
    snprintf(h->task, sizeof h->task, "%s", task);
    snprintf(h->project, sizeof h->project, "%s", project);
    snprintf(h->backend, sizeof h->backend, "%s", field(o, "backend") ? field(o, "backend") : "");
    snprintf(h->model, sizeof h->model, "%s", field(o, "model") ? field(o, "model") : "");
    h->pid = pid;
    clock_gettime(CLOCK_MONOTONIC, &h->since);
    h->busy = 1;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "prompt", prompt);
    if (h->backend[0])
        cJSON_AddStringToObject(req, "backend", h->backend);
    if (h->model[0])
        cJSON_AddStringToObject(req, "model", h->model);
    if (field(o, "cwd"))
        cJSON_AddStringToObject(req, "cwd", field(o, "cwd"));
    if (field(o, "effort"))
        cJSON_AddStringToObject(req, "effort", field(o, "effort"));
    if (field(o, "title"))
        cJSON_AddStringToObject(req, "title", field(o, "title"));
    cJSON_AddStringToObject(req, "task", task);
    /* the worker's instance records the ending; this instance delivers it */
    cJSON_AddStringToObject(req, "notify", "orchestrator");

    char *text = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!text) {
        h->busy = 0;
        reply_error(dir, base, "out of memory");
        return;
    }

    char path[4600], tmp[4700], stale[4700];
    snprintf(path, sizeof path, "%s/%s", h->muxdir, h->muxbase);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    snprintf(stale, sizeof stale, "%s/%.*s.res", h->muxdir,
             (int)(strlen(h->muxbase) - 4), h->muxbase);
    unlink(stale);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        free(text);
        h->busy = 0;
        reply_error(dir, base, "could not write the worker request");
        return;
    }
    fputs(text, f);
    fputc('\n', f);
    fclose(f);
    rename(tmp, path);
    free(text);
}

/* The worker request's own reply lands beside it; when it does, the task record
   learns where its worker is and the caller is answered. */
static void settle_handoffs(const char *dir)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    for (int i = 0; i < WORKSPACE_MAX; i++) {
        struct handoff *h = &handoffs[i];
        if (!h->busy)
            continue;

        char res[4700];
        size_t stem = strlen(h->muxbase) - 4;
        snprintf(res, sizeof res, "%s/%.*s.res", h->muxdir, (int)stem, h->muxbase);
        char *text = text_slurp(res, REQUEST_MAX, NULL);
        if (!text) {
            long waited = since_ms(&h->since, &now);
            if (!h->late && waited >= DISPATCH_WAIT_MS) {
                /* the caller cannot wait forever, but the worker may still be
                   starting: answer now and keep the handoff watching, so a late
                   session id lands on the task instead of leaving an untracked
                   worker and a task that still reads queued. */
                reply_error(dir, h->base, "the instance never answered");
                orchtask_log("error", h->project, h->task, "dispatch went unanswered");
                h->late = 1;
            }
            if (h->late && waited >= DISPATCH_LATE_MS) {
                unlink(res);
                h->busy = 0;
            }
            continue;
        }
        unlink(res);

        cJSON *o = cJSON_Parse(text);
        free(text);
        const char *session = o ? field(o, "session") : NULL;
        const char *err = o ? field(o, "error") : "unreadable reply";

        if (session) {
            orchtask_set_status(h->project, h->task, "dispatched",
                                h->backend[0] ? h->backend : NULL,
                                h->model[0] ? h->model : NULL, session, NULL, h->pid);
            orchtask_log(h->late ? "dispatch-late" : "dispatch", h->project, h->task,
                         h->backend);
            if (h->late) {
                char line[600];
                snprintf(line, sizeof line,
                         "orchestrator: task %s in %s reported its session late; "
                         "its worker is live and the task is now dispatched.",
                         h->task, h->project);
                tell(line);
                cJSON_Delete(o);
                h->busy = 0;
                continue;
            }
            cJSON *r = cJSON_CreateObject();
            cJSON_AddStringToObject(r, "task", h->task);
            cJSON_AddStringToObject(r, "project", h->project);
            cJSON_AddStringToObject(r, "session", session);
            if (field(o, "addr"))
                cJSON_AddStringToObject(r, "addr", field(o, "addr"));
            cJSON_AddNumberToObject(r, "pid", (double)h->pid);
            char *json = cJSON_PrintUnformatted(r);
            reply(dir, h->base, json ? json : "{\"error\":\"oom\"}");
            free(json);
            cJSON_Delete(r);
        } else {
            if (!h->late)
                reply_error(dir, h->base, err ? err : "the worker never reported a session");
            orchtask_log("error", h->project, h->task, err ? err : "no session");
        }
        cJSON_Delete(o);
        h->busy = 0;
    }
}

/* ---- closing the tabs of finished work ----------------------------------- */

/* A task that reached done or cancelled has nothing left for its worker to do,
   and the tab it ran in sits there for good unless somebody closes it. failed
   is not swept: it is still an open status here, its worker can be followed up
   or redispatched, and its tab is where the user reads what went wrong.

   The tab may be in any instance, so the close goes out on the dispatch
   protocol, which refuses a session that is in view or mid-turn. That is the
   wanted behaviour: a tab somebody is reading, or a worker still running, is
   left where it is and tried again on the next sweep. */
static void close_worker(const struct orch_rec *rec, long pid)
{
    char dir[4200];
    if (!mux_dispatch_dir(dir, sizeof dir))
        return;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "close", rec->session);
    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!json)
        return;

    char path[4600], tmp[4700], res[4700];
    snprintf(path, sizeof path, "%s/%ld-close-%s.req", dir, pid, rec->id);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    snprintf(res, sizeof res, "%s/%ld-close-%s.res", dir, pid, rec->id);
    /* the reply is not waited on: the session leaving the live directory is
       the answer, and a refusal is simply the next sweep's work */
    unlink(res);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fputs(json, f);
        fputc('\n', f);
        fclose(f);
        rename(tmp, path);
    }
    free(json);
}

static void reap_closed(void)
{
    struct orch_rec *recs = NULL;
    int n = orchtask_load(NULL, 1, &recs);
    orchtarget_apply_live(recs, n);

    int again = 0;
    for (int i = 0; i < n; i++) {
        if (!orchtask_closed(recs[i].status) || !recs[i].live[0])
            continue;
        /* a live tab is a reason to come back: this sweep either could not
           close it, or asked and has not seen it go yet */
        again = 1;
        if (!strcmp(recs[i].live, "working"))
            continue;
        long pid = orchtarget_session_pid(recs[i].session);
        if (pid)
            close_worker(&recs[i], pid);
    }
    free(recs);
    reap_retry = again ? time(NULL) + REAP_RETRY_SECONDS : 0;
}

/* Reading every task log on a timer would be waste, and the status change that
   matters here is made by whoever ran the command rather than by this
   instance. The logs are append-only, so their sizes and times moving is the
   whole signal. */
static long store_signature(void)
{
    char root[4200], dir[4300];
    if (!orchtask_root(root, sizeof root))
        return 0;
    if ((size_t)snprintf(dir, sizeof dir, "%s/projects", root) >= sizeof dir)
        return 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    long sig = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char path[4600];
        struct stat st;
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= sizeof path)
            continue;
        if (!stat(path, &st) && S_ISREG(st.st_mode))
            sig += (long)st.st_size + (long)st.st_mtime;
    }
    closedir(d);
    return sig;
}

static void reap_poll(void)
{
    long sig = store_signature();
    if (sig == reap_sig && (!reap_retry || time(NULL) < reap_retry))
        return;
    reap_sig = sig;
    reap_closed();
}

/* Type a line into a task's live worker: a follow-up. */
static void serve_send(const char *dir, const char *base, const cJSON *o)
{
    const char *task = field(o, "task");
    const char *textline = field(o, "text");
    if (!task || !textline) {
        reply_error(dir, base, "send takes a task and text");
        return;
    }
    char project[128];
    cJSON *rec = orchtask_find(field(o, "project"), task, project, sizeof project);
    if (!rec) {
        reply_error(dir, base, "no such task");
        return;
    }
    const char *session = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rec, "session"));
    char id[160];
    snprintf(id, sizeof id, "%s", session ? session : "");
    cJSON_Delete(rec);

    long pid = id[0] ? orchtarget_session_pid(id) : 0;
    if (!pid) {
        /* nobody to type at: keep it for the next worker on this task */
        orchtask_add_pending(project, task, textline);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "queued", 1);
        cJSON_AddStringToObject(r, "task", task);
        char *json = cJSON_PrintUnformatted(r);
        reply(dir, base, json ? json : "{\"queued\":true}");
        free(json);
        cJSON_Delete(r);
        return;
    }

    char muxdir[4200];
    if (!mux_dispatch_dir(muxdir, sizeof muxdir)) {
        reply_error(dir, base, "no dispatch directory");
        return;
    }
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "send", textline);
    cJSON_AddStringToObject(req, "session", id);
    char *json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!json) {
        reply_error(dir, base, "out of memory");
        return;
    }

    char path[4600], tmp[4700];
    snprintf(path, sizeof path, "%s/%ld-send-%s.req", muxdir, pid, task);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        fputs(json, f);
        fputc('\n', f);
        fclose(f);
        rename(tmp, path);
    }
    free(json);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "sent", f != NULL);
    cJSON_AddStringToObject(r, "session", id);
    cJSON_AddNumberToObject(r, "pid", (double)pid);
    char *out = cJSON_PrintUnformatted(r);
    reply(dir, base, out ? out : "{\"error\":\"oom\"}");
    free(out);
    cJSON_Delete(r);
}

static void serve(const char *dir, const char *base, const char *text)
{
    cJSON *o = cJSON_Parse(text);
    if (!o) {
        reply_error(dir, base, "bad json");
        return;
    }
    const char *op = field(o, "op");

    if (op && !strcmp(op, "dispatch")) {
        serve_dispatch(dir, base, o);
    } else if (op && !strcmp(op, "send")) {
        serve_send(dir, base, o);
    } else if (op && !strcmp(op, "reconcile")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "changed", orch_reconcile());
        char *json = cJSON_PrintUnformatted(r);
        reply(dir, base, json ? json : "{\"changed\":0}");
        free(json);
        cJSON_Delete(r);
    } else if (op && !strcmp(op, "tell")) {
        const char *line = field(o, "text");
        cJSON *r = cJSON_CreateObject();
        cJSON_AddBoolToObject(r, "told", line && tell(line));
        char *json = cJSON_PrintUnformatted(r);
        reply(dir, base, json ? json : "{\"told\":false}");
        free(json);
        cJSON_Delete(r);
    } else if (op && !strcmp(op, "status")) {
        struct orch_rec *recs = NULL;
        int n = orchtask_load(NULL, 0, &recs);
        free(recs);
        cJSON *r = cJSON_CreateObject();
        cJSON_AddNumberToObject(r, "pid", (double)getpid());
        cJSON_AddNumberToObject(r, "open", n < 0 ? 0 : n);
        const char *id = told_id[0] ? told_id : NULL;
        cJSON_AddStringToObject(r, "session", id ? id : "");
        char *json = cJSON_PrintUnformatted(r);
        reply(dir, base, json ? json : "{\"error\":\"oom\"}");
        free(json);
        cJSON_Delete(r);
    } else {
        reply_error(dir, base, "unknown op");
    }
    cJSON_Delete(o);
}

static void serve_requests(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    char prefix[32];
    int plen = snprintf(prefix, sizeof prefix, "%ld-", (long)getpid());

    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len <= (size_t)plen + 4 || strncmp(e->d_name, prefix, (size_t)plen))
            continue;
        if (strcmp(e->d_name + len - 4, ".req"))
            continue;
        char path[4400];
        if ((size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= sizeof path)
            continue;
        char *text = text_slurp(path, REQUEST_MAX, NULL);
        unlink(path);
        if (!text)
            continue;
        serve(dir, e->d_name, text);
        free(text);
    }
    closedir(d);
}

/* Any instance may own the orchestrator, so a request naming a pid that is no
   longer running would sit there forever. The owner answers those too. */
static void adopt_orphans(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t len = strlen(e->d_name);
        if (len <= 4 || strcmp(e->d_name + len - 4, ".req"))
            continue;
        if (strncmp(e->d_name, "any-", 4))
            continue;
        char path[4400], mine[4400];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        snprintf(mine, sizeof mine, "%s/%ld-%s", dir, (long)getpid(), e->d_name + 4);
        rename(path, mine);
    }
    closedir(d);
}

void orch_poll(void)
{
    if (owner_lock < 0)
        return;

    static struct timespec last;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - last.tv_sec) * 1000 + (now.tv_nsec - last.tv_nsec) / 1000000;
    if (last.tv_sec && ms >= 0 && ms < POLL_MS)
        return;
    last = now;

    char dir[4200];
    if (!requests_dir(dir, sizeof dir))
        return;

    adopt_orphans(dir);
    settle_handoffs(dir);
    serve_requests(dir);
    drain_events();
    reap_poll();

    if (time(NULL) >= next_reconcile)
        orch_reconcile();
}

int orch_start(struct session *s)
{
    start_error[0] = '\0';
    if (owner_lock >= 0) {
        told_remember(s);
        return 1;
    }
    if (!claim_orchestrator())
        return 0;
    told_remember(s);
    /* reconcile on load: the state files are the truth, and anything that
       happened while nothing was running is found here */
    orch_reconcile();
    return 1;
}

void orch_stop(void)
{
    if (owner_lock >= 0)
        filelock_release(owner_lock);
    owner_lock = -1;
    told = NULL;
    told_id[0] = '\0';
    reap_sig = 0;
    reap_retry = 0;
    for (int i = 0; i < WORKSPACE_MAX; i++)
        handoffs[i].busy = 0;
}
