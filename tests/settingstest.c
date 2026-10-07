#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "settings.h"
#include "text.h"

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

int main(void)
{
    char path[] = "/tmp/scrap-settingstest-XXXXXX";
    int  fd = mkstemp(path);
    if (fd < 0)
        return 1;
    close(fd);

    struct settings *a = calloc(1, sizeof *a), *b = calloc(1, sizeof *b);
    struct settings *c = calloc(1, sizeof *c);
    if (!a || !b || !c)
        return 1;

    settings_load(a, path);
    settings_put(a, "voice", "1");
    settings_put(a, "model", "one");

    settings_load(b, path);
    settings_put(a, "voice", "0");
    settings_put(b, "model", "two");

    settings_load(c, path);
    if (strcmp(settings_get(c, "voice", ""), "0") != 0)
        fail("a change written by another copy survives a stale writer");
    if (strcmp(settings_get(c, "model", ""), "two") != 0)
        fail("the stale writer's own change lands");
    if (strcmp(settings_get(b, "voice", ""), "0") != 0)
        fail("a writer takes up the entries already on disk");

    unlink(path);
    char lock[300];
    snprintf(lock, sizeof lock, "%s.lock", path);
    unlink(lock);
    free(a);
    free(b);
    free(c);

    char root[] = "/tmp/scrap-configdir-XXXXXX";
    if (!mkdtemp(root))
        return 1;
    char want[128], got[4096];
    snprintf(want, sizeof want, "%s/a/b", root);
    setenv("SCRAP_CONFIG_DIR", want, 1);
    if (!path_config_dir(got, sizeof got) || strcmp(got, want) != 0)
        fail("SCRAP_CONFIG_DIR replaces the config directory");
    if (access(want, F_OK) != 0)
        fail("SCRAP_CONFIG_DIR is created with its parents");
    rmdir(want);
    snprintf(want, sizeof want, "%s/a", root);
    rmdir(want);
    rmdir(root);

    if (failures)
        return 1;
    puts("settingstest: all checks passed");
    return 0;
}
