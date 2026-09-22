#include <stdlib.h>
#include <string.h>

#include "canvas.h"
#include "strutil.h"

MermaidArt *render_class(const Graph *g, const ClassInfo *infos, size_t n_infos, int max_width, Oversize *err) {
    (void)n_infos;
    size_t n = g->n_nodes;
    NodeExtra *extras = calloc(n ? n : 1, sizeof(NodeExtra));
    for (size_t i = 0; i < n; i++) {
        const ClassInfo *info = &infos[i];
        char **title;
        size_t title_n = 0;
        size_t cap = 2;
        title = malloc(cap * sizeof(char *));
        if (info->annotation) {
            Buf b; buf_init(&b);
            buf_push_cstr(&b, "\xC2\xAB");
            buf_push_cstr(&b, info->annotation);
            buf_push_cstr(&b, "\xC2\xBB");
            title[title_n++] = buf_take(&b);
        }
        title[title_n++] = display_generics(g->nodes[i].label);

        extras[i].kind = EXTRA_COMPARTMENTS;
        extras[i].n_sections = 3;
        extras[i].sections = malloc(3 * sizeof(char **));
        extras[i].section_n = malloc(3 * sizeof(size_t));
        extras[i].sections[0] = title;
        extras[i].section_n[0] = title_n;

        extras[i].sections[1] = malloc((info->n_attrs ? info->n_attrs : 1) * sizeof(char *));
        for (size_t k = 0; k < info->n_attrs; k++) extras[i].sections[1][k] = cstr_dup(info->attrs[k]);
        extras[i].section_n[1] = info->n_attrs;

        extras[i].sections[2] = malloc((info->n_methods ? info->n_methods : 1) * sizeof(char *));
        for (size_t k = 0; k < info->n_methods; k++) extras[i].sections[2][k] = cstr_dup(info->methods[k]);
        extras[i].section_n[2] = info->n_methods;
    }

    Canvas *canvas = layout_canvas(g, extras, max_width, err);

    for (size_t i = 0; i < n; i++) {
        for (size_t s = 0; s < extras[i].n_sections; s++) {
            for (size_t k = 0; k < extras[i].section_n[s]; k++) free(extras[i].sections[s][k]);
            free(extras[i].sections[s]);
        }
        free(extras[i].sections);
        free(extras[i].section_n);
    }
    free(extras);

    if (!canvas) return NULL;
    if (g->dir == DIR_UP) canvas_flip_vertical(canvas);
    else if (g->dir == DIR_LEFT) canvas_flip_horizontal(canvas);
    MermaidArt *art = mermaid_art_from_canvas(canvas);
    canvas_free(canvas);
    return art;
}
