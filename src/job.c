#include "job.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "schedule.h"
#include "settings.h"
#include "text.h"

#define FILE_MAX    (256 * 1024)
#define CHECK_COUNT 5

struct job {
    char            name[128];
    struct schedule when;
    char            backend[64], model[128], effort[32], dir[4096];
    char           *prompt;
};

struct slot {
    char       path[4400];
    time_t     mtime;
    off_t      size;
    ino_t      ino;
    int        ok, seen;
    time_t     next;
    struct job job;
};

static struct slot *slots;
static int          nslots;

static int jobs_dir(char *out, size_t size)
{
    return path_config_file(out, size, "jobs");
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = '\0';
    return s;
}

static void job_free(struct job *j)
{
    free(j->prompt);
    j->prompt = NULL;
}

static int job_load(const char *path, struct job *j, char *err, size_t size)
{
    memset(j, 0, sizeof *j);
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t n = strlen(base);
    if (n > 4 && !strcmp(base + n - 4, ".job"))
        n -= 4;
    snprintf(j->name, sizeof j->name, "%.*s", (int)n, base);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(err, size, "%s", strerror(errno));
        return 0;
    }
    char *text = malloc(FILE_MAX + 1);
    if (!text) {
        fclose(f);
        snprintf(err, size, "out of memory");
        return 0;
    }
    text[fread(text, 1, FILE_MAX, f)] = '\0';
    fclose(f);
    char schedule[512] = "", *p = text;
    int  ok = 1;
    while (ok && *p) {
        char *eol = strchr(p, '\n');
        if (eol)
            *eol = '\0';
        char *line = trim(p);
        p = eol ? eol + 1 : p + strlen(p);
        if (!*line)
            break;
        if (*line == '#')
            continue;
        char *eq = strchr(line, '=');
        if (!eq) {
            snprintf(err, size, "'%s': expected key = value", line);
            ok = 0;
            break;
        }
        *eq = '\0';
        char *k = trim(line), *v = trim(eq + 1);
        if (!strcmp(k, "schedule"))
            snprintf(schedule, sizeof schedule, "%s", v);
        else if (!strcmp(k, "backend"))
            snprintf(j->backend, sizeof j->backend, "%s", v);
        else if (!strcmp(k, "model"))
            snprintf(j->model, sizeof j->model, "%s", v);
        else if (!strcmp(k, "effort"))
            snprintf(j->effort, sizeof j->effort, "%s", v);
        else if (!strcmp(k, "dir")) {
            const char *home = getenv("HOME");
            if (v[0] == '~' && (v[1] == '/' || !v[1]) && home)
                snprintf(j->dir, sizeof j->dir, "%s%s", home, v + 1);
            else
                snprintf(j->dir, sizeof j->dir, "%s", v);
        } else {
            snprintf(err, size, "unknown key '%s'", k);
            ok = 0;
        }
    }
    struct stat st;
    if (ok && !schedule[0]) {
        snprintf(err, size, "no schedule");
        ok = 0;
    } else if (ok && !schedule_parse(schedule, &j->when, err, size))
        ok = 0;
    else if (ok && j->dir[0] && (stat(j->dir, &st) != 0 || !S_ISDIR(st.st_mode))) {
        snprintf(err, size, "no such directory: %s", j->dir);
        ok = 0;
    } else if (ok && !*(p = trim(p))) {
        snprintf(err, size, "no prompt: put it after a blank line");
        ok = 0;
    }
    if (ok)
        j->prompt = strdup(p);
    free(text);
    return ok && j->prompt;
}

static void stamp(time_t t, char *out, size_t size)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, size, "%a %Y-%m-%d %H:%M:%S", &tm);
}

static const char *pick(const char *own, const struct settings *cfg, const char *key)
{
    return own[0] ? own : settings_get(cfg, key, NULL);
}

