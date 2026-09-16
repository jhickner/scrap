/**
 * grokvnc.h — single-header RFB 3.8 client over a WebSocket (C)
 *
 * Connects to a websockify endpoint (wss:// or ws://), performs the RFB 3.8
 * handshake with security type None, and keeps an RGB24 copy of the remote
 * framebuffer. Decodes Raw, CopyRect, ZRLE, DesktopSize and LastRect.
 *
 * TLS is Apple SecureTransport (Security.framework, TLS 1.2). Other platforms
 * get ws:// only. Single-threaded: call every function from one thread.
 *
 * USAGE:
 *
 *   #define GROKVNC_IMPLEMENTATION
 *   #include "grokvnc.h"
 *
 *   const char *hdr[] = { "x-anyrun-network-token: nto-...", NULL };
 *   grokvnc_err e;
 *   grokvnc *v = grokvnc_open("wss://host/websockify?token=3", hdr, &e);
 *   while (grokvnc_pump(v)) {
 *       struct pollfd p = { grokvnc_fd(v), POLLIN | (grokvnc_want_write(v) ? POLLOUT : 0) };
 *       poll(&p, 1, grokvnc_timeout_ms(v));
 *       int w, h; uint64_t gen;
 *       const uint8_t *rgb = grokvnc_frame(v, &w, &h, &gen);
 *   }
 *   grokvnc_close(v);
 *
 * grokvnc_open blocks through TCP connect, TLS and the HTTP upgrade (bounded
 * by timeout). The RFB handshake and everything after run in grokvnc_pump.
 *
 * LINK: -lz, and on macOS -framework Security -framework CoreFoundation
 */

#ifndef GROKVNC_H
#define GROKVNC_H

#include <stddef.h>
#include <stdint.h>

typedef struct grokvnc grokvnc;

typedef struct {
    int  http_status;   /* upgrade response status; 0 when not reached */
    char msg[256];
} grokvnc_err;

typedef struct {
    int x, y, w, h;
} grokvnc_rect;

typedef struct {
    uint64_t updates;     /* completed FramebufferUpdate messages */
    uint64_t rects;
    uint64_t bytes_in;    /* WebSocket payload bytes */
    uint64_t bytes_out;
} grokvnc_stats;

/* headers: NULL-terminated "name: value" lines, may be NULL. err may be NULL. */
grokvnc *grokvnc_open(const char *url, const char *const *headers, grokvnc_err *err);
void     grokvnc_close(grokvnc *v);

/* Reads what is available, decodes, sends due requests, flushes output.
 * Returns 0 once the connection is closed or failed. Never blocks. */
int grokvnc_pump(grokvnc *v);
int grokvnc_fd(const grokvnc *v);
int grokvnc_want_write(const grokvnc *v);
/* Milliseconds until the next update request is due; -1 for none. */
int grokvnc_timeout_ms(const grokvnc *v);

const char *grokvnc_error(const grokvnc *v);
/* 1 after ServerInit. */
int         grokvnc_ready(const grokvnc *v);
const char *grokvnc_name(const grokvnc *v);
void        grokvnc_stats_get(const grokvnc *v, grokvnc_stats *out);

/* Minimum interval between incremental update requests. Default 50 ms. */
void grokvnc_set_interval(grokvnc *v, int ms);
/* Next request is non-incremental. */
void grokvnc_request_full(grokvnc *v);

/* RGB24, row-major, w*h*3 bytes. generation increases on every change.
 * NULL before ServerInit. Valid until the next pump. */
const uint8_t *grokvnc_frame(const grokvnc *v, int *w, int *h, uint64_t *generation);
/* Union of changed area since the last call; returns 0 when nothing changed. */
int grokvnc_take_dirty(grokvnc *v, grokvnc_rect *out);

/* Input. mask bits: 1 left, 2 middle, 4 right, 8/16 wheel up/down. */
int grokvnc_send_pointer(grokvnc *v, int x, int y, int mask);
int grokvnc_send_key(grokvnc *v, uint32_t keysym, int down);

/* Transport-less client for tests: grokvnc_inject feeds bytes as they would
 * arrive after TLS (WebSocket frames from the server). Output is discarded. */
grokvnc *grokvnc_new_detached(void);
int      grokvnc_inject(grokvnc *v, const uint8_t *p, size_t n);

#endif

#ifdef GROKVNC_IMPLEMENTATION

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <zlib.h>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#include <Security/SecureTransport.h>
#endif

#define GV_TIMEOUT_MS     15000
#define GV_MAX_FRAME      (64u * 1024 * 1024)
#define GV_MAX_HTTP       16384
#define GV_READ_CHUNK     65536

enum { GV_T_NONE, GV_T_PLAIN, GV_T_TLS };
enum { GV_S_VERSION, GV_S_SECTYPES, GV_S_SECRESULT, GV_S_INIT, GV_S_NORMAL, GV_S_CLOSED };

typedef struct {
    uint8_t *p;
    size_t   off, len, cap;
} gv_buf;

struct grokvnc {
    int fd;
    int transport;
#ifdef __APPLE__
    SSLContextRef ssl;
#endif
    gv_buf raw_out;
    gv_buf ws_in;
    gv_buf rfb_in;
    gv_buf scratch;
    gv_buf zbuf;
    z_stream zs;
    int zs_init;

