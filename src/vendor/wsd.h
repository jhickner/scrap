/**
 * wsd.h — single-header WebSocket server (C)
 *
 * Accepts up to WSD_MAX_CLIENTS WebSocket clients on its own thread and hands
 * text messages to a callback, tagged with a client id. When the table is
 * full, the oldest client is dropped. Sends are queued per client and written
 * by the server thread, so wsd_send never blocks on the network; a client
 * whose queue passes max_queue is dropped. The server pings every client
 * every 15 s and drops one that has sent nothing for 45 s.
 *
 * Only what a chat transport needs: RFC 6455 text and close frames, pings,
 * no extensions, no fragmentation, no TLS. Bind it to a Tailscale address
 * (see wsd_tailscale_ip) and require a token; treat it as a private socket,
 * not a public endpoint.
 *
 * USAGE (stb style):
 *
 *   // In exactly ONE .c file:
 *   #define WSD_IMPLEMENTATION
 *   #include "wsd.h"
 *
 *   wsd_opts o = { .bind_ip = wsd_tailscale_ip(), .port = 8790, .token = tok };
 *   wsd *w = wsd_start(&o, on_text, on_state, ud);
 *   wsd_send(w, client, "{\"t\":\"hello\"}", 0);
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

#define WSD_MAX_CLIENTS 8

typedef struct wsd wsd;

typedef struct {
    const char *bind_ip;     /* NULL/"" binds every interface                  */
    int         port;
    const char *token;       /* required in the upgrade request when non-empty */
    size_t      max_message; /* incoming text limit, 0 -> 1 MiB               */
    size_t      max_queue;   /* outgoing bytes per client, 0 -> 64 MiB         */
} wsd_opts;

/* Both run on the server thread. Client ids are positive and never reused.
 * on_text gets a NUL-terminated copy that is valid for the call only.
 * on_state fires with 1 after a client's handshake and 0 when it goes. */
typedef void (*wsd_text_fn)(void *ud, int client, const char *text, size_t n);
typedef void (*wsd_state_fn)(void *ud, int client, int connected);

wsd *wsd_start(const wsd_opts *opts, wsd_text_fn on_text, wsd_state_fn on_state, void *ud);

/* Queue a text frame for a client. n == 0 means strlen(text). Returns 0 when
 * queued, -1 when the client is gone. Any thread. */
int wsd_send(wsd *w, int client, const char *text, size_t n);

/* Connected client count. */
int wsd_clients(wsd *w);

/* Close every client, stop the thread and free. Safe with NULL. */
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
#include <time.h>
#include <unistd.h>

#define WSD_PING_SECS 15
#define WSD_DEAD_SECS 45

typedef struct {
    int            fd;     /* -1 when the slot is free */
    int            id;
    unsigned char *in;
    size_t         in_len, in_cap;
    unsigned char *out;    /* framed bytes not yet written, from out_off */
    size_t         out_len, out_off, out_cap;
    int            doomed; /* queue overflow: drop on the next loop */
    time_t         last_rx, last_ping;
} wsd_client;

