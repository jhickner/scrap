#include "prompt.h"
#include "keyhelp.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "app.h"
#include "bash.h"
#include "chrome.h"
#include "block.h"
#include "files.h"
#include "scrollback.h"
#include "settings.h"
#include "sidechannel.h"
#include "status.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"
#include "vendor/cJSON.h"
#include "replframe.h"
#include "replkeys.h"
#include "text.h"
#include "terminalrun.h"

#define EDIT_MAX_BYTES (1u << 22)

struct prompt {
    Repl         repl;
    struct replframe frame;
    int          painted_cols;
    char        *history_path;
    prompt_live_fn live_command;
    void          *live_ud;
    int          (*echo_filter)(void *ud, const char *line);
    void          *echo_ud;
    char       **queued;
    int          queued_count;
    int          queued_cap;
    char        *queued_open;
    int        (*q_count)(void *ud);
    const char *(*q_at)(void *ud, int i);
    char      *(*q_take)(void *ud);
    void        *q_ud;
    char        *file_root;
    char      *(*external)(void *ud);
    void        *external_ud;
    int          external_taken;
    int        (*idle_fds)(void *ud, int *out, int max);
    int        (*idle_render)(void *ud);
    int        (*idle_poll)(void *ud);
    void        *idle_ud;
    void       (*replay)(void *ud);
    void        *replay_ud;
    void       (*blank)(void *ud);
    void        *blank_ud;
    int        (*animate_busy)(void *ud);
    void       (*animate_tick)(void *ud);
    void        *animate_ud;
    int        (*restart_pending)(void *ud);
    int        (*restart)(void *ud);
    void        *restart_ud;
    int        (*takeover_pending)(void *ud);
    void       (*takeover)(void *ud);
    void        *takeover_ud;
    void       (*switcher)(void *ud);
    void        *switcher_ud;
    int        (*click)(void *ud, int row, int col);
    void        *click_ud;
    char      *(*command)(void *ud, int nth);
    void        *command_ud;
    int          command_nth;
    void       (*split)(void *ud, int quiet);
    void        *split_ud;
    void       (*another)(void *ud);
    void        *another_ud;
    void       (*step)(void *ud, int dir);
    void        *step_ud;
    int        (*busy)(void *ud);
    void        *busy_ud;
    void       (*cycle)(void *ud, int delta);
    void        *cycle_ud;
    void       (*history_follow)(void);
    void       (*collapse)(void *ud);
    void        *collapse_ud;
    int        (*cancel)(void *ud);
    void        *cancel_ud;
    int          stopped;
    int          frame_ok;
};

static int prompt_echoes(struct prompt *p, const char *line);

static struct prompt *active;

static void history_put(FILE *f, const char *line)
{
    for (const char *q = line; *q; q++)
        fputc(*q == '\n' ? ' ' : *q, f);
    fputc('\n', f);
}

static void history_append(struct prompt *p, const char *line)
{
    if (!p->history_path || !*line)
        return;
    FILE *f = fopen(p->history_path, "a");
    if (!f)
        return;
    history_put(f, line);
    fclose(f);
}

static void history_rewrite(struct prompt *p)
{
    char tmp[4200];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", p->history_path) >= sizeof tmp)
        return;
    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    for (int i = 0; i < p->repl.hist_count; i++)
        history_put(f, p->repl.history[i]);
    if (fclose(f) != 0 || rename(tmp, p->history_path) != 0)
        unlink(tmp);
}

void prompt_history_open(const char *path)
{
    struct prompt *p = active;
    if (!p)
        return;

    Repl *r = &p->repl;
    for (int i = 0; i < r->hist_count; i++)
        free(r->history[i]);
    r->hist_count = 0;
    r->hist_pos = -1;
    r->searching = false;
    r->search_idx = -1;
    free(p->history_path);
    p->history_path = path ? strdup(path) : NULL;
    if (!p->history_path)
        return;

    FILE *f = fopen(path, "r");
    if (!f)
        return;
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int lines = 0;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n')
            line[n - 1] = '\0';
        if (*line)
            repl_history_add(r, line);
        lines++;
    }
    free(line);
    fclose(f);
    if (lines > REPL_HISTORY_MAX)
        history_rewrite(p);
}

static void put_codepoint(uint32_t cp)
{
    char buf[4];
    ui_putn(buf, text_utf8_encode(cp, buf));
}

static size_t queued_budget(int cols)
{
    return (size_t)(cols - 2 > 4 ? cols - 2 : 4);
}

static struct ui_wrap bar_wrap(size_t budget, enum ui_role role, int cap,
                               const char *mark)
{
    struct ui_wrap w = {0};

    if (cap > 0 && budget > 1)
        budget--;
    w.budget = budget;
    w.gutter = UI_BAR " ";
    w.mark = mark;
    w.role = role;
    w.max_rows = cap;
    w.erase = 1;
    w.paint_empty = 1;
    return w;
}

static int painted_rows(const char *text, size_t budget, int cap, const char *mark)
{
    struct ui_wrap w = bar_wrap(budget, UI_RESET, cap, mark);
    w.measure = 1;
    return ui_wrap_paint(text, &w);
}

static int caret_is_synthetic(const Repl *r)
{
    return r->cursor >= r->len || r->buf[r->cursor] == '\n';
}

static void paint_bars(const char *text, size_t budget, enum ui_role role, int cap,
                       const char *mark)
{
    struct ui_wrap w = bar_wrap(budget, role, cap, mark);
    ui_wrap_paint(text, &w);
}

void prompt_set_queued_source(struct prompt *p, int (*count)(void *ud),
                              const char *(*at)(void *ud, int i),
                              char *(*take_last)(void *ud), void *ud)
{
    if (!p)
        return;
    p->q_count = count;
    p->q_at = at;
    p->q_take = take_last;
    p->q_ud = ud;
}

