#include "md.h"

#include "highlight.h"
#include "vendor/mermaid/mermaid.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "image.h"
#include "scrollback.h"
#include "ui.h"
#include "viewport.h"
#include "vendor/cJSON.h"

#define LINK_MAX 64
#define COMMAND_SCHEME "scrap-cmd:"
#define COMMAND_MAX 2048

struct styled {
    char        *text;
    signed char *style;
    signed char *link;
    size_t       len, cap;
    char        *url[LINK_MAX];
    int          nurls;
    int          cur_link;
};

static void styled_init(struct styled *s)
{
    s->cap = 128;
    s->len = 0;
    s->text = malloc(s->cap);
    s->style = calloc(s->cap, 1);
    s->link = calloc(s->cap, 1);
    s->nurls = 0;
    s->cur_link = 0;
}

static void styled_free(struct styled *s)
{
    free(s->text);
    free(s->style);
    free(s->link);
    for (int i = 0; i < s->nurls; i++)
        free(s->url[i]);
    s->nurls = 0;
}

static int url_ok(const char *p, size_t n)
{
    size_t i = 0;
    if (!n || !isalpha((unsigned char)p[0]))
        return 0;
    while (i < n && (isalnum((unsigned char)p[i]) || p[i] == '+' || p[i] == '.' || p[i] == '-'))
        i++;
    if (i == 0 || i >= n || p[i] != ':')
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x21 || c == 0x7f)
            return 0;
    }
    return 1;
}

static int styled_link(struct styled *s, const char *url, size_t n)
{
    if (s->nurls >= LINK_MAX || !url_ok(url, n))
        return 0;
    for (int i = 0; i < s->nurls; i++)
        if (strlen(s->url[i]) == n && memcmp(s->url[i], url, n) == 0)
            return i + 1;
    char *copy = malloc(n + 1);
    if (!copy)
        return 0;
    memcpy(copy, url, n);
    copy[n] = '\0';
    s->url[s->nurls++] = copy;
    return s->nurls;
}

static void styled_push(struct styled *s, const char *bytes, size_t n, int role)
{
    if (!s->text || !s->style || !s->link)
        return;
    if (s->len + n + 1 > s->cap) {
        size_t want = (s->len + n + 1) * 2;
        char *t = realloc(s->text, want);
        signed char *y = realloc(s->style, want);
        signed char *l = realloc(s->link, want);
        if (!t || !y || !l) {
            free(t ? t : s->text);
            free(y ? y : s->style);
            free(l ? l : s->link);
            s->text = NULL;
            s->style = NULL;
            s->link = NULL;
            s->len = 0;
            return;
        }
        s->text = t;
        s->style = y;
        s->link = l;
        s->cap = want;
    }

    size_t out = s->len;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)bytes[i];
        if (c == 0x7f || (c < 0x20 && c != '\t' && c != '\n'))
            continue;
        s->text[out] = (char)c;
        s->style[out] = (signed char)role;
        s->link[out] = (signed char)s->cur_link;
        out++;
    }
    s->len = out;
    s->text[s->len] = '\0';
}

static void put_safe_n(const char *s, size_t n)
{
    const char *run = s, *end = s + n;
    for (const char *p = s;; p++) {
        unsigned char c = p < end ? (unsigned char)*p : 0;
        if (p < end && !(c == 0x7f || (c < 0x20 && c != '\t')))
            continue;
        if (p > run)
            ui_putn(run, (size_t)(p - run));
        if (p >= end)
            return;
        run = p + 1;
    }
}

static void put_safe(const char *s)
{
    put_safe_n(s, strlen(s));
}

static char *command_url(const char *cmd, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t k = sizeof COMMAND_SCHEME - 1;
    if (!n || n > COMMAND_MAX)
        return NULL;
    char *url = malloc(k + n * 3 + 1);
    if (!url)
        return NULL;
    memcpy(url, COMMAND_SCHEME, k);
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)cmd[i];
        if (isalnum(c) || (c && strchr("-._~/", c))) {
            url[k++] = (char)c;
        } else {
            url[k++] = '%';
            url[k++] = hex[c >> 4];
            url[k++] = hex[c & 15];
        }
    }
    url[k] = '\0';
    return url;
}

char *md_command(const char *url)
{
    size_t k = sizeof COMMAND_SCHEME - 1;
    if (!url || strncmp(url, COMMAND_SCHEME, k) != 0)
        return NULL;
    url += k;
    char *out = malloc(strlen(url) + 2);
    if (!out)
        return NULL;
    size_t n = 0;
    out[n++] = '!';
    for (; *url; url++) {
        if (url[0] == '%' && isxdigit((unsigned char)url[1]) && isxdigit((unsigned char)url[2])) {
            char hex[3] = {url[1], url[2], '\0'};
            out[n++] = (char)strtol(hex, NULL, 16);
            url += 2;
        } else {
            out[n++] = *url;
        }
    }
    out[n] = '\0';
    return out;
}

