
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "screenmodel.h"
#include "ui.h"
#include "viewport.h"

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static int tap_read = -1;

static size_t pumped;

static void pump(struct screen *s)
{
    fflush(stdout);
    char    buf[65536];
    ssize_t n;
    while ((n = read(tap_read, buf, sizeof buf)) > 0) {
        pumped += (size_t)n;
        feed(s, buf, (size_t)n);
    }
}

static void say(const char *text)
{
    viewport_write(text, strlen(text));
    viewport_write("\n", 1);
}

static void redraw(struct screen *s)
{
    viewport_paint();
    pump(s);
}

static void refresh(struct screen *s, int cols, int rows)
{
    screen_init(s, rows, cols);
    viewport_forget();
    redraw(s);
}

static void set_size(int cols, int rows)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%d", cols);
    setenv("COLUMNS", buf, 1);
    snprintf(buf, sizeof buf, "%d", rows);
    setenv("LINES", buf, 1);
}

static void chrome(const char *a, const char *b)
{
    char *rows[2];
    rows[0] = (char *)a;
    rows[1] = (char *)b;
    viewport_chrome(rows, b ? 2 : 1, 0, -1);
}

static void check_resize_strands_nothing(struct screen *s)
{
    viewport_clear();
    for (int i = 0; i < 40; i++) {
        char line[64];
        snprintf(line, sizeof line, "transcript %d", i);
        say(line);
    }
    chrome("CHROME-sticky", "CHROME-prompt");

    const int W[] = {80, 20, 34, 52, 100, 24};
    for (int i = 0; i < (int)(sizeof W / sizeof *W); i++) {
        set_size(W[i], 24);
        refresh(s, W[i], 24);

        if (count_on_screen(s, "CHROME-prompt") != 1)
            fail("the chrome is on screen exactly once after a resize");
        if (count_on_screen(s, "transcript 39") != 1)
            fail("the newest transcript row is on screen exactly once");
        if (count_on_screen(s, "transcript 0") != 0)
            fail("a row far above the window is not on screen");
    }
}

static void check_bottom_up(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);
    say("first");
    say("second");
    say("third");
    chrome("CHROME-prompt", NULL);

    refresh(s, 80, 24);

    if (!strstr(row_text(s, 23), "CHROME-prompt"))
        fail("the chrome is on the last row");
    if (!strstr(row_text(s, 22), "third"))
        fail("the newest row is directly above the chrome");
    if (!strstr(row_text(s, 20), "first"))
        fail("the oldest row is pushed down to meet it");
    for (int r = 0; r < 20; r++)
        if (strstr(row_text(s, r), "first") || strstr(row_text(s, r), "third"))
            fail("nothing is painted at the top of an unfilled screen");
}

static void check_restore_keeps_chrome(struct screen *s)
{
    viewport_clear();
    viewport_chrome_clear();
    set_size(80, 24);
    say("banner");
    refresh(s, 80, 24);

    if (!strstr(row_text(s, 23), "banner"))
        fail("without chrome the newest row sits on the last screen row");

    char *row = "";
    viewport_chrome(&row, 1, 0, 0);
    redraw(s);

    if (strstr(row_text(s, 23), "banner"))
        fail("a reserved prompt row lifts the banner off the last row");
    if (!strstr(row_text(s, 22), "banner"))
        fail("the banner sits on the row above the reserved prompt");
}

static void check_tail(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);
    for (int i = 0; i < 100; i++) {
        char line[64];
        snprintf(line, sizeof line, "row %d", i);
        say(line);
    }
    chrome("CHROME-prompt", NULL);

    refresh(s, 80, 24);

    if (count_on_screen(s, "row 99") != 1)
        fail("the newest row is shown");
    if (count_on_screen(s, "row 77") != 1)
        fail("the oldest row that fits is shown");
    if (count_on_screen(s, "row 76") != 0)
        fail("the row above the window is not shown");
}

