#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gitinfo.h"
#include "text.h"

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static char dir[] = "/tmp/gitinfotest.XXXXXX";

static void cleanup(void)
{
    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not clean %s\n", dir);
}

static int sh(const char *cmd)
{
    return system(cmd);
}

static int write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    fputs(text, f);
    fclose(f);
    return 1;
}

int main(void)
{
    if (!mkdtemp(dir)) {
        perror("mkdtemp");
        return 1;
    }
    atexit(cleanup);

    char cmd[8192], path[4200], quoted[4200];
    if (!text_shell_quote(dir, quoted, sizeof quoted))
        return 1;

    snprintf(cmd, sizeof cmd,
             "git -C %s init -q && "
             "git -C %s config user.email t@t && "
             "git -C %s config user.name t && "
             "git -C %s config commit.gpgsign false",
             quoted, quoted, quoted, quoted);
    if (sh(cmd) != 0) {
        fprintf(stderr, "git init failed\n");
        return 1;
    }

    snprintf(path, sizeof path, "%s/tracked.txt", dir);
    if (!write_file(path, "one\n"))
        return 1;
    snprintf(cmd, sizeof cmd, "git -C %s add tracked.txt && git -C %s commit -qm init",
             quoted, quoted);
    if (sh(cmd) != 0) {
        fprintf(stderr, "git commit failed\n");
        return 1;
    }

    const struct gitinfo *g = gitinfo_get(dir);
    if (!g->repo)
        fail("committed repo is a repo");
    if (!g->branch[0])
        fail("branch is set");
    if (!g->sha[0])
        fail("sha is set");
    if (g->dirty || g->untracked || g->added || g->removed)
        fail("clean tree has no flags");

    if (!write_file(path, "one\ntwo\n"))
        return 1;
    g = gitinfo_get(dir);
    if (!g->dirty)
        fail("first read after an edit is dirty");
    if (g->added < 1)
        fail("first read after an edit has insertions");

    gitinfo_forget();
    snprintf(path, sizeof path, "%s/new.txt", dir);
    if (!write_file(path, "new\n"))
        return 1;
    g = gitinfo_get(dir);
    if (!g->untracked)
        fail("first read after forget sees untracked");
    if (!g->dirty)
        fail("first read after forget still sees the edit");

    return failures ? 1 : 0;
}
