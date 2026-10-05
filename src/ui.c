#include "ui.h"

#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/wait.h>

#include "app.h"
#include "viewport.h"
#include "settings.h"
#include "tty.h"
#include "text.h"

static int use_color;
static int raw_newlines;

static const struct {
    const char *attr;
    const char *key;
    unsigned    rgb;
    int         tint;
} ROLES[UI_RESET] = {
    [UI_ACCENT]  = { NULL, "input", 0x22a2c9, 0 },
    [UI_ECHO]    = { NULL, "input_echo", 0x22a2c9, 90 },
    [UI_TEXT]    = { "39", NULL, 0, 0 },
    [UI_STICKY]  = { NULL, "sticky", 0x22a2c9, 90 },
    [UI_STICKY_DONE] = { NULL, "sticky_done", 0xac9739, 90 },
    [UI_BRAND]   = { NULL, "brand", 0xdfe2f1, 0 },
    [UI_SIDE]    = { NULL, "side", 0xdfe2f1, 90 },
    [UI_CHROME]  = { NULL, "chrome", 0x5e6687, 0 },
    [UI_DIM]     = { NULL, "dim", 0x6b7394, 0 },
    [UI_BODY]    = { NULL, "body", 0x979db4, 0 },
    [UI_BOLD]    = { "1",  "bold", 0xdfe2f1, 0 },
    [UI_ITALIC]  = { "3",  "italic", 0xdfe2f1, 0 },
    [UI_CODE]    = { NULL, "code", 0xdfe2f1, 0 },
    [UI_HEADING] = { "1",  "heading", 0xdfe2f1, 0 },
    [UI_LINK]    = { "4",  "link", 0x3d8fd1, 0 },
    [UI_ERROR]   = { NULL, "error", 0xc94922, 0 },
    [UI_OK]      = { NULL, "ok", 0xac9739, 0 },
    [UI_THINKING] = { "3", "thinking", 0x6679cc, 0 },
    [UI_TOOL]    = { NULL, "tool", 0xdfe2f1, 0 },
    [UI_SPIN]    = { NULL, "spinner", 0xdfe2f1, 0 },
    [UI_BASH]    = { NULL, "bash", 0xc94922, 90 },
    [UI_SYN_CMD]     = { NULL, "syntax_command", 0x3d8fd1, 0 },
    [UI_SYN_KEYWORD] = { NULL, "syntax_keyword", 0x6679cc, 0 },
    [UI_SYN_STRING]  = { NULL, "syntax_string", 0xac9739, 0 },
    [UI_SYN_COMMENT] = { NULL, "syntax_comment", 0x6b7394, 0 },
    [UI_SYN_VAR]     = { NULL, "syntax_variable", 0xc76b29, 0 },
    [UI_SYN_OP]      = { NULL, "syntax_operator", 0x979db4, 0 },
    [UI_SYN_FLAG]    = { NULL, "syntax_flag", 0x898ea4, 0 },
    [UI_SYN_NUMBER]  = { NULL, "syntax_number", 0xc08b30, 0 },
};

#define BACKGROUND_DEFAULT 0x202746
#define CURSOR_DEFAULT     0xf5f7ff

static char styles[UI_RESET][64];

static unsigned rgb[UI_RESET];
static unsigned background = BACKGROUND_DEFAULT;
static unsigned cursor = CURSOR_DEFAULT;
static int      term_bg = -1;
static int      term_bg_shown;
static int      term_active;

static char sel_bg[48];
static int  sel_row;

#define R(c) (((c) >> 16) & 0xff)
#define G(c) (((c) >> 8) & 0xff)
#define B(c) ((c) & 0xff)

static unsigned mix(unsigned fg, unsigned bg, int pct)
{
    return (fg * (100 - pct) + bg * pct + 50) / 100;
}

static void build_sel_bg(void)
{
    unsigned a = rgb[UI_ACCENT];
    snprintf(sel_bg, sizeof sel_bg, "\x1b[48;2;%u;%u;%um",
             mix(R(a), R(background), 92), mix(G(a), G(background), 92),
             mix(B(a), B(background), 92));
}

