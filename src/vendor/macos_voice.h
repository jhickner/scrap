#ifndef MACOS_VOICE_H
#define MACOS_VOICE_H

/* Hands-free voice conversation for terminal programs. One signed helper per user owns the
 * microphone and speech engine. Any number of clients may connect; the most recently focused
 * client receives transcripts and controls spoken output.
 *
 * Events, in the callback's `kind`:
 *   ready      helper is listening (text NULL)
 *   partial    the running hypothesis for the current turn
 *   final      a finished turn, ready to send
 *   interrupt  the stop word; cancel the turn in flight
 *   dropped    the turn in progress was discarded without a final (text NULL)
 *   cancelled  the turn in progress was cancelled by a spoken cancel phrase (text NULL)
 *   speaking   "1" while a reply is being read aloud, "0" when it stops
 *   mode       idle | starting | listening | answering | speaking
 *   mic        "0" when another client turned the microphone off for every client
 *   notice     something to show the user; the helper keeps running
 *   error      the helper failed; it is stopping
 */

typedef struct macos_voice macos_voice;
typedef struct {
    const char *helper_path; /* path to VoiceHelper.app                         */
    const char *voice;       /* voice name or identifier; NULL -> helper default */
    double      rate;        /* AVSpeechUtterance rate, 0 -> default            */
    double      silence;     /* baseline end-of-turn silence, 0 -> default      */
    double      volume;      /* 0-1; 0 leaves the helper default (full)         */
    const char *input;       /* input device name substring; NULL -> default    */
    int         timeout_ms;  /* wait for the helper to connect; 0 -> 15000      */
    int       (*tick)(void *ud); /* while waiting; nonzero gives up             */
    void       *tick_ud;
} macos_voice_opts;
typedef void (*macos_voice_cb)(void *ud, const char *kind, const char *text);

macos_voice *macos_voice_start(const macos_voice_opts *opts, macos_voice_cb cb, void *ud);
int  macos_voice_fd(const macos_voice *v);
int  macos_voice_poll(macos_voice *v, int timeout_ms);
int  macos_voice_say(macos_voice *v, const char *text);
/* Speak without taking focus, so an unfocused client can cue without the mic. */
int  macos_voice_announce(macos_voice *v, const char *text);
int  macos_voice_finish(macos_voice *v);
int  macos_voice_cancel(macos_voice *v);
int  macos_voice_mute(macos_voice *v);
/* Play an acknowledgement tone: "sent", "interrupted" or "listening". The helper does
 * not chime on its own, since only the client knows whether the turn is being used. */
int  macos_voice_chime(macos_voice *v, const char *name);
/* Off releases the input device so another app can take it; a reply in progress is cut off. */
int  macos_voice_mic(macos_voice *v, int on);
/* Off for every client: the others receive a mic "0" event. A helper without
 * support turns off only this client. */
int  macos_voice_mic_off_all(macos_voice *v);
int  macos_voice_busy(macos_voice *v, int busy);
int  macos_voice_volume(macos_voice *v, double volume);
/* AVSpeechUtterance rate, 0-1; the utterance in progress keeps its own rate. */
int  macos_voice_rate(macos_voice *v, double rate);
/* baseline end-of-turn silence in seconds */
int  macos_voice_silence(macos_voice *v, double seconds);
/* Only the focused client receives microphone events or controls output. */
int  macos_voice_focus(macos_voice *v, int focused);
/* Preserve the connection and its helper-side focus across exec. */
int  macos_voice_handoff(macos_voice *v);
/* Call at process entry, before spawning children, to protect an inherited fd. */
void macos_voice_protect_handoff(void);
int  macos_voice_resumed(const macos_voice *v);
/* Disconnect this client. The helper exits when the last client leaves. */
void macos_voice_stop(macos_voice *v);
/* End the helper for every client, then disconnect. */
void macos_voice_shutdown(macos_voice *v);
/* Kill helper processes left running after a shutdown. One that has already dropped
   its socket answers no probe but still holds the microphone, and a replacement
   cannot take the socket path from it. Returns the number killed. */
int  macos_voice_reap(const char *helper_path);
#endif

#ifdef MACOS_VOICE_IMPLEMENTATION
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

struct macos_voice {
    int fd;
    int resumed;
    char path[104];
    char *buf; size_t len, cap;
    macos_voice_cb cb; void *ud;
};

#define MV_HANDOFF_FD "MUX_VOICE_FD"
#define MV_HANDOFF_BUF "MUX_VOICE_PENDING"

