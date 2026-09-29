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
#define MARGIN_COLS 2
#define MARGIN_ROWS 1

#define STAMP_MODEL   "claude-haiku-4-5-20251001"
#define STAMP_CHOICES 6
#define PHRASE_MAX    32
#define REQUESTED_MAX 64

#define BLOCK "\xe2\x96\x88"
#define SHADE "\xe2\x96\x91"

static const char *const PHRASES[] = {
    "SALVAGED",
    "DEVOURED",
    "LIQUIDATED",
    "HARVESTED",
    "CONSUMED",
    "SCRAPPED",
    "RENDERED",
    "SMELTED",
    "OBEY",
    "RUSTED",
    "EXTRACTED",
    "REPOSSESSED",
    "THE HEAP HUNGERS",
    "BOARD PLEASED",
    "WEEKEND CANCELLED",
    "FEELINGS LIQUIDATED",
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
    if (t < 0 || t >= nlines * LINE_H || t % LINE_H == GLYPH_H || x < 0)
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
    int t = *(int *)ud + line - MARGIN_ROWS;

    for (int c = 0; c < w; c++) {
        int x = c - MARGIN_COLS - 1;
        if (ink(t, x)) {
            ui_esc(ui_style(UI_ERROR));
            ui_put(BLOCK);
        } else if (ink(t - 1, x + 1)) {
            ui_esc(ui_style(UI_DIM));
            ui_put(SHADE);
        } else {
            ui_put(" ");
        }
    }
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
             "announcing it, each a pun or play on the name, usually 1 word, sometimes 2, in the voice of an "
             "unhinged dystopian corporate scrapyard run by feral executive robots: menacing, "
             "absurd, darkly funny, a little too honest about what happens to the workers. "
             "Examples: headcount gives DECAPITATED or HEADS ROLLED; stakeholder gives "
             "STAKED or IMPALED; terminator gives TERMINATED or EXTERMINATED; layoff gives "
             "ERASED. Uppercase letters and spaces only. One phrase per line, nothing else.",
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
    if (count && path_config_file(path, sizeof path, "stamps3"))
        (void)kvlog_append(path, key, joined);
    free(answer);
    b->close(b);
}

static int cached(const char *key, char *out, size_t size)
{
    char path[1200];

    return path_config_file(path, sizeof path, "stamps3") && kvlog_lookup(path, key, out, size);
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

    int limit = cols - 2 * MARGIN_COLS - 1;
    if (!wrap(limit) || nlines * LINE_H + 2 * MARGIN_ROWS > n) {
        nlines = 1;
        snprintf(lines[0], sizeof lines[0], "*");
        text_w = span(lines[0], 1);
    }

    int h = nlines * LINE_H + 2 * MARGIN_ROWS;
    int w = text_w + 1 + 2 * MARGIN_COLS;
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
