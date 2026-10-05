#include "memnote.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "vendor/agents/backend.h"
#include "vendor/cJSON.h"

#define NOTE_MODEL   "claude-sonnet-5-5"
#define MEM_CAP      (8u << 20)
#define MEM_WAIT     15
#define TAGS_MAX     300

extern char **environ;

char *memnote_capture(char *const argv[], int quiet, size_t cap, int timeout,
                      size_t *len, int *status)
{
    int fds[2];
    *len = 0;
    *status = -1;
    if (pipe(fds) != 0)
        return NULL;
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t acts;
    posix_spawn_file_actions_init(&acts);
    posix_spawn_file_actions_adddup2(&acts, fds[1], STDOUT_FILENO);
    if (quiet)
        posix_spawn_file_actions_addopen(&acts, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    else
        posix_spawn_file_actions_adddup2(&acts, fds[1], STDERR_FILENO);
    posix_spawn_file_actions_addopen(&acts, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid;
    int   spawned = posix_spawnp(&pid, argv[0], &acts, &attr, argv, environ) == 0;
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&acts);
    close(fds[1]);

    char  *buf = spawned ? malloc(cap + 1) : NULL;
    size_t got = 0;
    int    timed_out = 0;
    time_t until = time(NULL) + timeout;
    while (buf && got < cap) {
        struct pollfd p = {.fd = fds[0], .events = POLLIN};
        int           left = (int)(until - time(NULL));
        int           r = left > 0 ? poll(&p, 1, left * 1000) : 0;
        if (r < 0 && errno == EINTR)
            continue;
        if (r == 0) {
            kill(-pid, SIGKILL);
            timed_out = 1;
            break;
        }
        ssize_t k = read(fds[0], buf + got, cap - got);
        if (k < 0 && errno == EINTR)
            continue;
        if (k <= 0)
            break;
        got += (size_t)k;
    }
    close(fds[0]);
    int st = -1;
    if (spawned)
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
            ;
    if (!buf)
        return NULL;
    buf[got] = '\0';
    *len = got;
    *status = timed_out ? -1 : st;
    return buf;
}

static cJSON *mem_json(char **argv)
{
    size_t len;
    int    status;
    char  *out = memnote_capture(argv, 1, MEM_CAP, MEM_WAIT, &len, &status);
    cJSON *j = out && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? cJSON_Parse(out) : NULL;
    free(out);
    return j;
}

/* The prompt's context: each store with the tags already in use there. */
static char *store_context(cJSON *stores)
{
    size_t cap = 1 << 16, at = 0;
    char  *out = malloc(cap);
    if (!out)
        return NULL;
    out[0] = '\0';
    cJSON *s;
    cJSON_ArrayForEach(s, stores) {
        char  *argv[] = {"mem", "--json", "-s", s->valuestring, "tags", NULL};
        cJSON *tags = mem_json(argv);
        at += (size_t)snprintf(out + at, cap - at, "store %s, tags in use:", s->valuestring);
        cJSON *t;
        int    n = 0;
        cJSON_ArrayForEach(t, cJSON_GetObjectItem(tags, "tags")) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(t, "name"));
            if (name && cJSON_GetNumberValue(cJSON_GetObjectItem(t, "count")) > 0 && n++ < TAGS_MAX && at < cap - 200)
                at += (size_t)snprintf(out + at, cap - at, " %s", name);
        }
        if (at < cap - 2)
            at += (size_t)snprintf(out + at, cap - at, "\n");
        cJSON_Delete(tags);
    }
    return out;
}

