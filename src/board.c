#include "board.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "boardlog.h"
#include "text.h"
#include "vendor/cJSON.h"

#define BOARD_MAX_BYTES (1u << 24)
#define BOARD_PATH_MAX  4300

static const char *const COL_NAMES[BOARD_COLS] = {
    "new", "unclear", "backlog", "active", "review", "audit", "merge", "done",
};

static const struct {
    const char    *was;
    enum board_col is;
} COL_WAS[] = {
    {"doing", BOARD_DOING},
    {"merging", BOARD_MERGING},
};

const char *board_col_name(enum board_col col)
{
    if (col < 0 || col >= BOARD_COLS)
        return COL_NAMES[BOARD_NEW];
    return COL_NAMES[col];
}

enum board_col board_col_from_name(const char *name)
{
    if (name) {
        for (int i = 0; i < BOARD_COLS; i++)
            if (!strcmp(name, COL_NAMES[i]))
                return (enum board_col)i;
        for (size_t i = 0; i < sizeof COL_WAS / sizeof *COL_WAS; i++)
            if (!strcmp(name, COL_WAS[i].was))
                return COL_WAS[i].is;
    }
    return BOARD_NEW;
}

const char *board_path(void)
{
    static char path[4200];
    if (!path[0] && !path_config_file(path, sizeof path, "board.jsonl"))
        snprintf(path, sizeof path, "/tmp/board.jsonl");
    return path;
}

static int sidecar_path(char *out, size_t n, const char *suffix)
{
    int k = snprintf(out, n, "%s%s", board_path(), suffix);
    return k > 0 && (size_t)k < n;
}

static int store_lock(int op)
{
    char lp[BOARD_PATH_MAX];
    if (!sidecar_path(lp, sizeof lp, ".lock"))
        return -1;
    int fd = open(lp, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    if (flock(fd, op) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void store_unlock(int fd)
{
    if (fd >= 0) {
        flock(fd, LOCK_UN);
        close(fd);
    }
}

static char *dup_or_empty(const char *s)
{
    char *out = strdup(s ? s : "");
    return out ? out : NULL;
}

static void set_str(char *dst, size_t n, const char *src)
{
    snprintf(dst, n, "%s", src ? src : "");
}

static void card_wipe(struct board_card *c)
{
    for (int i = 0; i < c->log_n; i++)
        free(c->log[i].text);
    free(c->log);
    free(c->body);
    c->log = NULL;
    c->log_n = 0;
    c->body = NULL;
}

void board_free(struct board_card *cards, int n)
{
    if (!cards)
        return;
    for (int i = 0; i < n; i++)
        card_wipe(&cards[i]);
    free(cards);
}

struct board_card *board_find(struct board_card *cards, int n, const char *id)
{
    if (!cards || !id)
        return NULL;
    for (int i = 0; i < n; i++)
        if (!strcmp(cards[i].id, id))
            return &cards[i];
    return NULL;
}

static int card_adopt(struct board_card *dst, const struct board_card *src)
{
    time_t created = dst->created;
    card_wipe(dst);

    *dst = *src;
    dst->created = created;
    dst->body = dup_or_empty(src->body);
    dst->log = NULL;
    dst->log_n = 0;

    if (src->log_n > 0) {
        dst->log = calloc((size_t)src->log_n, sizeof *dst->log);
        if (!dst->log)
            return 0;
        for (int i = 0; i < src->log_n; i++) {
            dst->log[i].ts = src->log[i].ts;
            set_str(dst->log[i].who, sizeof dst->log[i].who, src->log[i].who);
            dst->log[i].text = dup_or_empty(src->log[i].text);
        }
        dst->log_n = src->log_n;
    }
    return dst->body != NULL;
}

static int note_append(struct board_card *c, const char *who, const char *text)
{
    struct board_note *grown = realloc(c->log, (size_t)(c->log_n + 1) * sizeof *grown);
    if (!grown)
        return 0;
    c->log = grown;

    struct board_note *n = &c->log[c->log_n];
    memset(n, 0, sizeof *n);
    n->ts = time(NULL);
    set_str(n->who, sizeof n->who, who);
    n->text = dup_or_empty(text);
    if (!n->text)
        return 0;
    c->log_n++;
    return 1;
}

static const char *json_str(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)o, key));
    return s ? s : "";
}

