#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board.h"
#include "child.h"
#include "text.h"
#include "boarddefaults.h"
#include "boardfile.h"
#include "boarddiff.h"
#include "boardcfg.h"
#include "boardflow.h"
#include "boardstep.h"
#include "boardstep.h"
#include "boardtriage.h"
#include "gitcmd.h"
#include "mdcfg.h"
#include "replyjson.h"
#include "sessionfork.h"

static int failures;

const char *sessionfork_program(void)
{
    return "false";
}

static void fail(const char *what)
{
    fprintf(stderr, "FAIL %s\n", what);
    failures++;
}

static void expect(int ok, const char *what)
{
    if (!ok)
        fail(what);
}

static char home[] = "/tmp/boardtest.XXXXXX";

static void cleanup(void)
{
    char cmd[256];
    snprintf(cmd, sizeof cmd, "rm -rf %s", home);
    if (system(cmd) != 0)
        fprintf(stderr, "could not clean %s\n", home);
}

static int count_in(struct board_card *v, int n, enum board_col col)
{
    int k = 0;
    for (int i = 0; i < n; i++)
        k += v[i].col == col;
    return k;
}

static const char *where(const char *id)
{
    static char out[BOARD_STEP_NAME];
    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    snprintf(out, sizeof out, "%s", c ? board_where(c) : "");
    board_free(v, n);
    return out;
}

static int at(const char *id, const char *place)
{
    return !strcmp(where(id), place);
}

static void test_capture(void)
{
    char id[BOARD_ID_MAX] = {0};
    expect(board_add("fix the tab strip\nit wraps at 80 columns", "/tmp/repo", id),
           "capture returns");
    expect(id[0] != '\0', "capture mints an id");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    expect(n == 1, "one card in the store");

    struct board_card *c = board_find(v, n, id);
    if (!c) {
        fail("card is found by its id");
        board_free(v, n);
        return;
    }
    expect(c->col == BOARD_NEW, "capture lands in new");
    expect(!strcmp(c->title, "fix the tab strip"), "title is the first line");
    expect(strstr(c->body, "80 columns") != NULL, "body keeps the rest");
    expect(!strcmp(c->cwd, "/tmp/repo"), "cwd is recorded");
    expect(c->kind[0] == '\0', "kind waits for triage");
    expect(c->backend_pin[0] == '\0' && c->tier_pin[0] == '\0',
           "capture does not pin");
    expect(c->created > 0 && c->updated > 0, "card is stamped");
    board_free(v, n);
}

static void test_pin(void)
{
    char id[BOARD_ID_MAX] = {0};
    expect(board_add("pin the worker", "/tmp/repo", id), "capture");
    expect(board_pin(id, "grok", "high"), "pin both");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    if (!c) {
        fail("pinned card is found");
        board_free(v, n);
        return;
    }
    expect(!strcmp(c->backend_pin, "grok"), "backend pin");
    expect(!strcmp(c->tier_pin, "high"), "tier pin");
    expect(!strcmp(c->title, "pin the worker"), "pin leaves the spec");
    board_free(v, n);

    expect(board_pin(id, "claude", NULL), "pin backend only");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && !strcmp(c->backend_pin, "claude"), "backend pin replaced");
    expect(c && !strcmp(c->tier_pin, "high"), "tier pin left alone");
    board_free(v, n);

    expect(board_pin(id, NULL, "low"), "pin tier only");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && !strcmp(c->backend_pin, "claude"), "backend pin left alone");
    expect(c && !strcmp(c->tier_pin, "low"), "tier pin replaced");
    board_free(v, n);

    expect(!board_pin("nope", "grok", "high"), "pin of a stranger fails");
}

static void test_ids_are_distinct(void)
{
    char seen[32][BOARD_ID_MAX];
    for (int i = 0; i < 32; i++) {
        char text[64];
        snprintf(text, sizeof text, "card number %d", i);
        if (!board_add(text, "/tmp/repo", seen[i])) {
            fail("bulk capture");
            return;
        }
        for (int j = 0; j < i; j++)
            if (!strcmp(seen[i], seen[j])) {
                fail("ids collide");
                return;
            }
    }

    struct board_card *v = NULL;
    int                n = board_load(&v);
    expect(n == 33, "every captured card survives");
    board_free(v, n);
}

static void test_note_and_move(void)
{
    char id[BOARD_ID_MAX] = {0};
    expect(board_add("worktree cleanup after approve", "/tmp/repo", id), "capture");

    expect(board_note(id, "triage", "feature, mux, priority 0"), "note appends");
    expect(board_move(id, BOARD_BACKLOG, NULL, "triage", "classified as feature"),
           "move with a reason");
    expect(board_move(id, BOARD_STEP, "worktree", "you", NULL), "move without a reason");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    if (!c) {
        fail("card survives its notes");
        board_free(v, n);
        return;
    }
    expect(board_at(c, "worktree"), "column follows the last move");
    expect(c->log_n == 2, "a move with no reason logs nothing");
    expect(!strcmp(c->log[0].who, "triage"), "note records who");
    expect(strstr(c->log[1].text, "classified") != NULL, "move logs its reason");
    expect(c->log[0].ts > 0, "note is stamped");
    board_free(v, n);
}

