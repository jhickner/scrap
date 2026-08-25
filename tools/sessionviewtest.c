#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "screenmodel.h"
#include "sessionview.h"
#include "text.h"
#include "ui.h"
#include "viewport.h"

static int fail(const char *what, const char *out)
{
    fprintf(stderr, "sessionviewtest: %s\n---\n%s---\n", what, out ? out : "");
    return 1;
}

static int lines_of(const char *s)
{
    int n = 0;
    for (const char *p = s; *p; p++)
        if (*p == '\n')
            n++;
    return n;
}

static int tap_read = -1;

static void pump(struct screen *s)
{
    fflush(stdout);
    char    buf[65536];
    ssize_t n;
    while ((n = read(tap_read, buf, sizeof buf)) > 0)
        feed(s, buf, (size_t)n);
}

static int collapse_redraws(void)
{
    char path[] = "/tmp/mux-sessionviewtest-XXXXXX";
    int  wfd = mkstemp(path);
    tap_read = wfd >= 0 ? open(path, O_RDONLY) : -1;
    if (wfd < 0 || tap_read < 0)
        return fail("no temp file", NULL);
    unlink(path);
    fflush(stdout);
    if (dup2(wfd, STDOUT_FILENO) < 0)
        return fail("cannot redirect stdout", NULL);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);

    setenv("COLUMNS", "80", 1);
    setenv("LINES", "24", 1);

    ui_init();
    viewport_begin();

    struct screen s;
    screen_init(&s, 24, 80);

    view_collapse(0);
    view_keep_tool_call("Bash", "ls -la", 0);
    view_keep_output("total 8\nfoo\nbar", UI_DIM, 0);
    view_keep_tool_call("Read", "src/main.c", 1);
    view_keep_output("read 40 lines", UI_DIM, 0);
    viewport_paint();
    pump(&s);

    int ok = 1;
    if (count_on_screen(&s, "ls -la") != 1 || count_on_screen(&s, "total 8") != 1)
        ok = fail("the full view draws the call and its output", NULL) == 0;

    view_collapse(1);
    pump(&s);
    if (count_on_screen(&s, "total 8") != 0 || count_on_screen(&s, "read 40 lines") != 0)
        ok = fail("collapsing drops the output of a call already drawn", NULL) == 0;
    if (count_on_screen(&s, "ls -la") != 1 || count_on_screen(&s, "src/main.c") != 1)
        ok = fail("collapsing keeps one row per call already drawn", NULL) == 0;
    if (row_with(&s, "src/main.c") - row_with(&s, "ls -la") != 1)
        ok = fail("collapsed rows sit against each other", NULL) == 0;

    view_collapse(0);
    pump(&s);
    if (count_on_screen(&s, "total 8") != 1 || count_on_screen(&s, "read 40 lines") != 1)
        ok = fail("expanding brings the output back", NULL) == 0;
    if (!row_blank(&s, row_with(&s, "src/main.c") - 1))
        ok = fail("expanding brings the gap between calls back", NULL) == 0;

    view_collapse(1);
    view_keep_tool_call("Bash", "git status", 1);
    view_keep_output("on branch master", UI_DIM, 0);
    view_keep_tool_call("Edit", "src/two.c", 1);
    view_keep_diff(strdup("@@file src/two.c\n@@ -3 +3 @@\n-before\n+after\n"));
    viewport_paint();
    pump(&s);
    if (count_on_screen(&s, "git status") != 1 || count_on_screen(&s, "src/two.c") != 1)
        ok = fail("a call kept while collapsed draws one row", NULL) == 0;
    if (count_on_screen(&s, "on branch master") != 0 || count_on_screen(&s, "after") != 0)
        ok = fail("a call kept while collapsed draws no output", NULL) == 0;
    if (row_with(&s, "src/two.c") - row_with(&s, "git status") != 1)
        ok = fail("a call kept while collapsed sits against the row above", NULL) == 0;

    view_collapse(0);
    pump(&s);
    if (count_on_screen(&s, "on branch master") != 1 || count_on_screen(&s, "after") != 1)
        ok = fail("expanding shows what was kept while collapsed", NULL) == 0;

    viewport_end();
    return ok ? 0 : 1;
}

int main(void)
{
    char block[256];
    text_block("\n\n  import json\n  \nfor l in L:\n    print(l)\n\n\n", block, sizeof block);
    if (strcmp(block, "import json\nfor l in L:\n    print(l)") != 0)
        return fail("text_block did not preserve line structure", block);

    const backend_event tool = {.name = "Bash",
                                .input_json = "{\"command\":\"python - <<'EOF'\\n"
                                              "import json\\n"
                                              "d = json.load(open('x.json'))\\n"
                                              "EOF\"}"};
    char arg[4096];
    view_tool_argument(&tool, NULL, arg, sizeof arg);
    if (!strstr(arg, "\nimport json\n"))
        return fail("view_tool_argument flattened the command", arg);

    ui_capture_begin(80);
    view_tool_call("Bash", arg);
    char *out = ui_capture_end();
    if (!out || lines_of(out) != 4 || !strstr(out, "import json"))
        return fail("view_tool_call flattened the command", out);
    free(out);

    struct turnview v = {0};
    ui_capture_begin(200);
    view_cluster_start(&v, "Bash", arg, 0);
    view_cluster_paint(&v);
    out = ui_capture_end();
    if (!out || lines_of(out) != 1)
        return fail("cluster line spans rows", out);
    free(out);

    ui_capture_begin(40);
    view_cluster_paint(&v);
    char *cut = ui_capture_end();
    ui_capture_begin(200);
    view_cluster_paint(&v);
    char *full = ui_capture_end();
    if (!cut || !full || lines_of(cut) != 1)
        return fail("a cluster row stays one row at any width", cut);
    if (!strstr(cut, "\xe2\x80\xa6"))
        return fail("a cluster row too wide for the pane is cut short", cut);
    if (strlen(full) <= strlen(cut))
        return fail("a cluster row is laid out for the width it is drawn at", full);
    free(cut);
    free(full);
    view_cluster_forget(&v);

    ui_capture_begin(80);
    view_tool_error("failed: Exit code 1\n"
                    "Traceback (most recent call last):\n"
                    "  File \"<string>\", line 3, in <module>\n"
                    "    d = json.load(open('x.json'))\n"
                    "FileNotFoundError: 'x.json'\n");
    out = ui_capture_end();
    if (!out || !strstr(out, "failed: Exit code 1") || !strstr(out, "FileNotFoundError") ||
        !strstr(out, "+1 line"))
        return fail("view_tool_error dropped the exception", out);
    free(out);

    if (collapse_redraws())
        return 1;

    fflush(stdout);
    fprintf(stderr, "sessionviewtest: all checks passed\n");
    return 0;
}
