#ifndef TGQUEUE_H
#define TGQUEUE_H

typedef struct tg_client tg_client;

enum tgqueue_kind {
    TGQUEUE_TEXT,
    TGQUEUE_MARKDOWN,
    TGQUEUE_PHOTO,
    TGQUEUE_ACTION,
};

struct tgqueue;

struct tgqueue *tgqueue_new(tg_client *client, long chat_id);
void            tgqueue_free(struct tgqueue *q);

void tgqueue_push(struct tgqueue *q, enum tgqueue_kind kind, const char *text);

void tgqueue_hold(struct tgqueue *q);
void tgqueue_release(struct tgqueue *q);

int tgqueue_on_sender(const struct tgqueue *q);
int tgqueue_aborted(const struct tgqueue *q);

#endif
