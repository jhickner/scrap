#ifndef TABBAR_H
#define TABBAR_H

#define TABBAR_NONE -1

int  tabbar_stale(void);

void tabbar_cover(char **rows, int n, int cols);

int  tabbar_hit(int row, int col);

#endif
