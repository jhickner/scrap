
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#include "confirm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "block.h"
#include "chrome.h"
#include "prompt.h"
#include "tty.h"
#include "restart.h"
#include "screenmodel.h"
#include "sidechannel.h"
#include "status.h"
#include "ui.h"
#include "viewport.h"

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static int pending;
static int paint_calls;

int sidechannel_rows(void) { return pending; }

void sidechannel_paint(int budget)
{
    (void)budget;
    for (int i = 0; i < pending; i++) {
        paint_calls++;
        ui_put("BTWROW\n");
    }
}

void sidechannel_tick(void) {}
void sidechannel_poll(void) {}
int  sidechannel_busy(void) { return pending > 0; }
void sidechannel_close_all(void) {}
int  sidechannel_fds(int *out, int max) { (void)out; (void)max; return 0; }

void restart_shield_thread(void) {}

static int tap_read = -1;

static void pump(struct screen *s)
{
    fflush(stdout);
    char    buf[65536];
    ssize_t n;
    while ((n = read(tap_read, buf, sizeof buf)) > 0)
        feed(s, buf, (size_t)n);
}

static void repaint(struct screen *s)
{
    screen_init(s, 24, 80);
    viewport_forget();
    paint_calls = 0;
    chrome_paint();
    pump(s);
}

static int row_of(const struct screen *s, const char *needle) { return row_with(s, needle); }

static void repaint_rows(struct screen *s)
{
    screen_init(s, 24, 80);
    viewport_forget();
    viewport_paint();
    pump(s);
}

static void paint_menu(void *ud)
{
    (void)ud;
    ui_put("MENU\n");
    ui_put("  item\n");
}

static void queue_line(struct prompt *p, const char *text)
{
    for (const char *c = text; *c; c++) {
        tty_event ev = {.key = TK_CHAR, .cp = (uint32_t)(unsigned char)*c, .text = NULL};
        prompt_live_key(p, &ev);
    }
    tty_event enter = {.key = TK_ENTER, .cp = 0, .text = NULL};
    prompt_live_key(p, &enter);
}

static void check_queued(struct prompt *p, struct screen *s)
{
    char lots[512];
    memset(lots, 'q', sizeof lots - 1);
    lots[sizeof lots - 1] = '\0';
    memcpy(lots, "QFIRST", 6);

    queue_line(p, lots);
    queue_line(p, "QSECOND");
    repaint(s);

    int first = row_of(s, "QFIRST");
    int second = row_of(s, "QSECOND");
    if (first < 0)
        fail("a queued line is on screen");
    if (second < 0)
        fail("a second queued line is on screen");
    if (first >= 0 && second >= 0) {
        if (first >= second)
            fail("queued lines are painted in the order they were typed");

        if (second - first != QUEUED_LINES + 1)
            fail("a long queued line is cut short and followed by a blank row");
        if (!row_blank(s, second - 1))
            fail("the row between two queued lines is blank");
    }
    if (row_of(s, "\xe2\x80\xa6") < 0)
        fail("a queued line that was cut short says so");
}

static void check_confirmation(const char *input, int quit, int expected)
{
    int master, ready[2];
    if (pipe(ready) != 0) {
        fail("confirmation pipe");
        return;
    }
    pid_t child = forkpty(&master, NULL, NULL, NULL);
    if (child == 0) {
        close(ready[0]);
        alarm(3);
        unsetenv("TMUX");
        if (tty_raw_begin() != 0)
            _exit(2);
        ui_raw(1);
        if (write(ready[1], "r", 1) != 1)
            _exit(2);
        int answer = confirm_run("trust this folder in codex?");
        int clean = !chrome_modal_active();
        tty_raw_end();
        _exit(answer == expected && clean ? 0 : 1);
    }
    close(ready[1]);
    if (child < 0) {
        close(ready[0]);
        fail("confirmation forkpty");
        return;
    }
    char byte;
    if (read(ready[0], &byte, 1) != 1)
        fail("confirmation ready");
    else if (quit)
        kill(child, SIGTERM);
    else if (write(master, input, strlen(input)) != (ssize_t)strlen(input))
        fail("confirmation input");
    close(ready[0]);
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
        fail(quit ? "confirmation exits on quit signal" : "confirmation keyboard response");
    close(master);
}

