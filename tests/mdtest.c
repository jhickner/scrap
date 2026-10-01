
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "md.h"
#include "vendor/mermaid/mermaid.h"
#include "ui.h"

static int failures;

static void fail(const char *what, int width, size_t cells, const char *row, size_t len)
{
    fprintf(stderr, "FAIL %s: %d wide, row of %zu cells: [%.*s]\n", what, width, cells,
            (int)len, row);
    failures++;
}

static void check_fits(const char *what, const char *src, int width)
{
    ui_capture_begin(width);
    md_render(src, 0);
    char *out = ui_capture_end();
    if (!out)
        return;
    for (char *p = out; *p;) {
        char  *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        size_t cells = ui_cells_visible(p, len);
        if (cells > (size_t)width)
            fail(what, width, cells, p, len);
        if (!nl)
            break;
        p = nl + 1;
    }
    free(out);
}

static void check_mermaid(const char *what, const char *src, const char *want)
{
    MermaidArt *art = mermaid_render(src, 0);
    int         found = 0;
    for (size_t i = 0; art && i < art->n && !found && !strstr(art->lines[0], "mermaid:"); i++)
        found = strstr(art->lines[i], want) != NULL;
    if (!found) {
        fprintf(stderr, "FAIL %s: no row contains [%s]\n", what, want);
        failures++;
    }
    mermaid_art_free(art);
}

static void check_command(const char *what, const char *src, const char *want)
{
    ui_capture_begin(80);
    md_render(src, 0);
    char *out = ui_capture_end();
    char *got = NULL;
    for (char *at = out; at && !got && (at = strstr(at, "\x1b]8;")); at++) {
        char *url = strchr(at + 4, ';');
        char *end = url ? strchr(url, '\x1b') : NULL;
        if (end) {
            *end = '\0';
            got = md_command(url + 1);
            *end = '\x1b';
        }
    }
    if (want ? !got || strcmp(got, want) : got != NULL) {
        fprintf(stderr, "FAIL %s: want [%s] got [%s]\n", what, want ? want : "(none)",
                got ? got : "(none)");
        failures++;
    }
    free(got);
    free(out);
}

static void check_nth(const char *src, int nth, const char *want)
{
    char *got = md_command_nth(src, nth);
    if (want ? !got || strcmp(got, want) : got != NULL) {
        fprintf(stderr, "FAIL command %d: want [%s] got [%s]\n", nth, want ? want : "(none)",
                got ? got : "(none)");
        failures++;
    }
    free(got);
}

static void check_command_list(void)
{
    const char *reply = "First `ls -la src`, then:\n\n```bash\nmake\nmake check\n```\n\n"
                        "```c\n`cat x` in C\n```\n\nSee `struct prompt`, finally `rm -f a.o`.\n";
    check_nth(reply, 0, "!rm -f a.o");
    check_nth(reply, 1, "!make\nmake check");
    check_nth(reply, 2, "!ls -la src");
    check_nth(reply, 3, NULL);
    check_nth(NULL, 0, NULL);
}

static void check_commands(void)
{
    int tty = posix_openpt(O_RDWR | O_NOCTTY);
    if (tty < 0 || grantpt(tty) || unlockpt(tty)) {
        fprintf(stderr, "FAIL no pty for the command checks\n");
        failures++;
        return;
    }
    int peer = open(ptsname(tty), O_RDWR | O_NOCTTY);
    int saved = dup(STDOUT_FILENO);
    if (peer < 0 || dup2(peer, STDOUT_FILENO) < 0)
        return;
    ui_init();

    check_command("a bash block", "run:\n\n```bash\nmake\nmake check # 100%\n```\n",
                  "!make\nmake check # 100%");
    check_command("an untagged block", "```\nls -la\n```\n", "!ls -la");
    check_command("a c block", "```c\nint x;\n```\n", NULL);
    check_command("an inline command", "Then run `ls -la src` to check.\n", "!ls -la src");
    check_command("an inline path command", "Run `tools/rig/scraprig start`.\n",
                  "!tools/rig/scraprig start");
    check_command("an inline identifier", "See `struct prompt` in prompt.c.\n", NULL);
    check_command("an inline word", "Use `make`.\n", NULL);

    dup2(saved, STDOUT_FILENO);
    close(saved);
    close(peer);
    close(tty);
}

static void check_widths(const char *what, const char *src)
{
    for (int w = 20; w <= 120; w++)
        check_fits(what, src, w);
}

int main(void)
{
    setenv("COLUMNS", "80", 1);
    setenv("LINES", "24", 1);
    ui_init();

    check_widths("a paragraph",
                 "The generator already exists (tools/gen-testimages.sh) and has been run — "
                 "29 files are in testimages/ now, one per case.\n");

    check_widths("a bulleted list",
                 "- PNG fast path (8): basic, wide, tall, strip-2000x40, strip-40x2000, "
                 "large-3000x2000, tiny-16, pixel-1x1\n"
                 "- Conversion path / non-PNG (8): photo.jpg, bmp, tiff, webp, ico, "
                 "anim.gif, doc.pdf, vector.svg\n");

    check_widths("a numbered list",
                 "1. Conversion path / non-PNG (8): photo.jpg, bmp, tiff, webp, ico, "
                 "anim.gif, doc.pdf, vector.svg\n"
                 "10. Failure paths (7): broken.png, empty.png, nonimage.png, noext, "
                 "truncated.jpg, oversize-20mb.png\n");

    check_widths("a quote",
                 "> Conversion path / non-PNG (8): photo.jpg, bmp, tiff, webp, ico, "
                 "anim.gif, doc.pdf, vector.svg\n");

    check_widths("a nested list",
                 "- Conversion path / non-PNG (8): photo.jpg, bmp, tiff, webp, ico\n"
                 "  - anim.gif, doc.pdf, vector.svg, and whatever else turns up here\n");

    check_widths("styled text",
                 "- **Conversion path** / *non-PNG* (8): `photo.jpg`, [bmp](https://x.test/b), "
                 "tiff, webp, ico, anim.gif\n");

    check_mermaid("a semicolon in a message label",
                  "sequenceDiagram\n    A->>B: resolve; spill\n", "resolve; spill");
    check_mermaid("semicolon-separated statements", "graph TD\n    A-->B; B-->Cee\n", "Cee");

    check_widths("a mermaid diagram wider than the screen",
                 "```mermaid\nsequenceDiagram\n    participant A as first participant\n"
                 "    participant B as second participant\n    participant C as third participant\n"
                 "    A->>C: a message long enough to cross the screen\n```\n");

    check_command_list();
    check_commands();

    if (failures)
        return 1;
    fprintf(stderr, "mdtest: all checks passed\n");
    return 0;
}
