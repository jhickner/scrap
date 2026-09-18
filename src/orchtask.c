#include "orchtask.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "filelock.h"
#include "text.h"

#define LOG_MAX (4u << 20)

static const char *str(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive((cJSON *)o, key));
    return s ? s : "";
}

static double num(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive((cJSON *)o, key);
    return cJSON_IsNumber(v) ? cJSON_GetNumberValue(v) : 0;
}

static void copy(char *out, size_t size, const char *s)
{
    snprintf(out, size, "%s", s ? s : "");
}

static void set_str(cJSON *o, const char *key, const char *value)
{
    cJSON_DeleteItemFromObjectCaseSensitive(o, key);
    cJSON_AddStringToObject(o, key, value);
}

static void set_num(cJSON *o, const char *key, double value)
{
    cJSON_DeleteItemFromObjectCaseSensitive(o, key);
    cJSON_AddNumberToObject(o, key, value);
}

static int mkdir_p(const char *dir)
{
    char buf[4200];
    if ((size_t)snprintf(buf, sizeof buf, "%s", dir) >= sizeof buf)
        return 0;
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0700) != 0 && errno != EEXIST)
            return 0;
        *p = '/';
    }
    return mkdir(buf, 0700) == 0 || errno == EEXIST;
}

int orchtask_root(char *out, size_t size)
{
    const char *env = getenv("ORCHESTRATOR_DIR");
    if (env && *env)
        return (size_t)snprintf(out, size, "%s", env) < size;
    const char *home = getenv("HOME");
    if (!home || !*home)
        return 0;
    return (size_t)snprintf(out, size, "%s/.config/orchestrator", home) < size;
}

