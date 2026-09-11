#include "orchstatus.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "vendor/cJSON.h"

static const char *json_string(const cJSON *obj, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, key));
}

static time_t json_time(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? (time_t)cJSON_GetNumberValue(v) : 0;
}

static void copy(char *out, size_t size, const char *s)
{
    snprintf(out, size, "%s", s ? s : "");
}

static int suffix(const char *name, const char *want)
{
    size_t n = strlen(name), w = strlen(want);
    return n >= w && !strcmp(name + n - w, want);
}

static void project_name(char *out, size_t size, const char *filename)
{
    copy(out, size, filename);
    char *dot = strrchr(out, '.');
    if (dot)
        *dot = '\0';
}

static int task_index(const struct orch_task *tasks, int count,
                      const char *project, const char *id)
{
    for (int i = 0; i < count; i++)
        if (!strcmp(tasks[i].project, project) && !strcmp(tasks[i].id, id))
            return i;
    return -1;
}

static int grow(struct orch_task **tasks, int *capacity, int need)
{
    if (need <= *capacity)
        return 1;
    int next = *capacity ? *capacity * 2 : 32;
    while (next < need)
        next *= 2;
    struct orch_task *p = realloc(*tasks, (size_t)next * sizeof **tasks);
    if (!p)
        return 0;
    *tasks = p;
    *capacity = next;
    return 1;
}

static int load_project(const char *dir, const char *filename,
                        struct orch_task **tasks, int *count, int *capacity)
{
    char path[8192], project[128];
    if (snprintf(path, sizeof path, "%s/%s", dir, filename) >= (int)sizeof path)
        return 1;
    project_name(project, sizeof project, filename);

    FILE *f = fopen(path, "r");
    if (!f)
        return 1;

    char *line = NULL;
    size_t line_size = 0;
    while (getline(&line, &line_size, f) >= 0) {
        cJSON *obj = cJSON_Parse(line);
        const char *id = obj ? json_string(obj, "id") : NULL;
        if (!id || !*id) {
            cJSON_Delete(obj);
            continue;
        }

        int at = task_index(*tasks, *count, project, id);
        if (at < 0) {
            if (!grow(tasks, capacity, *count + 1)) {
                cJSON_Delete(obj);
                free(line);
                fclose(f);
                return 0;
            }
            at = (*count)++;
        }

        struct orch_task *t = &(*tasks)[at];
        memset(t, 0, sizeof *t);
        copy(t->project, sizeof t->project, project);
        copy(t->id, sizeof t->id, id);
        copy(t->desc, sizeof t->desc, json_string(obj, "desc"));
        copy(t->status, sizeof t->status, json_string(obj, "status"));
        copy(t->backend, sizeof t->backend, json_string(obj, "backend"));
        copy(t->model, sizeof t->model, json_string(obj, "model"));
        copy(t->session, sizeof t->session, json_string(obj, "session"));
        t->created = json_time(obj, "created");
        t->updated = json_time(obj, "updated");
        cJSON_Delete(obj);
    }
    free(line);
    fclose(f);
    return 1;
}

static int process_alive(long pid)
{
    if (pid <= 0)
        return 0;
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

static void apply_live(struct orch_task *tasks, int count, const char *dir)
{
    if (!dir || !*dir)
        return;
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (!suffix(e->d_name, ".json"))
            continue;
        char path[8192];
        if (snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= (int)sizeof path)
            continue;
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char *line = NULL;
        size_t size = 0;
        ssize_t got = getline(&line, &size, f);
        fclose(f);
        cJSON *obj = got >= 0 ? cJSON_Parse(line) : NULL;
        free(line);
        const char *session = obj ? json_string(obj, "id") : NULL;
        const char *status = obj ? json_string(obj, "status") : NULL;
        const cJSON *pidj = obj ? cJSON_GetObjectItemCaseSensitive(obj, "pid") : NULL;
        long pid = cJSON_IsNumber(pidj) ? (long)cJSON_GetNumberValue(pidj) : 0;
        if (session && *session && status && *status && process_alive(pid)) {
            for (int i = 0; i < count; i++)
                if (tasks[i].session[0] && !strcmp(tasks[i].session, session))
                    copy(tasks[i].live_status, sizeof tasks[i].live_status, status);
        }
        cJSON_Delete(obj);
    }
    closedir(d);
}

static int order(const void *a, const void *b)
{
    const struct orch_task *x = a, *y = b;
    int project = strcmp(x->project, y->project);
    if (project)
        return project;
    if (x->updated != y->updated)
        return x->updated < y->updated ? 1 : -1;
    return strcmp(x->id, y->id);
}

int orchstatus_load(const char *projects_dir, const char *live_dir,
                    int include_done, struct orch_task **out)
{
    *out = NULL;
    DIR *d = opendir(projects_dir);
    if (!d)
        return -1;

    struct orch_task *tasks = NULL;
    int count = 0, capacity = 0, ok = 1;
    struct dirent *e;
    while (ok && (e = readdir(d)))
        if (suffix(e->d_name, ".jsonl"))
            ok = load_project(projects_dir, e->d_name, &tasks, &count, &capacity);
    closedir(d);
    if (!ok) {
        free(tasks);
        return -1;
    }

    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (!include_done && !strcmp(tasks[i].status, "done"))
            continue;
        tasks[kept++] = tasks[i];
    }
    count = kept;
    apply_live(tasks, count, live_dir);
    qsort(tasks, (size_t)count, sizeof *tasks, order);
    *out = tasks;
    return count;
}

void orchstatus_age(char *out, size_t size, time_t then, time_t now)
{
    long age = then > 0 && now > then ? (long)(now - then) : 0;
    if (age < 60)
        snprintf(out, size, "%lds", age);
    else if (age < 60 * 60)
        snprintf(out, size, "%ldm", age / 60);
    else if (age < 24 * 60 * 60)
        snprintf(out, size, "%ldh", age / (60 * 60));
    else if (age < 14 * 24 * 60 * 60)
        snprintf(out, size, "%ldd", age / (24 * 60 * 60));
    else
        snprintf(out, size, "%ldw", age / (7 * 24 * 60 * 60));
}

static void shrink(int *width, int minimum, int *need)
{
    int take = *width - minimum;
    if (take > *need)
        take = *need;
    *width -= take;
    *need -= take;
}

void orchstatus_columns(struct orchstatus_columns *out, int columns)
{
    *out = (struct orchstatus_columns){
        .project = 12,
        .task = 4,
        .status = 18,
        .agent = 17,
        .age = 3,
    };

    /* Leave room for the indent, separators, and terminal right margin. */
    int available = columns - 7;
    if (available < 13)
        available = 13;
    int fixed = out->project + out->status + out->agent + out->age;
    int task = available - fixed;
    if (task < 4) {
        int need = 4 - task;
        shrink(&out->agent, 2, &need);
        shrink(&out->status, 2, &need);
        shrink(&out->project, 2, &need);
        task = available - out->project - out->status - out->agent - out->age;
    }
    out->task = task;
}

void orchstatus_cell(char *out, size_t size, const char *in, size_t width)
{
    size_t at = 0;
    for (; in && *in && at < width && at + 1 < size; in++) {
        char c = *in;
        out[at++] = c == '\n' || c == '\r' || c == '\t' ? ' ' : c;
    }
    if (in && *in && at >= 3) {
        out[at - 3] = '.';
        out[at - 2] = '.';
        out[at - 1] = '.';
    }
    out[at] = '\0';
}
