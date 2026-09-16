/* Connects to Grok Bot desktops read-only, saves the first full frame as PPM,
 * and reports update rate. Targets: "base" or an agent id/name.
 * usage: vncprobe [-t seconds] [-o dir] [target...]   (default: base BCG) */

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "agents/grokbot/grokbot.h"
#include "vnc/grokvnc.h"

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int endpoint(grokbot *g, const char *target, grokbot_vnc_endpoint *ep)
{
    if (!strcmp(target, "base"))
        return grokbot_vnc_endpoint_for(g, NULL, ep);
    grokbot_box box;
    if (!grokbot_box_status(g, target, &box))
        return 0;
    printf("%s: state=%s display=%d handoff=%d\n", target, box.state, box.display, box.handoff);
    int ok = grokbot_vnc_endpoint_for(g, &box, ep);
    grokbot_box_free(&box);
    return ok;
}

static grokvnc *connect_target(grokbot *g, const char *target)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        grokbot_vnc_endpoint ep;
        if (!endpoint(g, target, &ep)) {
            fprintf(stderr, "%s: %s\n", target, grokbot_error(g));
            return NULL;
        }
        const char *hdr[] = { ep.header, NULL };
        grokvnc_err err;
        grokvnc *v = grokvnc_open(ep.url, hdr, &err);
        memset(&ep, 0, sizeof ep);
        if (v)
            return v;
        fprintf(stderr, "%s: %s\n", target, err.msg);
        if (attempt || (err.http_status != 401 && err.http_status != 404) || !grokbot_reload(g))
            return NULL;
        fprintf(stderr, "%s: descriptor reloaded, retrying\n", target);
    }
    return NULL;
}

static int save_ppm(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    size_t n = fwrite(rgb, 3, (size_t)w * (size_t)h, f);
    return fclose(f) == 0 && n == (size_t)w * (size_t)h;
}

static int wait_io(grokvnc *v, int max_ms)
{
    int t = grokvnc_timeout_ms(v);
    if (t < 0 || t > max_ms)
        t = max_ms;
    struct pollfd p = { grokvnc_fd(v), (short)(POLLIN | (grokvnc_want_write(v) ? POLLOUT : 0)), 0 };
    return poll(&p, 1, t);
}

static int probe(grokbot *g, const char *target, double seconds, const char *dir)
{
    double t0 = now_s();
    grokvnc *v = connect_target(g, target);
    if (!v)
        return 1;
    double t_open = now_s();

    grokvnc_stats st;
    do {
        if (!grokvnc_pump(v)) {
            fprintf(stderr, "%s: %s\n", target, grokvnc_error(v));
            grokvnc_close(v);
            return 1;
        }
        grokvnc_stats_get(v, &st);
        if (st.updates)
            break;
        if (now_s() - t0 > 30) {
            fprintf(stderr, "%s: no framebuffer update within 30 s\n", target);
            grokvnc_close(v);
            return 1;
        }
        wait_io(v, 250);
    } while (1);
    double t_first = now_s();

    int w, h;
    uint64_t gen;
    const uint8_t *rgb = grokvnc_frame(v, &w, &h, &gen);
    char path[1024];
    snprintf(path, sizeof path, "%s/vncprobe-%s.ppm", dir, target);
    if (!save_ppm(path, rgb, w, h))
        fprintf(stderr, "%s: could not write %s\n", target, path);
    printf("%s: %s %dx%d open %.0f ms, first frame %.0f ms (%.1f MB), saved %s\n", target,
           grokvnc_name(v), w, h, (t_open - t0) * 1e3, (t_first - t0) * 1e3,
           (double)st.bytes_in / 1e6, path);

    grokvnc_stats base = st;
    double start = now_s();
    while (now_s() - start < seconds) {
        if (!grokvnc_pump(v)) {
            fprintf(stderr, "%s: %s\n", target, grokvnc_error(v));
            break;
        }
        wait_io(v, 100);
    }
    double el = now_s() - start;
    grokvnc_stats_get(v, &st);
    printf("%s: %.1f s: %.2f updates/s, %llu rects, %.3f MB/s in\n",
           target, el, (double)(st.updates - base.updates) / el,
           (unsigned long long)(st.rects - base.rects),
           (double)(st.bytes_in - base.bytes_in) / 1e6 / el);
    grokvnc_close(v);
    return 0;
}

int main(int argc, char **argv)
{
    double seconds = 10;
    const char *dir = "/tmp";
    int opt;
    while ((opt = getopt(argc, argv, "t:o:")) != -1) {
        if (opt == 't')
            seconds = atof(optarg);
        else if (opt == 'o')
            dir = optarg;
        else {
            fprintf(stderr, "usage: vncprobe [-t seconds] [-o dir] [target...]\n");
            return 2;
        }
    }
    grokbot *g = grokbot_open(NULL);
    if (!g) {
        fprintf(stderr, "grokbot: %s\n", grokbot_error(NULL));
        return 1;
    }
    static const char *defaults[] = { "base", "BCG" };
    const char **targets = optind < argc ? (const char **)argv + optind : defaults;
    int n = optind < argc ? argc - optind : 2;
    int rc = 0;
    for (int i = 0; i < n; i++)
        rc |= probe(g, targets[i], seconds, dir);
    grokbot_close(g);
    return rc;
}
