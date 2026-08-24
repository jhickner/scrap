
#ifndef FORM_H
#define FORM_H

#include <stddef.h>

#include "ui.h"

enum form_kind {
    FORM_TEXT,
    FORM_CHOICE,
    FORM_BUTTON,
};

struct form_field {
    const char    *label;
    enum form_kind kind;

    char  *value;
    size_t size;

    const char *const *choices;
    int                choices_n;
};

struct form {
    const char *title;

    const char *const  *notes;
    const enum ui_role *note_roles; /* one per note; NULL is UI_DIM throughout */
    int                 notes_n;

    struct form_field *fields;
    int                fields_n;
};

int form_run(struct form *f);

#endif
