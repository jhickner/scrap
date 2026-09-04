#ifndef FILELOCK_H
#define FILELOCK_H

#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <unistd.h>

static inline int filelock_acquire(const char *path, int op)
{
    char lockpath[8192];
    int  n = snprintf(lockpath, sizeof lockpath, "%s.lock", path);
    if (n <= 0 || (size_t)n >= sizeof lockpath)
        return -1;

    int fd = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, op) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static inline void filelock_release(int fd)
{
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

#endif
