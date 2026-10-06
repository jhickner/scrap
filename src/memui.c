#include "memui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#include "chrome.h"
#include "frontend.h"
#include "image.h"
#include "imageview.h"
#include "keyhelp.h"
#include "md.h"
#include "memnote.h"
#include "prompt.h"
#include "replbox.h"
#include "terminalrun.h"
#include "text.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"
#include "vendor/cJSON.h"

#define POLL_MS     250
#define MEM_MAX     (8u << 20)
#define MEM_TIMEOUT 15
#define STORES_MAX  16
#define TAGS_MAX    16
#define FIELD_MAX   2048
#define SUGGEST_MAX 6
#define BOX_GUTTER  2
#define THUMBS_MAX  16
#define THUMB_COLS  24
#define THUMB_ROWS  8
#define THUMB_GAP   2

#define REVERSE     "\x1b[7m \x1b[27m"

enum tab { T_TODO, T_SEARCH, T_STARRED, T_COUNT };
static const char *const TAB_NAMES[T_COUNT] = {"todo", "search", "starred"};

enum field { F_NONE, F_QUERY, F_TAG, F_NEW, F_RETAG, F_TITLE };

static int boxed(enum field f) { return f == F_NEW || f == F_TITLE; }

static const struct keyhelp_row LIST_KEYS[] = {
    {"LIST", "up/down j/k", "move"},
    {"LIST", "g/G", "first / last"},
    {"LIST", "enter", "open"},
    {"LIST", "tab 1 2 3", "todo / search / starred"},
    {"LIST", "s", "next store"},
    {"LIST", "h", "show or hide done"},
    {"LIST", "r", "reload"},
    {"FILTER", "/", "search text"},
    {"FILTER", "#", "add a tag filter"},
    {"FILTER", "backspace", "remove the last tag"},
    {"RECORD", "n", "new todo or note"},
    {"RECORD", "x / space", "toggle done"},
    {"RECORD", "*", "toggle starred"},
    {"RECORD", "d", "delete"},
    {"RECORD", "p", "put in the prompt"},
    {"VIEW", "?", "this help"},
    {"VIEW", "q / esc", "close"},
};

static const struct keyhelp_row RECORD_KEYS[] = {
    {"VIEW", "up/down j/k", "scroll"},
    {"VIEW", "space / b", "page"},
    {"VIEW", "g/G", "top / bottom"},
    {"VIEW", "?", "this help"},
    {"VIEW", "esc / q", "back to the list"},
    {"RECORD", "e", "edit in $EDITOR"},
    {"RECORD", "t", "edit the title"},
    {"RECORD", "#", "edit the tags"},
    {"RECORD", "x", "toggle done"},
    {"RECORD", "*", "toggle starred"},
    {"RECORD", "d", "delete"},
    {"RECORD", "p", "put in the prompt"},
    {"RECORD", "i / click", "images full screen"},
};

static const struct keyhelp_row FIELD_KEYS[] = {
    {"INPUT", "enter", "save"},
    {"INPUT", "shift-enter", "newline in a note"},
    {"INPUT", "ctrl-v", "paste"},
    {"INPUT", "esc", "cancel"},
    {"INPUT", "ctrl-u", "clear"},
    {"INPUT", "tab", "complete a tag"},
    {"INPUT", "!tag", "exclude a tag from the filter"},
};

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof *(a)))

struct view {
    char stores[STORES_MAX][64];
    int  nstores;
    int  store;

    enum tab tab;
    char     tags[TAGS_MAX][64];
    int      ntags;
    int      show_done;
    char     query[256];

    cJSON *res;
    cJSON *records;
    int    total;
    int    sel;
    int    top;
    int    body;

    cJSON *alltags;

    enum field field;
    char       edit[FIELD_MAX];
    struct replbox box;
    const char *ask;
    char        asking[256];
    char        problem[512];

    cJSON *rec;
    char **line;
    int    n;
    int    width;
    int    dtop;

    char     dir[256];
    char    *thumb[THUMBS_MAX];
    uint32_t thumb_id[THUMBS_MAX];
    int      thumb_cols[THUMBS_MAX];
    int      thumb_rows[THUMBS_MAX];
    int      nthumbs;
    int      strip;
};

static const char *store_name(const struct view *v) { return v->stores[v->store]; }

/* ponytail: mem runs synchronously (~25ms locally); a slow server stalls input up to MEM_TIMEOUT. Move to tty_watch if that matters. */
static cJSON *run_mem(struct view *v, const char *const *args)
{
    const char *argv[64];
    int         a = 0;
    argv[a++] = "mem";
    argv[a++] = "--json";
    argv[a++] = "-s";
    argv[a++] = store_name(v);
    while (*args && a < 63)
        argv[a++] = *args++;
    argv[a] = NULL;

    size_t got;
    int    status;
    char  *buf = memnote_capture((char *const *)argv, 0, MEM_MAX, MEM_TIMEOUT, &got, &status);
    if (!buf) {
        snprintf(v->problem, sizeof v->problem, "could not run mem");
        return NULL;
    }
    cJSON *out = status == -1 ? NULL : cJSON_Parse(buf);
    cJSON *err = cJSON_GetObjectItem(out, "error");
    if (status == -1 || !out || err || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message"));
        text_chomp(buf);
        text_one_line(status == -1 ? "mem timed out" : m ? m : *buf ? buf : "mem failed",
                      v->problem, sizeof v->problem);
        cJSON_Delete(out);
        out = NULL;
    }
    free(buf);
    return out;
}

static const char *str(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, key));
    return s ? s : "";
}

