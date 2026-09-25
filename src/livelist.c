#include "livelist.h"

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "parent.h"
#include "session.h"
#include "text.h"
#include "title.h"
#include "vendor/cJSON.h"

#define MAX_LIVE 200

#define CLOSED_MAX_AGE (14L * 24 * 60 * 60)
#define MAX_SLOTS 32

static int  publishing;
static char dir[4200];
static char tmux_window[32];
static char tmux_wname[64];
static char tmux_pane_index[8];

static const struct session *slots[MAX_SLOTS];

static int tmux_do(const char *verb, const char *target)
{
    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
            if (null > STDERR_FILENO)
                close(null);
        }
        char *argv[] = {"tmux", (char *)verb, "-t", (char *)target, NULL};
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int livelist_jump(const struct live_session *v, char *why, int size)
{
    if (why && size)
        snprintf(why, (size_t)size, "%s", "");
    if (!v || !getenv("TMUX") || !v->pane[0]) {
        if (why && size)
            snprintf(why, (size_t)size, "that session is not in a tmux pane");
        return 0;
    }
    if (!tmux_do("select-window", v->pane) || !tmux_do("select-pane", v->pane)) {
        if (why && size)
            snprintf(why, (size_t)size, "could not select the tmux pane");
        return 0;
    }
    return 1;
}

static int slot_of(const struct session *s)
{
    int free_at = -1;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots[i] == s)
            return i;
        if (!slots[i] && free_at < 0)
            free_at = i;
    }
    if (free_at < 0)
        return -1;
    slots[free_at] = s;
    return free_at;
}

static int record_path(int slot, char *out, size_t size)
{
    if (!dir[0])
        return 0;
    return (size_t)snprintf(out, size, "%s/%ld-%d.json", dir, (long)getpid(), slot) < size;
}

static void drop_all(void)
{
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!slots[i])
            continue;
        char path[4400];
        if (record_path(i, path, sizeof path))
            unlink(path);
        slots[i] = NULL;
    }
}

static int live_dir(char *out, size_t size)
{
    const char *env = getenv("MUX_LIVE_DIR");
    if (env && *env)
        return (size_t)snprintf(out, size, "%s", env) < size;

    return path_config_subdir(out, size, "live");
}

static void tmux_where(void)
{
    static long asked;
    static int  tried;

    long now = (long)time(NULL);
    if (tried && now - asked < 5)
        return;
    asked = now;
    tried = 1;

    const char *pane = getenv("TMUX_PANE");
    if (!getenv("TMUX") || !pane || !*pane)
        return;

    char quoted[256];
    if (!text_shell_quote(pane, quoted, sizeof quoted))
        return;

    char cmd[512];
    if (snprintf(cmd, sizeof cmd,
                 "tmux display-message -p -t %s "
                 "'#{window_id}\t#{window_index}:#{window_name}\t#{pane_index}' 2>/dev/null",
                 quoted) >= (int)sizeof cmd)
        return;

    FILE *p = popen(cmd, "r");
    if (!p)
        return;
    char line[256];
    char *got = fgets(line, sizeof line, p);
    pclose(p);
    if (!got)
        return;

    line[strcspn(line, "\n")] = '\0';
    char *name = strchr(line, '\t');
    if (!name)
        return;
    *name++ = '\0';
    char *index = strchr(name, '\t');
    if (index)
        *index++ = '\0';

    snprintf(tmux_window, sizeof tmux_window, "%s", line);
    snprintf(tmux_wname, sizeof tmux_wname, "%s", name);
    snprintf(tmux_pane_index, sizeof tmux_pane_index, "%s", index ? index : "");
}

const char *livelist_tmux_window(void)
{
    tmux_where();
    return tmux_window;
}

void livelist_begin(void)
{
    if (!live_dir(dir, sizeof dir)) {
        dir[0] = '\0';
        return;
    }
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        dir[0] = '\0';
        return;
    }
    publishing = 1;
    atexit(drop_all);
}