int orchtask_lockpath(char *out, size_t size)
{
    const char *env = getenv("ORCHESTRATOR_DIR");
    if (!env || !*env)
        return (size_t)snprintf(out, size, "/tmp/" APP_NAME "-%lu-orchestrator",
                                (unsigned long)getuid()) < size;

    /* FNV-1a over the store path: enough to keep two stores apart */
    unsigned long h = 2166136261u;
    for (const char *p = env; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    return (size_t)snprintf(out, size, "/tmp/" APP_NAME "-%lu-orchestrator-%08lx",
                            (unsigned long)getuid(), h & 0xffffffffu) < size;
}

static int under(char *out, size_t size, const char *leaf)
{
    char root[4200];
    if (!orchtask_root(root, sizeof root))
        return 0;
    return (size_t)snprintf(out, size, "%s/%s", root, leaf) < size;
}

static int project_path(char *out, size_t size, const char *project)
{
    char leaf[300];
    if (!project || !*project || strchr(project, '/'))
        return 0;
    snprintf(leaf, sizeof leaf, "projects/%s.jsonl", project);
    return under(out, size, leaf);
}

int orchtask_closed(const char *status)
{
    return status && (!strcmp(status, "done") || !strcmp(status, "cancelled"));
}

void orchtask_age(char *out, size_t size, time_t then, time_t now)
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

int orchtask_open(const char *status)
{
    return status && *status && !orchtask_closed(status);
}

/* ---- reading ------------------------------------------------------------ */

/* Walk a .jsonl and hand every well-formed object to fn, oldest first. A
   partial last line, which is what a torn write looks like, is skipped like
   any other unparseable line. */
static int each_record(const char *path, void (*fn)(cJSON *rec, void *ud), void *ud)
{
    char *text = text_slurp(path, LOG_MAX, NULL);
    if (!text)
        return 0;
    char *line = text;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        else if (line[0])
            break; /* no newline yet: a record still being written */
        if (*line) {
            cJSON *rec = cJSON_Parse(line);
            if (rec) {
                if (cJSON_IsObject(rec))
                    fn(rec, ud);
                cJSON_Delete(rec);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(text);
    return 1;
}

struct newest {
    const char *want_id;   /* NULL: keep them all */
    cJSON     **ids;       /* parallel arrays, newest record per id */
    char      (*keys)[ORCHTASK_ID_MAX];
    int         n, cap;
    cJSON      *hit;
};

static void keep_newest(cJSON *rec, void *ud)
{
    struct newest *k = ud;
    const char *id = str(rec, "id");
    if (!*id)
        return;
    if (k->want_id) {
        if (strcmp(id, k->want_id))
            return;
        cJSON_Delete(k->hit);
        k->hit = cJSON_Duplicate(rec, 1);
        return;
    }
    for (int i = 0; i < k->n; i++)
        if (!strcmp(k->keys[i], id)) {
            cJSON_Delete(k->ids[i]);
            k->ids[i] = cJSON_Duplicate(rec, 1);
            return;
        }
    if (k->n == k->cap) {
        int next = k->cap ? k->cap * 2 : 32;
        cJSON **ids = realloc(k->ids, (size_t)next * sizeof *ids);
        if (!ids)
            return;
        k->ids = ids;
        char (*keys)[ORCHTASK_ID_MAX] = realloc(k->keys, (size_t)next * sizeof *keys);
        if (!keys)
            return;
        k->keys = keys;
        k->cap = next;
    }
    copy(k->keys[k->n], ORCHTASK_ID_MAX, id);
    k->ids[k->n++] = cJSON_Duplicate(rec, 1);
}

static void fill(struct orch_rec *out, const cJSON *rec, const char *project)
{
    memset(out, 0, sizeof *out);
    copy(out->project, sizeof out->project, project);
    copy(out->id, sizeof out->id, str(rec, "id"));
    copy(out->desc, sizeof out->desc, str(rec, "desc"));
    copy(out->klass, sizeof out->klass, str(rec, "class"));
    copy(out->status, sizeof out->status, str(rec, "status"));
    copy(out->backend, sizeof out->backend, str(rec, "backend"));
    copy(out->model, sizeof out->model, str(rec, "model"));
    copy(out->session, sizeof out->session, str(rec, "session"));
    copy(out->worktree, sizeof out->worktree, str(rec, "worktree"));
    out->pid = (long)num(rec, "pid");
    out->created = (time_t)num(rec, "created");
    out->updated = (time_t)num(rec, "updated");
    out->checkpoint = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive((cJSON *)rec, "checkpoint"));
    const cJSON *pending = cJSON_GetObjectItemCaseSensitive((cJSON *)rec, "pending");
    out->pending = cJSON_IsArray(pending) ? cJSON_GetArraySize(pending) : 0;
}

static int name_of(char *out, size_t size, const char *filename)
{
    copy(out, size, filename);
    char *dot = strrchr(out, '.');
    if (!dot || strcmp(dot, ".jsonl"))
        return 0;
    *dot = '\0';
    return out[0] != '\0';
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int orchtask_projects(char ***out)
{
    *out = NULL;
    char dir[4200];
    if (!under(dir, sizeof dir, "projects"))
        return -1;
    DIR *d = opendir(dir);
    if (!d)
        return errno == ENOENT ? 0 : -1; /* no projects yet is not a failure */

    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        char name[256];
        if (!name_of(name, sizeof name, e->d_name))
            continue;
        if (n == cap) {
            int next = cap ? cap * 2 : 16;
            char **p = realloc(names, (size_t)next * sizeof *p);
            if (!p)
                break;
            names = p;
            cap = next;
        }
        names[n++] = strdup(name);
    }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmp_name);
    *out = names;
    return n;
}

static int same_fold(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
    return !*a && !*b;
}

int orchtask_project_cwd(const char *name, char *cwd, size_t cwd_size,
                         char *real, size_t real_size)
{
    if (!name || !*name)
        return 0;
    char path[4200];
    if (!under(path, sizeof path, "registry.json"))
        return 0;
    char *text = text_slurp(path, LOG_MAX, NULL);
    if (!text)
        return 0;
    cJSON *o = cJSON_Parse(text);
    free(text);
    if (!o)
        return 0;

    int found = 0;
    const cJSON *projects = cJSON_GetObjectItemCaseSensitive(o, "projects");
    /* an exact name beats another project's alias */
    for (int pass = 0; pass < 2 && !found; pass++) {
        const cJSON *p;
        cJSON_ArrayForEach(p, projects) {
            const char *pname = str(p, "name");
            int hit = pass == 0 && same_fold(pname, name);
            if (!hit && pass == 1) {
                const cJSON *alias;
                cJSON_ArrayForEach(alias, cJSON_GetObjectItemCaseSensitive((cJSON *)p, "aliases"))
                    if (cJSON_IsString(alias) && same_fold(alias->valuestring, name))
                        hit = 1;
            }
            if (!hit)
                continue;
            copy(cwd, cwd_size, str(p, "cwd"));
            if (real)
                copy(real, real_size, pname);
            found = 1;
            break;
        }
    }
    cJSON_Delete(o);
    return found;
}

static int order_recent(const void *a, const void *b)
{
    const struct orch_rec *x = a, *y = b;
    time_t xt = x->updated ? x->updated : x->created;
    time_t yt = y->updated ? y->updated : y->created;
    if (xt != yt)
        return xt > yt ? -1 : 1;
    return strcmp(x->id, y->id);
}

static int load_one(const char *project, int include_closed,
                    struct orch_rec **recs, int *n, int *cap)
{
    char path[4200];
    if (!project_path(path, sizeof path, project))
        return 0;

    struct newest k = {0};
    if (!each_record(path, keep_newest, &k)) {
        free(k.ids);
        free(k.keys);
        return 0;
    }

    for (int i = 0; i < k.n; i++) {
        const char *status = str(k.ids[i], "status");
        if (include_closed || !orchtask_closed(status)) {
            if (*n == *cap) {
                int next = *cap ? *cap * 2 : 32;
                struct orch_rec *p = realloc(*recs, (size_t)next * sizeof *p);
                if (p) {
                    *recs = p;
                    *cap = next;
                }
            }
            if (*n < *cap)
                fill(&(*recs)[(*n)++], k.ids[i], project);
        }
        cJSON_Delete(k.ids[i]);
    }
    free(k.ids);
    free(k.keys);
    return 1;
}

int orchtask_load(const char *project, int include_closed, struct orch_rec **out)
{
    *out = NULL;
    struct orch_rec *recs = NULL;
    int n = 0, cap = 0;

    if (project) {
        if (!load_one(project, include_closed, &recs, &n, &cap)) {
            free(recs);
            return -1;
        }
    } else {
        char **names = NULL;
        int count = orchtask_projects(&names);
        if (count < 0)
            return -1;
        for (int i = 0; i < count; i++) {
            load_one(names[i], include_closed, &recs, &n, &cap);
            free(names[i]);
        }
        free(names);
    }

    qsort(recs, (size_t)n, sizeof *recs, order_recent);
    *out = recs;
    return n;
}

cJSON *orchtask_find(const char *project, const char *id,
                     char *project_out, size_t size)
{
    if (!id || !*id)
        return NULL;

    char **names = NULL;
    char *one[1];
    int count;
    if (project) {
        one[0] = (char *)project;
        names = one;
        count = 1;
    } else {
        count = orchtask_projects(&names);
        if (count < 0)
            return NULL;
    }

    cJSON *hit = NULL;
    for (int i = 0; i < count; i++) {
        char path[4200];
        if (!hit && project_path(path, sizeof path, names[i])) {
            struct newest k = {.want_id = id};
            each_record(path, keep_newest, &k);
            if (k.hit) {
                hit = k.hit;
                if (project_out)
                    copy(project_out, size, names[i]);
            }
        }
        if (names != one)
            free(names[i]);
    }
    if (names != one)
        free(names);
    return hit;
}

/* ---- writing ------------------------------------------------------------ */

/* Append under an exclusive lock, so two writers cannot interleave a line and
   a reader never sees half of one. The append itself is one write call. */
static int append_line(const char *path, const char *line)
{
    char dir[4200];
    copy(dir, sizeof dir, path);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (!mkdir_p(dir))
            return 0;
    }

    int lock = filelock_acquire(path, LOCK_EX);
    if (lock < 0)
        return 0;
    FILE *f = fopen(path, "a");
    if (!f) {
        filelock_release(lock);
        return 0;
    }
    /* A previous writer that died mid-line leaves no terminating newline;
       appending straight onto it would join two records into one unreadable
       line and lose both. Close the torn line first. */
    int ok = 1;
    struct stat st;
    if (fstat(fileno(f), &st) == 0 && st.st_size > 0) {
        FILE *back = fopen(path, "rb");
        if (back) {
            char last = '\n';
            if (fseek(back, -1, SEEK_END) == 0)
                last = (char)fgetc(back);
            fclose(back);
            if (last != '\n')
                ok = fputc('\n', f) != EOF;
        }
    }
    ok = fprintf(f, "%s\n", line) > 0 && ok;
    ok = fflush(f) == 0 && ok;
    fclose(f);
    filelock_release(lock);
    return ok;
}