    int state;
    int close_sent;
    char err[256];

    int w, h;
    uint8_t *fb;
    char name[256];
    uint64_t generation;
    int dirty;
    grokvnc_rect dirty_rect;

    int rects_left;
    int awaiting;
    int want_full;
    int interval_ms;
    int64_t last_request_ms;
    grokvnc_stats stats;
};

static int64_t gv_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static uint16_t gv_u16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t gv_u32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void gv_put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void gv_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static int gv_fail(grokvnc *v, const char *fmt, const char *arg) {
    if (v->state != GV_S_CLOSED || !v->err[0])
        snprintf(v->err, sizeof v->err, fmt, arg ? arg : "");
    v->state = GV_S_CLOSED;
    return -1;
}

static int gv_buf_reserve(gv_buf *b, size_t add) {
    if (b->off && b->off == b->len) b->off = b->len = 0;
    if (b->len + add <= b->cap) return 1;
    if (b->off) {
        memmove(b->p, b->p + b->off, b->len - b->off);
        b->len -= b->off;
        b->off = 0;
        if (b->len + add <= b->cap) return 1;
    }
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + add) cap *= 2;
    uint8_t *np = realloc(b->p, cap);
    if (!np) return 0;
    b->p = np;
    b->cap = cap;
    return 1;
}

static int gv_buf_append(gv_buf *b, const uint8_t *p, size_t n) {
    if (!gv_buf_reserve(b, n)) return 0;
    memcpy(b->p + b->len, p, n);
    b->len += n;
    return 1;
}

static size_t gv_avail(const gv_buf *b) { return b->len - b->off; }
static uint8_t *gv_head(const gv_buf *b) { return b->p + b->off; }
static void gv_consume(gv_buf *b, size_t n) {
    b->off += n;
    if (b->off == b->len) b->off = b->len = 0;
}

/* ---- transport ---- */

static int gv_flush_raw(grokvnc *v) {
    if (v->transport == GV_T_NONE) { v->raw_out.off = v->raw_out.len = 0; return 0; }
    while (gv_avail(&v->raw_out)) {
        ssize_t r = write(v->fd, gv_head(&v->raw_out), gv_avail(&v->raw_out));
        if (r > 0) { gv_consume(&v->raw_out, (size_t)r); continue; }
        if (r < 0 && errno == EINTR) continue;
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    return 0;
}

#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static OSStatus gv_ssl_read(SSLConnectionRef c, void *data, size_t *len) {
    grokvnc *v = (grokvnc *)(uintptr_t)c;
    size_t want = *len, got = 0;
    while (got < want) {
        ssize_t r = read(v->fd, (char *)data + got, want - got);
        if (r > 0) { got += (size_t)r; continue; }
        *len = got;
        if (r == 0) return errSSLClosedGraceful;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return errSSLWouldBlock;
        return errSSLClosedAbort;
    }
    *len = got;
    return noErr;
}

/* Ciphertext is queued in raw_out so SSLWrite never reports errSSLWouldBlock. */
static OSStatus gv_ssl_write(SSLConnectionRef c, const void *data, size_t *len) {
    grokvnc *v = (grokvnc *)(uintptr_t)c;
    if (!gv_buf_append(&v->raw_out, data, *len)) return errSSLClosedAbort;
    return gv_flush_raw(v) < 0 ? errSSLClosedAbort : noErr;
}
#endif

static int gv_write(grokvnc *v, const uint8_t *p, size_t n) {
    if (v->transport == GV_T_NONE) return 0;
#ifdef __APPLE__
    if (v->transport == GV_T_TLS) {
        size_t done = 0;
        while (done < n) {
            size_t k = 0;
            OSStatus st = SSLWrite(v->ssl, p + done, n - done, &k);
            done += k;
            if (st != noErr && st != errSSLWouldBlock) return gv_fail(v, "tls write failed", NULL);
        }
        return 0;
    }
#endif
    if (!gv_buf_append(&v->raw_out, p, n)) return gv_fail(v, "out of memory", NULL);
    if (gv_flush_raw(v) < 0) return gv_fail(v, "write failed: %s", strerror(errno));
    return 0;
}

/* >0 bytes, 0 would block, -1 closed or error */
static ssize_t gv_read(grokvnc *v, uint8_t *p, size_t cap) {
#ifdef __APPLE__
    if (v->transport == GV_T_TLS) {
        size_t k = 0;
        OSStatus st = SSLRead(v->ssl, p, cap, &k);
        if (k) return (ssize_t)k;
        if (st == errSSLWouldBlock) return 0;
        if (st == errSSLClosedGraceful || st == errSSLClosedNoNotify)
            return gv_fail(v, "connection closed", NULL), -1;
        return gv_fail(v, "tls read failed", NULL), -1;
    }
#endif
    for (;;) {
        ssize_t r = read(v->fd, p, cap);
        if (r > 0) return r;
        if (r == 0) return gv_fail(v, "connection closed", NULL), -1;
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return gv_fail(v, "read failed: %s", strerror(errno)), -1;
    }
}

static int gv_wait(grokvnc *v, int64_t deadline, int for_write) {
    int64_t left = deadline - gv_now_ms();
    if (left <= 0) return 0;
    struct pollfd pfd = { v->fd, (short)(for_write ? POLLOUT : POLLIN), 0 };
    if (gv_avail(&v->raw_out)) pfd.events |= POLLOUT;
    int r = poll(&pfd, 1, (int)left);
    if (r > 0 && (pfd.revents & POLLOUT)) gv_flush_raw(v);
    return r >= 0;
}

