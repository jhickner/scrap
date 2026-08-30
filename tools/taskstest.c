#include <stdio.h>
#include <string.h>

#include "tasks.h"

static int failures;

static void expect(int got, int want, const char *what)
{
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

static void expect_text(const char *got, const char *want, const char *what)
{
    if (!got || strcmp(got, want)) {
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)", want);
        failures++;
    }
}

static const struct task *life(struct tasktab *t, const char *id, const char *status,
                               const char *text, int *repeat)
{
    backend_event ev = {.kind = BACKEND_EV_TASK, .id = id, .name = status, .text = text};
    return tasks_note(t, &ev, repeat);
}

static const struct task *tool(struct tasktab *t, const char *name, const char *input)
{
    backend_event ev = {.kind = BACKEND_EV_TOOL, .name = name, .input_json = input};
    return tasks_note(t, &ev, NULL);
}

/* A backend that reports the life cycle: one entry per task, a line only when
   its state changes, and the end of the work is knowable. */
static void lifecycle(void)
{
    struct tasktab t;
    tasks_reset(&t, "claude");

    const struct task *a = life(&t, "t1", "running", "review the diff", NULL);
    expect(a != NULL, 1, "started reports a change");
    expect(tasks_count(&t), 1, "one task tracked");
    expect(tasks_pending(&t), 1, "one pending");
    expect_text(a ? a->desc : NULL, "review the diff", "description kept");

    expect(life(&t, "t1", "running", "still going", NULL) == NULL, 1,
           "same status is not a change");
    expect(tasks_count(&t), 1, "no duplicate entry");

    a = life(&t, "t1", "completed", "found two bugs", NULL);
    expect(a != NULL, 1, "completion reports a change");
    expect(tasks_done(a), 1, "completed counts as done");
    expect(tasks_running(&t), 0, "nothing running");
    expect(tasks_pending(&t), 0, "nothing pending");
    expect(a && a->ended != 0, 1, "end stamped");

    char line[240];
    tasks_line(a, line, sizeof line);
    expect(strncmp(line, "agent completed: review the diff in ", 35) == 0, 1,
           "line names the status and the task");

    int repeat = 0;
    life(&t, "t1", "completed", NULL, &repeat);
    expect(repeat, 0, "one repeat is not a loop");
    life(&t, "t1", "completed", NULL, &repeat);
    expect(repeat, 1, "a task reporting after the end is a repeat");
}

/* A backend with no life cycle: the spawn call is all there is, so the entries
   never close and nothing may wait on them. */
static void inferred(void)
{
    struct tasktab t;
    tasks_reset(&t, "codex");

    expect(tool(&t, "read", "{\"file_path\":\"src/ui.c\"}") == NULL, 1,
           "an ordinary tool is not a task");

    const struct task *a = tool(&t, "task", "{\"description\":\"find the leak\"}");
    expect(a != NULL, 1, "a spawn call is tracked");
    expect_text(a ? a->desc : NULL, "find the leak", "description from the input");
    expect(tasks_running(&t), 0, "an inferred task has no open state to report");
    expect(tasks_pending(&t), 0, "and nothing waits on it");

    /* once the backend reports a life cycle, the guess is off */
    life(&t, "t9", "running", "the real thing", NULL);
    expect(tool(&t, "Task", "{\"description\":\"another\"}") == NULL, 1,
           "spawn calls are ignored once life cycle arrives");
    expect(tasks_count(&t), 2, "no entry invented for it");
}

/* A full table gives up finished entries first: what is still running is what a
   caller waiting on the work needs. */
static void eviction(void)
{
    struct tasktab t;
    tasks_reset(&t, "claude");

    char id[16];
    for (int i = 0; i < TASKS_MAX; i++) {
        snprintf(id, sizeof id, "t%d", i);
        life(&t, id, "running", "work", NULL);
        if (i < TASKS_MAX - 2)
            life(&t, id, "completed", NULL, NULL);
    }
    expect(tasks_count(&t), TASKS_MAX, "table full");
    expect(tasks_pending(&t), 2, "two still running");

    life(&t, "fresh", "running", "work", NULL);
    expect(tasks_count(&t), TASKS_MAX, "still full");
    expect(tasks_pending(&t), 3, "the running ones survived");
    expect_text(tasks_at(&t, 0)->id, "t1", "the oldest finished entry went");
}

int main(void)
{
    lifecycle();
    inferred();
    eviction();

    if (failures)
        printf("%d failure(s)\n", failures);
    else
        printf("ok\n");
    return failures ? 1 : 0;
}
