#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TERM_H
static char out[4096];
static size_t out_n;
static void term_write_n(const char *s, int n) { memcpy(out + out_n, s, (size_t)n); out_n += (size_t)n; }
static void term_write(const char *s) { term_write_n(s, (int)strlen(s)); }
static void term_flush(void) {}
static void term_move_cursor(int col, int row) { (void)col; (void)row; }

#define KITTY_IMPLEMENTATION
#include "../src/vendor/kitty.h"

static size_t unwrap(const char *in, size_t n, char *dst)
{
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        if (i + 7 <= n && !memcmp(in + i, "\x1bPtmux;", 7)) {
            i += 7;
            for (;;) {
                assert(i + 1 < n);
                if (in[i] == '\x1b' && in[i + 1] == '\x1b') { dst[o++] = '\x1b'; i += 2; }
                else if (in[i] == '\x1b' && in[i + 1] == '\\') { i += 2; break; }
                else dst[o++] = in[i++];
            }
        } else {
            dst[o++] = in[i++];
        }
    }
    return o;
}

static const char CMD[] = "\x1b_Ga=d,d=A,q=2\x1b\\";

static void check(int depth, int batch)
{
    char a[4096], b[4096];
    out_n = 0;
    kg_set_passthrough_depth(depth);
    if (batch) kg_batch_begin();
    kg_delete_all();
    kg_delete_all();
    if (batch) kg_batch_end();

    memcpy(a, out, out_n);
    size_t n = out_n;
    for (int d = 0; d < depth; d++) {
        n = unwrap(a, n, b);
        memcpy(a, b, n);
    }
    char want[128];
    snprintf(want, sizeof want, "%s%s", CMD, CMD);
    assert(n == strlen(want) && !memcmp(a, want, n));
}

int main(void)
{
    for (int d = 0; d <= KG_DEPTH_MAX; d++) {
        check(d, 0);
        check(d, 1);
    }
    puts("kittywraptest: ok");
    return 0;
}
