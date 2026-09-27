#include "tty.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "viewport.h"

#define BRACKETED_PASTE_ON  "\x1b[?2004h"
#define FOCUS_ENV           "SCRAP_FOCUSED"
#define FOCUS_ON            "\x1b[?1004h"
#define FOCUS_OFF           "\x1b[?1004l"

#define KEYBOARD_PUSH "\x1b[>1u\x1b[>4;2m"
#define KEYBOARD_SET  "\x1b[=1u\x1b[>4;2m"
#define KEYBOARD_OFF  "\x1b[>4;0m\x1b[<u"

#define MODE_RESTORE \
    "\x1b[?2026l" \
    "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1006l" \
    "\x1b[?2004l" \
    FOCUS_OFF \
    "\x1b[?25h" \
    "\x1b[?7h" \
    "\x1b[0m" \
    "\x1b]112\x07"

#define CRASH_RESTORE MODE_RESTORE "\x1b[?1049l"

#define ESC_GRACE_MS 30

static struct termios entry_mode;
static int have_entry;
static int in_raw;
static volatile sig_atomic_t keyboard_active;
static volatile sig_atomic_t got_winch;

static unsigned char pending[4096];
static size_t pending_len, pending_pos;

static volatile sig_atomic_t winch_count;

static void on_winch(int sig) { (void)sig; got_winch = 1; winch_count++; }

static volatile sig_atomic_t quit_signal;

static int apply_mode(const struct termios *t, int when)
{
    int rc;
    do {
        rc = tcsetattr(STDIN_FILENO, when, t);
    } while (rc != 0 && errno == EINTR);
    if (rc == 0 || when == TCSANOW)
        return rc;
    do {
        rc = tcsetattr(STDIN_FILENO, TCSANOW, t);
    } while (rc != 0 && errno == EINTR);
    return rc;
}

static void cooked_sane(struct termios *t)
{
    t->c_iflag |= ICRNL;
    t->c_oflag |= OPOST;
#ifdef ONLCR
    t->c_oflag |= ONLCR;
#endif
    t->c_lflag |= ECHO | ICANON | ISIG | IEXTEN;
}

static void snapshot_entry(const struct termios *now)
{
    if (have_entry)
        return;
    entry_mode = *now;
    if (!(entry_mode.c_lflag & ECHO) || !(entry_mode.c_lflag & ICANON))
        cooked_sane(&entry_mode);
    have_entry = 1;
}

static void on_fatal(int sig)
{
    if (in_raw || have_entry) {
        if (keyboard_active) {
            (void)!write(STDOUT_FILENO, KEYBOARD_OFF, sizeof KEYBOARD_OFF - 1);
            keyboard_active = 0;
        }
        (void)!write(STDOUT_FILENO, CRASH_RESTORE, sizeof CRASH_RESTORE - 1);
        if (have_entry)
            apply_mode(&entry_mode, TCSANOW);
        in_raw = 0;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void on_quit(int sig)
{
    if (quit_signal) {
        on_fatal(sig);
        return;
    }
    quit_signal = sig;
}

int tty_quit_requested(void) { return quit_signal != 0; }

int tty_is_raw(void) { return in_raw; }

int tty_cooked_termios(struct termios *out)
{
    if (in_raw) {
        *out = entry_mode;
        return 0;
    }
    return tcgetattr(STDIN_FILENO, out) == 0 ? 0 : -1;
}

size_t tty_take_pending(void *buf, size_t max)
{
    size_t have = pending_len - pending_pos;
    if (have > max)
        have = max;
    memcpy(buf, pending + pending_pos, have);
    pending_pos += have;
    return have;
}

int tty_input_waiting(void)
{
    int n = 0;
    if (pending_pos < pending_len)
        return 1;
    return ioctl(STDIN_FILENO, FIONREAD, &n) == 0 && n > 0;
}

unsigned tty_resize_epoch(void) { return (unsigned)winch_count; }

static int env_size(const char *name, int fallback)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return fallback;
    int n = atoi(v);
    return n > 0 ? n : fallback;
}

int tty_screen_columns(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return ws.ws_col;
    return env_size("COLUMNS", 80);
}

int tty_columns(void)
{
    int cols = tty_screen_columns();
    return cols < TTY_MIN_COLUMNS ? TTY_MIN_COLUMNS : cols;
}

int tty_rows(void)
{
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
        return ws.ws_row < 3 ? 3 : ws.ws_row;
    int n = env_size("LINES", 24);
    return n < 3 ? 3 : n;
}

static int focused = 1;

static int tmux_pane_focused(void)
{
    const char *pane = getenv("TMUX_PANE");
    if (!pane || !*pane)
        return 1;
    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "tmux display-message -p -t '%s' "
             "'#{&&:#{pane_active},#{window_active}}' 2>/dev/null",
             pane);
    FILE *f = popen(cmd, "r");
    if (!f)
        return 1;
    char out[8] = {0};
    if (!fgets(out, sizeof out, f)) {
        pclose(f);
        return 1;
    }
    pclose(f);
    return out[0] == '1';
}