static void build_style(int role)
{
    const char *attr = ROLES[role].attr;
    if (!ROLES[role].key) {
        snprintf(styles[role], sizeof styles[role], "\x1b[%sm", attr ? attr : "0");
        return;
    }
    unsigned c = rgb[role];
    int      t = ROLES[role].tint;
    char wash[24] = "";
    if (t > 0)
        snprintf(wash, sizeof wash, ";48;2;%u;%u;%u", mix(R(c), R(background), t),
                 mix(G(c), G(background), t), mix(B(c), B(background), t));
    snprintf(styles[role], sizeof styles[role], "\x1b[%s%s38;2;%u;%u;%u%sm",
             attr ? attr : "", attr ? ";" : "", R(c), G(c), B(c), wash);
}

static unsigned hex(const struct settings *theme, const char *key, unsigned fallback)
{
    const char *v = settings_get(theme, key, NULL);
    char       *end;
    if (!v || *v != '#')
        return fallback;
    unsigned long c = strtoul(v + 1, &end, 16);
    return end == v + 7 && !*end ? (unsigned)c : fallback;
}

static int theme_path(const char *name, char *path, size_t cap)
{
    char leaf[256];
    return *name && *name != '.' && !strchr(name, '/') &&
           (size_t)snprintf(leaf, sizeof leaf, "themes/%s", name) < sizeof leaf &&
           path_config_file(path, cap, leaf) && !access(path, R_OK);
}

static void load_theme(const char *path)
{
    static struct settings theme;
    settings_load(&theme, path);

    background = hex(&theme, "background", BACKGROUND_DEFAULT);
    cursor = hex(&theme, "cursor", CURSOR_DEFAULT);
    term_bg = (int)hex(&theme, "terminal_background", (unsigned)-1);
    for (int i = 0; i < UI_RESET; i++)
        if (ROLES[i].key)
            rgb[i] = hex(&theme, ROLES[i].key, ROLES[i].rgb);
    for (int i = 0; i < UI_RESET; i++)
        build_style(i);
    build_sel_bg();
}

int ui_theme_set(const char *name, int save)
{
    char path[4096];
    if (!use_color || !theme_path(name, path, sizeof path))
        return 0;
    load_theme(path);
    if (term_active)
        ui_term_colors();
    if (save)
        settings_set_str(SETTING_THEME, name);
    return 1;
}

const char *ui_theme(void)
{
    return settings_get_str(SETTING_THEME, "default");
}

void ui_init(void)
{
    setlocale(LC_CTYPE, "");

    const char *no_color = getenv("NO_COLOR");
    use_color = isatty(STDOUT_FILENO) && !(no_color && *no_color);
    if (!use_color)
        return;

    char path[4096];
    load_theme(theme_path(ui_theme(), path, sizeof path) ? path : "");
}

unsigned ui_background(void)
{
    return background;
}

const char *ui_role_key(enum ui_role role, unsigned *fg, unsigned *wash, const char **attr)
{
    if ((int)role < 0 || role >= UI_RESET || !ROLES[role].key)
        return NULL;
    unsigned c = rgb[role];
    int      t = ROLES[role].tint;
    *fg = c;
    *wash = t > 0 ? mix(R(c), R(background), t) << 16 | mix(G(c), G(background), t) << 8 |
                        mix(B(c), B(background), t)
                  : background;
    *attr = ROLES[role].attr;
    return ROLES[role].key;
}

const char *ui_style(enum ui_role role)
{
    if (!use_color)
        return "";
    if (role == UI_RESET) {
        if (sel_row && sel_bg[0]) {
            static char keep[80];
            snprintf(keep, sizeof keep, "\x1b[0m%s", sel_bg);
            return keep;
        }
        return "\x1b[0m";
    }
    if (role < 0 || role >= UI_RESET)
        return "";
    return styles[role];
}

void ui_row_sel(int on)
{
    sel_row = on ? 1 : 0;
    if (!use_color)
        return;
    if (on && sel_bg[0])
        ui_esc(sel_bg);
    else
        ui_esc("\x1b[0m");
}

int ui_color(void) { return use_color; }

