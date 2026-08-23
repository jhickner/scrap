#ifndef BLOCK_H
#define BLOCK_H

#include <stddef.h>

void block_begin(void);
void block_end(int caret_row, int caret_col);

int  block_have(void);
void block_row_begin(int row);
void block_row_end(void);

void block_clear(void);
void block_keep(int rows);

void block_forget(void);
void block_cleared(void);

#endif