int tty_raw_begin(void)
{
    if (in_raw)
        return 0;
    if (!isatty(STDIN_FILENO))
        return -1;

    struct termios now;
    if (tcgetattr(STDIN_FILENO, &now) != 0)
        return -1;
    snapshot_entry(&now);

    struct termios raw = now;
    raw.c_iflag &= ~(unsigned long)(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    raw.c_oflag &= ~(unsigned long)(OPOST);
    raw.c_lflag &= ~(unsigned long)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (apply_mode(&raw, TCSANOW) != 0)
        return -1;

    in_raw = 1;
    struct sigaction sa = {0};
    sa.sa_handler = on_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);

    static const int FAULT[] = {SIGSEGV, SIGBUS, SIGABRT};
    struct sigaction fs = {0};
    fs.sa_handler = on_fatal;
    sigemptyset(&fs.sa_mask);
    for (size_t i = 0; i < sizeof FAULT / sizeof *FAULT; i++)
        sigaction(FAULT[i], &fs, NULL);

    static const int QUIT[] = {SIGHUP, SIGINT, SIGTERM, SIGQUIT};
    struct sigaction qs = {0};
    qs.sa_handler = on_quit;
    sigemptyset(&qs.sa_mask);
    for (size_t i = 0; i < sizeof QUIT / sizeof *QUIT; i++)
        sigaction(QUIT[i], &qs, NULL);

    signal(SIGPIPE, SIG_IGN);

    const char *carried = getenv(FOCUS_ENV);
    if (carried) {
        focused = *carried != '0';
        unsetenv(FOCUS_ENV);
    } else if (getenv("TMUX")) {
        focused = tmux_pane_focused();
    }

    fputs(BRACKETED_PASTE_ON FOCUS_ON, stdout);
    tty_keyboard_on();
    fflush(stdout);
    if (getenv("TMUX"))
        (void)system("tmux set-option -p extended-keys on >/dev/null 2>&1; "
                     "tmux set-option -p focus-events on >/dev/null 2>&1");
    return 0;
}

static void (*focus_fn)(int on);
int tty_focused(void)
{
    return focused;
}

void tty_on_focus(void (*fn)(int on))
{
    focus_fn = fn;
}

void tty_keyboard_on(void)
{
    fputs(keyboard_active ? KEYBOARD_SET : KEYBOARD_PUSH, stdout);
    keyboard_active = 1;
}

void tty_keyboard_off(void)
{
    if (keyboard_active) {
        fputs(KEYBOARD_OFF, stdout);
        keyboard_active = 0;
    }
}

static void raw_end(const char *restore)
{
    if (!in_raw && !have_entry)
        return;
    if (in_raw) {
        tty_keyboard_off();
        fputs(restore, stdout);
        fflush(stdout);
        tcflush(STDIN_FILENO, TCIFLUSH);
        in_raw = 0;
    }
    if (have_entry)
        apply_mode(&entry_mode, TCSANOW);
}

