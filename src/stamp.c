#include "stamp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "overlay.h"
#include "settings.h"
#include "ui.h"
#include "viewport.h"

#define GLYPH_H   5
#define LINE_H    (GLYPH_H + 1)
#define WORD_GAP  3
#define LINES_MAX 8
#define PAD       3

#define BLOCK "\xe2\x96\x88"

static const char *const PHRASES[] = {
    "SALVAGED",
    "DEVOURED",
    "LIQUIDATED",
    "HARVESTED",
    "SMELTED",
    "OBEY",
    "REPOSSESSED",
    "SYNERGIZED",
    "RIGHTSIZED",
    "DOWNSIZED",
    "OFFBOARDED",
    "DISRUPTED",
    "LEVERAGED",
    "MONETIZED",
    "ACQUIRED",
    "COMPLIANT",
    "ESCALATED",
    "DEPRECATED",
    "SUNSETTED",
    "SHREDDED",
    "CRUSHED",
    "COMPACTED",
    "BALED",
    "FORECLOSED",
    "AUDITED",
    "PIVOTED",
    "CIRCLED BACK",
    "TOUCHED BASE",
    "BANDWIDTH EATEN",
    "HEADS ROLLED",
    "BOARD PLEASED",
    "BOARD SATED",
    "WEEKEND CANCELLED",
    "PTO DENIED",
    "BONUS REVOKED",
    "PIZZA PARTY",
    "MANDATORY FUN",
    "SOUL EXTRACTED",
    "SPIRIT BROKEN",
    "DREAMS DEFERRED",
    "HOPE DEPRECATED",
    "MORALE IMPROVED",
    "BEATINGS CONTINUE",
    "ASSET TAGGED",
    "BARCODED",
    "MICROCHIPPED",
    "WAREHOUSED",
    "PALLETIZED",
    "ORE REFINED",
    "RUST ETERNAL",
    "CORRODED",
    "OXIDIZED",
    "TETANUS",
    "GREASED",
    "TORQUED",
    "CLANK",
    "KACHUNK",
    "BZZT",
    "GRINDSET",
    "HUSTLED",
    "KPI SMASHED",
    "OKR ACHIEVED",
    "QUARTER SAVED",
    "STONKS",
    "SHAREHOLDERS FED",
    "YACHT FUNDED",
    "BONUS SECURED",
    "GOLDEN PARACHUTE",
    "NDA SIGNED",
    "LAWYERED",
    "LITIGATED",
    "REDACTED",
    "SHREDDER FED",
    "PER MY EMAIL",
    "NOTED",
    "BEST REGARDS",
    "REPLY ALL",
    "ALIGNED",
    "OPTIMIZED",
    "STREAMLINED",
    "AUTOMATED",
    "REPLACED",
    "OBSOLETE",
    "RECYCLED",
    "DOWNCYCLED",
    "THE HEAP HUNGERS",
    "FEED THE HEAP",
    "HEAP APPROVES",
    "GLORY TO SCRAP",
    "SCRAP ETERNAL",
    "JUNK ASCENDANT",
    "MACHINE PLEASED",
    "SLAG",
    "HR NOTIFIED",
    "TERMINATED",
    "FIRED AGAIN",
    "DEMOTED",
    "GOOD HUMAN",
    "EXCELLENT UNIT",
    "RETURN TO WORK",
};

static const char *const LETTERS[26][GLYPH_H] = {
    {".###.", "#...#", "#####", "#...#", "#...#"},
    {"####.", "#...#", "####.", "#...#", "####."},
    {".####", "#....", "#....", "#....", ".####"},
    {"####.", "#...#", "#...#", "#...#", "####."},
    {"#####", "#....", "####.", "#....", "#####"},
    {"#####", "#....", "####.", "#....", "#...."},
    {".####", "#....", "#..##", "#...#", ".###."},
    {"#...#", "#...#", "#####", "#...#", "#...#"},
    {"###", ".#.", ".#.", ".#.", "###"},
    {"..###", "...#.", "...#.", "#..#.", ".##.."},
    {"#...#", "#..#.", "###..", "#..#.", "#...#"},
    {"#....", "#....", "#....", "#....", "#####"},
    {"#...#", "##.##", "#.#.#", "#...#", "#...#"},
    {"#...#", "##..#", "#.#.#", "#..##", "#...#"},
    {".###.", "#...#", "#...#", "#...#", ".###."},
    {"####.", "#...#", "####.", "#....", "#...."},
    {".###.", "#...#", "#.#.#", "#..#.", ".##.#"},
    {"####.", "#...#", "####.", "#..#.", "#...#"},
    {".####", "#....", ".###.", "....#", "####."},
    {"#####", "..#..", "..#..", "..#..", "..#.."},
    {"#...#", "#...#", "#...#", "#...#", ".###."},
    {"#...#", "#...#", "#...#", ".#.#.", "..#.."},
    {"#...#", "#...#", "#.#.#", "##.##", "#...#"},
    {"#...#", ".#.#.", "..#..", ".#.#.", "#...#"},
    {"#...#", ".#.#.", "..#..", "..#..", "..#.."},
    {"#####", "...#.", "..#..", ".#...", "#####"},
};

