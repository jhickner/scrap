#include "edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "chrome.h"
#include "text.h"
#include "terminalrun.h"
#include "viewport.h"

static const char *editor_command(void)
{
    const char *editor = getenv("VISUAL");
    if (!editor || !*editor)
        editor = getenv("EDITOR");
    if (!editor || !*editor)
        editor = "vi";
    return editor;
}

static int run_editor(const char *path)
{
    char quoted[4200];
    if (!text_shell_quote(path, quoted, sizeof quoted))
        return -1;

    char cmd[8500];
    int  n = snprintf(cmd, sizeof cmd, "%s %s", editor_command(), quoted);
    if (n < 0 || (size_t)n >= sizeof cmd)
        return -1;

    chrome_clear();
    return terminal_run_external(cmd);
}

int edit_open(const char *path)
{
    if (!path || !*path || access(path, R_OK))
        return 0;
    return run_editor(path) != -1;
}