/* ---- websocket ---- */

static int gv_ws_send(grokvnc *v, int opcode, const uint8_t *p, size_t n) {
    uint8_t hdr[14];
    size_t h = 0;
    hdr[h++] = (uint8_t)(0x80 | opcode);
    if (n < 126) {
        hdr[h++] = (uint8_t)(0x80 | n);
    } else if (n < 65536) {
        hdr[h++] = 0x80 | 126;
        gv_put16(hdr + h, (uint16_t)n);
        h += 2;
    } else {
        hdr[h++] = 0x80 | 127;
        gv_put32(hdr + h, (uint32_t)((uint64_t)n >> 32));
        gv_put32(hdr + h + 4, (uint32_t)n);
        h += 8;
    }
    uint8_t mask[4];
    arc4random_buf(mask, 4);
    memcpy(hdr + h, mask, 4);
    h += 4;
    gv_buf *s = &v->scratch;
    s->off = s->len = 0;
    if (!gv_buf_reserve(s, h + n)) return gv_fail(v, "out of memory", NULL);
    memcpy(s->p, hdr, h);
    for (size_t i = 0; i < n; i++) s->p[h + i] = p[i] ^ mask[i & 3];
    s->len = h + n;
    v->stats.bytes_out += n;
    return gv_write(v, s->p, s->len);
}

static int gv_rfb_process(grokvnc *v);

static int gv_ws_process(grokvnc *v) {
    gv_buf *b = &v->ws_in;
    while (v->state != GV_S_CLOSED && gv_avail(b) >= 2) {
        uint8_t *p = gv_head(b);
        size_t avail = gv_avail(b);
        int op = p[0] & 0x0f;
        int masked = p[1] & 0x80;
        uint64_t len = p[1] & 0x7f;
        size_t h = 2;
        if (len == 126) {
            if (avail < 4) break;
            len = gv_u16(p + 2);
            h = 4;
        } else if (len == 127) {
            if (avail < 10) break;
            len = (uint64_t)gv_u32(p + 2) << 32 | gv_u32(p + 6);
            h = 10;
        }
        if (masked) h += 4;
        if (len > GV_MAX_FRAME) return gv_fail(v, "websocket frame too large", NULL);
        if (avail < h + len) break;
        uint8_t *pl = p + h;
        if (masked)
            for (uint64_t i = 0; i < len; i++) pl[i] ^= p[h - 4 + (i & 3)];
        switch (op) {
        case 0x0:
        case 0x2:
            if (!gv_buf_append(&v->rfb_in, pl, (size_t)len)) return gv_fail(v, "out of memory", NULL);
            v->stats.bytes_in += len;
            break;
        case 0x8:
            if (!v->close_sent) {
                v->close_sent = 1;
                gv_ws_send(v, 0x8, pl, len >= 2 ? 2 : 0);
                gv_flush_raw(v);
            }
            gv_consume(b, h + (size_t)len);
            return gv_fail(v, "server closed websocket", NULL);
        case 0x9:
            if (len > 125) return gv_fail(v, "bad ping", NULL);
            if (gv_ws_send(v, 0xA, pl, (size_t)len) < 0) return -1;
            break;
        default:
            break;
        }
        gv_consume(b, h + (size_t)len);
    }
    return gv_rfb_process(v);
}

static int gv_feed(grokvnc *v, const uint8_t *p, size_t n) {
    if (!gv_buf_append(&v->ws_in, p, n)) return gv_fail(v, "out of memory", NULL);
    return gv_ws_process(v);
}

/* ---- rfb ---- */

static int gv_send_request(grokvnc *v, int incremental) {
    uint8_t m[10] = { 3, (uint8_t)incremental };
    gv_put16(m + 6, (uint16_t)v->w);
    gv_put16(m + 8, (uint16_t)v->h);
    v->awaiting = 1;
    v->last_request_ms = gv_now_ms();
    return gv_ws_send(v, 0x2, m, sizeof m);
}

static void gv_mark(grokvnc *v, int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    v->generation++;
    if (!v->dirty) {
        v->dirty = 1;
        v->dirty_rect = (grokvnc_rect){ x, y, w, h };
        return;
    }
    grokvnc_rect *d = &v->dirty_rect;
    int x1 = d->x + d->w > x + w ? d->x + d->w : x + w;
    int y1 = d->y + d->h > y + h ? d->y + d->h : y + h;
    d->x = d->x < x ? d->x : x;
    d->y = d->y < y ? d->y : y;
    d->w = x1 - d->x;
    d->h = y1 - d->y;
}

static int gv_resize(grokvnc *v, int w, int h) {
    uint8_t *fb = calloc((size_t)w * (size_t)h * 3 + 1, 1);
    if (!fb) return gv_fail(v, "out of memory", NULL);
    free(v->fb);
    v->fb = fb;
    v->w = w;
    v->h = h;
    gv_mark(v, 0, 0, w, h);
    return 0;
}

