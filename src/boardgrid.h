#ifndef BOARDGRID_H
#define BOARDGRID_H

struct board_tile;
struct menu;

#define GRID_LANE_MIN 15
#define GRID_LANE_MAX 40

#define GRID_TITLE_ROWS 2

/* the terminal is too narrow for lanes; run the list instead */
#define GRID_NARROW (-3)

enum grid_part {
    GRID_PART_TILE,
    GRID_PART_STATUS,
};

struct grid_rect {
    int lane, tile;
    int row, col, w, h;
    int status_row;
};

struct grid_layout {
    int lane_w, lanes_shown, lane_first, lanes;

    struct grid_rect *tile;
    int               tiles;

    int *lane_top;
    int *lane_hidden;
    int *lane_more_row;

    int rows_used;

    int *order;
    int *height;

    int tiles_cap, lanes_cap;
};

int boardgrid_layout(const struct board_tile *t, const int *lane_of, int n,
                     int lanes, int cols, int rows, int sel,
                     struct grid_layout *out);

void boardgrid_layout_free(struct grid_layout *g);

int boardgrid_hit(const struct grid_layout *g, int row, int col, int *part);

int boardgrid_run(const char *title, const struct board_tile *tiles,
                  const int *lane_of, const char *const *lane_name, int n,
                  int lanes, int initial, const char *hint, const char *ask,
                  const char *shortcuts, int *pressed, int (*tick)(void *ud),
                  void *tick_ud, int *cursor, int *part, struct menu *menu);

#endif