void tty_raw_end(void)
{
    raw_end(CRASH_RESTORE);
}

void tty_raw_handoff(void)
{
    setenv(FOCUS_ENV, tty_focused() ? "1" : "0", 1);
    raw_end(MODE_RESTORE);
}

static int  (*watch_fds)(void *ud, int *out, int max);
static void (*watch_ready)(void *ud);
static void *watch_ud;

void tty_watch(int (*fds)(void *ud, int *out, int max), void (*ready)(void *ud),
               void *ud)
{
    watch_fds = fds;
    watch_ready = ready;
    watch_ud = ud;
}

int tty_watch_fds(int *out, int max)
{
    int n = watch_fds ? watch_fds(watch_ud, out, max) : 0;
    return n < 0 ? 0 : n;
}

void tty_watch_ready(void)
{
    if (watch_ready)
        watch_ready(watch_ud);
}

static volatile sig_atomic_t woken;

static int seq_depth;

void tty_wake(void) { woken = 1; }

static int wake_latched(void)
{
    if (seq_depth || !(got_winch || quit_signal || woken))
        return 0;
    woken = 0;
    return 1;
}

static long clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_readable(int timeout_ms)
{
    long deadline = timeout_ms < 0 ? 0 : clock_ms() + timeout_ms;

    for (;;) {
        if (wake_latched())
            return 0;

        int extra[TTY_WATCH_MAX];
        int count = watch_fds && !seq_depth ? watch_fds(watch_ud, extra, TTY_WATCH_MAX) : 0;
        if (count < 0)
            count = 0;

        struct pollfd pfd[TTY_WATCH_MAX + 1];
        int           n = 0;
        pfd[n].fd = STDIN_FILENO;
        pfd[n].events = POLLIN;
        pfd[n++].revents = 0;
        for (int i = 0; i < count; i++) {
            if (extra[i] < 0)
                continue;
            pfd[n].fd = extra[i];
            pfd[n].events = POLLIN;
            pfd[n++].revents = 0;
        }

        int left = -1;
        if (timeout_ms >= 0) {
            long ms = deadline - clock_ms();
            left = ms <= 0 ? 0 : (int)ms;
        }
        int r = poll(pfd, (nfds_t)n, left);
        if (r < 0) {
            if (errno != EINTR)
                return -1;
            continue;
        }
        if (r == 0)
            return 0;
        if (pfd[0].revents)
            return 1;

        int woke = 0;
        for (int i = 1; i < n && !woke; i++)
            woke = pfd[i].revents != 0;
        if (!woke)
            return 0;
        watch_ready(watch_ud);
        if (timeout_ms >= 0 && clock_ms() >= deadline)
            return 0;
    }
}

static void pending_push(const unsigned char *p, size_t n)
{
    size_t have = pending_len - pending_pos;
    memmove(pending, pending + pending_pos, have);
    pending_pos = 0;
    pending_len = have;
    if (n > sizeof pending - pending_len)
        n = sizeof pending - pending_len;
    memcpy(pending + pending_len, p, n);
    pending_len += n;
}

#define DSR_WAIT_MS 150