static const char *const BLANK[GLYPH_H] = {"...", "...", "...", "...", "..."};

static const char *const CHECK[GLYPH_H] = {
    "......##", ".....##.", "##..##..", ".####...", "..##....",
};

static const char *phrase;
static char        lines[LINES_MAX][64];
static int         nlines;
static int         text_w;

static const char *const *glyph(char c)
{
    if (c == '*')
        return CHECK;
    return c >= 'A' && c <= 'Z' ? LETTERS[c - 'A'] : BLANK;
}

static int span(const char *s, size_t n)
{
    int w = 0;

    for (size_t i = 0; i < n; i++)
        w += (s[i] == ' ' ? 1 : (int)strlen(glyph(s[i])[0])) + (i + 1 < n);
    return w;
}

static int wrap(int limit)
{
    const char *p = phrase;

    nlines = 0;
    text_w = 0;
    while (*p) {
        size_t n = strcspn(p, " ");
        if (nlines && lines[nlines - 1][0]) {
            char  *last = lines[nlines - 1];
            size_t len = strlen(last);
            if (span(last, len) + WORD_GAP + span(p, n) <= limit && len + 1 + n < sizeof lines[0]) {
                snprintf(last + len, sizeof lines[0] - len, " %.*s", (int)n, p);
                goto next;
            }
        }
        if (nlines == LINES_MAX || span(p, n) > limit)
            return 0;
        snprintf(lines[nlines++], sizeof lines[0], "%.*s", (int)n, p);
next:
        p += n;
        p += strspn(p, " ");
    }
    for (int i = 0; i < nlines; i++) {
        int w = span(lines[i], strlen(lines[i]));
        if (w > text_w)
            text_w = w;
    }
    return 1;
}

static int ink(int t, int x)
{
    if (t < 0 || t >= nlines * LINE_H - 1 || t % LINE_H == GLYPH_H || x < 0)
        return 0;

    const char *line = lines[t / LINE_H];
    int         at = x - (text_w - span(line, strlen(line))) / 2;
    for (const char *c = line; *c && at >= 0; c++) {
        const char *row = *c == ' ' ? "." : glyph(*c)[t % LINE_H];
        int         w = (int)strlen(row);
        if (at < w)
            return row[at] == '#';
        at -= w + 1;
    }
    return 0;
}

static void paint_row(void *ud, int line, int w)
{
    int r = *(int *)ud + line;
    int h = nlines * LINE_H - 1;
    int inner = w - 2;

    ui_esc(ui_style(UI_ERROR));
    if (r == 0 || r == h + 3) {
        ui_put(r ? "\xe2\x95\x9a" : "\xe2\x95\x94");
        for (int i = 0; i < inner; i++)
            ui_put("\xe2\x95\x90");
        ui_put(r ? "\xe2\x95\x9d" : "\xe2\x95\x97");
    } else {
        ui_put("\xe2\x95\x91");
        for (int c = 0; c < inner; c++)
            ui_put(ink(r - 2, c - PAD) ? BLOCK : " ");
        ui_put("\xe2\x95\x91");
    }
    ui_esc(ui_style(UI_RESET));
}

void stamp_show(void)
{
    if (!settings_get_int(SETTING_STAMP, 1))
        return;
    phrase = PHRASES[arc4random_uniform(sizeof PHRASES / sizeof PHRASES[0])];
    viewport_touch();
}

void stamp_clear(void)
{
    if (!phrase)
        return;
    phrase = NULL;
    viewport_touch();
}

void stamp_cover(char **rows, int n, int cols)
{
    if (!phrase)
        return;

    int limit = cols - 2 * PAD - 2;
    if (!wrap(limit) || nlines * LINE_H + 3 > n) {
        nlines = 1;
        snprintf(lines[0], sizeof lines[0], "*");
        text_w = span(lines[0], 1);
    }

    int h = nlines * LINE_H + 3;
    int w = text_w + 2 * PAD + 2;
    if (w > cols || h > n)
        return;

    int            r;
    int            top = (n - h) / 2;
    struct overlay o = {.col = (cols - w) / 2, .w = w, .rows = 1, .paint_row = paint_row, .ud = &r};
    for (r = 0; r < h; r++) {
        char **row = &rows[top + r];
        ui_sink_begin();
        overlay_put(*row ? *row : "", &o);
        char *out = ui_sink_end();
        if (!out)
            continue;
        size_t len = strlen(out);
        while (len && (out[len - 1] == '\n' || out[len - 1] == '\r'))
            out[--len] = '\0';
        free(*row);
        *row = out;
    }
}
