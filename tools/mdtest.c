
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md.h"
#include "ui.h"

static int failures;

static void fail(const char *what, int width, size_t cells, const char *row, size_t len)
{
    fprintf(stderr, "FAIL %s: %d wide, row of %zu cells: [%.*s]\n", what, width, cells,
            (int)len, row);
    failures++;
}

static void check_fits(const char *what, const char *src, int width)
{
    ui_capture_begin(width);
    md_render(src, 0);
    char *out = ui_capture_end();
    if (!out)
        return;
    for (char *p = out; *p;) {
        char  *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        size_t cells = ui_cells_visible(p, len);
        if (cells > (size_t)width)
            fail(what, width, cells, p, len);
        if (!nl)
            break;
        p = nl + 1;
    }
    free(out);
}

static void check_widths(const char *what, const char *src)
{
    for (int w = 20; w <= 120; w++)
        check_fits(what, src, w);
}

static void check_plain(const char *what, const char *src, const char *want)
{
    struct md_text *t = md_text_parse(src);
    size_t          len = 0;
    const char     *got = md_text_plain(t, &len);
    if (strlen(want) != len || memcmp(got, want, len)) {
        fprintf(stderr, "FAIL %s: [%.*s] wanted [%s]\n", what, (int)len, got, want);
        failures++;
    }
    md_text_free(t);
}

int main(void)
{
    setenv("COLUMNS", "80", 1);
    setenv("LINES", "24", 1);
    ui_init();

    check_widths("a paragraph",
                 "The generator already exists (tools/gen-testimages.sh) and has been run — "
                 "29 files are in testimages/ now, one per case.\n");

    check_widths("a bulleted list",
                 "- PNG fast path (8): basic, wide, tall, strip-2000x40, strip-40x2000, "
                 "large-3000x2000, tiny-16, pixel-1x1\n"
                 "- Conversion path / non-PNG (8): photo.jpg, bmp, tiff, webp, ico, "
                 "anim.gif, doc.pdf, vector.svg\n");

    check_widths("a numbered list",
                 "1. Conversion path / non-PNG (8): photo.jpg, bmp, tiff, webp, ico, "
                 "anim.gif, doc.pdf, vector.svg\n"
                 "10. Failure paths (7): broken.png, empty.png, nonimage.png, noext, "
                 "truncated.jpg, oversize-20mb.png\n");

    check_widths("a quote",
                 "> Conversion path / non-PNG (8): photo.jpg, bmp, tiff, webp, ico, "
                 "anim.gif, doc.pdf, vector.svg\n");

    check_widths("a nested list",
                 "- Conversion path / non-PNG (8): photo.jpg, bmp, tiff, webp, ico\n"
                 "  - anim.gif, doc.pdf, vector.svg, and whatever else turns up here\n");

    check_widths("styled text",
                 "- **Conversion path** / *non-PNG* (8): `photo.jpg`, [bmp](https://x.test/b), "
                 "tiff, webp, ico, anim.gif\n");

    check_plain("inline marks", "12:34  worker **done** with `x` and *y*",
                "12:34  worker done with x and y");
    check_plain("a heading", "## Summary\nit landed", "Summary\nit landed");
    check_plain("a bullet", "- one\n- two", "\xe2\x80\xa2 one\n\xe2\x80\xa2 two");
    check_plain("a fence", "```c\nint x;\n```", "int x;");

    {
        char big[9000];
        memset(big, 'x', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        check_plain("a long message", big, big);
    }

    if (failures)
        return 1;
    fprintf(stderr, "mdtest: all checks passed\n");
    return 0;
}
