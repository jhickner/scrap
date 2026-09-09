/**
 * wsd.h — single-header WebSocket server for one client (C)
 *
 * Accepts one WebSocket client at a time on its own thread and hands text
 * messages to a callback. A new client replaces the old one, so a phone that
 * reconnects after sleeping does not have to wait for the stale socket to time
 * out. Sends go straight to the socket under a lock and may be called from
 * any thread.
 *
 * Only what a chat transport needs: RFC 6455 text and close frames, pings
 * answered, no extensions, no fragmentation, no TLS. Bind it to a Tailscale
 * address (see wsd_tailscale_ip) and require a token; treat it as a private
 * socket, not a public endpoint.
 *
 * USAGE (stb style):
 *
 *   // In exactly ONE .c file:
 *   #define WSD_IMPLEMENTATION
 *   #include "wsd.h"
 *
 *   wsd_opts o = { .bind_ip = wsd_tailscale_ip(), .port = 8790, .token = tok };
 *   wsd *w = wsd_start(&o, on_text, on_state, ud);
 *   wsd_send(w, "{\"t\":\"hello\"}", 0);
 *   wsd_stop(w);
 *
 * The client authenticates in the upgrade request, either with
 * `Authorization: Bearer <token>` or `?token=<token>` on the path.
 *
 * LINK: -lpthread
 */

#ifndef WSD_H
#define WSD_H

#include <stddef.h>

typedef struct wsd wsd;

typedef struct {
    const char *bind_ip; /* NULL/"" binds every interface                     */
    int         port;
    const char *token;   /* required in the upgrade request when non-empty    */
    size_t      max_message; /* incoming text limit, 0 -> 1 MiB               */
} wsd_opts;

/* Both run on the server thread. on_text gets a NUL-terminated copy that is
 * valid for the call only. on_state fires with 1 after a client's handshake
 * and 0 when it goes away (also when a new client displaces it). */
typedef void (*wsd_text_fn)(void *ud, const char *text, size_t n);
typedef void (*wsd_state_fn)(void *ud, int connected);

wsd *wsd_start(const wsd_opts *opts, wsd_text_fn on_text, wsd_state_fn on_state, void *ud);

/* Send a text frame to the connected client. n == 0 means strlen(text).
 * Returns 0 on success, -1 when no client is connected or the write failed. */
int wsd_send(wsd *w, const char *text, size_t n);

int wsd_connected(const wsd *w);

/* Close the client, stop the thread and free. Safe with NULL. */
void wsd_stop(wsd *w);

/* This machine's Tailscale address, or NULL when the tailnet is not up. Points
 * at a static buffer. */
const char *wsd_tailscale_ip(void);

#endif /* WSD_H */

/* ======================================================================== */
/*   IMPLEMENTATION                                                          */
/* ======================================================================== */
#ifdef WSD_IMPLEMENTATION

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct wsd {
    wsd_opts      opts;
    char          token[128];
    char          bind_ip[64];
    wsd_text_fn   on_text;
    wsd_state_fn  on_state;
    void         *ud;
    int           listen_fd;
    int           client_fd;
    int           stop_pipe[2];
    pthread_t     thread;
    pthread_mutex_t send_lock;
    unsigned char *buf;
    size_t        len, cap;
};

/* ---- sha1 + base64, for the handshake only ------------------------------ */

