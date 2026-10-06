#ifndef AGENTS_SPAWNFD_H
#define AGENTS_SPAWNFD_H

#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#endif

/* Close every descriptor above stderr in a freshly forked child. A pipe another
 * thread created between its pipe() and FD_CLOEXEC would otherwise stay open in
 * this child for its whole life, and the reader of that pipe never sees EOF. */
static inline void agents_close_inherited(void) {
#if defined(__APPLE__)
    struct proc_fdinfo fds[512];
    int bytes = proc_pidinfo(getpid(), PROC_PIDLISTFDS, 0, fds, sizeof fds);
    if (bytes > 0 && bytes < (int)sizeof fds) {
        for (int i = 0; i < bytes / (int)sizeof fds[0]; i++)
            if (fds[i].proc_fd > STDERR_FILENO) close(fds[i].proc_fd);
        return;
    }
#elif defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 3U, ~0U, 0) == 0) return;
#endif
    long max = sysconf(_SC_OPEN_MAX);
    if (max < 0 || max > 65536) max = 65536;
    for (int fd = STDERR_FILENO + 1; fd < max; fd++) close(fd);
}

#endif
