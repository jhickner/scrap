#ifndef IMAGEVIEW_H
#define IMAGEVIEW_H

int imageview_open(int at);

int imageview_open_paths(const char *const *paths, int n, int at);

int imageview_click(int row, int col);

#endif
