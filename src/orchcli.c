#include "orchcli.h"

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
#include "orchtarget.h"
#include "orchtask.h"
#include "text.h"
#include "vendor/cJSON.h"

#define REPLY_WAIT_MS 45000
#define REPLY_STEP_MS 100
#define REQUEST_MAX   65536

static int flag(int *argc, char **argv, const char *name)
{
    for (int i = 1; i < *argc; i++)
        if (!strcmp(argv[i], name)) {
            for (int j = i; j < *argc - 1; j++)
                argv[j] = argv[j + 1];
            (*argc)--;
            return 1;
        }
    return 0;
}

static const char *option(int *argc, char **argv, const char *name)
{
    for (int i = 1; i < *argc - 1; i++)
        if (!strcmp(argv[i], name)) {
            const char *value = argv[i + 1];
            for (int j = i; j < *argc - 2; j++)
                argv[j] = argv[j + 2];
            *argc -= 2;
            return value;
        }
    return NULL;
}

static int usage(void)
{
    fprintf(stderr,
        "usage: " APP_NAME " orch <verb>\n"
        "\n"
        "  list [all]              open tasks, or every task\n"
        "  show <task>             one task, in full\n"
        "  projects                known projects and their directories\n"
        "  add <project> <text>    a new queued task\n"
        "  status <task> <status>  queued|dispatched|review|done|failed|paused|cancelled\n"
        "  note <task> <text>      append to the task's notes\n"
        "  followup <task> <text>  queue an instruction for the task's worker\n"
        "  dispatch <task> <prompt>  start a worker (needs a running mux)\n"
        "  send <task> <text>      type at the task's live worker\n"
        "  tell <text>             say something to the orchestrator session\n"
        "  reconcile               fold in results now\n"
        "\n"
        "  --json                  machine-readable output where it applies\n"
        "  --project <name>        disambiguate a task id\n"
        "  --class, --backend, --model, --cwd, --title, --checkpoint\n");
    return 2;
}

/* Nobody holds the lock means nobody is running the orchestrator. Taking it
   here for a moment is the test; it is dropped before returning either way. */
static int owner_running(void)
{
    char path[4300];
    if (!orchtask_lockpath(path, sizeof path))
        return 0;
    int fd = filelock_acquire(path, LOCK_EX | LOCK_NB);
    if (fd < 0)
        return errno == EWOULDBLOCK || errno == EAGAIN;
    filelock_release(fd);
    return 0;
}

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

/* The owner may be any instance, so the request is addressed to whoever has it
   ("any-") and the owner renames it to itself. The reply keeps the key, so it
   is found by its tail rather than by a pid. */
static char *take_reply(const char *dir, const char *key)
{
    char tail[300];
    snprintf(tail, sizeof tail, "-%s.res", key);
    size_t tlen = strlen(tail);

    for (int waited = 0; waited < REPLY_WAIT_MS; waited += REPLY_STEP_MS) {
        DIR *d = opendir(dir);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                size_t len = strlen(e->d_name);
                if (len <= tlen || strcmp(e->d_name + len - tlen, tail))
                    continue;
                char path[4400];
                snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
                char *text = text_slurp(path, REQUEST_MAX, NULL);
                unlink(path);
                closedir(d);
                return text;
            }
            closedir(d);
        }
        struct timespec ts = {0, REPLY_STEP_MS * 1000 * 1000L};
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static int ask_owner(cJSON *req, int as_json)
{
    int rc = 1;
    char dir[4200];
    if (!requests_dir(dir, sizeof dir)) {
        fprintf(stderr, APP_NAME ": no orchestrator state directory\n");
        cJSON_Delete(req);
        return 1;
    }
    if (!owner_running()) {
        fprintf(stderr, APP_NAME ": no mux instance is running the orchestrator "
                        "(start one with --orchestrator or /orchestrator)\n");
        cJSON_Delete(req);
        return 1;
    }

    char key[64];
    snprintf(key, sizeof key, "cli%ld-%ld", (long)getpid(), (long)time(NULL));
    char path[4400], tmp[4500];
    snprintf(path, sizeof path, "%s/any-%s.req", dir, key);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);

    char *text = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!text)
        return 1;
    FILE *f = fopen(tmp, "w");
    if (!f) {
        free(text);
        fprintf(stderr, APP_NAME ": could not write the request\n");
        return 1;
    }
    fputs(text, f);
    fputc('\n', f);
    fclose(f);
    free(text);
    rename(tmp, path);

    char *reply = take_reply(dir, key);
    if (!reply) {
        unlink(path);
        fprintf(stderr, APP_NAME ": the orchestrator did not answer\n");
        return 1;
    }

    cJSON *o = cJSON_Parse(reply);
    const char *err = o ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "error"))
                        : "unreadable reply";
    if (err)
        fprintf(stderr, APP_NAME ": %s\n", err);
    else {
        rc = 0;
        if (as_json)
            fputs(reply, stdout);
        else {
            char *pretty = o ? cJSON_Print(o) : NULL;
            printf("%s\n", pretty ? pretty : reply);
            free(pretty);
        }
    }
    cJSON_Delete(o);
    free(reply);
    return rc;
}