static int tmux_bg(const char *color)
{
    const char *pane = getenv("TMUX_PANE");
    if (!getenv("TMUX") || !pane || !*pane)
        return 0;

    char style[32];
    snprintf(style, sizeof style, "bg=%s", color ? color : "");
    char *set[] = {"tmux", "set", "-p", "-t", (char *)pane, "window-style", style, ";",
                   "set", "-p", "-t", (char *)pane, "window-active-style", style, NULL};
    char *unset[] = {"tmux", "set", "-pu", "-t", (char *)pane, "window-style", ";",
                     "set", "-pu", "-t", (char *)pane, "window-active-style", NULL};

    pid_t pid = fork();
    if (pid < 0)
        return 0;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execvp("tmux", color ? set : unset);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return 1;
}

static void term_bg_apply(void)
{
    char color[8];
    snprintf(color, sizeof color, "#%06x", (unsigned)term_bg);
    if (term_bg >= 0 && !tmux_bg(color))
        printf("\x1b]11;%s\x07", color);
    else if (term_bg < 0 && term_bg_shown && !tmux_bg(NULL))
        fputs("\x1b]111\x07", stdout);
    term_bg_shown = term_bg >= 0;
}

void ui_term_colors(void)
{
    if (!use_color)
        return;
    term_active = 1;
    printf("\x1b]12;#%06x\x07", cursor);
    term_bg_apply();
    fflush(stdout);
}

void ui_term_colors_restore(void)
{
    if (!use_color)
        return;
    term_active = 0;
    fputs("\x1b]112\x07", stdout);
    int keep = term_bg;
    term_bg = -1;
    term_bg_apply();
    term_bg = keep;
    fflush(stdout);
}

#define SINK_MAX 8

struct sink {
    FILE  *f;
    char  *buf;
    size_t len;
    int    rows;
    int    tee;
};

static struct sink sinks[SINK_MAX];
static int         sink_depth;

static struct sink *sink_top(void)
{
    if (sink_depth == 0 || sink_depth > SINK_MAX)
        return NULL;
    return &sinks[sink_depth - 1];
}

static void out(const char *s, size_t n)
{
    if (sink_depth > SINK_MAX)
        return;
    if (sink_depth > 0) {
        int lines = 0;
        for (const char *p = memchr(s, '\n', n); p;
             p = memchr(p + 1, '\n', n - (size_t)(p + 1 - s)))
            lines++;
        for (int i = sink_depth - 1; i >= 0; i--) {
            if (sinks[i].f) {
                fwrite(s, 1, n, sinks[i].f);
                sinks[i].rows += lines;
            }
            if (!sinks[i].tee)
                return;
        }
    }
    if (viewport_active()) {
        viewport_write(s, n);
        return;
    }
    fwrite(s, 1, n, stdout);
}

void ui_sink_begin(void)
{
    if (sink_depth < SINK_MAX) {
        struct sink *s = &sinks[sink_depth];
        s->tee = 0;
        s->rows = 0;
        s->len = 0;
        free(s->buf);
        s->buf = NULL;
        s->f = open_memstream(&s->buf, &s->len);
    }
    sink_depth++;
}

void ui_sink_begin_tee(void)
{
    ui_sink_begin();
    if (sink_depth <= SINK_MAX)
        sinks[sink_depth - 1].tee = 1;
}

int ui_sink_rows(void)
{
    struct sink *s = sink_top();
    return s && s->f ? s->rows : 0;
}

char *ui_sink_end(void)
{
    if (sink_depth == 0)
        return NULL;
    sink_depth--;
    if (sink_depth >= SINK_MAX)
        return NULL;

    struct sink *s = &sinks[sink_depth];
    if (s->f) {
        fclose(s->f);
        s->f = NULL;
    }
    char *taken = s->buf;
    s->buf = NULL;
    s->len = 0;
    s->rows = 0;
    s->tee = 0;
    return taken ? taken : strdup("");
}

void ui_raw(int on) { raw_newlines = on; }

#define CAPTURE_MAX 8

struct capture {
    char  *buf;
    size_t len, cap;
    int    cols;
};

static struct capture captures[CAPTURE_MAX];
static int            capture_depth;