static void test_update_preserves_created(void)
{
    char id[BOARD_ID_MAX] = {0};
    expect(board_add("telegram menus lose the cancel row", "/tmp/repo", id), "capture");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    if (!c) {
        fail("card to update");
        board_free(v, n);
        return;
    }

    time_t created = c->created;
    snprintf(c->kind, sizeof c->kind, "bug");
    snprintf(c->model, sizeof c->model, "opus");
    snprintf(c->base, sizeof c->base, "b519936");
    c->priority = 2;
    c->cost_usd = 0.42;
    c->tokens_in = 12345;
    c->tokens_out = 678;
    board_put(c, "review");
    expect(board_update(c), "update writes back");
    board_free(v, n);

    n = board_load(&v);
    c = board_find(v, n, id);
    if (!c) {
        fail("updated card is still there");
        board_free(v, n);
        return;
    }
    expect(!strcmp(c->kind, "bug"), "kind round-trips");
    expect(!strcmp(c->model, "opus"), "model round-trips");
    expect(!strcmp(c->base, "b519936"), "base sha round-trips");
    expect(c->priority == 2, "priority round-trips");
    expect(c->cost_usd > 0.41 && c->cost_usd < 0.43, "cost round-trips");
    expect(c->tokens_in == 12345 && c->tokens_out == 678, "tokens round-trip");
    expect(board_at(c, "review"), "column round-trips");
    expect(c->created == created, "update leaves created alone");
    board_free(v, n);
}

static void test_update_leaves_others_alone(void)
{
    struct board_card *before = NULL;
    int                n_before = board_load(&before);

    char id[BOARD_ID_MAX] = {0};
    expect(board_add("a card to touch", "/tmp/repo", id), "capture");
    expect(board_note(id, "you", "touched"), "note");

    struct board_card *after = NULL;
    int                n_after = board_load(&after);
    expect(n_after == n_before + 1, "only the new card was added");

    int intact = 1;
    for (int i = 0; i < n_before; i++) {
        struct board_card *c = board_find(after, n_after, before[i].id);
        if (!c || strcmp(board_where(c), board_where(&before[i])) ||
            strcmp(c->title, before[i].title) ||
            c->log_n != before[i].log_n)
            intact = 0;
    }
    expect(intact, "every other card is untouched");

    board_free(before, n_before);
    board_free(after, n_after);
}

static void test_remove(void)
{
    char id[BOARD_ID_MAX] = {0};
    expect(board_add("delete me", "/tmp/repo", id), "capture");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    int                had = n;
    expect(board_find(v, n, id) != NULL, "card is there before");
    board_free(v, n);

    expect(board_remove(id), "remove reports success");
    expect(!board_remove(id), "removing twice fails");
    expect(!board_remove("nope"), "removing a stranger fails");

    n = board_load(&v);
    expect(n == had - 1, "one fewer card");
    expect(board_find(v, n, id) == NULL, "card is gone");
    board_free(v, n);
}

static void test_columns(void)
{
    struct board_card where_at = {0};

    board_put(&where_at, "unclear");
    expect(where_at.col == BOARD_UNCLEAR, "one of the board's own columns by name");
    board_put(&where_at, "nonsense");
    expect(board_at(&where_at, "nonsense"),
           "a column the board does not know is a step it has not read yet");
    board_put(&where_at, "review");
    expect(board_at(&where_at, "review"), "a step is a column of its own");
    board_put(&where_at, "merging");
    expect(board_at(&where_at, "merge"), "a column the store used to write");
    board_put(&where_at, "active");
    expect(board_at(&where_at, "worktree"), "and the one a working card had");

    for (int i = 0; i < BOARD_COLS; i++) {
        if (i == BOARD_STEP)
            continue;
        board_put(&where_at, board_col_name((enum board_col)i));
        if ((int)where_at.col != i) {
            fail("every column round-trips");
            break;
        }
    }

    struct board_card *v = NULL;
    int                n = board_load(&v);
    expect(count_in(v, n, BOARD_NEW) > 0, "cards sit in new");
    board_free(v, n);
}

static void test_attempts_reset_when_answered(void)
{
    char id[BOARD_ID_MAX] = {0};
    board_add("do the thing", "/tmp/repo", id);

    struct board_card *v = NULL;
    int                n = board_load(&v);
    expect(boardtriage_attempts(board_find(v, n, id)) == 0,
           "a fresh card has had no turns");
    board_free(v, n);

    board_move(id, BOARD_UNCLEAR, NULL, "triage", "which thing?");
    n = board_load(&v);
    expect(boardtriage_attempts(board_find(v, n, id)) == 1, "unclear counts a turn");
    board_free(v, n);

    board_note(id, "worker", "still stuck");
    n = board_load(&v);
    expect(boardtriage_attempts(board_find(v, n, id)) == 1,
           "a worker saying something is not an answer");
    board_free(v, n);

    board_note(id, "you", "answered, and sent back to triage");
    n = board_load(&v);
    expect(boardtriage_attempts(board_find(v, n, id)) == 0,
           "answering it makes it new again");
    board_free(v, n);

    board_move(id, BOARD_UNCLEAR, NULL, "triage", "still cannot tell");
    n = board_load(&v);
    expect(boardtriage_attempts(board_find(v, n, id)) == 1,
           "the next turn counts from the answer");
    board_free(v, n);

    board_remove(id);
}

static void plant_stale(const char *id)
{
    FILE *f = fopen(board_path(), "a");
    if (!f) {
        fail("could not plant a stale card");
        return;
    }
    fprintf(f, "{\"id\":\"%s\",\"col\":\"done\",\"title\":\"ancient\","
               "\"body\":\"ancient\",\"cwd\":\"/tmp/repo\","
               "\"created\":1000,\"updated\":1000,\"log\":[]}\n", id);
    fclose(f);
}

static void test_archive(void)
{
    char busy[BOARD_ID_MAX] = {0}, fresh[BOARD_ID_MAX] = {0};
    expect(board_add("still being worked on", "/tmp/repo", busy), "capture");
    expect(board_add("finished just now", "/tmp/repo", fresh), "capture");
    expect(board_move(fresh, BOARD_DONE, NULL, "you", NULL), "done");
    plant_stale("aged");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    expect(board_find(v, n, "aged") != NULL, "the stale card is on the board");
    board_free(v, n);

    expect(board_archive(0) == 0, "zero days keeps everything");
    n = board_load(&v);
    expect(board_find(v, n, "aged") != NULL, "and it is still there");
    board_free(v, n);

    expect(board_archive(1) == 1, "one stale card is archived");

    n = board_load(&v);
    expect(board_find(v, n, "aged") == NULL, "the stale card has left the board");
    expect(board_find(v, n, busy) != NULL, "unfinished work stays");
    expect(board_find(v, n, fresh) != NULL, "recently finished work stays");
    board_free(v, n);

    char path[4300];
    snprintf(path, sizeof path, "%s/.config/mux/board-archive.jsonl", home);
    char *text = text_slurp(path, 1u << 20, NULL);
    expect(text && strstr(text, "\"id\":\"aged\""), "and is in the archive");
    free(text);

    expect(board_archive(1) == 0, "nothing left to archive");

    board_remove(busy);
    board_remove(fresh);
}

