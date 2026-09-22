#ifndef MERMAID_H
#define MERMAID_H

#include <stddef.h>

typedef struct {
    char **lines;
    size_t n;
} MermaidArt;

/* Render mermaid source. Returns NULL for blank input. max_width <= 0 means unlimited. */
MermaidArt *mermaid_render(const char *src, int max_width);
void mermaid_art_free(MermaidArt *art);

#endif
