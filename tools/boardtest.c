#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "board.h"
#include "child.h"
#include "text.h"
#include "boardaudit.h"
#include "boardcfg.h"
#include "boardflow.h"
#include "boardsweep.h"
#include "boardtriage.h"
#include "gitcmd.h"
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
    expect(c->created > 0 && c->updated > 0, "card is stamped");
    board_free(v, n);
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
    expect(board_move(id, BOARD_BACKLOG, "triage", "classified as feature"),
           "move with a reason");
    expect(board_move(id, BOARD_DOING, "you", NULL), "move without a reason");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    if (!c) {
        fail("card survives its notes");
        board_free(v, n);
        return;
    }
    expect(c->col == BOARD_DOING, "column follows the last move");
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
    c->col = BOARD_REVIEW;
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
    expect(c->col == BOARD_REVIEW, "column round-trips");
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
        if (!c || c->col != before[i].col || strcmp(c->title, before[i].title) ||
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
    expect(board_col_from_name("merging") == BOARD_MERGING, "column by name");
    expect(board_col_from_name("unclear") == BOARD_UNCLEAR, "unclear by name");
    expect(board_col_from_name("nonsense") == BOARD_NEW, "unknown falls back to new");
    expect(!strcmp(board_col_name(BOARD_REVIEW), "review"), "column to name");

    for (int i = 0; i < BOARD_COLS; i++)
        if ((int)board_col_from_name(board_col_name((enum board_col)i)) != i) {
            fail("every column round-trips");
            break;
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

    board_move(id, BOARD_UNCLEAR, "triage", "which thing?");
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

    board_move(id, BOARD_UNCLEAR, "triage", "still cannot tell");
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
    expect(board_move(fresh, BOARD_DONE, "you", NULL), "done");
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

static void test_audit_verdict(void)
{
    char id[BOARD_ID_MAX] = {0};
    expect(board_add("a card to audit", "/tmp/repo", id), "capture");
    expect(board_move(id, BOARD_AUDIT, "you", NULL), "into audit");

    expect(!boardaudit_finished("", "{}"), "a verdict needs a card");

    expect(boardaudit_finished(id, "{\"clean\":false,"
                                   "\"findings\":[\"src/a.c: leaks the buffer\"]}"),
           "a verdict is taken");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    if (!c) {
        fail("card survives the audit");
        board_free(v, n);
        return;
    }
    expect(c->col == BOARD_DOING, "findings send it back to be worked on");
    int said = 0;
    for (int i = 0; i < c->log_n; i++)
        if (!strcmp(c->log[i].who, "audit") && strstr(c->log[i].text, "leaks"))
            said = 1;
    expect(said, "the finding is on the card");
    board_free(v, n);

    expect(board_move(id, BOARD_AUDIT, "you", NULL), "back into audit");
    expect(boardaudit_finished(id, "{\"clean\":true,\"findings\":[]}"), "clean verdict");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && c->col == BOARD_MERGING, "clean sends it on to land");
    board_free(v, n);

    expect(board_move(id, BOARD_AUDIT, "you", NULL), "into audit once more");
    expect(boardaudit_finished(id, "{\"clean\":false,\"findings\":["
                                   "{\"file\":\"src/b.c\",\"finding\":\"restates the code\"}]}"),
           "an object verdict is taken");
    n = board_load(&v);
    c = board_find(v, n, id);
    said = 0;
    if (c)
        for (int i = 0; i < c->log_n; i++)
            if (!strcmp(c->log[i].who, "audit") &&
                strstr(c->log[i].text, "src/b.c") &&
                strstr(c->log[i].text, "restates the code"))
                said = 1;
    expect(said, "the finding reaches the card whatever shape it came in");
    expect(c && c->col == BOARD_DOING, "and still sends it back");
    board_free(v, n);

    expect(board_move(id, BOARD_AUDIT, "you", NULL), "into audit again");
    expect(boardaudit_finished(id, "the model wandered off"), "unparsable verdict");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && c->col == BOARD_MERGING, "an unreadable audit does not hold it");
    board_free(v, n);

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

static void test_kinds(void)
{
    expect(boardcfg_kind("bug") != NULL, "a configured kind is found");
    expect(boardcfg_kind("nonsense") == NULL, "an unconfigured one is not");
    expect(boardcfg_kind("") == NULL, "nor is no kind at all");

    expect(boardcfg_priority("bug") > boardcfg_priority("feature"),
           "a bug comes before a feature");
    expect(boardcfg_priority("feature") > boardcfg_priority("chore"),
           "a feature comes before a chore");
    expect(boardcfg_priority("nonsense") == 0, "an unknown kind is worth nothing");

    expect(boardcfg_kind_takes("bug", BOARD_STEP_WORKTREE), "work gets a worktree");
    expect(boardcfg_kind_takes("bug", BOARD_STEP_MERGE), "and lands through the queue");
    expect(!boardcfg_kind_takes("reference", BOARD_STEP_WORKTREE),
           "a note to file gets neither");
    expect(!boardcfg_kind_takes("reference", BOARD_STEP_REVIEW), "nor a review");
    expect(boardcfg_kind_takes("nonsense", BOARD_STEP_MERGE),
           "an unknown kind takes every step, which is the careful way round");

    expect(boardflow_from("bug", BOARD_STEP_REVIEW, 1) == BOARD_REVIEW,
           "work stops for a person first");
    expect(boardflow_from("bug", BOARD_STEP_AUDIT, 1) == BOARD_AUDIT,
           "then for an audit when the diff is worth it");
    expect(boardflow_from("bug", BOARD_STEP_AUDIT, 0) == BOARD_MERGING,
           "and straight to the queue when it is not");
    expect(boardflow_from("bug", BOARD_STEP_MERGE, 0) == BOARD_MERGING,
           "the queue is the last of it");
    expect(boardflow_from("reference", BOARD_STEP_REVIEW, 1) == BOARD_DONE,
           "a filed note is done when the worker stops");

    char block[4096];
    boardcfg_kinds_block(block, sizeof block);
    expect(strstr(block, "todo") && strstr(block, "chore"), "every kind is named");
    expect(strstr(block, "something that exists and is wrong") != NULL,
           "and each says what it means");
}

static int sweep_mark_count(const char *cwd)
{
    struct board_card *v = NULL;
    int                n = board_load(&v), marked = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(v[i].cwd, cwd))
            continue;
        for (int j = 0; j < v[i].log_n; j++)
            marked += !strcmp(v[i].log[j].who, "sweep") &&
                      !strcmp(v[i].log[j].text, "swept");
    }
    board_free(v, n);
    return marked;
}

