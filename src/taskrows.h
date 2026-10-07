#ifndef TASKROWS_H
#define TASKROWS_H

int  taskrows_count(int cols);
void taskrows_paint(int cols);
int  taskrows_click(int row);
int  taskrows_items(void);
void taskrows_focus(int i);
void taskrows_toggle(int i);
int  taskrows_stop(int i);
int  taskrows_stale(void);

#endif