static void test_empty_and_missing(void)
{
    struct board_card *v = (struct board_card *)1;
    unlink(board_path());
    int n = board_load(&v);
    expect(n == 0 && v == NULL, "a missing store loads as empty");
    expect(!board_add("", "/tmp/repo", NULL), "empty text is not a card");
    expect(!board_note("ghost", "you", "hi"), "a note needs a card");
    board_free(v, n);
}

#define DEFS_MAX 32

static struct board_default defs[DEFS_MAX];
static int                  defs_n;

static void def_add(const char *path, const char *front, const char *body)
{
    char text[8192];
    snprintf(text, sizeof text, "---\n%s---\n\n%s", front, body ? body : "");

    int at = defs_n;
    for (int i = 0; i < defs_n; i++)
        if (!strcmp(defs[i].path, path))
            at = i;
    if (at == defs_n) {
        if (defs_n == DEFS_MAX)
            return;
        defs[defs_n++].path = strdup(path);
    } else {
        free((char *)defs[at].text);
    }
    defs[at].text = strdup(text);
}

static void action_def(const char *name, const char *front, const char *prompt)
{
    char path[128];
    snprintf(path, sizeof path, "actions/%s.md", name);
    def_add(path, front, prompt);
}

static void kind_def(const char *name, const char *front, const char *body)
{
    char path[128];
    snprintf(path, sizeof path, "kinds/%s.md", name);
    def_add(path, front, body);
}

static void defs_clear(void)
{
    for (int i = 0; i < defs_n; i++) {
        free((char *)defs[i].path);
        free((char *)defs[i].text);
    }
    defs_n = 0;
}

static void defs_use(void)
{
    boardcfg_defaults(defs, defs_n);
}

static struct board_card of_kind(const char *kind)
{
    struct board_card c = {0};
    snprintf(c.kind, sizeof c.kind, "%s", kind);
    return c;
}

static int goes(const char *kind, const char *from, const char *want)
{
    struct board_card c = of_kind(kind);
    const char       *next = boardflow_next(&c, from, 1);
    if (!want)
        return next == NULL;
    return next && !strcmp(next, want);
}

static void steps_for(const char *kind, const char *steps)
{
    char front[512];
    snprintf(front, sizeof front,
             "means: a kind a test made up\npriority: 1\nsteps: %s\n", steps);
    kind_def(kind, front, "");
    defs_use();
}

static void with_actions(void)
{
    defs_clear();
    action_def("worker", "runs: worker\ntier: high\nstep: worktree\n",
             "work the card");
    action_def("test", "runs: worker\ntier: high\n", "test the change");
    action_def("review", "runs: person\ntier: high\n", "");
    action_def("audit",
             "runs: worker\ntier: high\nfail marker: FINDINGS\n"
             "fail step: worktree\n",
             "read the diff");
    action_def("merge", "runs: worker\ntier: high\n", "land it");
    defs_use();
}

static void test_a_step_answers_for_itself(void)
{
    with_actions();
    steps_for("audited", "worktree, review, audit, merge");

    char id[BOARD_ID_MAX] = {0};
    expect(board_add("a card to audit", "/tmp/repo", id), "capture");
    expect(board_note(id, "worker", "wrote the fix"), "the worker had it first");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    if (!c) {
        fail("the card is stored");
        board_free(v, n);
        return;
    }
    struct board_card edited = *c;
    snprintf(edited.kind, sizeof edited.kind, "audited");
    board_put(&edited, "audit");
    expect(board_update(&edited), "the card sits at the audit step");
    board_free(v, n);

    n = board_load(&v);
    c = board_find(v, n, id);
    expect(boardstep_finished(c, boardcfg_for_step("audit"),
                              "src/a.c leaks the buffer\nFINDINGS"),
           "an answer is taken");
    board_free(v, n);

    expect(at(id, "worktree"), "the marker sends the card back to its worker");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && board_said(c, "audit") &&
               strstr(board_said(c, "audit"), "leaks"),
           "with what it found on the card");
    char *since = c ? boardstep_since(c, "worker") : NULL;
    expect(since && strstr(since, "leaks") && strstr(since, "audit"),
           "and in what the worker picking it up is told");
    free(since);
    board_free(v, n);

    expect(board_move(id, BOARD_STEP, "audit", "you", NULL), "back into audit");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(boardstep_finished(c, boardcfg_for_step("audit"), "nothing to report"), "a clean answer");
    board_free(v, n);
    expect(at(id, "merge"), "sends the card on to the step after it");

    expect(board_move(id, BOARD_STEP, "audit", "you", NULL), "into audit again");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(boardstep_finished(c, boardcfg_for_step("audit"), NULL), "a turn that said nothing");
    board_free(v, n);
    expect(at(id, "merge"), "does not hold the card either");

    board_remove(id);
}

