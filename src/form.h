
#ifndef FORM_H
#define FORM_H

#include <stddef.h>

// A modal of fields rather than a modal of one line: tab walks them, typing
// edits the one in hand, enter keeps the lot and escape keeps none of it.
// ask.c is the single-field case of this; this is what a record wants.

enum form_kind {
    FORM_TEXT,      /* typed, with a caret */
    FORM_CHOICE,    /* one of a list, cycled with left and right */
};

struct form_field {
    const char    *label;
    enum form_kind kind;

    // The caller's buffer: read for what the field starts as, and written
    // only if the form is kept.
    char  *value;
    size_t size;

    const char *const *choices;   /* FORM_CHOICE */
    int                choices_n;
};

struct form {
    const char *title;

    // Read-only lines above the fields: what the record says that is not
    // being edited. Blank entries are drawn as blank rows.
    const char *const *notes;
    int                notes_n;

    struct form_field *fields;
    int                fields_n;
};

// Nonzero when the fields were kept, which is the only time they are written.
int form_run(struct form *f);

#endif
