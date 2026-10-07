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

static void spawned(struct tasktab *t, const char *id, const char *tool_use, const char *desc)
{
    backend_event ev = {.kind = BACKEND_EV_TASK, .id = id, .name = "running",
                        .text = desc, .parent = tool_use};
    tasks_note(t, &ev, NULL);
}

static void labels(const char *desc, const char *want)
{
    struct tasktab t;
    char           got[32];

    tasks_reset(&t, "claude");
    spawned(&t, "x", "toolu_x", desc);
    tasks_label(tasks_by_parent(&t, "toolu_x"), got, sizeof got);
    expect_text(got, want, desc);
}

static void by_parent(void)
{
    struct tasktab t;
    tasks_reset(&t, "claude");

    spawned(&t, "a", "toolu_A", "Read the battery state");
    spawned(&t, "b", "toolu_B", "Watch the network come back");

    expect(tasks_by_parent(&t, "toolu_A") == tasks_at(&t, 0), 1, "first call's task");
    expect(tasks_by_parent(&t, "toolu_B") == tasks_at(&t, 1), 1, "second call's task");
    expect(tasks_by_parent(&t, "toolu_Z") == NULL, 1, "a call that started none");
    expect(tasks_by_parent(&t, "") == NULL, 1, "no id is not a match");

    labels("Read the battery state", "read battery");
    labels("Watch the network come back", "watch network");
    labels("Run echo command and report output", "run echo");
    labels("Investigate", "investigate");

    labels("Extraordinarily long single word here", "extraordinaril");
}

static const struct task *tool(struct tasktab *t, const char *name, const char *input)
{
    backend_event ev = {.kind = BACKEND_EV_TOOL, .name = name, .input_json = input};
    return tasks_note(t, &ev, NULL);
}

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

    char line[240];
    tasks_line(a, line, sizeof line, NULL, NULL);
    expect(strncmp(line, "[agent completed] review the diff in ", 37) == 0, 1,
           "line names the status and the task");

    int repeat = 0;
    life(&t, "t1", "completed", NULL, &repeat);
    expect(repeat, 0, "one repeat is not a loop");
    life(&t, "t1", "completed", NULL, &repeat);
    expect(repeat, 1, "a task reporting after the end is a repeat");
}

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

    life(&t, "t9", "running", "the real thing", NULL);
    expect(tool(&t, "Task", "{\"description\":\"another\"}") == NULL, 1,
           "spawn calls are ignored once life cycle arrives");
    expect(tasks_count(&t), 2, "no entry invented for it");
}

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

static void kind_line(const char *task_type, const char *arg, const char *want)
{
    struct tasktab t;
    char           line[240];

    tasks_reset(&t, "claude");
    backend_event ev = {.kind = BACKEND_EV_TASK, .id = "k", .name = "completed",
                        .text = "x", .arg = arg, .task_type = task_type};
    tasks_line(tasks_note(&t, &ev, NULL), line, sizeof line, NULL, NULL);
    expect(strncmp(line, want, strlen(want)) == 0, 1, want);
}

static void kinds(void)
{
    kind_line("local_bash", NULL, "[bash completed] ");
    kind_line("local_agent", "Explore", "[agent explore completed] ");
    kind_line("local_workflow", "review_changes", "[workflow review_changes completed] ");
    kind_line("monitor_mcp", NULL, "[monitor completed] ");
    kind_line("in_process_teammate", NULL, "[teammate completed] ");
    kind_line("new_kind", NULL, "[new kind completed] ");
    kind_line(NULL, NULL, "[agent completed] ");
}

int main(void)
{
    lifecycle();
    kinds();
    inferred();
    eviction();
    by_parent();

    if (failures)
        printf("%d failure(s)\n", failures);
    else
        printf("ok\n");
    return failures ? 1 : 0;
}
