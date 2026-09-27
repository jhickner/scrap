
#ifndef PICK_H
#define PICK_H

struct pick_item {
    const char *label;
    const char *detail;
};

int pick_run(const char *title, const struct pick_item *items, int count, int initial);

int pick_run_filter(const char *title, const struct pick_item *items, int count, int initial);

#define PICK_TYPED_MAX 1024
#define PICK_TYPED     (-3)

int pick_run_typed(const char *title, const struct pick_item *items, int count, int initial,
                   char *typed);

enum pick_search {
    PICK_SEARCH_TYPE,
    PICK_SEARCH_SLASH,
};

#define PICK_KEY_RIGHT '\x1c'

#define PICK_TICK_REOPEN 2
#define PICK_REOPEN      (-2)

#define PICK_POLL_MS 500

#define PICK_HEADING 1
#define PICK_APART   2
#define PICK_TEXT    3

struct keyhelp_row;

struct pick_live {
    const unsigned char *heading;
    int                  group_gap;

    const unsigned char *spin;
    const char *const   *mark;
    const unsigned char *mark_role;

    const char *const *lead;
    const char *const *tail;

    const char *hint;

    const struct keyhelp_row *keys;
    int                       nkeys;

    const char *ask;

    int align;

    int stack;

    int  (*tick)(void *ud);
    void  *ud;

    int *cursor;

    int keep;
};

int pick_run_live(const char *title, const struct pick_item *items, int count,
                  int initial, const struct pick_live *live,
                  enum pick_search search, const char *shortcuts, int *pressed);

#endif
