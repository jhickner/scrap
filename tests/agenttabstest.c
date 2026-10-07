#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agenttabs.h"
#include "vendor/cJSON.h"

static int has_provider(const char *path, const char *provider)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    cJSON *row = cJSON_Parse(buf);
    const char *got = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "provider"));
    int ok = row && (provider ? got && !strcmp(got, provider) : !got);
    cJSON_Delete(row);
    return ok;
}

int main(void)
{
    char root[] = "/tmp/scrap-tabs-XXXXXX";
    if (!mkdtemp(root)) {
        perror("agenttabstest: mkdtemp");
        return 1;
    }
    setenv("AGENT_TABS_STATE_DIR", root, 1);
    agenttabs_begin();

    int one = 1;
    char agents[512], first[640];
    snprintf(agents, sizeof agents, "%s/agents", root);
    snprintf(first, sizeof first, "%s/%ld-0.json", agents, (long)getpid());

    agenttabs_publish(&one, "pi", "working", "openrouter");
    if (!has_provider(first, "openrouter")) {
        fputs("agenttabstest: openrouter provider was not recorded\n", stderr);
        return 1;
    }
    agenttabs_publish(&one, "pi", "finished", NULL);
    if (!has_provider(first, NULL)) {
        fputs("agenttabstest: dropping the route left provider on the record\n", stderr);
        return 1;
    }

    unlink(first);
    rmdir(agents);
    rmdir(root);
    puts("agenttabstest: ok");
    return 0;
}
