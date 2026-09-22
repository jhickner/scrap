#ifndef CANVAS_H
#define CANVAS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "glyphs.h"
#include "internal.h"

typedef struct {
    size_t w, h;
    uint32_t *ch;
    unsigned char *cls;
    unsigned char *mask;
    unsigned char *style;
    bool *occupied;
    unsigned char cur_style;
} Canvas;

typedef struct {
    size_t x, y, w, h, cx, cy, rank;
} Placed;

typedef enum { EXTRA_PLAIN, EXTRA_FRAME, EXTRA_COMPARTMENTS } ExtraKind;
typedef struct {
    ExtraKind kind;
    Canvas *frame;
    char ***sections;
    size_t *section_n;
    size_t n_sections;
} NodeExtra;

Canvas *canvas_new(size_t w, size_t h);
void canvas_free(Canvas *c);
size_t canvas_idx(const Canvas *c, size_t x, size_t y);
void canvas_set(Canvas *c, size_t x, size_t y, uint32_t ch, Cls cls);
void canvas_add_bits(Canvas *c, size_t x, size_t y, unsigned bits);
void canvas_blit(Canvas *c, const Canvas *sub, size_t ox, size_t oy);
void canvas_junction(Canvas *c, size_t x, size_t y, unsigned bits);
void canvas_seg_v(Canvas *c, size_t x, size_t y0, size_t y1);
void canvas_seg_h(Canvas *c, size_t y, size_t x0, size_t x1);
void canvas_finalize_mask(Canvas *c);
void canvas_flip_vertical(Canvas *c);
void canvas_flip_horizontal(Canvas *c);
char **canvas_to_lines(const Canvas *c, size_t *n_out);

void draw_box(Canvas *c, const Placed *p, char **lines, size_t n_lines, Shape shape);
void draw_seq_text(Canvas *c, const char *text, size_t x, size_t y, Cls cls);
void place_label(Canvas *c, const char *label, size_t row, size_t start_x);
uint32_t head_glyph(Head head, uint32_t arrow);

Canvas *layout_canvas(const Graph *g, NodeExtra *extras, int max_width, Oversize *err);
MermaidArt *mermaid_art_from_canvas(Canvas *c);
void draw_class_box(Canvas *c, const Placed *p, char ***sections, size_t *section_n, size_t n_sections);
void draw_frame(Canvas *c, const Placed *p, const char *title, const Canvas *sub);

#endif
