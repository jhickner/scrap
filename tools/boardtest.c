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
#include "boardname.h"
#include "boardstep.h"
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
    expect(!c->closed, "capture lands open");
    expect(!strcmp(c->title, "fix the tab strip"), "title is the first line");
    expect(strstr(c->body, "80 columns") != NULL, "body keeps the rest");
    expect(!strcmp(c->cwd, "/tmp/repo"), "cwd is recorded");
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

static void test_note_and_close(void)
{
    char id[BOARD_ID_MAX] = {0};
    expect(board_add("worktree cleanup after landing", "/tmp/repo", id), "capture");

    expect(board_note(id, "name", "worktree cleanup"), "note appends");
    expect(board_note(id, "you", "after landing, not before"), "and again");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    if (c) {
        snprintf(c->session, sizeof c->session, "%s", "sess-1");
        expect(board_update(c), "the card records a session");
    }
    board_free(v, n);

    expect(board_close(id), "closing the card");

    n = board_load(&v);
    c = board_find(v, n, id);
    if (!c) {
        fail("card survives its notes");
        board_free(v, n);
        return;
    }
    expect(c->closed, "closed is the one bit it carries");
    expect(board_stands(c) == BOARD_CLOSED, "and it stands closed");
    expect(c->log_n == 2, "both notes are on it");
    expect(!strcmp(c->log[0].who, "name"), "note records who");
    expect(strstr(c->log[1].text, "landing") != NULL, "and what was said");
    expect(c->log[0].ts > 0, "note is stamped");
    expect(!c->session[0], "and it lets go of its session");
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
    snprintf(c->model, sizeof c->model, "opus");
    snprintf(c->base, sizeof c->base, "b519936");
    c->priority = 2;
    c->cost_usd = 0.42;
    c->tokens_in = 12345;
    c->tokens_out = 678;
    c->closed = 1;
    expect(board_update(c), "update writes back");
    board_free(v, n);

    n = board_load(&v);
    c = board_find(v, n, id);
    if (!c) {
        fail("updated card is still there");
        board_free(v, n);
        return;
    }
    expect(!strcmp(c->model, "opus"), "model round-trips");
    expect(!strcmp(c->base, "b519936"), "base sha round-trips");
    expect(c->priority == 2, "priority round-trips");
    expect(c->cost_usd > 0.41 && c->cost_usd < 0.43, "cost round-trips");
    expect(c->tokens_in == 12345 && c->tokens_out == 678, "tokens round-trip");
    expect(c->closed, "closed round-trips");
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
        if (!c || c->closed != before[i].closed ||
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

/* the columns a card used to sit in are gone; only closed survived them */
static void test_an_old_column_reads_as_closed_or_not(void)
{
    FILE *f = fopen(board_path(), "a");
    if (!f) {
        fail("the store takes an old line");
        return;
    }
    const char *was[] = {"new", "unclear", "backlog", "worktree", "done"};
    for (size_t i = 0; i < sizeof was / sizeof *was; i++)
        fprintf(f, "{\"id\":\"was%zu\",\"col\":\"%s\",\"title\":\"old\","
                   "\"body\":\"old\",\"cwd\":\"/tmp/repo\","
                   "\"created\":1,\"updated\":1,\"log\":[]}\n", i, was[i]);
    fclose(f);

    struct board_card *v = NULL;
    int                n = board_load(&v);
    for (size_t i = 0; i < sizeof was / sizeof *was; i++) {
        char id[BOARD_ID_MAX];
        snprintf(id, sizeof id, "was%zu", i);
        struct board_card *c = board_find(v, n, id);
        int want = !strcmp(was[i], "done");
        if (!c || c->closed != want) {
            fail("every old column reads as closed or not");
            break;
        }
        if (!want && board_stands(c) != BOARD_OPEN) {
            fail("and a card that was not done is open");
            break;
        }
    }
    board_free(v, n);

    for (size_t i = 0; i < sizeof was / sizeof *was; i++) {
        char id[BOARD_ID_MAX];
        snprintf(id, sizeof id, "was%zu", i);
        board_remove(id);
    }
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
    expect(board_close(fresh), "done");
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

static void pipeline_def(const char *name, const char *actions)
{
    char path[128], front[256];
    snprintf(path, sizeof path, "pipelines/%s.md", name);
    snprintf(front, sizeof front, "actions: %s\n", actions);
    def_add(path, front, "");
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

    older.closed = newer.closed = 1;
    snprintf(older.id, sizeof older.id, "old");
    snprintf(newer.id, sizeof newer.id, "new");
    older.created = 100;
    newer.created = 200;
    older.updated = 1000;
    newer.updated = 2000;
    older.priority = 9;
    newer.priority = 0;
    expect(board_cmp(&newer, &older) < 0, "newer done card sorts first");
    expect(board_cmp(&older, &newer) > 0, "older done card sorts second");


    snprintf(high.id, sizeof high.id, "hi");
    snprintf(low.id, sizeof low.id, "lo");
    high.priority = 2;
    low.priority = 1;
    high.created = 300;
    low.created = 100;
    expect(board_cmp(&high, &low) < 0, "higher priority still leads the open cards");
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

    expect(board_close(id), "close");
    expect(board_revision() != added, "closing moves the revision");

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

static void test_the_shipped_actions_are_built_in(void)
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
    expect(!has_dir("board/kinds"), "nor a kinds dir of its own");
    expect(!has_dir("board/pipelines"), "nor a pipelines dir");
    expect(boardcfg_action("leftover") == NULL, "the stale role is gone");

    char why[512];
    expect(!boardcfg_missing(why, sizeof why), "the built-in set is whole");

    const struct board_action *p = boardcfg_action("plan");
    expect(p && p->where == BOARD_IN_WORKTREE, "the plan action runs in the worktree");
    expect(p && !p->needs_n, "gated on nothing");
    expect(p && p->prompt && strstr(p->prompt, "plan mode"),
           "its prompt is the body of the file it was built from");

    expect(p && !p->commits, "and is not asked to commit");

    const struct board_action *build_it = boardcfg_action("implement");
    expect(build_it && build_it->commits, "implement is asked to commit");

    const struct board_action *name = boardcfg_action("name");
    expect(name && name->on_capture, "naming runs on capture, not on trigger");

    const struct board_action *merge = boardcfg_action("merge");
    expect(merge && merge->where == BOARD_IN_REPO, "merge runs in the checkout");
    expect(merge && merge->needs_n == 1 && !strcmp(merge->needs[0], "implement"),
           "and is gated on implement");
    expect(merge && !strcmp(merge->fail_marker, "MERGE BLOCKED"),
           "and says so when it cannot land");
    expect(merge && merge->closes, "and closes the card when it lands");

    const char *pipelines[BOARD_PIPELINES_MAX];
    expect(!boardcfg_pipelines(pipelines, BOARD_PIPELINES_MAX),
           "no pipeline ships with the binary");

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

static void test_archiving_drops_the_card_file(void)
{
    plant_stale("ag02");

    char kept[4300];
    expect(boardfile_kept("ag02", kept, sizeof kept), "the copy has a path");
    put_file(kept, "# an archived card\n");

    expect(board_archive(1) == 1, "the stale card is archived");
    expect(slurp_file(kept) == NULL, "and its card file goes with it");
}

static void test_a_card_carries_a_queue_and_a_history(void)
{
    with_actions();

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

    const char *twice[] = {"worktree", "worktree"};
    expect(board_queued(id, twice, 2), "a queue may name an action twice");
    expect(board_took(id, "worktree"), "the head of it passes");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && c->queue_n == 1 && !strcmp(c->queue[0], "worktree"),
           "and the second turn on it is still to come");
    board_free(v, n);
    expect(board_took(id, "worktree"), "which then passes too");

    expect(board_close(id), "closing it");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && board_stands(c) == BOARD_CLOSED, "stands it done");
    board_free(v, n);

    const char *after[] = {"merge"};
    expect(board_queued(id, after, 1), "queueing on a closed card");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && board_stands(c) == BOARD_WORKING, "sets it working again");
    board_free(v, n);

    expect(board_close(id), "closing it once more");
    expect(board_stopped(id), "saying what is wrong with it");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && board_stands(c) == BOARD_REVIEW, "takes it back to your turn");
    board_free(v, n);

    board_remove(id);
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

    n = boardflow_offered(&c, offered, BOARD_ACTIONS_MAX);
    expect(n == 4 && !strcmp(offered[0], "test") && !strcmp(offered[1], "merge"),
           "and offers what implement opened ahead of what needed nothing");

    snprintf(c.done[c.done_n++], BOARD_ACTION_NAME, "merge");
    expect(boardflow_gated(&c, "deploy"), "merging opens deploy");
    n = boardflow_offered(&c, offered, BOARD_ACTIONS_MAX);
    expect(n == 5, "and every action is offered by then");
    expect(!strcmp(offered[0], "deploy"), "deploy, two gates in, leads them");

    /* where each one runs, which is what moves the session */
    expect(!strcmp(boardflow_cwd(&c, boardcfg_action("implement")), "/tmp/repo"),
           "with no worktree yet, a worktree action runs in the repo");
    snprintf(c.worktree, sizeof c.worktree, "/tmp/repo/.claude/worktrees/gate");
    expect(!strcmp(boardflow_cwd(&c, boardcfg_action("implement")), c.worktree),
           "once there is one, it runs in the worktree");
    expect(!strcmp(boardflow_cwd(&c, boardcfg_action("merge")), "/tmp/repo"),
           "and merge runs in the checkout it came from");

    expect(boardflow_exclusive(boardcfg_action("merge")),
           "merge takes the checkout to itself");
    expect(!boardflow_exclusive(boardcfg_action("implement")),
           "an action in a worktree of its own does not");

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

/* an action can say it finishes the card, and closes it on the turn it passes */
static void test_an_action_closes_the_card(void)
{
    defs_clear();
    action_def("implement", "tier: high\nin: worktree\n", "build it");
    action_def("merge", "tier: med\nin: repo\nneeds: implement\ncloses: yes\n",
               "land it");
    action_def("deploy", "tier: med\nin: repo\nneeds: merge\n", "ship it");
    defs_use();

    const struct board_action *merge = boardcfg_action("merge");
    const struct board_action *build_it = boardcfg_action("implement");
    expect(merge && merge->closes, "the file says merge closes the card");
    expect(build_it && !build_it->closes, "implement does not");

    char id[BOARD_ID_MAX] = {0};
    expect(board_add("work to land", "/tmp/repo", id), "capture");
    expect(board_took(id, "implement"), "implement passed");

    const char *just_merge[] = {"merge"};
    expect(board_queued(id, just_merge, 1), "merge is queued on its own");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    expect(c && boardflow_closes(c, merge), "so passing it would close the card");
    expect(c && !boardflow_closes(c, build_it),
           "an action that does not say so would not");
    board_free(v, n);

    const char *then_deploy[] = {"merge", "deploy"};
    expect(board_queued(id, then_deploy, 2), "with deploy queued behind it");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && !boardflow_closes(c, merge),
           "merge leaves the card open for what was asked after it");
    board_free(v, n);

    board_remove(id);
    with_actions();
}

