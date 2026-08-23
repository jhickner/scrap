#include "boardtriage.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "board.h"
#include "boardcfg.h"
#include "sessionfork.h"
#include "vendor/cJSON.h"

#define TRIAGE_SLOTS 4
#define REPLY_MAX    (1u << 18)

// A card short enough to read as a row is its own best title. Rewriting one
// buys a few columns and stakes the meaning on a paraphrase -- and a title
// that inverts a card is worse than a title that runs off the edge, because
// the truncated one is still the words that were written.
#define TITLE_KEEP 100

struct slot {
    pid_t  pid;
    char   id[BOARD_ID_MAX];
    int    fd;
    char  *buf;
    size_t len, cap;
};

static struct slot slots[TRIAGE_SLOTS];

static void slot_free(struct slot *s)
{
    if (s->fd >= 0)
        close(s->fd);
    free(s->buf);
    memset(s, 0, sizeof *s);
    s->fd = -1;
}

/* ---- what the card is asked ------------------------------------------- */

// The prompt, the card, and where it was captured. The directory matters:
// most cards are thrown from the repo they belong to, so it is the strongest
// hint triage has, and it is still only a hint.
static char *build_prompt(const struct board_card *c)
{
    const struct board_profile *p = boardcfg_for(BOARD_WHO_TRIAGE);
    const char                 *head = p->prompt ? p->prompt : "";
    const char                 *body = c->body && *c->body ? c->body : c->title;

    size_t need = strlen(head) + strlen(body) + strlen(c->cwd) + 128;
    char  *out = malloc(need);
    if (!out)
        return NULL;
    snprintf(out, need,
             "%s\n\nThe card was captured in: %s\n\ncard:\n%s\n",
             head, c->cwd[0] ? c->cwd : "(nowhere in particular)", body);
    return out;
}

static int spawn(struct slot *s, const struct board_card *c)
{
    char *prompt = build_prompt(c);
    if (!prompt)
        return 0;

    int pipes[2];
    if (pipe(pipes) != 0) {
        free(prompt);
        return 0;
    }

    const struct board_profile *p = boardcfg_for(BOARD_WHO_TRIAGE);

    char *argv[16];
    int   n = 0;
    argv[n++] = (char *)sessionfork_program();
    argv[n++] = "-b";
    argv[n++] = (char *)(p->backend[0] ? p->backend : "claude");
    if (c->cwd[0]) {
        argv[n++] = "-C";
        argv[n++] = (char *)c->cwd;
    }
    if (p->model[0] && strcmp(p->model, "default")) {
        argv[n++] = "-m";
        argv[n++] = (char *)p->model;
    }
    if (p->effort[0] && strcmp(p->effort, "default")) {
        argv[n++] = "-e";
        argv[n++] = (char *)p->effort;
    }
    argv[n++] = prompt;
    argv[n] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipes[0]);
        close(pipes[1]);
        free(prompt);
        return 0;
    }
    if (pid == 0) {
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO);
        if (pipes[1] != STDOUT_FILENO)
            close(pipes[1]);
        // What it says on stderr is the CLI's own noise, not an answer.
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDERR_FILENO);
            dup2(null, STDIN_FILENO);
            if (null > STDERR_FILENO)
                close(null);
        }
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pipes[1]);
    free(prompt);
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    s->pid = pid;
    s->fd = pipes[0];
    snprintf(s->id, sizeof s->id, "%s", c->id);
    return 1;
}

/* ---- what comes back --------------------------------------------------- */

// The reply should be JSON and nothing else, but a model that has been asked
// for JSON only will still sometimes wrap it. Take the outermost braces.
static cJSON *parse_reply(const char *text)
{
    if (!text)
        return NULL;
    const char *open = strchr(text, '{');
    const char *close = strrchr(text, '}');
    if (!open || !close || close < open)
        return NULL;

    size_t n = (size_t)(close - open) + 1;
    char  *slice = malloc(n + 1);
    if (!slice)
        return NULL;
    memcpy(slice, open, n);
    slice[n] = '\0';
    cJSON *o = cJSON_Parse(slice);
    free(slice);
    return o;
}

static const char *str_of(const cJSON *o, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)o, key));
    return s ? s : "";
}

static double num_of(const cJSON *o, const char *key, double fallback)
{
    const cJSON *j = cJSON_GetObjectItem((cJSON *)o, key);
    return (j && cJSON_IsNumber(j)) ? j->valuedouble : fallback;
}

// Notes to file want no worker; work does.
static int is_wiki_kind(const char *kind)
{
    return !strcmp(kind, "todo") || !strcmp(kind, "data") ||
           !strcmp(kind, "reference");
}

static int is_work_kind(const char *kind)
{
    return !strcmp(kind, "feature") || !strcmp(kind, "bug") ||
           !strcmp(kind, "chore");
}

int boardtriage_attempts(const struct board_card *c)
{
    int n = 0;
    for (int i = 0; i < c->log_n; i++)
        if (!strcmp(c->log[i].who, "triage"))
            n++;
    return n;
}