static void check_scroll(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);
    for (int i = 0; i < 100; i++) {
        char line[64];
        snprintf(line, sizeof line, "row %d", i);
        say(line);
    }
    chrome("CHROME-prompt", NULL);

    refresh(s, 80, 24);
    viewport_scroll(10);
    pump(s);
    if (count_on_screen(s, "row 89") != 1)
        fail("scrolling up moves the window back by that many rows");
    if (count_on_screen(s, "row 99") != 0)
        fail("scrolling up drops the newest rows off the bottom");

    refresh(s, 80, 24);
    viewport_scroll_end();
    pump(s);
    if (count_on_screen(s, "row 99") != 1)
        fail("scrolling back to the end shows the newest row again");

    viewport_scroll(10);
    pump(s);
    say("newest");
    redraw(s);
    if (count_on_screen(s, "newest") != 0)
        fail("new output does not drag the window to the tail");
    if (count_on_screen(s, "row 89") != 1)
        fail("new output leaves a scrolled window where it was");

    viewport_scroll_end();
    pump(s);
    say("newer still");
    redraw(s);
    if (count_on_screen(s, "newer still") != 1)
        fail("output arriving at the tail is followed");
}

static void check_chrome_scrolls_off(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);
    for (int i = 0; i < 100; i++) {
        char line[64];
        snprintf(line, sizeof line, "row %d", i);
        say(line);
    }
    chrome("CHROME-sticky", "CHROME-prompt");
    refresh(s, 80, 24);

    if (row_with(s, "CHROME-prompt") != 23)
        fail("at the end of the transcript the prompt is on the bottom row");

    viewport_scroll(1);
    pump(s);
    if (count_on_screen(s, "CHROME-prompt") != 0)
        fail("one row of scroll takes the last chrome row off the bottom");
    if (row_with(s, "CHROME-sticky") != 23)
        fail("what is left of the chrome is still against the bottom");

    viewport_scroll(1);
    pump(s);
    if (count_on_screen(s, "CHROME-sticky") != 0)
        fail("scrolling on takes the rest of the chrome with it");
    if (row_with(s, "row 99") != 23)
        fail("the transcript has the bottom row to itself");

    viewport_scroll_end();
    pump(s);
    if (row_with(s, "CHROME-prompt") != 23)
        fail("returning to the end puts the prompt back on the bottom row");
    if (row_with(s, "CHROME-sticky") != 22)
        fail("returning to the end puts the whole chrome back");
}

static void check_scroll_is_cheap(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    char line[400];
    memset(line, 'x', sizeof line - 1);
    line[sizeof line - 1] = '\0';
    for (int i = 0; i < 200; i++)
        say(line);
    chrome("CHROME-prompt", NULL);

    refresh(s, 80, 24);

    viewport_forget();
    pumped = 0;
    redraw(s);
    size_t full = pumped;
    if (full < 2000)
        fail("the screen under test is actually expensive to repaint");

    pumped = 0;
    redraw(s);
    if (pumped > full / 20)
        fail("a paint with nothing changed sends next to nothing");

    pumped = 0;
    viewport_scroll(3);
    pump(s);
    size_t scrolled_cost = pumped;
    if (scrolled_cost >= full / 3)
        fail("scrolling costs a fraction of a full repaint");

    if (count_on_screen(s, "CHROME-prompt") != 0)
        fail("a scroll takes the chrome off the bottom with everything else");
}

struct live {
    int  frame;
    int  done;
};

static void live_render(void *ud, int cols)
{
    (void)cols;
    const struct live *l = ud;
    ui_printf("LIVE-%s-%d", l->done ? "done" : "spin", l->frame);
    ui_put("\n");
}

static void check_live_entry(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    struct live *l = calloc(1, sizeof *l);
    unsigned mark = viewport_mark();
    viewport_item_begin(&(struct viewport_entry){.render = live_render, .ud = l, .free_ud = free, .reflow = 1});
    live_render(l, 80);
    viewport_item_end();

    refresh(s, 80, 24);
    if (count_on_screen(s, "LIVE-spin-0") != 1)
        fail("a live entry shows its first state");

    if (viewport_item_data(mark) != l)
        fail("the payload is reachable by mark");

    l->frame = 7;
    viewport_item_update(mark);
    refresh(s, 80, 24);
    if (count_on_screen(s, "LIVE-spin-7") != 1)
        fail("updating the payload redraws the entry");
    if (count_on_screen(s, "LIVE-spin-0") != 0)
        fail("the previous state of the entry is gone");

    say("after");
    l->done = 1;
    viewport_item_update(mark);
    refresh(s, 80, 24);
    if (count_on_screen(s, "LIVE-done-7") != 1)
        fail("an entry can still be changed once later output has landed");
    if (count_on_screen(s, "after") != 1)
        fail("the later output is still there");

    viewport_clear();
    if (viewport_item_data(mark) != NULL)
        fail("a mark for a dropped entry reports nothing");
    viewport_item_update(mark);
}