/* a pipeline is a name for a run of actions, gated action by action */
static void test_a_pipeline_stands_for_its_actions(void)
{
    defs_clear();
    action_def("plan", "tier: high\nin: worktree\n", "plan it");
    action_def("implement", "tier: high\nin: worktree\n", "build it");
    action_def("merge", "tier: med\nin: repo\nneeds: implement\n", "land it");
    action_def("deploy", "tier: med\nin: repo\nneeds: merge\n", "ship it");
    pipeline_def("build", "plan, implement");
    pipeline_def("ship", "merge, deploy");
    defs_use();

    const struct board_pipeline *p = boardcfg_pipeline("build");
    expect(p && p->actions_n == 2, "a pipeline file lists its actions");
    expect(p && !strcmp(p->actions[0], "plan"), "in the order it names them");
    expect(boardcfg_pipeline("plan") == NULL, "an action is not a pipeline");

    char why[512];
    expect(!boardcfg_missing(why, sizeof why), "the set hangs together");

    char id[BOARD_ID_MAX] = {0};
    expect(board_add("run a pipeline by name", "/tmp/repo", id), "capture");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);

    const char *offered[BOARD_ACTIONS_MAX];
    int         k = boardflow_offered(c, offered, BOARD_ACTIONS_MAX);
    int         has_build = 0, has_ship = 0;
    for (int i = 0; i < k; i++) {
        has_build |= !strcmp(offered[i], "build");
        has_ship |= !strcmp(offered[i], "ship");
    }
    expect(has_build, "a pipeline whose actions all clear is offered");
    expect(!has_ship, "one whose first action is gated is not");

    const char *one[] = {"build"};
    expect(c && boardflow_trigger(c, one, 1, why, sizeof why),
           "triggering it by name is taken");
    board_free(v, n);

    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && c->queue_n == 2, "and queues what it stands for");
    expect(c && !strcmp(c->queue[0], "plan") && !strcmp(c->queue[1], "implement"),
           "in order, as if they had been typed out");

    /* ship needs merge, which needs implement, which build has queued */
    const char *after[] = {"ship"};
    expect(c && boardflow_trigger(c, after, 1, why, sizeof why),
           "a pipeline gated on one already queued may follow it");
    board_free(v, n);

    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && c->queue_n == 4 && !strcmp(c->queue[3], "deploy"),
           "and flattens onto the end");
    board_free(v, n);

    board_remove(id);
    with_actions();
}

