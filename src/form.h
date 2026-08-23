
#ifndef FORM_H
#define FORM_H

#include <stddef.h>

enum form_kind {
    FORM_TEXT,
    FORM_CHOICE,
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

    const char *const *notes;
    int                notes_n;

    struct form_field *fields;
    int                fields_n;
};

int form_run(struct form *f);

#endif
