
#ifndef FRONTEND_H
#define FRONTEND_H

enum {
    FRONTEND_KEYBOARD = 1u << 0,
};

void     frontend_push(unsigned caps);
void     frontend_pop(void);
unsigned frontend_caps(void);
int      frontend_has_keyboard(void);

#endif