static cJSON *ask_llm(const char *text, const char *context)
{
    size_t len = strlen(text) + strlen(context) + 2048;
    char  *prompt = malloc(len);
    if (!prompt)
        return NULL;
    snprintf(prompt, len,
             "A note dictated by voice, to save in a tagged memory store.\n\n"
             "%s\n"
             "Reply with only a JSON object: {\"store\": ..., \"title\": ..., \"text\": ..., \"tags\": [...]}.\n"
             "- store: personal unless the note names another store from the list.\n"
             "- text: the note with transcription errors, filler words and false starts fixed; "
             "keep its meaning and wording otherwise. Drop any spoken instructions about the store or tags.\n"
             "- title: 3 to 8 words, sentence case, no final period.\n"
             "- tags: 1 to 4 lowercase tags; prefer tags already in use in that store. "
             "Add todo when the note is something to do.\n\n"
             "Note:\n%s\n",
             context, text);
    backend_opts o = {0};
    o.name = "claude";
    o.model = NOTE_MODEL;
    o.system = "Answer with the JSON object alone, without using tools.";
    o.session_name = APP_NAME " note helper";
    o.ephemeral = 1;
    o.disable_tools = 1;
    Backend *b = backend_open_ex(&o);
    char    *answer = b ? b->ask(b, prompt) : NULL;
    free(prompt);
    if (b)
        b->close(b);
    char  *open = answer ? strchr(answer, '{') : NULL;
    char  *close = answer ? strrchr(answer, '}') : NULL;
    cJSON *j = NULL;
    if (open && close > open) {
        close[1] = '\0';
        j = cJSON_Parse(open);
    }
    free(answer);
    return j;
}

static int in_list(cJSON *list, const char *s)
{
    cJSON *i;
    cJSON_ArrayForEach(i, list) if (cJSON_IsString(i) && !strcmp(i->valuestring, s)) return 1;
    return 0;
}

int memnote_main(int argc, char **argv)
{
    const char *text = argc > 1 ? argv[1] : NULL;
    if (!text || !*text) {
        fprintf(stderr, "usage: " APP_NAME " note TEXT\n");
        return 2;
    }
    char  *sargv[] = {"mem", "--json", "stores", NULL};
    cJSON *sj = mem_json(sargv);
    cJSON *stores = cJSON_GetObjectItem(sj, "stores");
    char  *context = store_context(stores);
    cJSON *note = context ? ask_llm(text, context) : NULL;
    free(context);

    const char *store = cJSON_GetStringValue(cJSON_GetObjectItem(note, "store"));
    const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(note, "title"));
    const char *body = cJSON_GetStringValue(cJSON_GetObjectItem(note, "text"));
    cJSON      *tags = cJSON_GetObjectItem(note, "tags");
    if (!store || !in_list(stores, store))
        store = "personal";
    if (!body || !*body) {
        body = text;
        title = NULL;
    }

    int    n = cJSON_GetArraySize(tags);
    char **cargv = calloc((size_t)(2 * n + 12), sizeof *cargv);
    int    a = 0;
    cargv[a++] = "mem";
    cargv[a++] = "--json";
    cargv[a++] = "-s";
    cargv[a++] = (char *)store;
    cargv[a++] = "create";
    if (title && *title) {
        cargv[a++] = "--title";
        cargv[a++] = (char *)title;
    }
    cargv[a++] = "--text";
    cargv[a++] = (char *)body;
    for (int i = 0; i < n; i++) {
        cJSON *t = cJSON_GetArrayItem(tags, i);
        if (cJSON_IsString(t) && *t->valuestring) {
            cargv[a++] = "--tag";
            cargv[a++] = t->valuestring;
        }
    }
    size_t len;
    int    status;
    char  *out = memnote_capture(cargv, 0, MEM_CAP, MEM_WAIT, &len, &status);
    int    ok = out && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    cJSON *rec = ok ? cJSON_Parse(out) : NULL;
    int    saved = rec != NULL;
    if (saved) {
        cJSON_AddStringToObject(rec, "store", store);
        char *s = cJSON_PrintUnformatted(rec);
        if (s)
            puts(s);
        free(s);
    } else
        fprintf(stdout, "%s", out ? out : "could not run mem");
    cJSON_Delete(rec);
    free(out);
    free(cargv);
    cJSON_Delete(note);
    cJSON_Delete(sj);
    return saved ? 0 : 1;
}
