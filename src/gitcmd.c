#include "gitcmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "text.h"

static int dir_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int inside(const char *path)
{
    char out[16];
    return gitcmd_line(path, "rev-parse --is-inside-work-tree", out, sizeof out) &&
           !strcmp(out, "true");
}

int gitcmd_line(const char *dir, const char *args, char *out, size_t size)
{
    char quoted[4200];
    if (!text_shell_quote(dir, quoted, sizeof quoted))
        return 0;

    char cmd[8192];
    snprintf(cmd, sizeof cmd, "git -C %s %s 2>/dev/null", quoted, args);
    FILE *f = popen(cmd, "r");
    if (!f)
        return 0;
    out[0] = '\0';
    if (!fgets(out, (int)size, f)) {
        pclose(f);
        return 0;
    }
    pclose(f);
    text_chomp(out);
    return out[0] != '\0';
}

int gitcmd_root(const char *cwd, char *out, size_t size)
{
    char line[4200];
    if (gitcmd_line(cwd, "worktree list --porcelain", line, sizeof line) &&
        !strncmp(line, "worktree ", 9) && line[9]) {
        snprintf(out, size, "%s", line + 9);
        return 1;
    }
    return gitcmd_line(cwd, "rev-parse --show-toplevel", out, size);
}

int gitcmd_worktree_add(const char *root, const char *path, const char *branch)
{
    if (!root || !path || !branch || !*root || !*path || !*branch)
        return 0;

    if (inside(path))
        return 1;

    char qroot[4200], qpath[4200], qbranch[256];
    if (!text_shell_quote(root, qroot, sizeof qroot) ||
        !text_shell_quote(path, qpath, sizeof qpath) ||
        !text_shell_quote(branch, qbranch, sizeof qbranch))
        return 0;

    char cmd[9000];
    snprintf(cmd, sizeof cmd, "git -C %s worktree prune >/dev/null 2>&1", qroot);
    system(cmd);

    if (inside(path))
        return 1;

    snprintf(cmd, sizeof cmd,
             "git -C %s worktree add %s -b %s >/dev/null 2>&1",
             qroot, qpath, qbranch);
    if (system(cmd) == 0)
        return 1;

    snprintf(cmd, sizeof cmd,
             "git -C %s worktree add %s %s >/dev/null 2>&1",
             qroot, qpath, qbranch);
    if (system(cmd) == 0)
        return 1;

    return inside(path) || dir_exists(path);
}
