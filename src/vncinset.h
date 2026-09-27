#ifndef VNCINSET_H
#define VNCINSET_H

#include <stdint.h>

#define VNCINSET_WIDTH_DEFAULT 40
#define VNCINSET_WIDTH_MIN     10
#define VNCINSET_WIDTH_MAX     100

enum vncinset_side { VNCINSET_RIGHT, VNCINSET_LEFT };

struct vncinset_frame {
    const uint8_t *rgb;
    int            w, h;
    uint64_t       gen;
};

struct vncinset_source {
    int         (*frame)(struct vncinset_source *src, struct vncinset_frame *out);
    void        (*close)(struct vncinset_source *src);
    const char *(*status)(struct vncinset_source *src);
};

#define VNCINSET_STUB_FPS 5
struct vncinset_source *vncinset_stub_open(void);

typedef struct vncinset_source *(*vncinset_open_fn)(const char *bot);
void vncinset_set_opener(vncinset_open_fn fn);

#define VNCINSET_FRAME_INTERVAL 0.2

#define VNCINSET_TRANSMIT_W_MAX 800

void vncinset_downscale(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh);

struct vncinset_box {
    int row, col, w, h;
    int img_cols, img_rows;
};

int vncinset_layout(enum vncinset_side side, int pct, int cols, int rows, int frame_w,
                    int frame_h, int cell_w, int cell_h, struct vncinset_box *out);

struct vncinset;

struct vncinset *vncinset_new(const char *bot);
void             vncinset_free(struct vncinset *v);
void             vncinset_set_bot(struct vncinset *v, const char *bot);

void             vncinset_set_test(struct vncinset *v, int on);

int  vncinset_shown(const struct vncinset *v);
void vncinset_show(struct vncinset *v, int on);
void vncinset_set_side(struct vncinset *v, enum vncinset_side side);
void vncinset_set_width(struct vncinset *v, int pct);

int vncinset_stale(struct vncinset *v);

void vncinset_cover(struct vncinset *v, char **rows, int n, int cols);

#endif
