#include "handoff.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "livelist.h"
#include "text.h"

#define POLL_MS   30

static int dir_path(char *out, size_t size)
{
    return path_config_subdir(out, size, "handoff");
}

static int id_ok(const char *id)
{
    if (!id || !*id || strlen(id) > 120)
        return 0;
    for (const char *p = id; *p; p++)
        if (*p == '/' || *p == '.' || *p < 0x20)
            return 0;
    return 1;
}

static int leaf(const char *name, const char *ext, char *out, size_t size)
{
    char dir[4200];
    if (!dir_path(dir, sizeof dir))
        return 0;
    return (size_t)snprintf(out, size, "%s/%s%s", dir, name, ext) < size;
}

static int request_path(long pid, char *out, size_t size)
{
    char name[32];
    snprintf(name, sizeof name, "%ld", pid);
    return leaf(name, ".req", out, size);
}

int handoff_screen_path(const char *id, char *out, size_t size)
{
    return id_ok(id) && leaf(id, ".state.tmp", out, size);
}

static int state_path(const char *id, char *out, size_t size)
{
    return id_ok(id) && leaf(id, ".state", out, size);
}

static int refused_path(const char *id, char *out, size_t size)
{
    return id_ok(id) && leaf(id, ".no", out, size);
}

static int cancel_path(const char *id, char *out, size_t size)
{
    return id_ok(id) && leaf(id, ".cancel", out, size);
}

static int asker_path(const char *id, char *out, size_t size)
{
    return id_ok(id) && leaf(id, ".asker", out, size);
}

int handoff_wanted(void)
{
    char path[4400];
    struct stat st;
    return request_path((long)getpid(), path, sizeof path) && stat(path, &st) == 0;
}

int handoff_take_request(char *id, size_t size, long *asker)
{
    *asker = 0;
    char path[4400];
    if (!request_path((long)getpid(), path, sizeof path))
        return 0;

    size_t len = 0;
    char *text = text_slurp(path, 4096, &len);
    unlink(path);
    if (!text)
        return 0;
    text_chomp(text);
    snprintf(id, size, "%s", text);
    free(text);
    if (!id_ok(id))
        return 0;
    char who[4400];
    if (asker_path(id, who, sizeof who)) {
        char *pid = text_slurp(who, 64, &len);
        if (pid)
            *asker = strtol(pid, NULL, 10);
        free(pid);
        unlink(who);
    }
    return 1;
}

void handoff_refuse(const char *id)
{
    char path[4400];
    if (!refused_path(id, path, sizeof path))
        return;
    FILE *f = fopen(path, "w");
    if (f)
        fclose(f);
}

int handoff_withdrawn(const char *id, long asker)
{
    char path[4400];
    struct stat st;
    if (cancel_path(id, path, sizeof path) && stat(path, &st) == 0)
        return 1;
    return asker > 0 && !livelist_alive(asker);
}

void handoff_forget(const char *id)
{
    char path[4400];
    if (cancel_path(id, path, sizeof path))
        unlink(path);
}

int handoff_publish(const char *id)
{
    char tmp[4400], path[4400];
    if (!handoff_screen_path(id, tmp, sizeof tmp) || !state_path(id, path, sizeof path))
        return 0;

    struct stat st;
    if (stat(tmp, &st) != 0) {
        FILE *f = fopen(tmp, "w");
        if (!f)
            return 0;
        fclose(f);
    }
    return rename(tmp, path) == 0;
}

static void nap(void)
{
    struct timespec ts = {0, POLL_MS * 1000000L};
    nanosleep(&ts, NULL);
}

static int write_id(FILE *f, void *ud)
{
    return fprintf(f, "%s\n", (const char *)ud) > 0;
}

static int write_pid(FILE *f, void *ud)
{
    (void)ud;
    return fprintf(f, "%ld\n", (long)getpid()) > 0;
}

int handoff_ask(long pid, const char *id, char *screen, size_t size,
                int (*tick)(int waited_ms, void *ud), void *ud)
{
    char req[4400], state[4400], no[4400], cancel[4400];
    if (!id_ok(id) || !request_path(pid, req, sizeof req) ||
        !state_path(id, state, sizeof state) || !refused_path(id, no, sizeof no) ||
        !cancel_path(id, cancel, sizeof cancel))
        return 0;
    unlink(state);
    unlink(no);
    unlink(cancel);

    char who[4400];
    if (asker_path(id, who, sizeof who))
        text_spit(who, write_pid, NULL);
    if (!text_spit(req, write_id, (void *)id))
        return 0;

    if (kill((pid_t)pid, SIGURG) != 0) {
        unlink(req);
        return 0;
    }

    struct stat st;
    for (int waited = 0;; waited += POLL_MS) {
        if (stat(state, &st) == 0) {
            snprintf(screen, size, "%s", state);
            return 1;
        }
        if (stat(no, &st) == 0) {
            unlink(no);
            return 0;
        }
        if (!livelist_alive(pid))
            break;
        if (tick && tick(waited, ud)) {
            /* The giver may be holding the request until the session's turn
             * ends; the marker tells it to keep the session after all. */
            FILE *f = fopen(cancel, "w");
            if (f)
                fclose(f);
            break;
        }
        nap();
    }

    unlink(req);

    if (stat(state, &st) == 0) {
        unlink(cancel);
        snprintf(screen, size, "%s", state);
        return 1;
    }
    return 0;
}
