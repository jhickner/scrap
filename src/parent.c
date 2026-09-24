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