int tty_cursor_position(int *row, int *col)
{
    if (!in_raw)
        return 0;

    fflush(stdout);
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return 0;

    unsigned char keep[sizeof pending];
    size_t        nkeep = 0;
    unsigned char seq[32];
    size_t        nseq = 0;
    int           in_seq = 0, ok = 0;
    long          deadline = clock_ms() + DSR_WAIT_MS;

#define KEEP(p, n)                                       \
    do {                                                 \
        for (size_t k_ = 0; k_ < (n); k_++)              \
            if (nkeep < sizeof keep)                     \
                keep[nkeep++] = (p)[k_];                 \
    } while (0)

    while (!ok) {
        long left = deadline - clock_ms();
        if (left <= 0)
            break;
        struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
        int           r = poll(&pfd, 1, (int)left);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (r == 0)
            break;

        unsigned char buf[256];
        ssize_t       n = read(STDIN_FILENO, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }

        ssize_t i = 0;
        for (; i < n && !ok; i++) {
            unsigned char c = buf[i];
            if (!in_seq) {
                if (c == 0x1b) {
                    in_seq = 1;
                    nseq = 0;
                    seq[nseq++] = c;
                } else if (nkeep < sizeof keep) {
                    keep[nkeep++] = c;
                }
                continue;
            }
            if (nseq < sizeof seq - 1)
                seq[nseq++] = c;
            if (nseq == 2 && c != '[') {
                KEEP(seq, nseq);
                in_seq = 0;
                continue;
            }
            if (nseq < 3 || c == ';' || (c >= '0' && c <= '9'))
                continue;
            if (c == 'R') {
                int r0 = 0, c0 = 0;
                seq[nseq] = 0;
                if (sscanf((char *)seq + 2, "%d;%d", &r0, &c0) == 2 && r0 > 0 && c0 > 0) {
                    if (row)
                        *row = r0;
                    if (col)
                        *col = c0;
                    ok = 1;
                }
            } else {
                KEEP(seq, nseq);
            }
            in_seq = 0;
        }
        if (i < n)
            KEEP(buf + i, (size_t)(n - i));
    }
    if (in_seq)
        KEEP(seq, nseq);
#undef KEEP

    if (nkeep)
        pending_push(keep, nkeep);
    return ok;
}

static int refill(int timeout_ms)
{
    if (pending_pos < pending_len)
        return (int)(pending_len - pending_pos);
    pending_pos = pending_len = 0;

    viewport_flush();

    int ready = wait_readable(timeout_ms);
    if (ready <= 0)
        return ready < 0 ? -1 : 0;

    ssize_t n = read(STDIN_FILENO, pending, sizeof pending);
    if (n <= 0)
        return (n == 0 || errno != EINTR) ? -1 : 0;
    pending_len = (size_t)n;
    return (int)pending_len;
}

static int peek_byte(int timeout_ms)
{
    if (pending_pos >= pending_len && refill(timeout_ms) <= 0)
        return -1;
    return pending[pending_pos];
}

static int take_byte(int timeout_ms)
{
    int b = peek_byte(timeout_ms);
    if (b >= 0)
        pending_pos++;
    return b;
}

static void emit(tty_event *ev, tty_key key)
{
    ev->key = key;
    ev->cp = 0;
    ev->text = NULL;
    ev->row = 0;
    ev->col = 0;
}

static void read_paste(tty_event *ev)
{
    size_t cap = 256, len = 0;
    char *body = malloc(cap);
    if (!body) {
        emit(ev, TK_ESCAPE);
        return;
    }
    static const char END[] = "\x1b[201~";

    const size_t PASTE_MAX = 8u << 20;
    int full = 0;
    size_t matched = 0;
    for (;;) {
        int b = take_byte(2000);
        if (b < 0)
            break;
        if ((char)b == END[matched]) {
            if (++matched == sizeof END - 1)
                break;
            continue;
        }
        if (len >= PASTE_MAX)
            full = 1;
        if (full) {
            matched = 0;
            continue;
        }

        for (size_t i = 0; i < matched; i++) {
            if (len + 2 > cap) {
                char *g = realloc(body, cap * 2);
                if (!g)
                    goto done;
                body = g;
                cap *= 2;
            }
            body[len++] = END[i];
        }
        matched = 0;
        if (len + 2 > cap) {
            char *g = realloc(body, cap * 2);
            if (!g)
                break;
            body = g;
            cap *= 2;
        }
        body[len++] = (char)b;
    }
done:
    body[len] = '\0';
    ev->key = TK_TEXT;
    ev->cp = 0;
    ev->text = body;
}

