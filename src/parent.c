#include "parent.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "text.h"

int parent_of(const char *child_id, char *out, size_t size)
{
    char path[1200];
    if (!child_id || !*child_id || !path_config_file(path, sizeof path, "parent"))
        return 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    char  *line = NULL;
    size_t cap = 0;
    size_t id_len = strlen(child_id);
    int    found = 0;

    while (getline(&line, &cap, f) > 0) {
        if (strncmp(line, child_id, id_len) != 0 || line[id_len] != '\t')
            continue;
        char *text = line + id_len + 1;
        text[strcspn(text, "\n")] = '\0';
        if (*text) {
            snprintf(out, size, "%s", text);
            found = 1;
        }
    }
    free(line);
    fclose(f);
    return found;
}

int parent_set(const char *child_id, const char *parent_id)
{
    if (!child_id || !*child_id || !parent_id || !*parent_id)
        return 0;
    if (!strcmp(child_id, parent_id))
        return 0;

    char path[1200];
    if (!path_config_file(path, sizeof path, "parent"))
        return 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return 0;

    char row[320];
    int  n = snprintf(row, sizeof row, "%s\t%s\n", child_id, parent_id);
    if (n > 0 && (size_t)n < sizeof row) {
        const char *p = row;
        size_t      left = (size_t)n;
        while (left) {
            ssize_t w = write(fd, p, left);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            p += w;
            left -= (size_t)w;
        }
    }
    close(fd);
    return 1;
}
