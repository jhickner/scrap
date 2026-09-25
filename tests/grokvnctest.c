#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define GROKVNC_IMPLEMENTATION
#include "vnc/grokvnc.h"

static void require(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "grokvnctest: %s\n", message);
        exit(1);
    }
}

struct out {
    uint8_t *p;
    size_t n, cap;
};

static void put(struct out *o, const void *p, size_t n)
{
    if (o->n + n > o->cap) {
        o->cap = (o->n + n) * 2 + 256;
        o->p = realloc(o->p, o->cap);
        require(o->p != NULL, "out of memory");
    }
    memcpy(o->p + o->n, p, n);
    o->n += n;
}

static void put8(struct out *o, unsigned v) { uint8_t b = (uint8_t)v; put(o, &b, 1); }
static void put16(struct out *o, unsigned v) { put8(o, v >> 8); put8(o, v); }
static void put32(struct out *o, uint32_t v) { put16(o, v >> 16); put16(o, v & 0xffff); }

static void rect_hdr(struct out *o, int x, int y, int w, int h, int32_t enc)
{
    put16(o, (unsigned)x); put16(o, (unsigned)y); put16(o, (unsigned)w); put16(o, (unsigned)h);
    put32(o, (uint32_t)enc);
}

static void zrle_rect(struct out *o, z_stream *zs, int x, int y, int w, int h,
                      const uint8_t *tile, size_t n)
{
    uint8_t z[1024];
    zs->next_in = (Bytef *)(uintptr_t)tile;
    zs->avail_in = (uInt)n;
    zs->next_out = z;
    zs->avail_out = sizeof z;
    require(deflate(zs, Z_SYNC_FLUSH) == Z_OK, "deflate failed");
    rect_hdr(o, x, y, w, h, 16);
    put32(o, (uint32_t)(sizeof z - zs->avail_out));
    put(o, z, sizeof z - zs->avail_out);
}

static struct out frames(const struct out *rfb, size_t max)
{
    struct out ws = {0};
    for (size_t off = 0; off < rfb->n; off += max) {
        size_t n = rfb->n - off < max ? rfb->n - off : max;
        put8(&ws, 0x82);
        if (n < 126) {
            put8(&ws, (unsigned)n);
        } else {
            put8(&ws, 126);
            put16(&ws, (unsigned)n);
        }
        put(&ws, rfb->p + off, n);
    }
    return ws;
}

static const uint8_t *px(const uint8_t *fb, int w, int x, int y) { return fb + (y * w + x) * 3; }

static int is_rgb(const uint8_t *p, int r, int g, int b) { return p[0] == r && p[1] == g && p[2] == b; }

static struct out build_stream(void)
{
    struct out o = {0};
    put(&o, "RFB 003.008\n", 12);
    put8(&o, 2); put8(&o, 2); put8(&o, 1);
    put32(&o, 0);
    put16(&o, 8); put16(&o, 6);
    uint8_t pf[16] = { 32, 24, 0, 1, 0, 255, 0, 255, 0, 255, 16, 8, 0 };
    put(&o, pf, sizeof pf);
    put32(&o, 4); put(&o, "test", 4);

    put8(&o, 2);
    put8(&o, 3); put8(&o, 0); put16(&o, 0); put32(&o, 4); put(&o, "clip", 4);

    z_stream zs = {0};
    require(deflateInit(&zs, 6) == Z_OK, "deflateInit failed");