static int sweep_due(char *cwd, size_t size)
{
    struct board_card *v = NULL;
    int                n = board_load(&v);
    int                due = boardsweep_due(v, n, cwd, size);
    board_free(v, n);
    return due;
}

static int card_titled(const char *text, const char *cwd)
{
    struct board_card *v = NULL;
    int                n = board_load(&v), found = 0;
    for (int i = 0; i < n; i++)
        found += strstr(v[i].title, text) != NULL && !strcmp(v[i].cwd, cwd);
    board_free(v, n);
    return found;
}

static void land_cards(const char *cwd, int count, const char *what)
{
    for (int i = 0; i < count; i++) {
        char id[BOARD_ID_MAX] = {0}, text[64];
        snprintf(text, sizeof text, "%s %d", what, i);
        expect(board_add(text, cwd, id), "capture");
        expect(board_move(id, BOARD_DONE, "board", NULL), "landed");
    }
}

static enum board_col col_of(const char *id)
{
    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    enum board_col     col = c ? c->col : BOARD_COLS;
    board_free(v, n);
    return col;
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

static void role_add(struct board_cfg *cfg, const char *name, const char *does,
                     const char *prompt)
{
    struct board_profile *r = &cfg->roles[cfg->roles_n++];
    memset(r, 0, sizeof *r);
    snprintf(r->name, sizeof r->name, "%s", name);
    snprintf(r->does, sizeof r->does, "%s", does);
    snprintf(r->tier, sizeof r->tier, "high");
    snprintf(r->step, sizeof r->step, "%s", does);
    r->skippable = 1;
    r->prompt = strdup(prompt);
}

static void test_roles_are_what_the_files_say(void)
{
    struct board_cfg *cfg = boardcfg_copy();
    cfg->roles_n = 0;
    role_add(cfg, "auditor", "audit", "read the diff");
    role_add(cfg, "merge", "merge", "land it");
    expect(boardcfg_set(cfg), "the roles are written out");
    boardcfg_free(cfg);

    boardcfg_reload();

    const struct board_profile *p = boardcfg_doing("audit");
    expect(p != NULL, "a role is found by the job its file says it does");
    expect(p && !strcmp(p->name, "auditor"),
           "and the file it came from names it, whatever the job");
    expect(p && p->prompt && strstr(p->prompt, "read the diff"),
           "the body of the file is its prompt");
    expect(boardcfg_doing("triage") == NULL, "a job no role does has no role");
    expect(boardcfg_for_step(BOARD_STEP_AUDIT) == p,
           "the step it stands in is its own");
}

static void test_skipping_a_step(void)
{
    char id[BOARD_ID_MAX] = {0};
    expect(board_add("a card to skip past", "/tmp/repo", id), "capture");
    expect(board_move(id, BOARD_AUDIT, "you", NULL), "into audit");

    expect(boardflow_step_at(BOARD_AUDIT) == BOARD_STEP_AUDIT,
           "a card in audit waits on the audit step");
    expect(boardflow_step_at(BOARD_BACKLOG) == BOARD_STEPS,
           "a card waiting for a worker waits on no step");

    struct board_cfg *cfg = boardcfg_copy();
    for (int i = 0; i < cfg->roles_n; i++)
        cfg->roles[i].skippable = 0;
    boardcfg_set(cfg);

    expect(!boardflow_skip(NULL), "a missing card is not skipped");
    expect(!skip_card(id), "an unskippable step holds the card");

    for (int i = 0; i < cfg->roles_n; i++)
        cfg->roles[i].skippable = 1;
    boardcfg_set(cfg);
    boardcfg_free(cfg);

    expect(skip_card(id), "a skippable step is skipped");
    expect(col_of(id) == BOARD_MERGING, "skipping the audit sends it on to land");
    expect(skip_card(id), "and the next step skips too");
    expect(col_of(id) == BOARD_DONE, "skipping the merge is the end of it");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    int                said = 0;
    if (c)
        for (int i = 0; i < c->log_n; i++)
            if (strstr(c->log[i].text, "audit skipped"))
                said = 1;
    expect(said, "the skip says which step it was");
    board_free(v, n);

    board_remove(id);
}

static void test_sweep_counts_landed_cards(void)
{
    const char *cwd = "/tmp/sweeprepo";
    int         every = boardcfg()->sweep_every;
    char        due[4096];

    land_cards(cwd, every - 1, "landed card");
    expect(!sweep_due(due, sizeof due), "one short of the interval is not due");

    land_cards(cwd, 1, "the card that trips it");
    expect(sweep_due(due, sizeof due) && !strcmp(due, cwd),
           "the interval makes the repo due");

    char id[BOARD_ID_MAX] = {0};
    expect(boardsweep_open(cwd, id, sizeof id), "a sweep card is minted");
    expect(sweep_mark_count(cwd) == every,
           "every landed card is marked, so the count is on the board");
    expect(!sweep_due(due, sizeof due), "and a restart does not sweep them twice");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    expect(c && c->col == BOARD_DOING, "the sweep card starts in doing");
    expect(c && boardsweep_is(c), "and is a sweep");
    board_free(v, n);

    expect(!boardsweep_finished("", "{}"), "a sweep needs a card");
    expect(boardsweep_finished(id, "{\"cards\":[\"two ways to spell a worktree\"]}"),
           "the sweep's cards are taken");

    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && c->col == BOARD_REVIEW, "what it raised goes to review");
    expect(c && boardsweep_proposed(c) == 1, "with the proposal on it");
    expect(!card_titled("two ways to spell a worktree", cwd),
           "and nothing on the board yet");

