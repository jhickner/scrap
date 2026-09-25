#include "docview.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chrome.h"
#include "frontend.h"
#include "md.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"

#define POLL_MS 250

static const char HINT[] =
    "\xe2\x86\x91\xe2\x86\x93 scroll  \xc2\xb7  space page  \xc2\xb7  g/G ends  \xc2\xb7  q close";

struct view {
    const char *path;
    char       *text;
    int         markdown;
    char      **line;
    int         n;
    int         width;
    int         top;
    int         body;
};

static void lines_free(struct view *v)
{
    for (int i = 0; i < v->n; i++)
        free(v->line[i]);
    free(v->line);
    v->line = NULL;
    v->n = 0;
}

static void layout(struct view *v, int width)
{
    if (v->line && v->width == width)
        return;
    lines_free(v);
    v->width = width;

    ui_capture_begin(width);
    if (v->markdown)
        md_render(v->text, 0);
    else
        ui_wrapped(v->text, 0, UI_BODY);
    char *out = ui_capture_end();
    if (!out)
        return;

    int cap = 0;
    for (char *s = out, *nl; *s; s = nl + 1) {
        nl = strchr(s, '\n');
        if (!nl)
            nl = s + strlen(s);
        if (v->n == cap) {
            cap = cap ? cap * 2 : 256;
            v->line = realloc(v->line, (size_t)cap * sizeof *v->line);
        }
        v->line[v->n++] = strndup(s, (size_t)(nl - s));
        if (!*nl)
            break;
    }
    free(out);
}

static void clamp(struct view *v)
{
    int max = v->n - v->body;
    if (v->top > max)
        v->top = max;
    if (v->top < 0)
        v->top = 0;
}

static void paint(void *ud)
{
    struct view *v = ud;
    int          columns = ui_columns();

    int foot = chrome_foot_rows(NULL, HINT, columns);
    v->body = chrome_modal_rows() - 1 - foot;
    if (v->body < 1)
        v->body = 1;

    layout(v, columns > 4 ? columns - 4 : 1);
    clamp(v);

    const char *slash = strrchr(v->path, '/');
    int         last = v->top + v->body < v->n ? v->top + v->body : v->n;
    char        title[512];
    snprintf(title, sizeof title, "%s \xc2\xb7 %d-%d/%d", slash ? slash + 1 : v->path,
             v->n ? v->top + 1 : 0, last, v->n);
    chrome_title_paint(title);

    ui_put("\n");
    for (int i = 0; i < v->body; i++) {
        if (v->top + i < v->n) {
            ui_pad(2);
            ui_put(v->line[v->top + i]);
            ui_esc(ui_style(UI_RESET));
        }
        ui_put("\n");
    }
    chrome_foot_paint(NULL, HINT, columns);
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char  *buf = NULL;
    size_t len = 0, cap = 0, got;
    do {
        if (cap - len < 4096) {
            cap = cap ? cap * 2 : 8192;
            buf = realloc(buf, cap + 1);
        }
        got = fread(buf + len, 1, cap - len, f);
        len += got;
    } while (got);
    fclose(f);
    buf[len] = '\0';
    return buf;
}

static int is_markdown(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && (!strcmp(dot, ".md") || !strcmp(dot, ".markdown"));
}

int docview_open(const char *path)
{
    if (!frontend_has_keyboard() || !tty_is_raw())
        return 0;

    struct view v = {0};
    v.path = path;
    v.text = slurp(path);
    if (!v.text)
        return 0;
    v.markdown = is_markdown(path);

    chrome_full(1);
    chrome_modal(paint, &v);

    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, POLL_MS)) {
            if (chrome_modal_interrupted())
                break;
            workspace_pump_quiet();
            continue;
        }

        int done = 0;
        int page = v.body > 1 ? v.body - 1 : 1;
        switch (ev.key) {
        case TK_UP:          v.top--; break;
        case TK_DOWN:        v.top++; break;
        case TK_SCROLL_UP:   v.top -= 3; break;
        case TK_SCROLL_DOWN: v.top += 3; break;
        case TK_PAGE_UP:     v.top -= page; break;
        case TK_PAGE_DOWN:   v.top += page; break;
        case TK_HOME:        v.top = 0; break;
        case TK_END:         v.top = v.n; break;
        case TK_RESIZE:      v.width = 0; break;

        case TK_ESCAPE:
        case TK_EOF:
            done = 1;
            break;

        case TK_CHAR:
            switch (ev.cp) {
            case 'j': v.top++; break;
            case 'k': v.top--; break;
            case ' ':
            case 'f': v.top += page; break;
            case 'b': v.top -= page; break;
            case 'd': v.top += page / 2; break;
            case 'u': v.top -= page / 2; break;
            case 'g': v.top = 0; break;
            case 'G': v.top = v.n; break;
            case 'q':
            case 'x':
            case 3:
            case 4:
                done = 1;
                break;
            }
            break;

        default:
            break;
        }
        free(ev.text);
        if (done)
            break;
        clamp(&v);

        if (tty_input_waiting())
            continue;
        chrome_paint();
    }

    chrome_modal(NULL, NULL);
    chrome_full(0);
    lines_free(&v);
    free(v.text);

    viewport_forget();
    viewport_touch();
    viewport_flush();
    return 1;
}

int docview_click(int row, int col)
{
    char *url = viewport_link_at(row, col);
    if (!url)
        return 0;
    int opened = strncmp(url, "file://", 7) == 0 && docview_open(url + 7);
    free(url);
    return opened;
}
