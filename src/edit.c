#include "edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "block.h"
#include "chrome.h"
#include "text.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"

#define EDIT_MAX_BYTES (1u << 22)

static const char *editor_command(void)
{
    const char *editor = getenv("VISUAL");
    if (!editor || !*editor)
        editor = getenv("EDITOR");
    if (!editor || !*editor)
        editor = "vi";
    return editor;
}

static int temp_path(char *out, size_t size, const char *suffix)
{
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = "/tmp";

    int n = snprintf(out, size, "%s/mux-edit-XXXXXX", dir);
    if (n <= 0 || (size_t)n >= size)
        return 0;

    int fd = mkstemp(out);
    if (fd < 0)
        return 0;
    close(fd);

    // mkstemp cannot make the name end in .md, so the suffix is added after
    // and the reservation is dropped: the name is still ours.
    if (suffix && *suffix) {
        char named[4200];
        if ((size_t)snprintf(named, sizeof named, "%s%s", out, suffix) < sizeof named &&
            rename(out, named) == 0)
            snprintf(out, size, "%s", named);
    }
    return 1;
}

char *edit_run(const char *initial, const char *suffix)
{
    char path[4100];
    if (!temp_path(path, sizeof path, suffix))
        return NULL;

    FILE *f = fopen(path, "w");
    if (!f) {
        unlink(path);
        return NULL;
    }
    if (initial && *initial)
        fputs(initial, f);
    // A file that does not end in a newline is one the editor adds one to,
    // which would count as a change every time it was opened.
    size_t len = initial ? strlen(initial) : 0;
    if (!len || initial[len - 1] != '\n')
        fputc('\n', f);
    if (fclose(f) != 0) {
        unlink(path);
        return NULL;
    }

    char quoted[4200];
    if (!text_shell_quote(path, quoted, sizeof quoted)) {
        unlink(path);
        return NULL;
    }
    char cmd[8500];
    snprintf(cmd, sizeof cmd, "%s %s", editor_command(), quoted);

    // The terminal goes to the editor whole and comes back the same way.
    chrome_clear();
    viewport_suspend();
    ui_raw(0);
    tty_raw_end();

    int status = system(cmd);

    if (tty_raw_begin() != 0) {
        fprintf(stderr, "could not return the terminal to raw mode\n");
        exit(1);
    }
    ui_raw(1);
    ui_cursor_plain();
    block_forget();
    viewport_resume();

    char *text = NULL;
    if (status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        text = text_slurp(path, EDIT_MAX_BYTES, NULL);
    unlink(path);

    if (text)
        text_chomp(text);
    return text;
}