static int has_tag(const cJSON *rec, const char *name)
{
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(rec, "tags"))
        if (!strcmp(str(t, "name"), name))
            return 1;
    return 0;
}

static void age(const char *iso, char *out, size_t size)
{
    struct tm tm = {0};
    out[0] = '\0';
    if (iso && strptime(iso, "%Y-%m-%dT%H:%M:%S", &tm))
        text_ago(timegm(&tm), 1, out, size);
}

static void lines_free(struct view *v)
{
    for (int i = 0; i < v->n; i++)
        free(v->line[i]);
    free(v->line);
    v->line = NULL;
    v->n = 0;
    v->width = 0;
}

static void load_stores(struct view *v)
{
    snprintf(v->stores[0], sizeof v->stores[0], "personal");
    v->nstores = 1;
    cJSON *r = run_mem(v, (const char *[]){"stores", NULL});
    const cJSON *s;
    int          n = 0;
    cJSON_ArrayForEach(s, cJSON_GetObjectItem(r, "stores"))
        if (cJSON_IsString(s) && n < STORES_MAX)
            snprintf(v->stores[n++], sizeof v->stores[0], "%s", s->valuestring);
    if (n)
        v->nstores = n;
    cJSON_Delete(r);
    v->problem[0] = '\0';
}

static void load(struct view *v)
{
    char id[64];
    snprintf(id, sizeof id, "%s", str(cJSON_GetArrayItem(v->records, v->sel), "id"));

    char filter[TAGS_MAX * 66 + 32] = "";
    if (v->tab == T_TODO)
        strcat(filter, "todo");
    else if (v->tab == T_STARRED)
        strcat(filter, "starred");
    for (int i = 0; i < v->ntags; i++) {
        if (*filter)
            strcat(filter, " ");
        strcat(filter, v->tags[i]);
    }
    if (v->tab == T_TODO && !v->show_done)
        strcat(filter, " !done");

    const char *args[16];
    int         a = 0;
    args[a++] = "search";
    if (*filter)
        args[a++] = filter;
    if (v->tab == T_SEARCH && *v->query) {
        args[a++] = "--text";
        args[a++] = v->query;
    }
    args[a++] = "--sort";
    args[a++] = v->tab == T_TODO ? "created_at" : "modified_at";
    args[a++] = "--order";
    args[a++] = "desc";
    args[a++] = "--limit";
    args[a++] = "100";
    if (v->tab == T_SEARCH)
        args[a++] = "--tag-counts";
    args[a] = NULL;

    v->problem[0] = '\0';
    cJSON *r = run_mem(v, args);
    cJSON_Delete(v->res);
    v->res = r;
    v->records = cJSON_GetObjectItem(r, "records");
    v->total = cJSON_GetArraySize(v->records);
    cJSON *total = cJSON_GetObjectItem(r, "total");
    if (cJSON_IsNumber(total))
        v->total = total->valueint;

    int n = cJSON_GetArraySize(v->records);
    for (int i = 0; *id && i < n; i++)
        if (!strcmp(str(cJSON_GetArrayItem(v->records, i), "id"), id)) {
            v->sel = i;
            break;
        }
    if (v->sel >= n)
        v->sel = n - 1;
    if (v->sel < 0)
        v->sel = 0;
}

static void load_alltags(struct view *v)
{
    if (v->alltags)
        return;
    v->alltags = run_mem(v, (const char *[]){"tags", "-n", "5000", NULL});
}

static void forget_tags(struct view *v)
{
    cJSON_Delete(v->alltags);
    v->alltags = NULL;
}

static int edit_record(struct view *v, const cJSON *rec, const char *const *extra)
{
    char rev[32];
    const cJSON *r = cJSON_GetObjectItem(rec, "revision");
    snprintf(rev, sizeof rev, "%d", cJSON_IsNumber(r) ? r->valueint : 0);
    const char *args[64] = {"edit", str(rec, "id"), "--revision", rev};
    int         a = 4;
    while (*extra && a < 63)
        args[a++] = *extra++;
    args[a] = NULL;
    v->problem[0] = '\0';
    cJSON *out = run_mem(v, args);
    cJSON_Delete(out);
    return out != NULL;
}

static int set_tags(struct view *v, const cJSON *rec, char names[][64], int n)
{
    const char *extra[2 * 32 + 1];
    int         a = 0;
    for (int i = 0; i < n && i < 32; i++) {
        extra[a++] = "--tag";
        extra[a++] = names[i];
    }
    extra[a] = NULL;
    forget_tags(v);
    return edit_record(v, rec, extra);
}

static int tag_names(const cJSON *rec, char out[][64], int max)
{
    int          n = 0;
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(rec, "tags"))
        if (*str(t, "name") && n < max)
            snprintf(out[n++], 64, "%s", str(t, "name"));
    return n;
}

static int toggle_tag(struct view *v, const cJSON *rec, const char *tag)
{
    char names[32][64];
    int  n = tag_names(rec, names, 31), w = 0, had = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(names[i], tag))
            had = 1;
        else if (w != i)
            memcpy(names[w++], names[i], 64);
        else
            w++;
    }
    if (!had)
        snprintf(names[w++], 64, "%s", tag);
    return set_tags(v, rec, names, w);
}

static int split_words(const char *s, char out[][64], int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == ',')
            s++;
        size_t len = strcspn(s, " ,");
        if (!len)
            break;
        snprintf(out[n], 64, "%.*s", (int)(len < 63 ? len : 63), s);
        for (char *c = out[n]; *c; c++)
            if (*c >= 'A' && *c <= 'Z')
                *c += 'a' - 'A';
        n++;
        s += len;
    }
    return n;
}

