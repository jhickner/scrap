/* The tab bar reaches the whole workspace; a chrome test links this instead. */

#include "tabbar.h"

int  tabbar_rows(int cols) { (void)cols; return 0; }
int  tabbar_stale(void) { return 0; }
void tabbar_paint(int cols) { (void)cols; }
int  tabbar_hit(int col) { (void)col; return -1; }