static struct capture *capture_top(void)
{
    if (capture_depth == 0 || capture_depth > CAPTURE_MAX)
        return NULL;
    return &captures[capture_depth - 1];
}

static void emit(const char *s, size_t n)
{
    if (capture_depth == 0) {
        out(s, n);
        return;
    }
    struct capture *c = capture_top();
    if (!c)
        return;
    if (c->len + n + 1 > c->cap) {
        size_t cap = c->cap ? c->cap : 1024;
        while (cap < c->len + n + 1)
            cap *= 2;
        char *grown = realloc(c->buf, cap);
        if (!grown)
            return;
        c->buf = grown;
        c->cap = cap;
    }
    memcpy(c->buf + c->len, s, n);
    c->len += n;
    c->buf[c->len] = '\0';
}

void ui_capture_begin(int columns)
{
    if (capture_depth < CAPTURE_MAX) {
        struct capture *c = &captures[capture_depth];
        c->len = 0;
        c->cols = columns;
        if (c->buf)
            c->buf[0] = '\0';
    }
    capture_depth++;
}

char *ui_capture_end(void)
{
    if (capture_depth == 0)
        return NULL;
    capture_depth--;
    if (capture_depth >= CAPTURE_MAX)
        return NULL;

    struct capture *c = &captures[capture_depth];
    char *taken = c->len ? strdup(c->buf) : NULL;
    c->len = 0;
    c->cols = 0;
    return taken;
}

void ui_putn(const char *s, size_t n)
{
    if (capture_depth) {
        emit(s, n);
        return;
    }

    if (!sink_depth && viewport_active()) {
        viewport_write(s, n);
        return;
    }
    if (!raw_newlines) {
        out(s, n);
        return;
    }
    size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] != '\n')
            continue;
        out(s + start, i - start);
        out("\r\n", 2);
        start = i + 1;
    }
    if (start < n)
        out(s + start, n - start);
}

void ui_put(const char *s)
{
    if (s)
        ui_putn(s, strlen(s));
}

void ui_printf(const char *fmt, ...)
{
    char stack[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(stack, sizeof stack, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n < sizeof stack) {
        ui_putn(stack, (size_t)n);
        return;
    }
    char *heap = malloc((size_t)n + 1);
    if (!heap)
        return;
    va_start(ap, fmt);
    vsnprintf(heap, (size_t)n + 1, fmt, ap);
    va_end(ap);
    ui_putn(heap, (size_t)n);
    free(heap);
}

void ui_esc(const char *s)
{
    if (s)
        emit(s, strlen(s));
}

void ui_pad(int cells)
{
    static const char SPACES[] = "                                ";
    const int         chunk = (int)(sizeof SPACES - 1);

    while (cells > 0) {
        int n = cells < chunk ? cells : chunk;
        ui_putn(SPACES, (size_t)n);
        cells -= n;
    }
}

void ui_flush(void)
{
    struct sink *s = sink_top();
    if (sink_depth) {
        if (s && s->f)
            fflush(s->f);
        return;
    }
    fflush(stdout);
}

static int capture_width(void)
{
    struct capture *c = capture_top();
    return c ? c->cols : 0;
}

int ui_columns(void)
{
    int cols = capture_width();
    return cols > 0 ? cols : tty_columns();
}

int ui_screen_columns(void)
{
    int cols = capture_width();
    return cols > 0 ? cols : tty_screen_columns();
}

int ui_too_narrow(void) { return ui_screen_columns() < TTY_MIN_COLUMNS; }

static unsigned decode(const char *s, size_t n, size_t *i)
{
    unsigned char b = (unsigned char)s[*i];
    unsigned cp;
    int extra;
    if (b < 0x80)      { (*i)++; return b; }
    else if (b < 0xC0) { (*i)++; return '?'; }
    else if (b < 0xE0) { cp = b & 0x1Fu; extra = 1; }
    else if (b < 0xF0) { cp = b & 0x0Fu; extra = 2; }
    else               { cp = b & 0x07u; extra = 3; }
    (*i)++;
    for (int k = 0; k < extra && *i < n; k++, (*i)++) {
        if (((unsigned char)s[*i] & 0xC0) != 0x80)
            return '?';
        cp = (cp << 6) | ((unsigned char)s[*i] & 0x3Fu);
    }
    return cp;
}

static const struct {
    unsigned lo, hi;
} WIDE_SYMBOLS[] = {
    {0x231A, 0x231B}, {0x23E9, 0x23EC}, {0x23F0, 0x23F0}, {0x23F3, 0x23F3},
    {0x25FD, 0x25FE}, {0x2614, 0x2615}, {0x2648, 0x2653}, {0x267F, 0x267F},
    {0x2693, 0x2693}, {0x26A1, 0x26A1}, {0x26AA, 0x26AB}, {0x26BD, 0x26BE},
    {0x26C4, 0x26C5}, {0x26CE, 0x26CE}, {0x26D4, 0x26D4}, {0x26EA, 0x26EA},
    {0x26F2, 0x26F3}, {0x26F5, 0x26F5}, {0x26FA, 0x26FA}, {0x26FD, 0x26FD},
    {0x2705, 0x2705}, {0x270A, 0x270B}, {0x2728, 0x2728}, {0x274C, 0x274C},
    {0x274E, 0x274E}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797},
    {0x27B0, 0x27B0}, {0x27BF, 0x27BF},
};