static double json_num(const cJSON *o, const char *key)
{
    const cJSON *j = cJSON_GetObjectItem((cJSON *)o, key);
    return (j && cJSON_IsNumber(j)) ? j->valuedouble : 0;
}

static int card_from_json(const cJSON *o, struct board_card *c)
{
    memset(c, 0, sizeof *c);
    set_str(c->id, sizeof c->id, json_str(o, "id"));
    if (!c->id[0])
        return 0;

    c->col = board_col_from_name(json_str(o, "col"));
    set_str(c->kind, sizeof c->kind, json_str(o, "kind"));
    set_str(c->title, sizeof c->title, json_str(o, "title"));
    set_str(c->cwd, sizeof c->cwd, json_str(o, "cwd"));
    set_str(c->backend, sizeof c->backend, json_str(o, "backend"));
    set_str(c->model, sizeof c->model, json_str(o, "model"));
    set_str(c->effort, sizeof c->effort, json_str(o, "effort"));
    set_str(c->session, sizeof c->session, json_str(o, "session"));
    set_str(c->worktree, sizeof c->worktree, json_str(o, "worktree"));
    set_str(c->base, sizeof c->base, json_str(o, "base"));
    set_str(c->stuck, sizeof c->stuck, json_str(o, "stuck"));

    c->priority = (int)json_num(o, "priority");
    c->cost_usd = json_num(o, "cost_usd");
    c->created = (time_t)json_num(o, "created");
    c->updated = (time_t)json_num(o, "updated");

    c->body = dup_or_empty(json_str(o, "body"));
    if (!c->body)
        return 0;

    const cJSON *log = cJSON_GetObjectItem((cJSON *)o, "log"), *e = NULL;
    cJSON_ArrayForEach(e, log) {
        if (!cJSON_IsObject(e))
            continue;
        struct board_note *grown = realloc(c->log, (size_t)(c->log_n + 1) * sizeof *grown);
        if (!grown)
            break;
        c->log = grown;
        struct board_note *n = &c->log[c->log_n];
        memset(n, 0, sizeof *n);
        n->ts = (time_t)json_num(e, "ts");
        set_str(n->who, sizeof n->who, json_str(e, "who"));
        n->text = dup_or_empty(json_str(e, "text"));
        if (!n->text)
            break;
        c->log_n++;
    }
    return 1;
}

static cJSON *card_to_json(const struct board_card *c)
{
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return NULL;

    cJSON_AddStringToObject(o, "id", c->id);
    cJSON_AddStringToObject(o, "col", board_col_name(c->col));
    cJSON_AddStringToObject(o, "kind", c->kind);
    cJSON_AddStringToObject(o, "title", c->title);
    cJSON_AddStringToObject(o, "body", c->body ? c->body : "");
    cJSON_AddStringToObject(o, "cwd", c->cwd);
    cJSON_AddNumberToObject(o, "priority", c->priority);
    cJSON_AddStringToObject(o, "backend", c->backend);
    cJSON_AddStringToObject(o, "model", c->model);
    cJSON_AddStringToObject(o, "effort", c->effort);
    cJSON_AddStringToObject(o, "session", c->session);
    cJSON_AddStringToObject(o, "worktree", c->worktree);
    cJSON_AddStringToObject(o, "base", c->base);
    cJSON_AddStringToObject(o, "stuck", c->stuck);
    cJSON_AddNumberToObject(o, "cost_usd", c->cost_usd);
    cJSON_AddNumberToObject(o, "created", (double)c->created);
    cJSON_AddNumberToObject(o, "updated", (double)c->updated);

    cJSON *log = cJSON_AddArrayToObject(o, "log");
    if (!log) {
        cJSON_Delete(o);
        return NULL;
    }
    for (int i = 0; i < c->log_n; i++) {
        cJSON *e = cJSON_CreateObject();
        if (!e)
            break;
        cJSON_AddNumberToObject(e, "ts", (double)c->log[i].ts);
        cJSON_AddStringToObject(e, "who", c->log[i].who);
        cJSON_AddStringToObject(e, "text", c->log[i].text ? c->log[i].text : "");
        cJSON_AddItemToArray(log, e);
    }
    return o;
}

static int by_created_desc(const void *a, const void *b)
{
    const struct board_card *x = a, *y = b;
    if (x->created != y->created)
        return x->created < y->created ? 1 : -1;
    return strcmp(y->id, x->id);
}

