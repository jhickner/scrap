#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "orchevent.h"

static char root[] = "/tmp/mux-orchevent-XXXXXX";

int main(void)
{
    assert(mkdtemp(root));
    setenv("ORCHESTRATOR_DIR", root, 1);

    struct orch_event *v = NULL;
    assert(orchevent_pending(&v) == 0);
    free(v);

    assert(orchevent_emit("t-aaa", "s-1", "s-orch", "idle"));
    assert(orchevent_pending(&v) == 1);
    assert(!strcmp(v[0].task, "t-aaa"));
    assert(!strcmp(v[0].session, "s-1"));
    assert(!strcmp(v[0].notify, "s-orch"));
    assert(!strcmp(v[0].reason, "idle"));
    assert(v[0].ts > 0);
    char first[ORCHEVENT_ID_MAX];
    snprintf(first, sizeof first, "%s", v[0].id);
    free(v);

    /* a repeat of the same completion is not a second completion */
    assert(orchevent_emit("t-aaa", "s-1", "s-orch", "idle"));
    assert(orchevent_emit("t-aaa", "s-1", "s-orch", "idle"));
    assert(orchevent_pending(&v) == 1);
    free(v);

    /* a different reason for the same task is its own event, as is another task */
    assert(orchevent_emit("t-aaa", "s-1", "s-orch", "exit"));
    assert(orchevent_emit("t-bbb", "s-2", "s-orch", "idle"));
    assert(orchevent_pending(&v) == 3);
    free(v);

    /* nothing to say is not an event */
    assert(!orchevent_emit(NULL, "s-1", "s-orch", "idle"));
    assert(!orchevent_emit("t-ccc", "s-1", "s-orch", NULL));
    assert(!orchevent_emit("", "s-1", "s-orch", "idle"));
    assert(orchevent_pending(&v) == 3);
    free(v);

    /* an ack drops exactly one, and only once */
    assert(orchevent_ack(first));
    assert(!orchevent_ack(first));
    assert(orchevent_pending(&v) == 2);
    for (int i = 0; i < 2; i++)
        assert(strcmp(v[i].id, first));
    free(v);

    /* acking one that was never there changes nothing */
    assert(!orchevent_ack("no-such-event"));
    assert(!orchevent_ack(NULL));
    assert(!orchevent_ack("../escape"));
    assert(orchevent_pending(&v) == 2);
    free(v);

    /* the queue is on disk: a fresh process sees the same backlog. after the
       ack above, the same completion can be reported again */
    assert(orchevent_emit("t-aaa", "s-1", "s-orch", "idle"));
    int n = orchevent_pending(&v);
    assert(n == 3);
    free(v);
    if (fork() == 0) {
        struct orch_event *mine = NULL;
        int count = orchevent_pending(&mine);
        _exit(count == 3 ? 0 : 1);
    }
    int status = 0;
    assert(wait(&status) > 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    printf("orcheventtest: ok\n");
    return 0;
}