/* ---- verbs --------------------------------------------------------------- */

static int do_list(int argc, char **argv, int as_json, const char *project)
{
    int all = argc > 1 && !strcmp(argv[1], "all");
    struct orch_rec *recs = NULL;
    int n = orchtask_load(project, all, &recs);
    if (n < 0) {
        fprintf(stderr, APP_NAME ": could not read the orchestrator tasks\n");
        return 1;
    }
    if (as_json)
        orchtask_print_json(stdout, recs, n);
    else if (!n)
        printf("no %stasks\n", all ? "" : "open ");
    else
        orchtask_print_table(stdout, recs, n, time(NULL));
    free(recs);
    return 0;
}

static int do_show(int argc, char **argv, const char *project)
{
    if (argc < 2)
        return usage();
    char found[128];
    cJSON *rec = orchtask_find(project, argv[1], found, sizeof found);
    if (!rec) {
        fprintf(stderr, APP_NAME ": no such task\n");
        return 1;
    }
    cJSON_AddStringToObject(rec, "project", found);
    const char *session = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(rec,
                                                                                "session"));
    char status[32];
    long pid = 0;
    if (session && orchtarget_session_status(session, status, sizeof status, &pid)) {
        cJSON_AddStringToObject(rec, "live", status);
        cJSON_AddNumberToObject(rec, "live_pid", (double)pid);
    }
    orchtask_print_one(stdout, rec);
    cJSON_Delete(rec);
    return 0;
}

static int do_projects(int as_json)
{
    char **names = NULL;
    int n = orchtask_projects(&names);
    if (n < 0) {
        fprintf(stderr, APP_NAME ": could not read the orchestrator projects\n");
        return 1;
    }
    cJSON *arr = as_json ? cJSON_CreateArray() : NULL;
    for (int i = 0; i < n; i++) {
        char cwd[4200] = "";
        orchtask_project_cwd(names[i], cwd, sizeof cwd, NULL, 0);
        if (arr) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", names[i]);
            cJSON_AddStringToObject(o, "cwd", cwd);
            cJSON_AddItemToArray(arr, o);
        } else {
            printf("%-20s %s\n", names[i], cwd);
        }
        free(names[i]);
    }
    free(names);
    if (arr) {
        char *text = cJSON_PrintUnformatted(arr);
        if (text)
            printf("%s\n", text);
        free(text);
        cJSON_Delete(arr);
    }
    return 0;
}

static int do_add(int argc, char **argv, const char *klass, int checkpoint,
                  const char *notes, int as_json)
{
    if (argc < 3)
        return usage();
    const char *project = argv[1];
    char real[128];
    char cwd[4200];
    if (orchtask_project_cwd(project, cwd, sizeof cwd, real, sizeof real))
        project = real;

    char id[ORCHTASK_ID_MAX];
    long pid = 0;
    const char *env = getenv("MUX_PID");
    if (env)
        pid = strtol(env, NULL, 10);
    if (!orchtask_create(project, argv[2], klass, checkpoint, pid, notes, id, sizeof id)) {
        fprintf(stderr, APP_NAME ": could not add the task\n");
        return 1;
    }
    if (as_json)
        printf("{\"task\":\"%s\",\"project\":\"%s\"}\n", id, project);
    else
        printf("%s %s\n", id, project);
    return 0;
}