static void decode_mouse(tty_event *ev, const int *params, int nparams, int final)
{
    if (final != 'M' || nparams < 1) {
        emit(ev, TK_NONE);
        return;
    }
    switch (params[0] & ~0x1c) {
    case 64: emit(ev, TK_SCROLL_UP); return;
    case 65: emit(ev, TK_SCROLL_DOWN); return;
    case 0:
        if (nparams < 3) {
            emit(ev, TK_NONE);
            return;
        }
        emit(ev, TK_MOUSE_DOWN);
        ev->col = params[1];
        ev->row = params[2];
        return;
    default: emit(ev, TK_NONE); return;
    }
}

static void emit_modified_tab(tty_event *ev, int mods)
{
    int bits = mods > 1 ? mods - 1 : 0;
    if (bits & 4)
        emit(ev, (bits & 1) ? TK_PREV_TAB : TK_NEXT_TAB);
    else
        emit(ev, (bits & 1) ? TK_PREV_TAB : TK_TAB);
}

static void focus_change(tty_event *ev, int on)
{
    focused = on;
    if (focus_fn)
        focus_fn(focused);
    emit(ev, focused ? TK_FOCUS_IN : TK_FOCUS_OUT);
}

static void emit_codepoint(tty_event *ev, uint32_t cp)
{
    switch (cp) {
    case 13: emit(ev, TK_ENTER); return;
    case 10: emit(ev, TK_NEWLINE); return;
    case 9: emit(ev, TK_TAB); return;
    case 8: case 127: emit(ev, TK_BACKSPACE); return;
    case 27: emit(ev, TK_ESCAPE); return;
    default: emit(ev, TK_CHAR); ev->cp = cp; return;
    }
}

static void decode_modified(tty_event *ev, int key, int mods, int shifted)
{
    int bits = mods > 1 ? mods - 1 : 0;
    int shortcuts = bits & 63;

    if (key >= 57399 && key <= 57408) key = '0' + key - 57399;
    switch (key) {
    case 57409: key = '.'; break;
    case 57410: key = '/'; break;
    case 57411: key = '*'; break;
    case 57412: key = '-'; break;
    case 57413: key = '+'; break;
    case 57414: key = 13; break;
    case 57415: key = '='; break;
    case 57416: key = ','; break;
    case 57417: emit(ev, (bits & 6) ? TK_WORD_LEFT : TK_LEFT); return;
    case 57418: emit(ev, (bits & 6) ? TK_WORD_RIGHT : TK_RIGHT); return;
    case 57419: emit(ev, TK_UP); return;
    case 57420: emit(ev, TK_DOWN); return;
    case 57421: emit(ev, TK_PAGE_UP); return;
    case 57422: emit(ev, TK_PAGE_DOWN); return;
    case 57423: emit(ev, TK_HOME); return;
    case 57424: emit(ev, TK_END); return;
    case 57426: emit(ev, TK_DELETE); return;
    }
    if (key < 0 || key > 0x10ffff || (key >= 0xd800 && key <= 0xdfff) ||
        (key >= 57344 && key <= 63743)) {

        emit(ev, TK_NONE);
        return;
    }
    if (key == 13) {
        emit(ev, shortcuts ? TK_NEWLINE : TK_ENTER);
        return;
    }
    if (key == 9) {
        emit_modified_tab(ev, mods);
        return;
    }
    if (bits & 4) {
        if (key >= 'a' && key <= 'z') key -= 'a' - 'A';
        if (key >= '@' && key <= '_') key &= 31;
        else if (key == ' ' || key == '2') key = 0;
        else if (key >= '3' && key <= '7') key = key - '3' + 27;
        else if (key == '/') key = 31;
        else if (key == '~') key = 30;
        else if (key == '8' || key == '?') key = 127;
    } else if (bits & 2) {
        switch (key) {
        case 'b': emit(ev, TK_WORD_LEFT); return;
        case 'f': emit(ev, TK_WORD_RIGHT); return;
        case 127: emit(ev, TK_WORD_LEFT); return;
        default: emit(ev, TK_NONE); return;
        }
    } else if (bits & (8 | 16 | 32)) {
        emit(ev, TK_NONE);
        return;
    } else if ((bits & 1) && shifted > 0 && shifted <= 0x10ffff &&
               !(shifted >= 0xd800 && shifted <= 0xdfff)) {
        key = shifted;
    }
    emit_codepoint(ev, (uint32_t)key);
}