static int gv_on_server_init(grokvnc *v) {
    uint8_t pf[20] = { 0 };
    pf[4] = 32; pf[5] = 24; pf[6] = 0; pf[7] = 1;
    gv_put16(pf + 8, 255); gv_put16(pf + 10, 255); gv_put16(pf + 12, 255);
    pf[14] = 16; pf[15] = 8; pf[16] = 0;
    if (gv_ws_send(v, 0x2, pf, sizeof pf) < 0) return -1;

    static const int32_t enc[] = { 16, 1, 0, -223, -224 };
    uint8_t m[4 + 4 * sizeof enc / sizeof enc[0]];
    m[0] = 2; m[1] = 0;
    gv_put16(m + 2, (uint16_t)(sizeof enc / sizeof enc[0]));
    for (size_t i = 0; i < sizeof enc / sizeof enc[0]; i++)
        gv_put32(m + 4 + 4 * i, (uint32_t)enc[i]);
    if (gv_ws_send(v, 0x2, m, sizeof m) < 0) return -1;
    v->want_full = 0;
    return gv_send_request(v, 0);
}

static int gv_zrle(grokvnc *v, int rx, int ry, int rw, int rh, const uint8_t *zdata, size_t zlen) {
    if (!v->zs_init) {
        if (inflateInit(&v->zs) != Z_OK) return gv_fail(v, "zlib init failed", NULL);
        v->zs_init = 1;
    }
    gv_buf *o = &v->zbuf;
    o->off = o->len = 0;
    v->zs.next_in = (Bytef *)(uintptr_t)zdata;
    v->zs.avail_in = (uInt)zlen;
    for (;;) {
        if (!gv_buf_reserve(o, (size_t)rw * rh * 3 + 65536)) return gv_fail(v, "out of memory", NULL);
        v->zs.next_out = o->p + o->len;
        v->zs.avail_out = (uInt)(o->cap - o->len);
        int zr = inflate(&v->zs, Z_SYNC_FLUSH);
        o->len = o->cap - v->zs.avail_out;
        if (zr != Z_OK && zr != Z_BUF_ERROR) return gv_fail(v, "zrle inflate failed", NULL);
        if (v->zs.avail_in == 0 && v->zs.avail_out != 0) break;
        if (zr == Z_BUF_ERROR && v->zs.avail_in == 0) break;
    }

    const uint8_t *p = o->p, *end = o->p + o->len;
#define GV_NEED(n) do { if ((size_t)(end - p) < (size_t)(n)) return gv_fail(v, "zrle data truncated", NULL); } while (0)
    for (int ty = ry; ty < ry + rh; ty += 64) {
        int th = ry + rh - ty < 64 ? ry + rh - ty : 64;
        for (int tx = rx; tx < rx + rw; tx += 64) {
            int tw = rx + rw - tx < 64 ? rx + rw - tx : 64;
            GV_NEED(1);
            int sub = *p++;
            int npix = tw * th;
            uint8_t pal[128 * 3];
            if (sub == 0) {
                GV_NEED(npix * 3);
                for (int j = 0; j < th; j++) {
                    uint8_t *d = v->fb + ((size_t)(ty + j) * v->w + tx) * 3;
                    for (int i = 0; i < tw; i++, p += 3, d += 3) { d[0] = p[2]; d[1] = p[1]; d[2] = p[0]; }
                }
            } else if (sub == 1) {
                GV_NEED(3);
                uint8_t c[3] = { p[2], p[1], p[0] };
                p += 3;
                for (int j = 0; j < th; j++) {
                    uint8_t *d = v->fb + ((size_t)(ty + j) * v->w + tx) * 3;
                    for (int i = 0; i < tw; i++, d += 3) memcpy(d, c, 3);
                }
            } else if (sub >= 2 && sub <= 16) {
                GV_NEED(sub * 3);
                for (int k = 0; k < sub; k++, p += 3) { pal[k*3] = p[2]; pal[k*3+1] = p[1]; pal[k*3+2] = p[0]; }
                int bits = sub == 2 ? 1 : sub <= 4 ? 2 : 4;
                int rowbytes = (tw * bits + 7) / 8;
                GV_NEED(rowbytes * th);
                for (int j = 0; j < th; j++, p += rowbytes) {
                    uint8_t *d = v->fb + ((size_t)(ty + j) * v->w + tx) * 3;
                    for (int i = 0; i < tw; i++, d += 3) {
                        int bit = i * bits;
                        int idx = (p[bit >> 3] >> (8 - bits - (bit & 7))) & ((1 << bits) - 1);
                        if (idx >= sub) return gv_fail(v, "zrle palette index", NULL);
                        memcpy(d, pal + idx * 3, 3);
                    }
                }
            } else if (sub == 128 || sub >= 130) {
                int psize = sub == 128 ? 0 : sub - 128;
                if (psize) {
                    GV_NEED(psize * 3);
                    for (int k = 0; k < psize; k++, p += 3) { pal[k*3] = p[2]; pal[k*3+1] = p[1]; pal[k*3+2] = p[0]; }
                }
                int i = 0;
                while (i < npix) {
                    uint8_t c[3];
                    int run = 1;
                    if (!psize) {
                        GV_NEED(3);
                        c[0] = p[2]; c[1] = p[1]; c[2] = p[0];
                        p += 3;
                        run = 1;
                        for (;;) { GV_NEED(1); int b = *p++; run += b; if (b != 255) break; }
                    } else {
                        GV_NEED(1);
                        int idx = *p++;
                        if (idx & 0x80) {
                            for (;;) { GV_NEED(1); int b = *p++; run += b; if (b != 255) break; }
                        }
                        idx &= 0x7f;
                        if (idx >= psize) return gv_fail(v, "zrle palette index", NULL);
                        memcpy(c, pal + idx * 3, 3);
                    }
                    if (run > npix - i) return gv_fail(v, "zrle run overflow", NULL);
                    for (; run > 0; run--, i++) {
                        uint8_t *d = v->fb + ((size_t)(ty + i / tw) * v->w + tx + i % tw) * 3;
                        memcpy(d, c, 3);
                    }
                }
            } else {
                return gv_fail(v, "zrle subencoding", NULL);
            }
        }
    }
#undef GV_NEED
    return 0;
}