static int queued_total(struct prompt *p)
{
    return (p->q_count ? p->q_count(p->q_ud) : 0) + p->queued_count;
}

static const char *queued_line(struct prompt *p, int i)
{
    int theirs = p->q_count ? p->q_count(p->q_ud) : 0;
    return i < theirs ? p->q_at(p->q_ud, i) : p->queued[i - theirs];
}

static int queued_cap(struct prompt *p, const char *line)
{
    if (!p->queued_open || strcmp(p->queued_open, line))
        return QUEUED_LINES;
    int cap = tty_rows() / 2;
    return cap > QUEUED_LINES ? cap : QUEUED_LINES;
}

static int queued_click(struct prompt *p, int row, int col)
{
    char *url = viewport_link_at(row, col);
    int index = -1;
    if (url)
        sscanf(url, "scrap-queue:%d", &index);
    free(url);
    if (index < 0 || index >= queued_total(p))
        return 0;
    const char *line = queued_line(p, index);
    int open = p->queued_open && !strcmp(p->queued_open, line);
    free(p->queued_open);
    p->queued_open = open ? NULL : strdup(line);
    chrome_paint();
    return 1;
}

int prompt_queued_rows(struct prompt *p, int cols)
{
    int n = p ? queued_total(p) : 0;
    if (n == 0)
        return 0;
    size_t budget = queued_budget(cols);
    int rows = n - 1;
    for (int i = 0; i < n; i++)
        rows += painted_rows(queued_line(p, i), budget, queued_cap(p, queued_line(p, i)), NULL);
    return rows;
}

void prompt_paint_queued(struct prompt *p, int room)
{
    int n = p ? queued_total(p) : 0;
    if (n == 0)
        return;
    size_t budget = queued_budget(ui_columns());
    int used = 0;
    for (int i = 0; i < n; i++) {
        const char *line = queued_line(p, i);
        int need = painted_rows(line, budget, queued_cap(p, line), NULL);
        if (i)
            need++;
        if (used + need > room)
            break;
        used += need;
        if (i)
            ui_put("\n");
        int folds = painted_rows(line, budget, QUEUED_LINES + 1, NULL) > QUEUED_LINES;
        if (folds) {
            char link[64];
            snprintf(link, sizeof link, "\x1b]8;;scrap-queue:%d\x1b\\", i);
            ui_esc(link);
        }
        paint_bars(line, budget, UI_DIM, queued_cap(p, line), NULL);
        if (folds)
            ui_esc("\x1b]8;;\x1b\\");
    }
}

static void emit_input(struct prompt *p, int rows)
{
    int synthetic = caret_is_synthetic(&p->repl);
    int focused = tty_focused();

    for (int y = 0; y < rows; y++) {
        ui_esc(UI_ERASE_EOL);
        int extent = replframe_extent(&p->frame, y);
        const char *open = "";
        for (int x = 0; x < extent; x++) {
            const struct replframe_cell *c = replframe_at(&p->frame, y, x);
            uint32_t cp = c->cp;
            const char *seq = replframe_style(c->style);
            int caret = c->style == REPL_STYLE_CURSOR && p->frame.have_cursor &&
                        p->frame.cursor_x == x && p->frame.cursor_y == y && focused;

            if (c->style == REPL_STYLE_PROMPT && cp == '*') {
                cp = 0x23FA;
                seq = ui_style(UI_ERROR);
            } else if (c->style == REPL_STYLE_PROMPT && cp == '>') {
                cp = 0x276F;
                seq = ui_style(UI_ACCENT);
            }
            if (c->style == REPL_STYLE_CURSOR && cp == '_' && synthetic &&
                p->frame.cursor_x == x && p->frame.cursor_y == y)
                cp = ' ';
            if (seq != open) {
                ui_esc(ui_style(UI_RESET));
                ui_esc(seq);
                open = seq;
            }

            if (caret)
                ui_esc("\x1b[7m");
            put_codepoint(cp);
            if (caret)
                ui_esc("\x1b[27m");
        }
        if (*open)
            ui_esc(ui_style(UI_RESET));
        if (y + 1 < rows)
            ui_put("\n");
    }
}

int prompt_input_rows(struct prompt *p, int cols)
{
    if (!p)
        return 1;
    if (p->frame_ok && cols == p->painted_cols && p->frame.cells && p->frame.rows > 0)
        return p->frame.rows;

    int rows = repl_input_rows(&p->repl, cols) + repl_dropdown_rows(&p->repl);
    if (rows < 1)
        rows = 1;

    p->painted_cols = cols;
    if (!replframe_render(&p->frame, &p->repl, rows, cols, 1)) {
        p->frame_ok = 0;
        return 1;
    }
    p->frame_ok = 1;
    return rows;
}

void prompt_paint_input(struct prompt *p, int rows, int *caret_row, int *caret_col)
{
    *caret_row = 0;
    *caret_col = 0;
    if (!p || !p->frame_ok) {
        ui_put("");
        return;
    }
    emit_input(p, rows);
    *caret_row = p->frame.have_cursor ? p->frame.cursor_y : rows - 1;
    *caret_col = p->frame.have_cursor ? p->frame.cursor_x : 0;
}

static void repaint(struct prompt *p)
{
    (void)p;
    chrome_paint();
}

struct echo_item {
    char        *text;
    enum ui_role role;
    int          cap;
    int          gap;
};