static int cell_width(unsigned cp)
{
    if (cp == 0)
        return 0;
    if (cp < 0x20 || cp == 0x7f)
        return 0;

    if ((cp >= 0x1F300 && cp <= 0x1FAFF) || (cp >= 0x1F000 && cp <= 0x1F2FF))
        return 2;
    if (cp >= 0x231A && cp <= 0x27BF)
        for (int i = 0; i < COUNT(WIDE_SYMBOLS); i++)
            if (cp >= WIDE_SYMBOLS[i].lo && cp <= WIDE_SYMBOLS[i].hi)
                return 2;
    int w = wcwidth((wchar_t)cp);
    return w < 0 ? 1 : w;
}

size_t ui_cells_n(const char *s, size_t n)
{
    if (!s)
        return 0;
    size_t i = 0, cells = 0;
    while (i < n)
        cells += (size_t)cell_width(decode(s, n, &i));
    return cells;
}

size_t ui_cells(const char *s) { return s ? ui_cells_n(s, strlen(s)) : 0; }

static int opens_string(unsigned char c)
{
    return c == ']' || c == 'P' || c == '_' || c == '^' || c == 'X';
}

size_t ui_esc_span(const char *s, size_t n, size_t i, enum ui_esc_kind *kind)
{
    enum ui_esc_kind k = UI_ESC_TEXT;
    size_t j;

    if (s[i] != '\x1b') {
        j = i + 1;
        while (j < n && ((unsigned char)s[j] & 0xC0) == 0x80)
            j++;
        if (kind)
            *kind = k;
        return j;
    }

    j = i + 1;
    if (j < n && s[j] == '[') {
        for (j++; j < n && (s[j] < '@' || s[j] > '~'); j++)
            ;
        k = j < n && s[j] == 'm' ? UI_ESC_SGR : UI_ESC_OTHER;
    } else if (j < n && opens_string((unsigned char)s[j])) {
        k = s[j] == ']' && j + 1 < n && s[j + 1] == '8' ? UI_ESC_OSC8 : UI_ESC_OTHER;
        for (j++; j < n && s[j] != '\a' && s[j] != '\x1b'; j++)
            ;
        if (j < n && s[j] == '\x1b')
            j++;
    } else {
        k = UI_ESC_OTHER;
    }
    if (kind)
        *kind = k;
    return j < n ? j + 1 : n;
}

