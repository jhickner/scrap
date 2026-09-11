#include "orchinstall.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "orchdata.h"
#include "text.h"

static const char *const SKILL_DIRS[] = {
    ".claude/skills/orchestrate",
    ".agents/skills/orchestrate",
};

static int config_file(const char *rel)
{
    return !strcmp(rel, "quota.sh") || !strcmp(rel, "routing.json");
}

static int seed_only(const char *rel)
{
    return !strcmp(rel, "routing.json");
}

static int mkdir_p(const char *dir)
{
    char buf[4096];
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

static int write_text(FILE *f, void *ud)
{
    return fputs((const char *)ud, f) >= 0;
}

static int same_text(const char *path, const char *text)
{
    char *have = text_slurp(path, 1u << 20, NULL);
    if (!have)
        return 0;
    int same = strcmp(have, text) == 0;
    free(have);
    return same;
}

static int write_file(const char *path, const char *text, int exec, int only_if_missing)
{
    struct stat st;
    if (only_if_missing && stat(path, &st) == 0)
        return 1;
    if (same_text(path, text)) {
        if (exec)
            chmod(path, 0755);
        return 1;
    }

    char dir[4096];
    if ((size_t)snprintf(dir, sizeof dir, "%s", path) >= sizeof dir)
        return 0;
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = '\0';
        if (!mkdir_p(dir))
            return 0;
    }
    if (!text_spit(path, write_text, (void *)text))
        return 0;
    if (exec)
        chmod(path, 0755);
    return 1;
}

static int dest(char *out, size_t size, const char *home, const char *dir,
                const char *rel)
{
    return (size_t)snprintf(out, size, "%s/%s/%s", home, dir, rel) < size;
}

void orch_install(void)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return;

    char path[4300];
    if (dest(path, sizeof path, home, ".config/orchestrator", "projects"))
        mkdir_p(path);
    if (dest(path, sizeof path, home, ".config/orchestrator", "results"))
        mkdir_p(path);

    for (int i = 0; i < orch_files_n; i++) {
        const struct orch_file *f = &orch_files[i];
        int exec = !strcmp(f->path, "quota.sh");

        if (config_file(f->path)) {
            if (!dest(path, sizeof path, home, ".config/orchestrator", f->path))
                continue;
            write_file(path, f->text, exec, seed_only(f->path));
            continue;
        }

        for (int d = 0; d < (int)(sizeof SKILL_DIRS / sizeof *SKILL_DIRS); d++) {
            if (!dest(path, sizeof path, home, SKILL_DIRS[d], f->path))
                continue;
            write_file(path, f->text, 0, 0);
        }
    }
}