/* 1 consumed a rect, 0 needs more data, -1 error */
static int gv_rect(grokvnc *v) {
    gv_buf *b = &v->rfb_in;
    size_t avail = gv_avail(b);
    if (avail < 12) return 0;
    const uint8_t *p = gv_head(b);
    int x = gv_u16(p), y = gv_u16(p + 2), w = gv_u16(p + 4), h = gv_u16(p + 6);
    int32_t enc = (int32_t)gv_u32(p + 8);
    int in_bounds = x + w <= v->w && y + h <= v->h;

    switch (enc) {
    case 0: {
        size_t need = 12 + (size_t)w * h * 4;
        if (avail < need) return 0;
        if (!in_bounds) return gv_fail(v, "raw rect out of bounds", NULL);
        const uint8_t *s = p + 12;
        for (int j = 0; j < h; j++) {
            uint8_t *d = v->fb + ((size_t)(y + j) * v->w + x) * 3;
            for (int i = 0; i < w; i++, s += 4, d += 3) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; }
        }
        gv_consume(b, need);
        break;
    }
    case 1: {
        if (avail < 16) return 0;
        int sx = gv_u16(p + 12), sy = gv_u16(p + 14);
        if (!in_bounds || sx + w > v->w || sy + h > v->h)
            return gv_fail(v, "copyrect out of bounds", NULL);
        size_t row = (size_t)w * 3;
        if (sy >= y) {
            for (int j = 0; j < h; j++)
                memmove(v->fb + ((size_t)(y + j) * v->w + x) * 3,
                        v->fb + ((size_t)(sy + j) * v->w + sx) * 3, row);
        } else {
            for (int j = h - 1; j >= 0; j--)
                memmove(v->fb + ((size_t)(y + j) * v->w + x) * 3,
                        v->fb + ((size_t)(sy + j) * v->w + sx) * 3, row);
        }
        gv_consume(b, 16);
        break;
    }
    case 16: {
        if (avail < 16) return 0;
        size_t zlen = gv_u32(p + 12);
        if (zlen > GV_MAX_FRAME) return gv_fail(v, "zrle rect too large", NULL);
        if (avail < 16 + zlen) return 0;
        if (!in_bounds) return gv_fail(v, "zrle rect out of bounds", NULL);
        if (gv_zrle(v, x, y, w, h, p + 16, zlen) < 0) return -1;
        gv_consume(b, 16 + zlen);
        break;
    }
    case -223:
        gv_consume(b, 12);
        if (!w || !h) return gv_fail(v, "bad desktop size", NULL);
        if (gv_resize(v, w, h) < 0) return -1;
        v->want_full = 1;
        v->stats.rects++;
        return 1;
    case -224:
        gv_consume(b, 12);
        v->rects_left = 1;
        return 1;
    default: {
        char e[32];
        snprintf(e, sizeof e, "%d", enc);
        return gv_fail(v, "unsupported encoding %s", e);
    }
    }
    v->stats.rects++;
    gv_mark(v, x, y, w, h);
    return 1;
}