static int rendered_at;

static void width_render(void *ud, int cols)
{
    (void)ud;
    rendered_at = cols;
    ui_printf("WIDTH-%d", cols);
    ui_put("\n");
}

static void check_item_counting(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    unsigned before = viewport_mark();
    viewport_item_begin(&(struct viewport_entry){.render = width_render, .reflow = 1});
    width_render(NULL, 80);
    viewport_item_end();
    if (viewport_mark() - before != 1)
        fail("a begin/end pair makes exactly one entry");

    viewport_write("LOOSE\n", 6);
    viewport_write("PARTIAL", 7);
    before = viewport_mark();
    viewport_item_begin(&(struct viewport_entry){.render = width_render, .reflow = 1});
    width_render(NULL, 80);
    viewport_item_end();
    if (viewport_mark() - before != 2)
        fail("output in hand becomes an entry of its own");

    refresh(s, 80, 24);
    if (count_on_screen(s, "LOOSE") != 1)
        fail("a loose row is on screen once");
    if (count_on_screen(s, "PARTIAL") != 1)
        fail("a partial row is on screen once");
    if (count_on_screen(s, "WIDTH-80") != 2)
        fail("each entry is on screen once");
}

static void placeholder_rows(void *ud, int cols)
{
    (void)ud;
    (void)cols;

    for (int r = 0; r < 2; r++)
        ui_put("\x1b[38;2;1;2;3m\xf4\x8e\xbb\xae\xf4\x8e\xbb\xae\x1b[39m\n");
}

static void block_with_image(void *ud, int cols)
{
    ui_put("before\n");
    placeholder_rows(ud, cols);
    ui_put("after\n");
}

static void check_image_at_row(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    say("one");

    viewport_item_begin(&(struct viewport_entry){.render = block_with_image, .reflow = 1});
    block_with_image(NULL, 80);
    viewport_item_end();
    chrome("CHROME-prompt", NULL);

    refresh(s, 80, 24);

    int at = 0;
    for (int row = 1; row <= 24; row++)
        if (viewport_image_at(row, 1) == 0x010203u)
            at++;
    if (at != 2)
        fail("both placeholder rows name the image");
    for (int row = 1; row <= 24; row++)
        for (int col = 1; col <= 80; col++)
            if (viewport_image_at(row, col) != 0 &&
                viewport_image_at(row, col) != 0x010203u)
                fail("no other cell names an image");

    for (int row = 1; row <= 24; row++) {
        if (viewport_image_at(row, 1) != viewport_image_at(row, 2))
            fail("both placeholder cells name the image");
        for (int col = 3; col <= 80; col++)
            if (viewport_image_at(row, col) != 0)
                fail("a column past the image names nothing");
    }
    if (viewport_image_at(0, 1) != 0 || viewport_image_at(99, 1) != 0)
        fail("a row off the screen names nothing");
    if (viewport_image_at(1, 0) != 0)
        fail("a column off the screen names nothing");
}

static void image_beside_text(void *ud, int cols)
{
    (void)ud;
    (void)cols;
    ui_put("text \x1b[38;2;1;2;3m\xf4\x8e\xbb\xae\xf4\x8e\xbb\xae\x1b[39m tail\n");
}

static void check_image_at_column(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    viewport_item_begin(&(struct viewport_entry){.render = image_beside_text,
                                                 .reflow = 1});
    image_beside_text(NULL, 80);
    viewport_item_end();
    chrome("CHROME-prompt", NULL);

    refresh(s, 80, 24);

    int at = 0;
    for (int row = 1; row <= 24; row++) {
        if (viewport_image_at(row, 6) != 0x010203u)
            continue;
        at++;
        for (int col = 1; col <= 5; col++)
            if (viewport_image_at(row, col) != 0)
                fail("the text before an image names nothing");
        if (viewport_image_at(row, 7) != 0x010203u)
            fail("the second placeholder cell names the image");
        for (int col = 8; col <= 80; col++)
            if (viewport_image_at(row, col) != 0)
                fail("the text after an image names nothing");
    }
    if (at != 1)
        fail("the row beside the text names the image");
}

static int row_has_image(struct screen *s, int row)
{
    return strstr(row_text(s, row - 1), "\xf4\x8e\xbb\xae") != NULL;
}

