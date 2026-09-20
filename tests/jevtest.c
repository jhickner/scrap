#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jev.h"
#include "vendor/cJSON.h"

static int failures;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static void eq_str(const char *what, const char *got, const char *want)
{
    if (got && want && !strcmp(got, want))
        return;
    fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)",
            want ? want : "(null)");
    failures++;
}

static void eq_num(const char *what, double got, double want)
{
    if (got > want - 0.0001 && got < want + 0.0001)
        return;
    fprintf(stderr, "FAIL %s: got %g, want %g\n", what, got, want);
    failures++;
}

static const char *ANSWER =
    "{\"answers\":{\"ready\":{\"noul\":0.92},\"cancel\":{\"noul\":0.03},"
    "\"hold\":{\"noul\":0.61},\"release\":{\"noul\":0.12}},"
    "\"usage\":{\"input_tokens\":740,\"output_tokens\":12}}";

static void check_parse(void)
{
    struct jev_result r;
    if (!jev_parse(ANSWER, &r))
        fail("an answer with a ready noul parses");
    eq_num("ready", r.ready, 0.92);
    eq_num("cancel", r.cancel, 0.03);
    eq_num("hold", r.hold, 0.61);
    eq_num("release", r.release, 0.12);
    if (r.in_tok != 740 || r.out_tok != 12)
        fail("usage tokens are read");
    if (r.error)
        fail("a complete answer is not an error");

    if (jev_parse("{\"answers\":{\"cancel\":{\"noul\":0.1}}}", &r))
        fail("an answer without a ready noul fails");
    if (!r.error)
        fail("an answer without a ready noul is an error");
    eq_num("a missing question", r.ready, -1);

    if (jev_parse("not json", &r))
        fail("text that is not json fails");
    if (!r.error)
        fail("text that is not json is an error");
}

static void check_strip(void)
{
    eq_str("the whole utterance was submitted", jev_strip_prefix("Okay.", "Okay?"), "");
    eq_str("words after the submitted prefix",
           jev_strip_prefix("Okay.", "Okay? So next"), "So next");
    eq_str("a revised word inside the prefix",
           jev_strip_prefix("Let's look at the mem project",
                            "Let's look at the men project? And then commit"),
           "And then commit");
    eq_str("a new utterance is left whole", jev_strip_prefix("Okay.", "Yes"), "Yes");
    eq_str("nothing submitted leaves the text alone",
           jev_strip_prefix("", "Okay? So next"), "Okay? So next");
}

static void check_body(void)
{
    char *body = jev_body("run the tests", "assistant: Should I run them?\nuser: yes",
                          "run the\nrun the tests\n", 340);
    if (!body) {
        fail("a body is built");
        return;
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) {
        fail("the body is json");
        return;
    }
    cJSON *state = cJSON_GetObjectItem(root, "state");
    eq_str("transcript", cJSON_GetStringValue(cJSON_GetObjectItem(state, "transcript")),
           "run the tests");
    eq_str("recent turns", cJSON_GetStringValue(cJSON_GetObjectItem(state, "recent_turns")),
           "assistant: Should I run them?\nuser: yes");
    eq_str("previous partials",
           cJSON_GetStringValue(cJSON_GetObjectItem(state, "previous_partials")),
           "run the\nrun the tests\n");
    eq_num("ms since the last change",
           cJSON_GetNumberValue(cJSON_GetObjectItem(state, "ms_since_last_change")), 340);

    cJSON *questions = cJSON_GetObjectItem(root, "questions");
    static const char *const names[] = {"ready", "cancel", "hold", "release"};
    for (int i = 0; i < (int)(sizeof names / sizeof names[0]); i++) {
        cJSON *q = cJSON_GetObjectItem(questions, names[i]);
        if (!q) {
            fail(names[i]);
            continue;
        }
        eq_str("question type", cJSON_GetStringValue(cJSON_GetObjectItem(q, "type")), "noul");
        cJSON *crit = cJSON_GetObjectItem(q, "criteria");
        if (!cJSON_GetStringValue(cJSON_GetObjectItem(q, "instructions")) ||
            !cJSON_GetStringValue(cJSON_GetObjectItem(crit, "true")) ||
            !cJSON_GetStringValue(cJSON_GetObjectItem(crit, "false")))
            fail("every question carries instructions and criteria");
    }
    cJSON_Delete(root);
}

static void check_backend(void)
{
    if (!jev_set_backend("experiential") || strcmp(jev_backend(), "experiential"))
        fail("experiential is a backend");
    if (!jev_set_backend("typesafe") || strcmp(jev_backend(), "typesafe"))
        fail("typesafe is a backend");
    if (jev_set_backend("elsewhere"))
        fail("an unknown backend is refused");
    eq_str("the backend is unchanged by a refused name", jev_backend(), "typesafe");
}

int main(void)
{
    check_parse();
    check_strip();
    check_body();
    check_backend();
    if (failures)
        return 1;
    puts("jevtest: all checks passed");
    return 0;
}