static int write_line(FILE *f, void *ud)
{
    return fprintf(f, "%s\n", (const char *)ud) > 0;
}

void livelist_publish(const struct session *s, const char *status)
{
    if (!publishing || !s || !status)
        return;
    int slot = slot_of(s);
    if (slot < 0)
        return;

    char path[4400];
    if (!record_path(slot, path, sizeof path))
        return;

    const char *id = session_id(s);
    char name[200] = "";
    if (id)
        title_lookup(id, name, sizeof name);
    if (!name[0]) {
        const char *given = session_title(s);
        if (given && *given)
            snprintf(name, sizeof name, "%s", given);
    }

    cJSON *rec = cJSON_CreateObject();
    if (!rec)
        return;
    cJSON_AddNumberToObject(rec, "pid", (double)getpid());
    cJSON_AddNumberToObject(rec, "slot", slot);
    cJSON_AddStringToObject(rec, "backend", session_backend(s));
    cJSON_AddStringToObject(rec, "model", session_model(s));
    cJSON_AddStringToObject(rec, "label", session_model_label(s));
    cJSON_AddStringToObject(rec, "effort", session_effort(s));
    cJSON_AddStringToObject(rec, "cwd", session_cwd(s) ? session_cwd(s) : "");
    cJSON_AddStringToObject(rec, "id", id ? id : "");
    char up[128] = "";
    if (id)
        parent_of(id, up, sizeof up);
    cJSON_AddStringToObject(rec, "parent", up);
    cJSON_AddStringToObject(rec, "title", name);
    cJSON_AddStringToObject(rec, "status", status);
    cJSON_AddNumberToObject(rec, "unseen", session_unseen(s) ? 1 : 0);
    cJSON_AddNumberToObject(rec, "ts", (double)time(NULL));
    const char *pane = getenv("TMUX_PANE");
    if (pane && *pane)
        cJSON_AddStringToObject(rec, "pane", pane);
    tmux_where();
    if (tmux_window[0]) {
        cJSON_AddStringToObject(rec, "window", tmux_window);
        cJSON_AddStringToObject(rec, "wname", tmux_wname);
        cJSON_AddStringToObject(rec, "pane_index", tmux_pane_index);
    }

    char *text = cJSON_PrintUnformatted(rec);
    cJSON_Delete(rec);
    if (!text)
        return;

    text_spit(path, write_line, text);
    free(text);
}

void livelist_forget(const struct session *s)
{
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots[i] != s)
            continue;
        char path[4400];
        if (record_path(i, path, sizeof path))
            unlink(path);
        slots[i] = NULL;
        return;
    }
}

int livelist_alive(long pid)
{
    if (pid <= 0)
        return 0;
    if (kill((pid_t)pid, 0) == 0)
        return 1;
    return errno == EPERM;
}

static void copy_str(char *out, size_t size, const cJSON *rec, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(rec, key));
    snprintf(out, size, "%s", s ? s : "");
}

static long number(const cJSON *rec, const char *key)
{
    const cJSON *n = cJSON_GetObjectItem(rec, key);
    return cJSON_IsNumber(n) ? (long)cJSON_GetNumberValue(n) : 0;
}

static int newer(const void *a, const void *b)
{
    const struct live_session *x = a, *y = b;
    if (x->ts != y->ts)
        return x->ts < y->ts ? 1 : -1;
    if (x->pid != y->pid)
        return x->pid < y->pid ? -1 : 1;
    return x->slot - y->slot;
}

static int closed_dir(char *out, size_t size)
{
    char where[4200];
    if (!live_dir(where, sizeof where))
        return 0;
    return (size_t)snprintf(out, size, "%s/closed", where) < size;
}

