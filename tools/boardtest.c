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
#include "boardcfg.h"
#include "boardflow.h"
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
    expect(board_move(id, BOARD_BACKLOG, "triage", "classified as feature"),
           "move with a reason");
    expect(board_move(id, BOARD_DONE, "you", NULL), "move without a reason");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    if (!c) {
        fail("card survives its notes");
        board_free(v, n);
        return;
    }
    expect(c->col == BOARD_DONE, "column follows the last move");
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
    expect(c->col == BOARD_BACKLOG, "column round-trips");
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
        if (!c || c->col != before[i].col ||
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
    expect(where_at.col == BOARD_BACKLOG,
           "a column the board does not know named a step, and lands in backlog");

    for (int i = 0; i < BOARD_COLS; i++) {
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
    action_def("worktree", "tier: high\n", "work the card");
    action_def("test", "tier: high\n", "test the change");
    action_def("review", "tier: high\n", "read the plan");
    action_def("audit", "tier: high\nfail marker: FINDINGS\n", "read the diff");
    action_def("merge", "tier: high\n", "land it");
    defs_use();
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


static void test_actions_are_what_the_files_say(void)
{
    defs_clear();
    action_def("auditor", "tier: high\nfail marker: FINDINGS\n",
             "read the diff");
    action_def("merge", "tier: high\n", "land it");
    defs_use();

    const struct board_action *p = boardcfg_action("auditor");
    expect(p != NULL, "an action is found by the name of its file");
    expect(p && !strcmp(p->tier, "high"), "and carries what the file sets");
    expect(p && !strcmp(p->fail_marker, "FINDINGS"), "including its fail marker");
    expect(p && p->prompt && strstr(p->prompt, "read the diff"),
           "the body of the file is its prompt");
    expect(boardcfg_action("audit") == NULL,
           "and nothing else answers to a name no file has");
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

    expect(board_move(id, BOARD_BACKLOG, "worker", "finished"), "move");
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

static void test_the_shipped_kinds_and_actions_are_built_in(void)
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
    expect(boardcfg_action("leftover") == NULL, "the stale role is gone");

    char why[512];
    expect(!boardcfg_missing(why, sizeof why), "the built-in set is whole");

    const struct board_kind *k = boardcfg_kind("plan");
    expect(k != NULL, "plan is a kind without a file anywhere");
    expect(k && k->steps_n == 1, "walking the one step it names");

    const struct board_action *p = boardcfg_action("plan");
    expect(p && p->where == BOARD_IN_WORKTREE, "the plan action runs in the worktree");
    expect(p && !p->needs_n, "gated on nothing");
    expect(p && p->prompt && strstr(p->prompt, "plan mode"),
           "its prompt is the body of the file it was built from");

    const struct board_action *name = boardcfg_action("triage");
    expect(name && name->on_capture, "naming runs on capture, not on trigger");

    const char *all[BOARD_ACTIONS_MAX];
    int         n = boardcfg_actions(all, BOARD_ACTIONS_MAX);
    expect(n == 3, "every action file is one action");
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
    expect(c && c->col == BOARD_BACKLOG, "the step it named is not a column");
    board_free(v, n);

    board_remove("old1");
}

static void test_an_action_waits_on_what_it_needs(void)
{
    defs_clear();
    action_def("plan", "tier: high\nin: worktree\n", "plan it");
    action_def("implement", "tier: high\nin: worktree\n", "build it");
    action_def("test", "tier: med\nin: worktree\nneeds: implement\n", "test it");
    action_def("merge", "tier: med\nin: repo\nneeds: implement\n", "land it");
    action_def("deploy", "tier: med\nin: repo\nneeds: merge\n", "ship it");
    action_def("name", "tier: low\non: capture\n", "name it");
    defs_use();

    struct board_card c = {0};
    snprintf(c.id, sizeof c.id, "gate");
    snprintf(c.cwd, sizeof c.cwd, "/tmp/repo");

    expect(boardflow_gated(&c, "plan"), "an action needing nothing may run");
    expect(boardflow_gated(&c, "implement"), "and so may another");
    expect(!boardflow_gated(&c, "test"), "one needing implement may not");
    expect(!boardflow_gated(&c, "merge"), "nor may merge");
    expect(!boardflow_gated(&c, "deploy"), "nor deploy, two gates back");
    expect(!boardflow_gated(&c, "name"), "and a capture action is never offered");

    const char *offered[BOARD_ACTIONS_MAX];
    int         n = boardflow_offered(&c, offered, BOARD_ACTIONS_MAX);
    expect(n == 2, "a fresh card offers the two ungated actions");

    snprintf(c.done[c.done_n++], BOARD_ACTION_NAME, "implement");
    expect(boardflow_gated(&c, "test"), "implementing opens test");
    expect(boardflow_gated(&c, "merge"), "and merge");
    expect(!boardflow_gated(&c, "deploy"), "but not deploy, which needs merge");

    snprintf(c.done[c.done_n++], BOARD_ACTION_NAME, "merge");
    expect(boardflow_gated(&c, "deploy"), "merging opens deploy");
    n = boardflow_offered(&c, offered, BOARD_ACTIONS_MAX);
    expect(n == 5, "and every action is offered by then");

    /* where each one runs, which is what moves the session */
    expect(!strcmp(boardflow_cwd(&c, boardcfg_action("implement")), "/tmp/repo"),
           "with no worktree yet, a worktree action runs in the repo");
    snprintf(c.worktree, sizeof c.worktree, "/tmp/repo/.claude/worktrees/gate");
    expect(!strcmp(boardflow_cwd(&c, boardcfg_action("implement")), c.worktree),
           "once there is one, it runs in the worktree");
    expect(!strcmp(boardflow_cwd(&c, boardcfg_action("merge")), "/tmp/repo"),
           "and merge runs in the checkout it came from");

    struct board_card only_repo = {0};
    snprintf(only_repo.queue[only_repo.queue_n++], BOARD_ACTION_NAME, "merge");
    expect(!boardflow_worktree(&only_repo),
           "a card whose queue and history are all repo actions makes no tree");
    snprintf(only_repo.queue[only_repo.queue_n++], BOARD_ACTION_NAME, "test");
    expect(boardflow_worktree(&only_repo),
           "one that queues a worktree action does");

    with_actions();
}

/* the gates a pipeline satisfies for itself */
static void test_a_trigger_queues_a_pipeline(void)
{
    defs_clear();
    action_def("plan", "tier: high\nin: worktree\n", "plan it");
    action_def("implement", "tier: high\nin: worktree\n", "build it");
    action_def("merge", "tier: med\nin: repo\nneeds: implement\n", "land it");
    action_def("deploy", "tier: med\nin: repo\nneeds: merge\n", "ship it");
    action_def("name", "tier: low\non: capture\n", "name it");
    defs_use();

    char id[BOARD_ID_MAX] = {0};
    expect(board_add("trigger a pipeline", "/tmp/repo", id), "capture");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);

    char        why[512] = "";
    const char *one[] = {"merge"};
    expect(c && !boardflow_trigger(c, one, 1, why, sizeof why),
           "an action whose gate is unmet is refused");
    expect(strstr(why, "implement") != NULL, "and says what it is waiting on");

    const char *both[] = {"implement", "merge"};
    expect(c && boardflow_trigger(c, both, 2, why, sizeof why),
           "naming what it needs ahead of it lets it through");
    board_free(v, n);

    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && c->queue_n == 2 && !strcmp(c->queue[1], "merge"),
           "both land on the queue, in order");

    const char *after[] = {"deploy"};
    expect(c && boardflow_trigger(c, after, 1, why, sizeof why),
           "an action gated on one already queued may follow it");
    board_free(v, n);

    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && c->queue_n == 3 && !strcmp(c->queue[0], "implement"),
           "and goes on the end, leaving the queue in front of it alone");

    const char *capture[] = {"name"};
    expect(c && !boardflow_trigger(c, capture, 1, why, sizeof why),
           "a capture action is not one to trigger");
    const char *stranger[] = {"nonsense"};
    expect(c && !boardflow_trigger(c, stranger, 1, why, sizeof why),
           "nor is a name no file has");
    board_free(v, n);

    board_remove(id);
    with_actions();
}