static void test_reply_json(void)
{
    cJSON *o = replyjson_parse("{\"n\":1}\n\nCorrection:\n\n{\"n\":2}");
    expect(o && cJSON_GetObjectItem(o, "n") &&
           cJSON_GetObjectItem(o, "n")->valuedouble == 2,
           "the last answer is the one meant");
    cJSON_Delete(o);

    o = replyjson_parse("Here you go:\n{\"n\":3}\nhope that helps");
    expect(o && cJSON_GetObjectItem(o, "n") &&
           cJSON_GetObjectItem(o, "n")->valuedouble == 3,
           "prose either side is stepped over");
    cJSON_Delete(o);

    o = replyjson_parse("{\"s\":\"a } in the text\",\"n\":4}");
    expect(o && cJSON_GetObjectItem(o, "n") &&
           cJSON_GetObjectItem(o, "n")->valuedouble == 4,
           "a brace in a string does not close the object");
    cJSON_Delete(o);

    o = replyjson_parse("{\"outer\":{\"inner\":1},\"n\":5}");
    expect(o && cJSON_GetObjectItem(o, "n") &&
           cJSON_GetObjectItem(o, "n")->valuedouble == 5,
           "a nested object is not mistaken for the whole");
    cJSON_Delete(o);

    expect(!replyjson_parse("no json at all"), "prose alone is nothing");
    expect(!replyjson_parse(NULL), "nothing is nothing");
}

static void write_kind(const char *name, const char *const *keys,
                       const char *const *vals, int n, const char *body)
{
    char front[2048];
    size_t at = 0;
    front[0] = '\0';
    for (int i = 0; i < n && at < sizeof front; i++)
        at += (size_t)snprintf(front + at, sizeof front - at, "%s: %s\n",
                               keys[i], vals[i]);
    kind_def(name, front, body);
}

static void test_a_planner_works_without_a_worktree(void)
{
    defs_clear();
    action_def("plan", "runs: worker\ntier: high\nstep: plan\n",
             "Read the repo and write the plan.");
    action_def("review", "runs: person\ntier: med\n", "");
    kind_def("plan", "means: a plan of the work\npriority: 1\n"
                     "steps: plan, review\nworktree: 0\n", "");
    defs_use();

    expect(boardflow_lands("plan"), "a plan card is the worker's to take");
    expect(!boardflow_worktree("plan"), "and gets no worktree to take it in");
    expect(goes("plan", NULL, "plan"), "it starts on the worker");
    expect(goes("plan", "plan", "review"), "then stops for a person");
    expect(goes("plan", "review", NULL), "and approving that finishes it");

    with_actions();
}

static void test_a_kind_file_carries_its_approval_prompt(void)
{
    const char *keys[] = {"means", "priority", "steps", "approval prompt"};
    const char *vals[] = {"something the person wants to buy", "1", "review",
                          "Order it, and say what was ordered."};
    with_actions();
    write_kind("buy", keys, vals, 4, "Search Amazon with the web skill.\n");
    defs_use();

    const struct board_kind *k = boardcfg_kind("buy");
    if (!k) {
        fail("a kind is whatever its file says");
        return;
    }
    expect(k->priority == 1, "priority comes off the file");
    expect(boardcfg_kind_takes("buy", "review"), "so does the step it stops at");
    expect(!boardcfg_kind_takes("buy", "worktree"), "and the ones it skips");
    expect(goes("buy", NULL, "review"), "the card waits in review");
    expect(k->approval_prompt && !strcmp(k->approval_prompt, vals[3]),
           "the approval prompt is read whole");

    struct board_card card = of_kind("buy");
    snprintf(card.step, sizeof card.step, "worktree");
    card.col = BOARD_STEP;
    expect(boardflow_after_turn(&card) &&
               !strcmp(boardflow_after_turn(&card), "review"),
           "the first turn stops for a person");

    struct board_note said = {0, "you", (char *)vals[3]};
    card.log = &said;
    card.log_n = 1;
    expect(boardflow_approval(&card) == NULL, "the approval prompt is sent once");
    expect(boardflow_after_turn(&card) == NULL,
           "and the turn answering it ends the card");

    char block[4096];
    boardcfg_kinds_block(block, sizeof block);
    expect(strstr(block, "buy") != NULL, "the classifier is told about it");

    struct board_cfg *c = boardcfg_copy();
    expect(boardcfg_set(c), "the config writes back");
    boardcfg_free(c);
    boardcfg_reload();
    k = boardcfg_kind("buy");
    expect(k && k->approval_prompt && !strcmp(k->approval_prompt, vals[3]),
           "and survives a write and a reload");
}

static void test_a_kind_prompt_takes_the_card_id(void)
{
    const char *keys[] = {"means", "priority", "steps", "approval prompt"};
    const char *vals[] = {"something the person wants to buy", "1", "review",
                          "Order it in the web-{id} window."};
    write_kind("buy", keys, vals, 4, "Drive the web-{id} window.\n");
    defs_use();

    struct board_card card = {0};
    snprintf(card.id, sizeof card.id, "c7f2");
    snprintf(card.kind, sizeof card.kind, "buy");

    char *say = boardflow_approval(&card);
    expect(say && !strcmp(say, "Order it in the web-c7f2 window."),
           "the approval prompt takes the card id");

    struct board_note said = {0, "you", say};
    card.log = &said;
    card.log_n = 1;
    char *again = boardflow_approval(&card);
    expect(again == NULL, "and the expanded prompt is sent once");
    free(again);
    free(say);

    char *mine = boardcfg_expand("Drive the web-{id} window.\n", card.id);
    expect(mine && !strcmp(mine, "Drive the web-c7f2 window.\n"),
           "so does the prompt the worker is given");
    free(mine);
}

static void write_the_shipped_kinds(void)
{
    with_actions();
    static const char *const keys[] = {"means", "priority", "steps"};
    static const char *const work = "worktree, review, audit, merge";
    static const struct {
        const char *name, *means, *priority, *steps;
    } kinds[] = {
        {"bug", "something that exists and is wrong", "2", NULL},
        {"feature", "something that should exist and does not", "1", NULL},
        {"chore", "upkeep: a rename, a bump, a cleanup", "0", NULL},
        {"todo", "something the person means to do", "0", ""},
        {"reference", "a link, a name, a fact", "0", ""},
    };

    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++) {
        const char *vals[] = {kinds[i].means, kinds[i].priority,
                              kinds[i].steps ? kinds[i].steps : work};
        write_kind(kinds[i].name, keys, vals, 3, "");
    }
    defs_use();
}

