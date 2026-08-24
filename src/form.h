
#ifndef FORM_H
#define FORM_H

#include <stddef.h>

#include "ui.h"

/* the most fields form_run will take */
#define FORM_FIELDS 12

enum form_kind {
    FORM_TEXT,
    FORM_CHOICE,
    FORM_BUTTON,
    FORM_TOGGLE, /* flips between choices[0] and choices[1], form stays up */
};

struct form_field {
    const char    *label;
    enum form_kind kind;

    char  *value;
    size_t size;

    const char *const *choices;
    int                choices_n;

    int rows_max; /* FORM_TEXT: rows drawn while the form is collapsed */
};

struct form {
    const char *title;

    const char *const  *notes;
    const char *const  *note_labels; /* one per note; NULL leaves the column bare */
    const enum ui_role *note_roles;  /* one per note; NULL is UI_DIM throughout */
    int                 notes_n;

    int notes_from; /* the first note of the block that collapses */
    int notes_max;  /* rows of that block drawn while the form is collapsed */

    struct form_field *fields;
    int                fields_n;
};

int form_run(struct form *f);

#endif
