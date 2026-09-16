
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

/* as image_fit, but fills the box even when that enlarges the image */
void image_fill(int img_w, int img_h, int cw, int ch, int cols_box, int rows_box,
                int *cols, int *rows);

/* every image drawn this session, in the order it appeared */
int         image_count(void);
const char *image_path_at(int i);
int         image_index_of(uint32_t id);

/* transmit `path` fitted to a cols x rows cell box under the id the viewer
   reuses, reporting the cells it fills. 0 when the file cannot be read. */
uint32_t image_load(const char *path, int cols_box, int rows_box, int *cols, int *rows);

/* write the placeholder cells for an already transmitted image, no trailing
   newline */
void image_place(uint32_t id, int indent, int cols, int rows);

void image_drop(uint32_t id);

void     image_cell_size(int *cw, int *ch);
int      image_cells_max(void);

/* the id the desktop inset draws under, distinct from every transcript image */
uint32_t image_inset_id(void);

/* transmit RGB24 pixels under `id`, placed virtually over cols x rows cells */
void image_frame(uint32_t id, const uint8_t *rgb, int w, int h, int cols, int rows);

/* one row of placeholder cells for a virtually placed image, no newline */
void image_place_row(uint32_t id, int row, int cols);

void image_poll(void);

void image_wait(void);

#define IMAGE_PLACED_KIND "image"
void image_placed_load(const cJSON *st);

#endif