static void wsd_sha1(const unsigned char *msg, size_t len, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t total = ((len + 8) / 64 + 1) * 64;
    unsigned char *m = calloc(total, 1);
    if (!m) { memset(out, 0, 20); return; }
    memcpy(m, msg, len);
    m[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m[total - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (size_t off = 0; off < total; off += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[off + i * 4] << 24 | (uint32_t)m[off + i * 4 + 1] << 16 |
                   (uint32_t)m[off + i * 4 + 2] << 8 | m[off + i * 4 + 3];
        for (int i = 16; i < 80; i++) {
            uint32_t x = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (x << 1) | (x >> 31);
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
            uint32_t t = ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    free(m);
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (unsigned char)(h[i] >> 24); out[i * 4 + 1] = (unsigned char)(h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(h[i] >> 8); out[i * 4 + 3] = (unsigned char)h[i];
    }
}

static void wsd_b64(const unsigned char *in, size_t n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        out[o++] = t[in[i] >> 2];
        out[o++] = t[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        out[o++] = t[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)];
        out[o++] = t[in[i + 2] & 63];
    }
    if (i < n) {
        out[o++] = t[in[i] >> 2];
        if (i + 1 < n) {
            out[o++] = t[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
            out[o++] = t[(in[i + 1] & 15) << 2];
        } else {
            out[o++] = t[(in[i] & 3) << 4];
            out[o++] = '=';
        }
        out[o++] = '=';
    }
    out[o] = '\0';
}

/* ---- socket helpers ----------------------------------------------------- */

const char *wsd_tailscale_ip(void) {
    static char ip[INET_ADDRSTRLEN];
    struct ifaddrs *ifs = NULL;
    if (getifaddrs(&ifs) != 0) return NULL;
    ip[0] = '\0';
    for (struct ifaddrs *a = ifs; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        uint32_t v = ntohl(((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr);
        if ((v & 0xFFC00000u) == 0x64400000u) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)a->ifa_addr)->sin_addr, ip, sizeof ip);
            break;
        }
    }
    freeifaddrs(ifs);
    return ip[0] ? ip : NULL;
}

static int wsd_write_all(int fd, const void *p, size_t n) {
    const unsigned char *b = p;
    while (n) {
        ssize_t k = send(fd, b, n, 0);
        if (k < 0) { if (errno == EINTR) continue; return -1; }
        b += k; n -= (size_t)k;
    }
    return 0;
}

static int wsd_write_frame(int fd, int opcode, const void *data, size_t n) {
    unsigned char hdr[10];
    size_t hl = 2;
    hdr[0] = (unsigned char)(0x80 | opcode);
    if (n < 126) hdr[1] = (unsigned char)n;
    else if (n < 65536) { hdr[1] = 126; hdr[2] = (unsigned char)(n >> 8); hdr[3] = (unsigned char)n; hl = 4; }
    else { hdr[1] = 127; for (int i = 0; i < 8; i++) hdr[2 + i] = (unsigned char)((uint64_t)n >> (56 - 8 * i)); hl = 10; }
    if (wsd_write_all(fd, hdr, hl)) return -1;
    return n ? wsd_write_all(fd, data, n) : 0;
}

static void wsd_drop_client(wsd *w, int tell) {
    pthread_mutex_lock(&w->send_lock);
    int fd = w->client_fd;
    w->client_fd = -1;
    pthread_mutex_unlock(&w->send_lock);
    if (fd < 0) return;
    if (tell) wsd_write_frame(fd, 0x8, "\x03\xe8", 2);
    close(fd);
    w->len = 0;
    if (w->on_state) w->on_state(w->ud, 0);
}

/* ---- handshake ---------------------------------------------------------- */

static int wsd_header(const char *req, const char *name, char *out, size_t size) {
    size_t nl = strlen(name);
    for (const char *p = req; (p = strcasestr(p, name)) != NULL; p += nl) {
        if (p != req && p[-1] != '\n') continue;
        if (p[nl] != ':') continue;
        p += nl + 1;
        while (*p == ' ') p++;
        size_t k = strcspn(p, "\r\n");
        if (k >= size) k = size - 1;
        memcpy(out, p, k);
        out[k] = '\0';
        return 1;
    }
    return 0;
}

static int wsd_authorized(const wsd *w, const char *req) {
    if (!w->token[0]) return 1;
    char v[256];
    if (wsd_header(req, "Authorization", v, sizeof v) &&
        !strncasecmp(v, "Bearer ", 7) && !strcmp(v + 7, w->token))
        return 1;
    const char *q = strstr(req, "token=");
    if (q && q < strchr(req, '\n')) {
        q += 6;
        size_t n = strcspn(q, "& \r\n");
        return n == strlen(w->token) && !strncmp(q, w->token, n);
    }
    return 0;
}

static int wsd_handshake(wsd *w, int fd) {
    char req[4096];
    size_t n = 0;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    for (;;) {
        ssize_t k = recv(fd, req + n, sizeof req - 1 - n, 0);
        if (k <= 0) return -1;
        n += (size_t)k;
        req[n] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
        if (n >= sizeof req - 1) return -1;
    }
    char key[128];
    if (strncmp(req, "GET ", 4) || !wsd_header(req, "Sec-WebSocket-Key", key, sizeof key)) {
        const char *bad = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        wsd_write_all(fd, bad, strlen(bad));
        return -1;
    }
    if (!wsd_authorized(w, req)) {
        const char *bad = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n";
        wsd_write_all(fd, bad, strlen(bad));
        return -1;
    }
    char cat[200];
    snprintf(cat, sizeof cat, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
    unsigned char digest[20];
    wsd_sha1((const unsigned char *)cat, strlen(cat), digest);
    char accept[32];
    wsd_b64(digest, 20, accept);
    char resp[300];
    int rl = snprintf(resp, sizeof resp,
                      "HTTP/1.1 101 Switching Protocols\r\n"
                      "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                      "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    tv.tv_sec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return wsd_write_all(fd, resp, (size_t)rl);
}

/* ---- frames ------------------------------------------------------------- */

/* Consume complete frames from the buffer; -1 asks for the client to go. */
static int wsd_parse(wsd *w) {
    for (;;) {
        if (w->len < 2) return 0;
        unsigned char *b = w->buf;
        int fin = b[0] & 0x80, op = b[0] & 0x0f, masked = b[1] & 0x80;
        uint64_t plen = b[1] & 0x7f;
        size_t hl = 2;
        if (plen == 126) { if (w->len < 4) return 0; plen = (uint64_t)b[2] << 8 | b[3]; hl = 4; }
        else if (plen == 127) {
            if (w->len < 10) return 0;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = plen << 8 | b[2 + i];
            hl = 10;
        }
        if (!masked || !fin) return -1;
        if (plen > w->opts.max_message) return -1;
        if (w->len < hl + 4 + plen) return 0;
        unsigned char *mask = b + hl, *p = b + hl + 4;
        for (uint64_t i = 0; i < plen; i++) p[i] ^= mask[i & 3];
        size_t used = hl + 4 + (size_t)plen;
        if (op == 0x1) {
            unsigned char save = p[plen];
            p[plen] = '\0';
            if (w->on_text) w->on_text(w->ud, (const char *)p, (size_t)plen);
            p[plen] = save;
        } else if (op == 0x8) {
            return -1;
        } else if (op == 0x9) {
            pthread_mutex_lock(&w->send_lock);
            if (w->client_fd >= 0) wsd_write_frame(w->client_fd, 0xA, p, (size_t)plen);
            pthread_mutex_unlock(&w->send_lock);
        }
        memmove(w->buf, w->buf + used, w->len - used);
        w->len -= used;
    }
}

static int wsd_read(wsd *w) {
    if (w->cap - w->len < 4096) {
        size_t nc = w->cap ? w->cap * 2 : 8192;
        if (nc > w->opts.max_message + 16 + 4096) nc = w->opts.max_message + 16 + 4096;
        if (nc <= w->len + 1) return -1;
        unsigned char *nb = realloc(w->buf, nc);
        if (!nb) return -1;
        w->buf = nb; w->cap = nc;
    }
    ssize_t k = recv(w->client_fd, w->buf + w->len, w->cap - w->len - 1, 0);
    if (k <= 0) return (k < 0 && (errno == EINTR || errno == EAGAIN)) ? 0 : -1;
    w->len += (size_t)k;
    return wsd_parse(w);
}

static void wsd_accept(wsd *w) {
    int fd = accept(w->listen_fd, NULL, NULL);
    if (fd < 0) return;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (wsd_handshake(w, fd)) { close(fd); return; }
    wsd_drop_client(w, 1);
    pthread_mutex_lock(&w->send_lock);
    w->client_fd = fd;
    pthread_mutex_unlock(&w->send_lock);
    if (w->on_state) w->on_state(w->ud, 1);
}

static void *wsd_loop(void *ud) {
    wsd *w = ud;
    for (;;) {
        struct pollfd p[3] = {
            {w->stop_pipe[0], POLLIN, 0}, {w->listen_fd, POLLIN, 0}, {w->client_fd, POLLIN, 0}};
        int n = w->client_fd >= 0 ? 3 : 2;
        if (poll(p, (nfds_t)n, -1) < 0) { if (errno == EINTR) continue; break; }
        if (p[0].revents) break;
        if (n == 3 && p[2].revents && wsd_read(w) < 0) wsd_drop_client(w, 1);
        if (p[1].revents) wsd_accept(w);
    }
    wsd_drop_client(w, 1);
    return NULL;
}

/* ---- api ---------------------------------------------------------------- */

wsd *wsd_start(const wsd_opts *opts, wsd_text_fn on_text, wsd_state_fn on_state, void *ud) {
    if (!opts || opts->port <= 0) return NULL;
    wsd *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->opts = *opts;
    if (!w->opts.max_message) w->opts.max_message = 1 << 20;
    if (opts->token) snprintf(w->token, sizeof w->token, "%s", opts->token);
    if (opts->bind_ip) snprintf(w->bind_ip, sizeof w->bind_ip, "%s", opts->bind_ip);
    w->opts.token = w->token;
    w->opts.bind_ip = w->bind_ip;
    w->on_text = on_text; w->on_state = on_state; w->ud = ud;
    w->client_fd = -1;
    w->stop_pipe[0] = w->stop_pipe[1] = -1;
    pthread_mutex_init(&w->send_lock, NULL);
    signal(SIGPIPE, SIG_IGN);

    w->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (w->listen_fd < 0) goto fail;
    int one = 1;
    setsockopt(w->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)opts->port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (w->bind_ip[0] && inet_pton(AF_INET, w->bind_ip, &sa.sin_addr) != 1) goto fail;
    if (bind(w->listen_fd, (struct sockaddr *)&sa, sizeof sa) || listen(w->listen_fd, 4)) goto fail;
    fcntl(w->listen_fd, F_SETFD, FD_CLOEXEC);
    if (pipe(w->stop_pipe)) goto fail;
    fcntl(w->stop_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(w->stop_pipe[1], F_SETFD, FD_CLOEXEC);
    if (pthread_create(&w->thread, NULL, wsd_loop, w)) goto fail;
    return w;
fail:
    if (w->listen_fd >= 0) close(w->listen_fd);
    if (w->stop_pipe[0] >= 0) close(w->stop_pipe[0]);
    if (w->stop_pipe[1] >= 0) close(w->stop_pipe[1]);
    free(w);
    return NULL;
}

int wsd_send(wsd *w, const char *text, size_t n) {
    if (!w || !text) return -1;
    if (!n) n = strlen(text);
    pthread_mutex_lock(&w->send_lock);
    int r = w->client_fd >= 0 ? wsd_write_frame(w->client_fd, 0x1, text, n) : -1;
    pthread_mutex_unlock(&w->send_lock);
    return r;
}

int wsd_connected(const wsd *w) {
    return w && w->client_fd >= 0;
}

void wsd_stop(wsd *w) {
    if (!w) return;
    char b = 1;
    ssize_t ignored = write(w->stop_pipe[1], &b, 1);
    (void)ignored;
    pthread_join(w->thread, NULL);
    close(w->stop_pipe[0]);
    close(w->stop_pipe[1]);
    close(w->listen_fd);
    pthread_mutex_destroy(&w->send_lock);
    free(w->buf);
    free(w);
}

#endif /* WSD_IMPLEMENTATION */
