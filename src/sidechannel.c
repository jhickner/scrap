#include "sidechannel.h"

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "chrome.h"
#include "md.h"
#include "scrollback.h"
#include "sessionfork.h"
#include "sidechannelcmd.h"
#include "sidechannelview.h"
#include "status.h"
#include "ui.h"
#include "viewport.h"
#include "workspace.h"
#include "vendor/cJSON.h"

#define SIDE_MAX 4

#define BTW_PREAMBLE                                                            \
    "Answer the question below as an aside in a conversation you are not part " \
    "of. The person asking cannot reply to you: you get one answer and the "    \
    "exchange ends there. Do not ask follow-up questions, do not offer to do "  \
    "more, and do not ask what they want next. Be brief.\n\n"

struct stream {
    int    fd;
    char  *buf;
    size_t len, cap;
};

struct btw {
    char *question;
    char *answer;
    int   failed;
    int   gap;
};

struct side {
    pid_t            pid;
    struct stream    out, err;
    char            *question;
    const struct session *owner;
    sidechannel_done done;
    void            *ud;
};

#define STATUS_PREAMBLE                                                          \
    "You are a read-only fork of an agent in the middle of a long turn. The "   \
    "original agent is still running and was not interrupted; any tool call "   \
    "that looks cut off is still in progress there. Give the user a one or "    \
    "two sentence status update on what the agent has been doing and where it " \
    "is now, based on the conversation so far. Plain prose, no lists, no "      \
    "questions, no offers. Do not do any work yourself. "

#define STATUS_AGAIN                                                             \
    "The previous update was:\n\n%s\n\nDo not repeat it; report only what has " \
    "happened since."

static struct side slots[SIDE_MAX];

static void stream_free(struct stream *s)
{
    if (s->fd >= 0)
        close(s->fd);
    free(s->buf);
    s->fd = -1;
    s->buf = NULL;
    s->len = s->cap = 0;
}

static void slot_free(struct side *c)
{
    stream_free(&c->out);
    stream_free(&c->err);
    free(c->question);
    c->question = NULL;
    c->owner = NULL;
    c->done = NULL;
    c->ud = NULL;
    c->pid = 0;
}

#define BTW_DONE "\u2713 "
#define BTW_FAIL "\u00d7 "

static void bar_rows(const char *painted, enum ui_role role)
{
    size_t len = strlen(painted);
    while (len && painted[len - 1] == '\n')
        len--;

    const char *p = painted;
    const char *end = painted + len;
    while (p <= end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t n = nl ? (size_t)(nl - p) : (size_t)(end - p);

        ui_esc(ui_style(role));
        ui_put(UI_BAR);
        ui_esc(ui_style(UI_RESET));
        if (n) {
            ui_put(" ");
            ui_putn(p, n);
            ui_esc(ui_style(UI_RESET));
        }
        ui_put("\n");

        if (!nl)
            break;
        p = nl + 1;
    }
}

static char *btw_encode(void *ud)
{
    const struct btw *b = ud;
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddStringToObject(o, "question", b->question ? b->question : "");
    cJSON_AddStringToObject(o, "answer", b->answer ? b->answer : "");
    cJSON_AddNumberToObject(o, "failed", b->failed);
    cJSON_AddNumberToObject(o, "gap", b->gap);
    char *out = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return out;
}

static void btw_render(void *ud, int cols)
{
    const struct btw *b = ud;
    (void)cols;

    char mark[16];
    snprintf(mark, sizeof mark, "%s", b->failed ? BTW_FAIL : BTW_DONE);

    int budget = ui_columns() - 5;
    struct ui_wrap w = {0};
    w.budget = (size_t)(budget > 4 ? budget : 4);
    w.gutter = UI_BAR " ";
    w.mark = mark;
    w.role = UI_SIDE;
    w.erase = 1;
    w.paint_empty = 1;
    ui_wrap_paint(b->question, &w);

    if (!b->answer)
        return;

    int inner = ui_columns() - 2;
    ui_capture_begin(inner > 8 ? inner : 8);
    if (b->failed)
        ui_wrapped(b->answer, 0, UI_DIM);
    else
        md_render(b->answer, 0);
    char *painted = ui_capture_end();
    if (painted) {
        bar_rows(painted, UI_BRAND);
        free(painted);
    }
}

static void btw_free(void *ud)
{
    struct btw *b = ud;
    free(b->question);
    free(b->answer);
    free(b);
}

static int    spin_frame;
static double spun_at;

static int shown(const struct side *c)
{
    return c->pid && c->question && (!c->owner || c->owner == workspace_current() ||
                                     workspace_index_of(c->owner) < 0);
}

