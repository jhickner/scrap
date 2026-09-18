#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "orchtarget.h"
#include "orchtask.h"
#include "workspace.h"

static char root[] = "/tmp/mux-orchtarget-XXXXXX";

static void live(long pid, int slot, const char *id, const char *status, long ts)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%ld-%d.json", root, pid, slot);
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "{\"pid\":%ld,\"slot\":%d,\"id\":\"%s\",\"status\":\"%s\",\"ts\":%ld}\n",
            pid, slot, id, status, ts);
    assert(fclose(f) == 0);
}

/* a pid that is certainly not running: claim one and let it go */
static long dead_pid(void)
{
    return 999998;
}

int main(void)
{
    assert(mkdtemp(root));
    setenv("MUX_LIVE_DIR", root, 1);

    long me = (long)getpid();
    long dead = dead_pid();

    /* nothing live: nowhere to dispatch, so the task stays queued */
    assert(orchtarget_pick(0) == 0);
    assert(orchtarget_pick(me) == 0);

    live(me, 0, "s-mine", "finished", 500);
    live(dead, 0, "s-dead", "working", 900);

    struct orch_instance *v = NULL;
    int n = orchtarget_instances(&v);
    assert(n == 1); /* the dead instance is not an instance */
    assert(v[0].pid == me && v[0].sessions == 1 && v[0].ts == 500);
    free(v);

    assert(orchtarget_pick(0) == me);
    assert(orchtarget_pick(me) == me);
    /* a preference that is not alive falls back rather than failing */
    assert(orchtarget_pick(dead) == me);

    /* a session is found in the instance hosting it, and only while alive */
    char status[32];
    long pid = 0;
    assert(orchtarget_session_status("s-mine", status, sizeof status, &pid));
    assert(pid == me && !strcmp(status, "finished"));
    assert(orchtarget_session_pid("s-mine") == me);
    assert(!orchtarget_session_status("s-dead", status, sizeof status, &pid));
    assert(orchtarget_session_pid("s-dead") == 0);
    assert(orchtarget_session_pid("s-nosuch") == 0);
    assert(orchtarget_session_pid(NULL) == 0);

    /* a full instance is skipped */
    for (int slot = 1; slot < WORKSPACE_MAX; slot++) {
        char id[32];
        snprintf(id, sizeof id, "s-%d", slot);
        live(me, slot, id, "working", 500 + slot);
    }
    n = orchtarget_instances(&v);
    assert(n == 1 && v[0].sessions == WORKSPACE_MAX);
    assert(!orchtarget_has_room(&v[0]));
    free(v);
    assert(orchtarget_pick(me) == 0);
    assert(orchtarget_pick(0) == 0);

    /* decorating a loaded task list takes one pass and only counts the living */
    struct orch_rec recs[3] = {0};
    snprintf(recs[0].session, sizeof recs[0].session, "s-mine");
    snprintf(recs[1].session, sizeof recs[1].session, "s-dead");
    snprintf(recs[2].session, sizeof recs[2].session, "");
    snprintf(recs[2].live, sizeof recs[2].live, "stale");
    orchtarget_apply_live(recs, 3);
    assert(!strcmp(recs[0].live, "finished"));
    assert(!recs[1].live[0]); /* its instance is gone */
    assert(!recs[2].live[0]); /* no session, and the old value is cleared */
    orchtarget_apply_live(NULL, 3);
    orchtarget_apply_live(recs, 0);

    printf("orchtargettest: ok\n");
    return 0;
}