static void test_a_named_kind_skips_triage(void)
{
    write_the_shipped_kinds();

    char id[BOARD_ID_MAX] = {0};
    expect(board_add("bug: the list scrolls past its end\n\nhow to repeat it",
                     "/tmp/repo", id),
           "capture");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    expect(boardtriage_start(board_find(v, n, id)), "the named kind is taken");
    board_free(v, n);

    n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    expect(c && !strcmp(c->kind, "bug"), "the card is the kind it named");
    expect(c && c->col == BOARD_BACKLOG, "and skips new for backlog");
    expect(c && c->priority == boardcfg_priority("bug"),
           "at the priority of that kind");
    expect(c && !strcmp(c->title, "the list scrolls past its end"),
           "the title drops the prefix");
    expect(c && c->body && strstr(c->body, "how to repeat it") != NULL,
           "the body is left whole");
    board_free(v, n);

    board_remove(id);
}

static void test_projects_block(void)
{
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/working", home);
    mkdir(dir, 0700);

    char path[4300];
    snprintf(path, sizeof path, "%s/alpha", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/mux", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/.hidden", dir);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/notes.txt", dir);
    fclose(fopen(path, "w"));

    boardcfg_reload();

    char block[8192];
    boardcfg_projects_block(block, sizeof block);

    char want[4300];
    snprintf(want, sizeof want, "%s/mux", dir);
    expect(strstr(block, want) != NULL, "a project is listed by its path");
    expect(!strstr(block, "notes.txt"), "a file is not a project");
    expect(!strstr(block, ".hidden"), "nor is a dotted directory");

    snprintf(want, sizeof want, "%s/alpha", dir);
    const char *first = strstr(block, want);
    snprintf(want, sizeof want, "%s/mux", dir);
    expect(first && first < strstr(block, want), "the list is sorted");

    struct board_cfg *c = boardcfg_copy();
    c->projects[0] = '\0';
    expect(boardcfg_set(c), "the directory writes back");
    boardcfg_free(c);
    boardcfg_reload();
    expect(boardcfg()->projects[0] == '\0', "an empty directory survives a reload");

    boardcfg_projects_block(block, sizeof block);
    expect(block[0] == '\0', "and lists nothing");

    c = boardcfg_copy();
    snprintf(c->projects, sizeof c->projects, "~/working");
    boardcfg_set(c);
    boardcfg_free(c);
    boardcfg_reload();
}

static void test_kinds(void)
{
    write_the_shipped_kinds();

    expect(boardcfg_kind("bug") != NULL, "a configured kind is found");
    expect(boardcfg_kind("nonsense") == NULL, "an unconfigured one is not");
    expect(boardcfg_kind("") == NULL, "nor is no kind at all");

    expect(boardcfg_priority("bug") > boardcfg_priority("feature"),
           "a bug comes before a feature");
    expect(boardcfg_priority("feature") > boardcfg_priority("chore"),
           "a feature comes before a chore");
    expect(boardcfg_priority("nonsense") == 0, "an unknown kind is worth nothing");

    expect(boardcfg_kind_takes("bug", "worktree"), "work gets a worktree");
    expect(boardcfg_kind_takes("bug", "merge"), "and lands through the queue");
    expect(!boardcfg_kind_takes("reference", "worktree"),
           "a note to file gets neither");
    expect(!boardcfg_kind_takes("reference", "review"), "nor a review");
    expect(boardcfg_kind_takes("nonsense", "merge"),
           "an unknown kind walks the board's own steps, which is the careful "
           "way round");

    expect(goes("bug", "worktree", "review"), "work stops for a person first");
    expect(goes("bug", "review", "audit"), "then for an audit");
    expect(goes("bug", "audit", "merge"), "then for the queue");
    expect(goes("bug", "merge", NULL), "the queue is the last of it");
    expect(goes("reference", NULL, NULL),
           "a filed note is done when the worker stops");

    char block[4096];
    boardcfg_kinds_block(block, sizeof block);
    expect(strstr(block, "todo") && strstr(block, "chore"), "every kind is named");
    expect(strstr(block, "something that exists and is wrong") != NULL,
           "and each says what it means");
}

static int skip_card(const char *id)
{
    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    int                did = c && boardflow_skip(c);
    board_free(v, n);
    return did;
}

static void test_actions_are_what_the_files_say(void)
{
    defs_clear();
    action_def("auditor", "runs: worker\ntier: high\njob: audit\n",
             "read the diff");
    action_def("merge", "runs: worker\ntier: high\n", "land it");
    defs_use();

    const struct board_action *p = boardcfg_doing("audit");
    expect(p != NULL, "an action is found by the job its file names");
    expect(p && !strcmp(p->name, "auditor"),
           "and takes its own name from that file");
    expect(p && p->prompt && strstr(p->prompt, "read the diff"),
           "the body of the file is its prompt");
    expect(boardcfg_doing("triage") == NULL, "a job no role does has no role");
    expect(boardcfg_for_step("audit") == p, "the step it stands in is its own");
    expect(boardcfg_for_step("auditor") == NULL,
           "which is the job it does, not the file it is in");
}

static void test_a_step_that_fails_with_nowhere_to_send_it(void)
{
    with_actions();
    action_def("audit", "runs: worker\ntier: high\nfail marker: FINDINGS\n",
             "read the diff");
    defs_use();
    steps_for("nowhere", "worktree, review, audit, merge");

    char id[BOARD_ID_MAX] = {0};
    expect(board_add("a card with nowhere to go back to", "/tmp/repo", id),
           "capture");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    if (c) {
        struct board_card edited = *c;
        snprintf(edited.kind, sizeof edited.kind, "nowhere");
        board_put(&edited, "audit");
        board_update(&edited);
    }
    board_free(v, n);

    n = board_load(&v);
    c = board_find(v, n, id);
    expect(boardstep_finished(c, boardcfg_for_step("audit"), "FINDINGS"),
           "the answer fails the card");
    board_free(v, n);
    expect(at(id, "backlog"), "and it waits in the backlog for a worker");

    board_remove(id);
}

