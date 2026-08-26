#ifndef MENU_H
#define MENU_H

#define MENU_MAX   16
#define MENU_LABEL 40

/* a small box of choices drawn against the row it belongs to */
struct menu {
    char          item[MENU_MAX][MENU_LABEL];
    unsigned char apart[MENU_MAX];
    int           n;
    int           sel;
    int           open;
};

void menu_clear(struct menu *m);
int  menu_add(struct menu *m, const char *label, int apart);
void menu_step(struct menu *m, int dir);

int  menu_rows(const struct menu *m);
int  menu_width(const struct menu *m);

/* paints one row of the box, 0 to menu_rows() - 1, in exactly width cells */
void menu_paint_row(const struct menu *m, int row, int width);

#endif
