#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include "tty.h"
#include "ui.h"
#include "viewport.h"

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static int echo_on(void)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0)
        return 0;
    return (t.c_lflag & (ECHO | ICANON)) == (ECHO | ICANON);
}

static void set_echo(int on)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) != 0)
        return;
    if (on)
        t.c_lflag |= ECHO | ICANON | ISIG | IEXTEN;
    else
        t.c_lflag &= ~(unsigned long)(ECHO | ICANON | ISIG | IEXTEN);
    t.c_oflag = on ? (t.c_oflag | OPOST) : (t.c_oflag & ~(unsigned long)OPOST);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static int send(int wfd, const char *bytes, size_t n, tty_event *ev)
{
    if (write(wfd, bytes, n) != (ssize_t)n)
        return 0;
    return tty_read(ev, 200);
}

static int read_until(tty_event *ev, int ms)
{
    for (int left = ms;;) {
        if (tty_read(ev, left > 50 ? 50 : left))
            return 1;
        left -= 50;
        if (left <= 0)
            return 0;
    }
}

static void expect_focus(int wfd, const char *bytes, tty_key want, const char *what)
{
    tty_event ev;
    if (write(wfd, bytes, 3) != 3) {
        fail(what);
        return;
    }
    if (!read_until(&ev, 600) || ev.key != want) {
        fprintf(stderr, "FAIL %s: key %d want %d\n", what, (int)ev.key, (int)want);
        failures++;
    }
}

static void handoff_keeps_alt_screen(void)
{
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        fail("handoff openpty");
        return;
    }

    int oldin = dup(STDIN_FILENO), oldout = dup(STDOUT_FILENO);
    if (oldin < 0 || oldout < 0 || dup2(slave, STDIN_FILENO) < 0 ||
        dup2(slave, STDOUT_FILENO) < 0) {
        fail("handoff dup2");
        close(master);
        close(slave);
        return;
    }

    if (tty_raw_begin() != 0)
        fail("handoff raw begin");
    tty_raw_handoff();
    if (!echo_on())
        fail("handoff restores cooked input");

    dup2(oldin, STDIN_FILENO);
    dup2(oldout, STDOUT_FILENO);
    close(oldin);
    close(oldout);
    fcntl(master, F_SETFL, O_NONBLOCK);

    char output[4096];
    size_t n = 0;
    for (;;) {
        ssize_t got = read(master, output + n, sizeof output - n - 1);
        if (got <= 0)
            break;
        n += (size_t)got;
        if (n == sizeof output - 1)
            break;
    }
    output[n] = '\0';
    close(slave);
    close(master);

    if (!strstr(output, "\x1b[>1u") || !strstr(output, "\x1b[<u") ||
        !strstr(output, "\x1b[>4;0m"))
        fail("handoff restores keyboard modes");
    if (strstr(output, "\x1b[>9u"))
        fail("report-all inserts standalone modifier keys");
    if (strstr(output, "\x1b[?1049l"))
        fail("handoff left the alternate screen");
}

static void expect_key(int wfd, const char *bytes, size_t n, tty_key want,
                       const char *what)
{
    tty_event ev;
    if (!send(wfd, bytes, n, &ev)) {
        fail(what);
        return;
    }
    if (ev.key != want) {
        fprintf(stderr, "FAIL %s: key %d want %d\n", what, (int)ev.key, (int)want);
        failures++;
        if (ev.text)
            free(ev.text);
        return;
    }
    if (ev.text)
        free(ev.text);
}

static void expect_none(int wfd, const char *bytes, size_t n, const char *what)
{
    tty_event ev;
    if (send(wfd, bytes, n, &ev)) {
        fprintf(stderr, "FAIL %s: key %d want none\n", what, (int)ev.key);
        failures++;
        if (ev.text)
            free(ev.text);
    }
}

static void expect_ctrl(int wfd, const char *bytes, size_t n, uint32_t cp,
                        const char *what)
{
    tty_event ev;
    if (!send(wfd, bytes, n, &ev) || ev.key != TK_CHAR || ev.cp != cp) {
        fprintf(stderr, "FAIL %s: key %d cp %u\n", what, (int)ev.key, ev.cp);
        failures++;
        if (ev.text)
            free(ev.text);
        return;
    }
}

