#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "askblock.h"

int main(void)
{
    assert(!askblock_parse("no block here"));
    assert(!askblock_parse("@asking\n1. q"));
    assert(!askblock_parse("text\n@ask\n"));

    struct askblock *b = askblock_parse(
        "Some text.\n\n@ask\n1. Which db?\n- postgres \xe2\x80\x94 reliable\n- sqlite\n"
        "2) Name for it?\n\n3. Ship now?\n- yes - today\n- no\n");
    assert(b && b->n == 3);
    assert(!strcmp(b->q[0].text, "Which db?"));
    assert(b->q[0].nopt == 2);
    assert(!strcmp(b->q[0].label[0], "postgres"));
    assert(!strcmp(b->q[0].detail[0], "reliable"));
    assert(!strcmp(b->q[0].label[1], "sqlite") && !b->q[0].detail[1]);
    assert(b->q[1].nopt == 0);
    assert(!strcmp(b->q[2].label[0], "yes") && !strcmp(b->q[2].detail[0], "today"));

    int         choice[] = {1, -1, 0};
    const char *text[] = {"", "scrapdb", "after tests"};
    char       *out = askblock_answer(b, choice, text, NULL);
    assert(!strcmp(out, "1. sqlite\n2. scrapdb\n3. yes \xe2\x80\x94 after tests"));
    free(out);

    int         none[] = {-1, -1, -1};
    const char *blank[] = {"", "", ""};
    out = askblock_answer(b, none, blank, "just do it");
    assert(!strcmp(out, "just do it"));
    free(out);
    assert(!askblock_answer(b, none, blank, ""));
    askblock_free(b);

    b = askblock_parse("@ask\n1. a\nend of block\n2. b\n");
    assert(b && b->n == 1);
    askblock_free(b);
    return 0;
}
