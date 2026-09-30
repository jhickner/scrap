#include "instance.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app.h"
#include "handoff.h"
#include "intercom.h"
#include "livelist.h"
#include "session.h"
#include "tabs.h"
#include "text.h"
#include "workspace.h"

static int instance_path(const char *name, char *out, size_t size)
{
    char dir[4096];
    return intercom_name_valid(name) && path_config_subdir(dir, sizeof dir, "instances") &&
           (size_t)snprintf(out, size, "%s/%s", dir, name) < size;
}

int instance_save(const char *name)
{
    char path[4200], tmp[4300];
    if (!instance_path(name, path, sizeof path) ||
        (size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp)
        return -1;
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -1;
    int n = 0;
    for (int i = 0; i < workspace_count(); i++) {
        struct session *s = workspace_at(i);
        const char *id = session_id(s);
        if (!session_remote(s) && (!id || !*id || !session_can_resume(s)))
            continue;
        tabs_write(f, s, "");
        n++;
    }
    if (fclose(f) != 0 || !n || rename(tmp, path) != 0) {
        unlink(tmp);
        return n ? -1 : 0;
    }
    return n;
}

int instance_names(char *out, size_t size)
{
    char dir[4096];
    out[0] = '\0';
    if (!path_config_subdir(dir, sizeof dir, "instances"))
        return 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;
    int n = 0;
    size_t at = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (!intercom_name_valid(e->d_name))
            continue;
        int w = snprintf(out + at, size - at, "%s%s", n ? " " : "", e->d_name);
        if (w < 0 || (size_t)w >= size - at)
            break;
        at += (size_t)w;
        n++;
    }
    closedir(d);
    return n;
}

static void waiting(int waited_ms, void *ud)
{
    int *said = ud;
    if (waited_ms < 1000 || *said)
        return;
    *said = 1;
    fprintf(stderr, APP_NAME ": waiting for another window to release the session\n");
}

static const char *yank(const char *id, const struct live_session *live, int nlive,
                        char *screen, size_t size, char *held, size_t held_size)
{
    for (int i = 0; i < nlive; i++) {
        if (strcmp(live[i].id, id))
            continue;
        int said = 0;
        if (handoff_ask(live[i].pid, id, screen, size, waiting, &said))
            return screen;
        size_t at = strlen(held);
        snprintf(held + at, held_size - at, "%s@%s", at ? " " : "",
                 live[i].name[0] ? live[i].name : id);
        return NULL;
    }
    return "";
}

int instance_open(const char *name, char *front, size_t front_size, struct tab_args *t,
                  char *tabs, size_t tabs_size, char *held, size_t held_size)
{
    char path[4200];
    held[0] = '\0';
    tabs[0] = '\0';
    if (!instance_path(name, path, sizeof path))
        return 0;
    FILE *in = fopen(path, "r");
    if (!in)
        return 0;

    const char *tmp = getenv("TMPDIR");
    snprintf(tabs, tabs_size, "%s/" APP_NAME "-instance-%ld", tmp && *tmp ? tmp : "/tmp",
             (long)getpid());
    FILE *out = NULL;

    struct live_session *live = NULL;
    int nlive = livelist_load(&live);
    int have_front = 0;
    char line[6144];
    while (fgets(line, sizeof line, in)) {
        const char *args = strchr(line, '\t');
        if (!args)
            continue;
        char saved[6144];
        snprintf(saved, sizeof saved, "%s", args);
        struct tab_args a;
        if (!tabs_parse(line, &a))
            continue;
        char screen[4400];
        const char *got = a.remote ? "" : yank(a.id, live, nlive, screen, sizeof screen,
                                                held, held_size);
        if (!got)
            continue;
        if (!have_front) {
            snprintf(front, front_size, "%s%s", got, saved);
            have_front = tabs_parse(front, t);
            continue;
        }
        if (!out && !(out = fopen(tabs, "w")))
            break;
        fprintf(out, "%s%s", got, saved);
    }
    free(live);
    fclose(in);
    if (out)
        fclose(out);
    else
        tabs[0] = '\0';
    if (!have_front && tabs[0]) {
        unlink(tabs);
        tabs[0] = '\0';
    }
    return have_front;
}