char *ui_plain(const char *in, int keep_indent)
{
    if (!in)
        return NULL;

    size_t n = strlen(in);
    char  *out = malloc(n + 1);
    if (!out)
        return NULL;

    size_t w = 0;
    for (size_t i = 0; i < n;) {
        enum ui_esc_kind kind;
        size_t           end = ui_esc_span(in, n, i, &kind);
        if (kind == UI_ESC_TEXT)
            for (size_t k = i; k < end; k++) {
                unsigned char ch = (unsigned char)in[k];
                if (ch == '\r')
                    continue;
                if (ch == '\t')
                    ch = ' ';
                if (ch != '\n' && ch < ' ')
                    continue;
                if (ch == 0x7f)
                    continue;
                if (!keep_indent && ch == ' ' && w && out[w - 1] == '\n')
                    continue;
                out[w++] = (char)ch;
            }
        i = end;
    }
    while (w && (out[w - 1] == '\n' || out[w - 1] == ' '))
        w--;
    out[w] = '\0';
    return out;
}

static size_t step_visible(const char *s, size_t n, size_t i, size_t *cells)
{
    enum ui_esc_kind kind;
    size_t end = ui_esc_span(s, n, i, &kind);
    if (kind == UI_ESC_TEXT)
        *cells += ui_cells_n(s + i, end - i);
    return end;
}

size_t ui_cells_visible(const char *s, size_t n)
{
    struct ui_cellstream st = {0};
    return ui_cells_stream(&st, s, n);
}

enum { CS_TEXT, CS_ESC, CS_CSI, CS_STR, CS_STR_ESC };

static size_t utf8_len(unsigned char c)
{
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

size_t ui_cells_stream(struct ui_cellstream *st, const char *s, size_t n)
{
    if (!st || !s)
        return 0;

    size_t cells = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        switch (st->state) {
        case CS_ESC:
            if (c == '[')
                st->state = CS_CSI;
            else if (opens_string(c))
                st->state = CS_STR;
            else
                st->state = CS_TEXT;
            continue;

        case CS_CSI:
            if (c >= '@' && c <= '~')
                st->state = CS_TEXT;
            continue;

        case CS_STR:
            if (c == 0x07)
                st->state = CS_TEXT;
            else if (c == 0x1b)
                st->state = CS_STR_ESC;
            continue;

        case CS_STR_ESC:
            st->state = c == '\\' ? CS_TEXT : CS_STR;
            continue;

        default:
            break;
        }

        if (c == 0x1b) {
            st->pending_n = 0;
            st->state = CS_ESC;
            continue;
        }

        if (st->pending_n == 0 && c < 0x80) {
            cells += (size_t)cell_width(c);
            continue;
        }
        if (st->pending_n < sizeof st->pending)
            st->pending[st->pending_n++] = c;
        size_t want = utf8_len(st->pending[0]);
        if (st->pending_n >= want) {
            size_t at = 0;
            cells += (size_t)cell_width(decode((const char *)st->pending, st->pending_n, &at));
            st->pending_n = 0;
        }
    }
    return cells;
}

size_t ui_fit_visible(const char *s, size_t n, size_t budget)
{
    if (!s)
        return 0;
    size_t cells = 0, i = 0, fit = 0;
    while (i < n) {
        size_t next = step_visible(s, n, i, &cells);
        if (cells > budget)
            break;
        i = fit = next;
    }
    return fit;
}

size_t ui_fit_bytes(const char *s, size_t budget)
{
    return s ? ui_fit_visible(s, strlen(s), budget) : 0;
}

void ui_put_spans(const char *s, size_t n, const unsigned char *roles, enum ui_role base)
{
    for (size_t i = 0; i < n;) {
        enum ui_role role = (enum ui_role)roles[i];
        size_t       j = i;
        while (j < n && (enum ui_role)roles[j] == role)
            j++;
        ui_esc(ui_style(role == UI_RESET ? base : role));
        ui_putn(s + i, j - i);
        i = j;
    }
}

size_t ui_wrap_row(const char *s, size_t n, size_t budget, size_t *skip, size_t *cells_out)
{
    if (skip)
        *skip = 0;
    if (cells_out)
        *cells_out = 0;
    if (!s || !n)
        return 0;
    if (budget < 1)
        budget = 1;

    size_t i = 0, cells = 0, last_space = 0, cells_at_space = 0;
    while (i < n) {
        if (s[i] == '\n') {
            if (skip)
                *skip = 1;
            if (cells_out)
                *cells_out = cells;
            return i;
        }
        size_t start = i;
        size_t w = (size_t)cell_width(decode(s, n, &i));
        if (cells + w > budget) {
            if (last_space > 0) {
                if (skip)
                    *skip = 1;
                if (cells_out)
                    *cells_out = cells_at_space;
                return last_space;
            }
            if (cells_out)
                *cells_out = start ? cells : w;
            return start ? start : i;
        }
        cells += w;
        if (s[start] == ' ') {
            last_space = start;
            cells_at_space = cells - w;
        }
    }
    if (cells_out)
        *cells_out = cells;
    return n;
}

