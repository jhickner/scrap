#include "sideroute.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vendor/cJSON.h"

#define SIDEROUTE_THRESHOLD 0.8
#define SIDEROUTE_TIMEOUT_MS 1500

static const char QUESTIONS[] =
    "{\"side\":{\"type\":\"noul\",\"instructions\":\"An agent is working on "
    "running_turn. The user typed queued while it works. Answer yes only if "
    "queued can run right now in a separate copy of the agent: it does not "
    "refer to, depend on, or follow up the running turn's work or result, and "
    "it does not edit the project's files. Recording a todo, note, or reminder "
    "(for example any mem command) is always yes. Unrelated questions are yes. "
    "Anything ambiguous is no.\",\"criteria\":{\"true\":\"Independent: safe to "
    "run now in parallel\",\"false\":\"Depends on the running turn, edits "
    "project files, or unclear\"}}}";

static long ms_since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

int sideroute_independent(const char *running, const char *queued)
{
    if (!running || !queued)
        return 0;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "running_turn", running);
    cJSON_AddStringToObject(o, "queued", queued);
    char *state = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!state)
        return 0;

    int fd[2];
    if (pipe(fd) != 0) {
        free(state);
        return 0;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(fd[0]);
        dup2(fd[1], STDOUT_FILENO);
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execlp("jev", "jev", "eval", "-q", QUESTIONS, "--state", state, "--field",
               "answers.side.noul", (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    free(state);
    if (pid < 0) {
        close(fd[0]);
        return 0;
    }

    char   buf[64];
    size_t n = 0;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        long left = SIDEROUTE_TIMEOUT_MS - ms_since(&t0);
        struct pollfd p = {fd[0], POLLIN, 0};
        if (left <= 0 || poll(&p, 1, (int)left) <= 0)
            break;
        ssize_t r = read(fd[0], buf + n, sizeof buf - 1 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
        if (n == sizeof buf - 1)
            break;
    }
    buf[n] = 0;
    close(fd[0]);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return n && atof(buf) >= SIDEROUTE_THRESHOLD;
}