static int do_status(int argc, char **argv, const char *project)
{
    if (argc < 3)
        return usage();
    if (!orchtask_set_status(project, argv[1], argv[2], NULL, NULL, NULL, NULL, 0)) {
        fprintf(stderr, APP_NAME ": could not move %s to %s "
                        "(no such task, or done is where it stops)\n", argv[1], argv[2]);
        return 1;
    }
    orchtask_log("status", project, argv[1], argv[2]);
    printf("%s %s\n", argv[1], argv[2]);
    return 0;
}

static int do_note(int argc, char **argv, const char *project)
{
    if (argc < 3)
        return usage();
    if (!orchtask_note(project, argv[1], argv[2])) {
        fprintf(stderr, APP_NAME ": no such task\n");
        return 1;
    }
    return 0;
}

static int do_followup(int argc, char **argv, const char *project)
{
    if (argc < 3)
        return usage();
    if (!orchtask_add_pending(project, argv[1], argv[2])) {
        fprintf(stderr, APP_NAME ": no such task\n");
        return 1;
    }
    printf("queued for %s\n", argv[1]);
    return 0;
}

int orchcli_main(int argc, char **argv)
{
    if (argc < 2)
        return usage();

    int as_json = flag(&argc, argv, "--json");
    int checkpoint = flag(&argc, argv, "--checkpoint");
    const char *project = option(&argc, argv, "--project");
    const char *klass = option(&argc, argv, "--class");
    const char *backend = option(&argc, argv, "--backend");
    const char *model = option(&argc, argv, "--model");
    const char *cwd = option(&argc, argv, "--cwd");
    const char *title = option(&argc, argv, "--title");
    const char *effort = option(&argc, argv, "--effort");
    const char *notes = option(&argc, argv, "--notes");
    const char *verb = argv[1];
    argv++;
    argc--;

    if (!strcmp(verb, "list"))
        return do_list(argc, argv, as_json, project);
    if (!strcmp(verb, "show"))
        return do_show(argc, argv, project);
    if (!strcmp(verb, "projects"))
        return do_projects(as_json);
    if (!strcmp(verb, "add"))
        return do_add(argc, argv, klass, checkpoint, notes, as_json);
    if (!strcmp(verb, "status"))
        return do_status(argc, argv, project);
    if (!strcmp(verb, "note"))
        return do_note(argc, argv, project);
    if (!strcmp(verb, "followup"))
        return do_followup(argc, argv, project);

    if (!strcmp(verb, "dispatch")) {
        if (argc < 3)
            return usage();
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "op", "dispatch");
        cJSON_AddStringToObject(req, "task", argv[1]);
        cJSON_AddStringToObject(req, "prompt", argv[2]);
        if (project)
            cJSON_AddStringToObject(req, "project", project);
        if (backend)
            cJSON_AddStringToObject(req, "backend", backend);
        if (model)
            cJSON_AddStringToObject(req, "model", model);
        if (cwd)
            cJSON_AddStringToObject(req, "cwd", cwd);
        if (effort)
            cJSON_AddStringToObject(req, "effort", effort);
        if (title)
            cJSON_AddStringToObject(req, "title", title);
        return ask_owner(req, as_json);
    }
    if (!strcmp(verb, "send")) {
        if (argc < 3)
            return usage();
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "op", "send");
        cJSON_AddStringToObject(req, "task", argv[1]);
        cJSON_AddStringToObject(req, "text", argv[2]);
        if (project)
            cJSON_AddStringToObject(req, "project", project);
        return ask_owner(req, as_json);
    }
    if (!strcmp(verb, "tell")) {
        if (argc < 2)
            return usage();
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "op", "tell");
        cJSON_AddStringToObject(req, "text", argv[1]);
        return ask_owner(req, as_json);
    }
    if (!strcmp(verb, "reconcile")) {
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "op", "reconcile");
        return ask_owner(req, as_json);
    }
    if (!strcmp(verb, "owner")) {
        cJSON *req = cJSON_CreateObject();
        cJSON_AddStringToObject(req, "op", "status");
        return ask_owner(req, as_json);
    }

    return usage();
}
