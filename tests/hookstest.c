#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hooks.h"

static int failures;

static void eq(const char *what, const char *got, const char *want)
{
    if ((got && want && !strcmp(got, want)) || (!got && !want))
        return;
    fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", what,
            got ? got : "(null)", want ? want : "(null)");
    failures++;
}

static void eq_int(const char *what, int got, int want)
{
    if (got == want)
        return;
    fprintf(stderr, "FAIL %s: got %d, want %d\n", what, got, want);
    failures++;
}

static const char *sample =
    "# tool hooks\n"
    "- tool: Bash\n"
    "  match: git commit\n"
    "  context: |\n"
    "    Commit rules: author is jhickner only.\n"
    "\n"
    "    Brief technical message.\n"
    "- tool: Edit|Write\n"
    "  context: >\n"
    "    Comment rules:\n"
    "    generally don't.\n"
    "- tool: Write\n"
    "  match: \"README.md\"   # docs\n"
    "  event: PostToolUse\n"
    "  context: 'Do not update README.md unless asked.'\n"
    "-\n"
    "  context: every tool\n"
    "  extra: ignored\n"
    "- tool: Bash\n"
    "  match:\n"
    "    - \"<<\"\n"
    "    # heredocs and in-place edits\n"
    "    - sed -i\n"
    "  context: bash edit\n"
    "- tool: Bash\n"
    "  match: [tee, \"python -c\", 'perl -pi']\n"
    "  context: flow list\n";

int main(void)
{
    eq_int("count", hooks_parse(sample), 6);

    const struct hook *h = hooks_at(0);
    eq("bash tool", h->tool, "Bash");
    eq_int("bash match count", h->match_count, 1);
    eq("bash match", h->match[0], "git commit");
    eq("bash event", h->event, "PreToolUse");
    eq("literal block", h->context,
       "Commit rules: author is jhickner only.\n\nBrief technical message.");

    h = hooks_at(1);
    eq("edit tool", h->tool, "Edit|Write");
    eq_int("edit match", h->match_count, 0);
    eq("folded block", h->context, "Comment rules: generally don't.");

    h = hooks_at(2);
    eq("quoted match", h->match[0], "README.md");
    eq("event", h->event, "PostToolUse");
    eq("single quoted", h->context, "Do not update README.md unless asked.");

    h = hooks_at(3);
    eq("no tool", h->tool, NULL);
    eq("bare item context", h->context, "every tool");

    h = hooks_at(4);
    eq_int("block list count", h->match_count, 2);
    eq("block list quoted", h->match[0], "<<");
    eq("block list plain", h->match[1], "sed -i");

    h = hooks_at(5);
    eq_int("flow list count", h->match_count, 3);
    eq("flow list plain", h->match[0], "tee");
    eq("flow list double quoted", h->match[1], "python -c");
    eq("flow list single quoted", h->match[2], "perl -pi");

    backend_hook table[HOOKS_MAX];
    eq_int("backend count", hooks_backend(table, HOOKS_MAX), 6);
    eq("backend tool", table[0].tool, "Bash");
    eq("backend any tool", table[3].tool, "");

    char *ctx = hooks_context(0, "Bash", "{\"command\":\"git commit -m fix\"}");
    eq("command match", ctx,
       "Commit rules: author is jhickner only.\n\nBrief technical message.");
    free(ctx);
    ctx = hooks_context(0, "Bash", "{\"command\":\"git status\"}");
    eq("command miss", ctx, NULL);
    ctx = hooks_context(0, "Bash", "{\"command\":\"ls\",\"description\":\"git commit\"}");
    eq("match is scoped to the command", ctx, NULL);
    ctx = hooks_context(0, "Agent", "{\"prompt\":\"run git commit\"}");
    eq("whole input when no command or path", ctx,
       "Commit rules: author is jhickner only.\n\nBrief technical message.");
    free(ctx);
    ctx = hooks_context(1, "Edit", "{\"file_path\":\"/a/b.c\"}");
    eq("no match always fires", ctx, "Comment rules: generally don't.");
    free(ctx);
    ctx = hooks_context(2, "Write", "{\"file_path\":\"/p/README.md\",\"content\":\"x\"}");
    eq("file path match", ctx, "Do not update README.md unless asked.");
    free(ctx);
    ctx = hooks_context(2, "Write", "{\"file_path\":\"/p/notes.md\",\"content\":\"README.md\"}");
    eq("content does not count", ctx, NULL);
    ctx = hooks_context(4, "Bash", "{\"command\":\"cat <<EOF > a.c\"}");
    eq("heredoc", ctx, "bash edit");
    free(ctx);
    ctx = hooks_context(4, "Bash", "{\"command\":\"sed -i '' s/a/b/ a.c\"}");
    eq("second alternative", ctx, "bash edit");
    free(ctx);
    ctx = hooks_context(4, "Bash", "{\"command\":\"ls\"}");
    eq("no alternative", ctx, NULL);
    ctx = hooks_context(5, "Bash", "{\"command\":\"python -c 'open(1)'\"}");
    eq("flow alternative", ctx, "flow list");
    free(ctx);
    ctx = hooks_context(9, "Bash", "{}");
    eq("out of range", ctx, NULL);

    eq_int("prompt count", hooks_parse(
        "- event: UserPromptSubmit\n"
        "  match: [kitty graphics, kitty image]\n"
        "  context: Read kitty.md.\n"), 1);
    eq("prompt backend tool", (hooks_backend(table, HOOKS_MAX), table[0].tool), "");
    ctx = hooks_context(0, NULL,
        "{\"prompt\":\"make a Kitty Graphics viewer\",\"cwd\":\"/p\"}");
    eq("prompt match", ctx, "Read kitty.md.");
    free(ctx);
    ctx = hooks_context(0, NULL,
        "{\"prompt\":\"fix the build\",\"cwd\":\"/kitty graphics\"}");
    eq("prompt miss ignores other fields", ctx, NULL);

    eq_int("empty", hooks_parse(""), 0);
    eq_int("empty count", hooks_count(), 0);

    if (failures) {
        fprintf(stderr, "hookstest: %d failure(s)\n", failures);
        return 1;
    }
    puts("hookstest: ok");
    return 0;
}
