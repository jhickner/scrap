#ifndef MDCFG_H
#define MDCFG_H

#include <stddef.h>

// A settings file a person edits: frontmatter between --- fences, then the
// body, which for a prompt is the prompt. Markdown because these are written
// by hand and read in an editor. The app's own state stays JSON, which nobody
// has to look at.

#define MDCFG_KEYS 32
#define MDCFG_NAME 64

struct mdcfg {
    char *keys[MDCFG_KEYS];
    char *values[MDCFG_KEYS];
    int   n;
    char *body;     /* never NULL once loaded */
    char *text;     /* the file itself; everything above points into it */
};

// Zero when there is no such file. The struct is safe to free either way.
int  mdcfg_load(struct mdcfg *m, const char *path);
void mdcfg_free(struct mdcfg *m);

const char *mdcfg_get(const struct mdcfg *m, const char *key);  /* "" when absent */
int         mdcfg_int(const struct mdcfg *m, const char *key, int fallback);

// Writing one back. `keys` and `values` are parallel; a NULL or empty body
// leaves the file as frontmatter alone.
int mdcfg_write(const char *path, const char *const *keys,
                const char *const *values, int n, const char *body);

// Every .md in a directory, named without the suffix, in order. Returns how
// many were found.
int mdcfg_list(const char *dir, char names[][MDCFG_NAME], int max);

// A directory under the config folder, made if it is not there. `leaf` may
// have slashes in it.
int mdcfg_dir(char *out, size_t size, const char *leaf);

#endif