static void check_image_at_row_scrolled(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    for (int i = 0; i < 30; i++) {
        char line[64];
        snprintf(line, sizeof line, "filler %d", i);
        say(line);
    }
    viewport_item_begin(&(struct viewport_entry){.render = block_with_image, .reflow = 1});
    block_with_image(NULL, 80);
    viewport_item_end();
    for (int i = 0; i < 30; i++) {
        char line[64];
        snprintf(line, sizeof line, "tail %d", i);
        say(line);
    }
    chrome("CHROME-prompt", NULL);

    refresh(s, 80, 24);

    for (int back = 0; back < 40; back++) {
        if (back)
            viewport_scroll(1);
        redraw(s);
        for (int row = 1; row <= 24; row++) {
            int      drawn = row_has_image(s, row);
            uint32_t named = viewport_image_at(row, 1);
            if (drawn && named != 0x010203u)
                fail("a drawn image row names the image while scrolled");
            if (!drawn && named != 0)
                fail("a row without the image names nothing while scrolled");
        }
    }
    viewport_scroll_end();
    redraw(s);
}

static void check_modal_holds_the_screen(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    for (int i = 0; i < 60; i++) {
        char line[64];
        snprintf(line, sizeof line, "transcript %d", i);
        say(line);
    }
    chrome("CHROME-prompt", NULL);
    refresh(s, 80, 24);

    viewport_scroll(20);
    redraw(s);
    if (strstr(row_text(s, 23), "CHROME-prompt"))
        fail("scrolling back takes the prompt off screen");

    char *rows[3] = {"MODAL-title", "MODAL-body", "MODAL-foot"};
    viewport_chrome_pin(1);
    viewport_chrome(rows, 3, 0, -1);
    redraw(s);

    int seen = 0;
    for (int row = 0; row < 24; row++)
        if (strstr(row_text(s, row), "MODAL-body"))
            seen = 1;
    if (!seen)
        fail("a modal opened while scrolled back is on screen");

    viewport_chrome_pin(0);
    chrome("CHROME-prompt", NULL);
    redraw(s);
    if (!strstr(row_text(s, 23), "transcript 40") ||
        !strstr(row_text(s, 0), "transcript 17"))
        fail("closing a modal returns to the scrolled position");
}

static void nested_render(void *ud, int cols)
{
    (void)ud;
    ui_put("HEAD\n");

    ui_capture_begin(cols - 2);
    ui_printf("INNER-%d", ui_columns());
    ui_put("\n");
    ui_printf("INNER-%d", ui_columns());
    ui_put("\n");
    char *painted = ui_capture_end();

    for (char *p = painted; p && *p;) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        ui_put("| ");
        ui_putn(p, len);
        ui_put("\n");
        if (!nl)
            break;
        p = nl + 1;
    }
    free(painted);
}

static void check_ends_blank(void)
{
    viewport_clear();
    set_size(80, 24);

    if (!viewport_ends_blank())
        fail("an empty transcript is not owed a blank row");

    viewport_write("text\n", 5);
    if (viewport_ends_blank())
        fail("a transcript ending in text is owed a blank row");

    viewport_write("\n", 1);
    if (!viewport_ends_blank())
        fail("a transcript ending in a blank row is not owed another");

    viewport_item_begin(&(struct viewport_entry){0});
    ui_put("in an entry\n");
    viewport_item_end();
    if (viewport_ends_blank())
        fail("an entry ending in text is owed a blank row");

    viewport_item_begin(&(struct viewport_entry){0});
    ui_put("styled\n\x1b[2m\x1b[0m\n");
    viewport_item_end();
    if (!viewport_ends_blank())
        fail("a row of nothing but styling is blank");
}

static void check_nested_capture(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    viewport_item_begin(&(struct viewport_entry){.render = nested_render, .reflow = 1});
    nested_render(NULL, 80);
    viewport_item_end();

    refresh(s, 80, 24);
    if (count_on_screen(s, "HEAD") != 1)
        fail("an entry with a nested capture keeps what it wrote first");
    if (count_on_screen(s, "| INNER-78") != 2)
        fail("every row of the inner render is prefixed");

    set_size(60, 24);
    refresh(s, 60, 24);
    if (count_on_screen(s, "HEAD") != 1)
        fail("a re-rendered entry still has what it wrote first");
    if (count_on_screen(s, "| INNER-58") != 2)
        fail("a re-rendered entry still prefixes every row");
    if (count_on_screen(s, "INNER-78") != 0)
        fail("nothing is left over from the width it was rendered at before");
}

