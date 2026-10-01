#include <stdio.h>
#include <string.h>

#include "highlight.h"
#include "ui.h"

static int         failures;
static const char *fence;

static char letter(unsigned char role)
{
    switch ((enum ui_role)role) {
    case UI_SYN_CMD:     return 'c';
    case UI_SYN_KEYWORD: return 'k';
    case UI_SYN_STRING:  return 's';
    case UI_SYN_COMMENT: return '#';
    case UI_SYN_VAR:     return 'v';
    case UI_SYN_OP:      return 'o';
    case UI_SYN_FLAG:    return 'f';
    case UI_SYN_NUMBER:  return 'n';
    default:             return '.';
    }
}

static void show(const char *command, const unsigned char *roles, size_t len)
{
    printf("     %.*s\n     ", (int)len, command);
    for (size_t i = 0; i < len; i++)
        putchar(command[i] == '\n' ? '\n' : letter(roles[i]));
    putchar('\n');
}

static void role_nth(const char *command, const char *needle, int nth, char want)
{
    unsigned char roles[4096];
    size_t        len = strlen(command);

    if (fence)
        highlight_code(fence, command, len, roles);
    else
        highlight_shell(command, len, roles);

    const char *at = command;
    for (int i = 0; at && i <= nth; i++)
        at = i ? strstr(at + 1, needle) : strstr(at, needle);
    if (!at) {
        printf("FAIL %s: no %s\n", command, needle);
        failures++;
        return;
    }

    size_t from = (size_t)(at - command), n = strlen(needle);
    for (size_t i = from; i < from + n; i++) {
        if (letter(roles[i]) == want)
            continue;
        printf("FAIL %s: %s is %c, want %c\n", command, needle, letter(roles[i]), want);
        show(command, roles, len);
        failures++;
        return;
    }
}

static void role(const char *command, const char *needle, char want)
{
    role_nth(command, needle, 0, want);
}

