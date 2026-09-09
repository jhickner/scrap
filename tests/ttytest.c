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
    close(slave);

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
    close(master);

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
    expect_key(w, "\x1b[I", 3, TK_FOCUS_IN, "focus in");
    expect_key(w, "\x1b[O", 3, TK_FOCUS_OUT, "focus out");

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

/* a watched fd that takes longer to serve than the 50ms a sequence waits for
   its next byte: the tail must still arrive as one event */
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

int main(void)
{
    handoff_keeps_alt_screen();
    if (restore_from_raw_pty() != 0)
        return 1;
    wake_unblocks_read();
    watch_keeps_a_sequence_whole();
    keys_from_pipe();
    if (failures)
        return 1;
    puts("ttytest: ok");
    return 0;
}