static void check_reflow(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    rendered_at = 0;
    viewport_item_begin(&(struct viewport_entry){.render = width_render, .reflow = 1});
    width_render(NULL, 80);
    viewport_item_end();

    refresh(s, 80, 24);
    if (count_on_screen(s, "WIDTH-80") != 1)
        fail("a kept entry shows what it rendered at the first width");

    set_size(48, 24);
    refresh(s, 48, 24);
    if (rendered_at != 48)
        fail("a resize re-renders a kept entry at the new width");
    if (count_on_screen(s, "WIDTH-48") != 1)
        fail("the re-rendered entry is what is shown");
    if (count_on_screen(s, "WIDTH-80") != 0)
        fail("the entry laid out for the old width is gone");

    viewport_clear();
    set_size(80, 24);
    say("RAW-row");
    set_size(48, 24);
    refresh(s, 48, 24);
    if (count_on_screen(s, "RAW-row") != 1)
        fail("raw output survives a resize even without a renderer");
}

static void check_soft_wrap(struct screen *s)
{
    viewport_clear();
    set_size(20, 24);

    say("aaaaaaaaaaaaaaaaaaaa"
        "bbbbbbbbbbbbbbbbbbbb"
        "cccccccccccccccccccc");
    chrome("CHROME-prompt", NULL);

    refresh(s, 20, 24);

    if (count_on_screen(s, "aaaaaaaaaaaaaaaaaaaa") != 1)
        fail("the head of a wrapped row is shown");
    if (count_on_screen(s, "bbbbbbbbbbbbbbbbbbbb") != 1)
        fail("the middle of a wrapped row is shown");
    if (count_on_screen(s, "cccccccccccccccccccc") != 1)
        fail("the tail of a wrapped row is shown, not clipped");
}

static char *fake_encode(void *ud)
{
    (void)ud;
    return strdup("{\"text\":\"hi\"}");
}

static char *no_encode(void *ud)
{
    (void)ud;
    return NULL;
}

static void check_stash(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    say("FIRST-session");
    chrome("CHROME-prompt", NULL);
    refresh(s, 80, 24);
    if (count_on_screen(s, "FIRST-session") != 1)
        fail("the first screen is shown");

    struct viewport_state *a = viewport_state_new();
    struct viewport_state *b = viewport_state_new();
    if (!a || !b) {
        fail("a screen can be held aside");
        return;
    }

    viewport_stash(a);
    viewport_adopt(b);
    viewport_forget();
    say("SECOND-session");
    redraw(s);
    if (count_on_screen(s, "SECOND-session") != 1)
        fail("the second screen draws");
    if (count_on_screen(s, "FIRST-session") != 0)
        fail("the screen held aside is off the terminal");

    viewport_hold(1);
    viewport_stash(b);
    viewport_adopt(a);
    say("BACKGROUND-turn");
    viewport_paint();
    viewport_stash(a);
    viewport_adopt(b);
    viewport_hold(0);
    pump(s);
    if (count_on_screen(s, "BACKGROUND-turn") != 0)
        fail("a held screen paints nothing");
    if (count_on_screen(s, "SECOND-session") != 1)
        fail("the screen in front is untouched by the one behind");

    viewport_stash(b);
    viewport_adopt(a);
    viewport_forget();
    redraw(s);
    if (count_on_screen(s, "FIRST-session") != 1)
        fail("the first screen comes back");
    if (count_on_screen(s, "BACKGROUND-turn") != 1)
        fail("what was drawn behind is there on return");
    if (count_on_screen(s, "SECOND-session") != 0)
        fail("the other screen is gone");

    viewport_state_free(b);
    viewport_clear();
    viewport_state_free(a);
}

