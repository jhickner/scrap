#include "gitcmd.h"

#include <stdio.h>
#include <string.h>

#include "text.h"

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
