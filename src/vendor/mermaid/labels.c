#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "dwidth.h"
#include "internal.h"
#include "strutil.h"

const char LABEL_BREAK_CHARS[4] = { '_', '-', '.', '/' };

static bool is_alnum_byte(char c) {
    return isalnum((unsigned char)c) || (unsigned char)c >= 0x80;
}

char *strip_markdown(const char *s) {
    size_t len = strlen(s);
    Buf nostrong;
    buf_init(&nostrong);
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '`') continue;
        if ((s[i] == '*' && i + 1 < len && s[i + 1] == '*') ||
            (s[i] == '_' && i + 1 < len && s[i + 1] == '_')) {
            i++;
            continue;
        }
        buf_push_char(&nostrong, s[i]);
    }
    char *no_strong = buf_take(&nostrong);
    size_t nl = strlen(no_strong);
    Buf out;
    buf_init(&out);
    for (size_t i = 0; i < nl; i++) {
        char c = no_strong[i];
        if (c == '*' || c == '_') {
            bool prev_alnum = i > 0 && is_alnum_byte(no_strong[i - 1]);
            bool next_alnum = i + 1 < nl && is_alnum_byte(no_strong[i + 1]);
            if (!(prev_alnum && next_alnum)) continue;
        }
        buf_push_char(&out, c);
    }
    free(no_strong);
    char *raw = buf_take(&out);
    Slice trimmed = slice_trim(slice_cstr(raw));
    char *result = slice_dup(trimmed);
    free(raw);
    return result;
}

static const char *HTML_FORMAT_TAGS[] = {
    "b", "strong", "i", "em", "u", "s", "strike", "del", "ins", "mark", "small", "big", "sub",
    "sup", "code", "kbd", "samp", "var", "tt", "span", "font", "q", "abbr", "cite", "pre", NULL
};

static bool tag_at(const char *s, size_t len, size_t start, size_t *name_start, size_t *name_len, size_t *end) {
    size_t i = start + 1;
    if (i < len && s[i] == '/') i++;
    *name_start = i;
    while (i < len && isalnum((unsigned char)s[i])) i++;
    if (i == *name_start) return false;
    *name_len = i - *name_start;
    while (i < len && s[i] != '>') {
        if (s[i] == '<') return false;
        i++;
    }
    if (i < len && s[i] == '>') { *end = i + 1; return true; }
    return false;
}

char *strip_html_tags(const char *s) {
    size_t len = strlen(s);
    Buf out;
    buf_init(&out);
    size_t i = 0;
    while (i < len) {
        if (s[i] == '<') {
            size_t ns, nl, end;
            if (tag_at(s, len, i, &ns, &nl, &end)) {
                char lower[32];
                size_t cl = nl < 31 ? nl : 31;
                for (size_t k = 0; k < cl; k++) lower[k] = (char)tolower((unsigned char)s[ns + k]);
                lower[cl] = 0;
                if (strcmp(lower, "br") == 0) {
                    buf_push_char(&out, ' ');
                    i = end;
                    continue;
                }
                bool fmt = false;
                for (int t = 0; HTML_FORMAT_TAGS[t]; t++)
                    if (strcmp(lower, HTML_FORMAT_TAGS[t]) == 0) { fmt = true; break; }
                if (fmt) { i = end; continue; }
            }
        }
        buf_push_char(&out, s[i]);
        i++;
    }
    return buf_take(&out);
}

static int decode_entity_body(const char *body, size_t len, uint32_t *cp_out) {
    Slice b = { body, len };
    if (slice_eq_cstr(b, "lt")) { *cp_out = '<'; return 1; }
    if (slice_eq_cstr(b, "gt")) { *cp_out = '>'; return 1; }
    if (slice_eq_cstr(b, "amp")) { *cp_out = '&'; return 1; }
    if (slice_eq_cstr(b, "quot")) { *cp_out = '"'; return 1; }
    if (slice_eq_cstr(b, "apos")) { *cp_out = '\''; return 1; }
    if (len == 0 || body[0] != '#') return 0;
    const char *num = body + 1;
    size_t numlen = len - 1;
    int base = 10;
    if (numlen > 0 && (num[0] == 'x' || num[0] == 'X')) { base = 16; num++; numlen--; }
    if (numlen == 0) return 0;
    char tmp[16];
    if (numlen >= sizeof(tmp)) return 0;
    memcpy(tmp, num, numlen);
    tmp[numlen] = 0;
    char *endp;
    unsigned long v = strtoul(tmp, &endp, base);
    if (*endp != 0) return 0;
    if (v < 0x20 || v == 0x7f || (v >= 0x80 && v <= 0x9f)) return 0;
    *cp_out = (uint32_t)v;
    return 1;
}