int main(void)
{
    role("ls -la src", "ls", 'c');
    role("ls -la src", "-la", 'f');
    role("ls -la src", "src", '.');
    role("head -n 20 file", "20", 'n');
    role("/usr/bin/env ls", "/usr/bin/env", 'c');

    role("grep -rn foo src | head -20", "|", 'o');
    role("grep -rn foo src | head -20", "head", 'c');
    role("cd /tmp && ls", "&&", 'o');
    role("cd /tmp && ls", "ls", 'c');
    role("make; ./scrap", ";", 'o');
    role("make; ./scrap", "./scrap", 'c');
    role("ls > out.txt", ">", 'o');
    role("ls 2>/dev/null", "2>", 'o');

    role("echo 'hi there'", "'hi there'", 's');
    role("echo \"one two\"", "\"one two\"", 's');
    role("grep -r 'a b' .", "'a b'", 's');

    role("echo $HOME", "$HOME", 'v');
    role("echo ${HOME}/x", "${HOME}", 'v');
    role("echo \"$USER home\"", "$USER", 'v');
    role("echo \"$USER home\"", "home", 's');
    role("echo $(date +%s)", "date", 'c');
    role("echo $(date +%s)", "$(", 'o');
    role("echo `date`", "date", 'c');

    role("ls # a listing", "# a listing", '#');
    role("ls\n# note\nls", "# note", '#');

    role("if [ -f x ]; then cat x; fi", "if", 'k');
    role("if [ -f x ]; then cat x; fi", "then", 'k');
    role("if [ -f x ]; then cat x; fi", "cat", 'c');
    role("if [ -f x ]; then cat x; fi", "fi", 'k');
    role("for f in *.c; do echo $f; done", "for", 'k');
    role_nth("for f in *.c; do echo $f; done", "f", 1, 'v');
    role("for f in *.c; do echo $f; done", "in", 'k');
    role("for f in *.c; do echo $f; done", "*.c", '.');
    role("for f in *.c; do echo $f; done", "do", 'k');
    role("for f in *.c; do echo $f; done", "done", 'k');

    role("FOO=1 make", "FOO", 'v');
    role("FOO=1 make", "=", 'o');
    role("FOO=1 make", "make", 'c');

    role("python3 -c 'import os'", "python3", 'c');
    role("python3 -c 'import os'", "-c", 'f');
    role("python3 -c 'import os'", "import", 'k');
    role("python3 -c 'import os'", "'", 's');
    role("uv run python -c \"print(1)\"", "print", 'c');
    role("uv run python -c \"print(1)\"", "1", 'n');
    role("python3 -c 'x = \"hi\"  # note'", "\"hi\"", 's');
    role("python3 -c 'x = \"hi\"  # note'", "# note", '#');
    role("python3 -c 'def go(): pass'", "def", 'k');
    role("python3 -c 'def go(): pass'", "go", 'c');
    role("python3 -c 'f = f\"{x}\"'", "f\"{x}\"", 's');

    role("sh -c 'ls | wc -l'", "wc", 'c');
    role("sh -c 'ls | wc -l'", "-l", 'f');

    role("python3 <<'PY'\nimport sys\nprint(sys.path)\nPY\n", "import", 'k');
    role("python3 <<'PY'\nimport sys\nprint(sys.path)\nPY\n", "print", 'c');
    role_nth("python3 <<'PY'\nimport sys\nprint(sys.path)\nPY\n", "PY", 1, 's');
    role("python3 - <<EOF\nx = 1\nEOF", "x", '.');
    role("python3 - <<EOF\nx = 1\nEOF", "1", 'n');
    role_nth("python3 - <<EOF\nx = 1\nEOF", "EOF", 1, 's');
    role("cat <<EOF\nplain text\nEOF", "plain text", '.');
    role("bash <<'SH'\ngit status\nSH", "git", 'c');
    role("cat <<-EOF\n\tindented\n\tEOF", "indented", '.');
    role("cat > f <<EOF\nbody\nEOF\nls", "ls", 'c');

    role("git commit -m 'wip: highlight'", "git", 'c');
    role("git commit -m 'wip: highlight'", "'wip: highlight'", 's');

    fence = "json";
    role("{\"name\": \"x\", \"n\": -1.5, \"ok\": true}", "\"name\"", 'v');
    role("{\"name\": \"x\", \"n\": -1.5, \"ok\": true}", "\"x\"", 's');
    role("{\"name\": \"x\", \"n\": -1.5, \"ok\": true}", "-1.5", 'n');
    role("{\"name\": \"x\", \"n\": -1.5, \"ok\": true}", "true", 'k');

    fence = "yaml";
    role("# top\nname: web\nports:\n  - 80\nenv: {a: 1}\n", "# top", '#');
    role("# top\nname: web\nports:\n  - 80\nenv: {a: 1}\n", "name", 'v');
    role("# top\nname: web\nports:\n  - 80\nenv: {a: 1}\n", "web", 's');
    role("# top\nname: web\nports:\n  - 80\nenv: {a: 1}\n", "80", 'n');
    role("# top\nname: web\nports:\n  - 80\nenv: {a: 1}\n", "1", 'n');
    role("url: http://x:80 # c\non: yes\n", "http://x:80", 's');
    role("url: http://x:80 # c\non: yes\n", "# c", '#');
    role("url: http://x:80 # c\non: yes\n", "yes", 'k');
    role("run: |\n  echo: hi\nnext: 1\n", "echo: hi", 's');
    role("run: |\n  echo: hi\nnext: 1\n", "next", 'v');

    fence = "c";
    role("#include <stdio.h>\nint main(void) { /* x */ return printf(\"%d\", 1); } // end", "#include", 'k');
    role("#include <stdio.h>\nint main(void) { /* x */ return printf(\"%d\", 1); } // end", "main", 'c');
    role("#include <stdio.h>\nint main(void) { /* x */ return printf(\"%d\", 1); } // end", "/* x */", '#');
    role("#include <stdio.h>\nint main(void) { /* x */ return printf(\"%d\", 1); } // end", "\"%d\"", 's');
    role("#include <stdio.h>\nint main(void) { /* x */ return printf(\"%d\", 1); } // end", "// end", '#');

    fence = "ts {title}";
    role("const x = `a\nb`; await go()", "const", 'k');
    role("const x = `a\nb`; await go()", "b`", 's');
    role("const x = `a\nb`; await go()", "go", 'c');

    fence = "text";
    role("const x", "const", '.');
    fence = NULL;

    if (failures)
        printf("%d failure(s)\n", failures);
    else
        printf("ok\n");
    return failures ? 1 : 0;
}