static int load_locked(struct board_card **out)
{
    *out = NULL;

    size_t len = 0;
    char  *text = text_slurp(board_path(), BOARD_MAX_BYTES, &len);
    if (!text)
        return 0;

    struct board_card *v = NULL;
    int                n = 0;

    char *save = text;
    for (char *line; (line = strsep(&save, "\n"));) {
        while (*line == ' ' || *line == '\t' || *line == '\r')
            line++;
        if (!*line)
            continue;
        cJSON *o = cJSON_Parse(line);
        if (!o)
            continue;

        struct board_card card;
        if (card_from_json(o, &card)) {
            struct board_card *grown = realloc(v, (size_t)(n + 1) * sizeof *grown);
            if (grown) {
                v = grown;
                v[n++] = card;
            } else {
                card_wipe(&card);
            }
        }
        cJSON_Delete(o);
    }
    free(text);

    if (n > 1)
        qsort(v, (size_t)n, sizeof *v, by_created_desc);
    *out = v;
    return n;
}

static unsigned long revision;

static int save_locked(const struct board_card *v, int n)
{
    char tmp[BOARD_PATH_MAX];
    if (!sidecar_path(tmp, sizeof tmp, ".tmp"))
        return 0;

    FILE *f = fopen(tmp, "wb");
    if (!f)
        return 0;

    int ok = 1;
    for (int i = 0; i < n && ok; i++) {
        cJSON *o = card_to_json(&v[i]);
        char  *s = o ? cJSON_PrintUnformatted(o) : NULL;
        cJSON_Delete(o);
        if (!s || fprintf(f, "%s\n", s) < 0)
            ok = 0;
        free(s);
    }
    if (fclose(f) != 0)
        ok = 0;

    if (!ok || rename(tmp, board_path()) != 0) {
        unlink(tmp);
        return 0;
    }
    revision++;
    return 1;
}

unsigned long board_revision(void) { return revision; }

int board_load(struct board_card **out)
{
    int lock = store_lock(LOCK_SH);
    int n = load_locked(out);
    store_unlock(lock);
    return n;
}

static void mint_id(const struct board_card *v, int n, char out[BOARD_ID_MAX])
{
    static const char ALPHABET[] = "0123456789abcdefghijkmnpqrstuvwxyz";
    static unsigned long minted;
    unsigned long        seed = (unsigned long)time(NULL) * 1099511628211UL ^
                                (unsigned long)getpid() ^
                                (minted++ * 2654435761UL);

    for (int attempt = 0; attempt < 64; attempt++) {
        seed = seed * 6364136223846793005UL + 1442695040888963407UL;
        unsigned long x = seed >> 17;
        for (int i = 0; i < 4; i++) {
            out[i] = ALPHABET[x % (sizeof ALPHABET - 1)];
            x /= (sizeof ALPHABET - 1);
        }
        out[4] = '\0';

        int taken = 0;
        for (int i = 0; i < n && !taken; i++)
            taken = !strcmp(v[i].id, out);
        if (!taken)
            return;
    }
}

void board_title_of(const char *text, char *out, size_t size)
{
    size_t n = strcspn(text, "\n");
    char  *first = strndup(text, n);
    if (!first) {
        text_one_line(text, out, size);
        return;
    }
    text_one_line(first, out, size);
    free(first);
}

int board_add(const char *text, const char *cwd, char id_out[BOARD_ID_MAX])
{
    if (!text || !*text)
        return 0;

    int lock = store_lock(LOCK_EX);

    struct board_card *v = NULL;
    int                n = load_locked(&v);

    struct board_card *grown = realloc(v, (size_t)(n + 1) * sizeof *grown);
    if (!grown) {
        board_free(v, n);
        store_unlock(lock);
        return 0;
    }
    v = grown;

    struct board_card *c = &v[n];
    memset(c, 0, sizeof *c);
    mint_id(v, n, c->id);
    c->col = BOARD_NEW;
    c->created = c->updated = time(NULL);
    board_title_of(text, c->title, sizeof c->title);
    c->body = dup_or_empty(text);

    char here[4096];
    if (cwd && *cwd)
        set_str(c->cwd, sizeof c->cwd, cwd);
    else if (getcwd(here, sizeof here))
        set_str(c->cwd, sizeof c->cwd, here);

    n++;

    int ok = c->body && save_locked(v, n);
    if (ok && id_out)
        set_str(id_out, BOARD_ID_MAX, c->id);

    board_free(v, n);
    store_unlock(lock);
    return ok;
}