static cJSON *fetch(struct view *v, const cJSON *rec)
{
    const char *id = str(rec, "id");
    return *id ? run_mem(v, (const char *[]){"get", id, NULL}) : NULL;
}

static void layout(struct view *v, int width)
{
    if (v->line && v->width == width)
        return;
    lines_free(v);
    v->width = width;

    const char *content = str(v->rec, "content");
    ui_capture_begin(width);
    md_render(content, 0);
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItem(v->rec, "attachments")) {
        if (!strncmp(str(a, "mime"), "image/", 6))
            continue;
        char row[512];
        snprintf(row, sizeof row, "\n%s\xe2\x80\xa2 %s  %s%s", ui_style(UI_DIM),
                 *str(a, "filename") ? str(a, "filename") : str(a, "id"),
                 str(a, "mime"), ui_style(UI_RESET));
        ui_put(row);
    }
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

static void put_fit(const char *s, int *left);
static void paint(void *ud);

static void thumbs_free(struct view *v)
{
    for (int i = 0; i < v->nthumbs; i++) {
        image_drop(v->thumb_id[i]);
        unlink(v->thumb[i]);
        *strrchr(v->thumb[i], '/') = '\0';
        rmdir(v->thumb[i]);
        free(v->thumb[i]);
    }
    v->nthumbs = 0;
    v->strip = 0;
    if (v->dir[0])
        rmdir(v->dir);
    v->dir[0] = '\0';
}

static void thumbs_fetch(struct view *v)
{
    thumbs_free(v);
    const char *tmp = getenv("TMPDIR");
    snprintf(v->dir, sizeof v->dir, "%s/scrap-mem-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(v->dir)) {
        v->dir[0] = '\0';
        return;
    }
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItem(v->rec, "attachments")) {
        if (strncmp(str(a, "mime"), "image/", 6) || v->nthumbs == THUMBS_MAX)
            continue;
        char name[128];
        snprintf(name, sizeof name, "%s", *str(a, "filename") ? str(a, "filename") : str(a, "id"));
        for (char *c = name; *c; c++)
            if (*c == '/')
                *c = '-';
        char *path = text_dsprintf("%s/%d", v->dir, v->nthumbs);
        if (!path || mkdir(path, 0700) != 0) {
            free(path);
            continue;
        }
        free(path);
        path = text_dsprintf("%s/%d/%s", v->dir, v->nthumbs, name);
        const char *argv[] = {"mem", "-s", store_name(v), "get", str(a, "id"),
                              "--output", path, NULL};
        size_t got;
        int    status;
        char  *out = memnote_capture((char *const *)argv, 1, 4096, MEM_TIMEOUT, &got, &status);
        free(out);
        if (!out || status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            unlink(path);
            *strrchr(path, '/') = '\0';
            rmdir(path);
            free(path);
            continue;
        }
        v->thumb_id[v->nthumbs] = 0;
        v->thumb[v->nthumbs++] = path;
    }
}

static void thumbs_load(struct view *v)
{
    v->strip = 0;
    for (int i = 0; i < v->nthumbs; i++) {
        if (!v->thumb_id[i])
            v->thumb_id[i] = image_thumb(v->thumb[i], THUMB_COLS, THUMB_ROWS,
                                         &v->thumb_cols[i], &v->thumb_rows[i]);
        if (!v->thumb_id[i]) {
            v->thumb_cols[i] = THUMB_COLS;
            v->thumb_rows[i] = 1;
        }
        if (v->thumb_rows[i] > v->strip)
            v->strip = v->thumb_rows[i];
    }
}

static void paint_strip_row(struct view *v, int r, int columns)
{
    int left = columns - 3;
    ui_pad(2);
    for (int i = 0; i < v->nthumbs && left >= v->thumb_cols[i]; i++) {
        if (i) {
            ui_pad(THUMB_GAP);
            left -= THUMB_GAP;
        }
        if (v->thumb_id[i] && r < v->thumb_rows[i])
            image_place_row(v->thumb_id[i], r, v->thumb_cols[i]);
        else if (!v->thumb_id[i] && r == 0) {
            const char *slash = strrchr(v->thumb[i], '/');
            int         room = v->thumb_cols[i];
            ui_esc(ui_style(UI_DIM));
            put_fit("[image] ", &room);
            put_fit(slash ? slash + 1 : v->thumb[i], &room);
            ui_esc(ui_style(UI_RESET));
            ui_pad(room);
        } else
            ui_pad(v->thumb_cols[i]);
        left -= v->thumb_cols[i];
    }
}

static int thumb_at(const struct view *v, int col)
{
    int x = 2;
    for (int i = 0; i < v->nthumbs; i++) {
        if (col >= x && col < x + v->thumb_cols[i])
            return i;
        x += v->thumb_cols[i] + THUMB_GAP;
    }
    return -1;
}

static void open_images(struct view *v, int at)
{
    if (at < 0 || at >= v->nthumbs)
        return;
    imageview_open_paths((const char *const *)v->thumb, v->nthumbs, at);
    chrome_full(1);
    chrome_modal(paint, v);
}

static void open_record(struct view *v, cJSON *rec)
{
    cJSON *full = fetch(v, rec);
    if (!full)
        return;
    cJSON_Delete(v->rec);
    v->rec = full;
    v->dtop = 0;
    lines_free(v);
    thumbs_fetch(v);
}

static void close_record(struct view *v)
{
    cJSON_Delete(v->rec);
    v->rec = NULL;
    lines_free(v);
    thumbs_free(v);
}

