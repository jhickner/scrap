#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "replbox.h"
#include "restart.h"

void restart_shield_thread(void)
{
}

static int fails;

static void eq(const char *what, int got, int want)
{
    if (got == want)
        return;
    fprintf(stderr, "%s: got %d, want %d\n", what, got, want);
    fails++;
}

static void type(struct replbox *b, const char *text)
{
    replbox_set_text(b, text);
}

static void scroll_window(void)
{
    struct replbox b;
    replbox_init(&b, NULL, 0);
    replbox_width(&b, 20);

    type(&b, "one\ntwo\nthree\nfour\nfive\nsix");
    int rows = replbox_wants(&b);
    eq("rows", rows, 6);

    replbox_render(&b, rows);
    replbox_scroll(&b, 3);
    eq("caret at end", replbox_top(&b), 3);

    replbox_scroll(&b, 6);
    eq("all of it fits", replbox_top(&b), 0);

    replbox_scroll(&b, 10);
    eq("room to spare", replbox_top(&b), 0);

    b.repl.cursor = 0;
    b.fresh = 0;
    replbox_render(&b, rows);
    replbox_scroll(&b, 2);
    eq("caret at the top", replbox_top(&b), 0);

    replbox_free(&b);
}

static void wrapping(void)
{
    struct replbox b;
    replbox_init(&b, NULL, 0);
    replbox_width(&b, 12);
    type(&b, "aaaaaaaaaaaaaaaaaaaaaaaa");

    int narrow = replbox_wants(&b);
    replbox_width(&b, 40);
    int wide = replbox_wants(&b);

    if (narrow <= wide) {
        fprintf(stderr, "wrapping: %d rows narrow, %d wide\n", narrow, wide);
        fails++;
    }
    eq("one line when wide", wide, 1);

    replbox_free(&b);
}

static void line_out(void)
{
    struct replbox b;
    replbox_init(&b, NULL, 0);
    replbox_width(&b, 40);
    type(&b, "hello");
    eq("line", strcmp(replbox_line(&b), "hello"), 0);
    replbox_free(&b);
}

static void ghost_wraps(void)
{
    Repl r;
    repl_init(&r, NULL, 0);
    repl_set_placeholder(&r, "one two three four five six seven eight nine ten");
    int wide = repl_input_rows(&r, 80);
    int narrow = repl_input_rows(&r, 16);
    eq("ghost fits on a wide line", wide, 1);
    if (narrow <= wide) {
        fprintf(stderr, "ghost_wraps: %d rows narrow, %d wide\n", narrow, wide);
        fails++;
    }
    repl_free(&r);
}

int main(void)
{
    scroll_window();
    wrapping();
    line_out();
    ghost_wraps();

    if (fails) {
        fprintf(stderr, "replboxtest: %d failed\n", fails);
        return 1;
    }
    puts("replboxtest: ok");
    return 0;
}
