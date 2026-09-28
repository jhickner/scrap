#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "strutil.h"

static void flush_statement(Buf *cur, StrVec *out) {
    Slice trimmed = slice_trim(slice_cstr(cur->data ? cur->data : ""));
    if (!slice_is_empty(trimmed)) strvec_push(out, slice_dup(trimmed));
    buf_free(cur);
    buf_init(cur);
}

static void split_statements(const char *line, StrVec *out) {
    Buf cur;
    buf_init(&cur);
    bool in_quotes = false, in_label = false;
    size_t len = strlen(line);
    for (size_t i = 0; i < len; i++) {
        char c = line[i];
        if (in_quotes) {
            if (c == '"') in_quotes = false;
            buf_push_char(&cur, c);
            continue;
        }
        if (c == '"') { in_quotes = true; buf_push_char(&cur, c); }
        else if (c == '%' && i + 1 < len && line[i + 1] == '%') break;
        else if (c == ';' && !in_label) flush_statement(&cur, out);
        else if (c == ':' && (line[i + 1] == ' ' || line[i + 1] == '\t')) {
            in_label = true;
            buf_push_char(&cur, c);
        }
        else buf_push_char(&cur, c);
    }
    flush_statement(&cur, out);
    buf_free(&cur);
}

void collect_statements(const char *src, StrVec *out) {
    strvec_init(out);
    const char *p = src;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
        char *line = malloc(linelen + 1);
        memcpy(line, p, linelen);
        line[linelen] = 0;
        if (linelen > 0 && line[linelen - 1] == '\r') line[linelen - 1] = 0;
        split_statements(line, out);
        free(line);
        if (!nl) break;
        p = nl + 1;
    }
}
