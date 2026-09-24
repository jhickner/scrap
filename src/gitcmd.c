#include "gitcmd.h"

#include <stdio.h>
#include <stdlib.h>
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

int gitcmd_run(const char *dir, const char *args)
{
    char quoted[4200];
    if (!text_shell_quote(dir, quoted, sizeof quoted))
        return 0;

    char cmd[8192];
    snprintf(cmd, sizeof cmd, "git -C %s %s >/dev/null 2>&1", quoted, args);
    return system(cmd) == 0;
}