static void echo_paint(const struct echo_item *e)
{
    size_t budget = queued_budget(ui_columns());
    int    open = viewport_fold_open();
    int    folds = e->cap > 0 && painted_rows(e->text, budget, e->cap + 1, NULL) > e->cap;
    struct ui_wrap w = bar_wrap(budget, e->role, open ? 0 : e->cap, NULL);
    ui_wrap_paint(e->text, &w);
    if (folds)
        viewport_fold_enable();
}

static int echo_pad_after(const struct echo_item *e)
{
    return e->role != UI_BASH;
}

static char *echo_encode(void *ud)
{
    const struct echo_item *e = ud;
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "text", e->text ? e->text : "");
    cJSON_AddNumberToObject(o, "role", e->role);
    cJSON_AddNumberToObject(o, "cap", e->cap);
    cJSON_AddNumberToObject(o, "gap", e->gap);
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return out;
}

static void echo_render(void *ud, int cols)
{
    (void)cols;
    echo_paint(ud);
}

static void echo_free(void *ud)
{
    struct echo_item *e = ud;
    free(e->text);
    free(e);
}

static enum ui_role echo_role(const char *text)
{
    if (bash_is_command(text))
        return UI_BASH;
    return text && !strncmp(text, "from @", 6) ? UI_SIDE : UI_ECHO;
}

void prompt_echo_message(const char *text)
{
    int cap = settings_get_int(SETTING_ECHO_ROWS, ECHO_ROWS_DEFAULT);

    struct echo_item *e = malloc(sizeof *e);
    if (e) {
        e->text = strdup(text ? text : "");
        e->role = echo_role(text);
        e->cap = cap > 0 ? cap : 0;
        e->gap = 1;
        if (!e->text) {
            free(e);
            e = NULL;
        }
    }

    if (e) {
        unsigned mark = viewport_item_begin(&(struct viewport_entry){
            .render = echo_render, .ud = e, .free_ud = echo_free, .reflow = 1,
            .pad_before = e->gap, .pad_after = echo_pad_after(e)});
        echo_paint(e);
        viewport_item_end();
        viewport_item_persist(mark, PROMPT_ECHO_KIND, echo_encode);
    } else {
        struct echo_item fallback = {(char *)(text ? text : ""),
                                     echo_role(text),
                                     cap > 0 ? cap : 0, 1};
        echo_paint(&fallback);
    }
    ui_flush();
}

void prompt_echo_load(const cJSON *st)
{
    struct echo_item *e = malloc(sizeof *e);
    if (!e)
        return;
    e->text = strdup(scrollback_str(st, "text"));
    e->role = (enum ui_role)scrollback_int(st, "role");
    e->cap = scrollback_int(st, "cap");
    e->gap = scrollback_int(st, "gap");
    if (!e->text) {
        free(e);
        return;
    }
    unsigned mark = viewport_item_begin(&(struct viewport_entry){
        .render = echo_render, .ud = e, .free_ud = echo_free, .reflow = 1,
        .pad_before = e->gap, .pad_after = echo_pad_after(e)});
    echo_paint(e);
    viewport_item_end();
    viewport_item_persist(mark, PROMPT_ECHO_KIND, echo_encode);
}

static struct prompt *completion_owner;
static ReplCompleter  name_completer;

struct prompt *prompt_new(const ReplCommand *commands, int command_count)
{
    struct prompt *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    repl_init(&p->repl, commands, command_count);

    p->repl.suggest_off = true;
    active = p;
    return p;
}

void prompt_set_name_completer(ReplCompleter fn)
{
    name_completer = fn;
}

void prompt_file_completion(struct prompt *p, const char *root)
{
    completion_owner = p;
    free(p->file_root);
    p->file_root = strdup(root);
    if (p->file_root)
        repl_set_completer(&p->repl, files_complete, p->file_root);
    repl_set_name_completer(&p->repl, name_completer, NULL);
    files_prefetch(p->file_root);
}

void prompt_rehome(const char *root)
{
    if (completion_owner && root && *root)
        prompt_file_completion(completion_owner, root);
}

void prompt_free(struct prompt *p)
{
    if (!p)
        return;
    if (active == p)
        active = NULL;
    if (completion_owner == p)
        completion_owner = NULL;
    repl_free(&p->repl);
    files_forget();
    replframe_free(&p->frame);
    free(p->file_root);
    free(p->history_path);
    for (int i = 0; i < p->queued_count; i++)
        free(p->queued[i]);
    free(p->queued);
    free(p->queued_open);
    free(p);
}

static int feed_event(struct prompt *p, const ReplEvent *ev)
{
    repl_set_width(&p->repl, ui_columns());
    p->frame_ok = 0;
    return repl_handle_input(&p->repl, ev);
}

static int feed(struct prompt *p, ReplKey key, uint32_t cp, const char *text)
{
    ReplEvent ev = {.key = key, .codepoint = cp, .text = text};
    return feed_event(p, &ev);
}

static void paste_clipboard(struct prompt *p, int live)
{
    p->frame_ok = 0;
    if (replkeys_paste(&p->repl))
        return;
    if (live)
        return;
    ui_note("clipboard is empty");
    repaint(p);
}

static int editor_temp(char *out, size_t size)
{
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir)
        dir = "/tmp";
    if (strchr(dir, '\''))
        return 0;
    if ((size_t)snprintf(out, size, "%s/" APP_NAME "-XXXXXX", dir) >= size)
        return 0;
    int fd = mkstemp(out);
    if (fd < 0)
        return 0;
    close(fd);
    return 1;
}

static char *read_whole(const char *path)
{
    size_t got = 0;
    char *buf = text_slurp(path, EDIT_MAX_BYTES, &got);
    if (!buf)
        return NULL;

    if (got && buf[got - 1] == '\n')
        buf[--got] = '\0';
    if (got && buf[got - 1] == '\r')
        buf[--got] = '\0';
    return buf;
}