/* nothing triggers naming and nothing waits on it: the reply lands on the card
   the key names, and a reply with no name in it leaves the title alone */
static void test_the_merge_turn_carries_its_commands(void)
{
    char root[4096];
    if (!make_repo(root, sizeof root)) {
        fprintf(stderr, "skipping merge turn test: no git\n");
        return;
    }

    boardcfg_defaults(NULL, 0);

    char id[BOARD_ID_MAX] = {0};
    expect(board_add("work to land", root, id), "capture");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    if (!c) {
        fail("the card is there");
        board_free(v, n);
        return;
    }
    snprintf(c->worktree, sizeof c->worktree, "%s/.claude/worktrees/%s", root,
             id);
    snprintf(c->base, sizeof c->base, "abc1234");

    char branch[160];
    board_branch(id, branch, sizeof branch);

    char *turn = boardstep_prompt(c, boardcfg_action("merge"));
    expect(turn != NULL, "the merge turn is built");
    if (turn) {
        char want[8600];
        snprintf(want, sizeof want, "git -C %s merge --ff-only %s", root,
                 branch);
        expect(strstr(turn, want) != NULL,
               "and carries the merge command with the paths in it");

        char onto[128] = "";
        gitcmd_line(root, "rev-parse --abbrev-ref HEAD", onto, sizeof onto);
        snprintf(want, sizeof want, "git -C %s rebase %s", c->worktree, onto);
        expect(strstr(turn, want) != NULL,
               "and the rebase command, onto the checkout's branch");
        expect(strstr(turn, "{repo}") == NULL, "no placeholder is left behind");
        expect(strstr(turn, c->base) != NULL,
               "and the turn still says where the branch came from");
        free(turn);
    }

    char *plan = boardstep_prompt(c, boardcfg_action("plan"));
    expect(plan && !strstr(plan, "You are in the checkout"),
           "an action in the worktree is not told about the checkout");
    free(plan);

    board_free(v, n);
}

