#include <stdio.h>
#include <string.h>

#include "session.h"
#include "sessionfork.h"
#include "sidechannelcmd.h"

struct session {
    const char *id;
    int can_resume;
};

const char *sessionfork_program(void) { return "scrap"; }
const char *session_backend(const struct session *s) { (void)s; return "codex"; }
const char *session_cwd(const struct session *s) { (void)s; return "/work"; }
const char *session_id(const struct session *s) { return s->id; }
int session_can_resume(const struct session *s) { return s->can_resume; }
const char *session_model(const struct session *s) { (void)s; return "default"; }
const char *session_effort(const struct session *s) { (void)s; return "medium"; }

static int has(char **argv, const char *value)
{
    for (int i = 0; argv[i]; i++)
        if (!strcmp(argv[i], value))
            return 1;
    return 0;
}

int main(void)
{
    char *argv[24];
    struct session fresh = {0};
    sidechannel_argv(&fresh, "answer this", argv, 24);
    if (has(argv, "--session") || has(argv, "--fork")) {
        fprintf(stderr, "sidechannelcmdtest: fresh turn tried to fork\n");
        return 1;
    }

    struct session resumed = {.id = "thread-1", .can_resume = 1};
    sidechannel_argv(&resumed, "answer this", argv, 24);
    if (!has(argv, "thread-1") || !has(argv, "--fork")) {
        fprintf(stderr, "sidechannelcmdtest: resumable turn did not fork\n");
        return 1;
    }

    int p = 0;
    while (argv[p] && strcmp(argv[p], "-p"))
        p++;
    if (!argv[p] || !argv[p + 1] || strcmp(argv[p + 1], "answer this") || argv[p + 2]) {
        fprintf(stderr, "sidechannelcmdtest: prompt is not passed with -p\n");
        return 1;
    }

    printf("sidechannelcmdtest: ok\n");
    return 0;
}