char *md_command_at(int row, int col)
{
    char *url = viewport_link_at(row, col);
    char *cmd = md_command(url);
    free(url);
    return cmd;
}

static int on_path(const char *word, size_t n)
{
    if (memchr(word, '/', n))
        return 1;
    const char *path = getenv("PATH");
    char        file[1024];
    struct stat st;
    for (const char *d = path; d && *d;) {
        size_t len = strcspn(d, ":");
        int    w = snprintf(file, sizeof file, "%.*s/%.*s", (int)len, d, (int)n, word);
        if (w > 0 && (size_t)w < sizeof file && stat(file, &st) == 0 && S_ISREG(st.st_mode) &&
            (st.st_mode & 0111))
            return 1;
        d += len;
        if (*d)
            d++;
    }
    return 0;
}

static int span_is_command(const char *s, size_t n)
{
    const char *space = memchr(s, ' ', n);
    return space && space > s && on_path(s, (size_t)(space - s));
}

static int is_url_start(const char *p)
{
    return strncmp(p, "http://", 7) == 0 || strncmp(p, "https://", 8) == 0;
}

struct nomatch {
    const char *from, *to;
};

enum { NM_CODE, NM_STRONG, NM_EM, NM_BRACKET, NM_PAREN, NM_COUNT };

static const char *find_close(const char *p, const char *delim, size_t dlen,
                              struct nomatch *seen)
{
    if (seen->from && p >= seen->from && p <= seen->to)
        return NULL;

    const char *q = p;
    for (; *q && *q != '\n'; q++)
        if (strncmp(q, delim, dlen) == 0)
            return q > p ? q : NULL;
    seen->from = p;
    seen->to = q;
    return NULL;
}

static void inline_scan(const char *p, struct styled *out)
{
    struct nomatch seen[NM_COUNT] = {{0}};
    while (*p) {
        if (*p == '`') {
            const char *end = find_close(p + 1, "`", 1, &seen[NM_CODE]);
            if (end) {
                size_t n = (size_t)(end - p - 1);
                char  *url = span_is_command(p + 1, n) ? command_url(p + 1, n) : NULL;
                if (url)
                    out->cur_link = styled_link(out, url, strlen(url));
                free(url);
                styled_push(out, p + 1, n, UI_CODE + 1);
                out->cur_link = 0;
                p = end + 1;
                continue;
            }
        }
        if (p[0] == '*' && p[1] == '*') {
            const char *end = find_close(p + 2, "**", 2, &seen[NM_STRONG]);
            if (end) {
                styled_push(out, p + 2, (size_t)(end - p - 2), UI_BOLD + 1);
                p = end + 2;
                continue;
            }
        }
        if (p[0] == '*' && p[1] != ' ') {
            const char *end = find_close(p + 1, "*", 1, &seen[NM_EM]);
            if (end) {
                styled_push(out, p + 1, (size_t)(end - p - 1), UI_ITALIC + 1);
                p = end + 1;
                continue;
            }
        }

        if (*p == '[') {
            const char *close = find_close(p + 1, "]", 1, &seen[NM_BRACKET]);
            if (close && close[1] == '(') {
                const char *paren = find_close(close + 2, ")", 1, &seen[NM_PAREN]);
                if (paren) {
                    out->cur_link = styled_link(out, close + 2, (size_t)(paren - close - 2));
                    styled_push(out, p + 1, (size_t)(close - p - 1), UI_LINK + 1);
                    out->cur_link = 0;
                    p = paren + 1;
                    continue;
                }
            }
        }
        if (is_url_start(p)) {
            const char *end = p;
            while (*end && !isspace((unsigned char)*end) && *end != ')' && *end != '>')
                end++;
            out->cur_link = styled_link(out, p, (size_t)(end - p));
            styled_push(out, p, (size_t)(end - p), UI_LINK + 1);
            out->cur_link = 0;
            p = end;
            continue;
        }
        styled_push(out, p, 1, 0);
        p++;
    }
}

static void link_open(const struct styled *s, int id)
{
    char buf[1024];
    if (!ui_color())
        return;
    int n = snprintf(buf, sizeof buf, "\x1b]8;id=scrap%d;%s\x1b\\", id, s->url[id - 1]);
    if (n > 0 && (size_t)n < sizeof buf)
        ui_esc(buf);
}

static void link_close(void)
{
    if (ui_color())
        ui_esc("\x1b]8;;\x1b\\");
}