static void edit_in_editor(struct prompt *p, int live)
{
    if (p->repl.searching)
        feed(p, REPL_KEY_ENTER, 0, NULL);

    const char *editor = getenv("VISUAL");
    if (!editor || !*editor)
        editor = getenv("EDITOR");
    if (!editor || !*editor)
        editor = "vi";

    char path[256];
    if (!editor_temp(path, sizeof path)) {
        if (!live) {
            ui_note("could not create a temp file for $EDITOR");
            repaint(p);
        }
        return;
    }

    FILE *f = fopen(path, "w");
    if (!f) {
        unlink(path);
        if (!live) {
            ui_note("could not write %s", path);
            repaint(p);
        }
        return;
    }
    const char *line = repl_line(&p->repl);
    int wrote = 1;
    if (line && *line)
        wrote = fputs(line, f) >= 0;
    if (fclose(f) != 0)
        wrote = 0;
    if (!wrote) {
        unlink(path);
        if (!live) {
            ui_note("could not write %s", path);
            repaint(p);
        }
        return;
    }

    char quoted[sizeof path * 4 + 3];
    char cmd[4096];
    int  len = text_shell_quote(path, quoted, sizeof quoted)
                   ? snprintf(cmd, sizeof cmd, "%s %s", editor, quoted)
                   : -1;
    if (len < 0 || (size_t)len >= sizeof cmd) {
        unlink(path);
        if (!live) {
            ui_note("$EDITOR command is too long");
            repaint(p);
        }
        return;
    }

    if (live)
        status_pause();
    else
        chrome_clear();
    int status = terminal_run_external(cmd);

    int ok = status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (ok) {
        char *text = read_whole(path);
        if (text) {
            p->frame_ok = 0;
            repl_reset(&p->repl);
            if (*text)
                repl_insert_text(&p->repl, text);
            free(text);
        } else {
            ok = 0;
        }
    }
    unlink(path);

    if (live)
        status_resume();
    else if (!ok)
        ui_note("$EDITOR failed");
}

enum key_result {
    KEY_OK,
    KEY_SUBMIT,
    KEY_EOF,
    KEY_CANCEL,
};

#define KEY_CTRL(c) ((c) - 'A' + 1)

static const struct prompt_key SHORTCUTS[] = {
    {"SESSIONS", "tab", "sessions list",
     "accept the completion, else open /sessions", PROMPT_KEY_ALWAYS},
    {"SESSIONS", "left", "sessions list (empty line)",
     "on an empty line, open /sessions", PROMPT_KEY_ALWAYS},
    {"SESSIONS", "ctrl-o/p", "back / forward",
     "step back / forward through recently active sessions", PROMPT_KEY_ALWAYS},
    {"SESSIONS", "ctrl-tab", "next tab, +shift previous",
     "cycle to the next / previous session", PROMPT_KEY_ALWAYS},
    {"SESSIONS", "ctrl-n", "new session",
     "open a new session with this one's backend, model, effort, and directory", PROMPT_KEY_ALWAYS},
    {"SESSIONS", "ctrl-t", "shell split",
     "open a shell split in this directory", PROMPT_KEY_ALWAYS},
    {"SESSIONS", "ctrl-d", "close (empty line)",
     "on an empty line, close the session (quit on the last one)", PROMPT_KEY_ALWAYS},
    {"EDIT", "ctrl-a/e", "line start / end",
     "jump to the start / end of the line", PROMPT_KEY_ALWAYS},
    {"EDIT", "ctrl-w", "delete word",
     "delete the word before the cursor", PROMPT_KEY_ALWAYS},
    {"EDIT", "ctrl-u/k", "delete to start / end",
     "delete to the start / end of the line", PROMPT_KEY_ALWAYS},
    {"EDIT", "ctrl-y", "paste deleted text",
     "paste back the last deleted text", PROMPT_KEY_ALWAYS},
    {"EDIT", "ctrl-_", "undo",
     "undo the last edit", PROMPT_KEY_ALWAYS},
    {"EDIT", "ctrl-j", "newline",
     "insert a newline", PROMPT_KEY_ALWAYS},
    {"EDIT", "ctrl-g", "edit in $EDITOR",
     "edit the prompt in $EDITOR", PROMPT_KEY_ALWAYS},
    {"EDIT", "ctrl-v", "paste text or image",
     "paste text, or a clipboard image as a file path", PROMPT_KEY_ALWAYS},
    {"EDIT", "ctrl-r", "search history",
     "search history", PROMPT_KEY_ALWAYS},
    {"EDIT", "up/down", "history",
     "move through the completion list, else browse history", PROMPT_KEY_ALWAYS},
    {"EDIT", "#", "complete a path",
     "complete a file path from the working directory", PROMPT_KEY_ALWAYS},
    {"EDIT", "@", "complete a session name",
     "complete a live session name", PROMPT_KEY_ALWAYS},
    {"VIEW", "ctrl-f", "compact tool calls",
     "compact or full tool calls, redrawing the transcript", PROMPT_KEY_ALWAYS},
    {"VIEW", "pgup/dn", "scroll transcript",
     "scroll the transcript half a screen", PROMPT_KEY_ALWAYS},
    {"VIEW", "ctrl-l", "clear screen",
     "clear the screen", PROMPT_KEY_ALWAYS},
    {"VIEW", "click img", "full size",
     "open an image at full size, \xe2\x86\x90\xe2\x86\x92 to step", PROMPT_KEY_ALWAYS},
    {"VIEW", "? / F1", "this help",
     "on an empty line, show the keys for the current view", PROMPT_KEY_ALWAYS},
    {"INPUT", "enter", "send",
     "submit the prompt", PROMPT_KEY_IDLE},
    {"INPUT", "enter", "status bar (empty line)",
     "on an empty line, reprint the status bar", PROMPT_KEY_IDLE},
    {"INPUT", "!cmd", "run in $SHELL",
     "run cmd in $SHELL; ctrl-x pastes it and its output until the next turn", PROMPT_KEY_ALWAYS},
    {"INPUT", "ctrl-x", "paste last !cmd output",
     "paste the last !cmd and its output into the prompt", PROMPT_KEY_ALWAYS},
    {"INPUT", "ctrl-]", "command from last reply",
     "put the last command in the last reply on the line, again for earlier ones",
     PROMPT_KEY_ALWAYS},
    {"INPUT", "esc", "close / stop speech",
     "close the completion, else stop the reply being read aloud", PROMPT_KEY_IDLE},
    {"INPUT", "ctrl-c", "clear the line",
     "clear the prompt line", PROMPT_KEY_IDLE},
    {"TURN", "enter", "queue the line",
     "queue the prompt until the running turn ends", PROMPT_KEY_TURN},
    {"TURN", "esc", "interrupt",
     "close the completion, else stop the reply being read aloud, else interrupt the model", PROMPT_KEY_TURN},
    {"TURN", "ctrl-c", "clear, else interrupt",
     "clear the prompt line, else interrupt the running turn", PROMPT_KEY_TURN},
};