static int mv_handoff_fd(void) {
    const char *s = getenv(MV_HANDOFF_FD);
    if (!s || !*s) return -1;
    char *end;
    long fd = strtol(s, &end, 10);
    if (*end || fd < 3 || fd > 0x7fffffff) return -1;
    struct sockaddr_un peer = {0};
    socklen_t size = sizeof peer;
    char path[104];
    snprintf(path, sizeof path, "/tmp/macos-voice-%u.sock", (unsigned)getuid());
    if (getpeername((int)fd, (struct sockaddr *)&peer, &size) != 0 ||
        peer.sun_family != AF_UNIX || strcmp(peer.sun_path, path)) return -1;
    return (int)fd;
}

void macos_voice_protect_handoff(void) {
    int fd = mv_handoff_fd();
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
}

int macos_voice_handoff(macos_voice *v) {
    if (!v || v->fd < 0) return -1;
    char fd[32];
    snprintf(fd, sizeof fd, "%d", v->fd);
    if (v->buf) v->buf[v->len] = 0;
    if (setenv(MV_HANDOFF_BUF, v->buf ? v->buf : "", 1) != 0 ||
        setenv(MV_HANDOFF_FD, fd, 1) != 0 ||
        fcntl(v->fd, F_SETFD, 0) != 0) {
        unsetenv(MV_HANDOFF_FD);
        unsetenv(MV_HANDOFF_BUF);
        return -1;
    }
    return 0;
}

int macos_voice_resumed(const macos_voice *v) { return v && v->resumed; }

static int mv_write(macos_voice *v, const char *verb, const char *text) {
    if (!v || v->fd < 0) return -1;
    size_t n = strlen(verb) + (text ? strlen(text) * 2 : 0) + 3;
    char *line = malloc(n); if (!line) return -1;
    char *p = line; p += sprintf(p, "%s", verb);
    if (text) {
        *p++ = ' ';
        for (const char *s = text; *s; s++) {
            if (*s == '\n') { *p++ = '\\'; *p++ = 'n'; }
            else if (*s != '\r') *p++ = *s;
        }
    }
    *p++ = '\n';
    size_t total = (size_t)(p - line), off = 0; int rc = 0;
    while (off < total) {
        ssize_t w = write(v->fd, line + off, total - off);
        if (w < 0) { if (errno == EINTR) continue; rc = -1; break; }
        off += (size_t)w;
    }
    free(line); return rc;
}

int macos_voice_reap(const char *helper_path);

static void mv_unlock(int lock) {
    if (lock < 0) return;
    flock(lock, LOCK_UN);
    close(lock);
}

static int mv_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0); if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct sockaddr_un sa = {0}; sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) { close(fd); return -1; }
    return fd;
}

static void mv_unescape(char *s) {
    char *d = s;
    for (; *s; s++) {
        if (s[0] == '\\' && s[1] == 'n') { *d++ = '\n'; s++; }
        else *d++ = *s;
    }
    *d = 0;
}

static void mv_emit(macos_voice *v, char *line) {
    if (!strcmp(line, "READY")) v->cb(v->ud, "ready", NULL);
    else if (!strncmp(line, "P ", 2)) { mv_unescape(line + 2); v->cb(v->ud, "partial", line + 2); }
    else if (!strncmp(line, "T ", 2)) { mv_unescape(line + 2); v->cb(v->ud, "final", line + 2); }
    else if (!strcmp(line, "INTERRUPT")) v->cb(v->ud, "interrupt", NULL);
    else if (!strcmp(line, "DROP")) v->cb(v->ud, "dropped", NULL);
    else if (!strcmp(line, "CANCEL")) v->cb(v->ud, "cancelled", NULL);
    else if (!strncmp(line, "SPEAKING ", 9)) v->cb(v->ud, "speaking", line + 9);
    else if (!strncmp(line, "MODE ", 5)) v->cb(v->ud, "mode", line + 5);
    else if (!strncmp(line, "MIC ", 4)) v->cb(v->ud, "mic", line + 4);
    else if (!strncmp(line, "NOTE ", 5)) v->cb(v->ud, "notice", line + 5);
    else if (!strncmp(line, "ERR ", 4)) v->cb(v->ud, "error", line + 4);
}

