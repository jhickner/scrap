#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "image.h"
#include "screenmodel.h"
#include "ui.h"
#include "viewport.h"
#include "vncinset.h"
#include "vendor/cJSON.h"

#define COLS 80
#define ROWS 30

#define TL "\xe2\x94\x8c"
#define BR "\xe2\x94\x98"
#define V  "\xe2\x94\x82"

static int failures;
static int tap_read = -1;

#define CHECK(cond, ...)                         \
    do {                                         \
        if (!(cond)) {                           \
            fprintf(stderr, "FAIL: " __VA_ARGS__); \
            fprintf(stderr, "\n");               \
            failures++;                          \
        }                                        \
    } while (0)

static void pump(struct screen *s)
{
    fflush(stdout);
    char    buf[1 << 16];
    ssize_t n;
    while ((n = read(tap_read, buf, sizeof buf)) > 0)
        feed(s, buf, (size_t)n);
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

static struct vncinset *inset;

static void cover(char **rows, int n, int cols) { vncinset_cover(inset, rows, n, cols); }

struct fixed {
    struct vncinset_source base;
    uint8_t                rgb[4 * 3];
    uint64_t               gen;
    int                    closed;
};

static struct fixed fixed_src;

static int fixed_frame(struct vncinset_source *src, struct vncinset_frame *out)
{
    struct fixed *f = (struct fixed *)src;
    out->rgb = f->rgb;
    out->w = 1600;
    out->h = 1000;
    out->gen = f->gen;
    return 1;
}

static void fixed_close(struct vncinset_source *src) { ((struct fixed *)src)->closed++; }

static struct vncinset_source *fixed_open(const char *bot)
{
    (void)bot;
    fixed_src.base.frame = fixed_frame;
    fixed_src.base.close = fixed_close;
    return &fixed_src.base;
}

static int box_chars(const struct screen *s, int from_col)
{
    int n = 0;
    for (int r = 0; r < s->rows; r++)
        for (int c = from_col; c < s->cols; c++)
            if (!strcmp(s->cell[r][c], TL) || !strcmp(s->cell[r][c], BR) ||
                !strcmp(s->cell[r][c], V))
                n++;
    return n;
}

static void test_layout(void)
{
    struct vncinset_box b;
    CHECK(vncinset_layout(VNCINSET_RIGHT, 40, 80, 30, 320, 200, 8, 16, &b), "layout 80x30");
    CHECK(b.w == 32 && b.col == 48 && b.row == 0, "right box w=%d col=%d", b.w, b.col);
    CHECK(b.img_cols == 30 && b.img_rows == 9 && b.h == 11, "image %dx%d h=%d", b.img_cols,
          b.img_rows, b.h);

    CHECK(vncinset_layout(VNCINSET_LEFT, 40, 80, 30, 320, 200, 8, 16, &b) && b.col == 0,
          "left box col=%d", b.col);

    CHECK(vncinset_layout(VNCINSET_RIGHT, 100, 80, 6, 320, 200, 8, 16, &b), "short area");
    CHECK(b.img_rows == 4 && b.img_cols == 13 && b.w == 15 && b.col == 65,
          "short area image %dx%d w=%d col=%d", b.img_cols, b.img_rows, b.w, b.col);

    CHECK(!vncinset_layout(VNCINSET_RIGHT, 40, 10, 30, 320, 200, 8, 16, &b), "too narrow");
    CHECK(!vncinset_layout(VNCINSET_RIGHT, 40, 80, 3, 320, 200, 8, 16, &b), "too short");
}

static void test_downscale(void)
{
    uint8_t src[4 * 2 * 3], dst[2 * 1 * 3];
    for (int i = 0; i < 8; i++) {
        src[i * 3] = (uint8_t)(i < 2 || (i >= 4 && i < 6) ? 100 : 200);
        src[i * 3 + 1] = 10;
        src[i * 3 + 2] = (uint8_t)(i * 10);
    }
    vncinset_downscale(src, 4, 2, dst, 2, 1);
    CHECK(dst[0] == 100 && dst[3] == 200 && dst[1] == 10, "box filter %d %d %d", dst[0],
          dst[3], dst[1]);
    CHECK(dst[2] == 25 && dst[5] == 45, "box filter blue %d %d", dst[2], dst[5]);
}

static void test_stub(void)
{
    struct vncinset_source *src = vncinset_stub_open();
    struct vncinset_frame   f;
    CHECK(src && src->frame(src, &f) && f.rgb && f.w > 0 && f.h > 0, "stub frame");
    src->close(src);
}

static void test_state(void)
{
    vncinset_set_opener(fixed_open);
    struct vncinset *v = vncinset_new("bot");
    vncinset_show(v, 1);
    CHECK(vncinset_stale(v), "no frame drawn yet");
    char *many[12];
    for (int i = 0; i < 12; i++)
        many[i] = strdup("");
    vncinset_cover(v, many, 12, 40);
    CHECK(!vncinset_stale(v), "frame drawn");
    fixed_src.gen++;
    CHECK(!vncinset_stale(v) || !image_available(), "new generation held until the interval");
    usleep((useconds_t)(VNCINSET_FRAME_INTERVAL * 1e6) + 20000);
    CHECK(vncinset_stale(v), "new generation");
    vncinset_show(v, 0);
    CHECK(fixed_src.closed == 1, "source closed on hide");
    CHECK(!vncinset_stale(v), "hidden inset is never stale");
    vncinset_free(v);
    for (int i = 0; i < 12; i++)
        free(many[i]);
    vncinset_set_opener(NULL);
}

static void test_screen(void)
{
    char path[] = "/tmp/scrap-vncinsettest-XXXXXX";
    int  wfd = mkstemp(path);
    tap_read = wfd >= 0 ? open(path, O_RDONLY) : -1;
    if (wfd < 0 || tap_read < 0) {
        failures++;
        return;
    }
    unlink(path);
    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    dup2(wfd, STDOUT_FILENO);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);

    set_size(COLS, ROWS);
    ui_init();
    viewport_begin();
    viewport_on_cover(cover);

    struct screen s;
    screen_init(&s, ROWS, COLS);

    inset = vncinset_new("desk-bot");
    vncinset_show(inset, 1);

    char line[128];
    for (int i = 0; i < 60; i++) {
        snprintf(line, sizeof line, "line %02d %s", i,
                 "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
        say(line);
    }
    viewport_paint();
    pump(&s);

    CHECK(!strcmp(s.cell[0][48], TL), "top-left corner at 0,48: '%s'", s.cell[0][48]);
    CHECK(!strcmp(s.cell[10][79], BR), "bottom-right corner at 10,79: '%s'", s.cell[10][79]);
    CHECK(strstr(row_text(&s, 0), "desk-bot") != NULL, "title row names the bot");
    CHECK(!strncmp(row_text(&s, 5), "line", 4), "text left of the inset kept");
    int first = box_chars(&s, 0);
    CHECK(first == 20, "border cells drawn: %d", first);

    for (int i = 60; i < 67; i++) {
        snprintf(line, sizeof line, "more %02d", i);
        say(line);
    }
    viewport_paint();
    pump(&s);
    CHECK(!strcmp(s.cell[0][48], TL) && !strcmp(s.cell[10][79], BR),
          "inset in place after scroll");
    CHECK(box_chars(&s, 0) == first, "no stale border after scroll: %d vs %d",
          box_chars(&s, 0), first);

    viewport_scroll(5);
    viewport_paint();
    pump(&s);
    CHECK(box_chars(&s, 0) == first, "no stale border after scrollback: %d vs %d",
          box_chars(&s, 0), first);

    vncinset_set_side(inset, VNCINSET_LEFT);
    viewport_touch();
    viewport_paint();
    pump(&s);
    CHECK(!strcmp(s.cell[0][0], TL), "left inset corner: '%s'", s.cell[0][0]);
    CHECK(box_chars(&s, 32) == 0, "right inset erased after move");

    set_size(60, 24);
    screen_init(&s, 24, 60);
    viewport_forget();
    viewport_paint();
    pump(&s);
    CHECK(!strcmp(s.cell[0][0], TL), "inset after resize");

    vncinset_show(inset, 0);
    viewport_touch();
    viewport_paint();
    pump(&s);
    CHECK(box_chars(&s, 0) == 0, "inset erased when hidden");

    viewport_on_cover(NULL);
    vncinset_free(inset);
    viewport_end();
    fflush(stdout);
    dup2(saved, STDOUT_FILENO);
    close(saved);
}

int main(void)
{
    test_layout();
    test_downscale();
    test_stub();
    test_state();
    test_screen();
    fprintf(stderr, failures ? "vncinsettest: FAILURES\n" : "vncinsettest: ok\n");
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