static void test_a_name_lands_on_the_card(void)
{
    char id[BOARD_ID_MAX] = {0};
    expect(board_add("the strip of tabs along the top wraps as soon as there "
                     "are more of them than fit in 80 columns",
                     "/tmp/repo", id),
           "capture");

    char key[CHILD_KEY_MAX];
    snprintf(key, sizeof key, "name:%s", id);

    expect(!boardname_take("card:something", "{\"title\":\"not a name\"}"),
           "a reply from another turn is not a name");
    expect(boardname_take(key, "{\"title\":\"tab strip wraps at 80\"}"),
           "a naming reply is taken");

    struct board_card *v = NULL;
    int                n = board_load(&v);
    struct board_card *c = board_find(v, n, id);
    expect(c && !strcmp(c->title, "tab strip wraps at 80"),
           "and stands as the title of the card");
    board_free(v, n);

    expect(boardname_take(key, "sorry, I could not"), "a reply with no JSON is taken");
    n = board_load(&v);
    c = board_find(v, n, id);
    expect(c && !strcmp(c->title, "tab strip wraps at 80"),
           "and leaves the title the card already had");
    board_free(v, n);
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
    test_note_and_close();
    test_update_preserves_created();
    test_update_leaves_others_alone();
    test_remove();
    test_an_old_column_reads_as_closed_or_not();
    test_actions_are_what_the_files_say();

    test_reply_json();
    test_archive();
    test_empty_and_missing();
    test_done_lists_newest_first();
    test_worktree_name_is_stable();
    test_auto_pick_roundtrip();
    test_revision_tracks_writes();
    test_the_shipped_actions_are_built_in();
    test_the_card_file_outlives_the_worktree();
    test_archiving_drops_the_card_file();
    test_a_card_carries_a_queue_and_a_history();
    test_an_action_waits_on_what_it_needs();
    test_a_trigger_queues_a_pipeline();
    test_a_stopped_card_waits_on_you();
    test_an_action_closes_the_card();
    test_a_pipeline_stands_for_its_actions();
    test_a_name_lands_on_the_card();
    test_the_merge_turn_carries_its_commands();

    cleanup();
    if (failures)
        fprintf(stderr, "%d failure(s)\n", failures);
    else
        printf("ok\n");
    return failures ? 1 : 0;
}
