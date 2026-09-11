#ifndef ORCHDATA_H
#define ORCHDATA_H

struct orch_file {
    const char *path;
    const char *text;
};

extern const struct orch_file orch_files[];
extern const int              orch_files_n;

#endif