char *decode_html_entities(const char *s) {
    if (!strchr(s, '&')) return cstr_dup(s);
    uint32_t *cps;
    size_t n = utf8_decode(s, &cps);
    uint32_t *out = malloc((n + 1) * sizeof(uint32_t));
    size_t on = 0;
    size_t i = 0;
    const size_t LOOKAHEAD = 10;
    while (i < n) {
        if (cps[i] != '&') { out[on++] = cps[i]; i++; continue; }
        size_t hi = i + 1 + LOOKAHEAD;
        if (hi > n) hi = n;
        long semi = -1;
        for (size_t j = i + 1; j < hi; j++) if (cps[j] == ';') { semi = (long)j; break; }
        bool decoded = false;
        if (semi >= 0) {
            char bytes[64];
            size_t bl = 0;
            for (size_t j = i + 1; j < (size_t)semi && bl + 4 < sizeof(bytes); j++)
                bl += utf8_encode_cp(cps[j], bytes + bl);
            uint32_t cp;
            if (decode_entity_body(bytes, bl, &cp)) {
                out[on++] = cp;
                i = (size_t)semi + 1;
                decoded = true;
            }
        }
        if (!decoded) { out[on++] = '&'; i++; }
    }
    free(cps);
    char *result = utf8_encode(out, on);
    free(out);
    return result;
}

char *clean_label(const char *raw) {
    char *stripped = strip_html_tags(raw);
    Slice trimmed1 = slice_trim(slice_cstr(stripped));
    bool ok;
    Slice u = slice_strip_prefix(trimmed1, "\"", &ok);
    if (ok) {
        bool ok2;
        Slice u2 = slice_strip_suffix(u, "\"", &ok2);
        if (ok2) u = u2; else u = trimmed1;
    } else {
        Slice u3 = slice_strip_prefix(trimmed1, "'", &ok);
        if (ok) {
            bool ok2;
            Slice u4 = slice_strip_suffix(u3, "'", &ok2);
            u = ok2 ? u4 : trimmed1;
        } else {
            u = trimmed1;
        }
    }
    u = slice_trim(u);
    char *text;
    bool okp;
    Slice md = slice_strip_prefix(u, "`", &okp);
    if (okp) {
        bool oks;
        Slice md2 = slice_strip_suffix(md, "`", &oks);
        if (oks) {
            char *inner = slice_dup(slice_trim(md2));
            text = strip_markdown(inner);
            free(inner);
        } else {
            text = slice_dup(u);
        }
    } else {
        text = slice_dup(u);
    }
    free(stripped);
    char *result = decode_html_entities(text);
    free(text);
    return result;
}

bool is_id_char(char c) {
    return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}