    put8(&o, 0); put8(&o, 0); put16(&o, 6);
    rect_hdr(&o, 0, 0, 2, 2, 0);
    const uint8_t raw[] = { 30, 20, 10, 0, 60, 50, 40, 0, 90, 80, 70, 0, 120, 110, 100, 0 };
    put(&o, raw, sizeof raw);
    rect_hdr(&o, 4, 0, 2, 2, 1);
    put16(&o, 0); put16(&o, 0);
    const uint8_t packed[] = { 2, 0, 0, 255, 255, 0, 0, 0xAA, 0x55 };
    zrle_rect(&o, &zs, 0, 2, 8, 2, packed, sizeof packed);
    const uint8_t palrle[] = { 130, 0, 255, 0, 255, 255, 255, 0x80, 9, 1, 0x80, 4 };
    zrle_rect(&o, &zs, 0, 4, 8, 2, palrle, sizeof palrle);
    const uint8_t plainrle[] = { 128, 0x40, 0x40, 0x40, 3 };
    zrle_rect(&o, &zs, 6, 0, 2, 2, plainrle, sizeof plainrle);
    const uint8_t rawtile[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
    zrle_rect(&o, &zs, 2, 0, 2, 2, rawtile, sizeof rawtile);

    put8(&o, 0); put8(&o, 0); put16(&o, 3);
    const uint8_t solid[] = { 1, 9, 8, 7 };
    zrle_rect(&o, &zs, 7, 5, 1, 1, solid, sizeof solid);
    rect_hdr(&o, 0, 0, 0, 0, -224);

    deflateEnd(&zs);
    return o;
}

static void check_frame(grokvnc *v)
{
    int w, h;
    uint64_t gen;
    const uint8_t *fb = grokvnc_frame(v, &w, &h, &gen);
    require(fb && w == 8 && h == 6, "framebuffer size");
    require(gen > 0, "generation not advanced");
    require(!strcmp(grokvnc_name(v), "test"), "desktop name");
    require(is_rgb(px(fb, w, 0, 0), 10, 20, 30), "raw pixel 0,0");
    require(is_rgb(px(fb, w, 1, 1), 100, 110, 120), "raw pixel 1,1");
    require(is_rgb(px(fb, w, 4, 0), 10, 20, 30), "copyrect pixel 4,0");
    require(is_rgb(px(fb, w, 5, 1), 100, 110, 120), "copyrect pixel 5,1");
    require(is_rgb(px(fb, w, 2, 0), 3, 2, 1), "zrle raw tile 2,0");
    require(is_rgb(px(fb, w, 3, 1), 12, 11, 10), "zrle raw tile 3,1");
    require(is_rgb(px(fb, w, 6, 0), 0x40, 0x40, 0x40), "zrle plain rle");
    require(is_rgb(px(fb, w, 7, 1), 0x40, 0x40, 0x40), "zrle plain rle end");
    require(is_rgb(px(fb, w, 0, 2), 0, 0, 255), "zrle packed 0,2");
    require(is_rgb(px(fb, w, 1, 2), 255, 0, 0), "zrle packed 1,2");
    require(is_rgb(px(fb, w, 0, 3), 255, 0, 0), "zrle packed 0,3");
    require(is_rgb(px(fb, w, 7, 3), 0, 0, 255), "zrle packed 7,3");
    require(is_rgb(px(fb, w, 1, 5), 0, 255, 0), "zrle palette rle run 1");
    require(is_rgb(px(fb, w, 2, 5), 255, 255, 255), "zrle palette rle single");
    require(is_rgb(px(fb, w, 3, 5), 0, 255, 0), "zrle palette rle run 2");
    require(is_rgb(px(fb, w, 7, 5), 7, 8, 9), "zrle solid tile after shared zlib stream");

    grokvnc_rect r;
    require(grokvnc_take_dirty(v, &r) && r.x == 0 && r.y == 0 && r.w == 8 && r.h == 6, "dirty rect");
    require(!grokvnc_take_dirty(v, &r), "dirty not cleared");
    grokvnc_stats st;
    grokvnc_stats_get(v, &st);
    require(st.updates == 2, "update count");
}

static void run(const struct out *ws, size_t chunk)
{
    grokvnc *v = grokvnc_new_detached();
    require(v != NULL, "detached client");
    for (size_t off = 0; off < ws->n; off += chunk) {
        size_t n = ws->n - off < chunk ? ws->n - off : chunk;
        require(grokvnc_inject(v, ws->p + off, n) == 0, grokvnc_error(v));
    }
    require(grokvnc_ready(v), "not ready after ServerInit");
    check_frame(v);

    struct out tail = {0};
    put8(&tail, 0x89); put8(&tail, 2); put(&tail, "hi", 2);
    put8(&tail, 0x82); put8(&tail, 16);
    put8(&tail, 0); put8(&tail, 0); put16(&tail, 1);
    rect_hdr(&tail, 0, 0, 4, 4, -223);
    require(grokvnc_inject(v, tail.p, tail.n) == 0, grokvnc_error(v));
    int w, h;
    require(grokvnc_frame(v, &w, &h, NULL) && w == 4 && h == 4, "desktop resize");
    require(grokvnc_timeout_ms(v) == 0, "full request not due after resize");
    require(grokvnc_send_pointer(v, 1, 1, 1) == 0 && grokvnc_send_key(v, 0xff0d, 1) == 0, "input send");

    const uint8_t close_frame[] = { 0x88, 2, 0x03, 0xe8 };
    require(grokvnc_inject(v, close_frame, sizeof close_frame) < 0, "close frame not reported");
    require(!grokvnc_pump(v), "pump after close");
    free(tail.p);
    grokvnc_close(v);
}

int main(void)
{
    struct out rfb = build_stream();
    struct out big = frames(&rfb, 4096);
    struct out small = frames(&rfb, 7);
    run(&big, big.n);
    run(&big, 1);
    run(&small, 3);

    grokvnc *v = grokvnc_new_detached();
    const uint8_t refuse[] = { 0x82, 12, 'R', 'F', 'B', ' ', '0', '0', '3', '.', '0', '0', '8', '\n',
                               0x82, 1, 1, 0x82, 1, 2 };
    require(grokvnc_inject(v, refuse, sizeof refuse) < 0, "auth-only server accepted");
    require(strstr(grokvnc_error(v), "authentication") != NULL, "auth error message");
    grokvnc_close(v);

    free(rfb.p); free(big.p); free(small.p);
    return 0;
}
