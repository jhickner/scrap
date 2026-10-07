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

static int tool_rows(struct screen *s)
{
    int ok = 1;

    const backend_event tool = {.name = "Bash",
                                .input_json = "{\"command\":\"python - <<'EOF'\\n"
                                              "import json\\n"
                                              "d = json.load(open('x.json'))\\n"
                                              "EOF\"}"};
    char arg[4096];
    view_tool_argument(&tool, NULL, arg, sizeof arg);

    viewport_clear();
    view_collapse(0);
    view_keep_tool_call("Bash", arg, 0);
    viewport_paint();
    pump(s);
    if (row_with(s, "[bash]") < 0 || row_with(s, "import json") == row_with(s, "[bash]"))
        ok = fail("a full call keeps the command on rows of its own", NULL) == 0;

    viewport_clear();
    view_keep_tool_call("Bash", arg, 1);
    viewport_paint();
    pump(s);
    int at = row_with(s, "[bash]");
    if (at < 0 || row_with(s, "import json") != at || row_with(s, "json.load") != at)
        ok = fail("a collapsing call flattens onto its tag row", NULL) == 0;

    setenv("COLUMNS", "40", 1);
    viewport_touch();
    viewport_paint();
    pump(s);
    at = row_with(s, "[bash]");
    if (at < 0 || !strstr(row_text(s, at), "\xe2\x80\xa6"))
        ok = fail("a row too wide for the pane is cut short", NULL) == 0;
    if (count_on_screen(s, "json.load") != 0)
        ok = fail("a cut row drops what does not fit", NULL) == 0;

    setenv("COLUMNS", "80", 1);
    viewport_touch();
    viewport_paint();
    pump(s);
    if (count_on_screen(s, "json.load") != 1)
        ok = fail("widening the pane brings the rest of the row back", NULL) == 0;

    viewport_clear();
    view_keep_tool_call("Read", "src/a.c", 1);
    view_keep_tool_call("Read", "src/b.c", 1);
    view_keep_tool_call("Bash", "make check", 0);
    viewport_paint();
    pump(s);
    if (row_with(s, "src/a.c") < 0 || row_with(s, "src/b.c") != row_with(s, "src/a.c"))
        ok = fail("collapsing calls merge in full mode too", NULL) == 0;
    if (row_with(s, "make check") == row_with(s, "src/a.c"))
        ok = fail("a full call does not join a collapsed row", NULL) == 0;

    return ok;
}

static int merges(struct screen *s)
{
    int ok = 1;

    viewport_clear();
    view_collapse(1);
    view_keep_tool_call("Bash", "git add -A", 0);
    view_keep_output("nothing to commit", UI_DIM, 0);
    view_keep_tool_call("Bash", "git commit -m wip", 0);
    view_keep_output("1 file changed", UI_DIM, 0);
    viewport_paint();
    pump(s);

    if (row_with(s, "git add -A") < 0 || row_with(s, "git commit") != row_with(s, "git add -A"))
        ok = fail("collapsed calls of one tool share a row", NULL) == 0;

    char pad[81], over[256];
    memset(pad, 'x', sizeof pad - 1);
    pad[sizeof pad - 1] = '\0';
    snprintf(over, sizeof over, "grep -rn %s src", pad);

    view_keep_tool_call("Bash", over, 0);
    viewport_paint();
    pump(s);
    if (row_with(s, "grep -rn") != row_with(s, "git commit") + 1)
        ok = fail("a call too wide for the row starts its own", NULL) == 0;

    setenv("COLUMNS", "34", 1);
    viewport_touch();
    viewport_paint();
    pump(s);
    if (row_with(s, "git commit") == row_with(s, "git add -A"))
        ok = fail("narrowing the pane splits a merged row", NULL) == 0;

    setenv("COLUMNS", "80", 1);
    viewport_touch();
    viewport_paint();
    pump(s);
    if (row_with(s, "git commit") != row_with(s, "git add -A"))
        ok = fail("widening the pane merges the rows back", NULL) == 0;

    view_collapse(0);
    pump(s);
    if (row_with(s, "git commit") == row_with(s, "git add -A"))
        ok = fail("expanding gives each call its own row again", NULL) == 0;
    if (count_on_screen(s, "nothing to commit") != 1 ||
        count_on_screen(s, "1 file changed") != 1)
        ok = fail("expanding shows the output of merged calls", NULL) == 0;

    return ok;
}

