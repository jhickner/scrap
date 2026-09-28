#ifndef TERMINALRUN_H
#define TERMINALRUN_H

#include <stdio.h>
#include <stdlib.h>

#include "block.h"
#include "tty.h"
#include "ui.h"
#include "viewport.h"

static inline int terminal_run_external(const char *command)
{
    viewport_suspend();
    ui_raw(0);
    tty_raw_end();

    int status = system(command);

    if (tty_raw_begin() != 0) {
        fprintf(stderr, "could not return the terminal to raw mode\n");
        exit(1);
    }
    ui_raw(1);
    ui_term_colors();
    block_forget();
    viewport_resume();
    return status;
}

#endif
