#ifndef DWIDTH_H
#define DWIDTH_H

#include <stddef.h>
#include <stdint.h>

int dwidth_char(uint32_t cp);
size_t dwidth_str(const char *utf8);
size_t dwidth_n(const uint32_t *cps, size_t n);

size_t utf8_decode(const char *s, uint32_t **out);
size_t utf8_decode_n(const char *s, size_t byte_len, uint32_t **out);
size_t utf8_encode_cp(uint32_t cp, char out[4]);
char *utf8_encode(const uint32_t *cps, size_t n);

#endif
