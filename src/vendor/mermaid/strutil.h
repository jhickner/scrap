#ifndef STRUTIL_H
#define STRUTIL_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *p;
    size_t len;
} Slice;

Slice slice_cstr(const char *s);
Slice slice_trim(Slice s);
Slice slice_trim_start(Slice s);
Slice slice_trim_end(Slice s);
bool slice_eq(Slice a, Slice b);
bool slice_eq_cstr(Slice a, const char *b);
bool slice_eq_ignore_case_cstr(Slice a, const char *b);
bool slice_starts_with(Slice s, const char *prefix);
bool slice_ends_with(Slice s, const char *suffix);
Slice slice_strip_prefix(Slice s, const char *prefix, bool *ok);
Slice slice_strip_suffix(Slice s, const char *suffix, bool *ok);
Slice slice_trim_end_char(Slice s, char c);
Slice slice_trim_start_char(Slice s, char c);
bool slice_split_once_char(Slice s, char sep, Slice *left, Slice *right);
bool slice_split_once_cstr(Slice s, const char *sep, Slice *left, Slice *right);
bool slice_is_empty(Slice s);
bool slice_contains_char(Slice s, char c);
bool slice_contains_cstr(Slice s, const char *needle);
long slice_find_char(Slice s, char c);
long slice_find_cstr(Slice s, const char *needle);
long slice_rfind_char(Slice s, char c);
bool slice_has_whitespace(Slice s);
Slice slice_first_word(Slice s, Slice *rest);
char *slice_dup(Slice s);
char *slice_to_lower(Slice s);
char *cstr_dup(const char *s);
char *cstr_to_lower(const char *s);

typedef struct {
    char **data;
    size_t n, cap;
} StrVec;
void strvec_init(StrVec *v);
void strvec_push(StrVec *v, char *owned);
void strvec_free(StrVec *v);

typedef struct {
    size_t *data;
    size_t n, cap;
} SizeVec;
void sizevec_init(SizeVec *v);
void sizevec_push(SizeVec *v, size_t x);
void sizevec_free(SizeVec *v);

typedef struct {
    int *data;
    size_t n, cap;
} IntVec;
void intvec_init(IntVec *v);
void intvec_push(IntVec *v, int x);
void intvec_free(IntVec *v);

typedef struct {
    char *data;
    size_t len, cap;
} Buf;
void buf_init(Buf *b);
void buf_push_char(Buf *b, char c);
void buf_push_cstr(Buf *b, const char *s);
void buf_push_slice(Buf *b, Slice s);
void buf_free(Buf *b);
char *buf_take(Buf *b);

#endif