static void job_run(const char *exe, const struct job *j, time_t now)
{
    static struct settings cfg;
    char                   path[4400], log[4600], when[64];
    settings_load(&cfg, path_config_file(path, sizeof path, "settings") ? path : NULL);
    if (!path_config_subdir(path, sizeof path, "jobs/log") ||
        (size_t)snprintf(log, sizeof log, "%s/%s.log", path, j->name) >= sizeof log)
        return;
    const char *backend = pick(j->backend, &cfg, SETTING_JOB_BACKEND);
    const char *model = pick(j->model, &cfg, SETTING_JOB_MODEL);
    const char *effort = pick(j->effort, &cfg, SETTING_JOB_EFFORT);
    const char *home = getenv("HOME");
    const char *dir = j->dir[0] ? j->dir : home ? home : "/";
    const char *argv[16];
    int         n = 0;
    argv[n++] = exe;
    if (backend && *backend) {
        argv[n++] = "-b";
        argv[n++] = backend;
    }
    if (model && *model) {
        argv[n++] = "-m";
        argv[n++] = model;
    }
    if (effort && *effort) {
        argv[n++] = "-e";
        argv[n++] = effort;
    }
    argv[n++] = "-C";
    argv[n++] = dir;
    argv[n++] = "-p";
    argv[n++] = j->prompt;
    argv[n] = NULL;
    stamp(now, when, sizeof when);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        if (fork() != 0)
            _exit(0);
        int in = open("/dev/null", O_RDONLY), out = open(log, O_WRONLY | O_CREAT | O_APPEND, 0600);
        dup2(in, 0);
        dup2(out >= 0 ? out : in, 1);
        dup2(out >= 0 ? out : in, 2);
        for (int i = 3; i < 1024; i++)
            close(i);
        printf("=== %s\n", when);
        fflush(stdout);
        execv(exe, (char *const *)argv);
        _exit(127);
    }
    if (pid > 0)
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
            ;
    printf("scrap hub: job %s ran\n", j->name);
}

static void report_next(const struct slot *s)
{
    char when[64] = "never";
    if (s->next)
        stamp(s->next, when, sizeof when);
    printf("scrap hub: job %s next %s\n", s->job.name, when);
}

static int is_job(const char *leaf)
{
    size_t n = strlen(leaf);
    return leaf[0] != '.' && n > 4 && !strcmp(leaf + n - 4, ".job");
}

void job_tick(const char *exe, time_t now)
{
    char dir[4200];
    if (!jobs_dir(dir, sizeof dir))
        return;
    for (int i = 0; i < nslots; i++)
        slots[i].seen = 0;
    DIR           *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char        path[4400];
        struct stat st;
        if (!is_job(e->d_name) ||
            (size_t)snprintf(path, sizeof path, "%s/%s", dir, e->d_name) >= sizeof path ||
            stat(path, &st) != 0)
            continue;
        struct slot *s = NULL;
        for (int i = 0; i < nslots && !s; i++)
            if (!strcmp(slots[i].path, path))
                s = &slots[i];
        if (!s) {
            struct slot *grown = realloc(slots, (size_t)(nslots + 1) * sizeof *slots);
            if (!grown)
                continue;
            slots = grown;
            s = &slots[nslots++];
            memset(s, 0, sizeof *s);
            snprintf(s->path, sizeof s->path, "%s", path);
            s->size = -1;
        }
        s->seen = 1;
        if (s->size == st.st_size && s->mtime == st.st_mtime && s->ino == st.st_ino)
            continue;
        s->size = st.st_size;
        s->mtime = st.st_mtime;
        s->ino = st.st_ino;
        job_free(&s->job);
        char err[512];
        s->ok = job_load(path, &s->job, err, sizeof err);
        if (!s->ok) {
            printf("scrap hub: job %s: %s\n", s->job.name, err);
            continue;
        }
        s->next = schedule_next(&s->job.when, s->job.name, now);
        report_next(s);
    }
    if (d)
        closedir(d);
    for (int i = 0; i < nslots; i++)
        if (!slots[i].seen) {
            job_free(&slots[i].job);
            slots[i--] = slots[--nslots];
        }
    for (int i = 0; i < nslots; i++) {
        struct slot *s = &slots[i];
        if (!s->ok || !s->next || now < s->next)
            continue;
        job_run(exe, &s->job, now);
        s->next = schedule_next(&s->job.when, s->job.name, now);
        report_next(s);
    }
}