int orchtask_append(const char *project, cJSON *rec)
{
    char path[4200];
    if (!rec || !project_path(path, sizeof path, project))
        return 0;
    set_num(rec, "updated", (double)time(NULL));
    char *line = cJSON_PrintUnformatted(rec);
    if (!line)
        return 0;
    int ok = append_line(path, line);
    free(line);
    return ok;
}

void orchtask_log(const char *ev, const char *project, const char *id,
                  const char *detail)
{
    char path[4200];
    if (!under(path, sizeof path, "log.jsonl"))
        return;
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return;
    cJSON_AddNumberToObject(o, "at", (double)time(NULL));
    cJSON_AddStringToObject(o, "ev", ev ? ev : "");
    if (project)
        cJSON_AddStringToObject(o, "project", project);
    if (id)
        cJSON_AddStringToObject(o, "task", id);
    if (detail)
        cJSON_AddStringToObject(o, "detail", detail);
    char *line = cJSON_PrintUnformatted(o);
    if (line)
        append_line(path, line);
    free(line);
    cJSON_Delete(o);
}

static int fresh_id(const char *project, char *out, size_t size)
{
    FILE *r = fopen("/dev/urandom", "rb");
    for (int tries = 0; tries < 16; tries++) {
        unsigned char b[3] = {0};
        if (r && fread(b, 1, sizeof b, r) != sizeof b) {
            fclose(r);
            r = NULL;
        }
        if (!r) {
            unsigned long seed = (unsigned long)time(NULL) ^ ((unsigned long)getpid() << 8);
            seed += (unsigned long)tries * 2654435761u;
            b[0] = seed & 0xff;
            b[1] = (seed >> 8) & 0xff;
            b[2] = (seed >> 16) & 0xff;
        }
        snprintf(out, size, "t-%02x%02x%02x", b[0], b[1], b[2]);
        cJSON *clash = orchtask_find(project, out, NULL, 0);
        if (!clash) {
            if (r)
                fclose(r);
            return 1;
        }
        cJSON_Delete(clash);
    }
    if (r)
        fclose(r);
    return 0;
}