/* a pipeline that fails part-way stops where it is and waits to be told */
static void test_a_stopped_card_waits_on_you(void)
{
    defs_clear();
    action_def("plan", "tier: high\nin: worktree\n", "plan it");
    action_def("implement", "tier: high\nin: worktree\n", "build it");
    action_def("merge", "tier: med\nin: repo\nneeds: implement\n", "land it");
    defs_use();

    char id[BOARD_ID_MAX] = {0};
    expect(board_add("a pipeline that breaks", "/tmp/repo", id), "capture");

    const char *both[] = {"implement", "merge"};
    expect(board_queued(id, both, 2), "two actions are queued");

    /* the head of the queue did not pass */
    expect(board_stopped(id), "the card is stopped on it");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    expect(c && !c->queue_n, "the rest of the pipeline is dropped");
    expect(c && !c->done_n, "nothing joins the history");
    expect(c && board_stands(c) == BOARD_REVIEW,
           "and the card waits on you, with nothing behind it");
    expect(c && !boardflow_gated(c, "merge"),
           "the gate the failed action guards stays shut");
    board_free(v, n);

    char        why[512] = "";
    const char *again[] = {"implement"};
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && boardflow_trigger(c, again, 1, why, sizeof why),
           "triggering it again is the answer");
    board_free(v, n);

    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && board_stands(c) == BOARD_WORKING, "which sets it working again");
    board_free(v, n);

    expect(board_took(id, "implement"), "and this time it passes");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && board_stands(c) == BOARD_REVIEW, "leaving the card in review");
    expect(c && !c->stopped, "no longer stopped, but done with what it was given");
    board_free(v, n);

    board_remove(id);
    with_actions();
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
    test_actions_are_what_the_files_say();

    test_reply_json();
    test_a_named_kind_skips_triage();
    test_projects_block();
    test_archive();
    test_empty_and_missing();
    test_done_lists_newest_first();
    test_worktree_name_is_stable();
    test_auto_pick_roundtrip();
    test_revision_tracks_writes();
    test_the_shipped_kinds_and_actions_are_built_in();
    test_the_card_file_outlives_the_worktree();
    test_a_card_carries_a_queue_and_a_history();
    test_a_card_written_before_the_history_reads_forward();
    test_an_action_waits_on_what_it_needs();
    test_a_trigger_queues_a_pipeline();
    test_a_stopped_card_waits_on_you();

    cleanup();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("ok\n");
    return failures ? 1 : 0;
}