macos_voice *macos_voice_start(const macos_voice_opts *o, macos_voice_cb cb, void *ud) {
    if (!o || !o->helper_path || !*o->helper_path || !cb) return NULL;
    macos_voice *v = calloc(1, sizeof *v); if (!v) return NULL;
    v->fd = -1; v->cb = cb; v->ud = ud;
    snprintf(v->path, sizeof v->path, "/tmp/macos-voice-%u.sock", (unsigned)getuid());
    v->fd = mv_handoff_fd();
    unsetenv(MV_HANDOFF_FD);
    if (v->fd >= 0) {
        fcntl(v->fd, F_SETFD, FD_CLOEXEC);
        const char *pending = getenv(MV_HANDOFF_BUF);
        if (pending && *pending) {
            v->buf = strdup(pending);
            if (!v->buf) { unsetenv(MV_HANDOFF_BUF); macos_voice_stop(v); return NULL; }
            v->len = strlen(v->buf);
            v->cap = v->len + 1;
        }
        unsetenv(MV_HANDOFF_BUF);
        v->resumed = 1;
        signal(SIGPIPE, SIG_IGN);
        cb(ud, "ready", NULL);
        return v;
    }
    unsetenv(MV_HANDOFF_BUF);
    v->fd = mv_connect(v->path);

    char rate[32] = "", silence[32] = "", volume[32] = "";
    if (o->rate > 0) snprintf(rate, sizeof rate, "%g", o->rate);
    if (o->silence > 0) snprintf(silence, sizeof silence, "%g", o->silence);
    if (o->volume > 0) snprintf(volume, sizeof volume, "%g", o->volume);
    /* A helper that died without unlinking its socket, or one left running after a
       shutdown, answers nothing and holds the path a new one needs; so a failed
       attempt reaps the strays and tries once more. */
    /* Two sessions that both find no helper would each launch one, and the loser
       keeps running without clients while still holding the microphone and the
       Personal Voice. Only the holder of the lock may launch. */
    char lockpath[sizeof v->path];
    snprintf(lockpath, sizeof lockpath, "/tmp/macos-voice-%u.lock", (unsigned)getuid());
    int lock = v->fd < 0 ? open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600) : -1;
    if (lock >= 0) {
        int left = o->timeout_ms > 0 ? o->timeout_ms : 15000;
        while (flock(lock, LOCK_EX | LOCK_NB) && errno == EWOULDBLOCK) {
            v->fd = mv_connect(v->path);
            if (v->fd >= 0) break;
            if (left <= 0 || (o->tick && o->tick(o->tick_ud))) {
                close(lock); macos_voice_stop(v); return NULL;
            }
            poll(NULL, 0, 90);
            left -= 90;
        }
        if (v->fd < 0) v->fd = mv_connect(v->path); /* the session ahead may have started one */
    }
    for (int attempt = 0; v->fd < 0 && attempt < 2; attempt++) {
        if (attempt)
            macos_voice_reap(o->helper_path);
        pid_t pid = fork();
        if (!pid) {
            const char *av[24]; int n = 0;
            av[n++] = "open"; av[n++] = "-gn"; av[n++] = o->helper_path; av[n++] = "--args";
            av[n++] = "--socket"; av[n++] = v->path;
            if (o->voice && *o->voice) { av[n++] = "--voice"; av[n++] = o->voice; }
            if (*rate)    { av[n++] = "--rate"; av[n++] = rate; }
            if (*silence) { av[n++] = "--silence"; av[n++] = silence; }
            if (*volume)  { av[n++] = "--volume"; av[n++] = volume; }
            if (o->input && *o->input) { av[n++] = "--input"; av[n++] = o->input; }
            av[n] = NULL;
            execvp("open", (char *const *)av); _exit(127);
        }
        if (pid < 0) { mv_unlock(lock); macos_voice_stop(v); return NULL; }
        int status = 0; waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status)) { mv_unlock(lock); macos_voice_stop(v); return NULL; }

        for (int left = o->timeout_ms > 0 ? o->timeout_ms : 15000; left > 0;) {
            int slice = left > 90 ? 90 : left;
            poll(NULL, 0, slice);
            left -= slice;
            v->fd = mv_connect(v->path);
            if (v->fd >= 0) break;
            if (o->tick && o->tick(o->tick_ud)) { mv_unlock(lock); macos_voice_stop(v); return NULL; }
        }
    }
    mv_unlock(lock);
    if (v->fd < 0) { macos_voice_stop(v); return NULL; }

    signal(SIGPIPE, SIG_IGN);
    return v;
}

int macos_voice_fd(const macos_voice *v) { return v ? v->fd : -1; }

