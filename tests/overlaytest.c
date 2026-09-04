#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "menu.h"
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

int main(void)
{
    struct menu m = {0};
    menu_add(&m, "implement", 0);
    menu_add(&m, "plan", 0);
    menu_add(&m, "attach", 1);
    m.open = 1;

    const char *under = "one\n"
                        "two two two two two two\n"
                        "\n"
                        "four four four four four\n"
                        "five five five five five\n"
                        "six six six six six six\n"
                        "seven seven seven seven\n"
                        "eight eight eight eight\n";

    struct overlay o = menu_overlay(&m, 2, 4, menu_width(&m));
    char          *drawn = composited(&o, under);

    want(drawn, 0, "one");
    want(drawn, 1, "two two two two two two");
    want(drawn, 2, "    ╭─────────────╮");
    want(drawn, 3, "four│ → implement │ four");
    want(drawn, 4, "five│   plan      │ five");
    want(drawn, 7, "eigh╰─────────────╯ight");
    free(drawn);

    /* the box runs past the end of what it stands on */
    o = menu_overlay(&m, 7, 4, menu_width(&m));
    drawn = composited(&o, under);
    want(drawn, 7, "eigh╭─────────────╮ight");
    want(drawn, 8, "    │ → implement │");
    want(drawn, 12, "    ╰─────────────╯");
    free(drawn);

    /* what a raw terminal was painted with carries returns */
    const char *raw = "one\r\ntwo two two two two two\r\n\r\n"
                      "four four four four four\r\n";
    o = menu_overlay(&m, 2, 4, menu_width(&m));
    drawn = drawn_raw(&o, raw);
    for (const char *p = strchr(drawn, '\r'); p; p = strchr(p + 1, '\r'))
        if (p[1] != '\n')
            fail("a return is left in the middle of a row", drawn);
    free(drawn);

    drawn = composited(&o, raw);
    want(drawn, 2, "    ╭─────────────╮");
    want(drawn, 3, "four│ → implement │ four");
    free(drawn);

    struct menu cycled = {0};
    menu_add(&cycled, "implement", 0);
    menu_add(&cycled, "plan", 0);
    menu_add(&cycled, "attach", 1);
    cycled.extra[0] = 1;
    cycled.extra[1] = 1;
    static const char *backends[] = {"claude", "grok"};
    cycled.choices = backends;
    cycled.choices_n = 2;
    cycled.choice = 0;
    snprintf(cycled.suffix, sizeof cycled.suffix, "%s", backends[0]);
    cycled.open = 1;

    o = menu_overlay(&cycled, 2, 4, menu_width(&cycled));
    drawn = composited(&o, under);
    want(drawn, 2, "    ╭──────────────────────╮");
    want(drawn, 3, "four│ → implement · claude │");
    want(drawn, 4, "five│   plan               │");
    free(drawn);

    menu_step(&cycled, 1);
    o = menu_overlay(&cycled, 2, 4, menu_width(&cycled));
    drawn = composited(&o, under);
    want(drawn, 3, "four│   implement          │");
    want(drawn, 4, "five│ → plan · claude      │");
    free(drawn);

    menu_steer(&cycled, 1);
    o = menu_overlay(&cycled, 2, 4, menu_width(&cycled));
    drawn = composited(&o, under);
    want(drawn, 4, "five│ → plan · grok        │");
    free(drawn);

    menu_step(&cycled, 1);
    o = menu_overlay(&cycled, 2, 4, menu_width(&cycled));
    drawn = composited(&o, under);
    want(drawn, 6, "seve│ → attach             │");
    free(drawn);

    printf(failures ? "overlaytest: %d failed\n" : "overlaytest: ok\n", failures);
    return failures ? 1 : 0;
}