static const struct keyhelp_row COMPLETION_KEYS[] = {
    {"COMPLETION", "up/down", "move"},
    {"COMPLETION", "tab", "accept"},
    {"COMPLETION", "enter", "accept"},
    {"COMPLETION", "esc", "close"},
};

static const struct keyhelp_row SEARCH_KEYS[] = {
    {"HISTORY SEARCH", "type", "narrow the match"},
    {"HISTORY SEARCH", "ctrl-r", "older match"},
    {"HISTORY SEARCH", "enter", "put it on the line"},
    {"HISTORY SEARCH", "arrows", "accept and move"},
    {"HISTORY SEARCH", "esc", "cancel"},
};

#define COUNT_OF(a) ((int)(sizeof(a) / sizeof *(a)))

static void show_keys(struct prompt *p, int live)
{
    struct keyhelp_row rows[COUNT_OF(SHORTCUTS)];
    const struct keyhelp_row *shown = rows;
    const char *title = "prompt";
    const char *foot = KEYHELP_FOOT_ALL;
    int n = 0;

    if (p->repl.searching) {
        shown = SEARCH_KEYS;
        n = COUNT_OF(SEARCH_KEYS);
        title = "history search";
        foot = KEYHELP_FOOT_F1;
    } else if (p->repl.dropdown_open) {
        shown = COMPLETION_KEYS;
        n = COUNT_OF(COMPLETION_KEYS);
        title = "completion";
        foot = KEYHELP_FOOT_F1;
    } else {
        int turn = live || (p->busy && p->busy(p->busy_ud));
        enum prompt_key_when skip = turn ? PROMPT_KEY_IDLE : PROMPT_KEY_TURN;
        for (int i = 0; i < COUNT_OF(SHORTCUTS); i++)
            if (SHORTCUTS[i].when != skip)
                rows[n++] = (struct keyhelp_row){SHORTCUTS[i].group, SHORTCUTS[i].key,
                                                 SHORTCUTS[i].brief};
        if (turn)
            title = "prompt \xc2\xb7 turn running";
    }

    keyhelp_show(title, shown, n, foot);
}

static int overlay_open(const struct prompt *p)
{
    return p->repl.dropdown_open || p->repl.searching;
}

static int recall_queued(struct prompt *p);

static enum key_result edit_key(struct prompt *p, tty_event *ev)
{
    ReplEvent re;
    if (replkeys_map(ev, &re))
        feed_event(p, &re);
    free(ev->text);
    ev->text = NULL;
    return KEY_OK;
}

static char *held_prompt;
static int   held_keep;

void prompt_hold(char *text)
{
    free(held_prompt);
    held_prompt = text;
    held_keep = 1;
}

void prompt_drop_held(void)
{
    if (held_keep) {
        held_keep = 0;
        return;
    }
    free(held_prompt);
    held_prompt = NULL;
}

static char *take_prompt_held(void)
{
    char *out = held_prompt;
    held_prompt = NULL;
    held_keep = 0;
    return out;
}

const char *prompt_held_label(void)
{
    static char label[192];
    if (!held_prompt)
        return NULL;
    size_t first = strcspn(held_prompt, "\n");
    size_t shown = first < 120 ? first : 120;
    snprintf(label, sizeof label, "queued: %.*s%s \xc2\xb7 ctrl-x paste", (int)shown,
             held_prompt, held_prompt[shown] ? " \xe2\x80\xa6" : "");
    return label;
}

static void paste_held(struct prompt *p)
{
    char *text = bash_take_held();
    if (!text)
        text = take_prompt_held();
    if (!text)
        return;
    if (p->repl.cursor > 0 && p->repl.buf[p->repl.cursor - 1] != '\n')
        repl_insert_text(&p->repl, "\n");
    repl_insert_text(&p->repl, text);
    repl_insert_text(&p->repl, "\n");
    free(text);
    p->frame_ok = 0;
    chrome_clear();
    if (!chrome_modal_active())
        repaint(p);
}