int orchtask_create(const char *project, const char *desc, const char *klass,
                    int checkpoint, long pid, const char *notes,
                    char *id_out, size_t id_size)
{
    if (!project || !*project || !desc || !*desc)
        return 0;
    char id[ORCHTASK_ID_MAX];
    if (!fresh_id(project, id, sizeof id))
        return 0;

    cJSON *o = cJSON_CreateObject();
    if (!o)
        return 0;
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddStringToObject(o, "desc", desc);
    cJSON_AddStringToObject(o, "class", klass && *klass ? klass : "impl");
    cJSON_AddStringToObject(o, "status", "queued");
    cJSON_AddNumberToObject(o, "created", (double)time(NULL));
    cJSON_AddItemToObject(o, "deps", cJSON_CreateArray());
    cJSON_AddBoolToObject(o, "checkpoint", checkpoint ? 1 : 0);
    if (pid > 0)
        cJSON_AddNumberToObject(o, "pid", (double)pid);
    if (notes && *notes)
        cJSON_AddStringToObject(o, "notes", notes);

    int ok = orchtask_append(project, o);
    cJSON_Delete(o);
    if (ok) {
        copy(id_out, id_size, id);
        orchtask_log("create", project, id, desc);
    }
    return ok;
}

/* done is terminal: reopening would strand anything that already acted on it.
   Anything else may move, including out of cancelled. */