static void modified_keys(int w)
{
    for (int ch = 'a'; ch <= 'z'; ch++) {
        char raw = (char)(ch - 'a' + 1), seq[96];
        tty_key key = TK_CHAR;
        if (raw == 8) key = TK_BACKSPACE;
        if (raw == 9) key = TK_TAB;
        if (raw == 10) key = TK_NEWLINE;
        if (raw == 13) key = TK_ENTER;
        for (int form = 0; form < 5; form++) {
            int n;
            if (form == 0) { seq[0] = raw; n = 1; }
            else if (form == 1) n = snprintf(seq, sizeof seq, "\x1b[%d;5u", ch);
            else if (form == 2) n = snprintf(seq, sizeof seq, "\x1b[27;5;%d~", ch);
            else if (form == 3) n = snprintf(seq, sizeof seq, "\x1b[%d::%d;69:2u", ch, ch);
            else n = snprintf(seq, sizeof seq, "\x1b[%d:%d;6:1u", ch, ch - 32);
            if (key == TK_CHAR)
                expect_ctrl(w, seq, (size_t)n, (uint32_t)raw, "ctrl-letter formats");
            else
                expect_key(w, seq, (size_t)n, key, "ctrl-letter formats");
        }
    }
#define KEY(s, k) expect_key(w, s, sizeof(s) - 1, k, s)
#define CHAR(s, c) expect_ctrl(w, s, sizeof(s) - 1, c, s)
#define NONE(s) expect_none(w, s, sizeof(s) - 1, s)
    NONE("\x1b[57442;5u");
    CHAR("\x1b[100;5u", 4);
    NONE("\x1b[57442u");
    NONE("\x1b[57448;5u");
    NONE("\x1b[100::100;5:3u");
    NONE("\x1b[?9u");
    NONE("\x1b[1114112u");
    NONE("\x1b[55296u");
    NONE("\x1b[999999999999999999999999u");
    NONE("\x1b[1;2;3;4;5;6;7;8;9u");
    CHAR("h", 'h');
    CHAR("H", 'H');
    CHAR("\x1b[104;1u", 'h');
    CHAR("\x1b[104:72;2u", 'H');
    CHAR("\x1b[27;2;72~", 'H');
    CHAR("\x1b[233u", 233);
    CHAR("\x1b[32;5u", 0);
    CHAR("\x1b[95;5u", 31);
    CHAR("\x1b[27;5;47~", 31);
    KEY("\x1b[91;5u", TK_ESCAPE);
    KEY("\x1b[27;5;127~", TK_BACKSPACE);
    KEY("\x1b[27;1;27~", TK_ESCAPE);
    KEY("\x1b[13;65u", TK_ENTER);
    KEY("\x1b[9;2u", TK_PREV_TAB);
    KEY("\x1b[27;2;9~", TK_PREV_TAB);
    KEY("\x1b[1;2A", TK_PREV_TAB);
    KEY("\x1b[1;2B", TK_NEXT_TAB);
    KEY("\x1b[1;5A", TK_UP);
    KEY("\x1b[98;3u", TK_WORD_LEFT);
    KEY("\x1b[27;3;102~", TK_WORD_RIGHT);
    KEY("\x1b[1;70D", TK_WORD_LEFT);
    KEY("\x1b[1;6C", TK_WORD_RIGHT);
    KEY("\x1bOD", TK_LEFT);
    KEY("\x1b[3;5~", TK_DELETE);
    KEY("\x1b[1;5H", TK_HOME);
    KEY("\x1b[57414u", TK_ENTER);
    KEY("\x1b[57414;2u", TK_NEWLINE);
    KEY("\x1b[57417;5u", TK_WORD_LEFT);
    CHAR("\x1b[57400u", '1');
    NONE("\x1b[97;9u");
#undef KEY
#undef CHAR
#undef NONE
}

