#include <stdlib.h>
#include <string.h>

#include "internal.h"
#include "sequence.h"
#include "strutil.h"

void mermaid_art_free(MermaidArt *art) {
    if (!art) return;
    for (size_t i = 0; i < art->n; i++) free(art->lines[i]);
    free(art->lines);
    free(art);
}

MermaidArt *mermaid_render(const char *src, int max_width) {
    Slice trimmed = slice_trim(slice_cstr(src));
    if (slice_is_empty(trimmed)) return NULL;

    Oversize err = OVERSIZE_NONE;
    MermaidArt *art = NULL;
    bool matched = false;

    Graph *g = parse_graph(src);
    if (g) {
        matched = true;
        art = g->n_groups == 0 ? layout_flowchart(g, max_width, &err) : render_grouped(g, max_width, &err);
        graph_free(g);
    }

    if (!matched) {
        g = parse_state(src);
        if (g) {
            matched = true;
            art = layout_flowchart(g, max_width, &err);
            graph_free(g);
        }
    }

    if (!matched) {
        ClassInfo *infos; size_t n_infos;
        g = parse_class(src, &infos, &n_infos);
        if (g) {
            matched = true;
            art = render_class(g, infos, n_infos, max_width, &err);
            graph_free(g);
            class_infos_free(infos, n_infos);
        }
    }

    if (!matched) {
        ClassInfo *infos; size_t n_infos;
        g = parse_er(src, &infos, &n_infos);
        if (g) {
            matched = true;
            art = render_class(g, infos, n_infos, max_width, &err);
            graph_free(g);
            class_infos_free(infos, n_infos);
        }
    }

    if (!matched) {
        Sequence *seq = parse_sequence(src);
        if (seq) {
            matched = true;
            art = layout_sequence(seq, max_width, &err);
            sequence_free(seq);
        }
    }

    if (matched) {
        if (art) return art;
        bool too_wide = err == OVERSIZE_WIDTH;
        return fallback(src, max_width, too_wide);
    }

    return fallback(src, max_width, false);
}