int main(void)
{
    check_confirmation("y", 0, 1);
    check_confirmation("n", 0, 0);
    check_confirmation("\003", 0, 0);
    check_confirmation("\033", 0, 0);
    check_confirmation("", 1, 0);
    setenv("COLUMNS", "80", 1);
    setenv("LINES", "24", 1);

    char path[] = "/tmp/scrap-chrometest-XXXXXX";
    int  wfd = mkstemp(path);
    tap_read = wfd >= 0 ? open(path, O_RDONLY) : -1;
    if (wfd < 0 || tap_read < 0) {
        fprintf(stderr, "chrometest: no temp file\n");
        return 1;
    }
    unlink(path);
    fflush(stdout);
    if (dup2(wfd, STDOUT_FILENO) < 0) {
        fprintf(stderr, "chrometest: cannot redirect stdout\n");
        return 1;
    }
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);

    ui_init();
    ui_raw(1);
    viewport_begin();

    struct prompt *p = prompt_new(NULL, 0);
    if (!p) {
        fprintf(stderr, "chrometest: no prompt\n");
        return 1;
    }
    chrome_bind(p);

    struct screen s;
    screen_init(&s, 24, 80);

    status_sticky_set(1);
    viewport_write("<echo>\n", 7);
    status_sticky_prompt("STICKYTEXT");
    for (int i = 0; i < 60; i++)
        viewport_write("filler\n", 7);

    pending = 1;
    status_begin();
    repaint(&s);

    if (paint_calls != 1)
        fail("a pending side turn is painted once per chrome build");

    int sticky = row_of(&s, "STICKYTEXT");
    int btw = row_of(&s, "BTWROW");
    int spin = row_of(&s, "working");

    if (sticky < 0)
        fail("the sticky prompt is on screen");
    if (btw < 0)
        fail("the pending side turn is on screen");
    if (spin < 0)
        fail("the spinner is on screen");

    if (btw >= 0 && sticky >= 0 && btw > sticky)
        fail("the pending side turn sits above the sticky prompt");
    if (sticky >= 0 && spin >= 0 && sticky > spin)
        fail("the sticky prompt sits above the spinner");
    if (btw >= 0 && spin >= 0 && !row_blank(&s, spin - 1))
        fail("a blank row separates what is pinned above from the spinner");
    if (s.cursor_visible)
        fail("the painted input keeps the terminal cursor hidden");

    pending = 0;
    repaint(&s);
    if (paint_calls != 0)
        fail("nothing is painted for a side turn that is over");
    if (row_of(&s, "BTWROW") >= 0)
        fail("an answered side turn leaves no row behind");
    if (row_of(&s, "STICKYTEXT") < 0)
        fail("the sticky prompt outlives the side turn");

    pending = 2;
    repaint(&s);
    if (paint_calls != 2)
        fail("each pending side turn gets a row of its own");

    pending = 0;
    check_queued(p, &s);

    status_end();

    chrome_modal(paint_menu, NULL);
    pump(&s);
    if (s.cursor_visible)
        fail("a modal opened after a turn keeps the terminal cursor hidden");

    repaint(&s);
    if (row_of(&s, "filler") < 0)
        fail("a modal leaves the session on screen above it");
    if (chrome_modal_rows() >= 24)
        fail("a modal is offered fewer rows than the screen");

    chrome_full(1);
    repaint(&s);
    if (row_of(&s, "filler") >= 0)
        fail("a full modal covers the session");
    if (row_of(&s, "MENU") != 0)
        fail("a full modal starts on the top row");
    if (chrome_modal_rows() != 24)
        fail("a full modal is offered every row");

    chrome_modal_keep();
    chrome_clear();
    chrome_paint();
    pump(&s);
    if (row_of(&s, "MENU") != 0)
        fail("a kept modal stays on screen until its owner paints it again");

    block_forget();
    repaint_rows(&s);
    if (row_of(&s, "MENU") != 0)
        fail("a kept modal survives the block_forget of a tab swap");

    block_cleared();
    repaint_rows(&s);
    if (row_of(&s, "MENU") != 0)
        fail("a kept modal survives the block_cleared of a tab swap");
    chrome_full(0);

    chrome_modal(NULL, NULL);

    chrome_bind(NULL);
    prompt_free(p);
    viewport_end();

    fflush(stdout);
    if (failures)
        return 1;
    fprintf(stderr, "chrometest: all checks passed\n");
    return 0;
}
