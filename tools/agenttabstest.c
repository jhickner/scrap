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
                        int usage_percent)
{
    cJSON *row = load(path);
    const char *agent = row ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(row, "agent")) : NULL;
    const char *actual = row ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(row, "status")) : NULL;
    const char *pane = row ? cJSON_GetStringValue(
        cJSON_GetObjectItemCaseSensitive(row, "tmux_pane")) : NULL;
    cJSON *ts = row ? cJSON_GetObjectItemCaseSensitive(row, "ts") : NULL;
    cJSON *pct = row ? cJSON_GetObjectItemCaseSensitive(row, "usage_percent") : NULL;
    cJSON *reset = row ? cJSON_GetObjectItemCaseSensitive(row, "usage_resets_at") : NULL;
    cJSON *window = row ? cJSON_GetObjectItemCaseSensitive(row, "usage_window_minutes") : NULL;
    cJSON *uts = row ? cJSON_GetObjectItemCaseSensitive(row, "usage_ts") : NULL;
    int ok = agent && !strcmp(agent, backend) && actual && !strcmp(actual, status) &&
             pane && !strcmp(pane, "%42") && cJSON_IsNumber(ts) && ts->valuedouble > 0;
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

    /* Two sessions in one process, as tabs in one window are. */
    int one = 1, two = 2;

    char agents[512], first[640], second[640];
    snprintf(agents, sizeof agents, "%s/agents", root);
    snprintf(first, sizeof first, "%s/%ld-0.json", agents, (long)getpid());
    snprintf(second, sizeof second, "%s/%ld-1.json", agents, (long)getpid());

    agenttabs_publish(&one, "claude", "finished");
    if (!check_record(first, "claude", "finished", -1)) {
        fputs("agenttabstest: initial provider record is invalid\n", stderr);
        return 1;
    }

    agenttabs_publish(&two, "codex", "working");
    agenttabs_usage(&two, 37, 2000000000L, 10080);
    if (!check_record(second, "codex", "working", 37)) {
        fputs("agenttabstest: background session record is invalid\n", stderr);
        return 1;
    }
    if (!check_record(first, "claude", "finished", -1)) {
        fputs("agenttabstest: a second session overwrote the first\n", stderr);
        return 1;
    }

    agenttabs_publish(&two, "codex", "finished");
    if (!check_record(second, "codex", "finished", 37)) {
        fputs("agenttabstest: status rewrite discarded quota\n", stderr);
        return 1;
    }

    /* A switched backend takes the record's name with it, and leaves the
     * previous provider's quota behind. */
    agenttabs_publish(&two, "claude", "working");
    if (!check_record(second, "claude", "working", -1)) {
        fputs("agenttabstest: switching backends kept the old quota\n", stderr);
        return 1;
    }

    agenttabs_forget(&one);
    if (load(first)) {
        fputs("agenttabstest: a closed session left its record behind\n", stderr);
        return 1;
    }
    if (!check_record(second, "claude", "working", -1)) {
        fputs("agenttabstest: closing one session dropped another's record\n", stderr);
        return 1;
    }

    /* The freed slot is reused rather than growing the table. */
    int three = 3;
    agenttabs_publish(&three, "grok", "working");
    if (!check_record(first, "grok", "working", -1)) {
        fputs("agenttabstest: a new session did not reuse the freed slot\n", stderr);
        return 1;
    }

    unlink(first);
    unlink(second);
    rmdir(agents);
    rmdir(root);
    puts("agenttabstest: ok");
    return 0;
}
