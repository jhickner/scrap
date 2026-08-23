#include "frontend.h"

#define FRONTEND_MAX 8

static unsigned stack[FRONTEND_MAX];
static int      depth;

void frontend_push(unsigned caps)
{
    if (depth < FRONTEND_MAX)
        stack[depth] = caps;
    depth++;
}

void frontend_pop(void)
{
    if (depth > 0)
        depth--;
}

unsigned frontend_caps(void)
{
    if (depth == 0)
        return FRONTEND_KEYBOARD;

    int at = depth < FRONTEND_MAX ? depth : FRONTEND_MAX;
    return stack[at - 1];
}

int frontend_has_keyboard(void)
{
    return (frontend_caps() & FRONTEND_KEYBOARD) != 0;
}