static enum key_result feed_key(struct prompt *p, tty_event *ev, int live)
{
    if (p->history_follow)
        p->history_follow();
    if (ev->key != TK_CHAR || ev->cp != KEY_CTRL(']'))
        p->command_nth = 0;
    switch (ev->key) {
    case TK_EOF:
        return KEY_EOF;

    case TK_RESIZE:
        return KEY_OK;

    case TK_TEXT:
        return edit_key(p, ev);

    case TK_CHAR:
        if (ev->cp == KEY_CTRL('D')) {
            if (p->repl.len == 0 && !overlay_open(p))
                return KEY_EOF;
            feed(p, REPL_KEY_DELETE, 0, NULL);
            return KEY_OK;
        }
        if (ev->cp == KEY_CTRL('C') && p->repl.len && !overlay_open(p)) {
            return edit_key(p, ev);
        }
        if (ev->cp == KEY_CTRL('C') && p->repl.len == 0 && !overlay_open(p)) {
            if (live)
                return KEY_CANCEL;

            if (p->cancel && p->cancel(p->cancel_ud))
                return KEY_OK;
        }
        if (ev->cp == KEY_CTRL('V')) {
            paste_clipboard(p, live);
            return KEY_OK;
        }
        if (ev->cp == KEY_CTRL('G')) {
            edit_in_editor(p, live);
            return KEY_OK;
        }
        if (ev->cp == KEY_CTRL('X')) {
            paste_held(p);
            return KEY_OK;
        }
        if (ev->cp == KEY_CTRL(']')) {
            char *cmd = p->command ? p->command(p->command_ud, p->command_nth) : NULL;
            if (!cmd && p->command_nth) {
                p->command_nth = 0;
                cmd = p->command(p->command_ud, 0);
            }
            if (cmd) {
                prompt_set_line(p, cmd);
                p->command_nth++;
                free(cmd);
            }
            return KEY_OK;
        }

        if (ev->cp == KEY_CTRL('T')) {
            if (p->split)
                p->split(p->split_ud, live);
            if (!live)
                repaint(p);
            return KEY_OK;
        }

        if (ev->cp == KEY_CTRL('N')) {
            if (p->another) {
                if (live)
                    status_pause();
                viewport_defer();
                chrome_clear();
                p->another(p->another_ud);
                if (live)
                    status_resume();
            }
            return KEY_OK;
        }
        if (ev->cp == KEY_CTRL('F')) {
            if (p->collapse) {
                p->collapse(p->collapse_ud);
                if (!live)
                    repaint(p);
            }
            return KEY_OK;
        }
        if (ev->cp == '?' && p->repl.len == 0 && !overlay_open(p)) {
            show_keys(p, live);
            return KEY_OK;
        }
        if (ev->cp == KEY_CTRL('O') || ev->cp == KEY_CTRL('P')) {
            if (p->step && !overlay_open(p)) {
                if (live)
                    status_pause();
                viewport_defer();
                chrome_clear();
                p->step(p->step_ud, ev->cp == KEY_CTRL('O') ? -1 : 1);
                if (live)
                    status_resume();
            }
            return KEY_OK;
        }
        if (ev->cp == KEY_CTRL('L')) {

            viewport_forget();
            repaint(p);
            if (live)
                return KEY_OK;
            status_sticky_erased();
            block_forget();
            viewport_clear_screen();
            return KEY_OK;
        }
        return edit_key(p, ev);

    case TK_NEXT_TAB:
    case TK_PREV_TAB:
        if (p->cycle) {
            if (live)
                status_pause();
            viewport_defer();
            chrome_clear();
            p->cycle(p->cycle_ud, ev->key == TK_NEXT_TAB ? 1 : -1);
            if (live)
                status_resume();
        }
        return KEY_OK;

    case TK_F1:
        show_keys(p, live);
        return KEY_OK;

    case TK_TAB:

        if (repl_accept_completion(&p->repl) || repl_open_completion(&p->repl))
            return KEY_OK;
        if (repl_suggestion(&p->repl)) {
            feed(p, REPL_KEY_RIGHT, 0, NULL);
            return KEY_OK;
        }

        if (p->switcher && p->repl.len == 0 && !overlay_open(p)) {
            if (live)
                status_pause();
            viewport_defer();
            chrome_clear();
            p->switcher(p->switcher_ud);
            if (live)
                status_resume();
        }
        return KEY_OK;

    case TK_ESCAPE:

        if (!overlay_open(p) && p->repl.len == 0 && p->cancel && p->cancel(p->cancel_ud))
            return KEY_OK;
        if (live && !overlay_open(p))
            return KEY_CANCEL;
        feed(p, REPL_KEY_ESCAPE, 0, NULL);
        return KEY_OK;

    case TK_ENTER: {
        if (!overlay_open(p) && tty_input_waiting()) {
            feed(p, REPL_KEY_NEWLINE, 0, NULL);
            return KEY_OK;
        }
        if (p->blank && p->repl.len == 0 && !overlay_open(p)) {
            if (live)
                status_pause();
            viewport_defer();
            chrome_clear();
            p->blank(p->blank_ud);
            if (live)
                status_resume();
            return KEY_OK;
        }
        if (feed(p, REPL_KEY_ENTER, 0, NULL) != REPL_SUBMIT)
            return KEY_OK;
        const char *line = repl_line(&p->repl);
        if (!line || !*line)
            return KEY_OK;

        viewport_scroll_end();
        return KEY_SUBMIT;
    }

    case TK_PAGE_UP:
        viewport_scroll(tty_rows() / 2);
        return KEY_OK;

    case TK_PAGE_DOWN:
        viewport_scroll(-(tty_rows() / 2));
        return KEY_OK;

    case TK_MOUSE_DOWN:
        if (!queued_click(p, ev->row, ev->col) && p->click)
            p->click(p->click_ud, ev->row, ev->col);
        return KEY_OK;

    case TK_SCROLL_UP:
        viewport_scroll(3);
        return KEY_OK;

    case TK_SCROLL_DOWN:
        viewport_scroll(-3);
        return KEY_OK;

    default: {
        if (ev->key == TK_LEFT && !live && p->switcher && p->repl.len == 0 &&
            !overlay_open(p)) {
            viewport_defer();
            chrome_clear();
            p->switcher(p->switcher_ud);
            return KEY_OK;
        }

        if (ev->key == TK_UP && p->repl.len == 0 && !overlay_open(p) &&
            recall_queued(p))
            return KEY_OK;

        return edit_key(p, ev);
    }
    }
}

