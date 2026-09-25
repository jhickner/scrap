
#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>

#include "vendor/cJSON.h"

#define IMAGE_ROWS_DEFAULT 20
#define IMAGE_ROWS_MIN     2
#define IMAGE_ROWS_MAX     100

void image_init(void);

void image_set_rows(int rows);
int  image_rows(void);

int image_available(void);

int image_show(const char *path, int indent);

void image_fit(int img_w, int img_h, int cw, int ch, int cols_box, int rows_box,
               int *cols, int *rows);

void image_fill(int img_w, int img_h, int cw, int ch, int cols_box, int rows_box,
                int *cols, int *rows);

int         image_count(void);
const char *image_path_at(int i);
int         image_index_of(uint32_t id);

uint32_t image_load(const char *path, int cols_box, int rows_box, int *cols, int *rows);

void image_place(uint32_t id, int indent, int cols, int rows);

void image_drop(uint32_t id);

void     image_cell_size(int *cw, int *ch);
int      image_cells_max(void);

uint32_t image_inset_id(void);

void image_frame(uint32_t id, const uint8_t *rgb, int w, int h, int cols, int rows);

void image_place_row(uint32_t id, int row, int cols);

void image_poll(void);

void image_wait(void);

#define IMAGE_PLACED_KIND "image"
void image_placed_load(const cJSON *st);

#endif
