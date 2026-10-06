#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "vendor/agents/spawnfd.h"

int main(void)
{
    int p[2];
    if (pipe(p) != 0)
        return 1;
    pid_t pid = fork();
    if (pid == 0) {
        agents_close_inherited();
        _exit(fcntl(p[0], F_GETFD) == -1 && fcntl(p[1], F_GETFD) == -1 &&
                      fcntl(STDERR_FILENO, F_GETFD) != -1
                  ? 0
                  : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fputs("spawnfdtest: child kept inherited descriptors\n", stderr);
        return 1;
    }
    puts("spawnfdtest: ok");
    return 0;
}