static int legal_move(const char *from, const char *to)
{
    if (!from || !*from)
        return 1;
    if (!strcmp(from, "done") && strcmp(to, "done"))
        return 0;
    return 1;
}

int orchtask_set_status(const char *project, const char *id, const char *status,
                        const char *backend, const char *model,
                        const char *session, const char *worktree, long pid)
{
    char found[128];
    cJSON *rec = orchtask_find(project, id, found, sizeof found);
    if (!rec)
        return 0;
    if (status && !legal_move(str(rec, "status"), status)) {
        cJSON_Delete(rec);
        return 0;
    }
    if (status)
        set_str(rec, "status", status);
    if (backend)
        set_str(rec, "backend", backend);
    if (model)
        set_str(rec, "model", model);
    if (session)
        set_str(rec, "session", session);
    if (worktree)
        set_str(rec, "worktree", worktree);
    if (pid > 0)
        set_num(rec, "pid", (double)pid);

    int ok = orchtask_append(project ? project : found, rec);
    cJSON_Delete(rec);
    return ok;
}

int orchtask_note(const char *project, const char *id, const char *note)
{
    char found[128];
    cJSON *rec = orchtask_find(project, id, found, sizeof found);
    if (!rec || !note || !*note) {
        cJSON_Delete(rec);
        return 0;
    }
    const char *had = str(rec, "notes");
    char *joined = *had ? text_dsprintf("%s\n%s", had, note) : strdup(note);
    if (joined)
        set_str(rec, "notes", joined);
    free(joined);
    int ok = orchtask_append(project ? project : found, rec);
    cJSON_Delete(rec);
    return ok;
}

int orchtask_set_summary(const char *project, const char *id, const char *summary)
{
    char found[128];
    cJSON *rec = orchtask_find(project, id, found, sizeof found);
    if (!rec || !summary || !*summary) {
        cJSON_Delete(rec);
        return 0;
    }
    set_str(rec, "summary", summary);
    int ok = orchtask_append(project ? project : found, rec);
    cJSON_Delete(rec);
    return ok;
}

int orchtask_add_pending(const char *project, const char *id, const char *text)
{
    char found[128];
    cJSON *rec = orchtask_find(project, id, found, sizeof found);
    if (!rec || !text || !*text) {
        cJSON_Delete(rec);
        return 0;
    }
    cJSON *pending = cJSON_GetObjectItemCaseSensitive(rec, "pending");
    if (!cJSON_IsArray(pending)) {
        cJSON_DeleteItemFromObjectCaseSensitive(rec, "pending");
        pending = cJSON_AddArrayToObject(rec, "pending");
    }
    cJSON_AddItemToArray(pending, cJSON_CreateString(text));
    int ok = orchtask_append(project ? project : found, rec);
    cJSON_Delete(rec);
    return ok;
}

int orchtask_take_pending(const char *project, const char *id, char *out, size_t size)
{
    char found[128];
    cJSON *rec = orchtask_find(project, id, found, sizeof found);
    if (!rec)
        return 0;
    cJSON *pending = cJSON_GetObjectItemCaseSensitive(rec, "pending");
    if (!cJSON_IsArray(pending) || cJSON_GetArraySize(pending) == 0) {
        cJSON_Delete(rec);
        return 0;
    }
    cJSON *first = cJSON_GetArrayItem(pending, 0);
    copy(out, size, cJSON_IsString(first) ? first->valuestring : "");
    cJSON_DeleteItemFromArray(pending, 0);
    int ok = orchtask_append(project ? project : found, rec);
    cJSON_Delete(rec);
    return ok && *out;
}

