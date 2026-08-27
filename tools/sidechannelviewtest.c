#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sidechannelview.h"
#include "ui.h"

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static int lines(const char *text)
{
    int n = 0;
    for (; *text; text++)
        if (*text == '\n')
            n++;
    return n;
}

int main(void)
{
    ui_init();

    const char *question = "/btw why does this question need more than one row";
    int measured = sidechannel_question_paint(question, NULL, 24, 0, 1);
    if (measured != 3)
        fail("a pending question measures all of its wrapped rows");

    ui_capture_begin(24);
    int painted = sidechannel_question_paint(question, "* ", 24, 0, 0);
    char *out = ui_capture_end();
    if (!out) {
        fail("a pending question paints output");
    } else {
        if (painted != measured || lines(out) != measured)
            fail("a pending question paints every measured row");
        if (strstr(out, "…"))
            fail("a wrapped pending question is not clipped with an ellipsis");
        if (!strstr(out, "one row"))
            fail("the tail of a wrapped pending question is visible");
    }
    free(out);

    if (failures)
        return 1;
    fprintf(stderr, "sidechannelviewtest: all checks passed\n");
    return 0;
}