static void test_a_step_is_a_file(void)
{
    with_actions();
    steps_for("tested", "worktree, test, review, audit, merge");
    steps_for("untested", "worktree, review, audit, merge");
    steps_for("filed", "");

    const struct board_action *p = boardcfg_for_step("test");
    expect(p != NULL, "a file standing in a step is the step");
    expect(p && p->runs == BOARD_RUNS_WORKER, "a worker runs it unless it says");
    expect(p && p->skippable, "and it may be skipped by hand");
    expect(goes("tested", "worktree", "test"), "the kind that lists it walks it");
    expect(goes("untested", "worktree", "review"),
           "and the kind that does not walks past");
    expect(goes("tested", "test", "review"), "the step before the person's");

    expect(boardcfg_for_step("review") &&
               boardcfg_for_step("review")->runs == BOARD_RUNS_PERSON,
           "a step nothing runs waits for a person");
    expect(boardflow_lands("tested"), "a kind listing the worker's step gets a "
                                      "worktree");
    expect(!boardflow_lands("filed"), "and one that lists no steps at all does "
                                      "not");
    expect(goes("filed", NULL, NULL), "nor does it stop anywhere");

    char id[BOARD_ID_MAX] = {0};
    expect(board_add("the tab strip wraps at 80 columns", "/tmp/repo", id),
           "capture");
    expect(board_note(id, "worker", "fixed the wrap; run mux and widen"),
           "the worker says how to test it");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    if (!c) {
        fail("the card is stored");
        board_free(v, n);
        return;
    }
    struct board_card edited = *c;
    snprintf(edited.kind, sizeof edited.kind, "tested");
    snprintf(edited.base, sizeof edited.base, "b519936");
    board_put(&edited, "test");
    expect(board_update(&edited), "the card sits at the test step");
    board_free(v, n);

    n = board_load(&v);
    c = board_find(v, n, id);
    char *prompt = boardstep_prompt(c, boardcfg_for_step("test"));
    expect(prompt && strstr(prompt, "test the change"),
           "the file is what the worker at that step is told");
    expect(prompt && strstr(prompt, "the tab strip wraps"), "with the card");
    expect(prompt && !strstr(prompt, "run mux and widen"),
           "and not what was said earlier, which its own session already has");
    free(prompt);

    expect(boardstep_finished(c, boardcfg_for_step("test"),
                              "ran mux at 60 columns; the strip held"),
           "its answer moves the card on");
    board_free(v, n);
    expect(at(id, "review"), "to the person");

    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && board_said(c, "test") && board_said(c, "worker"),
           "with what both of them said on the card");
    board_free(v, n);

    board_remove(id);
}

static void unskippable(int no)
{
    with_actions();
    if (no) {
        action_def("audit",
                 "runs: worker\ntier: high\nskippable: 0\n"
                 "fail marker: FINDINGS\nfail step: worktree\n",
                 "read the diff");
        defs_use();
    }
    steps_for("skipped", "worktree, review, audit, merge");
}

static void test_skipping_a_step(void)
{
    with_actions();
    steps_for("skipped", "worktree, review, audit, merge");

    char id[BOARD_ID_MAX] = {0};
    expect(board_add("a card to skip past", "/tmp/repo", id), "capture");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    if (c) {
        struct board_card edited = *c;
        snprintf(edited.kind, sizeof edited.kind, "skipped");
        board_put(&edited, "audit");
        board_update(&edited);
    }
    board_free(v, n);

    unskippable(1);

    expect(!boardflow_skip(NULL), "a missing card is not skipped");
    expect(!skip_card(id), "an unskippable step holds the card");

    unskippable(0);

    expect(skip_card(id), "a skippable step is skipped");
    expect(at(id, "merge"), "skipping the audit sends it on to land");
    expect(skip_card(id), "and the next step skips too");
    expect(at(id, "done"), "skipping the last of them is the end of it");

    n = board_load(&v);
    c = board_find(v, n, id);
    int said = 0;
    if (c)
        for (int i = 0; i < c->log_n; i++)
            if (strstr(c->log[i].text, "audit skipped"))
                said = 1;
    expect(said, "the skip says which step it was");
    board_free(v, n);

    board_remove(id);
}

static void test_done_lists_newest_first(void)
{
    struct board_card older = {0}, newer = {0}, high = {0}, low = {0};

    older.col = newer.col = BOARD_DONE;
    snprintf(older.id, sizeof older.id, "old");
    snprintf(newer.id, sizeof newer.id, "new");
    older.created = 100;
    newer.created = 200;
    older.updated = 1000;
    newer.updated = 2000;
    older.priority = 9;
    newer.priority = 0;
    expect(board_cmp_col(&newer, &older) < 0, "newer done card sorts first");
    expect(board_cmp_col(&older, &newer) > 0, "older done card sorts second");

    high.col = low.col = BOARD_BACKLOG;
    snprintf(high.id, sizeof high.id, "hi");
    snprintf(low.id, sizeof low.id, "lo");
    high.priority = 2;
    low.priority = 1;
    high.created = 300;
    low.created = 100;
    expect(board_cmp_col(&high, &low) < 0, "higher priority still leads the backlog");
}

static int make_repo(char *root, size_t rsize)
{
    snprintf(root, rsize, "%s/named-wt", home);
    char cmd[2048];
    snprintf(cmd, sizeof cmd,
             "rm -rf %s && git init -q %s >/dev/null 2>&1 && "
             "git -C %s -c user.email=t@t -c user.name=t commit -q --allow-empty "
             "-m base >/dev/null 2>&1",
             root, root, root);
    if (system(cmd) != 0)
        return 0;

    char real[4096];
    if (realpath(root, real))
        snprintf(root, rsize, "%s", real);
    return 1;
}