static int restore_from_raw_pty(void)
{
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        fprintf(stderr, "openpty: %s\n", strerror(errno));
        return 1;
    }

    int oldin = dup(STDIN_FILENO);
    int oldout = dup(STDOUT_FILENO);
    int nullfd = open("/dev/null", O_RDWR);
    if (oldin < 0 || oldout < 0 || nullfd < 0)
        return 1;
    if (dup2(slave, STDIN_FILENO) < 0 || dup2(slave, STDOUT_FILENO) < 0)
        return 1;

    pid_t drain = fork();
    if (drain < 0)
        return 1;
    if (drain == 0) {
        close(slave);
        dup2(nullfd, STDIN_FILENO);
        dup2(nullfd, STDOUT_FILENO);
        char buf[4096];
        while (read(master, buf, sizeof buf) > 0)
            ;
        _exit(0);
    }
    close(nullfd);

    set_echo(0);
    if (tty_raw_begin() != 0)
        fail("tty_raw_begin on a raw pty");
    tty_raw_end();
    if (!echo_on())
        fail("restore from a raw entry snapshot");

    set_echo(0);
    if (tty_raw_begin() != 0)
        fail("second tty_raw_begin");
    tty_raw_end();
    if (!echo_on())
        fail("later begin keeps the first cooked snapshot");

    tty_raw_end();
    if (!echo_on())
        fail("a second raw end still leaves echo on");

    dup2(oldin, STDIN_FILENO);
    dup2(oldout, STDOUT_FILENO);
    close(oldin);
    close(oldout);
    close(slave);
    close(master);
    while (waitpid(drain, NULL, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

static volatile sig_atomic_t alarm_hit;

static void on_usr1(int sig)
{
    (void)sig;
    tty_wake();
}

static void on_alrm(int sig)
{
    (void)sig;
    alarm_hit = 1;
    tty_wake();
}

static void wake_unblocks_read(void)
{
    int sp[2];
    if (pipe(sp) != 0) {
        fail("wake pipe");
        return;
    }
    int oldin = dup(STDIN_FILENO);
    if (oldin < 0 || dup2(sp[0], STDIN_FILENO) < 0) {
        fail("wake dup2");
        close(sp[0]);
        close(sp[1]);
        return;
    }
    close(sp[0]);

    tty_event ev;
    tty_wake();
    if (tty_read(&ev, -1))
        fail("latched tty_wake still blocked");

    struct sigaction sa = {0}, oldusr, oldalrm;
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, &oldusr);
    sa.sa_handler = on_alrm;
    sigaction(SIGALRM, &sa, &oldalrm);

    alarm_hit = 0;
    pid_t child = fork();
    if (child < 0) {
        fail("wake fork");
    } else if (child == 0) {
        usleep(50 * 1000);
        kill(getppid(), SIGUSR1);
        _exit(0);
    } else {
        alarm(2);
        int r = tty_read(&ev, -1);
        alarm(0);
        if (alarm_hit)
            fail("signal tty_wake did not unblock");
        else if (r)
            fail("signal tty_wake returned an event");
        int st;
        while (waitpid(child, &st, 0) < 0 && errno == EINTR)
            ;
    }

    sigaction(SIGUSR1, &oldusr, NULL);
    sigaction(SIGALRM, &oldalrm, NULL);
    dup2(oldin, STDIN_FILENO);
    close(oldin);
    close(sp[1]);
}

static void keys_from_pipe(void)
{
    int sp[2];
    if (pipe(sp) != 0) {
        fail("pipe");
        return;
    }
    if (dup2(sp[0], STDIN_FILENO) < 0) {
        fail("dup2 stdin");
        return;
    }
    close(sp[0]);
    int w = sp[1];

    modified_keys(w);
    expect_key(w, "\t", 1, TK_TAB, "tab");
    expect_key(w, "\x1b[9u", 4, TK_TAB, "csi-u tab");
    expect_key(w, "\x1b[9;5u", 6, TK_NEXT_TAB, "csi-u ctrl-tab");
    expect_key(w, "\x1b[9;5:1u", 8, TK_NEXT_TAB, "csi-u ctrl-tab press");
    expect_none(w, "\x1b[9;5:3u", 8, "csi-u ctrl-tab release");
    expect_key(w, "\x1b[9;6u", 6, TK_PREV_TAB, "csi-u ctrl-shift-tab");
    expect_key(w, "\x1b[Z", 3, TK_PREV_TAB, "csi backtab");
    expect_key(w, "\x1b[27;5;9~", 9, TK_NEXT_TAB, "xterm ctrl-tab");
    expect_key(w, "\x1b[27;6;9~", 9, TK_PREV_TAB, "xterm ctrl-shift-tab");
    expect_key(w, "\x1b[127u", 6, TK_BACKSPACE, "csi-u backspace");
    expect_key(w, "\x1b[27u", 5, TK_ESCAPE, "csi-u escape");
    expect_key(w, "\x1b[13;2u", 7, TK_NEWLINE, "csi-u shift-enter");
    expect_key(w, "\x1b[27;2;13~", 10, TK_NEWLINE, "xterm shift-enter");
    expect_key(w, "\x1b[106;5u", 8, TK_NEWLINE, "csi-u ctrl-j");
    expect_key(w, "\x1b[27;5;106~", 11, TK_NEWLINE, "xterm ctrl-j");
    expect_ctrl(w, "\x03", 1, 3, "ctrl-c");
    expect_ctrl(w, "\x1b[27;5;99~", 10, 3, "xterm ctrl-c");
    expect_ctrl(w, "\x1b[97;5u", 7, 1, "csi-u ctrl-a");
    expect_focus(w, "\x1b[O", TK_FOCUS_OUT, "focus out");
    expect_focus(w, "\x1b[I", TK_FOCUS_IN, "focus in");

    expect_focus(w, "\x1b[I", TK_FOCUS_IN, "focus repeat");

    close(w);
}

static int watch_fd = -1;

static int busy_fds(void *ud, int *out, int max)
{
    (void)ud;
    if (max < 1 || watch_fd < 0)
        return 0;
    out[0] = watch_fd;
    return 1;
}

static void busy_ready(void *ud)
{
    (void)ud;
    usleep(80 * 1000);
}

static void watch_keeps_a_sequence_whole(void)
{
    int sp[2], watch[2];
    if (pipe(sp) != 0 || pipe(watch) != 0) {
        fail("watch pipes");
        return;
    }
    int oldin = dup(STDIN_FILENO);
    if (oldin < 0 || dup2(sp[0], STDIN_FILENO) < 0) {
        fail("watch dup2");
        return;
    }
    close(sp[0]);

    if (write(watch[1], "x", 1) != 1)
        fail("watch prime");
    watch_fd = watch[0];
    tty_watch(busy_fds, busy_ready, NULL);

    if (write(sp[1], "\x1b[<", 3) != 3)
        fail("watch head");

    pid_t child = fork();
    if (child < 0) {
        fail("watch fork");
    } else if (child == 0) {
        usleep(10 * 1000);
        ssize_t put = write(sp[1], "0;5;5M", 6);
        _exit(put == 6 ? 0 : 1);
    }

    tty_event ev;
    if (!tty_read(&ev, 400) || ev.key != TK_MOUSE_DOWN)
        fail("a watched fd cut a mouse sequence short");
    if (ev.text)
        free(ev.text);

    int st;
    while (child > 0 && waitpid(child, &st, 0) < 0 && errno == EINTR)
        ;
    tty_watch(NULL, NULL, NULL);
    watch_fd = -1;
    close(watch[0]);
    close(watch[1]);
    close(sp[1]);
    dup2(oldin, STDIN_FILENO);
    close(oldin);
}

static void cursor_report(void)
{
    int master, slave;
    if (openpty(&master, &slave, NULL, NULL, NULL) < 0) {
        fail("cursor openpty");
        return;
    }

    int oldin = dup(STDIN_FILENO), oldout = dup(STDOUT_FILENO);
    if (oldin < 0 || oldout < 0 || dup2(slave, STDIN_FILENO) < 0 ||
        dup2(slave, STDOUT_FILENO) < 0) {
        fail("cursor dup2");
        close(master);
        close(slave);
        return;
    }
    set_echo(0);
    if (tty_raw_begin() != 0)
        fail("cursor raw begin");

    const char reply[] = "x\x1b[12;34Ry";
    if (write(master, reply, sizeof reply - 1) != (ssize_t)(sizeof reply - 1))
        fail("cursor reply write");

    int row = 0, col = 0;
    if (!tty_cursor_position(&row, &col))
        fail("cursor position not reported");
    else if (row != 12 || col != 34)
        fprintf(stderr, "FAIL cursor position: %d;%d want 12;34\n", row, col), failures++;

    expect_ctrl(master, "", 0, 'x', "cursor keeps typeahead before the reply");
    expect_ctrl(master, "", 0, 'y', "cursor keeps typeahead after the reply");

    if (write(master, "z", 1) != 1)
        fail("cursor quiet write");
    row = col = 0;
    if (tty_cursor_position(&row, &col))
        fail("cursor position reported without a reply");
    expect_ctrl(master, "", 0, 'z', "cursor keeps typeahead with no reply");

    tty_raw_end();
    dup2(oldin, STDIN_FILENO);
    dup2(oldout, STDOUT_FILENO);
    close(oldin);
    close(oldout);
    close(slave);
    close(master);
}

static size_t drain(int fd, char *out, size_t max)
{
    size_t n = 0;
    for (;;) {
        ssize_t got = read(fd, out + n, max - n);
        if (got <= 0)
            return n;
        n += (size_t)got;
        if (n >= max)
            return n;
    }
}

static void exit_returns_home(void)
{
    struct winsize ws = {24, 80, 0, 0};
    int            master, slave;
    if (openpty(&master, &slave, NULL, NULL, &ws) < 0) {
        fail("home openpty");
        return;
    }

    int oldin = dup(STDIN_FILENO), oldout = dup(STDOUT_FILENO);
    if (oldin < 0 || oldout < 0 || dup2(slave, STDIN_FILENO) < 0 ||
        dup2(slave, STDOUT_FILENO) < 0) {
        fail("home dup2");
        close(master);
        close(slave);
        return;
    }
    set_echo(0);
    if (tty_raw_begin() != 0)
        fail("home raw begin");

    const char reply[] = "\x1b[7;3R";
    if (write(master, reply, sizeof reply - 1) != (ssize_t)(sizeof reply - 1))
        fail("home reply write");

    static char out[1 << 16];
    size_t      n = 0;
    fcntl(master, F_SETFL, O_NONBLOCK);

    ui_init();
    viewport_begin();

    n += drain(master, out + n, sizeof out - n - 1);
    viewport_suspend();
    viewport_resume();
    viewport_end();
    fflush(stdout);
    n += drain(master, out + n, sizeof out - n - 1);
    tty_raw_end();
    n += drain(master, out + n, sizeof out - n - 1);
    out[n] = '\0';

    dup2(oldin, STDIN_FILENO);
    dup2(oldout, STDOUT_FILENO);
    close(oldin);
    close(oldout);
    close(slave);
    close(master);

    int screen = 0, depth[2] = {0};
    for (const char *p = out; *p; p++) {
        if (!strncmp(p, "\x1b[>1u", 5)) depth[screen]++;
        if (!strncmp(p, "\x1b[<u", 4) && --depth[screen] < 0)
            fail("keyboard stack underflow");
        if (!strncmp(p, "\x1b[?1049h", 8) || !strncmp(p, "\x1b[?1049l", 8)) {
            if (depth[screen]) fail("keyboard mode left on previous screen");
            screen = p[7] == 'h';
        }
    }
    if (depth[0] || depth[1]) fail("keyboard stack leaked on exit");
    const char *off = strstr(out, "\x1b[?1049l");
    if (!off)
        fail("home never left the alternate screen");
    else if (!strstr(off, "\x1b[7;3H"))
        fail("home did not put the cursor back where the shell left it");
}

int main(void)
{
    handoff_keeps_alt_screen();
    if (restore_from_raw_pty() != 0)
        return 1;
    wake_unblocks_read();
    watch_keeps_a_sequence_whole();
    keys_from_pipe();
    cursor_report();
    exit_returns_home();
    if (failures)
        return 1;
    puts("ttytest: ok");
    return 0;
}