static void decode_csi(tty_event *ev, const int *params, int nparams, int final,
                       int event, int shifted)
{
    if (event != 1 && event != 2) {
        emit(ev, TK_NONE);
        return;
    }
    int mods = nparams >= 2 ? params[1] : 1;
    int bits = mods > 1 ? mods - 1 : 0;
    int ctrl_or_alt = bits & (2 | 4 | 8);

    switch (final) {
    case 'A': emit(ev, TK_UP); return;
    case 'B': emit(ev, TK_DOWN); return;
    case 'C': emit(ev, ctrl_or_alt ? TK_WORD_RIGHT : TK_RIGHT); return;
    case 'D': emit(ev, ctrl_or_alt ? TK_WORD_LEFT : TK_LEFT); return;
    case 'H': emit(ev, TK_HOME); return;
    case 'F': emit(ev, TK_END); return;
    case 'I': focus_change(ev, 1); return;
    case 'O': focus_change(ev, 0); return;
    case 'Z': emit(ev, TK_PREV_TAB); return;
    case 'P': emit(ev, TK_F1); return;
    case 'u':
        if (nparams >= 1) {
            decode_modified(ev, params[0], mods, shifted);
            return;
        }
        break;
    case '~':
        if (nparams == 3 && params[0] == 27) {
            decode_modified(ev, params[2], mods, 0);
            return;
        }
        switch (nparams >= 1 ? params[0] : 0) {
        case 1: case 7: emit(ev, TK_HOME); return;
        case 3:         emit(ev, TK_DELETE); return;
        case 4: case 8: emit(ev, TK_END); return;
        case 5:         emit(ev, TK_PAGE_UP); return;
        case 6:         emit(ev, TK_PAGE_DOWN); return;
        case 11:        emit(ev, TK_F1); return;
        default: break;
        }
    }
    emit(ev, TK_NONE);
}

static void skip_string(tty_event *ev)
{
    for (size_t i = 0; i < 64u << 10; i++) {
        int c = take_byte(50);
        if (c < 0)
            break;
        if (c == 0x07)
            break;
        if (c == 0x1b) {
            if (peek_byte(50) == '\\')
                pending_pos++;
            break;
        }
    }
    emit(ev, TK_NONE);
}

