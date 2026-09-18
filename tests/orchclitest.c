#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "orchcli.h"
#include "orchtask.h"

static char root[] = "/tmp/mux-orchcli-XXXXXX";
static char out_path[512];

static int run_argv(int argc, char **argv, char *captured, size_t size)
{
    fflush(stdout);
    fflush(stderr);
    int saved = dup(fileno(stdout));
    int saved_err = dup(fileno(stderr));
    FILE *f = freopen(out_path, "w", stdout);
    assert(f);
    /* the failure cases explain themselves on stderr; keep that out of the
       test's own output */
    assert(freopen("/dev/null", "w", stderr));
    int rc = orchcli_main(argc, argv);
    fflush(stdout);
    fflush(stderr);
    dup2(saved, fileno(stdout));
    dup2(saved_err, fileno(stderr));
    close(saved);
    close(saved_err);
    clearerr(stdout);
    clearerr(stderr);

    captured[0] = '\0';
    FILE *r = fopen(out_path, "r");
    if (r) {
        size_t n = fread(captured, 1, size - 1, r);
        captured[n] = '\0';
        fclose(r);
    }
    return rc;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

int main(void)
{
    assert(mkdtemp(root));
    setenv("ORCHESTRATOR_DIR", root, 1);
    snprintf(out_path, sizeof out_path, "%s/stdout", root);

    char buf[8192];

    /* nothing there yet: an empty state is not an error */
    char *list[] = {"orch", "list"};
    assert(run_argv(2, list, buf, sizeof buf) == 0);
    assert(strstr(buf, "no open tasks"));

    char projects[512], path[1024];
    snprintf(projects, sizeof projects, "%s/projects", root);
    assert(mkdir(projects, 0700) == 0);
    snprintf(path, sizeof path, "%s/registry.json", root);
    write_text(path, "{\"projects\":[{\"name\":\"mux\",\"cwd\":\"/tmp/mux\","
                     "\"aliases\":[\"the repl\"]}]}\n");

    /* add resolves an alias to the real project name and reports the new id */
    char *add[] = {"orch", "add", "the repl", "wire up the thing", "--class", "impl"};
    assert(run_argv(6, add, buf, sizeof buf) == 0);
    char id[ORCHTASK_ID_MAX];
    assert(sscanf(buf, "%63s", id) == 1);
    assert(!strncmp(id, "t-", 2));
    assert(strstr(buf, "mux"));

    /* the human table names the columns and the task */
    assert(run_argv(2, list, buf, sizeof buf) == 0);
    assert(strstr(buf, "PROJECT") && strstr(buf, "STATUS") && strstr(buf, "TASK"));
    assert(strstr(buf, id) && strstr(buf, "wire up the thing"));

    /* --json is machine-readable and carries the same fields */
    char *listj[] = {"orch", "list", "--json"};
    assert(run_argv(3, listj, buf, sizeof buf) == 0);
    cJSON *arr = cJSON_Parse(buf);
    assert(cJSON_IsArray(arr) && cJSON_GetArraySize(arr) == 1);
    cJSON *one = cJSON_GetArrayItem(arr, 0);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(one, "id")), id));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(one, "status")), "queued"));
    cJSON_Delete(arr);

    /* show finds a task without being told its project */
    char *show[] = {"orch", "show", id};
    assert(run_argv(3, show, buf, sizeof buf) == 0);
    cJSON *rec = cJSON_Parse(buf);
    assert(rec);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(rec, "project")), "mux"));
    cJSON_Delete(rec);

    char *missing[] = {"orch", "show", "t-nosuch"};
    assert(run_argv(3, missing, buf, sizeof buf) == 1);

    /* transitions, notes and follow-ups all land in the state files */
    char *review[] = {"orch", "status", id, "review"};
    assert(run_argv(4, review, buf, sizeof buf) == 0);
    char *done[] = {"orch", "status", id, "done"};
    assert(run_argv(4, done, buf, sizeof buf) == 0);
    char *reopen[] = {"orch", "status", id, "queued"};
    assert(run_argv(4, reopen, buf, sizeof buf) == 1); /* done is where it stops */

    char *note[] = {"orch", "note", id, "checked by hand"};
    assert(run_argv(4, note, buf, sizeof buf) == 0);
    char *follow[] = {"orch", "followup", id, "also update the docs"};
    assert(run_argv(4, follow, buf, sizeof buf) == 0);

    cJSON *saved = orchtask_find("mux", id, NULL, 0);
    assert(saved);
    assert(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(saved, "notes")), "by hand"));
    assert(cJSON_GetArraySize(cJSON_GetObjectItem(saved, "pending")) == 1);
    cJSON_Delete(saved);

    /* a done task drops out of the open list but not out of `list all` */
    assert(run_argv(2, list, buf, sizeof buf) == 0);
    assert(strstr(buf, "no open tasks"));
    char *all[] = {"orch", "list", "all"};
    assert(run_argv(3, all, buf, sizeof buf) == 0);
    assert(strstr(buf, id));

    /* projects reports what the registry knows */
    char *projs[] = {"orch", "projects"};
    assert(run_argv(2, projs, buf, sizeof buf) == 0);
    assert(strstr(buf, "mux") && strstr(buf, "/tmp/mux"));

    /* the verbs that need a live mux say so rather than hanging or pretending */
    char *reconcile[] = {"orch", "reconcile"};
    assert(run_argv(2, reconcile, buf, sizeof buf) == 1);
    char *dispatch[] = {"orch", "dispatch", id, "go and do it"};
    assert(run_argv(4, dispatch, buf, sizeof buf) == 1);

    /* an unknown verb and a verb missing its arguments are usage errors */
    char *bogus[] = {"orch", "frobnicate"};
    assert(run_argv(2, bogus, buf, sizeof buf) == 2);
    char *bare[] = {"orch"};
    assert(run_argv(1, bare, buf, sizeof buf) == 2);
    char *shortadd[] = {"orch", "add", "mux"};
    assert(run_argv(3, shortadd, buf, sizeof buf) == 2);

    printf("orchclitest: ok\n");
    return 0;
}
