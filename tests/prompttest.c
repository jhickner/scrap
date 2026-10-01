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

static const char *draft_line(struct prompt *p)
{
    (void)p;
    static char buf[1024];
    char *text;
    prompt_stash_draft(&text, NULL);
    snprintf(buf, sizeof buf, "%s", text ? text : "");
    free(text);
    return buf;
}

static int draft_cursor(struct prompt *p)
{
    (void)p;
    char *text;
    int   cursor;
    prompt_stash_draft(&text, &cursor);
    free(text);
    return cursor;
}

static void eq_line(struct prompt *p, const char *what, const char *want)
{
    const char *got = draft_line(p);
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

    prompt_adopt_draft("typed more hello world", 22);
    {
        char *draft = NULL;
        int   cursor = 0;
        prompt_stash_draft(&draft, &cursor);
        eq_line(p, "stashing keeps the live line", "typed more hello world");
        if (cursor != (int)strlen("typed more hello world")) {
            fprintf(stderr, "FAIL stash cursor: got %d\n", cursor);
            failures++;
        }
        prompt_adopt_draft("ab cd", 3);
        eq_line(p, "adopting replaces the line", "ab cd");
        if (draft_cursor(p) != 3) {
            fprintf(stderr, "FAIL adopt cursor: got %d, want 3\n", draft_cursor(p));
            failures++;
        }
        prompt_adopt_draft(NULL, 0);
        eq_line(p, "an empty draft clears the line", "");
        prompt_adopt_draft(draft, 5);
        eq_line(p, "the parked draft comes back", "typed more hello world");
        if (draft_cursor(p) != 5) {
            fprintf(stderr, "FAIL restored cursor: got %d, want 5\n",
                    draft_cursor(p));
            failures++;
        }
        prompt_adopt_draft("one\ntwo", 3);
        eq_line(p, "a multi-line draft is kept", "one\ntwo");
        if (draft_cursor(p) != 3) {
            fprintf(stderr, "FAIL multi-line cursor: got %d, want 3\n",
                    draft_cursor(p));
            failures++;
        }
        free(draft);
    }

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
