#ifndef WORKSPACE_H
#define WORKSPACE_H

struct session;

#define WORKSPACE_MAX 12

int  workspace_begin(struct session *first, int safe_mode);
void workspace_end(void);

struct session *workspace_current(void);
struct session *workspace_base(void);
struct session *workspace_at(int index);
int  workspace_count(void);
int  workspace_index(void);
int  workspace_index_of(const struct session *s);

int  workspace_spawn(const char *backend, const char *model, const char *effort,
                     const char *cwd, const char *id);

int  workspace_open(struct session *s);

void workspace_show(int index);

void workspace_render(int index, void (*fn)(struct session *s, void *ud), void *ud);

int  workspace_find_id(const char *id);

int  workspace_dump(int index, const char *path);

int  workspace_close(int index);
int  workspace_close_to_base(int index);

int  workspace_send(int index, const char *line, const char *shown);
int  workspace_queued(int index);

const char *workspace_pending_at(int index, int i);

char *workspace_unqueue(int index);

void workspace_settle(struct session *s);

void workspace_on_finish(void (*fn)(struct session *s));

void workspace_on_settled(void (*fn)(struct session *s));

void workspace_on_turn(void (*fn)(struct session *s));

int  workspace_fds(int *out, int max);
int  workspace_pump(void);

int  workspace_pump_quiet(void);

int  workspace_drain(void);
int  workspace_busy(void);

const char *workspace_status(const struct session *s);

#endif
