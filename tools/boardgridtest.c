#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boardgrid.h"
#include "restart.h"
#include "sidechannel.h"

int  sidechannel_rows(void) { return 0; }
void sidechannel_paint(int budget) { (void)budget; }
void restart_shield_thread(void) {}

static int failures;

static void expect(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

#define TILES_MAX 64

static struct board_tile tiles[TILES_MAX];
static int               lane_of[TILES_MAX];
static int               tiles_n;

static void tile(int lane, const char *title, const char *spec,
                 const char *status)
{
    if (tiles_n >= TILES_MAX)
        return;
    struct board_tile *t = &tiles[tiles_n];
    memset(t, 0, sizeof *t);
    snprintf(t->title, sizeof t->title, "%s", title);
    snprintf(t->spec, sizeof t->spec, "%s", spec);
    snprintf(t->status, sizeof t->status, "%s", status);
    lane_of[tiles_n++] = lane;
}

static void reset(void)
{
    tiles_n = 0;
}

static void fill(int lanes, int per_lane)
{
    reset();
    for (int l = 0; l < lanes; l++)
        for (int i = 0; i < per_lane; i++) {
            char title[64];
            snprintf(title, sizeof title, "card %d of lane %d", i, l);
            tile(l, title, "a spec that says what the card is for", "tab 2 · 4m");
        }
}

static void test_lane_widths(void)
{
    struct grid_layout g = {0};
    fill(9, 2);

    struct {
        int cols, shown, lane_w;
    } want[] = {
        {60, 2, 29},
        {80, 3, 26},
        {120, 4, 29},
        {200, 8, 24},
    };

    for (int i = 0; i < (int)(sizeof want / sizeof *want); i++) {
        g.lane_first = 0;
        expect(boardgrid_layout(tiles, lane_of, tiles_n, 9, want[i].cols, 20, 0,
                                &g), "a board wider than a lane lays out");
        expect(g.lanes_shown == want[i].shown, "lanes shown fit the width");
        expect(g.lane_w == want[i].lane_w, "lane width shares the width");
        expect(1 + g.lanes_shown * g.lane_w <= want[i].cols,
               "the lanes fit inside the screen");
    }

    fill(2, 1);
    g.lane_first = 0;
    expect(boardgrid_layout(tiles, lane_of, tiles_n, 2, 200, 20, 0, &g),
           "two lanes lay out");
    expect(g.lane_w == GRID_LANE_MAX, "a lane is no wider than the maximum");

    fill(3, 1);
    g.lane_first = 0;
    expect(!boardgrid_layout(tiles, lane_of, tiles_n, 3, 20, 20, 0, &g),
           "a screen narrower than a lane has no layout");

    boardgrid_layout_free(&g);
}

static void test_the_window_follows_the_selection(void)
{
    struct grid_layout g = {0};
    fill(9, 2);

    expect(boardgrid_layout(tiles, lane_of, tiles_n, 9, 80, 20, 0, &g),
           "the first lane lays out");
    expect(g.lane_first == 0, "the window starts at the first lane");

    int last = tiles_n - 1;
    expect(boardgrid_layout(tiles, lane_of, tiles_n, 9, 80, 20, last, &g),
           "the last lane lays out");
    expect(g.lane_first == lane_of[last] - g.lanes_shown + 1,
           "the window ends on the selected lane");

    expect(boardgrid_layout(tiles, lane_of, tiles_n, 9, 80, 20, 0, &g),
           "the first lane lays out again");
    expect(g.lane_first == 0, "the window comes back to the selection");

    int found = 0;
    for (int i = 0; i < g.tiles; i++)
        found |= g.tile[i].tile == 0;
    expect(found, "the selected tile is placed");

    boardgrid_layout_free(&g);
}

static void test_a_lane_scrolls_on_its_own(void)
{
    struct grid_layout g = {0};
    fill(2, 12);

    int last = tiles_n - 1;
    expect(boardgrid_layout(tiles, lane_of, tiles_n, 2, 80, 12, last, &g),
           "a crowded lane lays out");
    expect(g.lane_top[1] > 0, "the selected lane scrolls to the selection");
    expect(g.lane_top[0] == 0, "the lane beside it stays where it was");
    expect(g.lane_hidden[1] > 0, "the cards that did not fit are counted");
    expect(g.rows_used <= 12, "the rows drawn fit the height");
    expect(g.lane_more_row[1] < 12 &&
               g.lane_more_row[1] < g.rows_used,
           "the count of what is hidden has a row of its own");

    int shown = 0;
    for (int i = 0; i < g.tiles; i++)
        shown |= g.tile[i].tile == last;
    expect(shown, "the selected card is on the screen");

    expect(boardgrid_layout(tiles, lane_of, tiles_n, 2, 80, 12, 0, &g),
           "the top of the lane lays out");
    expect(g.lane_top[0] == 0, "the selection at the top does not scroll");

    boardgrid_layout_free(&g);
}

static void test_tiles_keep_to_their_lane(void)
{
    struct grid_layout g = {0};
    fill(9, 3);

    expect(boardgrid_layout(tiles, lane_of, tiles_n, 9, 120, 24, 0, &g),
           "the board lays out");
    expect(g.tiles > 0, "tiles are placed");
    expect(g.rows_used > 0 && g.rows_used <= 24, "the rows drawn fit the height");

    for (int i = 0; i < g.tiles; i++) {
        const struct grid_rect *r = &g.tile[i];
        int                     li = r->lane - g.lane_first;
        expect(li >= 0 && li < g.lanes_shown, "a placed lane is on the screen");
        expect(r->col == 1 + li * g.lane_w, "a tile starts at its lane");
        expect(r->w == g.lane_w - 1, "a tile leaves a gutter beside it");
        expect(r->row >= 0 && r->row + r->h <= 24, "a tile fits the rows");
        expect(r->lane == lane_of[r->tile], "a tile is drawn in its own lane");
        expect(r->status_row == r->row + r->h - 1,
               "the status row is the last row of the tile");

        for (int j = 0; j < i; j++) {
            const struct grid_rect *o = &g.tile[j];
            if (o->lane != r->lane)
                continue;
            expect(o->row + o->h < r->row || r->row + r->h < o->row,
                   "two tiles in a lane do not overlap");
        }
    }

    boardgrid_layout_free(&g);
}

static void test_every_corner_hits_its_tile(void)
{
    struct grid_layout g = {0};
    fill(9, 3);

    expect(boardgrid_layout(tiles, lane_of, tiles_n, 9, 120, 24, 0, &g),
           "the board lays out for the hit map");

    for (int i = 0; i < g.tiles; i++) {
        const struct grid_rect *r = &g.tile[i];
        int rows[] = {r->row, r->row + r->h - 1};
        int cols[] = {r->col, r->col + r->w - 1};

        for (int a = 0; a < 2; a++)
            for (int b = 0; b < 2; b++) {
                int part = -1;
                int at = boardgrid_hit(&g, rows[a], cols[b], &part);
                expect(at == r->tile, "a corner hits the tile it belongs to");
                expect(part == (rows[a] == r->status_row ? GRID_PART_STATUS
                                                         : GRID_PART_TILE),
                       "the status row is told apart from the rest");
            }

        int part = -1;
        expect(boardgrid_hit(&g, r->row, r->col - 1, &part) != r->tile,
               "the cell left of a tile is not the tile");
        expect(boardgrid_hit(&g, r->row + r->h, r->col, &part) != r->tile,
               "the row under a tile is not the tile");
    }

    expect(boardgrid_hit(&g, 0, 0, NULL) < 0, "the margin hits nothing");

    boardgrid_layout_free(&g);
}

static void test_a_tile_taller_than_the_lane(void)
{
    struct grid_layout g = {0};
    reset();
    tile(0, "a title long enough to wrap over more than one row of a lane",
         "a spec long enough to wrap over more than one row of a narrow lane",
         "tab 1 · 2m");

    expect(boardgrid_layout(tiles, lane_of, tiles_n, 1, 80, 3, 0, &g),
           "a tile taller than the lane lays out");
    expect(g.tiles == 1, "the selected tile is still placed");
    expect(g.tile[0].h <= 3, "the tile is clipped to the rows it has");

    boardgrid_layout_free(&g);
}

int main(void)
{
    test_lane_widths();
    test_the_window_follows_the_selection();
    test_a_lane_scrolls_on_its_own();
    test_tiles_keep_to_their_lane();
    test_every_corner_hits_its_tile();
    test_a_tile_taller_than_the_lane();

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
