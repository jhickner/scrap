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

    /* the exact call order of a spoken cancel right after the wake word */
    prompt_set_preview(p, "Listen");
    prompt_set_preview(p, "Listen Cancel");
    prompt_set_preview(p, "Listen");
    prompt_claim_preview(p, prompt_line(p));
    prompt_set_preview(p, "");
    eq_line(p, "a spoken cancel empties the box", "");
    prompt_insert(p, "typed");

    prompt_set_preview(p, "spoken");
    prompt_insert(p, "more");
    eq_line(p, "a line inserted under a preview goes before it",
            "typed more spoken");
    prompt_set_preview(p, "spoken again");
    eq_line(p, "the moved preview is still replaced",
            "typed more spoken again");
    prompt_set_preview(p, "");
    eq_line(p, "and removed", "typed more");

    prompt_insert(p, " listen held");
    if (!prompt_claim_preview(p, "listen held")) {
        fprintf(stderr, "FAIL a preview is claimed from text in the line\n");
        failures++;
    }
    prompt_set_preview(p, "listen held words");
    eq_line(p, "a claimed preview is replaced in place", "typed more listen held words");
    prompt_set_preview(p, "");
    eq_line(p, "a claimed preview is removed", "typed more");
    if (prompt_claim_preview(p, "not there")) {
        fprintf(stderr, "FAIL claiming absent text fails\n");
        failures++;
    }
    prompt_set_preview(p, "spoken");
    eq_line(p, "a failed claim leaves no preview behind", "typed more spoken");
    prompt_set_preview(p, "");

    prompt_adopt_draft("x hello y hello", 0);
    prompt_claim_preview(p, "hello");
    prompt_insert(p, "ab");
    prompt_set_preview(p, "hello there");
    eq_line(p, "a moved preview updates the copy nearest where it was",
            "abx hello y hello there");
    tty_event home = {0};
    home.key = TK_HOME;
    prompt_live_key(p, &home);
    prompt_set_preview(p, "hello there");
    if (prompt_cursor(p) != 0) {
        fprintf(stderr, "FAIL the same preview again leaves the caret: got %d\n", prompt_cursor(p));
        failures++;
    }
    prompt_adopt_draft("before spoken after", 0);
    prompt_claim_preview(p, "spoken");
    prompt_set_preview(p, "spoken words");
    if (prompt_cursor(p) != 0) {
        fprintf(stderr, "FAIL updating speech moved a caret before its span\n");
        failures++;
    }
    prompt_adopt_draft("before spoken after", 19);
    prompt_claim_preview(p, "spoken");
    prompt_set_preview(p, "spoken words");
    if (prompt_cursor(p) != 25) {
        fprintf(stderr, "FAIL updating speech moved a caret out of its typed suffix\n");
        failures++;
    }
    prompt_adopt_draft("typed more", 10);

    prompt_insert(p, " hello world");
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
        if (prompt_cursor(p) != 3) {
            fprintf(stderr, "FAIL adopt cursor: got %d, want 3\n", prompt_cursor(p));
            failures++;
        }
        prompt_adopt_draft(NULL, 0);
        eq_line(p, "an empty draft clears the line", "");
        prompt_adopt_draft(draft, 5);
        eq_line(p, "the parked draft comes back", "typed more hello world");
        if (prompt_cursor(p) != 5) {
            fprintf(stderr, "FAIL restored cursor: got %d, want 5\n",
                    prompt_cursor(p));
            failures++;
        }
        prompt_adopt_draft("one\ntwo", 3);
        eq_line(p, "a multi-line draft is kept", "one\ntwo");
        if (prompt_cursor(p) != 3) {
            fprintf(stderr, "FAIL multi-line cursor: got %d, want 3\n",
                    prompt_cursor(p));
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
