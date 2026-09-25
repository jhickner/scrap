#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "agenttabs.h"
#include "vendor/cJSON.h"

static cJSON *load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    return cJSON_Parse(buf);
}

static int check_record(const char *path, const char *backend, const char *status,
                        int usage_percent, const char *provider)
{
    cJSON *row = load(path);
    const char *agent = row ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(row, "agent")) : NULL;
    const char *actual = row ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(row, "status")) : NULL;
    const char *pane = row ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(row, "tmux_pane")) : NULL;
    const char *got = row ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(row, "provider")) : NULL;
    cJSON *ts = row ? cJSON_GetObjectItemCaseSensitive(row, "ts") : NULL;
    cJSON *pct = row ? cJSON_GetObjectItemCaseSensitive(row, "usage_percent") : NULL;
    cJSON *reset = row ? cJSON_GetObjectItemCaseSensitive(row, "usage_resets_at") : NULL;
    cJSON *window = row ? cJSON_GetObjectItemCaseSensitive(row, "usage_window_minutes") : NULL;
    cJSON *uts = row ? cJSON_GetObjectItemCaseSensitive(row, "usage_ts") : NULL;
    int ok = agent && !strcmp(agent, backend) && actual && !strcmp(actual, status) &&
             pane && !strcmp(pane, "%42") && cJSON_IsNumber(ts) && ts->valuedouble > 0;
    if (provider && *provider)
        ok = ok && got && !strcmp(got, provider);
    else
        ok = ok && !got;
    if (usage_percent >= 0)
        ok = ok && cJSON_IsNumber(pct) && pct->valueint == usage_percent &&
             cJSON_IsNumber(reset) && reset->valuedouble == 2000000000.0 &&
             cJSON_IsNumber(window) && window->valueint == 10080 &&
             cJSON_IsNumber(uts) && uts->valuedouble > 0;
    else
        ok = ok && !pct && !reset && !window && !uts;
    cJSON_Delete(row);
    return ok;
}

int main(void)
{
    char root[] = "/tmp/mux-tabs-XXXXXX";
    if (!mkdtemp(root)) {
        perror("agenttabstest: mkdtemp");
        return 1;
    }
    setenv("AGENT_TABS_STATE_DIR", root, 1);
    setenv("TMUX_PANE", "%42", 1);
    agenttabs_begin();

    int one = 1, two = 2;

    char agents[512], first[640], second[640];
    snprintf(agents, sizeof agents, "%s/agents", root);
    snprintf(first, sizeof first, "%s/%ld-0.json", agents, (long)getpid());
    snprintf(second, sizeof second, "%s/%ld-1.json", agents, (long)getpid());

    agenttabs_publish(&one, "claude", "finished", NULL);
    if (!check_record(first, "claude", "finished", -1, NULL)) {
        fputs("agenttabstest: initial provider record is invalid\n", stderr);
        return 1;
    }

    agenttabs_publish(&two, "codex", "working", NULL);
    agenttabs_usage(&two, 37, 2000000000L, 10080);
    if (!check_record(second, "codex", "working", 37, NULL)) {
        fputs("agenttabstest: background session record is invalid\n", stderr);
        return 1;
    }
    if (!check_record(first, "claude", "finished", -1, NULL)) {
        fputs("agenttabstest: a second session overwrote the first\n", stderr);
        return 1;
    }

    agenttabs_publish(&two, "codex", "finished", NULL);
    if (!check_record(second, "codex", "finished", 37, NULL)) {
        fputs("agenttabstest: status rewrite discarded quota\n", stderr);
        return 1;
    }

    agenttabs_publish(&two, "claude", "working", NULL);
    if (!check_record(second, "claude", "working", -1, NULL)) {
        fputs("agenttabstest: switching backends kept the old quota\n", stderr);
        return 1;
    }

    agenttabs_forget(&one);
    if (load(first)) {
        fputs("agenttabstest: a closed session left its record behind\n", stderr);
        return 1;
    }
    if (!check_record(second, "claude", "working", -1, NULL)) {
        fputs("agenttabstest: closing one session dropped another's record\n", stderr);
        return 1;
    }

    int three = 3;
    agenttabs_publish(&three, "grok", "working", NULL);
    if (!check_record(first, "grok", "working", -1, NULL)) {
        fputs("agenttabstest: a new session did not reuse the freed slot\n", stderr);
        return 1;
    }

    agenttabs_publish(&three, "pi", "working", "openrouter");
    if (!check_record(first, "pi", "working", -1, "openrouter")) {
        fputs("agenttabstest: openrouter provider was not recorded\n", stderr);
        return 1;
    }
    agenttabs_publish(&three, "pi", "finished", NULL);
    if (!check_record(first, "pi", "finished", -1, NULL)) {
        fputs("agenttabstest: dropping the route left provider on the record\n", stderr);
        return 1;
    }

    unlink(first);
    unlink(second);
    rmdir(agents);
    rmdir(root);
    puts("agenttabstest: ok");
    return 0;
}