    expect(boardsweep_approve(c), "approving files them");
    board_free(v, n);

    expect(card_titled("two ways to spell a worktree", cwd) == 1,
           "the sweep files what was approved");

    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && c->col == BOARD_DONE, "and the sweep card is done");
    board_free(v, n);
}

static void test_sweep_review_can_refuse(void)
{
    const char *cwd = "/tmp/refusedsweep";
    char        due[4096], id[BOARD_ID_MAX] = {0};

    land_cards(cwd, boardcfg()->sweep_every, "dropped card");
    expect(sweep_due(due, sizeof due), "the interval makes the repo due");
    expect(boardsweep_open(cwd, id, sizeof id), "a sweep card is minted");
    expect(boardsweep_finished(id, "{\"cards\":[\"keep this one\",\"drop this one\"]}"),
           "the sweep's cards are taken");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    expect(c && boardsweep_proposed(c) == 2, "both wait on the card");
    expect(boardsweep_reject(c), "rejecting drops them");
    board_free(v, n);

    expect(!card_titled("keep this one", cwd) && !card_titled("drop this one", cwd),
           "nothing was filed");

    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && c->col == BOARD_DONE, "and the sweep card is done");
    board_free(v, n);
}

static void test_sweep_with_nothing_to_raise(void)
{
    const char *cwd = "/tmp/quietsweep";
    char        due[4096], id[BOARD_ID_MAX] = {0};

    land_cards(cwd, boardcfg()->sweep_every, "quiet card");
    expect(sweep_due(due, sizeof due), "the interval makes the repo due");
    expect(boardsweep_open(cwd, id, sizeof id), "a sweep card is minted");
    expect(boardsweep_finished(id, "{\"cards\":[]}"), "an empty sweep is taken");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    expect(c && c->col == BOARD_DONE, "a sweep with nothing to raise skips review");
    board_free(v, n);
}