static void emit_slice(const struct styled *s, size_t from, size_t len, enum ui_role base)
{
    int open = -1;
    int link = 0;
    for (size_t i = 0; i < len; ) {
        signed char style = s->style[from + i];
        signed char id = s->link ? s->link[from + i] : 0;
        if (style != open) {
            ui_esc(ui_style(UI_RESET));
            ui_esc(ui_style(style ? (enum ui_role)(style - 1) : base));
            open = style;
        }
        if (id != link) {
            if (link)
                link_close();
            if (id)
                link_open(s, id);
            link = id;
        }
        size_t run = 1;
        while (i + run < len && s->style[from + i + run] == style &&
               (s->link ? s->link[from + i + run] : 0) == id)
            run++;
        ui_putn(s->text + from + i, run);
        i += run;
    }
    if (link)
        link_close();
    if (open >= 0)
        ui_esc(ui_style(UI_RESET));
}

static void emit_styled(const struct styled *s, int spent, int first_indent, int indent)
{
    if (!s->text)
        return;
    int columns = ui_columns();
    const char *p = s->text;
    int row_indent = first_indent;

    while (*p || p == s->text) {
        int budget = columns - row_indent - spent;
        if (budget < 8)
            budget = 8;
        size_t skip = 0;
        size_t left = s->len - (size_t)(p - s->text);
        size_t row = *p ? ui_wrap_row(p, left, (size_t)budget, &skip, NULL) : 0;

        ui_pad(row_indent);
        size_t base = (size_t)(p - s->text);

        emit_slice(s, base, row, UI_BODY);
        ui_put("\n");

        p += row + skip;
        row_indent = indent;
        spent = 0;
        if (!*p)
            break;
    }
}

static char *take_line(const char **text);

#define TABLE_MAX_COLS 12
#define TABLE_MIN_COL  6

enum align { ALIGN_LEFT, ALIGN_RIGHT, ALIGN_CENTER };

struct trow {
    struct styled cell[TABLE_MAX_COLS];
    int           ncells;
};

static int split_row(const char *line, char **out, int max)
{
    const char *p = line;
    while (*p == ' ')
        p++;
    if (*p == '|')
        p++;

    char buf[2048];
    size_t b = 0;
    int n = 0;

    for (;; p++) {
        if (*p == '\\' && p[1] == '|') {
            if (b + 1 < sizeof buf)
                buf[b++] = '|';
            p++;
            continue;
        }
        if (*p != '|' && *p != '\0') {
            if (b + 1 < sizeof buf)
                buf[b++] = *p;
            continue;
        }

        size_t start = 0, end = b;
        while (start < end && buf[start] == ' ')
            start++;
        while (end > start && buf[end - 1] == ' ')
            end--;
        if (n < max) {
            char *cell = malloc(end - start + 1);
            if (!cell)
                return n;
            memcpy(cell, buf + start, end - start);
            cell[end - start] = '\0';
            out[n++] = cell;
        }
        b = 0;

        if (*p == '\0')
            break;

        const char *q = p + 1;
        while (*q == ' ')
            q++;
        if (*q == '\0')
            break;
    }
    return n;
}

static int is_delim_cell(const char *s, enum align *out)
{
    const char *p = s;
    int left = 0, right = 0, dashes = 0;
    if (*p == ':') {
        left = 1;
        p++;
    }
    while (*p == '-') {
        dashes++;
        p++;
    }
    if (*p == ':') {
        right = 1;
        p++;
    }
    if (*p || dashes == 0)
        return 0;
    *out = left && right ? ALIGN_CENTER : right ? ALIGN_RIGHT : ALIGN_LEFT;
    return 1;
}

static int is_delim_row(const char *line, enum align *align, int max)
{
    if (!strchr(line, '-') || !strchr(line, '|'))
        return 0;

    char *cells[TABLE_MAX_COLS];
    int n = split_row(line, cells, max);
    int ok = n > 0;
    for (int i = 0; i < n; i++) {
        enum align a = ALIGN_LEFT;
        if (!is_delim_cell(cells[i], &a))
            ok = 0;
        else if (align)
            align[i] = a;
        free(cells[i]);
    }
    return ok;
}

static int peek_is_delim(const char *text, enum align *align, int max)
{
    const char *nl = strchr(text, '\n');
    size_t n = nl ? (size_t)(nl - text) : strlen(text);
    if (n >= 512)
        return 0;
    char line[512];
    memcpy(line, text, n);
    line[n] = '\0';
    return is_delim_row(line, align, max);
}

static void row_init(struct trow *r, const char *line, int ncols)
{
    char *cells[TABLE_MAX_COLS];
    int n = split_row(line, cells, ncols);
    for (int i = 0; i < ncols; i++) {
        styled_init(&r->cell[i]);
        if (i < n) {
            inline_scan(cells[i], &r->cell[i]);
            free(cells[i]);
        }
    }

    for (int i = ncols; i < n; i++)
        free(cells[i]);
    r->ncells = ncols;
}

