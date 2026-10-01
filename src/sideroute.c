#include "sideroute.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "vendor/cJSON.h"

#define SIDEROUTE_SIDE_MIN 0.8
#define SIDEROUTE_REDIRECT_MIN 0.85
#define SIDEROUTE_TIMEOUT_MS 1500

static const char QUESTIONS[] =
    "{\"route\":{\"type\":\"choice\",\"instructions\":\"An agent is working on "
    "running_turn. The user typed queued while it works. Decide what to do with "
    "queued. side: it can run right now in a separate copy of the agent because "
    "it does not refer to, depend on, or follow up the running turn, and does "
    "not edit the project's files; recording a todo, note, or reminder (for "
    "example any mem command) is always side, even when its subject is the "
    "same project or topic as the running turn; unrelated questions are side. "
    "redirect: it "
    "corrects, cancels, or changes the instructions of the running turn, so the "
    "work in progress is wrong and should stop now (for example 'no, I "
    "meant...', 'stop', 'use the other folder instead', 'don't do that'), "
    "or it asks the agent to pause or to do something else before continuing "
    "(for example 'wait', 'hold on', 'wait, commit first', 'run the tests "
    "first'). "
    "other: anything else, including follow-ups meant to run after the running "
    "turn finishes, and anything ambiguous.\",\"criteria\":{\"other\":\"Run "
    "after the running turn finishes\",\"side\":\"Independent: run now in "
    "parallel\",\"redirect\":\"Corrects or changes the running turn: interrupt "
    "it and send this instead\"}}}";

static long ms_since(const struct timespec *t0)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0->tv_sec) * 1000 + (t.tv_nsec - t0->tv_nsec) / 1000000;
}

static double probability(const cJSON *p, const char *key)
{
    const cJSON *v = cJSON_GetObjectItem(p, key);
    return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

static enum sideroute parse(const char *out)
{
    cJSON *o = cJSON_Parse(out);
    cJSON *route = cJSON_GetObjectItem(cJSON_GetObjectItem(o, "answers"), "route");
    cJSON *p = cJSON_GetObjectItem(route, "probabilities");
    enum sideroute r = SIDEROUTE_QUEUE;
    if (probability(p, "redirect") >= SIDEROUTE_REDIRECT_MIN)
        r = SIDEROUTE_REDIRECT;
    else if (probability(p, "side") >= SIDEROUTE_SIDE_MIN)
        r = SIDEROUTE_SIDE;
    cJSON_Delete(o);
    return r;
}

enum sideroute sideroute_classify(const char *running, const char *queued)
{
    if (!running || !queued)
        return SIDEROUTE_QUEUE;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "running_turn", running);
    cJSON_AddStringToObject(o, "queued", queued);
    char *state = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!state)
        return SIDEROUTE_QUEUE;

    int fd[2];
    if (pipe(fd) != 0) {
        free(state);
        return SIDEROUTE_QUEUE;
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
        execlp("jev", "jev", "eval", "-q", QUESTIONS, "--state", state, (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    free(state);
    if (pid < 0) {
        close(fd[0]);
        return SIDEROUTE_QUEUE;
    }

    char   buf[4096];
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
    return parse(buf);
}
