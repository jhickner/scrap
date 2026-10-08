
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "image.h"
#include "screenmodel.h"
#include "ui.h"
#include "viewport.h"

static int failures;
static int tap_read = -1;
static int sync_begin;
static int sync_end;
static int places;
static int places_unnamed;

static void pump(struct screen *s)
{
    fflush(stdout);
    static char buf[1 << 20];
    ssize_t     n;
    while ((n = read(tap_read, buf, sizeof buf)) > 0) {
        for (ssize_t i = 0; i + 8 <= n; i++) {
            if (!memcmp(buf + i, "\x1b[?2026h", 8))
                sync_begin++;
            if (!memcmp(buf + i, "\x1b[?2026l", 8))
                sync_end++;
            if (!memcmp(buf + i, "\x1b_Ga=p,U=1,", 9)) {
                places++;
                const char *end = memchr(buf + i, '\\', (size_t)(n - i));
                const char *named = memmem(buf + i, end ? (size_t)(end - (buf + i)) : 0, ",p=", 3);
                if (!named)
                    places_unnamed++;
            }
        }
        feed(s, buf, (size_t)n);
    }
}

static void set_size(int cols, int rows)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%d", cols);
    setenv("COLUMNS", buf, 1);
    snprintf(buf, sizeof buf, "%d", rows);
    setenv("LINES", buf, 1);
}

static void say(const char *t)
{
    viewport_write(t, strlen(t));
    viewport_write("\n", 1);
}

static int row_cells(struct screen *s, int r, int *last_col)
{
    int n = 0;
    *last_col = -1;
    for (int c = 0; c < s->cols; c++) {
        const char *cell = s->cell[r][c];
        if (strncmp(cell, "\xf4\x8e\xbb\xae", 4) == 0) {
            n++;
            *last_col = c;
        }
    }
    return n;
}

static void report(struct screen *s, const char *what)
{
    fprintf(stderr, "--- %s (%dx%d)\n", what, s->cols, s->rows);
    for (int r = 0; r < s->rows; r++) {
        int last;
        int n = row_cells(s, r, &last);
        if (n)
            fprintf(stderr, "row %2d: %3d cells, last col %d\n", r, n, last);
    }
}

static void check_uniform(struct screen *s, const char *what)
{
    int first = -1, first_last = -1;
    for (int r = 0; r < s->rows; r++) {
        int last;
        int n = row_cells(s, r, &last);
        if (!n) {
            first = -1;
            continue;
        }
        if (last >= s->cols - 1) {
            fprintf(stderr, "FAIL %s: row %d reaches the last column (%d of %d)\n",
                    what, r, last, s->cols);
            failures++;
            report(s, what);
            return;
        }
        if (first < 0) {
            first = n;
            first_last = last;
            continue;
        }
        if (n != first || last != first_last) {
            fprintf(stderr, "FAIL %s: row %d has %d cells ending at %d, not %d/%d\n",
                    what, r, n, last, first, first_last);
            failures++;
            report(s, what);
            return;
        }
    }
}

static int screens_differ(const struct screen *a, const struct screen *b)
{
    for (int r = 0; r < a->rows; r++)
        for (int c = 0; c < a->cols; c++)
            if (strcmp(a->cell[r][c], b->cell[r][c]))
                return r;
    return -1;
}

static int placeholder_rows(const struct screen *s)
{
    int n = 0;
    for (int r = 0; r < s->rows; r++) {
        int last;
        if (row_cells((struct screen *)s, r, &last))
            n++;
    }
    return n;
}

static void match_fresh(struct screen *s, const char *what)
{
    static struct screen fresh;
    pump(s);
    screen_init(&fresh, s->rows, s->cols);
    viewport_forget();
    viewport_paint();
    pump(&fresh);
    int r = screens_differ(s, &fresh);
    if (r >= 0) {
        fprintf(stderr, "FAIL scroll %s: row %d differs from a full repaint\n  have %s\n  want %s\n",
                what, r, row_text(s, r), row_text(&fresh, r));
        failures++;
    }
    memcpy(s, &fresh, sizeof fresh);
}

static void place_image(int w, int h);

static void lines(const char *tag, int n)
{
    char line[64];
    for (int i = 0; i < n; i++) {
        snprintf(line, sizeof line, "%s %d", tag, i);
        say(line);
    }
}

static void scroll_paths(void)
{
    static struct screen s;
    set_size(80, 30);
    screen_init(&s, 30, 80);
    viewport_clear();
    viewport_forget();
    lines("above", 40);
    place_image(1404, 1872);
    lines("below", 40);
    viewport_paint();
    match_fresh(&s, "bottom");

    int seen = 0;
    for (int i = 0; i < 70; i++) {
        viewport_scroll(1);
        match_fresh(&s, "line up");
        seen |= placeholder_rows(&s) > 0;
    }
    if (!seen) {
        fprintf(stderr, "FAIL scroll: the image never came into view\n");
        failures++;
    }
    for (int i = 0; i < 70; i++) {
        viewport_scroll(-1);
        match_fresh(&s, "line down");
    }
    for (int i = 0; i < 6; i++) {
        viewport_scroll(i < 3 ? 27 : -27);
        match_fresh(&s, i < 3 ? "page up" : "page down");
    }
    viewport_scroll(45);
    match_fresh(&s, "into the image");
    say("arrives while scrolled back");
    viewport_paint();
    match_fresh(&s, "new output while scrolled back");
    set_size(70, 30);
    screen_init(&s, 30, 70);
    viewport_paint();
    match_fresh(&s, "resize while scrolled back");
    viewport_scroll_end();
    match_fresh(&s, "back to the bottom");
    lines("more", 10);
    viewport_paint();
    match_fresh(&s, "new output at the bottom");
}

