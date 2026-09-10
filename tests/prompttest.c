#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "chrome.h"
#include "prompt.h"
#include "sidechannel.h"
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

static void eq_line(struct prompt *p, const char *what, const char *want)
{
    const char *got = prompt_line(p);
    if (got && !strcmp(got, want))
        return;
    fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n",
            what, got ? got : "(null)", want);
    failures++;
}

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

    prompt_set_preview(p, "hello");
    eq_line(p, "the preview is the line text", "hello");

    prompt_set_preview(p, "hello there");
    eq_line(p, "an update replaces the preview", "hello there");

    prompt_set_preview(p, "");
    eq_line(p, "an empty preview removes it", "");

    prompt_insert(p, "typed");
    prompt_set_preview(p, "spoken");
    eq_line(p, "the preview follows typed text", "typed spoken");
    prompt_set_preview(p, "spoken words");
    eq_line(p, "an update keeps the typed text", "typed spoken words");
    prompt_set_preview(p, "");
    eq_line(p, "removing the preview keeps the typed text", "typed");

    prompt_set_preview(p, "spoken");
    prompt_insert(p, "more");
    eq_line(p, "a line inserted under a preview goes before it",
            "typed more spoken");
    prompt_set_preview(p, "spoken again");
    eq_line(p, "the moved preview is still replaced",
            "typed more spoken again");
    prompt_set_preview(p, "");
    eq_line(p, "and removed", "typed more");

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
