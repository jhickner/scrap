#include "parent.h"

#include <string.h>

#include "kvlog.h"
#include "text.h"

int parent_of(const char *child_id, char *out, size_t size)
{
    char path[1200];
    if (!child_id || !*child_id || !path_config_file(path, sizeof path, "parent"))
        return 0;
    return kvlog_lookup(path, child_id, out, size);
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
    (void)kvlog_append(path, child_id, parent_id);
    return 1;
}