static void row_free(struct trow *r)
{
    for (int i = 0; i < r->ncells; i++)
        styled_free(&r->cell[i]);
}

static void rule_row(const int *width, int ncols, int indent)
{
    ui_pad(indent);
    ui_esc(ui_style(UI_CHROME));
    for (int c = 0; c < ncols; c++) {
        if (c)
            ui_put("\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80");
        for (int i = 0; i < width[c]; i++)
            ui_put("\xe2\x94\x80");
    }
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void render_row(const struct trow *r, const int *width, const enum align *align, int ncols,
                       int indent, int header)
{
    size_t off[TABLE_MAX_COLS] = {0};
    int more = 1;

    while (more) {
        more = 0;
        ui_pad(indent);
        for (int c = 0; c < ncols; c++) {
            const struct styled *s = &r->cell[c];
            size_t take = 0, skip = 0;
            if (s->text && off[c] < s->len) {
                take = ui_wrap_row(s->text + off[c], s->len - off[c], (size_t)width[c],
                                   &skip, NULL);
                if (take == 0 && skip == 0)
                    take = s->len - off[c];
            }

            int used = take ? (int)ui_cells_n(s->text + off[c], take) : 0;
            int slack = width[c] - used;
            if (slack < 0)
                slack = 0;
            int before = align[c] == ALIGN_RIGHT    ? slack
                         : align[c] == ALIGN_CENTER ? slack / 2
                                                    : 0;

            int last = (c == ncols - 1);
            int blank = (take == 0);

            if (c) {
                ui_esc(ui_style(UI_CHROME));
                ui_put(" \xe2\x94\x82");
                ui_esc(ui_style(UI_RESET));
                if (!(last && blank))
                    ui_put(" ");
            }
            if (!(last && blank)) {
                ui_pad(before);
                if (take)
                    emit_slice(s, off[c], take, header ? UI_BOLD : UI_BODY);
                if (!last)
                    ui_pad(slack - before);
            }

            off[c] += take + skip;
            if (s->text && off[c] < s->len)
                more = 1;
        }
        ui_put("\n");
    }
}

static void fit_widths(int *width, int ncols, int indent)
{
    int avail = ui_columns() - indent - 3 * (ncols - 1);
    if (avail < ncols * TABLE_MIN_COL)
        avail = ncols * TABLE_MIN_COL;

    int total = 0;
    for (int c = 0; c < ncols; c++)
        total += width[c];

    while (total > avail) {
        int widest = 0;
        for (int c = 1; c < ncols; c++)
            if (width[c] > width[widest])
                widest = c;
        if (width[widest] <= TABLE_MIN_COL)
            break;
        width[widest]--;
        total--;
    }
}

static void render_table(const char *first, const char **text, int indent)
{
    enum align align[TABLE_MAX_COLS];
    for (int i = 0; i < TABLE_MAX_COLS; i++)
        align[i] = ALIGN_LEFT;

    char *head[TABLE_MAX_COLS];
    int ncols = split_row(first, head, TABLE_MAX_COLS);
    for (int i = 0; i < ncols; i++)
        free(head[i]);
    if (ncols == 0)
        return;

    char *delim = take_line(text);
    if (!delim)
        return;
    is_delim_row(delim, align, ncols);
    free(delim);

    struct trow *rows = NULL;
    int count = 0, cap = 0;

    struct trow header;
    row_init(&header, first, ncols);

    while (**text) {
        const char *nl = strchr(*text, '\n');
        size_t n = nl ? (size_t)(nl - *text) : strlen(*text);
        if (n == 0 || !memchr(*text, '|', n))
            break;

        char *line = take_line(text);
        if (!line)
            break;
        if (count == cap) {
            int want = cap ? cap * 2 : 8;
            struct trow *grown = realloc(rows, (size_t)want * sizeof *grown);
            if (!grown) {
                free(line);
                break;
            }
            rows = grown;
            cap = want;
        }
        row_init(&rows[count++], line, ncols);
        free(line);
    }

    int width[TABLE_MAX_COLS];
    for (int c = 0; c < ncols; c++) {
        width[c] = (int)ui_cells_n(header.cell[c].text, header.cell[c].len);
        for (int r = 0; r < count; r++) {
            int w = (int)ui_cells_n(rows[r].cell[c].text, rows[r].cell[c].len);
            if (w > width[c])
                width[c] = w;
        }
        if (width[c] < 1)
            width[c] = 1;
    }
    fit_widths(width, ncols, indent);

    render_row(&header, width, align, ncols, indent, 1);
    rule_row(width, ncols, indent);
    for (int r = 0; r < count; r++)
        render_row(&rows[r], width, align, ncols, indent, 0);

    row_free(&header);
    for (int r = 0; r < count; r++)
        row_free(&rows[r]);
    free(rows);
}

static void render_paragraph(const char *text, int spent, int first_indent, int indent)
{
    struct styled s;
    styled_init(&s);
    inline_scan(text, &s);
    emit_styled(&s, spent, first_indent, indent);
    styled_free(&s);
}

static int leading_indent(const char *line, const char **body)
{
    int n = 0;
    while (*line == ' ' || *line == '\t') {
        n += (*line == '\t') ? 4 : 1;
        line++;
    }
    *body = line;
    return n;
}

static int is_bullet(const char *p, const char **rest)
{
    if ((p[0] == '-' || p[0] == '*' || p[0] == '+') && p[1] == ' ') {
        *rest = p + 2;
        return 1;
    }
    return 0;
}

static int is_ordered(const char *p, size_t *marker_len)
{
    const char *q = p;
    while (isdigit((unsigned char)*q))
        q++;
    if (q == p || (*q != '.' && *q != ')') || q[1] != ' ')
        return 0;
    *marker_len = (size_t)(q - p) + 2;
    return 1;
}

static int is_rule(const char *p)
{
    if (*p != '-' && *p != '*' && *p != '_')
        return 0;
    char c = *p;
    int n = 0;
    while (*p == c || *p == ' ') {
        if (*p == c)
            n++;
        p++;
    }
    return *p == '\0' && n >= 3;
}

static char *take_line(const char **text)
{
    const char *start = *text;
    const char *nl = strchr(start, '\n');
    size_t n = nl ? (size_t)(nl - start) : strlen(start);
    char *line = malloc(n + 1);
    if (!line)
        return NULL;
    memcpy(line, start, n);
    line[n] = '\0';

    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' '))
        line[--n] = '\0';
    *text = nl ? nl + 1 : start + strlen(start);
    return line;
}