static int state_isolation(struct screen *s)
{
    struct viewport_state *vp_a = viewport_state_new();
    struct viewport_state *vp_b = viewport_state_new();
    if (!vp_a || !vp_b) {
        viewport_state_free(vp_a);
        viewport_state_free(vp_b);
        return fail("two viewport states for session-view isolation", NULL);
    }

    viewport_clear();
    view_collapse(1);
    view_keep_tool_call("Read", "tab-a", 0);
    viewport_stash(vp_a);

    viewport_adopt(vp_b);
    int ok = 1;
    if (view_collapsed())
        ok = fail("a fresh tab does not inherit collapse or cluster state", NULL) == 0;
    view_keep_break();
    viewport_stash(vp_b);

    viewport_adopt(vp_a);
    if (!view_collapsed())
        ok = fail("a tab restores its own collapse state", NULL) == 0;
    view_keep_tool_call("Read", "tab-a-next", 0);
    viewport_paint();
    pump(s);
    if (row_with(s, "tab-a") < 0 || row_with(s, "tab-a-next") != row_with(s, "tab-a"))
        ok = fail("a tab restores its own incremental cluster state", NULL) == 0;

    viewport_clear();
    viewport_state_free(vp_a);
    viewport_state_free(vp_b);
    return ok;
}

static int collapse_redraws(void)
{
    char path[] = "/tmp/scrap-sessionviewtest-XXXXXX";
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

    int ok = state_isolation(&s);

    view_collapse(0);
    view_keep_tool_call("Bash", "ls -la", 0);
    view_keep_output("total 8\nfoo\nbar", UI_DIM, 0);
    view_keep_tool_call("Read", "src/main.c", 0);
    view_keep_output("read 40 lines", UI_DIM, 0);
    viewport_paint();
    pump(&s);

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
    view_keep_tool_call("Bash", "git status", 0);
    view_keep_output("on branch master", UI_DIM, 0);
    view_keep_tool_call("Edit", "src/two.c", 0);
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
    if (!row_blank(&s, row_with(&s, "src/two.c") - 1))
        ok = fail("a call kept while collapsed takes a gap once expanded", NULL) == 0;

    view_collapse(1);
    pump(&s);
    if (row_with(&s, "src/two.c") - row_with(&s, "git status") != 1 ||
        row_with(&s, "git status") - row_with(&s, "src/main.c") != 1)
        ok = fail("collapsing again puts every row back against the one above", NULL) == 0;

    view_collapse(0);
    pump(&s);
    if (!row_blank(&s, row_with(&s, "src/two.c") - 1) ||
        !row_blank(&s, row_with(&s, "git status") - 1))
        ok = fail("expanding again gaps every call", NULL) == 0;

    ok = merges(&s) && ok;
    ok = tool_rows(&s) && ok;

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

    static const struct {
        const char *name, *input, *arg;
    } MEMORY_CALLS[] = {
        {"mcp__optchat__zoom", "{\"id\":512,\"n\":1}", "512+1"},
        {"zoom", "{\"id\":7,\"n\":4}", "7+4"},
        {"mcp__optchat__date", "{\"id\":40}", "40"},
        {"mcp__optchat__agent", "{\"task\":\"fix the bug\"}", "fix the bug"},
    };
    for (int i = 0; i < (int)(sizeof MEMORY_CALLS / sizeof *MEMORY_CALLS); i++) {
        const backend_event call = {.name = MEMORY_CALLS[i].name,
                                    .input_json = MEMORY_CALLS[i].input};
        view_tool_argument(&call, NULL, arg, sizeof arg);
        if (strcmp(arg, MEMORY_CALLS[i].arg) != 0)
            return fail("a memory tool call lost its arguments", arg);
    }
    ui_capture_begin(80);
    view_tool_call("mcp__optchat__zoom", "512+1");
    out = ui_capture_end();
    if (!out || !strstr(out, "[zoom] 512+1"))
        return fail("an optchat call kept its MCP prefix", out);
    free(out);

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
