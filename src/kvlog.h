#ifndef KVLOG_H
#define KVLOG_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Append-only, tab-separated key/value records. The last matching record wins. */
static inline int kvlog_lookup(const char *path, const char *key, char *out,
                               size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    char  *line = NULL;
    size_t cap = 0;
    size_t key_len = strlen(key);
    int    found = 0;
    while (getline(&line, &cap, f) > 0) {
        if (strncmp(line, key, key_len) != 0 || line[key_len] != '\t')
            continue;
        char *value = line + key_len + 1;
        value[strcspn(value, "\n")] = '\0';
        if (*value) {
            snprintf(out, size, "%s", value);
            found = 1;
        }
    }
    free(line);
    fclose(f);
    return found;
}

static inline int kvlog_append(const char *path, const char *key,
                               const char *value)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return 0;

    size_t size = strlen(key) + strlen(value) + 3;
    char  *row = malloc(size);
    if (!row) {
        close(fd);
        return 0;
    }
    int n = snprintf(row, size, "%s\t%s\n", key, value);
    size_t left = n > 0 && (size_t)n < size ? (size_t)n : 0;
    const char *p = row;
    while (left) {
        ssize_t written = write(fd, p, left);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (written == 0)
            break;
        p += written;
        left -= (size_t)written;
    }
    free(row);
    close(fd);
    return left == 0 && n > 0;
}

#endif
