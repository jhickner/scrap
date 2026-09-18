#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "orchstatus.h"

int main(void)
{
    /* what a dispatched task's worker is doing shows beside its status */
    struct orch_rec live = {.status = "dispatched", .live = "working"};
    struct orch_rec quiet = {.status = "dispatched"};
    struct orch_rec queued = {.status = "queued", .live = "working"};
    char status[80];
    orchstatus_status(status, sizeof status, &live);
    assert(!strcmp(status, "dispatched/working"));
    orchstatus_status(status, sizeof status, &quiet);
    assert(!strcmp(status, "dispatched"));
    orchstatus_status(status, sizeof status, &queued);
    assert(!strcmp(status, "queued"));

    /* follow-ups waiting for a worker are part of the state */
    struct orch_rec waiting = {.status = "dispatched", .live = "working", .pending = 2};
    struct orch_rec queued_pending = {.status = "queued", .pending = 1};
    orchstatus_status(status, sizeof status, &waiting);
    assert(!strcmp(status, "dispatched/working +2"));
    orchstatus_status(status, sizeof status, &queued_pending);
    assert(!strcmp(status, "queued +1"));

    /* a checkpoint says so, because it holds up its project */
    char task[560];
    struct orch_rec check = {.id = "t-1", .desc = "settle the design", .checkpoint = 1};
    struct orch_rec plain = {.id = "t-2", .desc = "settle the design"};
    struct orch_rec nameless = {.id = "t-3", .checkpoint = 1};
    orchstatus_task(task, sizeof task, &check);
    assert(!strcmp(task, "checkpoint: settle the design"));
    orchstatus_task(task, sizeof task, &plain);
    assert(!strcmp(task, "settle the design"));
    orchstatus_task(task, sizeof task, &nameless);
    assert(!strcmp(task, "checkpoint: t-3"));

    char agent[220];
    struct orch_rec both = {.backend = "claude", .model = "opus"};
    struct orch_rec one = {.backend = "claude"};
    struct orch_rec neither = {0};
    orchstatus_agent(agent, sizeof agent, &both);
    assert(!strcmp(agent, "claude / opus"));
    orchstatus_agent(agent, sizeof agent, &one);
    assert(!strcmp(agent, "claude"));
    orchstatus_agent(agent, sizeof agent, &neither);
    assert(!strcmp(agent, "-"));

    /* open tasks group by project; with the closed ones, recency alone orders */
    struct orch_rec rows[] = {
        {.project = "zeta",  .id = "t-1", .updated = 100},
        {.project = "alpha", .id = "t-2", .updated = 50},
        {.project = "zeta",  .id = "t-3", .updated = 300},
    };
    orchstatus_sort(rows, 3, 0);
    assert(!strcmp(rows[0].project, "alpha"));
    assert(!strcmp(rows[1].id, "t-3") && !strcmp(rows[2].id, "t-1"));
    orchstatus_sort(rows, 3, 1);
    assert(!strcmp(rows[0].id, "t-3"));
    assert(!strcmp(rows[1].id, "t-1"));
    assert(!strcmp(rows[2].id, "t-2"));

    struct orchstatus_columns widths;
    struct orch_rec row = {
        .project = "mux", .id = "t-3e8c15", .status = "open",
        .backend = "claude", .model = "opus",
    };
    orchstatus_columns(&widths, 80, &row, 1);
    assert(widths.project == 7);
    assert(widths.id == 8);
    assert(widths.status == 6);
    assert(widths.agent == 15);
    assert(widths.age == 3);
    assert(widths.task == 33);
    assert(2 + widths.project + 1 + widths.id + 1 + widths.task + 1 + widths.status
             + 1 + widths.agent + 1 + widths.age == 79);

    struct orch_rec wide = {
        .project = "a-long-project-name", .id = "t-3e8c15",
        .status = "dispatched", .live = "working", .pending = 2, .backend = "claude",
        .model = "claude-opus-5-with-a-long-suffix",
    };
    orchstatus_status(status, sizeof status, &wide);
    orchstatus_columns(&widths, 120, &wide, 1);
    assert(widths.status == (int)strlen(status)); /* the state column is not clipped */

    orchstatus_columns(&widths, 80, &wide, 1);
    assert(widths.project == 16);
    assert(widths.id == 8);
    assert(widths.task == 24);
    assert(2 + widths.project + 1 + widths.id + 1 + widths.task + 1 + widths.status
             + 1 + widths.agent + 1 + widths.age == 79);

    /* the id column keeps its full width however narrow the terminal is */
    orchstatus_columns(&widths, 40, &wide, 1);
    assert(widths.id == 8);
    assert(widths.task >= 4);
    assert(2 + widths.project + 1 + widths.id + 1 + widths.task + 1 + widths.status
             + 1 + widths.agent + 1 + widths.age <= 39);

    char cell[16];
    orchstatus_cell(cell, sizeof cell, "a long\ttask", 8);
    assert(!strcmp(cell, "a lon..."));

    char line[64];
    const char *rest = orchstatus_wrap(line, sizeof line, "fix the task\tcolumn width", 12);
    assert(!strcmp(line, "fix the task"));
    rest = orchstatus_wrap(line, sizeof line, rest, 12);
    assert(!strcmp(line, "column width"));
    assert(!*rest);
    rest = orchstatus_wrap(line, sizeof line, "abcdefghij klm", 4);
    assert(!strcmp(line, "abcd"));
    rest = orchstatus_wrap(line, sizeof line, rest, 4);
    assert(!strcmp(line, "efgh"));

    printf("orchstatustest: ok\n");
    return 0;
}
