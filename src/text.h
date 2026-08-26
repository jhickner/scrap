
#ifndef TEXT_H
#define TEXT_H

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

void text_one_line(const char *in, char *out, size_t size);

void text_block(const char *in, char *out, size_t size);

void text_chomp(char *s);

/* append to buf, clamping at size rather than running past it */
__attribute__((format(printf, 4, 5)))
void text_appendf(char *buf, size_t size, size_t *at, const char *fmt, ...);

int text_split_commas(char *list, const char **out, int max);

__attribute__((format(printf, 1, 2)))
char *text_dsprintf(const char *fmt, ...);

char *text_slurp(const char *path, size_t max_bytes, size_t *len_out);

int text_spit(const char *path, int (*fill)(FILE *f, void *ud), void *ud);

int text_shell_quote(const char *s, char *out, size_t size);

int text_fuzzy_score(const char *name, const char *q);

size_t text_utf8_encode(uint32_t cp, char out[4]);

void text_humanize(long n, char *out, size_t size);

void text_ago(time_t then, int suffix, char *out, size_t size);

void text_duration(double seconds, char *out, size_t size);

double now_seconds(void);

int path_config_dir(char *out, size_t size);

int path_config_file(char *out, size_t size, const char *leaf);

void path_home_relative(const char *dir, char *out, size_t size);

char *path_expand_home(const char *path);

#endif