static void reopen(struct view *v)
{
    cJSON *full = fetch(v, v->rec);
    if (full) {
        cJSON_Delete(v->rec);
        v->rec = full;
    }
    lines_free(v);
    thumbs_fetch(v);
}

static void put_fit(const char *s, int *left)
{
    if (*left <= 0 || !s)
        return;
    size_t n = ui_fit_bytes(s, (size_t)*left);
    ui_putn(s, n);
    *left -= (int)ui_cells_n(s, n);
}

static void put_role(enum ui_role role, const char *s, int *left)
{
    ui_esc(ui_style(role));
    put_fit(s, left);
    ui_esc(ui_style(UI_RESET));
}

static const char *last_word(const char *s)
{
    const char *sp = strrchr(s, ' ');
    s = sp ? sp + 1 : s;
    return *s == '!' ? s + 1 : s;
}

static int suggest(struct view *v, const char **out, int max)
{
    const char *w = last_word(v->edit);
    size_t      len = strlen(w);
    int         n = 0;
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(v->alltags, "tags")) {
        const char *name = str(t, "name");
        if (n < max && *name && (!len || !strncmp(name, w, len)) && strcmp(name, w))
            out[n++] = name;
    }
    return n;
}

static void complete(struct view *v)
{
    const char *best;
    if (!suggest(v, &best, 1))
        return;
    size_t at = (size_t)(last_word(v->edit) - v->edit);
    snprintf(v->edit + at, sizeof v->edit - at, "%s", best);
}

static void put_field(struct view *v, const char *label, int *left)
{
    ui_esc(ui_style(UI_ACCENT));
    put_fit(label, left);
    ui_esc(ui_style(UI_BRAND));
    const char *e = v->edit;
    int         room = *left - 1;
    if ((int)ui_cells(e) > room)
        while (*e && (int)ui_cells(e) > room)
            e++;
    put_fit(e, left);
    ui_esc(ui_style(UI_RESET));
    if (*left > 0) {
        ui_put(REVERSE);
        (*left)--;
    }
    if (v->field == F_TAG || v->field == F_RETAG) {
        const char *s[SUGGEST_MAX];
        int         n = suggest(v, s, SUGGEST_MAX);
        for (int i = 0; i < n && *left > 4; i++) {
            put_fit("  ", left);
            put_role(UI_DIM, s[i], left);
        }
    }
}

static void put_tags_of(const cJSON *rec, int with_marks, int *left)
{
    const cJSON *t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItem(rec, "tags")) {
        const char *name = str(t, "name");
        if (!*name || (!with_marks && (!strcmp(name, "todo") || !strcmp(name, "done") ||
                                      !strcmp(name, "starred"))))
            continue;
        put_fit("  ", left);
        put_role(UI_DIM, name, left);
    }
}

static void title_of(const cJSON *rec, char *out, size_t size)
{
    const char *t = str(rec, "title");
    const char *s = *str(rec, "summary") ? str(rec, "summary") : str(rec, "content");
    text_one_line(*t ? t : s, out, size);
}

static int list_head(struct view *v, int columns)
{
    char title[256];
    snprintf(title, sizeof title, "mem \xc2\xb7 %s \xc2\xb7 %d %s", store_name(v), v->total,
             v->tab == T_SEARCH ? (v->total == 1 ? "result" : "results")
             : v->tab == T_STARRED ? "starred" : v->show_done ? "todo" : "open");
    chrome_title_paint(title);

    int left = columns - 1;
    put_fit("  ", &left);
    for (int i = 0; i < T_COUNT; i++) {
        ui_esc(ui_style(i == (int)v->tab ? UI_ACCENT : UI_DIM));
        put_fit(i == (int)v->tab ? "[" : " ", &left);
        put_fit(TAB_NAMES[i], &left);
        put_fit(i == (int)v->tab ? "]" : " ", &left);
        ui_esc(ui_style(UI_RESET));
        put_fit("  ", &left);
    }
    if (v->nstores > 1) {
        put_role(UI_DIM, "   store", &left);
        for (int i = 0; i < v->nstores; i++) {
            put_fit("  ", &left);
            put_role(i == v->store ? UI_ACCENT : UI_DIM, v->stores[i], &left);
        }
    }
    ui_put("\n");
    int rows = 2;

    left = columns - 1;
    put_role(UI_ACCENT, "  # ", &left);
    for (int i = 0; i < v->ntags; i++) {
        put_role(v->tags[i][0] == '!' ? UI_ERROR : UI_BRAND, v->tags[i], &left);
        put_fit("  ", &left);
    }
    if (v->field == F_TAG)
        put_field(v, "", &left);
    else if (!v->ntags)
        put_role(UI_DIM, "no tag filter", &left);
    if (v->tab == T_TODO && v->field != F_TAG)
        put_role(UI_DIM, v->show_done ? "   done shown" : "   done hidden", &left);
    ui_put("\n");
    rows++;

    if (v->tab == T_SEARCH) {
        left = columns - 1;
        if (v->field == F_QUERY)
            put_field(v, "  \xe2\x80\xba ", &left);
        else {
            put_role(UI_ACCENT, "  \xe2\x80\xba ", &left);
            put_role(*v->query ? UI_BRAND : UI_DIM, *v->query ? v->query : "/ to search", &left);
        }
        ui_put("\n");
        left = columns - 1;
        put_fit("  ", &left);
        const cJSON *t;
        int          shown = 0;
        cJSON_ArrayForEach(t, cJSON_GetObjectItem(v->res, "tags")) {
            const char *name = str(t, "name");
            int         active = 0;
            for (int i = 0; i < v->ntags; i++)
                active |= !strcmp(v->tags[i] + (v->tags[i][0] == '!'), name);
            if (!*name || active || shown++ >= 12)
                continue;
            char cell[96];
            snprintf(cell, sizeof cell, "%s %d  ", name,
                     cJSON_GetObjectItem(t, "count") ? cJSON_GetObjectItem(t, "count")->valueint : 0);
            put_role(UI_DIM, cell, &left);
        }
        ui_put("\n");
        rows += 2;
    }
    ui_put("\n");
    return rows + 1;
}

