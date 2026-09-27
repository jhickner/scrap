#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "activelog.h"

static int open_all(const char *id, void *ud)
{
    const char *closed = ud;
    return !closed || strcmp(id, closed);
}

static void step(const char *path, const char *here, int dir, const char *closed,
                 const char *want)
{
    char got[64] = "";
    int ok = activelog_step(path, here, dir, open_all, (void *)closed, got, sizeof got);
    if (!want) {
        assert(!ok);
        return;
    }
    assert(ok && !strcmp(got, want));
    activelog_add(path, got, 0);
}

int main(void)
{
    char path[] = "/tmp/mux-activelog-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);

    step(path, "a", -1, NULL, NULL);

    const char *seq[] = {"a", "b", "a", "c", "c", "d"};
    for (size_t i = 0; i < sizeof seq / sizeof *seq; i++)
        activelog_add(path, seq[i], 0);

    step(path, "d", -1, NULL, "c");
    step(path, "c", -1, NULL, "a");
    step(path, "a", -1, NULL, "b");
    step(path, "b", -1, NULL, NULL);
    step(path, "b", 1, NULL, "a");
    step(path, "a", 1, NULL, "c");
    step(path, "c", -1, NULL, "a");
    step(path, "a", 1, NULL, "c");
    step(path, "c", 1, NULL, "d");
    step(path, "d", 1, NULL, NULL);

    step(path, "d", -1, NULL, "c");
    activelog_add(path, "e", 0);
    step(path, "e", 1, NULL, NULL);
    step(path, "e", -1, "b", "c");
    step(path, "c", -1, "b", "d");
    step(path, "d", -1, "b", "a");
    step(path, "a", -1, "b", NULL);

    unlink(path);
    puts("activelogtest ok");
    return 0;
}
