#ifndef BOARDDEFAULTS_H
#define BOARDDEFAULTS_H

struct board_default {
    const char *path;
    const char *text;
};

extern const struct board_default board_defaults[];
extern const int                  board_defaults_n;

#endif