static void test_worktree_name_is_stable(void)
{
    char root[4096];
    if (!make_repo(root, sizeof root)) {
        fprintf(stderr, "skipping worktree name test: no git\n");
        return;
    }

    char path[4200];
    snprintf(path, sizeof path, "%s/.claude/worktrees/abcd", root);

    expect(gitcmd_worktree_add(root, path, "worktree-abcd"), "first add");
    expect(gitcmd_worktree_add(root, path, "worktree-abcd"),
           "a second add reuses the same path");

    char file[4300];
    snprintf(file, sizeof file, "%s/kept.txt", path);
    FILE *f = fopen(file, "w");
    expect(f != NULL, "a file can be written in the worktree");
    if (f) {
        fputs("previous work\n", f);
        fclose(f);
    }

    expect(gitcmd_worktree_add(root, path, "worktree-abcd"),
           "add after a failed job still lands at the same path");
    f = fopen(file, "r");
    expect(f != NULL, "uncommitted work is still there");
    if (f) {
        char line[64] = {0};
        expect(fgets(line, sizeof line, f) && strstr(line, "previous work"),
               "and it is the file that was written");
        fclose(f);
    }

    char cmd[8192];
    snprintf(cmd, sizeof cmd,
             "git -C %s -c user.email=t@t -c user.name=t add kept.txt && "
             "git -C %s -c user.email=t@t -c user.name=t commit -q -m kept && "
             "rm -rf %s",
             path, path, path);
    expect(system(cmd) == 0, "commit then lose the directory");

    expect(gitcmd_worktree_add(root, path, "worktree-abcd"),
           "add after the directory is gone still uses the same path");
    f = fopen(file, "r");
    expect(f != NULL, "the committed file is checked out again");
    if (f) {
        char line[64] = {0};
        expect(fgets(line, sizeof line, f) && strstr(line, "previous work"),
               "from the branch of the same name");
        fclose(f);
    }
}

static void test_auto_pick_roundtrip(void)
{
    expect(boardcfg()->auto_pick == 0, "auto pick is off by default");

    struct board_cfg *c = boardcfg_copy();
    if (!c) {
        fail("cfg copy");
        return;
    }
    c->auto_pick = 1;
    expect(boardcfg_set(c), "save auto pick");
    boardcfg_free(c);

    boardcfg_reload();
    expect(boardcfg()->auto_pick == 1, "auto pick survives a reload");

    c = boardcfg_copy();
    if (!c) {
        fail("cfg copy after reload");
        return;
    }
    c->auto_pick = 0;
    boardcfg_set(c);
    boardcfg_free(c);
    boardcfg_reload();
    expect(boardcfg()->auto_pick == 0, "auto pick turns back off");
}

static void test_revision_tracks_writes(void)
{
    unsigned long before = board_revision();

    char id[BOARD_ID_MAX] = {0};
    expect(board_add("watch the store change", "/tmp/repo", id), "capture");
    unsigned long added = board_revision();
    expect(added != before, "capture moves the revision");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    expect(board_revision() == added, "a read leaves the revision alone");
    board_free(v, n);

    expect(board_move(id, BOARD_STEP, "review", "worker", "finished"), "move");
    expect(board_revision() != added, "a move moves the revision");

    board_remove(id);
}

static int has_dir(const char *leaf)
{
    char base[4096], path[4200];
    if (!path_config_dir(base, sizeof base))
        return 0;
    snprintf(path, sizeof path, "%s/%s", base, leaf);
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void test_the_shipped_kinds_and_roles_are_built_in(void)
{
    char dir[4096], path[4300];
    expect(mdcfg_dir(dir, sizeof dir, "board/roles"), "an old roles dir");
    snprintf(path, sizeof path, "%s/leftover.md", dir);
    const char *keys[] = {"runs"};
    const char *vals[] = {"agent"};
    expect(mdcfg_write(path, keys, vals, 1, "a role from an older mux"),
           "with a role file in it");

    boardcfg_defaults(NULL, 0);

    expect(!has_dir("board/roles"), "loading clears the old roles dir");
    expect(!has_dir("board/actions"), "and writes no actions dir of its own");
    expect(!has_dir("board/kinds"), "and writes no kinds dir of its own");
    expect(boardcfg_doing("leftover") == NULL, "the stale role is gone");

    char why[512];
    expect(!boardcfg_missing(why, sizeof why), "the built-in set is whole");

    const struct board_kind *k = boardcfg_kind("plan");
    expect(k != NULL, "plan is a kind without a file anywhere");
    expect(k && k->steps_n == 2, "walking the two steps it names");

    expect(!boardflow_worktree("plan"), "and takes no worktree");

    const struct board_action *p = boardcfg_action("plan");
    expect(p && p->runs == BOARD_RUNS_WORKER, "a worker takes the plan step");
    expect(p && p->where == BOARD_IN_WORKTREE, "and takes it in the worktree");
    expect(p && !p->needs_n, "gated on nothing");
    expect(p && p->prompt && strstr(p->prompt, "plan mode"),
           "its prompt is the body of the file it was built from");

    const struct board_action *name = boardcfg_action("triage");
    expect(name && name->on_capture, "naming runs on capture, not on trigger");

    const char *all[BOARD_ACTIONS_MAX];
    int         n = boardcfg_actions(all, BOARD_ACTIONS_MAX);
    expect(n == 4, "every action file is one action");
}

static void put_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fail("the file is written");
        return;
    }
    fputs(text, f);
    fclose(f);
}

static char *slurp_file(const char *path)
{
    return text_slurp(path, 1u << 20, NULL);
}