static int with_card(const char *id, int (*fn)(struct board_card *c, void *ud), void *ud)
{
    if (!id || !*id)
        return 0;

    int lock = store_lock(LOCK_EX);

    struct board_card *v = NULL;
    int                n = load_locked(&v);
    struct board_card *c = board_find(v, n, id);

    int ok = 0;
    if (c && fn(c, ud)) {
        c->updated = time(NULL);
        ok = save_locked(v, n);
    }

    board_free(v, n);
    store_unlock(lock);
    return ok;
}

static int apply_update(struct board_card *c, void *ud)
{
    return card_adopt(c, ud);
}

int board_update(const struct board_card *card)
{
    if (!card)
        return 0;
    return with_card(card->id, apply_update, (void *)card);
}

struct note_args {
    const char *who;
    const char *text;
};

static int apply_note(struct board_card *c, void *ud)
{
    const struct note_args *a = ud;
    return note_append(c, a->who, a->text);
}

int board_note(const char *id, const char *who, const char *text)
{
    struct note_args a = {who, text};
    int              ok = with_card(id, apply_note, &a);
    if (ok)
        boardlog_note(id, who, text);
    return ok;
}

struct move_args {
    enum board_col col;
    const char    *who;
    const char    *why;
};

static int apply_move(struct board_card *c, void *ud)
{
    const struct move_args *a = ud;
    c->col = a->col;
    if (a->why && *a->why)
        return note_append(c, a->who, a->why);
    return 1;
}

int board_move(const char *id, enum board_col col, const char *who, const char *why)
{
    struct move_args a = {col, who, why};
    if (!with_card(id, apply_move, &a))
        return 0;

    char said[512];
    snprintf(said, sizeof said, "\xe2\x86\x92 %s%s%s", board_col_name(col),
             why && *why ? " \xc2\xb7 " : "", why && *why ? why : "");
    boardlog_note(id, who, said);
    return 1;
}

static const char *archive_path(void)
{
    static char p[4200];
    if (!p[0] && !path_config_file(p, sizeof p, "board-archive.jsonl"))
        snprintf(p, sizeof p, "/tmp/board-archive.jsonl");
    return p;
}

int board_archive(int days)
{
    if (days <= 0)
        return 0;

    int lock = store_lock(LOCK_EX);

    struct board_card *v = NULL;
    int                n = load_locked(&v);

    time_t cutoff = time(NULL) - (time_t)days * 24 * 3600;
    int    moved = 0;

    FILE *out = NULL;
    for (int i = 0; i < n; i++) {
        if (v[i].col != BOARD_DONE)
            continue;
        time_t when = v[i].updated ? v[i].updated : v[i].created;
        if (when > cutoff)
            continue;

        if (!out && !(out = fopen(archive_path(), "ab")))
            break;

        cJSON *o = card_to_json(&v[i]);
        char  *text = o ? cJSON_PrintUnformatted(o) : NULL;
        cJSON_Delete(o);
        if (!text)
            continue;
        int wrote = fprintf(out, "%s\n", text) > 0;
        free(text);
        if (!wrote)
            break;

        card_wipe(&v[i]);
        memmove(&v[i], &v[i + 1], (size_t)(n - i - 1) * sizeof *v);
        n--;
        i--;
        moved++;
    }

    if (out && fclose(out) != 0)
        moved = 0;
    if (moved)
        moved = save_locked(v, n) ? moved : 0;

    board_free(v, n);
    store_unlock(lock);
    return moved;
}

int board_remove(const char *id)
{
    if (!id || !*id)
        return 0;

    boardlog_remove(id);

    int lock = store_lock(LOCK_EX);

    struct board_card *v = NULL;
    int                n = load_locked(&v);

    int at = -1;
    for (int i = 0; i < n && at < 0; i++)
        if (!strcmp(v[i].id, id))
            at = i;

    int ok = 0;
    if (at >= 0) {
        card_wipe(&v[at]);
        memmove(&v[at], &v[at + 1], (size_t)(n - at - 1) * sizeof *v);
        n--;
        ok = save_locked(v, n);
    }

    board_free(v, n);
    store_unlock(lock);
    return ok;
}
