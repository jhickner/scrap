#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tty.h"

static char logbuf[8192];
static size_t logn;

static void put(const char *s)
{
    fputs(s, stdout);
    fflush(stdout);
}

static void log_str(const char *s)
{
    size_t n = strlen(s);
    if (logn + n >= sizeof logbuf)
        return;
    memcpy(logbuf + logn, s, n);
    logn += n;
}

static void dump_bytes(const unsigned char *p, ssize_t n)
{
    char line[1024];
    size_t at = 0;
    for (ssize_t i = 0; i < n && at + 4 < sizeof line; i++)
        at += (size_t)snprintf(line + at, sizeof line - at, "%s%02x", i ? " " : "",
                               p[i]);
    at += (size_t)snprintf(line + at, sizeof line - at, "   ");
    for (ssize_t i = 0; i < n && at + 8 < sizeof line; i++) {
        unsigned char c = p[i];
        if (c == 0x1b)
            at += (size_t)snprintf(line + at, sizeof line - at, "ESC ");
        else if (c == '\t')
            at += (size_t)snprintf(line + at, sizeof line - at, "TAB ");
        else if (c == '\r')
            at += (size_t)snprintf(line + at, sizeof line - at, "CR ");
        else if (c == '\n')
            at += (size_t)snprintf(line + at, sizeof line - at, "LF ");
        else if (c >= 0x20 && c < 0x7f)
            line[at++] = (char)c, line[at] = '\0';
        else
            at += (size_t)snprintf(line + at, sizeof line - at, "\\x%02x", c);
    }
    snprintf(line + at, sizeof line - at, "\n");
    log_str(line);

    for (char *s = line; *s; s++) {
        if (*s == '\n')
            fputs("\r\n", stdout);
        else
            fputc(*s, stdout);
    }
    fflush(stdout);
}

static ssize_t gather(unsigned char *buf, size_t cap)
{
    ssize_t n = read(STDIN_FILENO, buf, cap);
    if (n <= 0)
        return n;
    for (;;) {
        struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
        if (poll(&p, 1, 40) <= 0)
            break;
        ssize_t more = read(STDIN_FILENO, buf + n, cap - (size_t)n);
        if (more <= 0)
            break;
        n += more;
        if ((size_t)n >= cap)
            break;
    }
    return n;
}

int main(void)
{
    char ident[256];
    snprintf(ident, sizeof ident, "TERM=%s TERM_PROGRAM=%s TMUX=%s KITTY_WINDOW_ID=%s\n",
             getenv("TERM") ? getenv("TERM") : "",
             getenv("TERM_PROGRAM") ? getenv("TERM_PROGRAM") : "",
             getenv("TMUX") ? "yes" : "",
             getenv("KITTY_WINDOW_ID") ? "yes" : "");
    log_str(ident);

    if (tty_raw_begin() != 0) {
        fprintf(stderr, "keydump: not a terminal\n");
        return 1;
    }

    put("\x1b[?1049h");
    tty_keyboard_on();
    put("\x1b[?u");

    put("ctrl-tab, then a plain tab, then q to quit\r\n");
    put("if ctrl-tab prints nothing, the terminal is keeping it\r\n\r\n");

    unsigned char buf[256];
    for (;;) {
        struct pollfd p = {.fd = STDIN_FILENO, .events = POLLIN};
        if (poll(&p, 1, -1) <= 0)
            continue;
        ssize_t n = gather(buf, sizeof buf);
        if (n <= 0)
            break;
        dump_bytes(buf, n);
        if (n == 1 && (buf[0] == 'q' || buf[0] == 0x03))
            break;
    }

    put("\x1b[?1049l");
    tty_raw_end();
    fwrite(logbuf, 1, logn, stdout);
    return 0;
}