static void test_the_card_file_outlives_the_worktree(void)
{
    struct board_card c = {0};
    snprintf(c.id, sizeof c.id, "cf01");
    snprintf(c.worktree, sizeof c.worktree, "%s/tree", home);
    mkdir(c.worktree, 0700);

    char in_tree[4300];
    snprintf(in_tree, sizeof in_tree, "%s/CARD.md", c.worktree);
    put_file(in_tree, "# a card\n\n## Plan\n\nrewrite the cancel row\n");

    boardfile_keep(&c);

    char kept[4300];
    expect(boardfile_kept(c.id, kept, sizeof kept), "the copy has a path");
    char *saved = slurp_file(kept);
    expect(saved && strstr(saved, "rewrite the cancel row"),
           "and holds what the worker wrote");
    free(saved);

    char gone[4400];
    snprintf(gone, sizeof gone, "rm -rf %s", c.worktree);
    expect(system(gone) == 0, "the worktree goes");

    mkdir(c.worktree, 0700);
    expect(boardfile_put(c.worktree, &c), "a new worktree takes the copy back");
    char *back = slurp_file(in_tree);
    expect(back && strstr(back, "rewrite the cancel row"),
           "with the plan still in it");
    free(back);

    unlink(in_tree);
    boardfile_keep(&c);
    saved = slurp_file(kept);
    expect(saved && strstr(saved, "rewrite the cancel row"),
           "a card file that is gone leaves the last copy standing");
    free(saved);

    struct board_card other = {0};
    snprintf(other.id, sizeof other.id, "cf02");
    snprintf(other.worktree, sizeof other.worktree, "%s", c.worktree);
    expect(!boardfile_put(other.worktree, &other),
           "a card with no copy kept has nothing to put back");

    boardfile_drop(c.id);
    expect(slurp_file(kept) == NULL, "dropping the card drops the copy");
}

static void test_a_card_carries_a_queue_and_a_history(void)
{
    with_actions();
    steps_for("queued", "worktree, review, audit, merge");

    char id[BOARD_ID_MAX] = {0};
    expect(board_add("a card to queue work onto", "/tmp/repo", id), "capture");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    expect(c && board_stands(c) == BOARD_OPEN, "a fresh card is open");
    board_free(v, n);

    const char *want[] = {"worktree", "merge"};
    expect(board_queued(id, want, 2), "two actions are queued");

    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && c->queue_n == 2 && !strcmp(c->queue[0], "worktree"),
           "in the order they were asked for");
    expect(c && board_stands(c) == BOARD_WORKING, "which sets it working");
    expect(c && !board_ran(c, "worktree"), "and nothing has run yet");
    board_free(v, n);

    expect(board_took(id, "worktree"), "the head of the queue passes");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && board_ran(c, "worktree"), "so it joins the history");
    expect(c && c->queue_n == 1 && !strcmp(c->queue[0], "merge"),
           "and leaves the queue");
    expect(c && board_stands(c) == BOARD_WORKING, "the rest is still working");
    board_free(v, n);

    expect(board_took(id, "worktree"), "an action that passes twice");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && c->done_n == 1, "is in the history once");
    board_free(v, n);

    expect(board_took(id, "merge"), "the last of the queue passes");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && !c->queue_n && c->done_n == 2, "the queue empties");
    expect(c && board_stands(c) == BOARD_REVIEW, "and the card is your turn");
    board_free(v, n);

    expect(board_close(id), "closing it");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && board_stands(c) == BOARD_CLOSED, "stands it done");
    board_free(v, n);

    board_remove(id);
}

static void test_a_card_written_before_the_history_reads_forward(void)
{
    with_actions();
    steps_for("walked", "worktree, review, audit, merge");

    /* a line as an older mux wrote it: a column, a step, and no history */
    FILE *f = fopen(board_path(), "a");
    if (!f) {
        fail("the store takes an old line");
        return;
    }
    fprintf(f, "{\"id\":\"old1\",\"col\":\"audit\",\"kind\":\"walked\","
               "\"title\":\"a card from an older mux\",\"body\":\"b\","
               "\"cwd\":\"/tmp/repo\",\"created\":1,\"updated\":1,"
               "\"log\":[]}\n");
    fclose(f);

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, "old1");
    expect(c != NULL, "the old line still loads");
    expect(c && board_ran(c, "worktree"), "the steps it walked are its history");
    expect(c && board_ran(c, "review"), "all of them");
    expect(c && !board_ran(c, "audit"), "but not the one it stopped on");
    expect(c && !board_ran(c, "merge"), "nor the ones after it");
    expect(c && !c->queue_n, "and nothing is queued on it");
    board_free(v, n);

    board_remove("old1");
}

int main(void)
{
    if (!mkdtemp(home)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("HOME", home, 1);

    test_capture();
    test_ids_are_distinct();
    test_pin();
    test_note_and_move();
    test_update_preserves_created();
    test_update_leaves_others_alone();
    test_remove();
    test_columns();
    test_attempts_reset_when_answered();
    test_a_step_answers_for_itself();
    test_actions_are_what_the_files_say();
    test_a_step_is_a_file();
    test_a_step_that_fails_with_nowhere_to_send_it();
    test_skipping_a_step();

    test_a_planner_works_without_a_worktree();
    test_reply_json();
    test_kinds();
    test_a_named_kind_skips_triage();
    test_projects_block();
    test_a_kind_file_carries_its_approval_prompt();
    test_a_kind_prompt_takes_the_card_id();
    test_archive();
    test_empty_and_missing();
    test_done_lists_newest_first();
    test_worktree_name_is_stable();
    test_auto_pick_roundtrip();
    test_revision_tracks_writes();
    test_the_shipped_kinds_and_roles_are_built_in();
    test_the_card_file_outlives_the_worktree();
    test_a_card_carries_a_queue_and_a_history();
    test_a_card_written_before_the_history_reads_forward();

    cleanup();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("ok\n");
    return failures ? 1 : 0;
}