char **wrap_label(const char *label, size_t width, size_t max_lines, size_t *n_out) {
    if (width < 1) width = 1;
    StrVec lines;
    strvec_init(&lines);
    Buf cur;
    buf_init(&cur);
    size_t cur_w = 0;

    Slice rest = slice_cstr(label);
    while (true) {
        rest = slice_trim_start(rest);
        if (slice_is_empty(rest)) break;
        size_t i = 0;
        while (i < rest.len && rest.p[i] != ' ' && rest.p[i] != '\t' && rest.p[i] != '\n' &&
               rest.p[i] != '\r' && rest.p[i] != '\v' && rest.p[i] != '\f') i++;
        Slice word = { rest.p, i };
        rest = (Slice){ rest.p + i, rest.len - i };

        char *wordc = slice_dup(word);
        size_t ww = dwidth_str(wordc);
        if (ww > width) {
            if (cur.len > 0) { strvec_push(&lines, buf_take(&cur)); buf_init(&cur); }
            Buf chunk; buf_init(&chunk);
            size_t chunk_w = 0;
            uint32_t *cps; size_t cn = utf8_decode(wordc, &cps);
            for (size_t k = 0; k < cn; k++) {
                int cw = dwidth_char(cps[k]);
                if (cw < 1) cw = 1;
                if (chunk_w + (size_t)cw > width && chunk.len > 0) {
                    long bp = -1;
                    for (long p = (long)chunk.len - 1; p >= 0; p--) {
                        for (int b = 0; b < 4; b++)
                            if (chunk.data[p] == LABEL_BREAK_CHARS[b]) { bp = p; break; }
                        if (bp >= 0) break;
                    }
                    char *carry;
                    if (bp >= 0) {
                        size_t split = (size_t)bp + 1;
                        carry = cstr_dup(chunk.data + split);
                        chunk.data[split] = 0;
                        chunk.len = split;
                    } else {
                        carry = cstr_dup("");
                    }
                    strvec_push(&lines, buf_take(&chunk));
                    buf_init(&chunk);
                    buf_push_cstr(&chunk, carry);
                    chunk_w = dwidth_str(carry);
                    free(carry);
                }
                char tmp[4];
                size_t tl = utf8_encode_cp(cps[k], tmp);
                for (size_t t = 0; t < tl; t++) buf_push_char(&chunk, tmp[t]);
                chunk_w += (size_t)cw;
            }
            free(cps);
            cur = chunk;
            cur_w = chunk_w;
        } else if (cur.len == 0) {
            buf_push_cstr(&cur, wordc);
            cur_w = ww;
        } else if (cur_w + 1 + ww <= width) {
            buf_push_char(&cur, ' ');
            buf_push_cstr(&cur, wordc);
            cur_w += 1 + ww;
        } else {
            strvec_push(&lines, buf_take(&cur));
            buf_init(&cur);
            buf_push_cstr(&cur, wordc);
            cur_w = ww;
        }
        free(wordc);
    }
    if (cur.len > 0) strvec_push(&lines, buf_take(&cur)); else buf_free(&cur);

    if (lines.n == 0) strvec_push(&lines, cstr_dup(""));

    if (lines.n > max_lines) {
        for (size_t i = max_lines; i < lines.n; i++) free(lines.data[i]);
        lines.n = max_lines;
        char *last = lines.data[lines.n - 1];
        size_t target = width > 0 ? width - 1 : 0;
        if (target < 1) target = 1;
        uint32_t *cps; size_t cn = utf8_decode(last, &cps);
        Buf trunc; buf_init(&trunc);
        size_t sw = 0;
        for (size_t k = 0; k < cn; k++) {
            int cw = dwidth_char(cps[k]);
            if (cw < 0) cw = 0;
            if (sw + (size_t)cw > target) break;
            char tmp[4]; size_t tl = utf8_encode_cp(cps[k], tmp);
            for (size_t t = 0; t < tl; t++) buf_push_char(&trunc, tmp[t]);
            sw += (size_t)cw;
        }
        free(cps);
        buf_push_cstr(&trunc, "\xE2\x80\xA6");
        free(last);
        lines.data[lines.n - 1] = buf_take(&trunc);
    }

    *n_out = lines.n;
    return lines.data;
}

char *fit_label(const char *label, size_t inner) {
    if (dwidth_str(label) <= inner) return cstr_dup(label);
    Buf out; buf_init(&out);
    uint32_t *cps; size_t n = utf8_decode(label, &cps);
    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        int cw = dwidth_char(cps[i]);
        if (used + (size_t)cw + 1 > inner) break;
        char tmp[4]; size_t tl = utf8_encode_cp(cps[i], tmp);
        for (size_t t = 0; t < tl; t++) buf_push_char(&out, tmp[t]);
        used += (size_t)cw;
    }
    free(cps);
    buf_push_cstr(&out, "\xE2\x80\xA6");
    return buf_take(&out);
}
