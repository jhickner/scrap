#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "filelock.h"
#include "prompt.h"
#include "orch.h"
#include "orchevent.h"
#include "orchtask.h"
#include "session.h"
#include "settings.h"
#include "workspace.h"

/* ---- the world the orchestrator talks to -------------------------------- */

struct session {
    int placeholder;
};

static struct session orchestrator;
static int  orch_at = 0;
static int  turn_running;
static int  send_fails;
static char last_send[4096];
static int  send_n;

int workspace_index_of(const struct session *s)
{
    return s == &orchestrator ? orch_at : -1;
}
void workspace_render(int index, void (*fn)(struct session *s, void *ud), void *ud)
{
    (void)index;
    (void)fn;
    (void)ud;
}
int workspace_send(int index, const char *line, const char *shown)
{
    (void)index;
    (void)shown;
    if (send_fails)
        return 0;
    send_n++;
    snprintf(last_send, sizeof last_send, "%s", line ? line : "");
    return 1;
}
void prompt_echo_message(const char *text) { (void)text; }
int  session_turn_running(const struct session *s) { (void)s; return turn_running; }
const char *session_id(const struct session *s) { (void)s; return "sess-orchestrator"; }
int  settings_get_int(const char *key, int dflt) { (void)key; return dflt; }

/* ---- helpers ------------------------------------------------------------- */

static char root[] = "/tmp/mux-orch-XXXXXX";
static char live[512], muxdir[512], requests[512], projects[512], results[512];

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static char *slurp(const char *path)
{
    static char buf[8192];
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static const char *status_of(const char *id)
{
    static char status[32];
    cJSON *rec = orchtask_find("mux", id, NULL, 0);
    if (!rec)
        return "";
    snprintf(status, sizeof status, "%s",
             cJSON_GetStringValue(cJSON_GetObjectItem(rec, "status")));
    cJSON_Delete(rec);
    return status;
}

static void task(const char *id, const char *status, const char *session)
{
    char path[1024], line[1024];
    snprintf(path, sizeof path, "%s/mux.jsonl", projects);
    FILE *f = fopen(path, "a");
    assert(f);
    snprintf(line, sizeof line,
             "{\"id\":\"%s\",\"desc\":\"work\",\"status\":\"%s\",\"session\":\"%s\","
             "\"created\":100,\"updated\":100}\n", id, status, session ? session : "");
    fputs(line, f);
    fclose(f);
}

static void request(const char *key, const char *json)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/any-%s.req", requests, key);
    write_text(path, json);
}

/* the reply keeps the key and gains the owner's pid */
static char *reply_for(const char *key)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%ld-%s.res", requests, (long)getpid(), key);
    return slurp(path);
}

static void poll_settled(void)
{
    /* orch_poll rate-limits itself to four times a second */
    struct timespec ts = {0, 300 * 1000 * 1000L};
    nanosleep(&ts, NULL);
    orch_poll();
}