static void record_line(struct prompt *p, const char *line)
{
    if (!line || !*line)
        return;
    repl_history_add(&p->repl, line);
    history_append(p, line);
}

static char *take_line(struct prompt *p)
{
    const char *line = repl_line(&p->repl);
    char *out = line && *line ? strdup(line) : NULL;
    p->frame_ok = 0;
    if (out)
        record_line(p, out);
    repl_reset(&p->repl);
    return out;
}

void prompt_set_idle(struct prompt *p, int (*fds)(void *ud, int *out, int max),
                     int (*render)(void *ud), int (*poll)(void *ud), void *ud)
{
    p->idle_fds = fds;
    p->idle_render = render;
    p->idle_poll = poll;
    p->idle_ud = ud;
}

void prompt_set_restart(struct prompt *p, int (*pending)(void *ud), int (*run)(void *ud),
                        void *ud)
{
    p->restart_pending = pending;
    p->restart = run;
    p->restart_ud = ud;
}

static int idle_fds_hook(void *ud, int *out, int max)
{
    struct prompt *p = ud;
    return p && p->idle_fds ? p->idle_fds(p->idle_ud, out, max) : 0;
}

static void idle_ready_hook(void *ud)
{
    struct prompt *p = ud;
    if (!p || !p->idle_render)
        return;
    p->idle_render(p->idle_ud);
    if (!chrome_modal_active())
        repaint(p);
}

static void restart_check(struct prompt *p)
{
    if (!p->restart || p->repl.len || p->queued_count)
        return;
    if (!p->restart_pending(p->restart_ud))
        return;
    p->restart(p->restart_ud);
    repaint(p);
}

void prompt_set_takeover(struct prompt *p, int (*pending)(void *ud), void (*run)(void *ud),
                         void *ud)
{
    p->takeover_pending = pending;
    p->takeover = run;
    p->takeover_ud = ud;
}

void prompt_set_cancel(struct prompt *p, int (*fn)(void *ud), void *ud)
{
    p->cancel = fn;
    p->cancel_ud = ud;
}

void prompt_set_click(struct prompt *p, int (*fn)(void *ud, int row, int col),
                      void *ud)
{
    p->click = fn;
    p->click_ud = ud;
}

void prompt_set_switcher(struct prompt *p, void (*fn)(void *ud), void *ud)
{
    p->switcher = fn;
    p->switcher_ud = ud;
}

void prompt_set_another(struct prompt *p, void (*fn)(void *ud), void *ud)
{
    p->another = fn;
    p->another_ud = ud;
}

void prompt_set_step(struct prompt *p, void (*fn)(void *ud, int dir), void *ud)
{
    p->step = fn;
    p->step_ud = ud;
}

void prompt_set_busy(struct prompt *p, int (*fn)(void *ud), void *ud)
{
    p->busy = fn;
    p->busy_ud = ud;
}

void prompt_set_history_follow(struct prompt *p, void (*fn)(void))
{
    p->history_follow = fn;
}

void prompt_set_cycle(struct prompt *p, void (*fn)(void *ud, int delta), void *ud)
{
    p->cycle = fn;
    p->cycle_ud = ud;
}

void prompt_set_collapse(struct prompt *p, void (*fn)(void *ud), void *ud)
{
    p->collapse = fn;
    p->collapse_ud = ud;
}

void prompt_set_split(struct prompt *p, void (*fn)(void *ud, int quiet), void *ud)
{
    p->split = fn;
    p->split_ud = ud;
}

void prompt_stop(struct prompt *p)
{
    if (p)
        p->stopped = 1;
}

static void takeover_check(struct prompt *p)
{
    if (!p->takeover || !p->takeover_pending || !p->takeover_pending(p->takeover_ud))
        return;
    chrome_clear();
    ui_flush();
    p->takeover(p->takeover_ud);
    repaint(p);
}

void prompt_restart_check(struct prompt *p)
{
    if (p)
        restart_check(p);
}

static void queue_push(struct prompt *p, char *line);

static char *read_loop(struct prompt *p)
{
    repaint(p);
    takeover_check(p);
    restart_check(p);

    int resizing = 0;
    for (;;) {
        tty_event ev;

        if (p->stopped) {
            p->stopped = 0;
            chrome_clear();
            return NULL;
        }

        if (p->external) {
            char *line = p->external(p->external_ud);
            if (line) {
                const char *composing = repl_line(&p->repl);
                if (composing && *composing) {
                    queue_push(p, line);
                    repaint(p);
                    continue;
                }
                p->external_taken = 1;
                record_line(p, line);
                chrome_clear();
                if (prompt_echoes(p, line))
                    prompt_echo_message(line);
                return line;
            }
        }

        int animating = !resizing && p->animate_busy && p->animate_busy(p->animate_ud);

        int polling = !resizing && p->idle_poll && p->idle_poll(p->idle_ud);
        int wait = resizing      ? TTY_RESIZE_SETTLE_MS
                   : animating   ? SPIN_FRAME_MS
                   : polling     ? PROMPT_IDLE_POLL_MS
                                 : -1;

        if (!tty_read(&ev, wait)) {
            if (resizing) {
                resizing = 0;

                viewport_forget();
                repaint(p);
            } else {
                if (animating && p->animate_tick) {
                    p->animate_tick(p->animate_ud);
                    repaint(p);
                }
                if (polling)
                    idle_ready_hook(p);
                takeover_check(p);
                restart_check(p);
            }
            continue;
        }
        if (ev.key == TK_RESIZE) {
            resizing = 1;
            continue;
        }
        resizing = 0;

        switch (feed_key(p, &ev, 0)) {
        case KEY_EOF:
            chrome_keep_above();
            return NULL;

        case KEY_SUBMIT: {
            p->external_taken = 0;
            char *out = take_line(p);
            viewport_defer();
            chrome_clear();
            if (out && prompt_echoes(p, out))
                prompt_echo_message(out);
            viewport_flush();
            return out;
        }

        default:
            repaint(p);
            takeover_check(p);
            restart_check(p);
            continue;
        }
    }
}

