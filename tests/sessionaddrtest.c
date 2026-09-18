#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sessionaddr.h"

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    static char buf[512];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

int main(void)
{
    char root[] = "/tmp/mux-sessionaddr-XXXXXX";
    assert(mkdtemp(root));
    setenv("MUX_ADDR_DIR", root, 1);

    char a[4300], b[4300];
    assert(sessionaddr_alloc(a, sizeof a));
    assert(sessionaddr_alloc(b, sizeof b));
    assert(strcmp(a, b)); /* one file per session */

    /* allocated empty, so a child that looks early reads nothing */
    struct stat st;
    assert(stat(a, &st) == 0);
    assert(st.st_size == 0);
    assert(!strcmp(slurp(a), ""));

    sessionaddr_write(a, "sess-one");
    assert(!strcmp(slurp(a), "sess-one\n"));
    assert(!strcmp(slurp(b), ""));

    /* the same id again leaves the file alone; a new one replaces it */
    sessionaddr_write(a, "sess-one");
    assert(!strcmp(slurp(a), "sess-one\n"));
    sessionaddr_write(a, "sess-two");
    assert(!strcmp(slurp(a), "sess-two\n"));

    /* nothing to say is not written */
    sessionaddr_write(b, NULL);
    sessionaddr_write(b, "");
    sessionaddr_write(NULL, "sess-three");
    assert(!strcmp(slurp(b), ""));

    /* no temporary file is left behind */
    char tmp[4400];
    snprintf(tmp, sizeof tmp, "%s.tmp", a);
    assert(stat(tmp, &st) != 0);

    sessionaddr_forget(a);
    assert(stat(a, &st) != 0);
    sessionaddr_forget(NULL);

    printf("sessionaddrtest: ok\n");
    return 0;
}
