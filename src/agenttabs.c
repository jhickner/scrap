#include "agenttabs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "text.h"

#define MAX_SLOTS 32

struct slot {
    const void *key;
    char        agent[32];
    char        provider[32];
    char        status[16];
    int         usage_percent; /* -1 until a reading lands */
    long        usage_resets_at;
    long        usage_window_minutes;
    time_t      usage_updated_at;
};

static struct slot slots[MAX_SLOTS];
static char agents_dir[4200];
static char hook_dir[4200];

static int state_dir(char *out, size_t size)
{
    const char *env = getenv("AGENT_TABS_STATE_DIR");
    if (env && *env) {
        snprintf(out, size, "%s", env);
        return 1;
    }

    const char *home = getenv("HOME");
    if (!home || !*home)
        return 0;

    char base[4096], legacy[4096];
    struct stat st;
    snprintf(base, sizeof base, "%s/.config/tmux", home);
    snprintf(legacy, sizeof legacy, "%s/.tmux", home);
    if ((stat(base, &st) != 0 || !S_ISDIR(st.st_mode)) &&
        stat(legacy, &st) == 0 && S_ISDIR(st.st_mode))
        snprintf(base, sizeof base, "%s", legacy);

    snprintf(out, size, "%s/tmux-agent-tabs", base);
    return 1;
}

static int pane_id(const char *s)
{
    if (!s || *s != '%' || !s[1])
        return 0;
    for (const char *p = s + 1; *p; p++)
        if (*p < '0' || *p > '9')
            return 0;
    return 1;
}

static int agent_name(const char *backend)
{
    if (!backend || !*backend || strlen(backend) >= sizeof slots[0].agent)
        return 0;
    for (const char *p = backend; *p; p++)
        if ((*p < 'a' || *p > 'z') && (*p < '0' || *p > '9') && *p != '-' && *p != '_')
            return 0;
    return 1;
}

static int record_path(int at, char *out, size_t size)
{
    if (!agents_dir[0])
        return 0;
    return (size_t)snprintf(out, size, "%s/%ld-%d.json", agents_dir, (long)getpid(), at) < size;
}

static int write_json(FILE *f, void *ud)
{
    const struct slot *s = ud;

    const char *pane = getenv("TMUX_PANE");
    fprintf(f, "{\"agent\":\"%s\",\"pid\":%ld,\"status\":\"%s\",\"ts\":%ld",
            s->agent, (long)getpid(), s->status, (long)time(NULL));
    if (s->provider[0])
        fprintf(f, ",\"provider\":\"%s\"", s->provider);
    if (pane_id(pane))
        fprintf(f, ",\"tmux_pane\":\"%s\"", pane);
    if (s->usage_percent >= 0) {
        fprintf(f, ",\"usage_percent\":%d,\"usage_resets_at\":%ld"
                   ",\"usage_window_minutes\":%ld,\"usage_ts\":%ld",
                s->usage_percent, s->usage_resets_at, s->usage_window_minutes,
                (long)s->usage_updated_at);
    }
    fprintf(f, "}\n");
    return 1;
}

static void write_record(int at)
{
    char path[4400];
    if (!slots[at].key || !slots[at].status[0] || !record_path(at, path, sizeof path))
        return;
    text_spit(path, write_json, &slots[at]);
}

static int slot_of(const void *key, int make)
{
    int free_at = -1;
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (slots[i].key == key)
            return i;
        if (!slots[i].key && free_at < 0)
            free_at = i;
    }
    if (!make || free_at < 0)
        return -1;
    slots[free_at] = (struct slot){0};
    slots[free_at].key = key;
    slots[free_at].usage_percent = -1;
    return free_at;
}

static void drop_all(void)
{
    for (int i = 0; i < MAX_SLOTS; i++) {
        if (!slots[i].key)
            continue;
        char path[4400];
        if (record_path(i, path, sizeof path))
            unlink(path);
        slots[i].key = NULL;
    }
}

void agenttabs_begin(void)
{
    if (!getenv("TMUX_PANE"))
        return;

    char dir[4096];
    struct stat st;
    if (!state_dir(dir, sizeof dir) || stat(dir, &st) != 0)
        return;

    char agents[4200];
    if (snprintf(agents, sizeof agents, "%s/agents", dir) >= (int)sizeof agents)
        return;
    if (mkdir(agents, 0700) != 0 && errno != EEXIST)
        return;

    if (snprintf(hook_dir, sizeof hook_dir, "%s/state", dir) >= (int)sizeof hook_dir)
        return;
    snprintf(agents_dir, sizeof agents_dir, "%s", agents);

    setenv("AGENT_TABS_WRAPPED", "1", 1);

    atexit(drop_all);
}

void agenttabs_publish(const void *key, const char *backend, const char *status,
                       const char *provider)
{
    if (!agents_dir[0] || !key || !status || !*status || !agent_name(backend))
        return;

    int at = slot_of(key, 1);
    if (at < 0)
        return;

    /* A quota reading belongs to the backend that reported it: a session that
     * switches drops the old one rather than showing it under the new name. */
    if (strcmp(slots[at].agent, backend) != 0) {
        snprintf(slots[at].agent, sizeof slots[at].agent, "%s", backend);
        slots[at].usage_percent = -1;
        slots[at].usage_resets_at = 0;
        slots[at].usage_window_minutes = 0;
    }
    if (provider && *provider && agent_name(provider))
        snprintf(slots[at].provider, sizeof slots[at].provider, "%s", provider);
    else
        slots[at].provider[0] = '\0';
    snprintf(slots[at].status, sizeof slots[at].status, "%s", status);
    write_record(at);
}

void agenttabs_usage(const void *key, int percent, long resets_at, long window_minutes)
{
    if (!agents_dir[0] || percent < 0 || percent > 100)
        return;

    int at = slot_of(key, 0);
    if (at < 0)
        return;

    resets_at = resets_at > 0 ? resets_at : 0;
    window_minutes = window_minutes > 0 ? window_minutes : 0;
    if (percent == slots[at].usage_percent && resets_at == slots[at].usage_resets_at &&
        window_minutes == slots[at].usage_window_minutes)
        return;
    slots[at].usage_percent = percent;
    slots[at].usage_resets_at = resets_at;
    slots[at].usage_window_minutes = window_minutes;
    slots[at].usage_updated_at = time(NULL);
    write_record(at);
}

void agenttabs_forget(const void *key)
{
    int at = slot_of(key, 0);
    if (at < 0)
        return;
    char path[4400];
    if (record_path(at, path, sizeof path))
        unlink(path);
    slots[at].key = NULL;
}

void agenttabs_forget_hook(const char *id)
{
    if (!hook_dir[0] || !id || !*id || strchr(id, '/'))
        return;

    char path[4300];
    snprintf(path, sizeof path, "%s/%s.json", hook_dir, id);
    unlink(path);
}