int main(void)
{
    assert(mkdtemp(root));
    snprintf(live, sizeof live, "%s/live", root);
    snprintf(muxdir, sizeof muxdir, "%s/dispatch", root);
    snprintf(requests, sizeof requests, "%s/requests", root);
    snprintf(projects, sizeof projects, "%s/projects", root);
    snprintf(results, sizeof results, "%s/results", root);
    assert(mkdir(live, 0700) == 0);
    assert(mkdir(muxdir, 0700) == 0);
    assert(mkdir(projects, 0700) == 0);
    assert(mkdir(results, 0700) == 0);
    setenv("ORCHESTRATOR_DIR", root, 1);
    setenv("MUX_LIVE_DIR", live, 1);
    setenv("MUX_DISPATCH_DIR", muxdir, 1);

    /* --- the owner lock ---------------------------------------------------- */

    /* the lock belongs to this store, so a real orchestrator running on this
       machine is none of this test's business */
    char lockpath[4300];
    assert(orchtask_lockpath(lockpath, sizeof lockpath));
    assert(strstr(lockpath, "orchestrator-"));
    int held = filelock_acquire(lockpath, LOCK_EX | LOCK_NB);
    assert(held >= 0);

    /* the refusal explains itself on stderr; not this test's output */
    fflush(stderr);
    int saved_err = dup(fileno(stderr));
    assert(freopen("/dev/null", "w", stderr));
    int claimed = orch_start(&orchestrator);
    fflush(stderr);
    dup2(saved_err, fileno(stderr));
    close(saved_err);
    clearerr(stderr);

    assert(!claimed);
    assert(orch_start_error());
    assert(!strcmp(orch_start_error(),
                   "orchestrator is already enabled by another mux instance"));
    assert(!orch_label());
    filelock_release(held);

    /* --- results become review, and the session is told -------------------- */

    task("t-win", "dispatched", "s-win");
    char path[1024];
    snprintf(path, sizeof path, "%s/t-win.json", results);
    write_text(path, "{\"task\":\"t-win\",\"status\":\"done\",\"summary\":\"built it\"}\n");

    assert(orch_start(&orchestrator)); /* reconciles on load */
    assert(orch_label());
    assert(!strcmp(status_of("t-win"), "review"));
    assert(send_n == 1);
    assert(strstr(last_send, "t-win") && strstr(last_send, "built it"));
    /* the result file is consumed, so the same completion is not reported twice */
    assert(!slurp(path));
    send_n = 0;
    assert(orch_reconcile() == 0);
    assert(send_n == 0);

    /* a worker that failed lands as failed, not review */
    task("t-bad", "dispatched", "s-bad");
    snprintf(path, sizeof path, "%s/t-bad.json", results);
    write_text(path, "{\"task\":\"t-bad\",\"status\":\"failed\",\"summary\":\"it broke\"}\n");
    assert(orch_reconcile() == 1);
    assert(!strcmp(status_of("t-bad"), "failed"));

    /* --- a worker that died without a result ------------------------------- */

    send_n = 0;
    task("t-lost", "dispatched", "s-lost");
    assert(orch_reconcile() == 1);
    assert(!strcmp(status_of("t-lost"), "failed"));
    assert(strstr(last_send, "t-lost"));
    cJSON *rec = orchtask_find("mux", "t-lost", NULL, 0);
    assert(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(rec, "notes")), "without"));
    cJSON_Delete(rec);

    /* a dispatched task whose session is alive is left alone */
    char livepath[1024];
    snprintf(livepath, sizeof livepath, "%s/%ld-0.json", live, (long)getpid());
    char record[512];
    snprintf(record, sizeof record,
             "{\"pid\":%ld,\"slot\":0,\"id\":\"s-busy\",\"status\":\"working\",\"ts\":9}\n",
             (long)getpid());
    write_text(livepath, record);
    task("t-busy", "dispatched", "s-busy");
    assert(orch_reconcile() == 0);
    assert(!strcmp(status_of("t-busy"), "dispatched"));

    /* --- completion events are delivered once, and only once --------------- */

    send_n = 0;
    assert(orchevent_emit("t-busy", "s-busy", "orchestrator", "idle"));
    poll_settled();
    assert(send_n == 1);
    assert(strstr(last_send, "t-busy"));
    struct orch_event *ev = NULL;
    assert(orchevent_pending(&ev) == 0); /* delivered, so acked */
    free(ev);
    poll_settled();
    assert(send_n == 1);

    /* an event for work nothing knows about is dropped rather than retried */
    assert(orchevent_emit("t-ghost", "s-ghost", "orchestrator", "exit"));
    poll_settled();
    assert(orchevent_pending(&ev) == 0);
    free(ev);
    assert(send_n == 1);

    /* an undeliverable event waits instead of being lost */
    send_fails = 1;
    assert(orchevent_emit("t-busy", "s-busy", "orchestrator", "exit"));
    poll_settled();
    assert(orchevent_pending(&ev) == 1);
    free(ev);
    send_fails = 0;
    poll_settled();
    assert(send_n == 2);
    assert(orchevent_pending(&ev) == 0);
    free(ev);

    /* --- requests ---------------------------------------------------------- */

    request("r1", "{\"op\":\"status\"}");
    poll_settled();
    char *res = reply_for("r1");
    assert(res && strstr(res, "sess-orchestrator"));

    request("r2", "{\"op\":\"tell\",\"text\":\"orchestrator: say this\"}");
    poll_settled();
    res = reply_for("r2");
    assert(res && strstr(res, "\"told\":true"));
    assert(!strcmp(last_send, "orchestrator: say this"));

    request("r3", "{\"op\":\"frobnicate\"}");
    poll_settled();
    res = reply_for("r3");
    assert(res && strstr(res, "unknown op"));

    request("r4", "not json at all");
    poll_settled();
    res = reply_for("r4");
    assert(res && strstr(res, "bad json"));

    /* a dispatch of a task already out with a live worker is refused */
    request("r5", "{\"op\":\"dispatch\",\"task\":\"t-busy\",\"prompt\":\"again\"}");
    poll_settled();
    res = reply_for("r5");
    assert(res && strstr(res, "already dispatched"));

    /* a dispatch reaches the instance it picked, and the reply carries through */
    task("t-new", "queued", "");
    request("r6", "{\"op\":\"dispatch\",\"task\":\"t-new\",\"prompt\":\"do it\","
                  "\"backend\":\"claude\",\"model\":\"opus\"}");
    poll_settled();
    assert(!reply_for("r6")); /* still waiting on the worker's session id */
    snprintf(path, sizeof path, "%s/%ld-t-new.req", muxdir, (long)getpid());
    char *req = slurp(path);
    assert(req);
    assert(strstr(req, "\"prompt\":\"do it\""));
    assert(strstr(req, "\"task\":\"t-new\""));
    assert(strstr(req, "\"notify\":\"orchestrator\""));
    unlink(path);
    snprintf(path, sizeof path, "%s/%ld-t-new.res", muxdir, (long)getpid());
    write_text(path, "{\"session\":\"s-new\"}\n");
    poll_settled();
    res = reply_for("r6");
    assert(res && strstr(res, "s-new"));
    assert(!strcmp(status_of("t-new"), "dispatched"));
    rec = orchtask_find("mux", "t-new", NULL, 0);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(rec, "backend")), "claude"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(rec, "session")), "s-new"));
    cJSON_Delete(rec);

    /* a follow-up for a worker that is gone is kept on the task */
    request("r7", "{\"op\":\"send\",\"task\":\"t-lost\",\"text\":\"one more thing\"}");
    poll_settled();
    res = reply_for("r7");
    assert(res && strstr(res, "\"queued\":true"));
    char got[256];
    assert(orchtask_take_pending("mux", "t-lost", got, sizeof got));
    assert(!strcmp(got, "one more thing"));

    /* --- stopping lets another instance have it ---------------------------- */

    orch_stop();
    assert(!orch_label());
    held = filelock_acquire(lockpath, LOCK_EX | LOCK_NB);
    assert(held >= 0);
    filelock_release(held);

    /* and a stopped orchestrator serves nothing */
    request("r8", "{\"op\":\"status\"}");
    poll_settled();
    assert(!reply_for("r8"));

    printf("orchtest: ok\n");
    return 0;
}