char *prompt_read(struct prompt *p)
{
    tty_watch(p->idle_fds ? idle_fds_hook : NULL, idle_ready_hook, p);
    char *out = read_loop(p);
    tty_watch(NULL, NULL, NULL);
    return out;
}

void prompt_set_command(struct prompt *p, char *(*fn)(void *ud, int nth), void *ud)
{
    p->command = fn;
    p->command_ud = ud;
}

void prompt_set_line(struct prompt *p, const char *text)
{
    if (!p || !text)
        return;
    repl_replace_range(&p->repl, 0, p->repl.len, text);
    p->frame_ok = 0;
    if (!chrome_modal_active())
        repaint(p);
}

static int clamp_cursor(const Repl *r, int cursor)
{
    if (!r->buf || cursor <= 0)
        return 0;
    if (cursor > r->len)
        cursor = r->len;
    while (cursor > 0 && cursor < r->len &&
           ((unsigned char)r->buf[cursor] & 0xC0) == 0x80)
        cursor--;
    return cursor;
}

void prompt_stash_draft(char **text, int *cursor)
{
    if (text)
        *text = NULL;
    if (cursor)
        *cursor = 0;
    if (!active || !text)
        return;

    const char *line = repl_line(&active->repl);
    if (line && *line) {
        *text = strdup(line);
        if (!*text)
            return;
    }
    if (cursor)
        *cursor = active->repl.cursor;
}

void prompt_adopt_draft(const char *text, int cursor)
{
    if (!active)
        return;

    active->frame_ok = 0;
    repl_reset(&active->repl);
    if (text && *text)
        repl_insert_text(&active->repl, text);
    active->repl.cursor = clamp_cursor(&active->repl, cursor);
}

void prompt_set_external(struct prompt *p, char *(*fn)(void *ud), void *ud)
{
    p->external = fn;
    p->external_ud = ud;
}

int prompt_line_was_external(struct prompt *p)
{
    return p && p->external_taken;
}

static void queue_push(struct prompt *p, char *line)
{
    if (!line)
        return;
    if (p->queued_count == p->queued_cap) {
        int cap = p->queued_cap ? p->queued_cap * 2 : 4;
        char **grown = realloc(p->queued, (size_t)cap * sizeof *grown);
        if (!grown) {
            free(line);
            return;
        }
        p->queued = grown;
        p->queued_cap = cap;
    }
    p->queued[p->queued_count++] = line;
    p->frame_ok = 0;
}

char *prompt_take_queued(struct prompt *p)
{
    if (p->queued_count == 0)
        return NULL;
    char *line = p->queued[0];
    memmove(p->queued, p->queued + 1, (size_t)(--p->queued_count) * sizeof *p->queued);
    record_line(p, line);
    return line;
}

static int recall_queued(struct prompt *p)
{
    const char *live = repl_line(&p->repl);
    if (live && *live)
        return 0;

    char *line = p->queued_count ? p->queued[--p->queued_count]
               : p->q_take       ? p->q_take(p->q_ud)
                                 : NULL;
    if (!line)
        return 0;
    p->frame_ok = 0;
    repl_reset(&p->repl);
    repl_insert_text(&p->repl, line);
    free(line);
    return 1;
}

void prompt_set_animate(struct prompt *p, int (*busy)(void *ud), void (*tick)(void *ud),
                        void *ud)
{
    p->animate_busy = busy;
    p->animate_tick = tick;
    p->animate_ud = ud;
}

void prompt_set_replay(struct prompt *p, void (*fn)(void *ud), void *ud)
{
    p->replay = fn;
    p->replay_ud = ud;
}

void prompt_set_blank(struct prompt *p, void (*fn)(void *ud), void *ud)
{
    p->blank = fn;
    p->blank_ud = ud;
}

void prompt_set_echo_filter(struct prompt *p, int (*fn)(void *ud, const char *line),
                            void *ud)
{
    p->echo_filter = fn;
    p->echo_ud = ud;
}

static int prompt_echoes(struct prompt *p, const char *line)
{
    return !p || !p->echo_filter || p->echo_filter(p->echo_ud, line);
}

void prompt_set_live_command(struct prompt *p, prompt_live_fn fn, void *ud)
{
    p->live_command = fn;
    p->live_ud = ud;
}

int prompt_live_key(void *ud, tty_event *ev)
{
    struct prompt *p = ud;
    if (ev->key == TK_UP && recall_queued(p))
        return 0;
    switch (feed_key(p, ev, 1)) {
    case KEY_SUBMIT: {
        char *line = take_line(p);
        if (line && p->live_command && p->live_command(p->live_ud, line))
            free(line);
        else
            queue_push(p, line);
        return 0;
    }
    case KEY_EOF:
    case KEY_CANCEL:
        return 1;
    default:
        return 0;
    }
}