static void render_ansi_line(const char *line, int indent)
{
    size_t n = strlen(line);
    char  *out = malloc(n + 1);
    if (!out)
        return;

    size_t at = 0;
    for (const char *p = line; *p; ) {
        if (p[0] != '\\') {
            out[at++] = *p++;
            continue;
        }
        if (p[1] == 'e') {
            out[at++] = '\x1b';
            p += 2;
        } else if (p[1] == 'x' && (p[2] == '1') && (p[3] == 'b' || p[3] == 'B')) {
            out[at++] = '\x1b';
            p += 4;
        } else if (p[1] == '0' && p[2] == '3' && p[3] == '3') {
            out[at++] = '\x1b';
            p += 4;
        } else if (p[1] == '\\') {
            out[at++] = '\\';
            p += 2;
        } else {
            out[at++] = *p++;
        }
    }

    ui_pad(indent);
    ui_putn(out, at);
    free(out);

    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static char *image_line(const char *body)
{
    if (body[0] != '!' || body[1] != '[')
        return NULL;
    const char *close = strchr(body + 2, ']');
    if (!close || close[1] != '(')
        return NULL;
    const char *open = close + 2;
    const char *end = strchr(open, ')');
    if (!end || end[1] != '\0' || end == open || is_url_start(open))
        return NULL;

    size_t n = (size_t)(end - open);
    char *path = malloc(n + 1);
    if (!path)
        return NULL;
    memcpy(path, open, n);
    path[n] = '\0';
    return path;
}

static void render_image(const char *path, int indent)
{
    if (image_show(path, indent))
        return;
    ui_pad(indent);
    ui_esc(ui_style(UI_DIM));
    ui_put("[image] ");
    put_safe(path);
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static char *view_line(const char *body)
{
    if (strncmp(body, "@view /", 7) != 0)
        return NULL;
    const char *path = body + 6;
    size_t      n = strlen(path);
    while (n && isspace((unsigned char)path[n - 1]))
        n--;
    return strndup(path, n);
}

static void render_view(const char *path, int indent)
{
    const char *slash = strrchr(path, '/');
    ui_pad(indent);
    if (ui_color()) {
        ui_esc("\x1b]8;;file://");
        put_safe(path);
        ui_esc("\x1b\\");
    }
    ui_esc(ui_style(UI_LINK));
    ui_put("\xe2\x96\xa4 ");
    put_safe(slash && slash[1] ? slash + 1 : path);
    ui_esc(ui_style(UI_RESET));
    if (ui_color())
        ui_esc("\x1b]8;;\x1b\\");
    ui_esc(ui_style(UI_DIM));
    ui_put("  click to view");
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static int handoff_line(const char *body)
{
    return !strncmp(body, "@handoff /", 10);
}

static void render_handoff(int indent)
{
    ui_pad(indent);
    ui_esc(ui_style(UI_DIM));
    ui_put("handoff ready");
    ui_esc(ui_style(UI_RESET));
    ui_put("\n");
}

static void render_code_line(const char *line, int indent, const char *url,
                             const unsigned char *roles)
{
    int link = url && ui_color();
    ui_pad(indent);
    if (link) {
        ui_esc("\x1b]8;;");
        ui_esc(url);
        ui_esc("\x1b\\");
    }
    size_t n = strlen(line);
    for (size_t i = 0, j; i < n; i = j) {
        unsigned char role = roles ? roles[i] : UI_RESET;
        for (j = i + 1; j < n && roles && roles[j] == role; j++)
            ;
        ui_esc(ui_style(role == UI_RESET ? UI_CODE : (enum ui_role)role));
        put_safe_n(line + i, j - i);
    }
    ui_esc(ui_style(UI_RESET));
    if (link)
        ui_esc("\x1b]8;;\x1b\\");
    ui_put("\n");
}

static int is_fence(const char *body)
{
    return strncmp(body, "```", 3) == 0 || strncmp(body, "~~~", 3) == 0;
}

static int shell_fence(const char *info)
{
    static const char *const lang[] = {"", "sh", "bash", "shell", "zsh"};
    while (*info == ' ')
        info++;
    size_t n = strcspn(info, " \t{");
    for (size_t i = 0; i < sizeof lang / sizeof *lang; i++)
        if (strlen(lang[i]) == n && strncmp(info, lang[i], n) == 0)
            return 1;
    return 0;
}

static char *fence_block(const char *text, int lead)
{
    char  *block = NULL;
    size_t len = 0;
    while (*text) {
        char *line = take_line(&text);
        if (!line)
            break;
        const char *body;
        leading_indent(line, &body);
        if (is_fence(body)) {
            free(line);
            break;
        }
        const char *from = line;
        for (int i = 0; i < lead && (*from == ' ' || *from == '\t'); i++)
            from++;
        size_t n = strlen(from);
        char  *grown = realloc(block, len + n + 2);
        if (!grown) {
            free(line);
            break;
        }
        block = grown;
        if (len)
            block[len++] = '\n';
        memcpy(block + len, from, n);
        len += n;
        block[len] = '\0';
        free(line);
    }
    while (len && block[len - 1] == '\n')
        block[--len] = '\0';
    return block;
}

static char *fence_url(const char *text, int lead)
{
    char *block = fence_block(text, lead);
    char *url = block ? command_url(block, strlen(block)) : NULL;
    free(block);
    return url;
}

static unsigned char *fence_roles(const char *info, const char *text, size_t *len)
{
    char          *block = fence_block(text, 0);
    size_t         n = block ? strlen(block) : 0;
    unsigned char *roles = n ? malloc(n) : NULL;
    if (roles && !highlight_code(info, block, n, roles)) {
        free(roles);
        roles = NULL;
    }
    free(block);
    *len = n;
    return roles;
}

static char *command_line(const char *cmd, size_t n)
{
    if (!n || n > COMMAND_MAX)
        return NULL;
    char *out = malloc(n + 2);
    if (out) {
        out[0] = '!';
        memcpy(out + 1, cmd, n);
        out[n + 1] = '\0';
    }
    return out;
}

char *md_command_nth(const char *text, int nth)
{
    char **found = NULL;
    int    n = 0;
    int    in_code = 0;
    while (text && *text) {
        char *line = take_line(&text);
        if (!line)
            break;
        const char *body;
        int         lead = leading_indent(line, &body);
        char       *cmd = NULL;
        int         fence = is_fence(body);
        if (fence) {
            if (!in_code && shell_fence(body + 3)) {
                char *block = fence_block(text, lead);
                cmd = block ? command_line(block, strlen(block)) : NULL;
                free(block);
            }
            in_code = !in_code;
        }
        for (const char *p = in_code || fence ? NULL : strchr(line, '`'), *end; p;
             p = strchr(end + 1, '`')) {
            end = strchr(p + 1, '`');
            if (!end)
                break;
            size_t len = (size_t)(end - p - 1);
            char  *span = span_is_command(p + 1, len) ? command_line(p + 1, len) : NULL;
            char **grown = span ? realloc(found, (size_t)(n + 1) * sizeof *found) : NULL;
            if (grown) {
                found = grown;
                found[n++] = span;
            } else {
                free(span);
            }
        }
        char **grown = cmd ? realloc(found, (size_t)(n + 1) * sizeof *found) : NULL;
        if (grown) {
            found = grown;
            found[n++] = cmd;
        } else {
            free(cmd);
        }
        free(line);
    }
    char *out = nth >= 0 && nth < n ? found[n - 1 - nth] : NULL;
    for (int i = 0; i < n; i++)
        if (found[i] != out)
            free(found[i]);
    free(found);
    return out;
}

static void render_mermaid(const char *src, int indent)
{
    int width = ui_columns() - indent;
    MermaidArt *art = mermaid_render(src, 0);
    if (art && art->n && strstr(art->lines[0], " mermaid: ")) {
        mermaid_art_free(art);
        art = mermaid_render(src, width > 20 ? width : 20);
    }
    if (!art) {
        for (const char *p = src; *p;) {
            const char *nl = strchr(p, '\n');
            char *line = strndup(p, nl ? (size_t)(nl - p) : strlen(p));
            render_code_line(line, indent, NULL, NULL);
            free(line);
            if (!nl)
                break;
            p = nl + 1;
        }
        return;
    }
    size_t first = 0, last = art->n;
    while (first < last && !*art->lines[first])
        first++;
    while (last > first && !*art->lines[last - 1])
        last--;
    for (size_t i = first; i < last; i++) {
        ui_pad(indent);
        char *row = art->lines[i];
        row[ui_fit_bytes(row, width > 0 ? (size_t)width : 0)] = '\0';
        put_safe(row);
        ui_put("\n");
    }
    mermaid_art_free(art);
}

struct kept {
    char  *text;
    int    indent;
    size_t hide_from, hide_to;
    int    hide_kept;
};

/* The text as shown: without its hidden span, if it has one. */
static char *kept_shown(const struct kept *k)
{
    size_t n = strlen(k->text);
    if (k->hide_to <= k->hide_from || k->hide_to > n)
        return strdup(k->text);
    char *s = malloc(n - (k->hide_to - k->hide_from) + 1);
    if (!s)
        return NULL;
    memcpy(s, k->text, k->hide_from);
    strcpy(s + k->hide_from, k->text + k->hide_to);
    return s;
}

static void kept_render(void *ud, int cols)
{
    (void)cols;
    const struct kept *k = ud;
    char *shown = kept_shown(k);
    md_render(shown ? shown : k->text, k->indent);
    free(shown);
}

static void kept_free(void *ud)
{
    struct kept *k = ud;
    free(k->text);
    free(k);
}

static char *kept_encode(void *ud)
{
    const struct kept *k = ud;
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    char *shown = k->hide_kept ? kept_shown(k) : NULL;
    cJSON_AddStringToObject(o, "text", shown ? shown : k->text ? k->text : "");
    free(shown);
    cJSON_AddNumberToObject(o, "indent", k->indent);
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return out;
}

const char *md_kept_text(unsigned mark)
{
    const struct kept *k = viewport_item_data(mark);
    return k ? k->text : NULL;
}

void md_kept_hide(unsigned mark, size_t from, size_t to, int keep)
{
    struct kept *k = viewport_item_data(mark);
    if (!k)
        return;
    if (to <= from)
        from = to = keep = 0;
    if (k->hide_from == from && k->hide_to == to && k->hide_kept == keep)
        return;
    k->hide_from = from;
    k->hide_to = to;
    k->hide_kept = keep;
    viewport_item_stale(mark);
}

void md_kept_load(const cJSON *st)
{
    md_render_kept(scrollback_str(st, "text"), scrollback_int(st, "indent"));
}

void md_render_kept(const char *text, int indent)
{
    struct kept *k = calloc(1, sizeof *k);
    if (k) {
        k->text = strdup(text ? text : "");
        k->indent = indent;
        if (!k->text) {
            free(k);
            k = NULL;
        }
    }
    unsigned mark = 0;
    if (k)
        mark = viewport_item_begin(&(struct viewport_entry){
            .render = kept_render, .ud = k, .free_ud = kept_free, .reflow = 1,
            .pad_before = 1, .pad_after = 1});
    md_render(text, indent);
    if (k) {
        viewport_item_end();
        viewport_item_persist(mark, MD_KEPT_KIND, kept_encode);
    }
}

void md_render(const char *text, int indent)
{
    int in_code = 0;
    int code_ansi = 0;
    int code_mermaid = 0;
    char *code_url = NULL;
    unsigned char *code_roles = NULL;
    size_t code_len = 0, code_at = 0;
    int blank_pending = 0;
    int wrote_any = 0;
    char *mermaid = NULL;
    size_t mermaid_len = 0;
    int ask_indent = -1;

    while (*text) {
        char *line = take_line(&text);
        if (!line)
            break;

        const char *body;
        int lead = leading_indent(line, &body);

        if (is_fence(body)) {
            in_code = !in_code;
            code_ansi = in_code && strncmp(body + 3, "ansi", 4) == 0;
            free(code_url);
            code_url = in_code && shell_fence(body + 3) ? fence_url(text, lead) : NULL;
            free(code_roles);
            code_roles = in_code ? fence_roles(body + 3, text, &code_len) : NULL;
            code_at = 0;
            if (in_code && strncmp(body + 3, "mermaid", 7) == 0) {
                code_mermaid = 1;
            } else if (code_mermaid) {
                code_mermaid = 0;
                if (blank_pending && wrote_any)
                    ui_put("\n");
                blank_pending = 0;
                render_mermaid(mermaid ? mermaid : "", indent + 2);
                wrote_any = 1;
                free(mermaid);
                mermaid = NULL;
                mermaid_len = 0;
            }
            free(line);
            continue;
        }
        if (code_mermaid) {
            size_t n = strlen(line);
            mermaid = realloc(mermaid, mermaid_len + n + 2);
            memcpy(mermaid + mermaid_len, line, n);
            mermaid_len += n;
            mermaid[mermaid_len++] = '\n';
            mermaid[mermaid_len] = 0;
            free(line);
            continue;
        }
        if (in_code) {
            if (blank_pending && wrote_any)
                ui_put("\n");
            blank_pending = 0;
            if (code_ansi)
                render_ansi_line(line, indent + 2);
            else
                render_code_line(line, indent + 2, code_url,
                                 code_roles && code_at < code_len ? code_roles + code_at : NULL);
            code_at += strlen(line) + 1;
            wrote_any = 1;
            free(line);
            continue;
        }

        if (!*body) {
            blank_pending = wrote_any;
            free(line);
            continue;
        }
        if (blank_pending) {
            ui_put("\n");
            blank_pending = 0;
        }

        if (!strcmp(body, "@ask"))
            ask_indent = 0;
        else if (ask_indent >= 0) {
            const char *after;
            size_t      width;
            if (!is_bullet(body, &after) && !is_ordered(body, &width))
                ask_indent = -1;
        }

        if (handoff_line(body)) {
            render_handoff(indent);
            wrote_any = 1;
            free(line);
            continue;
        }

        char *doc = view_line(body);
        if (doc) {
            render_view(doc, indent);
            free(doc);
            wrote_any = 1;
            free(line);
            continue;
        }

        char *img = image_line(body);
        if (img) {
            render_image(img, indent);
            free(img);
            wrote_any = 1;
            free(line);
            continue;
        }

        if (strchr(body, '|') && peek_is_delim(text, NULL, TABLE_MAX_COLS)) {
            render_table(body, &text, indent);
            wrote_any = 1;
            free(line);
            continue;
        }

        if (is_rule(body)) {
            int width = ui_columns() - indent;
            ui_pad(indent);
            ui_esc(ui_style(UI_DIM));
            for (int i = 0; i < width && i < 60; i++)
                ui_put("\xe2\x94\x80");
            ui_esc(ui_style(UI_RESET));
            ui_put("\n");
        } else if (*body == '#') {
            const char *h = body;
            while (*h == '#')
                h++;
            while (*h == ' ')
                h++;
            ui_pad(indent);
            ui_esc(ui_style(UI_HEADING));
            put_safe(h);
            ui_esc(ui_style(UI_RESET));
            ui_put("\n");
        } else if (*body == '>') {
            const char *q = body + 1;
            if (*q == ' ')
                q++;
            ui_pad(indent);
            ui_esc(ui_style(UI_DIM));
            ui_put("\xe2\x94\x82 ");
            ui_esc(ui_style(UI_RESET));
            render_paragraph(q, indent + 2, 0, indent + 2);
        } else {
            const char *rest;
            size_t marker = 0;
            int item_indent = indent + lead;
            if (is_bullet(body, &rest)) {
                if (ask_indent > 0 && !lead)
                    item_indent += ask_indent;
                ui_pad(item_indent);
                ui_esc(ui_style(UI_CHROME));
                ui_put("\xe2\x80\xa2 ");
                ui_esc(ui_style(UI_RESET));
                render_paragraph(rest, item_indent + 2, 0, item_indent + 2);
            } else if (is_ordered(body, &marker)) {
                ui_pad(item_indent);
                ui_esc(ui_style(UI_CHROME));
                ui_putn(body, marker - 1);
                ui_esc(ui_style(UI_RESET));
                ui_put(" ");
                int after = item_indent + (int)marker;
                if (ask_indent >= 0)
                    ask_indent = (int)marker;
                render_paragraph(body + marker, after, 0, after);
            } else {
                render_paragraph(body, 0, item_indent, item_indent);
            }
        }
        wrote_any = 1;
        free(line);
    }
    if (mermaid) {
        if (blank_pending && wrote_any)
            ui_put("\n");
        for (char *p = mermaid, *nl; (nl = strchr(p, '\n')); p = nl + 1) {
            *nl = 0;
            render_code_line(p, indent + 2, NULL, NULL);
        }
        free(mermaid);
    }
    free(code_url);
    free(code_roles);
}
