#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "board.h"
#include "text.h"
#include "boardaudit.h"
#include "boardcfg.h"
#include "boardflow.h"
#include "boardtriage.h"
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

    char key[64];
    snprintf(key, sizeof key, "audit:%s", id);

    expect(!boardaudit_take("merge:x", "{}"), "another job's key is not ours");

    expect(boardaudit_take(key, "{\"clean\":false,"
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
    expect(boardaudit_take(key, "{\"clean\":true,\"findings\":[]}"), "clean verdict");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && c->col == BOARD_MERGING, "clean sends it on to land");
    board_free(v, n);

    expect(board_move(id, BOARD_AUDIT, "you", NULL), "into audit once more");
    expect(boardaudit_take(key, "{\"clean\":false,\"findings\":["
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
    expect(boardaudit_take(key, "the model wandered off"), "unparsable verdict");
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
    test_reply_json();
    test_kinds();
    test_archive();
    test_empty_and_missing();

    cleanup();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("ok\n");
    return failures ? 1 : 0;
}
