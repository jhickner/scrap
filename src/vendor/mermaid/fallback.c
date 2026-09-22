#include <stdlib.h>
#include <string.h>

#include "canvas.h"
#include "dwidth.h"
#include "strutil.h"

static char *first_word(const char *src) {
    Slice w = slice_first_word(slice_cstr(src), NULL);
    if (slice_is_empty(w)) return cstr_dup("diagram");
    return slice_dup(w);
}

static void chunk_line(const char *line, int limit, StrVec *out) {
    size_t lw = dwidth_str(line);
    if (limit <= 0 || lw <= (size_t)limit) { strvec_push(out, cstr_dup(line)); return; }
    uint32_t *cps; size_t n = utf8_decode(line, &cps);
    Buf cur; buf_init(&cur);
    size_t cur_w = 0;
    for (size_t i = 0; i < n; i++) {
        int cw = dwidth_char(cps[i]);
        if (cw < 1) cw = 1;
        if (cur_w + (size_t)cw > (size_t)limit && cur.len > 0) {
            strvec_push(out, buf_take(&cur));
            buf_init(&cur);
            cur_w = 0;
        }
        char tmp[4]; size_t tl = utf8_encode_cp(cps[i], tmp);
        for (size_t t = 0; t < tl; t++) buf_push_char(&cur, tmp[t]);
        cur_w += (size_t)cw;
    }
    free(cps);
    if (cur.len > 0) strvec_push(out, buf_take(&cur)); else buf_free(&cur);
}

static void wrap_words(const char *text, int limit, StrVec *out) {
    if (limit <= 0) { strvec_push(out, cstr_dup(text)); return; }
    StrVec lines; strvec_init(&lines);
    Buf cur; buf_init(&cur);
    Slice rest = slice_cstr(text);
    while (true) {
        size_t i = 0;
        while (i < rest.len && rest.p[i] == ' ') { rest.p++; rest.len--; }
        i = 0;
        while (i < rest.len && rest.p[i] != ' ') i++;
        if (i == 0) break;
        Slice word = { rest.p, i };
        rest = (Slice){ rest.p + i, rest.len - i };
        char *wc = slice_dup(word);
        if (cur.len == 0) buf_push_cstr(&cur, wc);
        else {
            size_t curw = dwidth_str(cur.data);
            size_t ww = dwidth_str(wc);
            if (curw + 1 + ww <= (size_t)limit) { buf_push_char(&cur, ' '); buf_push_cstr(&cur, wc); }
            else { strvec_push(&lines, buf_take(&cur)); buf_init(&cur); buf_push_cstr(&cur, wc); }
        }
        free(wc);
    }
    if (cur.len > 0) strvec_push(&lines, buf_take(&cur)); else buf_free(&cur);
    for (size_t i = 0; i < lines.n; i++) chunk_line(lines.data[i], limit, out);
    strvec_free(&lines);
}

MermaidArt *fallback(const char *src, int max_width, bool too_wide) {
    char *header = first_word(src);
    Buf tb; buf_init(&tb);
    buf_push_cstr(&tb, " mermaid: ");
    buf_push_cstr(&tb, header);
    buf_push_char(&tb, ' ');
    free(header);
    char *title = buf_take(&tb);
    size_t title_w = dwidth_str(title);

    int limit = max_width > 0 ? (max_width > 4 ? max_width - 4 : 0) : -1;
    if (limit > 0 && limit < 8) limit = 8;

    StrVec body; strvec_init(&body);
    bool skipping = true;
    StrVec rawlines; strvec_init(&rawlines);
    {
        const char *p = src;
        while (true) {
            const char *nl = strchr(p, '\n');
            size_t ll = nl ? (size_t)(nl - p) : strlen(p);
            char *line = malloc(ll + 1);
            memcpy(line, p, ll);
            line[ll] = 0;
            if (ll > 0 && line[ll - 1] == '\r') line[ll - 1] = 0;
            Slice trimmed = slice_trim_end(slice_cstr(line));
            char *tl = slice_dup(trimmed);
            free(line);
            strvec_push(&rawlines, tl);
            if (!nl) break;
            p = nl + 1;
        }
    }

    for (size_t i = 0; i < rawlines.n; i++) {
        const char *line = rawlines.data[i];
        if (skipping && line[0] == 0) continue;
        skipping = false;
        chunk_line(line, limit, &body);
    }
    strvec_free(&rawlines);

    size_t content_w = title_w;
    for (size_t i = 0; i < body.n; i++) {
        size_t w = dwidth_str(body.data[i]);
        if (w > content_w) content_w = w;
    }
    size_t inner = content_w + 2;

    StrVec out; strvec_init(&out);

    Buf top; buf_init(&top);
    buf_push_cstr(&top, "\xE2\x95\xAD");
    buf_push_cstr(&top, title);
    size_t dashn = inner > title_w ? inner - title_w : 0;
    for (size_t i = 0; i < dashn; i++) buf_push_cstr(&top, "\xE2\x94\x80");
    buf_push_cstr(&top, "\xE2\x95\xAE");
    strvec_push(&out, buf_take(&top));

    for (size_t i = 0; i < body.n; i++) {
        size_t w = dwidth_str(body.data[i]);
        size_t pad = content_w > w ? content_w - w : 0;
        Buf row; buf_init(&row);
        buf_push_cstr(&row, "\xE2\x94\x82 ");
        buf_push_cstr(&row, body.data[i]);
        for (size_t k = 0; k < pad; k++) buf_push_char(&row, ' ');
        buf_push_cstr(&row, " \xE2\x94\x82");
        strvec_push(&out, buf_take(&row));
    }

    Buf bot; buf_init(&bot);
    buf_push_cstr(&bot, "\xE2\x95\xB0");
    for (size_t i = 0; i < inner; i++) buf_push_cstr(&bot, "\xE2\x94\x80");
    buf_push_cstr(&bot, "\xE2\x95\xAF");
    strvec_push(&out, buf_take(&bot));

    if (too_wide) {
        StrVec hint; strvec_init(&hint);
        wrap_words("This diagram is too wide to display here \xE2\x80\x94 open the image to view it in full.", max_width, &hint);
        for (size_t i = 0; i < hint.n; i++) strvec_push(&out, cstr_dup(hint.data[i]));
        strvec_free(&hint);
    }

    strvec_free(&body);
    free(title);

    MermaidArt *art = malloc(sizeof(MermaidArt));
    art->n = out.n;
    art->lines = out.data;
    return art;
}