static int result_path(char *out, size_t size, const char *id)
{
    char leaf[300];
    if (!id || !*id || strchr(id, '/'))
        return 0;
    snprintf(leaf, sizeof leaf, "results/%s.json", id);
    return under(out, size, leaf);
}

cJSON *orchtask_result_read(const char *id)
{
    char path[4200];
    if (!result_path(path, sizeof path, id))
        return NULL;
    char *text = text_slurp(path, LOG_MAX, NULL);
    if (!text)
        return NULL;
    cJSON *o = cJSON_Parse(text);
    free(text);
    return o;
}

int orchtask_result_drop(const char *id)
{
    char path[4200];
    if (!result_path(path, sizeof path, id))
        return 0;
    return unlink(path) == 0;
}

/* ---- rendering ---------------------------------------------------------- */

void orchtask_print_table(FILE *f, const struct orch_rec *recs, int n, time_t now)
{
    int idw = 2, pw = 7, sw = 6;
    for (int i = 0; i < n; i++) {
        int w = (int)strlen(recs[i].id);
        if (w > idw)
            idw = w;
        w = (int)strlen(recs[i].project);
        if (w > pw)
            pw = w;
        w = (int)strlen(recs[i].status) + (recs[i].pending > 0 ? 4 : 0);
        if (w > sw)
            sw = w;
    }
    fprintf(f, "%-*s  %-*s  %-*s  %4s  %s\n", pw, "PROJECT", idw, "ID", sw, "STATUS",
            "AGE", "TASK");
    for (int i = 0; i < n; i++) {
        char age[16];
        orchtask_age(age, sizeof age, recs[i].updated ? recs[i].updated : recs[i].created,
                       now);
        char status[64];
        if (recs[i].pending > 0)
            snprintf(status, sizeof status, "%s +%d", recs[i].status, recs[i].pending);
        else
            snprintf(status, sizeof status, "%s", recs[i].status);
        fprintf(f, "%-*s  %-*s  %-*s  %4s  %s%s\n", pw, recs[i].project, idw, recs[i].id,
                sw, status, age, recs[i].checkpoint ? "checkpoint: " : "",
                recs[i].desc);
    }
}

static cJSON *rec_json(const struct orch_rec *r)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "project", r->project);
    cJSON_AddStringToObject(o, "id", r->id);
    cJSON_AddStringToObject(o, "desc", r->desc);
    cJSON_AddStringToObject(o, "class", r->klass);
    cJSON_AddStringToObject(o, "status", r->status);
    cJSON_AddStringToObject(o, "backend", r->backend);
    cJSON_AddStringToObject(o, "model", r->model);
    cJSON_AddStringToObject(o, "session", r->session);
    cJSON_AddStringToObject(o, "worktree", r->worktree);
    cJSON_AddNumberToObject(o, "pid", (double)r->pid);
    cJSON_AddBoolToObject(o, "checkpoint", r->checkpoint);
    cJSON_AddNumberToObject(o, "pending", r->pending);
    cJSON_AddNumberToObject(o, "created", (double)r->created);
    cJSON_AddNumberToObject(o, "updated", (double)r->updated);
    return o;
}

void orchtask_print_json(FILE *f, const struct orch_rec *recs, int n)
{
    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return;
    for (int i = 0; i < n; i++) {
        cJSON *o = rec_json(&recs[i]);
        if (o)
            cJSON_AddItemToArray(arr, o);
    }
    char *text = cJSON_PrintUnformatted(arr);
    if (text)
        fprintf(f, "%s\n", text);
    free(text);
    cJSON_Delete(arr);
}

void orchtask_print_one(FILE *f, const cJSON *rec)
{
    char *text = cJSON_Print((cJSON *)rec);
    if (text)
        fprintf(f, "%s\n", text);
    free(text);
}