static void list_row(struct view *v, const cJSON *rec, int selected, int columns)
{
    int left = columns - 1;
    if (selected)
        ui_row_sel(1);
    ui_esc(ui_style(selected ? UI_ACCENT : UI_RESET));
    put_fit(selected ? "  \xe2\x86\x92 " : "    ", &left);

    int done = has_tag(rec, "done");
    if (v->tab == T_TODO)
        put_role(done ? UI_OK : UI_DIM, done ? "\xe2\x9c\x93 " : "\xe2\x97\x8b ", &left);
    else
        put_role(UI_ACCENT, has_tag(rec, "starred") ? "\xe2\x98\x85 " : "  ", &left);

    char title[512];
    title_of(rec, title, sizeof title);
    ui_esc(ui_style(done ? UI_DIM : selected ? UI_ACCENT : UI_BODY));
    if (selected)
        ui_row_sel(1);
    int cap = left > 30 ? left * 2 / 3 : left, room = cap;
    put_fit(title, &room);
    left -= cap - room;
    ui_esc(ui_style(UI_RESET));
    if (selected)
        ui_row_sel(1);

    if (v->tab != T_TODO) {
        put_fit("  ", &left);
        put_role(UI_DIM, str(rec, "id"), &left);
    }
    put_tags_of(rec, 0, &left);
    char ago[64];
    age(str(rec, "modified_at"), ago, sizeof ago);
    if (*ago) {
        put_fit("  ", &left);
        put_role(UI_DIM, ago, &left);
    }
    if (selected) {
        ui_row_sel(1);
        ui_esc(UI_ERASE_EOL);
        ui_row_sel(0);
    }
    ui_put("\n");
}

static int box_rows(struct view *v, int columns)
{
    if (!boxed(v->field))
        return 0;
    replbox_width(&v->box, columns - 1 - BOX_GUTTER);
    int rows = replbox_wants(&v->box);
    if (!replbox_render(&v->box, rows))
        return 0;
    int room = chrome_modal_rows() / 2;
    replbox_scroll(&v->box, room < 1 ? 1 : room);
    return rows < room ? rows : room;
}

static int foot_rows(struct view *v, int columns)
{
    int extra = (v->problem[0] ? 1 : 0) + box_rows(v, columns) + (boxed(v->field) ||
                (v->rec && v->field == F_RETAG));
    return chrome_foot_rows(v->ask, NULL, columns) + extra;
}

static void paint_box(struct view *v, int columns)
{
    int top = replbox_top(&v->box), end = top + box_rows(v, columns);
    for (int y = top; y < end; y++) {
        ui_pad(BOX_GUTTER);
        replbox_paint_row(&v->box, y, BOX_GUTTER, 1);
        ui_put("\n");
    }
}

static void paint_foot(struct view *v, int columns)
{
    int left = columns - 1;
    if (v->field == F_NEW) {
        char label[TAGS_MAX * 66 + 64] = "  \xe2\x80\xba ";
        strcat(label, v->tab == T_TODO ? "new todo" : v->tab == T_STARRED ? "new starred note" : "new note");
        int any = 0;
        for (int i = 0; i < v->ntags; i++)
            if (v->tags[i][0] != '!') {
                strcat(label, any++ ? " " : " \xc2\xb7 tagged ");
                strcat(label, v->tags[i]);
            }
        strcat(label, ":");
        put_role(UI_ACCENT, label, &left);
        ui_put("\n");
        paint_box(v, columns);
    } else if (v->field == F_TITLE) {
        put_role(UI_ACCENT, "  title:", &left);
        ui_put("\n");
        paint_box(v, columns);
    } else if (v->rec && v->field == F_RETAG) {
        put_field(v, "  tags: ", &left);
        ui_put("\n");
    }
    if (v->problem[0]) {
        left = columns - 1;
        put_role(UI_ERROR, "  ", &left);
        put_role(UI_ERROR, v->problem, &left);
        ui_put("\n");
    }
    chrome_foot_paint(v->ask, NULL, columns);
}

static void paint_list(struct view *v, int columns)
{
    int head = list_head(v, columns);
    v->body = chrome_modal_rows() - head - foot_rows(v, columns);
    if (v->body < 1)
        v->body = 1;

    int n = cJSON_GetArraySize(v->records);
    if (v->sel < v->top)
        v->top = v->sel;
    if (v->sel >= v->top + v->body)
        v->top = v->sel - v->body + 1;
    if (v->top > n - v->body)
        v->top = n - v->body;
    if (v->top < 0)
        v->top = 0;

    for (int i = 0; i < v->body; i++) {
        int row = v->top + i;
        if (row < n)
            list_row(v, cJSON_GetArrayItem(v->records, row), row == v->sel, columns);
        else if (row == 0 && !n && !v->problem[0]) {
            ui_esc(ui_style(UI_DIM));
            ui_put(v->tab == T_SEARCH ? "    nothing found\n"
                   : v->tab == T_STARRED ? "    nothing starred\n" : "    nothing to do\n");
            ui_esc(ui_style(UI_RESET));
        } else
            ui_put("\n");
    }
    paint_foot(v, columns);
}