struct wsd {
    wsd_opts        opts;
    char            token[128];
    char            bind_ip[64];
    wsd_text_fn     on_text;
    wsd_state_fn    on_state;
    void           *ud;
    int             listen_fd;
    int             wake[2];
    volatile int    stopping;
    int             next_id;
    pthread_t       thread;
    pthread_mutex_t lock;
    wsd_client      c[WSD_MAX_CLIENTS];
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

static size_t wsd_frame_header(unsigned char hdr[10], int opcode, size_t n) {
    hdr[0] = (unsigned char)(0x80 | opcode);
    if (n < 126) { hdr[1] = (unsigned char)n; return 2; }
    if (n < 65536) { hdr[1] = 126; hdr[2] = (unsigned char)(n >> 8); hdr[3] = (unsigned char)n; return 4; }
    hdr[1] = 127;
    for (int i = 0; i < 8; i++) hdr[2 + i] = (unsigned char)((uint64_t)n >> (56 - 8 * i));
    return 10;
}

/* Caller holds the lock. */
static void wsd_queue(wsd *w, wsd_client *c, int opcode, const void *data, size_t n) {
    if (c->doomed) return;
    unsigned char hdr[10];
    size_t hl = wsd_frame_header(hdr, opcode, n);
    if (c->out_off && c->out_off == c->out_len) c->out_off = c->out_len = 0;
    if (c->out_len - c->out_off + hl + n > w->opts.max_queue) { c->doomed = 1; return; }
    if (c->out_len + hl + n > c->out_cap) {
        if (c->out_off) {
            memmove(c->out, c->out + c->out_off, c->out_len - c->out_off);
            c->out_len -= c->out_off;
            c->out_off = 0;
        }
        size_t nc = c->out_cap ? c->out_cap : 8192;
        while (nc < c->out_len + hl + n) nc *= 2;
        unsigned char *nb = realloc(c->out, nc);
        if (!nb) { c->doomed = 1; return; }
        c->out = nb; c->out_cap = nc;
    }
    memcpy(c->out + c->out_len, hdr, hl);
    if (n) memcpy(c->out + c->out_len + hl, data, n);
    c->out_len += hl + n;
}

static void wsd_wake(wsd *w) {
    char b = 1;
    ssize_t ignored = write(w->wake[1], &b, 1);
    (void)ignored;
}

static void wsd_drop(wsd *w, wsd_client *c, int tell) {
    pthread_mutex_lock(&w->lock);
    int fd = c->fd, id = c->id;
    c->fd = -1;
    free(c->out);
    c->out = NULL;
    c->out_len = c->out_off = c->out_cap = 0;
    c->doomed = 0;
    pthread_mutex_unlock(&w->lock);
    if (fd < 0) return;
    if (tell) {
        unsigned char f[4] = {0x88, 2, 0x03, 0xe8};
        ssize_t ignored = send(fd, f, sizeof f, MSG_DONTWAIT);
        (void)ignored;
    }
    close(fd);
    free(c->in);
    c->in = NULL;
    c->in_len = c->in_cap = 0;
    if (w->on_state) w->on_state(w->ud, id, 0);
}

/* ---- handshake ---------------------------------------------------------- */

static int wsd_token_eq(const char *a, size_t n, const char *token) {
    size_t tn = strlen(token);
    unsigned char d = (unsigned char)(n != tn);
    for (size_t i = 0; i < tn; i++) d |= (unsigned char)((i < n ? a[i] : 0) ^ token[i]);
    return d == 0;
}

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
        !strncasecmp(v, "Bearer ", 7) && wsd_token_eq(v + 7, strlen(v + 7), w->token))
        return 1;
    const char *q = strstr(req, "token=");
    if (q && q < strchr(req, '\n')) {
        q += 6;
        size_t n = strcspn(q, "& \r\n");
        return wsd_token_eq(q, n, w->token);
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
static int wsd_parse(wsd *w, wsd_client *c) {
    for (;;) {
        if (c->in_len < 2) return 0;
        unsigned char *b = c->in;
        int fin = b[0] & 0x80, op = b[0] & 0x0f, masked = b[1] & 0x80;
        uint64_t plen = b[1] & 0x7f;
        size_t hl = 2;
        if (plen == 126) { if (c->in_len < 4) return 0; plen = (uint64_t)b[2] << 8 | b[3]; hl = 4; }
        else if (plen == 127) {
            if (c->in_len < 10) return 0;
            plen = 0;
            for (int i = 0; i < 8; i++) plen = plen << 8 | b[2 + i];
            hl = 10;
        }
        if (!masked || !fin) return -1;
        if (plen > w->opts.max_message) return -1;
        if (c->in_len < hl + 4 + plen) return 0;
        unsigned char *mask = b + hl, *p = b + hl + 4;
        for (uint64_t i = 0; i < plen; i++) p[i] ^= mask[i & 3];
        size_t used = hl + 4 + (size_t)plen;
        if (op == 0x1) {
            unsigned char save = p[plen];
            p[plen] = '\0';
            if (w->on_text) w->on_text(w->ud, c->id, (const char *)p, (size_t)plen);
            p[plen] = save;
        } else if (op == 0x8) {
            return -1;
        } else if (op == 0x9) {
            pthread_mutex_lock(&w->lock);
            wsd_queue(w, c, 0xA, p, (size_t)plen);
            pthread_mutex_unlock(&w->lock);
        }
        memmove(c->in, c->in + used, c->in_len - used);
        c->in_len -= used;
    }
}

static int wsd_read(wsd *w, wsd_client *c) {
    if (c->in_cap - c->in_len < 4096) {
        size_t nc = c->in_cap ? c->in_cap * 2 : 8192;
        if (nc > w->opts.max_message + 16 + 4096) nc = w->opts.max_message + 16 + 4096;
        if (nc <= c->in_len + 1) return -1;
        unsigned char *nb = realloc(c->in, nc);
        if (!nb) return -1;
        c->in = nb; c->in_cap = nc;
    }
    ssize_t k = recv(c->fd, c->in + c->in_len, c->in_cap - c->in_len - 1, 0);
    if (k < 0) return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    if (k == 0) return -1;
    c->in_len += (size_t)k;
    c->last_rx = time(NULL);
    return wsd_parse(w, c);
}

/* Write what the socket takes; -1 asks for the client to go. */
static int wsd_flush(wsd *w, wsd_client *c) {
    pthread_mutex_lock(&w->lock);
    int r = c->doomed ? -1 : 0;
    while (!r && c->out_off < c->out_len) {
        ssize_t k = send(c->fd, c->out + c->out_off, c->out_len - c->out_off, MSG_DONTWAIT);
        if (k < 0) {
            if (errno != EINTR && errno != EAGAIN) r = -1;
            if (errno != EINTR) break;
            continue;
        }
        c->out_off += (size_t)k;
    }
    pthread_mutex_unlock(&w->lock);
    return r;
}

static void wsd_accept(wsd *w) {
    int fd = accept(w->listen_fd, NULL, NULL);
    if (fd < 0) return;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (wsd_handshake(w, fd)) { close(fd); return; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    wsd_client *slot = NULL;
    for (int i = 0; i < WSD_MAX_CLIENTS && !slot; i++)
        if (w->c[i].fd < 0) slot = &w->c[i];
    if (!slot) {
        slot = &w->c[0];
        for (int i = 1; i < WSD_MAX_CLIENTS; i++)
            if (w->c[i].id < slot->id) slot = &w->c[i];
        wsd_drop(w, slot, 1);
    }
    pthread_mutex_lock(&w->lock);
    slot->fd = fd;
    slot->id = ++w->next_id;
    slot->last_rx = slot->last_ping = time(NULL);
    pthread_mutex_unlock(&w->lock);
    if (w->on_state) w->on_state(w->ud, slot->id, 1);
}

static void wsd_heartbeat(wsd *w) {
    time_t now = time(NULL);
    for (int i = 0; i < WSD_MAX_CLIENTS; i++) {
        wsd_client *c = &w->c[i];
        if (c->fd < 0) continue;
        if (now - c->last_rx > WSD_DEAD_SECS) { wsd_drop(w, c, 1); continue; }
        if (now - c->last_ping >= WSD_PING_SECS) {
            pthread_mutex_lock(&w->lock);
            wsd_queue(w, c, 0x9, "", 0);
            pthread_mutex_unlock(&w->lock);
            c->last_ping = now;
        }
    }
}

static void *wsd_loop(void *ud) {
    wsd *w = ud;
    while (!w->stopping) {
        struct pollfd p[2 + WSD_MAX_CLIENTS];
        wsd_client *who[2 + WSD_MAX_CLIENTS];
        nfds_t n = 0;
        p[n] = (struct pollfd){w->wake[0], POLLIN, 0}; who[n++] = NULL;
        p[n] = (struct pollfd){w->listen_fd, POLLIN, 0}; who[n++] = NULL;
        pthread_mutex_lock(&w->lock);
        for (int i = 0; i < WSD_MAX_CLIENTS; i++) {
            wsd_client *c = &w->c[i];
            if (c->fd < 0) continue;
            short ev = POLLIN;
            if (c->out_off < c->out_len || c->doomed) ev |= POLLOUT;
            p[n] = (struct pollfd){c->fd, ev, 0}; who[n++] = c;
        }
        pthread_mutex_unlock(&w->lock);
        if (poll(p, n, 1000) < 0) { if (errno == EINTR) continue; break; }
        if (p[0].revents) {
            char buf[64];
            while (read(w->wake[0], buf, sizeof buf) > 0) {}
        }
        for (nfds_t i = 2; i < n; i++) {
            wsd_client *c = who[i];
            if (c->fd != p[i].fd) continue;
            int bad = 0;
            if (p[i].revents & (POLLIN | POLLHUP | POLLERR)) bad = wsd_read(w, c) < 0;
            if (!bad) bad = wsd_flush(w, c) < 0;
            if (bad) wsd_drop(w, c, 1);
        }
        if (p[1].revents) wsd_accept(w);
        wsd_heartbeat(w);
    }
    for (int i = 0; i < WSD_MAX_CLIENTS; i++) wsd_drop(w, &w->c[i], 1);
    return NULL;
}

/* ---- api ---------------------------------------------------------------- */

wsd *wsd_start(const wsd_opts *opts, wsd_text_fn on_text, wsd_state_fn on_state, void *ud) {
    if (!opts || opts->port <= 0) return NULL;
    wsd *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->opts = *opts;
    if (!w->opts.max_message) w->opts.max_message = 1 << 20;
    if (!w->opts.max_queue) w->opts.max_queue = 64u << 20;
    if (opts->token) snprintf(w->token, sizeof w->token, "%s", opts->token);
    if (opts->bind_ip) snprintf(w->bind_ip, sizeof w->bind_ip, "%s", opts->bind_ip);
    w->opts.token = w->token;
    w->opts.bind_ip = w->bind_ip;
    w->on_text = on_text; w->on_state = on_state; w->ud = ud;
    for (int i = 0; i < WSD_MAX_CLIENTS; i++) w->c[i].fd = -1;
    w->wake[0] = w->wake[1] = -1;
    pthread_mutex_init(&w->lock, NULL);
    signal(SIGPIPE, SIG_IGN);

    w->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (w->listen_fd < 0) goto fail;
    int one = 1;
    setsockopt(w->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)opts->port);
    sa.sin_addr.s_addr = INADDR_ANY;
    if (w->bind_ip[0] && inet_pton(AF_INET, w->bind_ip, &sa.sin_addr) != 1) { errno = EINVAL; goto fail; }
    if (bind(w->listen_fd, (struct sockaddr *)&sa, sizeof sa) || listen(w->listen_fd, 4)) goto fail;
    fcntl(w->listen_fd, F_SETFD, FD_CLOEXEC);
    if (pipe(w->wake)) goto fail;
    for (int i = 0; i < 2; i++) {
        fcntl(w->wake[i], F_SETFD, FD_CLOEXEC);
        fcntl(w->wake[i], F_SETFL, O_NONBLOCK);
    }
    if (pthread_create(&w->thread, NULL, wsd_loop, w)) goto fail;
    return w;
fail:;
    int e = errno;
    if (w->listen_fd >= 0) close(w->listen_fd);
    if (w->wake[0] >= 0) close(w->wake[0]);
    if (w->wake[1] >= 0) close(w->wake[1]);
    pthread_mutex_destroy(&w->lock);
    free(w);
    errno = e;
    return NULL;
}

int wsd_send(wsd *w, int client, const char *text, size_t n) {
    if (!w || !text) return -1;
    if (!n) n = strlen(text);
    int r = -1;
    pthread_mutex_lock(&w->lock);
    for (int i = 0; i < WSD_MAX_CLIENTS; i++) {
        wsd_client *c = &w->c[i];
        if (c->fd >= 0 && c->id == client && !c->doomed) {
            wsd_queue(w, c, 0x1, text, n);
            r = c->doomed ? -1 : 0;
            break;
        }
    }
    pthread_mutex_unlock(&w->lock);
    if (!r) wsd_wake(w);
    return r;
}

int wsd_clients(wsd *w) {
    if (!w) return 0;
    int n = 0;
    pthread_mutex_lock(&w->lock);
    for (int i = 0; i < WSD_MAX_CLIENTS; i++) n += w->c[i].fd >= 0;
    pthread_mutex_unlock(&w->lock);
    return n;
}

void wsd_stop(wsd *w) {
    if (!w) return;
    w->stopping = 1;
    wsd_wake(w);
    pthread_join(w->thread, NULL);
    close(w->wake[0]);
    close(w->wake[1]);
    close(w->listen_fd);
    pthread_mutex_destroy(&w->lock);
    free(w);
}

#endif /* WSD_IMPLEMENTATION */