static int gv_rfb_process(grokvnc *v) {
    gv_buf *b = &v->rfb_in;
    while (v->state != GV_S_CLOSED) {
        size_t avail = gv_avail(b);
        const uint8_t *p = gv_head(b);
        switch (v->state) {
        case GV_S_VERSION:
            if (avail < 12) return 0;
            if (memcmp(p, "RFB 003.", 8)) return gv_fail(v, "not an RFB server", NULL);
            gv_consume(b, 12);
            if (gv_ws_send(v, 0x2, (const uint8_t *)"RFB 003.008\n", 12) < 0) return -1;
            v->state = GV_S_SECTYPES;
            break;
        case GV_S_SECTYPES: {
            if (avail < 1) return 0;
            size_t n = p[0];
            if (n == 0) {
                if (avail < 5) return 0;
                size_t rl = gv_u32(p + 1);
                if (avail < 5 + rl) return 0;
                char reason[200];
                snprintf(reason, sizeof reason, "%.*s", (int)(rl < 199 ? rl : 199), (const char *)p + 5);
                return gv_fail(v, "server refused: %s", reason);
            }
            if (avail < 1 + n) return 0;
            if (!memchr(p + 1, 1, n)) return gv_fail(v, "server requires authentication", NULL);
            gv_consume(b, 1 + n);
            uint8_t none = 1;
            if (gv_ws_send(v, 0x2, &none, 1) < 0) return -1;
            v->state = GV_S_SECRESULT;
            break;
        }
        case GV_S_SECRESULT: {
            if (avail < 4) return 0;
            if (gv_u32(p)) return gv_fail(v, "security handshake failed", NULL);
            gv_consume(b, 4);
            uint8_t shared = 1;
            if (gv_ws_send(v, 0x2, &shared, 1) < 0) return -1;
            v->state = GV_S_INIT;
            break;
        }
        case GV_S_INIT: {
            if (avail < 24) return 0;
            size_t nl = gv_u32(p + 20);
            if (nl > 1 << 20) return gv_fail(v, "bad ServerInit", NULL);
            if (avail < 24 + nl) return 0;
            int w = gv_u16(p), h = gv_u16(p + 2);
            snprintf(v->name, sizeof v->name, "%.*s", (int)(nl < 255 ? nl : 255), (const char *)p + 24);
            gv_consume(b, 24 + nl);
            if (!w || !h) return gv_fail(v, "bad ServerInit", NULL);
            if (gv_resize(v, w, h) < 0) return -1;
            v->state = GV_S_NORMAL;
            if (gv_on_server_init(v) < 0) return -1;
            break;
        }
        case GV_S_NORMAL:
            if (v->rects_left > 0) {
                int r = gv_rect(v);
                if (r <= 0) return r;
                if (--v->rects_left == 0) {
                    v->awaiting = 0;
                    v->stats.updates++;
                }
                break;
            }
            if (avail < 1) return 0;
            switch (p[0]) {
            case 0:
                if (avail < 4) return 0;
                v->rects_left = gv_u16(p + 2);
                gv_consume(b, 4);
                if (!v->rects_left) { v->awaiting = 0; v->stats.updates++; }
                break;
            case 1: {
                if (avail < 6) return 0;
                size_t need = 6 + 6 * (size_t)gv_u16(p + 4);
                if (avail < need) return 0;
                gv_consume(b, need);
                break;
            }
            case 2:
                gv_consume(b, 1);
                break;
            case 3: {
                if (avail < 8) return 0;
                size_t need = 8 + (size_t)gv_u32(p + 4);
                if (need > GV_MAX_FRAME) return gv_fail(v, "cut text too large", NULL);
                if (avail < need) return 0;
                gv_consume(b, need);
                break;
            }
            default: {
                char e[8];
                snprintf(e, sizeof e, "%d", p[0]);
                return gv_fail(v, "unknown server message %s", e);
            }
            }
            break;
        default:
            return -1;
        }
    }
    return -1;
}

/* ---- open ---- */

static void gv_seterr(grokvnc_err *e, int status, const char *fmt, const char *arg) {
    if (!e) return;
    e->http_status = status;
    snprintf(e->msg, sizeof e->msg, fmt, arg ? arg : "");
}

static const char gv_b64tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void gv_b64(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t t = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) |
                     (i + 2 < n ? in[i + 2] : 0);
        out[o++] = gv_b64tbl[(t >> 18) & 63];
        out[o++] = gv_b64tbl[(t >> 12) & 63];
        out[o++] = i + 1 < n ? gv_b64tbl[(t >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? gv_b64tbl[t & 63] : '=';
    }
    out[o] = 0;
}

static grokvnc *gv_alloc(void) {
    grokvnc *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->fd = -1;
    v->interval_ms = 50;
    return v;
}

static int gv_connect(const char *host, const char *port, int64_t deadline, grokvnc_err *err) {
    struct addrinfo hints = { 0 }, *res = NULL;
    hints.ai_socktype = SOCK_STREAM;
    int gr = getaddrinfo(host, port, &hints, &res);
    if (gr) { gv_seterr(err, 0, "resolve failed: %s", gai_strerror(gr)); return -1; }
    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        if (errno == EINPROGRESS) {
            struct pollfd p = { fd, POLLOUT, 0 };
            int64_t left = deadline - gv_now_ms();
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            if (left > 0 && poll(&p, 1, (int)left) == 1 &&
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0)
                break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) gv_seterr(err, 0, "connect to %s failed", host);
    return fd;
}

