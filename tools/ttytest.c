#include <errno.h>
#include <fcntl.h>
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
    expect_key(w, "\x1b[9;6u", 6, TK_PREV_TAB, "csi-u ctrl-shift-tab");
    expect_key(w, "\x1b[27;5;9~", 9, TK_NEXT_TAB, "xterm ctrl-tab");
    expect_key(w, "\x1b[27;6;9~", 9, TK_PREV_TAB, "xterm ctrl-shift-tab");
    expect_ctrl(w, "\x03", 1, 3, "ctrl-c");
    expect_ctrl(w, "\x1b[27;5;99~", 10, 3, "xterm ctrl-c");

    close(w);
}

int main(void)
{
    if (restore_from_raw_pty() != 0)
        return 1;
    keys_from_pipe();
    if (failures)
        return 1;
    puts("ttytest: ok");
    return 0;
}
