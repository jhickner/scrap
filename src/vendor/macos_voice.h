#ifndef MACOS_VOICE_H
#define MACOS_VOICE_H

/* Hands-free voice conversation for a terminal program: the signed helper app listens
 * through the microphone, decides when a turn is finished, and reads back whatever text the
 * client sends. The helper is a separate .app because processes started under a terminal or
 * tmux are denied the microphone by TCC.
 *
 * Events, in the callback's `kind`:
 *   ready      listening for the first time (text NULL)
 *   partial    the running hypothesis for the current turn
 *   final      a finished turn, ready to send
 *   interrupt  the user talked over the reply; cancel the turn in flight
 *   speaking   "1" while a reply is being read aloud, "0" when it stops
 *   mode       idle | starting | listening | answering | speaking
 *   error      the helper failed; it is stopping
 */

typedef struct macos_voice macos_voice;
typedef struct {
    const char *helper_path; /* path to VoiceHelper.app                       */
    const char *voice;       /* voice name or identifier; NULL -> helper default */
    double      rate;        /* AVSpeechUtterance rate, 0 -> default          */
    double      silence;     /* baseline end-of-turn silence, 0 -> default    */
    const char *input;       /* input device name substring; NULL -> default  */
    int         timeout_ms;  /* wait for the helper to connect; 0 -> 15000    */
    void      (*tick)(void *ud); /* while waiting for the helper to connect   */
    void       *tick_ud;
} macos_voice_opts;
typedef void (*macos_voice_cb)(void *ud, const char *kind, const char *text);

macos_voice *macos_voice_start(const macos_voice_opts *opts, macos_voice_cb cb, void *ud);
int  macos_voice_fd(const macos_voice *v);
int  macos_voice_poll(macos_voice *v, int timeout_ms);

/* Reply text as it arrives; the helper speaks it sentence by sentence. */
int  macos_voice_say(macos_voice *v, const char *text);
/* The reply is complete: flush what is left and listen again. */
int  macos_voice_finish(macos_voice *v);
/* The turn was abandoned: drop anything unspoken. */
int  macos_voice_cancel(macos_voice *v);
/* Silence the rest of this reply without abandoning the turn. */
int  macos_voice_mute(macos_voice *v);
/* While busy, a turn spoken mid-answer is held until the answer completes. */
int  macos_voice_busy(macos_voice *v, int busy);
void macos_voice_stop(macos_voice *v);
#endif

#ifdef MACOS_VOICE_IMPLEMENTATION
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

struct macos_voice {
    int server, fd;
    char path[104];
    char *buf; size_t len, cap;
    macos_voice_cb cb; void *ud;
};

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
    else if (!strncmp(line, "SPEAKING ", 9)) v->cb(v->ud, "speaking", line + 9);
    else if (!strncmp(line, "MODE ", 5)) v->cb(v->ud, "mode", line + 5);
    else if (!strncmp(line, "ERR ", 4)) v->cb(v->ud, "error", line + 4);
}

macos_voice *macos_voice_start(const macos_voice_opts *o, macos_voice_cb cb, void *ud) {
    if (!o || !o->helper_path || !*o->helper_path || !cb) return NULL;
    macos_voice *v = calloc(1, sizeof *v); if (!v) return NULL;
    v->server = v->fd = -1; v->cb = cb; v->ud = ud;
    snprintf(v->path, sizeof v->path, "/tmp/macos-voice-%d.sock", (int)getpid()); unlink(v->path);
    v->server = socket(AF_UNIX, SOCK_STREAM, 0); if (v->server < 0) { free(v); return NULL; }
    /* the helper quits on EOF, which children that inherit the socket would hold open */
    fcntl(v->server, F_SETFD, FD_CLOEXEC);
    struct sockaddr_un sa = {0}; sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", v->path);
    if (bind(v->server, (struct sockaddr *)&sa, sizeof sa) || listen(v->server, 1)) {
        close(v->server); unlink(v->path); free(v); return NULL;
    }
    char rate[32] = "", silence[32] = "";
    if (o->rate > 0) snprintf(rate, sizeof rate, "%g", o->rate);
    if (o->silence > 0) snprintf(silence, sizeof silence, "%g", o->silence);
    pid_t pid = fork();
    if (!pid) {
        const char *av[24]; int n = 0;
        av[n++] = "open"; av[n++] = "-gn"; av[n++] = o->helper_path; av[n++] = "--args";
        av[n++] = "--socket"; av[n++] = v->path;
        if (o->voice && *o->voice) { av[n++] = "--voice"; av[n++] = o->voice; }
        if (*rate)    { av[n++] = "--rate"; av[n++] = rate; }
        if (*silence) { av[n++] = "--silence"; av[n++] = silence; }
        if (o->input && *o->input) { av[n++] = "--input"; av[n++] = o->input; }
        av[n] = NULL;
        execvp("open", (char *const *)av); _exit(127);
    }
    if (pid < 0) { macos_voice_stop(v); return NULL; }
    int status = 0; waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) { macos_voice_stop(v); return NULL; }
    int left = o->timeout_ms > 0 ? o->timeout_ms : 15000;
    for (;;) {
        int slice = o->tick && left > 90 ? 90 : left;
        struct pollfd p = { v->server, POLLIN, 0 };
        int r = poll(&p, 1, slice);
        if (r > 0) break;
        if (r < 0 && errno != EINTR) { macos_voice_stop(v); return NULL; }
        if (r == 0) {
            left -= slice;
            if (left <= 0) { macos_voice_stop(v); return NULL; }
        }
        if (o->tick) o->tick(o->tick_ud);
    }
    v->fd = accept(v->server, NULL, NULL);
    if (v->fd < 0) { macos_voice_stop(v); return NULL; }
    fcntl(v->fd, F_SETFD, FD_CLOEXEC);
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

int  macos_voice_say(macos_voice *v, const char *text) { return text && *text ? mv_write(v, "SAY", text) : 0; }
int  macos_voice_finish(macos_voice *v) { return mv_write(v, "FINISH", NULL); }
int  macos_voice_cancel(macos_voice *v) { return mv_write(v, "CANCEL", NULL); }
int  macos_voice_mute(macos_voice *v)   { return mv_write(v, "MUTE", NULL); }
int  macos_voice_busy(macos_voice *v, int busy) { return mv_write(v, "BUSY", busy ? "1" : "0"); }

void macos_voice_stop(macos_voice *v) {
    if (!v) return;
    if (v->fd >= 0) { mv_write(v, "QUIT", NULL); close(v->fd); }
    if (v->server >= 0) close(v->server);
    unlink(v->path); free(v->buf); free(v);
}
#endif