grokvnc *grokvnc_open(const char *url, const char *const *headers, grokvnc_err *err) {
    if (err) memset(err, 0, sizeof *err);
    int tls;
    const char *rest;
    if (!strncmp(url, "wss://", 6)) { tls = 1; rest = url + 6; }
    else if (!strncmp(url, "ws://", 5)) { tls = 0; rest = url + 5; }
    else { gv_seterr(err, 0, "unsupported url scheme", NULL); return NULL; }
#ifndef __APPLE__
    if (tls) { gv_seterr(err, 0, "wss:// requires macOS SecureTransport", NULL); return NULL; }
#endif
    size_t hl = strcspn(rest, "/?");
    char hostport[512], host[512], port[16];
    if (!hl || hl >= sizeof hostport) { gv_seterr(err, 0, "bad url host", NULL); return NULL; }
    memcpy(hostport, rest, hl);
    hostport[hl] = 0;
    const char *path = rest[hl] ? rest + hl : "/";
    char *colon = strrchr(hostport, ':');
    if (colon && !strchr(colon, ']')) {
        snprintf(port, sizeof port, "%s", colon + 1);
        *colon = 0;
    } else {
        snprintf(port, sizeof port, "%s", tls ? "443" : "80");
    }
    snprintf(host, sizeof host, "%s", hostport[0] == '[' ? hostport + 1 : hostport);
    char *rb = strchr(host, ']');
    if (rb) *rb = 0;

    grokvnc *v = gv_alloc();
    if (!v) { gv_seterr(err, 0, "out of memory", NULL); return NULL; }
    int64_t deadline = gv_now_ms() + GV_TIMEOUT_MS;
    v->fd = gv_connect(host, port, deadline, err);
    if (v->fd < 0) { grokvnc_close(v); return NULL; }
    v->transport = GV_T_PLAIN;

#ifdef __APPLE__
    if (tls) {
        v->ssl = SSLCreateContext(NULL, kSSLClientSide, kSSLStreamType);
        if (!v->ssl) { gv_seterr(err, 0, "tls context failed", NULL); grokvnc_close(v); return NULL; }
        SSLSetIOFuncs(v->ssl, gv_ssl_read, gv_ssl_write);
        SSLSetConnection(v->ssl, (SSLConnectionRef)(uintptr_t)v);
        SSLSetPeerDomainName(v->ssl, host, strlen(host));
        SSLSetProtocolVersionMin(v->ssl, kTLSProtocol12);
        v->transport = GV_T_TLS;
        for (;;) {
            OSStatus st = SSLHandshake(v->ssl);
            if (st == noErr) break;
            if (st != errSSLWouldBlock || !gv_wait(v, deadline, 0)) {
                char code[16];
                snprintf(code, sizeof code, "%d", (int)st);
                gv_seterr(err, 0, st == errSSLWouldBlock ? "tls handshake timed out" : "tls handshake failed: %s", code);
                grokvnc_close(v);
                return NULL;
            }
        }
    }
#endif

    uint8_t keyraw[16];
    char key[32];
    arc4random_buf(keyraw, sizeof keyraw);
    gv_b64(keyraw, sizeof keyraw, key);
    gv_buf *req = &v->scratch;
    char line[4096];
    int n = snprintf(line, sizeof line,
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n"
                     "Sec-WebSocket-Protocol: binary\r\nUser-Agent: grokvnc\r\n",
                     path, hostport, key);
    if (n < 0 || (size_t)n >= sizeof line || !gv_buf_append(req, (uint8_t *)line, (size_t)n)) {
        gv_seterr(err, 0, "request too long", NULL);
        grokvnc_close(v);
        return NULL;
    }
    for (const char *const *h = headers; h && *h; h++) {
        if (!gv_buf_append(req, (uint8_t *)*h, strlen(*h)) || !gv_buf_append(req, (uint8_t *)"\r\n", 2)) {
            gv_seterr(err, 0, "out of memory", NULL);
            grokvnc_close(v);
            return NULL;
        }
    }
    gv_buf_append(req, (uint8_t *)"\r\n", 2);
    memset(line, 0, sizeof line);
    int wr = gv_write(v, req->p, req->len);
    memset(req->p, 0, req->len);
    req->len = 0;
    if (wr < 0) { gv_seterr(err, 0, "%s", v->err); grokvnc_close(v); return NULL; }

    uint8_t chunk[4096];
    char *hdr_end = NULL;
    while (!hdr_end) {
        while (gv_avail(&v->raw_out) && gv_now_ms() < deadline) {
            gv_flush_raw(v);
            if (gv_avail(&v->raw_out)) gv_wait(v, deadline, 1);
        }
        ssize_t r = gv_read(v, chunk, sizeof chunk);
        if (r < 0) { gv_seterr(err, 0, "%s", v->err); grokvnc_close(v); return NULL; }
        if (r > 0) {
            if (!gv_buf_append(&v->ws_in, chunk, (size_t)r) || !gv_buf_reserve(&v->ws_in, 1)) {
                gv_seterr(err, 0, "out of memory", NULL);
                grokvnc_close(v);
                return NULL;
            }
            v->ws_in.p[v->ws_in.len] = 0;
            hdr_end = strstr((char *)v->ws_in.p, "\r\n\r\n");
            if (!hdr_end && v->ws_in.len > GV_MAX_HTTP) {
                gv_seterr(err, 0, "upgrade response too large", NULL);
                grokvnc_close(v);
                return NULL;
            }
            continue;
        }
        if (!gv_wait(v, deadline, 0) || gv_now_ms() >= deadline) {
            gv_seterr(err, 0, "upgrade timed out", NULL);
            grokvnc_close(v);
            return NULL;
        }
    }

    char *resp = (char *)v->ws_in.p;
    size_t resp_len = (size_t)(hdr_end - resp) + 4;
    *hdr_end = 0;
    int status = 0;
    if (sscanf(resp, "HTTP/1.%*d %d", &status) != 1 || status != 101) {
        char first[128];
        snprintf(first, sizeof first, "%.*s", (int)strcspn(resp, "\r\n"), resp);
        gv_seterr(err, status, "upgrade rejected: %s", first);
        grokvnc_close(v);
        return NULL;
    }
#ifdef __APPLE__
    {
        char accept_src[64], expect[32];
        uint8_t sha[CC_SHA1_DIGEST_LENGTH];
        snprintf(accept_src, sizeof accept_src, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
        CC_SHA1(accept_src, (CC_LONG)strlen(accept_src), sha);
        gv_b64(sha, sizeof sha, expect);
        int ok = 0;
        for (char *l = strstr(resp, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
            if (!strncasecmp(l + 2, "sec-websocket-accept:", 21)) {
                char *val = l + 23;
                while (*val == ' ') val++;
                ok = !strncmp(val, expect, strlen(expect));
                break;
            }
        }
        if (!ok) {
            gv_seterr(err, status, "bad Sec-WebSocket-Accept", NULL);
            grokvnc_close(v);
            return NULL;
        }
    }
#endif
    gv_consume(&v->ws_in, resp_len);
    if (gv_ws_process(v) < 0) {
        gv_seterr(err, 101, "%s", v->err);
        grokvnc_close(v);
        return NULL;
    }
    return v;
}

void grokvnc_close(grokvnc *v) {
    if (!v) return;
    if (v->transport != GV_T_NONE && v->fd >= 0 && v->state != GV_S_CLOSED && !v->close_sent) {
        uint8_t code[2] = { 0x03, 0xe8 };
        v->close_sent = 1;
        gv_ws_send(v, 0x8, code, 2);
        gv_flush_raw(v);
    }
#ifdef __APPLE__
    if (v->ssl) {
        SSLClose(v->ssl);
        gv_flush_raw(v);
        CFRelease(v->ssl);
    }
#endif
    if (v->fd >= 0) close(v->fd);
    if (v->zs_init) inflateEnd(&v->zs);
    free(v->raw_out.p);
    free(v->ws_in.p);
    free(v->rfb_in.p);
    free(v->scratch.p);
    free(v->zbuf.p);
    free(v->fb);
    free(v);
}

#ifdef __APPLE__
#pragma clang diagnostic pop
#endif

/* ---- runtime ---- */

int grokvnc_pump(grokvnc *v) {
    if (!v || v->state == GV_S_CLOSED) return 0;
    if (v->transport != GV_T_NONE) {
        uint8_t chunk[GV_READ_CHUNK];
        for (int i = 0; i < 1024 && v->state != GV_S_CLOSED; i++) {
            ssize_t r = gv_read(v, chunk, sizeof chunk);
            if (r <= 0) break;
            if (gv_feed(v, chunk, (size_t)r) < 0) break;
        }
    }
    if (v->state == GV_S_NORMAL && !v->awaiting) {
        if (v->want_full) {
            v->want_full = 0;
            gv_send_request(v, 0);
        } else if (gv_now_ms() - v->last_request_ms >= v->interval_ms) {
            gv_send_request(v, 1);
        }
    }
    if (v->state != GV_S_CLOSED && gv_flush_raw(v) < 0)
        gv_fail(v, "write failed: %s", strerror(errno));
    return v->state != GV_S_CLOSED;
}

int grokvnc_fd(const grokvnc *v) { return v ? v->fd : -1; }
int grokvnc_want_write(const grokvnc *v) { return v && gv_avail(&v->raw_out) > 0; }

int grokvnc_timeout_ms(const grokvnc *v) {
    if (!v || v->state != GV_S_NORMAL || v->awaiting) return -1;
    if (v->want_full) return 0;
    int64_t left = v->last_request_ms + v->interval_ms - gv_now_ms();
    return left < 0 ? 0 : (int)left;
}

const char *grokvnc_error(const grokvnc *v) { return v ? v->err : "no client"; }
int grokvnc_ready(const grokvnc *v) { return v && v->fb && v->state == GV_S_NORMAL; }
const char *grokvnc_name(const grokvnc *v) { return v ? v->name : ""; }
void grokvnc_stats_get(const grokvnc *v, grokvnc_stats *out) { *out = v->stats; }
void grokvnc_set_interval(grokvnc *v, int ms) { v->interval_ms = ms < 0 ? 0 : ms; }
void grokvnc_request_full(grokvnc *v) { v->want_full = 1; }

const uint8_t *grokvnc_frame(const grokvnc *v, int *w, int *h, uint64_t *generation) {
    if (w) *w = v->w;
    if (h) *h = v->h;
    if (generation) *generation = v->generation;
    return v->fb;
}

int grokvnc_take_dirty(grokvnc *v, grokvnc_rect *out) {
    if (!v->dirty) return 0;
    if (out) *out = v->dirty_rect;
    v->dirty = 0;
    return 1;
}

int grokvnc_send_pointer(grokvnc *v, int x, int y, int mask) {
    if (v->state != GV_S_NORMAL) return -1;
    uint8_t m[6] = { 5, (uint8_t)mask };
    gv_put16(m + 2, (uint16_t)(x < 0 ? 0 : x));
    gv_put16(m + 4, (uint16_t)(y < 0 ? 0 : y));
    return gv_ws_send(v, 0x2, m, sizeof m);
}

int grokvnc_send_key(grokvnc *v, uint32_t keysym, int down) {
    if (v->state != GV_S_NORMAL) return -1;
    uint8_t m[8] = { 4, (uint8_t)(down != 0) };
    gv_put32(m + 4, keysym);
    return gv_ws_send(v, 0x2, m, sizeof m);
}

grokvnc *grokvnc_new_detached(void) { return gv_alloc(); }

int grokvnc_inject(grokvnc *v, const uint8_t *p, size_t n) {
    if (v->state == GV_S_CLOSED) return -1;
    return gv_feed(v, p, n) < 0 ? -1 : 0;
}

#endif
