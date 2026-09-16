#ifndef VNCINSET_H
#define VNCINSET_H

#include <stdint.h>

#define VNCINSET_WIDTH_DEFAULT 40
#define VNCINSET_WIDTH_MIN     10
#define VNCINSET_WIDTH_MAX     100

enum vncinset_side { VNCINSET_RIGHT, VNCINSET_LEFT };

struct vncinset_frame {
    const uint8_t *rgb; /* RGB24, w * h * 3 bytes, valid until the next call */
    int            w, h;
    uint64_t       gen; /* changes whenever the pixels do */
};

/* A frame source. `frame` fills `out` with the latest frame and returns 1, or
   returns 0 while none is available. `status`, optional, names the
   connection state for the title; NULL or "" when live. */
struct vncinset_source {
    int         (*frame)(struct vncinset_source *src, struct vncinset_frame *out);
    void        (*close)(struct vncinset_source *src);
    const char *(*status)(struct vncinset_source *src);
};

/* moving test pattern at VNCINSET_STUB_FPS */
#define VNCINSET_STUB_FPS 5
struct vncinset_source *vncinset_stub_open(void);

typedef struct vncinset_source *(*vncinset_open_fn)(const char *bot);
void vncinset_set_opener(vncinset_open_fn fn);

/* minimum seconds between transmitted frames */
#define VNCINSET_FRAME_INTERVAL 0.2
/* widest frame transmitted; the terminal scales it to the cell box */
#define VNCINSET_TRANSMIT_W_MAX 800

/* box-filter downscale of RGB24 src into dst (dw * dh * 3 bytes) */
void vncinset_downscale(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh);

/* outer box in cells, border included; image fills the interior */
struct vncinset_box {
    int row, col, w, h;
    int img_cols, img_rows;
};

/* 0 when the area is too small for an inset */
int vncinset_layout(enum vncinset_side side, int pct, int cols, int rows, int frame_w,
                    int frame_h, int cell_w, int cell_h, struct vncinset_box *out);

struct vncinset;

struct vncinset *vncinset_new(const char *bot);
void             vncinset_free(struct vncinset *v);
void             vncinset_set_bot(struct vncinset *v, const char *bot);
/* 1 draws the stub pattern instead of the opener's source */
void             vncinset_set_test(struct vncinset *v, int on);

int  vncinset_shown(const struct vncinset *v);
void vncinset_show(struct vncinset *v, int on);
void vncinset_set_side(struct vncinset *v, enum vncinset_side side);
void vncinset_set_width(struct vncinset *v, int pct);
int  vncinset_width(const struct vncinset *v);
enum vncinset_side vncinset_side(const struct vncinset *v);

/* 1 when the source has a frame newer than the last one drawn */
int vncinset_stale(struct vncinset *v);

/* composites the inset onto the transcript rows of a paint (a
   viewport_cover_fn); v NULL or hidden leaves the rows untouched */
void vncinset_cover(struct vncinset *v, char **rows, int n, int cols);

#endif
