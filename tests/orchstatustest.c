#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "orchstatus.h"

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

int main(void)
{
    char root[] = "/tmp/mux-orchstatus-XXXXXX";
    assert(mkdtemp(root));
    char projects[512], live[512], path[1024];
    snprintf(projects, sizeof projects, "%s/projects", root);
    snprintf(live, sizeof live, "%s/live", root);
    assert(mkdir(projects, 0700) == 0);
    assert(mkdir(live, 0700) == 0);

    snprintf(path, sizeof path, "%s/zeta.jsonl", projects);
    write_text(path,
        "{\"id\":\"t-one\",\"desc\":\"old text\",\"status\":\"queued\",\"created\":100,\"updated\":100}\n"
        "not json\n"
        "{\"id\":\"t-one\",\"desc\":\"new text\",\"status\":\"dispatched\",\"created\":100,\"updated\":300,\"backend\":\"codex\",\"model\":\"sol\",\"session\":\"session-one\"}\n"
        "{\"id\":\"t-done\",\"desc\":\"complete\",\"status\":\"done\",\"created\":90,\"updated\":400}\n");
    snprintf(path, sizeof path, "%s/alpha.jsonl", projects);
    write_text(path,
        "{\"id\":\"t-two\",\"desc\":\"queued work\",\"status\":\"queued\",\"created\":200,\"updated\":200}\n");
    snprintf(path, sizeof path, "%s/123-0.json", live);
    char record[512];
    snprintf(record, sizeof record,
             "{\"pid\":%ld,\"id\":\"session-one\",\"status\":\"working\"}\n",
             (long)getpid());
    write_text(path, record);

    struct orch_task *tasks = NULL;
    int count = orchstatus_load(projects, live, 0, &tasks);
    assert(count == 2);
    assert(!strcmp(tasks[0].project, "alpha"));
    assert(!strcmp(tasks[1].project, "zeta"));
    assert(!strcmp(tasks[1].desc, "new text"));
    assert(!strcmp(tasks[1].backend, "codex"));
    assert(!strcmp(tasks[1].live_status, "working"));
    free(tasks);

    count = orchstatus_load(projects, live, 1, &tasks);
    assert(count == 3);
    assert(!strcmp(tasks[0].id, "t-done"));
    assert(!strcmp(tasks[1].id, "t-one"));
    assert(!strcmp(tasks[2].id, "t-two"));
    free(tasks);

    char age[16];
    orchstatus_age(age, sizeof age, 100, 145);
    assert(!strcmp(age, "45s"));
    orchstatus_age(age, sizeof age, 100, 3700);
    assert(!strcmp(age, "1h"));
    orchstatus_age(age, sizeof age, 100, 3 * 24 * 60 * 60 + 100);
    assert(!strcmp(age, "3d"));

    struct orchstatus_columns widths;
    struct orch_task row = {
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

    struct orch_task wide = {
        .project = "a-long-project-name", .id = "t-3e8c15",
        .status = "dispatched", .live_status = "working", .backend = "claude",
        .model = "claude-opus-5-with-a-long-suffix",
    };
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

    snprintf(path, sizeof path, "%s/123-0.json", live);
    assert(unlink(path) == 0);
    snprintf(path, sizeof path, "%s/zeta.jsonl", projects);
    assert(unlink(path) == 0);
    snprintf(path, sizeof path, "%s/alpha.jsonl", projects);
    assert(unlink(path) == 0);
    assert(rmdir(live) == 0);
    assert(rmdir(projects) == 0);
    assert(rmdir(root) == 0);
    return 0;
}
