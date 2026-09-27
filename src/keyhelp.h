#ifndef KEYHELP_H
#define KEYHELP_H

struct keyhelp_row {
    const char *group;
    const char *key;
    const char *brief;
};

extern const char KEYHELP_FOOT_ALL[];
extern const char KEYHELP_FOOT_F1[];

void keyhelp_show(const char *title, const struct keyhelp_row *rows, int n,
                  const char *foot);

int keyhelp_layout(const struct keyhelp_row *rows, int n, int width,
                   int *split, int *colw);

#endif