static void retire(const char *path, const char *name)
{
    char where[4300], to[9000];
    if (!closed_dir(where, sizeof where)) {
        unlink(path);
        return;
    }
    mkdir(where, 0700);
    snprintf(to, sizeof to, "%s/%s", where, name);
    if (rename(path, to) != 0)
        unlink(path);
}

static int load_dir(const char *where, int live, struct live_session **out)
{
    *out = NULL;

    DIR *d = opendir(where);
    if (!d)
        return 0;

    struct live_session *list = calloc(MAX_LIVE, sizeof *list);
    if (!list) {
        closedir(d);
        return 0;
    }

    int count = 0;
    struct dirent *e;
    while (count < MAX_LIVE && (e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".json") != 0)
            continue;

        char path[9000];
        snprintf(path, sizeof path, "%s/%s", where, e->d_name);

        size_t len = 0;
        char *text = text_slurp(path, 64 * 1024, &len);
        if (!text)
            continue;
        cJSON *rec = cJSON_ParseWithLength(text, len);
        free(text);
        if (!rec)
            continue;

        struct live_session *v = &list[count];
        v->pid = number(rec, "pid");
        v->slot = (int)number(rec, "slot");
        v->ts = number(rec, "ts");
        copy_str(v->backend, sizeof v->backend, rec, "backend");
        copy_str(v->model, sizeof v->model, rec, "model");
        copy_str(v->label, sizeof v->label, rec, "label");
        copy_str(v->effort, sizeof v->effort, rec, "effort");
        copy_str(v->cwd, sizeof v->cwd, rec, "cwd");
        copy_str(v->id, sizeof v->id, rec, "id");
        copy_str(v->parent, sizeof v->parent, rec, "parent");
        copy_str(v->title, sizeof v->title, rec, "title");
        copy_str(v->status, sizeof v->status, rec, "status");
        v->unseen = (int)number(rec, "unseen");
        copy_str(v->window, sizeof v->window, rec, "window");
        copy_str(v->wname, sizeof v->wname, rec, "wname");
        copy_str(v->pane_index, sizeof v->pane_index, rec, "pane_index");
        copy_str(v->pane, sizeof v->pane, rec, "pane");
        cJSON_Delete(rec);

        if (live) {
            if (!livelist_alive(v->pid)) {

                if (v->id[0])
                    retire(path, e->d_name);
                else
                    unlink(path);
                continue;
            }
        } else if (v->ts && time(NULL) - v->ts > CLOSED_MAX_AGE) {
            unlink(path);
            continue;
        }
        v->mine = live && v->pid == (long)getpid();
        count++;
    }
    closedir(d);

    if (!count) {
        free(list);
        return 0;
    }
    qsort(list, (size_t)count, sizeof *list, newer);
    *out = list;
    return count;
}

int livelist_load(struct live_session **out)
{
    char where[4200];
    *out = NULL;
    if (!live_dir(where, sizeof where))
        return 0;
    return load_dir(where, 1, out);
}

static void forget_reopened(struct live_session *closed, int n,
                            const struct live_session *live, int live_n)
{
    char where[4300];
    if (!closed_dir(where, sizeof where))
        return;

    for (int i = 0; i < n; i++) {
        int back = 0;
        for (int j = 0; j < live_n && !back; j++)
            back = closed[i].id[0] && !strcmp(closed[i].id, live[j].id);
        if (!back)
            continue;

        char path[9000];
        snprintf(path, sizeof path, "%s/%ld-%d.json", where, closed[i].pid,
                 closed[i].slot);
        unlink(path);
        closed[i].id[0] = '\0';
    }
}

int livelist_closed_load(struct live_session **out)
{
    struct live_session *live = NULL;
    char                 where[4300];

    *out = NULL;

    int live_n = livelist_load(&live);

    int n = closed_dir(where, sizeof where) ? load_dir(where, 0, out) : 0;
    if (n > 0)
        forget_reopened(*out, n, live, live_n);
    free(live);
    return n;
}
