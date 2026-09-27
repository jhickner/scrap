#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include "filelock.h"

int main(void)
{
    char dir[] = "/tmp/scrap-filelock-XXXXXX";
    assert(mkdtemp(dir));

    char path[512];
    assert(snprintf(path, sizeof path, "%s/telegram", dir) < (int)sizeof path);
    int held = filelock_acquire(path, LOCK_EX | LOCK_NB);
    assert(held >= 0);

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(held);
        int competing = filelock_acquire(path, LOCK_EX | LOCK_NB);
        if (competing >= 0)
            filelock_release(competing);
        _exit(competing < 0 && (errno == EWOULDBLOCK || errno == EAGAIN) ? 0 : 1);
    }

    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    filelock_release(held);
    int next = filelock_acquire(path, LOCK_EX | LOCK_NB);
    assert(next >= 0);
    filelock_release(next);

    char lockpath[520];
    assert(snprintf(lockpath, sizeof lockpath, "%s.lock", path) < (int)sizeof lockpath);
    assert(unlink(lockpath) == 0);
    assert(rmdir(dir) == 0);
    puts("filelocktest: ok");
    return 0;
}
