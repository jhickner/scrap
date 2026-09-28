#include "tabbar.h"

int  tabbar_stale(void) { return 0; }
void tabbar_cover(char **rows, int n, int cols) { (void)rows; (void)n; (void)cols; }
int  tabbar_hit(int row, int col) { (void)row; (void)col; return TABBAR_NONE; }
