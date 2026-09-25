#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overlay.h"
#include "ui.h"

static int failures;

static void fail(const char *what, const char *got)
{
    fprintf(stderr, "FAIL %s: %s\n", what, got);
    failures++;
}

static char *drawn_raw(const struct overlay *o, const char *under)
{
    ui_sink_begin();
    overlay_put(under, o);
    return ui_sink_end();
}

static char *composited(const struct overlay *o, const char *under)
{
    char *drawn = drawn_raw(o, under);
    char *plain = ui_plain(drawn, 1);
    free(drawn);
    return plain;
}

static const char *line_at(const char *text, int at, char *out, size_t size)
{
    const char *p = text;
    for (int i = 0; i < at && p; i++) {
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : NULL;
    }
    if (!p) {
        out[0] = '\0';
        return out;
    }
    const char *nl = strchr(p, '\n');
    size_t      n = nl ? (size_t)(nl - p) : strlen(p);
    snprintf(out, size, "%.*s", (int)n, p);
    return out;
}

static void want(const char *text, int at, const char *expect)
{
    char got[256];
    line_at(text, at, got, sizeof got);
    if (strcmp(got, expect))
        fail(expect, got);
}

static void paint_box(void *ud, int at, int width)
{
    (void)ud;
    for (int i = 0; i < width; i++)
        ui_put(i == 0 || i == width - 1 ? "|" : at ? " " : "-");
}

int main(void)
{
    const char *under = "one\n"
                        "two two two two two two\n"
                        "\n"
                        "four four four four four\n"
                        "five five five five five\n"
                        "six six six six six six\n"
                        "seven seven seven seven\n"
                        "eight eight eight eight\n";

    struct overlay o = {.row = 2, .col = 4, .w = 6, .rows = 3, .paint_row = paint_box};
    char          *drawn = composited(&o, under);

    want(drawn, 0, "one");
    want(drawn, 1, "two two two two two two");
    want(drawn, 2, "    |----|");
    want(drawn, 3, "four|    |four four four");
    want(drawn, 4, "five|    |five five five");
    want(drawn, 5, "six six six six six six");
    free(drawn);

    o.row = 7;
    drawn = composited(&o, under);
    want(drawn, 7, "eigh|----|t eight eight");
    want(drawn, 8, "    |    |");
    want(drawn, 9, "    |    |");
    free(drawn);

    const char *raw = "one\r\ntwo two two two two two\r\n\r\n"
                      "four four four four four\r\n";
    o.row = 2;
    drawn = drawn_raw(&o, raw);
    for (const char *p = strchr(drawn, '\r'); p; p = strchr(p + 1, '\r'))
        if (p[1] != '\n')
            fail("a return is left in the middle of a row", drawn);
    free(drawn);

    drawn = composited(&o, raw);
    want(drawn, 2, "    |----|");
    want(drawn, 3, "four|    |four four four");
    free(drawn);

    printf(failures ? "overlaytest: %d failed\n" : "overlaytest: ok\n", failures);
    return failures ? 1 : 0;
}
