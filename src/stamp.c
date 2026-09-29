#include "stamp.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"
#include "kvlog.h"
#include "overlay.h"
#include "settings.h"
#include "text.h"
#include "ui.h"
#include "vendor/agents/backend.h"
#include "viewport.h"

#define GLYPH_H   5
#define LINE_H    (GLYPH_H + 1)
#define WORD_GAP  3
#define LINES_MAX 8
#define PAD       3

#define STAMP_MODEL   "claude-haiku-4-5-20251001"
#define STAMP_CHOICES 6
#define PHRASE_MAX    32
#define REQUESTED_MAX 64

#define BLOCK "\xe2\x96\x88"

static const char *const PHRASES[] = {
    "APPROVED",
    "SYNERGY ACHIEVED",
    "DELIVERABLE SHIPPED",
    "ACTION ITEM CLOSED",
    "KPI MET",
    "ROI POSITIVE",
    "PIVOT COMPLETE",
    "RIGHTSIZED",
    "PROCESSED",
    "SALVAGED",
    "LIQUIDATED",
    "COMPLIANT",
    "QUARTERLY TARGET MET",
    "PER MY LAST EMAIL",
    "SHAREHOLDER VALUE UNLOCKED",
    "PARADIGM SHIFTED",
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

static const char *phrase;
static char        lines[LINES_MAX][64];
static int         nlines;
static int         text_w;
static int         big;

static const char *const *glyph(char c)
{
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

static void big_row(const char *line, int gr)
{
    for (const char *c = line; *c; c++) {
        const char *row = *c == ' ' ? "." : glyph(*c)[gr];
        for (const char *d = row; *d; d++)
            ui_put(*d == '#' ? BLOCK : " ");
        if (c[1])
            ui_put(" ");
    }
}

static void paint_row(void *ud, int line, int w)
{
    int r = *(int *)ud + line;
    int h = big ? nlines * LINE_H - 1 : 1;
    int inner = w - 2;

    ui_esc(ui_style(UI_ERROR));
    if (r == 0 || r == h + 3) {
        ui_put(r ? "\xe2\x95\x9a" : "\xe2\x95\x94");
        for (int i = 0; i < inner; i++)
            ui_put("\xe2\x95\x90");
        ui_put(r ? "\xe2\x95\x9d" : "\xe2\x95\x97");
        ui_esc(ui_style(UI_RESET));
        return;
    }
    ui_put("\xe2\x95\x91");
    int t = r - 2;
    if (t < 0 || t >= h || (big && t % LINE_H == GLYPH_H)) {
        ui_pad(inner);
    } else {
        const char *text = big ? lines[t / LINE_H] : phrase;
        int         tw = big ? span(text, strlen(text)) : (int)strlen(text);
        int         left = (inner - tw) / 2;

        ui_pad(left);
        if (big)
            big_row(text, t % LINE_H);
        else
            ui_put(text);
        ui_pad(inner - left - tw);
    }
    ui_put("\xe2\x95\x91");
    ui_esc(ui_style(UI_RESET));
}

static void base(const char *name, char *out, size_t size)
{
    size_t n;

    name += *name == '@';
    n = strlen(name);
    while (n && name[n - 1] >= '0' && name[n - 1] <= '9')
        n--;
    snprintf(out, size, "%.*s", (int)n, name);
}

static void clean(char *line, char *out, size_t size)
{
    size_t n = 0;

    for (char *p = line; *p && n + 1 < size && n < PHRASE_MAX; p++) {
        char c = (char)toupper((unsigned char)*p);
        if ((c >= 'A' && c <= 'Z') || (c == ' ' && n && out[n - 1] != ' '))
            out[n++] = c;
    }
    while (n && out[n - 1] == ' ')
        n--;
    out[n] = '\0';
}

static void ask_and_cache(const char *key)
{
    char text[1024];
    snprintf(text, sizeof text,
             "A work session named \"%s\" just finished a task. Write %d rubber-stamp phrases "
             "announcing it, each a pun or play on the name, 1 to 3 words, in the voice of a "
             "dystopian corporate scrapyard. Examples: headcount gives HEADCOUNT REDUCED; "
             "stakeholder gives STAKED; terminator gives TERMINATED WITH CAUSE. Uppercase "
             "letters and spaces only. One phrase per line, nothing else.",
             key, STAMP_CHOICES);

    backend_opts o = {0};
    o.name = "claude";
    o.model = STAMP_MODEL;
    o.system = "Write the phrases without using tools.";
    o.session_name = APP_NAME " stamp helper";
    o.ephemeral = 1;
    o.disable_tools = 1;
    Backend *b = backend_open_ex(&o);
    if (!b)
        return;

    char *answer = b->ask(b, text);
    char  joined[STAMP_CHOICES * (PHRASE_MAX + 1) + 1] = "";
    int   count = 0;
    for (char *line = answer ? strtok(answer, "\n") : NULL; line && count < STAMP_CHOICES;
         line = strtok(NULL, "\n")) {
        char one[PHRASE_MAX + 1];
        clean(line, one, sizeof one);
        if (!*one)
            continue;
        size_t len = strlen(joined);
        snprintf(joined + len, sizeof joined - len, "%s%s", count++ ? "|" : "", one);
    }
    char path[1200];
    if (count && path_config_file(path, sizeof path, "stamps"))
        (void)kvlog_append(path, key, joined);
    free(answer);
    b->close(b);
}

static int cached(const char *key, char *out, size_t size)
{
    char path[1200];

    return path_config_file(path, sizeof path, "stamps") && kvlog_lookup(path, key, out, size);
}

void stamp_prepare(const char *name)
{
    static char requested[REQUESTED_MAX][64];
    static int  nrequested;
    char        key[64];
    char        val[STAMP_CHOICES * (PHRASE_MAX + 1) + 1];

    if (!settings_get_int(SETTING_STAMP, 1) || !name || !*name)
        return;
    base(name, key, sizeof key);
    if (!*key)
        return;
    for (int i = 0; i < nrequested; i++)
        if (!strcmp(requested[i], key))
            return;
    snprintf(requested[nrequested++ % REQUESTED_MAX], sizeof requested[0], "%s", key);
    if (nrequested > REQUESTED_MAX)
        nrequested = REQUESTED_MAX;
    if (cached(key, val, sizeof val))
        return;

    pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            int null = open("/dev/null", O_RDWR);
            if (null >= 0) {
                dup2(null, STDIN_FILENO);
                dup2(null, STDOUT_FILENO);
                dup2(null, STDERR_FILENO);
                if (null > STDERR_FILENO)
                    close(null);
            }
            for (int fd = getdtablesize() - 1; fd > STDERR_FILENO; fd--)
                close(fd);
            ask_and_cache(key);
        }
        _exit(0);
    }
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
}

void stamp_show(const char *name)
{
    static char chosen[PHRASE_MAX + 1];
    char        key[64];
    char        val[STAMP_CHOICES * (PHRASE_MAX + 1) + 1];

    if (!settings_get_int(SETTING_STAMP, 1))
        return;
    phrase = NULL;
    if (name && *name) {
        base(name, key, sizeof key);
        if (cached(key, val, sizeof val)) {
            int count = 1;
            for (char *p = val; *p; p++)
                count += *p == '|';
            char *at = val;
            for (int pick = (int)arc4random_uniform((unsigned)count); pick > 0; pick--)
                at = strchr(at, '|') + 1;
            at[strcspn(at, "|")] = '\0';
            snprintf(chosen, sizeof chosen, "%s", at);
            if (*chosen)
                phrase = chosen;
        }
    }
    if (!phrase)
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
    big = wrap(limit) && nlines * LINE_H - 1 + 4 <= n;
    if (!big)
        text_w = (int)strlen(phrase);

    int h = (big ? nlines * LINE_H - 1 : 1) + 4;
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
