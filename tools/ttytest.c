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

int main(void)
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
    return failures ? 1 : 0;
}
