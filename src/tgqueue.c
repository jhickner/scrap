#include "tgqueue.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vendor/telegram.h"

#define TGQUEUE_MAX           512
#define TGQUEUE_FLUSH_SECONDS 3

struct message {
    enum tgqueue_kind kind;
    char             *text;
    struct message   *next;
};

struct tgqueue {
    tg_client *client;
    long       chat_id;

    pthread_mutex_t lock;
    pthread_cond_t  changed;
    struct message *head, *tail;
    int             count, inflight;
    int             draining, abort, done;
    pthread_t       sender;
    int             sender_live;
};

static void send_one(struct tgqueue *q, enum tgqueue_kind kind, const char *text)
{
    switch (kind) {
    case TGQUEUE_TEXT:     tg_send_message(q->client, q->chat_id, text); break;
    case TGQUEUE_MARKDOWN: tg_send_message_md(q->client, q->chat_id, text); break;
    case TGQUEUE_PHOTO:    tg_send_photo(q->client, q->chat_id, text, NULL); break;
    case TGQUEUE_ACTION:   tg_send_chat_action(q->client, q->chat_id, text); break;
    }
}

static void message_free(struct message *m)
{
    free(m->text);
    free(m);
}

static void *sender_main(void *ud)
{
    struct tgqueue *q = ud;
    pthread_mutex_lock(&q->lock);
    for (;;) {
        while (!q->head && !q->draining)
            pthread_cond_wait(&q->changed, &q->lock);
        if (!q->head || q->abort)
            break;

        struct message *m = q->head;
        q->head = m->next;
        if (!q->head)
            q->tail = NULL;
        q->count--;
        q->inflight = 1;
        pthread_mutex_unlock(&q->lock);

        send_one(q, m->kind, m->text);
        message_free(m);

        pthread_mutex_lock(&q->lock);
        q->inflight = 0;
        pthread_cond_broadcast(&q->changed);
    }
    while (q->head) {
        struct message *m = q->head;
        q->head = m->next;
        q->count--;
        message_free(m);
    }
    q->tail = NULL;
    q->done = 1;
    pthread_cond_broadcast(&q->changed);
    pthread_mutex_unlock(&q->lock);
    return NULL;
}

struct tgqueue *tgqueue_new(tg_client *client, long chat_id)
{
    if (!client || !chat_id)
        return NULL;
    struct tgqueue *q = calloc(1, sizeof *q);
    if (!q)
        return NULL;
    q->client = client;
    q->chat_id = chat_id;
    if (pthread_mutex_init(&q->lock, NULL) != 0) {
        free(q);
        return NULL;
    }
    if (pthread_cond_init(&q->changed, NULL) != 0) {
        pthread_mutex_destroy(&q->lock);
        free(q);
        return NULL;
    }
    q->sender_live = pthread_create(&q->sender, NULL, sender_main, q) == 0;
    return q;
}

void tgqueue_push(struct tgqueue *q, enum tgqueue_kind kind, const char *text)
{
    if (!q || !text || !*text)
        return;
    if (!q->sender_live) {
        send_one(q, kind, text);
        return;
    }

    struct message *m = calloc(1, sizeof *m);
    char *copy = strdup(text);
    if (!m || !copy) {
        free(m);
        free(copy);
        return;
    }
    m->kind = kind;
    m->text = copy;

    pthread_mutex_lock(&q->lock);
    if (q->count >= TGQUEUE_MAX || q->draining) {
        pthread_mutex_unlock(&q->lock);
        message_free(m);
        return;
    }
    if (q->tail)
        q->tail->next = m;
    else
        q->head = m;
    q->tail = m;
    q->count++;
    pthread_cond_broadcast(&q->changed);
    pthread_mutex_unlock(&q->lock);
}

void tgqueue_hold(struct tgqueue *q)
{
    if (!q || !q->sender_live)
        return;
    pthread_mutex_lock(&q->lock);
    while (q->head || q->inflight)
        pthread_cond_wait(&q->changed, &q->lock);
}

void tgqueue_release(struct tgqueue *q)
{
    if (q && q->sender_live)
        pthread_mutex_unlock(&q->lock);
}

int tgqueue_on_sender(const struct tgqueue *q)
{
    return q && q->sender_live && pthread_equal(pthread_self(), q->sender);
}

int tgqueue_aborted(const struct tgqueue *q)
{
    return q && q->abort;
}

void tgqueue_free(struct tgqueue *q)
{
    if (!q)
        return;
    if (q->sender_live) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += TGQUEUE_FLUSH_SECONDS;

        pthread_mutex_lock(&q->lock);
        q->draining = 1;
        pthread_cond_broadcast(&q->changed);
        while (!q->done) {
            if (pthread_cond_timedwait(&q->changed, &q->lock, &until) == ETIMEDOUT) {
                q->abort = 1;
                break;
            }
        }
        pthread_mutex_unlock(&q->lock);
        pthread_join(q->sender, NULL);
    }
    pthread_cond_destroy(&q->changed);
    pthread_mutex_destroy(&q->lock);
    free(q);
}