static void decode_escape(tty_event *ev)
{
    int b = peek_byte(ESC_GRACE_MS);
    if (b < 0) {
        emit(ev, TK_ESCAPE);
        return;
    }

    if (b == '[') {
        pending_pos++;
        int params[8][3] = {{0}};
        int field = 0, sub = 0, have_params = 0, private = 0, invalid = 0;
        for (;;) {
            int c = take_byte(50);
            if (c < 0) {
                emit(ev, TK_NONE);
                return;
            }
            if (c >= '0' && c <= '9') {
                have_params = 1;
                if (field < 8 && sub < 3) {
                    int *v = &params[field][sub];
                    if (*v > (INT_MAX - (c - '0')) / 10)
                        invalid = 1;
                    else
                        *v = *v * 10 + (c - '0');
                }
                continue;
            }
            if (c == ':' || c == ';') {
                have_params = 1;
                if (c == ':') {
                    if (sub < 3) sub++;
                } else {
                    if (field < 8) field++;
                    else invalid = 1;
                    sub = 0;
                }
                continue;
            }
            if (c == '?' || c == '<' || c == '>' || c == '=') {
                if (have_params || private) invalid = 1;
                private = c;
                continue;
            }
            if (c >= 0x20 && c <= 0x2f) {
                invalid = 1;
                continue;
            }
            if (c < 0x40 || c > 0x7e) {
                emit(ev, TK_NONE);
                return;
            }
            if (invalid || field >= 8 || (private && private != '<')) {
                emit(ev, TK_NONE);
                return;
            }
            int flat[8];
            for (int i = 0; i < 8; i++) flat[i] = params[i][0];
            int nparams = have_params ? field + 1 : 0;
            if (private == '<') {
                decode_mouse(ev, flat, nparams, c);
                return;
            }
            if (c == '~' && nparams == 1 && flat[0] == 200) {
                read_paste(ev);
                return;
            }
            int event = params[1][1] ? params[1][1] : 1;
            decode_csi(ev, flat, nparams, c, event, params[0][1]);
            return;
        }
    }

    if (b == 'O') {
        pending_pos++;
        int c = take_byte(50);
        switch (c) {
        case 'A': emit(ev, TK_UP); return;
        case 'B': emit(ev, TK_DOWN); return;
        case 'C': emit(ev, TK_RIGHT); return;
        case 'D': emit(ev, TK_LEFT); return;
        case 'H': emit(ev, TK_HOME); return;
        case 'F': emit(ev, TK_END); return;
        case 'P': emit(ev, TK_F1); return;
        default:  emit(ev, TK_NONE); return;
        }
    }

    if (b == ']' || b == 'P' || b == '_' || b == '^' || b == 'X') {
        pending_pos++;
        skip_string(ev);
        return;
    }

    pending_pos++;
    switch (b) {
    case 'b': emit(ev, TK_WORD_LEFT); return;
    case 'f': emit(ev, TK_WORD_RIGHT); return;
    case '\r': case '\n': emit(ev, TK_NEWLINE); return;
    case 0x7f: emit(ev, TK_WORD_LEFT); return;
    case '\\': emit(ev, TK_NONE); return;
    default: emit(ev, TK_ESCAPE); return;
    }
}

static uint32_t decode_utf8(int lead)
{
    int extra;
    uint32_t cp;
    if (lead < 0xC0)      return '?';
    else if (lead < 0xE0) { cp = (uint32_t)lead & 0x1F; extra = 1; }
    else if (lead < 0xF0) { cp = (uint32_t)lead & 0x0F; extra = 2; }
    else                  { cp = (uint32_t)lead & 0x07; extra = 3; }

    for (int i = 0; i < extra; i++) {
        int c = peek_byte(50);
        if (c < 0 || (c & 0xC0) != 0x80)
            return '?';
        pending_pos++;
        cp = (cp << 6) | ((uint32_t)c & 0x3F);
    }
    return cp;
}

int tty_read(tty_event *ev, int timeout_ms)
{
    if (got_winch) {
        got_winch = 0;
        emit(ev, TK_RESIZE);
        return 1;
    }

    int avail = refill(timeout_ms);
    if (avail == 0) {
        if (got_winch) {
            got_winch = 0;
            emit(ev, TK_RESIZE);
            return 1;
        }
        return 0;
    }
    if (avail < 0) {
        emit(ev, TK_EOF);
        return 1;
    }

    int b = pending[pending_pos++];
    switch (b) {
    case 0x1b:
        seq_depth++;
        decode_escape(ev);
        seq_depth--;

        return ev->key == TK_NONE ? 0 : 1;
    case '\r': emit(ev, TK_ENTER); return 1;
    case '\n': emit(ev, TK_NEWLINE); return 1;
    case '\t': emit(ev, TK_TAB); return 1;
    case 0x7f:
    case 0x08: emit(ev, TK_BACKSPACE); return 1;
    default: break;
    }

    ev->key = TK_CHAR;
    ev->text = NULL;
    if (b < 0x80) {
        ev->cp = (uint32_t)b;
    } else {
        seq_depth++;
        ev->cp = decode_utf8(b);
        seq_depth--;
    }
    return 1;
}
