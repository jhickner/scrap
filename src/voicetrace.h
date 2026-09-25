#ifndef VOICETRACE_H
#define VOICETRACE_H

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include "app.h"
#include "settings.h"

static inline void voice_trace(const char *event, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static inline void voice_trace(const char *event, const char *fmt, ...)
{
    const char *home = getenv("HOME");
    if (!home || !*home)
        return;
    char path[4096];
    int n = snprintf(path, sizeof path, "%s/" APP_CONFIG "/voice-events.log", home);
    if (n < 0 || (size_t)n >= sizeof path)
        return;

    if (!settings_get_int(SETTING_VOICE_TRACE, 0) && access(path, F_OK) != 0)
        return;
    va_list ap, copy;
    va_start(ap, fmt);
    va_copy(copy, ap);
    n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    char *detail = n >= 0 ? malloc((size_t)n + 1) : NULL;
    if (detail)
        vsnprintf(detail, (size_t)n + 1, fmt, ap);
    va_end(ap);
    if (!detail)
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "a") : NULL;
    if (f) {
        struct timespec wall, mono;
        clock_gettime(CLOCK_REALTIME, &wall);
        clock_gettime(CLOCK_MONOTONIC, &mono);
        flock(fd, LOCK_EX);
        fprintf(f, "%lld.%03ld mono=%lld.%03ld pid=%ld %s ",
                (long long)wall.tv_sec, wall.tv_nsec / 1000000,
                (long long)mono.tv_sec, mono.tv_nsec / 1000000, (long)getpid(), event);
        for (const unsigned char *p = (const unsigned char *)detail; *p; p++) {
            if (*p == '\n') fputs("\\n", f);
            else if (*p == '\r') fputs("\\r", f);
            else if (*p == '\t') fputs("\\t", f);
            else if (*p == '\\') fputs("\\\\", f);
            else if (*p < 32 || *p == 127) fprintf(f, "\\x%02x", *p);
            else fputc(*p, f);
        }
        fputc('\n', f);
        fclose(f);
    } else if (fd >= 0) {
        close(fd);
    }
    free(detail);
}

#endif