static int make_repo_with_worktree(char *root, size_t rsize, char *tree, size_t tsize)
{
    snprintf(root, rsize, "%s/repo", home);
    snprintf(tree, tsize, "%s/repo/wt", home);

    char cmd[2048];
    snprintf(cmd, sizeof cmd,
             "git init -q %s >/dev/null 2>&1 && "
             "git -C %s -c user.email=t@t -c user.name=t commit -q --allow-empty "
             "-m base >/dev/null 2>&1 && "
             "git -C %s worktree add -q -b wt %s >/dev/null 2>&1",
             root, root, root, tree);
    if (system(cmd) != 0)
        return 0;

    char real[4096];
    if (realpath(root, real))
        snprintf(root, rsize, "%s", real);
    return 1;
}

static void test_sweep_files_against_the_repo(void)
{
    char root[4096], tree[4096];
    if (!make_repo_with_worktree(root, sizeof root, tree, sizeof tree)) {
        fprintf(stderr, "skipping worktree sweep test: no git\n");
        return;
    }

    land_cards(tree, boardcfg()->sweep_every, "worktree card");

    char id[BOARD_ID_MAX] = {0};
    expect(boardsweep_open(tree, id, sizeof id), "a sweep card is minted");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    expect(c && !strcmp(c->cwd, root), "a sweep sits on the main repo");
    board_free(v, n);

    expect(boardsweep_finished(id, "{\"cards\":[\"a finding from the worktree\"]}"),
           "the sweep's cards are taken");

    n = board_load(&v);
    c = board_find(v, n, id);
    expect(boardsweep_approve(c), "approving files them");
    board_free(v, n);

    expect(card_titled("a finding from the worktree", root) == 1,
           "a finding is filed against the main repo");
    expect(!card_titled("a finding from the worktree", tree),
           "and not against the worktree it was found in");
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

    expect(board_move(id, BOARD_REVIEW, "worker", "finished"), "move");
    expect(board_revision() != added, "a move moves the revision");

    board_remove(id);
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
    test_note_and_move();
    test_update_preserves_created();
    test_update_leaves_others_alone();
    test_remove();
    test_columns();
    test_attempts_reset_when_answered();
    test_audit_verdict();
    test_roles_are_what_the_files_say();
    test_skipping_a_step();
    test_sweep_counts_landed_cards();
    test_sweep_review_can_refuse();
    test_sweep_with_nothing_to_raise();
    test_sweep_files_against_the_repo();
    test_reply_json();
    test_kinds();
    test_archive();
    test_empty_and_missing();
    test_done_lists_newest_first();
    test_worktree_name_is_stable();
    test_auto_pick_roundtrip();
    test_revision_tracks_writes();

    cleanup();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("ok\n");
    return failures ? 1 : 0;
}
