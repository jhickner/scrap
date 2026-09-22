#include "strutil.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

Slice slice_cstr(const char *s) { return (Slice){ s, strlen(s) }; }

static bool is_sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

Slice slice_trim_start(Slice s) {
    while (s.len > 0 && is_sp(s.p[0])) { s.p++; s.len--; }
    return s;
}
Slice slice_trim_end(Slice s) {
    while (s.len > 0 && is_sp(s.p[s.len - 1])) s.len--;
    return s;
}
Slice slice_trim(Slice s) { return slice_trim_end(slice_trim_start(s)); }

bool slice_eq(Slice a, Slice b) {
    return a.len == b.len && (a.len == 0 || memcmp(a.p, b.p, a.len) == 0);
}
bool slice_eq_cstr(Slice a, const char *b) { return slice_eq(a, slice_cstr(b)); }

bool slice_eq_ignore_case_cstr(Slice a, const char *b) {
    size_t bl = strlen(b);
    if (a.len != bl) return false;
    for (size_t i = 0; i < a.len; i++)
        if (tolower((unsigned char)a.p[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

bool slice_starts_with(Slice s, const char *prefix) {
    size_t pl = strlen(prefix);
    return s.len >= pl && memcmp(s.p, prefix, pl) == 0;
}
bool slice_ends_with(Slice s, const char *suffix) {
    size_t sl = strlen(suffix);
    return s.len >= sl && memcmp(s.p + s.len - sl, suffix, sl) == 0;
}

Slice slice_strip_prefix(Slice s, const char *prefix, bool *ok) {
    if (slice_starts_with(s, prefix)) {
        size_t pl = strlen(prefix);
        if (ok) *ok = true;
        return (Slice){ s.p + pl, s.len - pl };
    }
    if (ok) *ok = false;
    return s;
}
Slice slice_strip_suffix(Slice s, const char *suffix, bool *ok) {
    if (slice_ends_with(s, suffix)) {
        size_t sl = strlen(suffix);
        if (ok) *ok = true;
        return (Slice){ s.p, s.len - sl };
    }
    if (ok) *ok = false;
    return s;
}
Slice slice_trim_end_char(Slice s, char c) {
    while (s.len > 0 && s.p[s.len - 1] == c) s.len--;
    return s;
}
Slice slice_trim_start_char(Slice s, char c) {
    while (s.len > 0 && s.p[0] == c) { s.p++; s.len--; }
    return s;
}

bool slice_split_once_char(Slice s, char sep, Slice *left, Slice *right) {
    for (size_t i = 0; i < s.len; i++) {
        if (s.p[i] == sep) {
            *left = (Slice){ s.p, i };
            *right = (Slice){ s.p + i + 1, s.len - i - 1 };
            return true;
        }
    }
    return false;
}

bool slice_split_once_cstr(Slice s, const char *sep, Slice *left, Slice *right) {
    size_t sl = strlen(sep);
    if (sl == 0 || s.len < sl) return false;
    for (size_t i = 0; i + sl <= s.len; i++) {
        if (memcmp(s.p + i, sep, sl) == 0) {
            *left = (Slice){ s.p, i };
            *right = (Slice){ s.p + i + sl, s.len - i - sl };
            return true;
        }
    }
    return false;
}

bool slice_is_empty(Slice s) { return s.len == 0; }

bool slice_contains_char(Slice s, char c) { return slice_find_char(s, c) >= 0; }
bool slice_contains_cstr(Slice s, const char *needle) { return slice_find_cstr(s, needle) >= 0; }

long slice_find_char(Slice s, char c) {
    for (size_t i = 0; i < s.len; i++) if (s.p[i] == c) return (long)i;
    return -1;
}
long slice_find_cstr(Slice s, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return 0;
    if (s.len < nl) return -1;
    for (size_t i = 0; i + nl <= s.len; i++)
        if (memcmp(s.p + i, needle, nl) == 0) return (long)i;
    return -1;
}
long slice_rfind_char(Slice s, char c) {
    for (long i = (long)s.len - 1; i >= 0; i--) if (s.p[i] == c) return i;
    return -1;
}

bool slice_has_whitespace(Slice s) {
    for (size_t i = 0; i < s.len; i++) if (is_sp(s.p[i])) return true;
    return false;
}

Slice slice_first_word(Slice s, Slice *rest) {
    s = slice_trim_start(s);
    size_t i = 0;
    while (i < s.len && !is_sp(s.p[i])) i++;
    Slice word = { s.p, i };
    if (rest) *rest = slice_trim_start((Slice){ s.p + i, s.len - i });
    return word;
}

char *slice_dup(Slice s) {
    char *out = malloc(s.len + 1);
    if (s.len) memcpy(out, s.p, s.len);
    out[s.len] = 0;
    return out;
}

char *slice_to_lower(Slice s) {
    char *out = malloc(s.len + 1);
    for (size_t i = 0; i < s.len; i++) out[i] = (char)tolower((unsigned char)s.p[i]);
    out[s.len] = 0;
    return out;
}

char *cstr_dup(const char *s) { return slice_dup(slice_cstr(s)); }
char *cstr_to_lower(const char *s) { return slice_to_lower(slice_cstr(s)); }

void strvec_init(StrVec *v) { v->data = NULL; v->n = 0; v->cap = 0; }
void strvec_push(StrVec *v, char *owned) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->data = realloc(v->data, v->cap * sizeof(char *));
    }
    v->data[v->n++] = owned;
}
void strvec_free(StrVec *v) {
    for (size_t i = 0; i < v->n; i++) free(v->data[i]);
    free(v->data);
    v->data = NULL; v->n = v->cap = 0;
}

void sizevec_init(SizeVec *v) { v->data = NULL; v->n = 0; v->cap = 0; }
void sizevec_push(SizeVec *v, size_t x) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->data = realloc(v->data, v->cap * sizeof(size_t));
    }
    v->data[v->n++] = x;
}
void sizevec_free(SizeVec *v) { free(v->data); v->data = NULL; v->n = v->cap = 0; }

void intvec_init(IntVec *v) { v->data = NULL; v->n = 0; v->cap = 0; }
void intvec_push(IntVec *v, int x) {
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->data = realloc(v->data, v->cap * sizeof(int));
    }
    v->data[v->n++] = x;
}
void intvec_free(IntVec *v) { free(v->data); v->data = NULL; v->n = v->cap = 0; }

void buf_init(Buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
static void buf_reserve(Buf *b, size_t extra) {
    if (b->len + extra + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 32;
        while (nc < b->len + extra + 1) nc *= 2;
        b->data = realloc(b->data, nc);
        b->cap = nc;
    }
}
void buf_push_char(Buf *b, char c) {
    buf_reserve(b, 1);
    b->data[b->len++] = c;
    b->data[b->len] = 0;
}
void buf_push_cstr(Buf *b, const char *s) { buf_push_slice(b, slice_cstr(s)); }
void buf_push_slice(Buf *b, Slice s) {
    buf_reserve(b, s.len);
    if (s.len) memcpy(b->data + b->len, s.p, s.len);
    b->len += s.len;
    b->data[b->len] = 0;
}
void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }
char *buf_take(Buf *b) {
    if (!b->data) return cstr_dup("");
    char *d = b->data;
    b->data = NULL; b->len = b->cap = 0;
    return d;
}
