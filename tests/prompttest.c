#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chrome.h"
#include "prompt.h"
#include "sidechannel.h"
#include "tty.h"
#include "ui.h"

#include "restart.h"
void restart_shield_thread(void) {}

int  sidechannel_rows(void) { return 0; }
void sidechannel_paint(int budget) { (void)budget; }
void sidechannel_tick(void) {}
void sidechannel_poll(void) {}
int  sidechannel_busy(void) { return 0; }
void sidechannel_close_all(void) {}
int  sidechannel_fds(int *out, int max) { (void)out; (void)max; return 0; }

static int failures;

static void check_drawn_cursor(struct prompt *p)
{
    int rows = prompt_input_rows(p, 80);
    int caret_row = 0, caret_col = 0;
    ui_capture_begin(80);
    prompt_paint_input(p, rows, &caret_row, &caret_col);
    char *painted = ui_capture_end();
    if (!painted || !strstr(painted, "\x1b[7m") || !strstr(painted, "\x1b[27m")) {
        fprintf(stderr, "FAIL the input paints its cursor into the row\n");
        failures++;
    }
    free(painted);
}

int main(void)
{
    setenv("COLUMNS", "80", 1);
    setenv("LINES", "24", 1);

    fflush(stdout);
    int saved = dup(STDOUT_FILENO);
    int null = open("/dev/null", O_WRONLY);
    if (null >= 0) {
        dup2(null, STDOUT_FILENO);
        close(null);
    }

    ui_init();
    struct prompt *p = prompt_new(NULL, 0);
    chrome_bind(p);

    check_drawn_cursor(p);

    prompt_free(p);

    fflush(stdout);
    if (saved >= 0) {
        dup2(saved, STDOUT_FILENO);
        close(saved);
    }
    if (failures) {
        fprintf(stderr, "prompttest: %d failure(s)\n", failures);
        return 1;
    }
    printf("prompttest: ok\n");
    return 0;
}