static void check_dump(void)
{
    viewport_clear();
    set_size(80, 24);

    viewport_write("RAW\n", 4);

    unsigned live = viewport_item_begin(&(struct viewport_entry){.render = width_render, .reflow = 1});
    width_render(NULL, 80);
    viewport_item_end();
    viewport_item_persist(live, "fake", fake_encode);

    unsigned mute = viewport_item_begin(&(struct viewport_entry){.render = width_render, .reflow = 1});
    width_render(NULL, 80);
    viewport_item_end();
    viewport_item_persist(mute, "fake", no_encode);

    char path[] = "/tmp/scrap-dumptest-XXXXXX";
    int  fd = mkstemp(path);
    if (fd < 0) {
        fail("a dump file could be made");
        return;
    }
    close(fd);

    if (!viewport_dump(path))
        fail("the entries dump");

    char  *text = NULL;
    size_t len = 0;
    FILE  *f = fopen(path, "r");
    if (f) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
            char *grown = realloc(text, len + n + 1);
            if (!grown)
                break;
            text = grown;
            memcpy(text + len, buf, n);
            len += n;
            text[len] = '\0';
        }
        fclose(f);
    }
    unlink(path);
    if (!text) {
        fail("the dump reads back");
        return;
    }

    if (!strstr(text, "{\"kind\":\"fake\",\"state\":{\"text\":\"hi\"}}"))
        fail("an entry that encodes travels as its state");
    if (!strstr(text, "\"kind\":\"rows\"") || !strstr(text, "RAW"))
        fail("raw output travels as rows");
    if (!strstr(text, "WIDTH-80"))
        fail("an entry that cannot encode falls back to its rows");
    free(text);
}

static void check_suspended_mark(void)
{
    viewport_clear();
    set_size(80, 24);

    viewport_suspend();
    unsigned muted = viewport_item_begin(&(struct viewport_entry){.render = width_render, .reflow = 1});
    width_render(NULL, 80);
    viewport_item_end();
    viewport_resume();

    unsigned next = viewport_item_begin(&(struct viewport_entry){.render = width_render, .reflow = 1});
    width_render(NULL, 80);
    viewport_item_end();

    if (muted)
        fail("an entry opened while the screen is suspended has no mark");
    if (muted == next)
        fail("the entry after a suspended one takes its own mark");
}

static void owed_render(void *ud, int cols)
{
    (void)cols;
    ui_put(ud);
    ui_put("\n");
}

static void check_owed_pad(struct screen *s)
{
    viewport_clear();
    set_size(80, 24);

    viewport_item_begin(&(struct viewport_entry){
        .render = owed_render, .ud = "OWED-A", .pad_after = 1});
    owed_render("OWED-A", 80);
    viewport_item_end();
    unsigned b = viewport_item_begin(&(struct viewport_entry){
        .render = owed_render, .ud = "OWED-B", .pad_before = 1});
    owed_render("OWED-B", 80);
    viewport_item_end();
    unsigned c = viewport_item_begin(&(struct viewport_entry){
        .render = owed_render, .ud = "OWED-C", .pad_before = 1});
    owed_render("OWED-C", 80);
    viewport_item_end();
    viewport_item_pad(b, 0);
    viewport_item_pad(c, 0);
    viewport_repad();

    refresh(s, 80, 24);
    int a = row_with(s, "OWED-A");
    if (a < 0 || !row_blank(s, a + 1) || row_with(s, "OWED-B") != a + 2)
        fail("a blank owed by the item before stays when the next item drops its pad");
    if (row_with(s, "OWED-C") != a + 3)
        fail("a padless item's own blank is hidden");
}

int main(void)
{
    set_size(80, 24);

    char path[] = "/tmp/scrap-viewporttest-XXXXXX";
    int  wfd = mkstemp(path);
    tap_read = wfd >= 0 ? open(path, O_RDONLY) : -1;
    if (wfd < 0 || tap_read < 0) {
        fprintf(stderr, "viewporttest: no temp file\n");
        return 1;
    }
    unlink(path);
    fflush(stdout);
    if (dup2(wfd, STDOUT_FILENO) < 0) {
        fprintf(stderr, "viewporttest: cannot redirect stdout\n");
        return 1;
    }
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);

    ui_init();
    viewport_begin();

    struct screen s;
    screen_init(&s, 24, 80);

    check_tail(&s);
    check_bottom_up(&s);
    check_restore_keeps_chrome(&s);
    check_scroll(&s);
    check_chrome_scrolls_off(&s);
    check_soft_wrap(&s);
    check_item_counting(&s);
    check_image_at_row(&s);
    check_image_at_column(&s);
    check_image_at_row_scrolled(&s);
    check_modal_holds_the_screen(&s);
    check_ends_blank();
    check_nested_capture(&s);
    check_reflow(&s);
    check_live_entry(&s);
    check_scroll_is_cheap(&s);
    check_resize_strands_nothing(&s);
    check_stash(&s);
    check_dump();
    check_suspended_mark();
    check_owed_pad(&s);

    fflush(stdout);
    if (failures)
        return 1;
    fprintf(stderr, "viewporttest: all checks passed\n");
    return 0;
}