static void placements(void)
{
    static struct screen s;
    set_size(80, 30);
    screen_init(&s, 30, 80);
    viewport_clear();
    viewport_forget();
    pump(&s);
    places = places_unnamed = 0;
    place_image(800, 600);
    say("after");
    viewport_paint();
    pump(&s);
    const int W[] = {60, 90, 60};
    for (int i = 0; i < 3; i++) {
        set_size(W[i], 30);
        screen_init(&s, 30, W[i]);
        viewport_forget();
        viewport_paint();
        pump(&s);
    }
    if (!places || places_unnamed) {
        fprintf(stderr, "FAIL placements: %d of %d virtual placements carry no placement id, "
                        "so each redraw at a new size adds another one\n",
                places_unnamed, places);
        failures++;
    }
}

static void ids(void)
{
    enum { N = 300 };
    static uint32_t got[N];
    uint32_t        full = image_full_id(), inset = image_inset_id();
    cJSON          *o = cJSON_CreateObject();
    uint32_t        restored = 0;
    for (int i = 0; i < N; i++) {
        got[i] = image_new_id();
        if (i == 5) {
            restored = got[i] + 1;
            cJSON_AddNumberToObject(o, "id", restored);
            cJSON_AddNumberToObject(o, "w", 10);
            cJSON_AddNumberToObject(o, "h", 10);
            image_placed_load(o);
        }
        for (int j = 0; j < i; j++)
            if (got[j] == got[i]) {
                fprintf(stderr, "FAIL ids: image %d reuses the id of image %d (%06x)\n", i, j,
                        got[i]);
                failures++;
                cJSON_Delete(o);
                return;
            }
        if (got[i] == full || got[i] == inset || (i > 5 && got[i] == restored)) {
            fprintf(stderr, "FAIL ids: image %d got a reserved id %06x\n", i, got[i]);
            failures++;
            cJSON_Delete(o);
            return;
        }
        for (int b = 0; b < 3; b++)
            if (((got[i] >> (8 * b)) & 0xFF) < 0x40) {
                fprintf(stderr, "FAIL ids: %06x has a low byte\n", got[i]);
                failures++;
            }
    }
    cJSON_Delete(o);
}

static void place_image(int w, int h)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", 0x414243);
    cJSON_AddNumberToObject(o, "indent", 2);
    cJSON_AddNumberToObject(o, "w", w);
    cJSON_AddNumberToObject(o, "h", h);
    image_placed_load(o);
    cJSON_Delete(o);
}

int main(void)
{
    setenv("TMUX", "/tmp/imagerowtest,1,0", 1);
    set_size(80, 30);

    char path[] = "/tmp/scrap-imagerowtest-XXXXXX";
    int  wfd = mkstemp(path);
    tap_read = wfd >= 0 ? open(path, O_RDONLY) : -1;
    if (wfd < 0 || tap_read < 0)
        return 1;
    unlink(path);
    fflush(stdout);
    dup2(wfd, STDOUT_FILENO);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);

    ui_init();
    viewport_begin();

    struct screen s;
    screen_init(&s, 30, 80);

    viewport_clear();
    viewport_sync_placeholders(1);
    say("before");
    place_image(1404, 1872);
    say("between");
    place_image(2400, 600);
    say("after");
    viewport_paint();
    pump(&s);
    if (sync_begin != 1 || sync_end != 1) {
        fprintf(stderr, "FAIL image repaint sync: begin=%d end=%d\n",
                sync_begin, sync_end);
        failures++;
    }
    check_uniform(&s, "first paint");
    report(&s, "first paint");

    const int W[] = {66, 40, 100, 66};
    for (int i = 0; i < (int)(sizeof W / sizeof *W); i++) {
        set_size(W[i], 30);
        screen_init(&s, 30, W[i]);
        viewport_forget();
        viewport_paint();
        pump(&s);
        check_uniform(&s, "after resize");
        report(&s, "after resize");
    }

    scroll_paths();
    placements();
    ids();

    fflush(stdout);
    fprintf(stderr, failures ? "imagerowtest: FAILURES\n" : "imagerowtest: ok\n");
    return failures ? 1 : 0;
}

void bash_ran_load(const cJSON *st);
void hud_load(const cJSON *st);
void md_kept_load(const cJSON *st);
void prompt_echo_load(const cJSON *st);
void sessionload_divider_load(const cJSON *st);
void sidechannel_btw_load(const cJSON *st);
void view_keep_load(const cJSON *st);
void bash_ran_load(const cJSON *st) { (void)st; }
void hud_load(const cJSON *st) { (void)st; }
void md_kept_load(const cJSON *st) { (void)st; }
void prompt_echo_load(const cJSON *st) { (void)st; }
void sessionload_divider_load(const cJSON *st) { (void)st; }
void sidechannel_btw_load(const cJSON *st) { (void)st; }
void view_keep_load(const cJSON *st) { (void)st; }