static void paint_record(struct view *v, int columns)
{
    char head[256];
    snprintf(head, sizeof head, "mem \xc2\xb7 %s \xc2\xb7 %s", store_name(v), str(v->rec, "id"));
    chrome_title_paint(head);

    char title[512];
    const char *t = str(v->rec, "title");
    text_one_line(*t ? t : "untitled", title, sizeof title);
    int left = columns - 1;
    put_fit("  ", &left);
    put_role(*t ? UI_HEADING : UI_DIM, title, &left);
    ui_put("\n");

    left = columns - 1;
    char meta[128], ago[64];
    age(str(v->rec, "modified_at"), ago, sizeof ago);
    const cJSON *rev = cJSON_GetObjectItem(v->rec, "revision");
    snprintf(meta, sizeof meta, "  %s \xc2\xb7 rev %d", ago, cJSON_IsNumber(rev) ? rev->valueint : 0);
    put_role(UI_DIM, meta, &left);
    put_tags_of(v->rec, 1, &left);
    ui_put("\n\n");

    v->body = chrome_modal_rows() - 4 - foot_rows(v, columns);
    if (v->body < 1)
        v->body = 1;
    layout(v, columns > 4 ? columns - 4 : 1);
    thumbs_load(v);
    int total = v->n + (v->strip ? 1 + v->strip : 0);
    if (v->dtop > total - v->body)
        v->dtop = total - v->body;
    if (v->dtop < 0)
        v->dtop = 0;
    for (int i = 0; i < v->body; i++) {
        int row = v->dtop + i;
        if (row < v->n) {
            ui_pad(2);
            ui_put(v->line[row]);
            ui_esc(ui_style(UI_RESET));
        } else if (row > v->n && row < total)
            paint_strip_row(v, row - v->n - 1, columns);
        ui_put("\n");
    }
    paint_foot(v, columns);
}

static void paint(void *ud)
{
    struct view *v = ud;
    int          columns = ui_columns();
    if (v->rec)
        paint_record(v, columns);
    else
        paint_list(v, columns);
}

static void to_prompt(struct view *v, const cJSON *rec)
{
    cJSON *full = fetch(v, rec);
    if (!full)
        return;
    const char *title = str(full, "title");
    const char *content = str(full, "content");
    char       *draft = NULL;
    int         cursor = 0;
    prompt_stash_draft(&draft, &cursor);
    char *text = text_dsprintf("%s%s%s%s%s", draft ? draft : "", draft ? "\n\n" : "",
                               title, *title ? "\n\n" : "",
                               content);
    if (text)
        prompt_adopt_draft(text, (int)strlen(text));
    free(text);
    free(draft);
    cJSON_Delete(full);
}

