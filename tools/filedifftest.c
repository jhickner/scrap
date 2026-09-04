#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "filediff.h"
#include "ui.h"

static int failures;

static void fail(const char *what, const char *out)
{
    fprintf(stderr, "FAIL %s\n---\n%s---\n", what, out ? out : "");
    failures++;
}

static char *render(const char *patch, int width, int *drew)
{
    ui_capture_begin(width);
    int d = filediff_render_patch(patch);
    char *out = ui_capture_end();
    if (drew)
        *drew = d;
    return out;
}

static void check_supplied(void)
{
    const char *patch =
        "@@file src/one.c\n"
        "@@ -1,2 +1,2 @@\n"
        " context\n"
        "-old\n"
        "+new\n"
        "@@file src/two.c\n"
        "@@ -3 +3 @@\n"
        "-before\n"
        "+after\n";

    int drew = 0;
    char *out = render(patch, 80, &drew);
    if (!drew || !out || !strstr(out, "src/one.c") || !strstr(out, "src/two.c") ||
        !strstr(out, "- old") || !strstr(out, "+ new") || !strstr(out, "- before") ||
        !strstr(out, "+ after"))
        fail("a supplied patch is rendered", out);
    free(out);
}

static void check_snapshot(void)
{
    struct filediff_snapshot snap = {0};
    char path[] = "/tmp/mux-filedifftest-XXXXXX";
    int  fd = mkstemp(path);
    if (fd < 0) {
        fail("a temp file to diff", NULL);
        return;
    }
    const char *before = "keep one\nkeep two\nthe line that changes, which is long "
                         "enough to need cutting short somewhere\nkeep three\n";
    (void)!write(fd, before, strlen(before));
    close(fd);

    filediff_snapshot(&snap, path);

    FILE *f = fopen(path, "w");
    if (!f) {
        fail("rewriting the file under the snapshot", NULL);
        unlink(path);
        return;
    }
    fputs("keep one\nkeep two\nthe line that replaced it, which is also long "
          "enough to need cutting short\nkeep three\n", f);
    fclose(f);

    char *patch = filediff_take_patch(&snap);
    if (!filediff_patch_draws(patch)) {
        fail("a changed file yields a patch", patch);
        free(patch);
        unlink(path);
        return;
    }

    unlink(path);

    char *wide = render(patch, 100, NULL);
    char *narrow = render(patch, 40, NULL);

    if (!wide || !strstr(wide, "- the line that changes") ||
        !strstr(wide, "+ the line that replaced it"))
        fail("the patch still has both sides of the change", wide);
    if (!narrow || !strstr(narrow, "\xe2\x80\xa6"))
        fail("a narrow pane cuts the diff short rather than overflowing it", narrow);
    if (wide && narrow && strlen(wide) == strlen(narrow))
        fail("the diff is laid out for the width it is drawn at", narrow);

    if (!wide || !strstr(wide, "keep two") || !strstr(wide, "keep three"))
        fail("the lines around the change are kept", wide);

    free(wide);
    free(narrow);
    free(patch);
}

static void check_unchanged(void)
{
    struct filediff_snapshot snap = {0};
    char path[] = "/tmp/mux-filedifftest-XXXXXX";
    int  fd = mkstemp(path);
    if (fd < 0) {
        fail("a temp file to diff", NULL);
        return;
    }
    (void)!write(fd, "same\n", 5);
    close(fd);

    filediff_snapshot(&snap, path);
    char *patch = filediff_take_patch(&snap);
    if (patch)
        fail("an unchanged file yields no patch", patch);
    free(patch);
    unlink(path);
}

static void check_interleaved(void)
{
    struct filediff_snapshot one = {0}, two = {0};
    char path_one[] = "/tmp/mux-filediff-one-XXXXXX";
    char path_two[] = "/tmp/mux-filediff-two-XXXXXX";
    int fd_one = mkstemp(path_one), fd_two = mkstemp(path_two);
    if (fd_one < 0 || fd_two < 0) {
        if (fd_one >= 0) close(fd_one);
        if (fd_two >= 0) close(fd_two);
        unlink(path_one);
        unlink(path_two);
        fail("two temp files to diff", NULL);
        return;
    }
    (void)!write(fd_one, "one before\n", 11);
    (void)!write(fd_two, "two before\n", 11);
    close(fd_one);
    close(fd_two);

    filediff_snapshot(&one, path_one);
    filediff_snapshot(&two, path_two);

    FILE *f = fopen(path_one, "w");
    if (f) {
        fputs("one after\n", f);
        fclose(f);
    }
    f = fopen(path_two, "w");
    if (f) {
        fputs("two after\n", f);
        fclose(f);
    }

    char *patch_one = filediff_take_patch(&one);
    char *patch_two = filediff_take_patch(&two);
    if (!patch_one || !strstr(patch_one, "one before") ||
        !strstr(patch_one, "one after") || strstr(patch_one, "two after"))
        fail("the first interleaved snapshot keeps its own file", patch_one);
    if (!patch_two || !strstr(patch_two, "two before") ||
        !strstr(patch_two, "two after") || strstr(patch_two, "one after"))
        fail("the second interleaved snapshot keeps its own file", patch_two);

    free(patch_one);
    free(patch_two);
    filediff_clear(&one);
    filediff_clear(&two);
    unlink(path_one);
    unlink(path_two);
}

int main(void)
{
    ui_init();

    check_supplied();
    check_snapshot();
    check_unchanged();
    check_interleaved();

    if (failures)
        return 1;
    puts("filedifftest: all checks passed");
    return 0;
}
