
#include <stdio.h>
#include <stdlib.h>

#include "image.h"

static int failures;

static void fail(const char *what, int img_w, int img_h, int cols, int rows)
{
    fprintf(stderr, "FAIL %s (%dx%d px -> %dx%d cells)\n", what, img_w, img_h, cols, rows);
    failures++;
}

static int skew(int img_w, int img_h, int cols, int rows, int cw, int ch)
{
    double want = (double)img_w / img_h;
    double got = (double)(cols * cw) / (rows * ch);
    double off = got > want ? got / want : want / got;
    return (int)((off - 1.0) * 100.0 + 0.5);
}

static void check(const char *what, int img_w, int img_h, int cw, int ch,
                  int cols_box, int rows_box, int max_skew)
{
    int cols = 0, rows = 0;
    image_fit(img_w, img_h, cw, ch, cols_box, rows_box, &cols, &rows);

    if (cols < 1 || rows < 1)
        fail("the box has both dimensions", img_w, img_h, cols, rows);
    if (cols > cols_box || rows > rows_box)
        fail("the box fits in the room available", img_w, img_h, cols, rows);

    int natural_cols = (img_w + cw - 1) / cw;
    int natural_rows = (img_h + ch - 1) / ch;
    if (cols > natural_cols || rows > natural_rows)
        fail("the image is not scaled up past its own size", img_w, img_h, cols, rows);

    int off = skew(img_w, img_h, cols, rows, cw, ch);
    if (off > max_skew) {
        fprintf(stderr, "FAIL %s: %d%% off its own shape (%dx%d px -> %dx%d cells)\n",
                what, off, img_w, img_h, cols, rows);
        failures++;
    }
}

int main(void)
{
    const int CW = 8, CH = 16;

    check("a wide screenshot", 1600, 900, CW, CH, 80, 20, 5);
    check("a tall screenshot", 900, 1600, CW, CH, 80, 20, 5);
    check("a square image", 1000, 1000, CW, CH, 80, 20, 5);
    check("a very wide banner", 2000, 200, CW, CH, 80, 20, 8);

    int cols = 0, rows = 0;
    image_fit(160, 160, CW, CH, 80, 20, &cols, &rows);
    if (cols != 20 || rows != 10)
        fail("a small image is drawn at its own size", 160, 160, cols, rows);

    image_fit(80, 32, CW, CH, 80, 20, &cols, &rows);
    if (cols != 10 || rows != 2)
        fail("a small wide image is drawn at its own size", 80, 32, cols, rows);

    check("a screenshot in a narrow pane", 1600, 900, CW, CH, 20, 20, 8);

    check("a screenshot with square cells", 1600, 900, 10, 10, 80, 20, 5);

    image_fit(0, 0, CW, CH, 80, 20, &cols, &rows);
    if (cols < 1 || rows < 1 || cols > 80 || rows > 20)
        fail("an unknown image gets a usable box", 0, 0, cols, rows);
    if (cols * CW > rows * CH * 3)
        fail("an unknown image is not given a letterbox shape", 0, 0, cols, rows);

    image_fit(1600, 900, CW, CH, 4, 2, &cols, &rows);
    if (cols < 1 || rows < 1 || cols > 4 || rows > 2)
        fail("a tiny pane still gets a box", 1600, 900, cols, rows);

    if (failures)
        return 1;
    fprintf(stderr, "imagefittest: all checks passed\n");
    return 0;
}
