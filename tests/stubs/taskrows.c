#include "taskrows.h"

int  taskrows_count(int cols) { (void)cols; return 0; }
void taskrows_paint(int cols) { (void)cols; }
int  taskrows_click(int row) { (void)row; return 0; }
int  taskrows_stale(void) { return 0; }
int  taskrows_items(void) { return 0; }
void taskrows_focus(int i) { (void)i; }
void taskrows_toggle(int i) { (void)i; }
int  taskrows_stop(int i) { (void)i; return 0; }