int macos_voice_poll(macos_voice *v, int timeout_ms) {
    if (!v || v->fd < 0) return -1;
    struct pollfd p = { v->fd, POLLIN, 0 };
    int r = poll(&p, 1, timeout_ms); if (r <= 0) return r;
    char in[4096]; ssize_t n = read(v->fd, in, sizeof in); if (n <= 0) return -1;
    int count = 0;
    for (ssize_t i = 0; i < n; i++) {
        if (in[i] == '\n') {
            if (v->buf) { v->buf[v->len] = 0; mv_emit(v, v->buf); }
            v->len = 0; count++;
            continue;
        }
        if (v->len + 2 > v->cap) {
            size_t cap = v->cap ? v->cap * 2 : 1024;
            char *grown = realloc(v->buf, cap); if (!grown) continue;
            v->buf = grown; v->cap = cap;
        }
        v->buf[v->len++] = in[i];
    }
    return count;
}

int macos_voice_say(macos_voice *v, const char *text) { return text && *text ? mv_write(v, "SAY", text) : 0; }
int macos_voice_announce(macos_voice *v, const char *text) { return text && *text ? mv_write(v, "ANNOUNCE", text) : 0; }
int macos_voice_finish(macos_voice *v) { return mv_write(v, "FINISH", NULL); }
int macos_voice_cancel(macos_voice *v) { return mv_write(v, "CANCEL", NULL); }
int macos_voice_mute(macos_voice *v) { return mv_write(v, "MUTE", NULL); }
int macos_voice_chime(macos_voice *v, const char *name) { return mv_write(v, "CHIME", name); }
int macos_voice_mic(macos_voice *v, int on) { return mv_write(v, "MIC", on ? "1" : "0"); }
int macos_voice_mic_off_all(macos_voice *v) { return mv_write(v, "MIC", "0 all"); }
int macos_voice_busy(macos_voice *v, int busy) { return mv_write(v, "BUSY", busy ? "1" : "0"); }
int macos_voice_volume(macos_voice *v, double volume) {
    if (volume < 0) volume = 0;
    if (volume > 1) volume = 1;
    char text[32];
    snprintf(text, sizeof text, "%g", volume);
    return mv_write(v, "VOLUME", text);
}
int macos_voice_rate(macos_voice *v, double rate) {
    if (rate < 0) rate = 0;
    if (rate > 1) rate = 1;
    char text[32];
    snprintf(text, sizeof text, "%g", rate);
    return mv_write(v, "RATE", text);
}
int macos_voice_silence(macos_voice *v, double seconds) {
    char text[32];
    snprintf(text, sizeof text, "%g", seconds);
    return mv_write(v, "SILENCE", text);
}
int macos_voice_focus(macos_voice *v, int focused) { return mv_write(v, "FOCUS", focused ? "1" : "0"); }

void macos_voice_shutdown(macos_voice *v) {
    if (!v) return;
    char path[sizeof v->path];
    snprintf(path, sizeof path, "%s", v->path);
    int asked = v->fd >= 0 && mv_write(v, "SHUTDOWN", NULL) == 0;
    macos_voice_stop(v);
    /* The helper exits on its own thread, so wait for the socket to stop answering
       before a caller starts a replacement and connects to the dying process. */
    for (int left = asked ? 3000 : 0; left > 0; left -= 20) {
        int probe = mv_connect(path);
        if (probe < 0) break;
        close(probe);
        poll(NULL, 0, 20);
    }
}

int macos_voice_reap(const char *helper_path) {
    if (!helper_path || !*helper_path) return 0;
    char cmd[8192];
    snprintf(cmd, sizeof cmd, "pgrep -f '^%s/Contents/MacOS/' 2>/dev/null", helper_path);
    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    pid_t pids[64]; int n = 0;
    while (n < (int)(sizeof pids / sizeof *pids)) {
        long pid = 0;
        if (fscanf(f, "%ld", &pid) != 1) break;
        if (pid > 1 && pid != getpid()) pids[n++] = (pid_t)pid;
    }
    pclose(f);
    for (int i = 0; i < n; i++) kill(pids[i], SIGTERM);
    for (int left = n ? 500 : 0; left > 0; left -= 20) {
        int alive = 0;
        for (int i = 0; i < n; i++)
            if (!kill(pids[i], 0)) alive = 1;
        if (!alive) return n;
        poll(NULL, 0, 20);
    }
    for (int i = 0; i < n; i++) kill(pids[i], SIGKILL);
    return n;
}

void macos_voice_stop(macos_voice *v) {
    if (!v) return;
    if (v->fd >= 0) { mv_write(v, "QUIT", NULL); close(v->fd); }
    free(v->buf); free(v);
}
#endif
