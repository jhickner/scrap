#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "orchtask.h"

static char root[] = "/tmp/mux-orchtask-XXXXXX";

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static int line_count(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    int n = 0, c;
    while ((c = fgetc(f)) != EOF)
        n += c == '\n';
    fclose(f);
    return n;
}

static const char *status_of(const struct orch_rec *recs, int n, const char *id)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(recs[i].id, id))
            return recs[i].status;
    return NULL;
}

int main(void)
{
    assert(mkdtemp(root));
    setenv("ORCHESTRATOR_DIR", root, 1);

    char projects[512], path[1024], reg[1024];
    snprintf(projects, sizeof projects, "%s/projects", root);
    assert(mkdir(projects, 0700) == 0);

    snprintf(path, sizeof path, "%s/mux.jsonl", projects);
    write_text(path,
        "{\"id\":\"t-aaa\",\"desc\":\"first\",\"status\":\"queued\",\"created\":100,\"updated\":100,"
        "\"deps\":[\"t-zzz\"],\"cost_usd\":0.5}\n"
        "garbage not json\n"
        "{\"id\":\"t-aaa\",\"desc\":\"first\",\"status\":\"dispatched\",\"created\":100,"
        "\"updated\":200,\"deps\":[\"t-zzz\"],\"cost_usd\":0.5,\"session\":\"s-1\"}\n"
        "{\"id\":\"t-bbb\",\"desc\":\"shipped\",\"status\":\"done\",\"created\":90,\"updated\":300}\n"
        "{\"id\":\"t-ccc\",\"desc\":\"dropped\",\"status\":\"cancelled\",\"created\":80,\"updated\":310}\n"
        "{\"id\":\"t-par");  /* a torn final record, mid-write */
    snprintf(path, sizeof path, "%s/rmchores.jsonl", projects);
    write_text(path,
        "{\"id\":\"t-ddd\",\"desc\":\"other project\",\"status\":\"queued\",\"created\":50,\"updated\":50}\n");

    snprintf(reg, sizeof reg, "%s/registry.json", root);
    write_text(reg,
        "{\"projects\":[{\"name\":\"mux\",\"cwd\":\"/tmp/mux\",\"aliases\":[\"the repl\"]},"
        "{\"name\":\"rmchores\",\"cwd\":\"/tmp/rmchores\"}]}\n");

    /* newest record per id wins; a torn line and a bad line are skipped */
    struct orch_rec *recs = NULL;
    int n = orchtask_load("mux", 0, &recs);
    assert(n == 1);
    assert(!strcmp(recs[0].id, "t-aaa"));
    assert(!strcmp(recs[0].status, "dispatched"));
    assert(!strcmp(recs[0].session, "s-1"));
    free(recs);

    n = orchtask_load("mux", 1, &recs);
    assert(n == 3);
    free(recs);

    /* every project at once, newest first */
    n = orchtask_load(NULL, 0, &recs);
    assert(n == 2);
    assert(!strcmp(recs[0].id, "t-aaa"));
    assert(!strcmp(recs[1].id, "t-ddd"));
    assert(!strcmp(recs[1].project, "rmchores"));
    free(recs);

    /* name resolution: exact, then alias, case-insensitively */
    char cwd[512], real[128];
    assert(orchtask_project_cwd("mux", cwd, sizeof cwd, real, sizeof real));
    assert(!strcmp(cwd, "/tmp/mux") && !strcmp(real, "mux"));
    assert(orchtask_project_cwd("THE REPL", cwd, sizeof cwd, real, sizeof real));
    assert(!strcmp(real, "mux"));
    assert(!orchtask_project_cwd("nope", cwd, sizeof cwd, real, sizeof real));

    /* a transition keeps fields it does not know about, and appends */
    snprintf(path, sizeof path, "%s/mux.jsonl", projects);
    int before = line_count(path);
    assert(orchtask_set_status("mux", "t-aaa", "review", NULL, NULL, NULL, NULL, 0));
    /* the torn line gets its newline back, then the new record lands whole */
    assert(line_count(path) == before + 2);
    assert(orchtask_set_status("mux", "t-aaa", "review", NULL, NULL, NULL, NULL, 0));
    assert(line_count(path) == before + 3);
    cJSON *rec = orchtask_find("mux", "t-aaa", NULL, 0);
    assert(rec);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(rec, "status")), "review"));
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(rec, "cost_usd")) == 0.5);
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(rec, "deps")) == 1);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItem(rec, "updated")) > 200);
    cJSON_Delete(rec);

    /* done is terminal */
    assert(orchtask_set_status("mux", "t-aaa", "done", NULL, NULL, NULL, NULL, 0));
    assert(!orchtask_set_status("mux", "t-aaa", "queued", NULL, NULL, NULL, NULL, 0));
    /* cancelled is not */
    assert(orchtask_set_status("mux", "t-ccc", "queued", NULL, NULL, NULL, NULL, 0));

    /* create picks a free id and lands queued */
    char id[ORCHTASK_ID_MAX];
    assert(orchtask_create("mux", "brand new", "planning", 1, 4242, "why", id, sizeof id));
    assert(!strncmp(id, "t-", 2));
    n = orchtask_load("mux", 0, &recs);
    assert(!strcmp(status_of(recs, n, id), "queued"));
    for (int i = 0; i < n; i++)
        if (!strcmp(recs[i].id, id)) {
            assert(recs[i].checkpoint == 1);
            assert(recs[i].pid == 4242);
            assert(!strcmp(recs[i].klass, "planning"));
        }
    free(recs);

    /* follow-ups queue and come back out in order, once each */
    char got[256];
    assert(!orchtask_take_pending("mux", id, got, sizeof got));
    assert(orchtask_add_pending("mux", id, "also do the other thing"));
    assert(orchtask_add_pending("mux", id, "and then this"));
    assert(orchtask_take_pending("mux", id, got, sizeof got));
    assert(!strcmp(got, "also do the other thing"));
    assert(orchtask_take_pending("mux", id, got, sizeof got));
    assert(!strcmp(got, "and then this"));
    assert(!orchtask_take_pending("mux", id, got, sizeof got));

    /* notes accumulate rather than replace */
    assert(orchtask_note("mux", id, "one"));
    assert(orchtask_note("mux", id, "two"));
    rec = orchtask_find("mux", id, NULL, 0);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(rec, "notes")), "why\none\ntwo"));
    cJSON_Delete(rec);

    /* a task can be found without naming its project */
    char where[128];
    rec = orchtask_find(NULL, "t-ddd", where, sizeof where);
    assert(rec && !strcmp(where, "rmchores"));
    cJSON_Delete(rec);
    assert(!orchtask_find(NULL, "t-nosuch", where, sizeof where));

    /* result files */
    char results[512];
    snprintf(results, sizeof results, "%s/results", root);
    assert(mkdir(results, 0700) == 0);
    snprintf(path, sizeof path, "%s/t-aaa.json", results);
    write_text(path, "{\"task\":\"t-aaa\",\"status\":\"done\",\"summary\":\"did it\"}\n");
    cJSON *res = orchtask_result_read("t-aaa");
    assert(res);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(res, "summary")), "did it"));
    cJSON_Delete(res);
    assert(orchtask_result_drop("t-aaa"));
    assert(!orchtask_result_read("t-aaa"));
    assert(!orchtask_result_drop("t-aaa"));

    /* concurrent appends under the lock: every line stays whole */
    snprintf(path, sizeof path, "%s/rmchores.jsonl", projects);
    before = line_count(path);
    for (int child = 0; child < 4; child++)
        if (fork() == 0) {
            for (int i = 0; i < 25; i++)
                orchtask_note("rmchores", "t-ddd", "a note long enough to straddle a write");
            _exit(0);
        }
    for (int child = 0; child < 4; child++)
        wait(NULL);
    assert(line_count(path) == before + 100);
    n = orchtask_load("rmchores", 1, &recs);
    assert(n == 1); /* 100 whole records, all parseable, all the same id */
    free(recs);

    /* log lines land */
    orchtask_log("dispatch", "mux", "t-aaa", "to claude");
    snprintf(path, sizeof path, "%s/log.jsonl", root);
    assert(line_count(path) >= 1);

    /* the closed statuses, and the age the tables print */
    assert(orchtask_closed("done"));
    assert(orchtask_closed("cancelled"));
    assert(!orchtask_closed("failed"));
    assert(!orchtask_closed("queued"));
    assert(!orchtask_closed(NULL));

    char age[16];
    orchtask_age(age, sizeof age, 100, 145);
    assert(!strcmp(age, "45s"));
    orchtask_age(age, sizeof age, 100, 3700);
    assert(!strcmp(age, "1h"));
    orchtask_age(age, sizeof age, 100, 3 * 24 * 60 * 60 + 100);
    assert(!strcmp(age, "3d"));
    orchtask_age(age, sizeof age, 0, 0);
    assert(!strcmp(age, "0s"));

    printf("orchtasktest: ok\n");
    return 0;
}