int sidechannel_rows(void)
{
    int n = 0;
    for (int i = 0; i < SIDE_MAX; i++)
        if (shown(&slots[i]))
            n += sidechannel_question_paint(slots[i].question, NULL, ui_columns(), 0, 1);
    return n;
}

void sidechannel_paint(int budget)
{
    for (int i = 0; i < SIDE_MAX && budget > 0; i++) {
        if (!shown(&slots[i]))
            continue;

        char mark[16];
        snprintf(mark, sizeof mark, "%s ", spin_glyph(spin_frame));
        budget -= sidechannel_question_paint(slots[i].question, mark, ui_columns(),
                                             budget, 0);
    }
}

void sidechannel_tick(void)
{
    if (!sidechannel_rows())
        return;

    if (!spin_advance(&spin_frame, &spun_at))
        return;

    chrome_paint();
}

static void slot_init(struct side *c)
{
    memset(c, 0, sizeof *c);
    c->out.fd = c->err.fd = -1;
}

static struct side *free_slot(void)
{
    for (int i = 0; i < SIDE_MAX; i++)
        if (!slots[i].pid)
            return &slots[i];
    return NULL;
}

static int spawn(struct side *c, const struct session *s, const char *prompt)
{
    int out_pipe[2], err_pipe[2];
    if (pipe(out_pipe) != 0)
        return 0;
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return 0;
    }
    fcntl(out_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(out_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(err_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(err_pipe[1], F_SETFD, FD_CLOEXEC);

    char *argv[24];
    if (!sidechannel_argv(s, prompt, argv, 24)) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        return 0;
    }
    if (pid == 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        if (out_pipe[1] != STDOUT_FILENO)
            close(out_pipe[1]);
        if (err_pipe[1] != STDERR_FILENO)
            close(err_pipe[1]);
        fcntl(STDOUT_FILENO, F_SETFD, 0);
        fcntl(STDERR_FILENO, F_SETFD, 0);
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            if (null != STDIN_FILENO)
                close(null);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);
    c->pid = pid;
    c->out.fd = out_pipe[0];
    c->err.fd = err_pipe[0];
    return 1;
}

static int start(const struct session *s, const char *asked, const char *label,
                 sidechannel_done done, void *ud, int loud)
{
    struct side *c = free_slot();
    if (!c) {
        if (loud) {
            ui_error("already running %d side turns", SIDE_MAX);
            ui_put("\n");
        }
        return 0;
    }

    slot_init(c);
    if (!spawn(c, s, asked)) {
        slot_free(c);
        if (loud) {
            ui_error("could not start the side turn");
            ui_put("\n");
        }
        return 0;
    }

    c->question = strdup(label);
    c->owner = s;
    c->done = done;
    c->ud = ud;
    chrome_paint();
    return 1;
}

int sidechannel_start(const struct session *s, const char *prompt, const char *label)
{
    if (!prompt || !*prompt)
        return 0;
    if (!label || !*label)
        label = prompt;

    size_t want = sizeof BTW_PREAMBLE + strlen(prompt);
    char  *asked = malloc(want);
    if (!asked)
        return 0;
    snprintf(asked, want, "%s%s", BTW_PREAMBLE, prompt);

    int ok = start(s, asked, label, NULL, NULL, 1);
    free(asked);
    return ok;
}

int sidechannel_status(const struct session *s, const char *prev,
                       sidechannel_done done, void *ud)
{
    size_t want = sizeof STATUS_PREAMBLE + sizeof STATUS_AGAIN +
                  (prev ? strlen(prev) : 0);
    char *asked = malloc(want);
    if (!asked)
        return 0;
    int n = snprintf(asked, want, "%s", STATUS_PREAMBLE);
    if (prev && *prev)
        snprintf(asked + n, want - (size_t)n, STATUS_AGAIN, prev);

    int ok = start(s, asked, "status update", done, ud, 0);
    free(asked);
    return ok;
}

static void slot_kill(struct side *c)
{
    kill(c->pid, SIGTERM);
    int status = 0;
    while (waitpid(c->pid, &status, 0) < 0 && errno == EINTR)
        ;
    slot_free(c);
}

void sidechannel_forget(const struct session *s)
{
    for (int i = 0; i < SIDE_MAX; i++)
        if (slots[i].pid && slots[i].owner == s)
            slot_kill(&slots[i]);
}

int sidechannel_fds(int *out, int max)
{
    int n = 0;
    for (int i = 0; i < SIDE_MAX; i++) {
        if (!slots[i].pid)
            continue;
        if (slots[i].out.fd >= 0 && n < max)
            out[n++] = slots[i].out.fd;
        if (slots[i].err.fd >= 0 && n < max)
            out[n++] = slots[i].err.fd;
    }
    return n;
}

int sidechannel_busy(void)
{
    for (int i = 0; i < SIDE_MAX; i++)
        if (slots[i].pid)
            return 1;
    return 0;
}

static char *trimmed(struct stream *s)
{
    if (!s->buf)
        return NULL;
    char *text = s->buf;
    while (*text == '\n' || *text == ' ')
        text++;
    size_t end = strlen(text);
    while (end && (text[end - 1] == '\n' || text[end - 1] == ' '))
        text[--end] = '\0';
    return *text ? text : NULL;
}

static void btw_place(struct session *s, void *ud)
{
    (void)s;
    struct btw *b = ud;
    unsigned mark = viewport_item_begin(&(struct viewport_entry){
        .render = btw_render, .ud = b, .free_ud = btw_free, .reflow = 1,
        .pad_before = b->gap, .pad_after = 1});
    btw_render(b, ui_columns());
    viewport_item_end();
    viewport_item_persist(mark, SIDECHANNEL_BTW_KIND, btw_encode);
}

static void emit(struct side *c, int status)
{
    char *reply = trimmed(&c->out);
    char *why = trimmed(&c->err);

    char note[256];
    const char *answer = reply;
    int failed = 0;
    if (!answer) {
        failed = 1;
        if (why) {
            answer = why;
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            snprintf(note, sizeof note, "could not run %s for the side turn",
                     sessionfork_program());
            answer = note;
        } else {
            snprintf(note, sizeof note, "the side turn produced nothing (exit %d)",
                     WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            answer = note;
        }
    }

    if (c->done)
        c->done(c->ud, failed ? NULL : answer);

    struct btw *b = calloc(1, sizeof *b);
    if (!b)
        return;
    b->question = strdup(c->question ? c->question : "");
    b->answer = strdup(answer);
    b->failed = failed;
    b->gap = 1;
    if (!b->question || !b->answer) {
        btw_free(b);
        return;
    }

    int tab = c->owner ? workspace_index_of(c->owner) : -1;
    if (tab >= 0 && c->owner != workspace_current())
        workspace_render(tab, btw_place, b);
    else
        btw_place(NULL, b);
    ui_flush();
}

void sidechannel_btw_load(const cJSON *st)
{
    struct btw *b = calloc(1, sizeof *b);
    if (!b)
        return;
    b->question = strdup(scrollback_str(st, "question"));
    b->answer = strdup(scrollback_str(st, "answer"));
    b->failed = scrollback_int(st, "failed");
    b->gap = scrollback_int(st, "gap");
    if (!b->question || !b->answer) {
        btw_free(b);
        return;
    }

    unsigned mark = viewport_item_begin(&(struct viewport_entry){
        .render = btw_render, .ud = b, .free_ud = btw_free, .reflow = 1,
        .pad_before = b->gap, .pad_after = 1});
    btw_render(b, ui_columns());
    viewport_item_end();
    viewport_item_persist(mark, SIDECHANNEL_BTW_KIND, btw_encode);
}

static int drain(struct stream *s)
{
    if (s->fd < 0)
        return 0;
    for (;;) {
        if (s->len + 8192 + 1 > s->cap) {
            size_t want = s->cap ? s->cap * 2 : 16384;
            while (want < s->len + 8192 + 1)
                want *= 2;
            char *grown = realloc(s->buf, want);
            if (!grown)
                break;
            s->buf = grown;
            s->cap = want;
        }
        ssize_t r = read(s->fd, s->buf + s->len, 8192);
        if (r > 0) {
            s->len += (size_t)r;
            s->buf[s->len] = '\0';
            continue;
        }
        if (r == 0)
            break;
        if (errno == EINTR)
            continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 1;
        break;
    }
    close(s->fd);
    s->fd = -1;
    return 0;
}

void sidechannel_poll(void)
{
    for (int i = 0; i < SIDE_MAX; i++) {
        struct side *c = &slots[i];
        if (!c->pid)
            continue;
        int live = drain(&c->out);
        live |= drain(&c->err);
        if (live)
            continue;

        int   status = 0;
        pid_t went = waitpid(c->pid, &status, WNOHANG);
        if (went == 0 || (went < 0 && errno == EINTR))
            continue;

        struct side done = *c;
        slot_init(c);

        emit(&done, status);

        stream_free(&done.out);
        stream_free(&done.err);
        free(done.question);

        chrome_paint();
    }
}

void sidechannel_close_all(void)
{
    for (int i = 0; i < SIDE_MAX; i++) {
        if (!slots[i].pid)
            continue;
        slot_kill(&slots[i]);
    }
}