static void run_editor(struct view *v)
{
    char store[256], id[64], cmd[512];
    if (!text_shell_quote(store_name(v), store, sizeof store) ||
        !text_shell_quote(str(v->rec, "id"), id, sizeof id))
        return;
    snprintf(cmd, sizeof cmd, "mem -s %s edit %s", store, id);
    int status = terminal_run_external(cmd);
    if (status == -1 || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        snprintf(v->problem, sizeof v->problem, "mem edit failed");
    viewport_forget();
    reopen(v);
}

static void field_begin(struct view *v, enum field f, const char *text)
{
    v->field = f;
    snprintf(v->edit, sizeof v->edit, "%s", text ? text : "");
    if (boxed(f)) {
        replbox_init(&v->box, NULL, 0);
        replbox_set_text(&v->box, text);
    }
    if (f == F_TAG || f == F_RETAG)
        load_alltags(v);
}

static void field_end(struct view *v)
{
    if (boxed(v->field))
        replbox_free(&v->box);
    v->field = F_NONE;
    v->edit[0] = '\0';
}

static int blank(const char *s)
{
    s += strspn(s, " \t\r\n");
    return !*s;
}

static void field_commit(struct view *v)
{
    enum field f = v->field;
    const char *text = boxed(f) ? replbox_line(&v->box) : v->edit;
    switch (f) {
    case F_QUERY:
        break;
    case F_TAG: {
        char words[TAGS_MAX][64];
        int  n = split_words(v->edit, words, TAGS_MAX);
        for (int i = 0; i < n && v->ntags < TAGS_MAX; i++) {
            int dup = 0;
            for (int k = 0; k < v->ntags; k++)
                dup |= !strcmp(v->tags[k], words[i]);
            if (!dup)
                memcpy(v->tags[v->ntags++], words[i], 64);
        }
        v->sel = 0;
        load(v);
        break;
    }
    case F_NEW: {
        if (blank(text))
            break;
        const char *args[2 * TAGS_MAX + 8] = {"create", "--text", text};
        int         a = 3;
        if (v->tab == T_TODO) {
            args[a++] = "--tag";
            args[a++] = "todo";
        } else if (v->tab == T_STARRED) {
            args[a++] = "--tag";
            args[a++] = "starred";
        }
        for (int i = 0; i < v->ntags; i++)
            if (v->tags[i][0] != '!') {
                args[a++] = "--tag";
                args[a++] = v->tags[i];
            }
        args[a] = NULL;
        v->problem[0] = '\0';
        cJSON *out = run_mem(v, args);
        if (out) {
            forget_tags(v);
            v->records = NULL;
            v->sel = 0;
            load(v);
        }
        cJSON_Delete(out);
        break;
    }
    case F_RETAG: {
        char words[32][64];
        int  n = split_words(v->edit, words, 32);
        if (set_tags(v, v->rec, words, n))
            reopen(v);
        break;
    }
    case F_TITLE: {
        char title[FIELD_MAX];
        text_one_line(text, title, sizeof title);
        if (edit_record(v, v->rec, (const char *[]){"--title", title, NULL}))
            reopen(v);
        break;
    }
    case F_NONE:
        break;
    }
    field_end(v);
}

static void field_backspace(char *s)
{
    size_t len = strlen(s);
    while (len && ((unsigned char)s[len - 1] & 0xC0) == 0x80)
        len--;
    if (len)
        len--;
    s[len] = '\0';
}

static void field_append(char *s, const char *add)
{
    size_t len = strlen(s);
    for (; *add && len + 1 < FIELD_MAX; add++)
        s[len++] = *add == '\n' || *add == '\r' || *add == '\t' ? ' ' : *add;
    s[len] = '\0';
}

/* Returns 1 when the event changed the query, which reloads the list. */
static int field_key(struct view *v, tty_event *ev)
{
    int was = v->field;
    if (boxed(v->field)) {
        if (ev->key == TK_ENTER)
            field_commit(v);
        else if (ev->key == TK_ESCAPE || (ev->key == TK_CHAR && (ev->cp == 3 || ev->cp == 4)))
            field_end(v);
        else if (!(ev->key == TK_NEWLINE && v->field == F_TITLE))
            replbox_key(&v->box, ev);
        return 0;
    }
    switch (ev->key) {
    case TK_TEXT:
        if (ev->text)
            field_append(v->edit, ev->text);
        break;
    case TK_BACKSPACE:
        if (!v->edit[0] && was == F_TAG && v->ntags) {
            v->ntags--;
            load(v);
        }
        field_backspace(v->edit);
        break;
    case TK_ESCAPE:
        field_end(v);
        return 0;
    case TK_ENTER:
    case TK_NEWLINE:
        field_commit(v);
        return 0;
    case TK_TAB:
        if (was == F_TAG || was == F_RETAG)
            complete(v);
        break;
    case TK_CHAR:
        if (ev->cp == 21)
            v->edit[0] = '\0';
        else if (ev->cp == 3 || ev->cp == 4) {
            field_end(v);
            return 0;
        } else if (ev->cp >= 32) {
            char u[5] = {0};
            text_utf8_encode(ev->cp, u);
            field_append(v->edit, u);
        }
        break;
    default:
        return 0;
    }
    return was == F_QUERY;
}

static void delete_current(struct view *v)
{
    const cJSON *rec = v->rec ? v->rec : cJSON_GetArrayItem(v->records, v->sel);
    if (!rec)
        return;
    char rev[32];
    const cJSON *r = cJSON_GetObjectItem(rec, "revision");
    snprintf(rev, sizeof rev, "%d", cJSON_IsNumber(r) ? r->valueint : 0);
    cJSON *out = run_mem(v, (const char *[]){"delete", str(rec, "id"), "--revision", rev, NULL});
    if (!out)
        return;
    cJSON_Delete(out);
    forget_tags(v);
    if (v->rec)
        close_record(v);
    load(v);
}

static void show_keys(const struct view *v)
{
    if (v->field != F_NONE)
        keyhelp_show("mem \xc2\xb7 input", FIELD_KEYS, COUNT_OF(FIELD_KEYS), KEYHELP_FOOT_F1);
    else if (v->rec)
        keyhelp_show("mem \xc2\xb7 record", RECORD_KEYS, COUNT_OF(RECORD_KEYS), KEYHELP_FOOT_ALL);
    else
        keyhelp_show("mem", LIST_KEYS, COUNT_OF(LIST_KEYS), KEYHELP_FOOT_ALL);
}

static void ask_delete(struct view *v, const cJSON *rec)
{
    char title[200];
    title_of(rec, title, sizeof title);
    snprintf(v->asking, sizeof v->asking, "delete \"%s\"?", title);
    v->ask = v->asking;
}

/* Returns 1 to close the view. */
static int list_key(struct view *v, tty_event *ev)
{
    int    n = cJSON_GetArraySize(v->records);
    cJSON *cur = cJSON_GetArrayItem(v->records, v->sel);
    int    page = v->body > 1 ? v->body - 1 : 1;

    switch (ev->key) {
    case TK_UP:
    case TK_SCROLL_UP:   v->sel--; break;
    case TK_DOWN:
    case TK_SCROLL_DOWN: v->sel++; break;
    case TK_PAGE_UP:     v->sel -= page; break;
    case TK_PAGE_DOWN:   v->sel += page; break;
    case TK_HOME:        v->sel = 0; break;
    case TK_END:         v->sel = n - 1; break;
    case TK_TAB:
    case TK_PREV_TAB:
        v->tab = (enum tab)((v->tab + (ev->key == TK_TAB ? 1 : T_COUNT - 1)) % T_COUNT);
        v->sel = 0;
        load(v);
        break;
    case TK_ENTER:
    case TK_RIGHT:
        if (cur)
            open_record(v, cur);
        break;
    case TK_BACKSPACE:
        if (v->ntags) {
            v->ntags--;
            load(v);
        }
        break;
    case TK_ESCAPE:
    case TK_EOF:
        return 1;
    case TK_CHAR:
        switch (ev->cp) {
        case 'j': v->sel++; break;
        case 'k': v->sel--; break;
        case 'g': v->sel = 0; break;
        case 'G': v->sel = n - 1; break;
        case '1':
        case '2':
        case '3':
            v->tab = (enum tab)(ev->cp - '1');
            v->sel = 0;
            load(v);
            break;
        case 's':
            if (v->nstores > 1) {
                v->store = (v->store + 1) % v->nstores;
                v->ntags = 0;
                v->sel = 0;
                forget_tags(v);
                load(v);
            }
            break;
        case '/':
            if (v->tab != T_SEARCH) {
                v->tab = T_SEARCH;
                v->sel = 0;
                load(v);
            }
            field_begin(v, F_QUERY, v->query);
            break;
        case '#':
            field_begin(v, F_TAG, NULL);
            break;
        case 'n':
        case 'a':
            field_begin(v, F_NEW, NULL);
            break;
        case 'h':
            if (v->tab == T_TODO) {
                v->show_done = !v->show_done;
                load(v);
            }
            break;
        case 'r':
            forget_tags(v);
            load(v);
            break;
        case 'l':
            if (cur)
                open_record(v, cur);
            break;
        case 'x':
        case ' ':
            if (cur && toggle_tag(v, cur, "done"))
                load(v);
            break;
        case '*':
        case 'f':
            if (cur && toggle_tag(v, cur, "starred"))
                load(v);
            break;
        case 'd':
        case 'D':
            if (cur)
                ask_delete(v, cur);
            break;
        case 'p':
            if (cur) {
                to_prompt(v, cur);
                return 1;
            }
            break;
        case 'q':
        case 3:
        case 4:
            return 1;
        }
        break;
    default:
        break;
    }
    if (v->sel >= n)
        v->sel = n - 1;
    if (v->sel < 0)
        v->sel = 0;
    return 0;
}

static int record_key(struct view *v, tty_event *ev)
{
    int page = v->body > 1 ? v->body - 1 : 1;
    switch (ev->key) {
    case TK_UP:          v->dtop--; break;
    case TK_DOWN:        v->dtop++; break;
    case TK_SCROLL_UP:   v->dtop -= 3; break;
    case TK_SCROLL_DOWN: v->dtop += 3; break;
    case TK_PAGE_UP:     v->dtop -= page; break;
    case TK_PAGE_DOWN:   v->dtop += page; break;
    case TK_HOME:        v->dtop = 0; break;
    case TK_END:         v->dtop = v->n; break;
    case TK_RESIZE:      lines_free(v); break;
    case TK_ESCAPE:
    case TK_LEFT:
    case TK_BACKSPACE:
        close_record(v);
        load(v);
        break;
    case TK_EOF:
        return 1;
    case TK_MOUSE_DOWN: {
        int top = viewport_chrome_top();
        int r = ev->row - 1 - (top < 0 ? 0 : top) - 4 + v->dtop - v->n - 1;
        if (r >= 0 && r < v->strip)
            open_images(v, thumb_at(v, ev->col - 1));
        break;
    }
    case TK_CHAR:
        switch (ev->cp) {
        case 'j': v->dtop++; break;
        case 'k': v->dtop--; break;
        case ' ': v->dtop += page; break;
        case 'b': v->dtop -= page; break;
        case 'g': v->dtop = 0; break;
        case 'G': v->dtop = v->n; break;
        case 'e':
            run_editor(v);
            break;
        case 'i':
            open_images(v, 0);
            break;
        case 't':
            field_begin(v, F_TITLE, str(v->rec, "title"));
            break;
        case '#': {
            char names[32][64];
            int  n = tag_names(v->rec, names, 32);
            char buf[FIELD_MAX] = "";
            for (int i = 0; i < n; i++) {
                strcat(buf, names[i]);
                strcat(buf, " ");
            }
            field_begin(v, F_RETAG, buf);
            break;
        }
        case 'x':
            if (toggle_tag(v, v->rec, "done"))
                reopen(v);
            break;
        case '*':
        case 'f':
            if (toggle_tag(v, v->rec, "starred"))
                reopen(v);
            break;
        case 'd':
        case 'D':
            ask_delete(v, v->rec);
            break;
        case 'p':
            to_prompt(v, v->rec);
            return 1;
        case 'h':
        case 'q':
            close_record(v);
            load(v);
            break;
        case 3:
        case 4:
            return 1;
        }
        break;
    default:
        break;
    }
    return 0;
}

int memui_run(void)
{
    if (!frontend_has_keyboard() || !tty_is_raw())
        return 0;

    struct view v = {0};
    load_stores(&v);
    load(&v);

    chrome_full(1);
    chrome_modal(paint, &v);
    chrome_paint();

    for (;;) {
        tty_event ev;
        if (!tty_read(&ev, POLL_MS)) {
            if (chrome_modal_interrupted())
                break;
            workspace_pump_quiet();
            continue;
        }

        int done = 0;
        if (ev.key == TK_RESIZE) {
            lines_free(&v);
        } else if (v.ask) {
            int yn = chrome_read_yesno(&ev);
            if (yn >= 0) {
                v.ask = NULL;
                if (yn)
                    delete_current(&v);
            }
        } else if (ev.key == TK_F1 || (ev.key == TK_CHAR && ev.cp == '?' && v.field == F_NONE)) {
            show_keys(&v);
        } else if (v.field != F_NONE) {
            if (field_key(&v, &ev)) {
                snprintf(v.query, sizeof v.query, "%s", v.edit);
                v.sel = 0;
                load(&v);
            }
        } else if (v.rec) {
            done = record_key(&v, &ev);
        } else {
            done = list_key(&v, &ev);
        }
        free(ev.text);
        if (done)
            break;

        if (tty_input_waiting())
            continue;
        chrome_paint();
    }

    chrome_modal(NULL, NULL);
    chrome_full(0);
    field_end(&v);
    close_record(&v);
    cJSON_Delete(v.res);
    cJSON_Delete(v.alltags);

    viewport_forget();
    viewport_touch();
    viewport_flush();
    return 1;
}