// Writes what triage decided onto the card, and moves it to whichever column
// that decision implies.
static int apply(const char *id, const cJSON *o)
{
    struct board_card *cards = NULL;
    int                n = board_load(&cards);
    struct board_card *c = board_find(cards, n, id);
    if (!c) {
        board_free(cards, n);
        return 0;
    }

    const char *kind = str_of(o, "kind");
    const char *title = str_of(o, "title");
    const char *spec = str_of(o, "spec");
    const char *cwd = str_of(o, "cwd");
    const char *question = str_of(o, "question");
    double      confidence = num_of(o, "confidence", 1.0);

    int unsure = confidence < 0.5 || *question || !(*kind) ||
                 (!is_wiki_kind(kind) && !is_work_kind(kind));

    struct board_card edited = *c;
    char             *spec_kept = NULL;

    // A title is only how the card reads in a list, so a better one is worth
    // having either way -- but only where there is something to gain by it.
    if (*title && c->body && strlen(c->body) > TITLE_KEEP)
        snprintf(edited.title, sizeof edited.title, "%s", title);

    // The rest is what triage worked out, and it only worked anything out if
    // it understood the card. Writing its spec over one it could not read
    // would lose the words the card was captured with -- which are the exact
    // words the question is about.
    if (!unsure) {
        if (*spec) {
            spec_kept = strdup(spec);
            if (spec_kept)
                edited.body = spec_kept;
        }
        if (*cwd)
            snprintf(edited.cwd, sizeof edited.cwd, "%s", cwd);
        edited.priority = (int)num_of(o, "priority", 0);
    }

    char said[1024];
    if (unsure) {
        // The kind is not written either: a guess recorded as a fact is
        // exactly what the column exists to avoid.
        edited.col = BOARD_UNCLEAR;
        snprintf(said, sizeof said, "%s",
                 *question ? question : "could not tell what this card is");
    } else {
        snprintf(edited.kind, sizeof edited.kind, "%s", kind);
        edited.col = is_wiki_kind(kind) ? BOARD_DONE : BOARD_BACKLOG;
        snprintf(said, sizeof said, "%s · %s · priority %d", kind,
                 is_wiki_kind(kind) ? "filed" : "for a worker", edited.priority);
    }

    int ok = board_update(&edited);
    free(spec_kept);
    board_free(cards, n);
    if (ok)
        board_note(id, "triage", said);
    return ok;
}

static void failed(const char *id, const char *why)
{
    board_move(id, BOARD_UNCLEAR, "triage", why);
}

/* ---- the pool ---------------------------------------------------------- */

int boardtriage_running(const char *id)
{
    for (int i = 0; i < TRIAGE_SLOTS; i++)
        if (slots[i].pid && !strcmp(slots[i].id, id))
            return 1;
    return 0;
}

int boardtriage_busy(void)
{
    for (int i = 0; i < TRIAGE_SLOTS; i++)
        if (slots[i].pid)
            return 1;
    return 0;
}

int boardtriage_start(const struct board_card *c)
{
    if (!c || boardtriage_running(c->id))
        return 0;
    for (int i = 0; i < TRIAGE_SLOTS; i++) {
        if (slots[i].pid)
            continue;
        slots[i].fd = -1;
        if (!spawn(&slots[i], c)) {
            slot_free(&slots[i]);
            return 0;
        }
        return 1;
    }
    return 0;
}

static void drain(struct slot *s)
{
    for (;;) {
        if (s->len + 4096 > s->cap) {
            if (s->cap >= REPLY_MAX)
                return;
            size_t cap = s->cap ? s->cap * 2 : 8192;
            char  *grown = realloc(s->buf, cap);
            if (!grown)
                return;
            s->buf = grown;
            s->cap = cap;
        }
        ssize_t k = read(s->fd, s->buf + s->len, s->cap - s->len - 1);
        if (k <= 0)
            return;
        s->len += (size_t)k;
        s->buf[s->len] = '\0';
    }
}

int boardtriage_poll(void)
{
    int changed = 0;

    for (int i = 0; i < TRIAGE_SLOTS; i++) {
        struct slot *s = &slots[i];
        if (!s->pid)
            continue;

        drain(s);

        int   status = 0;
        pid_t done = waitpid(s->pid, &status, WNOHANG);
        if (done != s->pid)
            continue;

        drain(s);

        char id[BOARD_ID_MAX];
        snprintf(id, sizeof id, "%s", s->id);

        cJSON *o = parse_reply(s->buf);
        if (o) {
            changed |= apply(id, o);
            cJSON_Delete(o);
        } else {
            failed(id, s->buf && *s->buf
                           ? "triage did not answer with a card"
                           : "triage did not answer");
            changed = 1;
        }
        slot_free(s);
    }
    return changed;
}

void boardtriage_close_all(void)
{
    for (int i = 0; i < TRIAGE_SLOTS; i++) {
        if (!slots[i].pid)
            continue;
        kill(slots[i].pid, SIGTERM);
        waitpid(slots[i].pid, NULL, 0);
        slot_free(&slots[i]);
    }
}
