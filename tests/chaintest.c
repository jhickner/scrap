#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chain.h"

static int failures;

static void expect(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

static int ids(const char *chain, const char *want)
{
    struct chain_segment *seg = NULL;
    int                   n = chain_before(chain, NULL, &seg);
    char                  got[512] = "";
    for (int k = 0; k < n; k++) {
        strcat(got, k ? " " : "");
        strcat(got, seg[k].id);
    }
    free(seg);
    if (strcmp(got, want))
        fprintf(stderr, "  got '%s', want '%s'\n", got, want);
    return !strcmp(got, want);
}

int main(void)
{
    char dir[] = "/tmp/chaintest.XXXXXX", a[CHAIN_ID_MAX], b[CHAIN_ID_MAX],
         c[CHAIN_ID_MAX];
    if (!mkdtemp(dir))
        return 1;
    setenv("SCRAP_CONFIG_DIR", dir, 1);

    chain_new(a, sizeof a);
    chain_set_name(a, "early");
    expect(!chain_named("early", c, sizeof c), "naming an unstored chain stores nothing");
    chain_add(a, "proj", "claude", "/w", "s1");
    chain_add(a, "proj", "claude", "/w", "s2");
    chain_add(a, "proj", "claude", "/w", "s2");
    chain_add(a, "proj", "codex", "/w dir", "s3");
    expect(ids(a, "s1 s2 s3"), "add appends once per id");

    struct chain_record r;
    expect(chain_read(a, &r) && r.n == 3 && !strcmp(r.name, "proj") &&
               !strcmp(r.seg[2].backend, "codex") && !strcmp(r.seg[2].cwd, "/w dir"),
           "record keeps name, backend and cwd");
    chain_free(&r);

    chain_set_name(a, "renamed");
    expect(chain_named("renamed", c, sizeof c) && !strcmp(c, a), "rename is one write");
    expect(!chain_named("proj", c, sizeof c), "old name is free");

    chain_new(b, sizeof b);
    chain_copy(a, "s3", b, "fork");
    chain_add(b, "fork", "claude", "/w", "f1");
    expect(ids(b, "s1 s2 f1"), "copy stops before the fork source");

    expect(chain_find("s3", c, sizeof c) && !strcmp(c, a), "find by last segment");
    expect(chain_find("f1", c, sizeof c) && !strcmp(c, b), "find a fork");
    expect(chain_find("s1", c, sizeof c) && !strcmp(c, a), "shared segment finds the original");
    expect(!chain_find("nope", c, sizeof c), "unknown id");

    struct chain_segment *seg = NULL;
    expect(chain_before(a, "s3", &seg) == 2 && !strcmp(seg[1].id, "s2"),
           "before lists earlier segments");
    free(seg);
    expect(chain_before(a, "nope", &seg) == 0, "before unknown id");
    free(seg);

    chain_add(a, "renamed", "claude", "/w", "s4");
    chain_cut(a, "s4");
    expect(ids(a, "s4"), "cut hides earlier segments");
    expect(chain_before(a, "s4", &seg) == 0, "nothing before the cut");
    free(seg);
    expect(chain_find("s2", c, sizeof c) && (!strcmp(c, a) || !strcmp(c, b)),
           "hidden segments stay findable");
    chain_cut(a, NULL);
    chain_add(a, "renamed", "claude", "/w", "s5");
    expect(ids(a, "s5"), "cut without id starts at the next segment");

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    system(cmd);
    if (failures)
        return 1;
    printf("chaintest ok\n");
    return 0;
}