int ui_wrap_paint(const char *text, const struct ui_wrap *w)
{
    const char *p = text ? text : "";
    size_t n = text ? strlen(text) : 0;
    size_t budget = w->budget ? w->budget : 1;
    int mark_cells = w->mark ? (int)ui_cells(w->mark) : 0;
    int wide_gutter = w->gutter ? (int)ui_cells(w->gutter) : 0;
    int rows = 0;
    int reflowed = 0;
    int first = 1;

    while (n || (first && w->paint_empty)) {
        size_t skip = 0, row_cells = 0;
        size_t row = n ? ui_wrap_row(p, n, budget, &skip, &row_cells) : 0;
        int indent = first ? w->first_indent : w->indent;
        const char *gutter = w->gutter;
        int gutter_cells = wide_gutter;
        if (w->gutters && rows < w->gutters_n && w->gutters[rows]) {
            gutter = w->gutters[rows];
            gutter_cells = (int)ui_cells(gutter);
        }
        int width = gutter_cells + (first ? mark_cells : 0) + (int)row_cells;

        if (!w->measure) {
            ui_pad(indent);
            if (w->role != UI_RESET)
                ui_esc(ui_style(w->role));
            if (w->erase)
                ui_esc(UI_ERASE_EOL);
            if (gutter)
                ui_put(gutter);
            if (w->mark && first)
                ui_put(w->mark);
            if (w->spans)
                ui_put_spans(p, row, w->spans + (size_t)(p - text), w->role);
            else
                ui_putn(p, row);
        }

        size_t used = row + skip;
        p += used;
        n -= used < n ? used : n;
        rows++;
        if (n && w->max_rows && rows == w->max_rows) {
            if (!w->measure)
                ui_put("…");
            width++;
        }
        if (!w->measure) {
            if (w->role != UI_RESET || w->spans)
                ui_esc(ui_style(UI_RESET));
            ui_put("\n");
        }
        if (w->widths && rows - 1 < w->widths_max)
            w->widths[rows - 1] = width + indent;
        if (w->reflow_cols > 0) {
            int cells = width + indent;
            reflowed += cells > 0 ? (cells + w->reflow_cols - 1) / w->reflow_cols : 1;
        }

        first = 0;
        if (w->max_rows && rows >= w->max_rows)
            break;
    }
    return w->reflow_cols > 0 ? reflowed : rows;
}

void ui_wrapped(const char *text, int indent, enum ui_role role)
{
    int columns = ui_columns();
    struct ui_wrap w = {0};
    w.budget = (size_t)(columns - indent > 1 ? columns - indent : 1);
    w.first_indent = w.indent = indent;
    w.role = role;
    w.paint_empty = 1;
    ui_wrap_paint(text, &w);
}

void ui_bar(const char *style, const char *fmt, ...)
{
    ui_esc(ui_style(UI_BRAND));
    ui_put(UI_BAR);
    ui_esc(ui_style(UI_RESET));
    ui_put(" ");
    if (style && *style)
        ui_esc(style);

    char stack[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(stack, sizeof stack, fmt, ap);
    va_end(ap);
    ui_put(stack);

    if (style && *style)
        ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void ui_line(enum ui_role role, const char *fmt, va_list ap)
{
    char stack[1024];
    vsnprintf(stack, sizeof stack, fmt, ap);
    ui_esc(ui_style(role));
    ui_put(stack);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
    ui_flush();
}

void ui_note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ui_line(UI_DIM, fmt, ap);
    va_end(ap);
}

void ui_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    ui_line(UI_ERROR, fmt, ap);
    va_end(ap);
}

int ui_diverted(void)
{
    return capture_depth || sink_depth;
}