static int usage(void)
{
    char dir[4200];
    if (!jobs_dir(dir, sizeof dir))
        snprintf(dir, sizeof dir, "~/.config/scrap/jobs");
    fprintf(stderr,
            "usage: scrap job ls\n"
            "       scrap job check NAME|FILE [COUNT]   validate a job and print its next runs\n"
            "\n"
            "jobs: %s/NAME.job, run by scrap hub as single-shot scrap runs;\n"
            "output is appended to %s/log/NAME.log\n"
            "\n"
            "  schedule = 0 0 15 ? * TUE ~2w@2026-09-29\n"
            "  backend  = claude     optional, default: job_backend in settings, else the default backend\n"
            "  model    = ...        optional, default: job_model in settings\n"
            "  effort   = low        optional, default: job_effort in settings\n"
            "  dir      = ~/src/x    optional, default: $HOME\n"
            "\n"
            "  The prompt follows the first blank line and may span lines.\n"
            "\n"
            "schedule: Quartz cron, SEC MIN HOUR DOM MON DOW [YEAR], local time; one of DOM and DOW is ?\n"
            "  fields take *, lists, ranges, and /steps; MON takes JAN-DEC\n"
            "  DOM: 1-31, L (last day)   DOW: 1-7 = SUN-SAT, MON#2 (second Monday), 6L (last Friday)\n"
            "  MIN HOUR may be replaced by one field R[COUNT][/GAP][HH:MM-HH:MM]: COUNT runs at random\n"
            "  times in the window, at least GAP apart (m or h, default minutes)\n"
            "  trailing ~Nw@YYYY-MM-DD: every N weeks, counted from that date\n"
            "\n"
            "  0 0 15 ? * TUE ~2w@2026-09-29   every other Tuesday 15:00\n"
            "  0 0 9 ? * SUN#1                 first Sunday of the month 09:00\n"
            "  0 R[08:00-21:00] * * ?          once a day at a random time 08:00-21:00\n"
            "  0 R5/30m[08:00-21:00] * * ?     5 times a day 08:00-21:00, at least 30 minutes apart\n",
            dir, dir);
    return 2;
}

static int resolve(const char *arg, char *out, size_t size)
{
    char dir[4200];
    if (strchr(arg, '/'))
        return (size_t)snprintf(out, size, "%s", arg) < size;
    return jobs_dir(dir, sizeof dir) &&
           (size_t)snprintf(out, size, "%s/%s%s", dir, arg, is_job(arg) ? "" : ".job") < size;
}

static int check(const char *arg, int count)
{
    char       path[4400], err[512], when[64];
    struct job j;
    if (!resolve(arg, path, sizeof path)) {
        fprintf(stderr, "scrap job: bad name '%s'\n", arg);
        return 1;
    }
    if (!job_load(path, &j, err, sizeof err)) {
        fprintf(stderr, "%s: %s\n", j.name, err);
        job_free(&j);
        return 1;
    }
    printf("%s: ok\n", j.name);
    time_t t = time(NULL);
    for (int i = 0; i < count && (t = schedule_next(&j.when, j.name, t)); i++) {
        stamp(t, when, sizeof when);
        printf("  %s\n", when);
    }
    if (!t)
        printf("  no further runs\n");
    job_free(&j);
    return 0;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int list(void)
{
    char dir[4200];
    if (!jobs_dir(dir, sizeof dir))
        return 1;
    DIR *d = opendir(dir);
    if (!d) {
        printf("no jobs in %s\n", dir);
        return 0;
    }
    char         **names = NULL;
    int            n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (is_job(e->d_name)) {
            char **grown = realloc(names, (size_t)(n + 1) * sizeof *names);
            if (!grown)
                break;
            names = grown;
            names[n++] = strdup(e->d_name);
        }
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, by_name);
    int bad = 0;
    for (int i = 0; i < n; i++) {
        char       path[4400], err[512], when[64] = "never";
        struct job j;
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        if (job_load(path, &j, err, sizeof err)) {
            time_t t = schedule_next(&j.when, j.name, time(NULL));
            if (t)
                stamp(t, when, sizeof when);
            printf("%-24s %s\n", j.name, when);
        } else {
            printf("%-24s error: %s\n", j.name, err);
            bad = 1;
        }
        job_free(&j);
        free(names[i]);
    }
    free(names);
    if (!n)
        printf("no jobs in %s\n", dir);
    return bad;
}

int job_main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "ls"))
        return list();
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "check")) {
        int count = argc == 4 ? atoi(argv[3]) : CHECK_COUNT;
        return check(argv[2], count > 0 ? count : CHECK_COUNT);
    }
    return usage();
}
