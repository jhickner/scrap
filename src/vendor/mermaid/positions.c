#include <math.h>
#include <stdlib.h>

#include "internal.h"

static void relax_rank(const size_t *nodes, size_t nn, IntVec *neigh, double *pos, const size_t *size, size_t sep) {
    if (nn == 0) return;
    double *desired = malloc(sizeof(double) * nn);
    for (size_t i = 0; i < nn; i++) {
        size_t v = nodes[i];
        if (neigh[v].n == 0) desired[i] = pos[v];
        else {
            double sum = 0;
            for (size_t k = 0; k < neigh[v].n; k++) sum += pos[neigh[v].data[k]];
            desired[i] = sum / (double)neigh[v].n;
        }
    }
    double half_of[64];
    double *half = nn <= 64 ? half_of : malloc(sizeof(double) * nn);
    for (size_t i = 0; i < nn; i++) half[i] = (double)size[nodes[i]] / 2.0;

    double *left = malloc(sizeof(double) * nn);
    double *right = malloc(sizeof(double) * nn);
    for (size_t i = 0; i < nn; i++) {
        if (i == 0) left[i] = desired[i];
        else {
            double m = left[i - 1] + half[i - 1] + (double)sep + half[i];
            left[i] = desired[i] > m ? desired[i] : m;
        }
    }
    for (long i = (long)nn - 1; i >= 0; i--) {
        if ((size_t)i == nn - 1) right[i] = desired[i];
        else {
            double m = right[i + 1] - half[i + 1] - (double)sep - half[i];
            right[i] = desired[i] < m ? desired[i] : m;
        }
    }
    for (size_t i = 0; i < nn; i++) pos[nodes[i]] = (left[i] + right[i]) / 2.0;
    for (size_t i = 1; i < nn; i++) {
        double min_p = pos[nodes[i - 1]] + half[i - 1] + (double)sep + half[i];
        if (pos[nodes[i]] < min_p) pos[nodes[i]] = min_p;
    }
    free(desired); free(left); free(right);
    if (half != half_of) free(half);
}

size_t *assign_positions(size_t **by_rank, const size_t *by_rank_n, size_t n_ranks, const size_t *size, size_t n,
                          size_t sep, const Graph *g, const size_t *ranks) {
    IntVec *parents = calloc(n, sizeof(IntVec));
    IntVec *children = calloc(n, sizeof(IntVec));
    for (size_t i = 0; i < n; i++) { intvec_init(&parents[i]); intvec_init(&children[i]); }
    for (size_t e = 0; e < g->n_edges; e++) {
        const Edge *ed = &g->edges[e];
        if (ed->from != ed->to && ranks[ed->to] > ranks[ed->from]) {
            intvec_push(&parents[ed->to], ed->from);
            intvec_push(&children[ed->from], ed->to);
        }
    }

    double *pos = calloc(n ? n : 1, sizeof(double));
    for (size_t r = 0; r < n_ranks; r++) {
        double x = 0;
        for (size_t k = 0; k < by_rank_n[r]; k++) {
            size_t v = by_rank[r][k];
            double half = (double)size[v] / 2.0;
            x += half;
            pos[v] = x;
            x += half + (double)sep;
        }
    }

    for (int it = 0; it < 10; it++) {
        if (it % 2 == 0) {
            for (size_t r = 0; r < n_ranks; r++) relax_rank(by_rank[r], by_rank_n[r], parents, pos, size, sep);
        } else {
            for (long r = (long)n_ranks - 1; r >= 0; r--) relax_rank(by_rank[r], by_rank_n[r], children, pos, size, sep);
        }
    }

    double min_left = INFINITY;
    for (size_t v = 0; v < n; v++) {
        double l = pos[v] - (double)size[v] / 2.0;
        if (l < min_left) min_left = l;
    }
    if (!isfinite(min_left)) min_left = 0.0;

    size_t *out = malloc(sizeof(size_t) * (n ? n : 1));
    for (size_t v = 0; v < n; v++) {
        double val = round(pos[v] - min_left);
        if (val < 0) val = 0;
        out[v] = (size_t)val;
    }

    free(pos);
    for (size_t i = 0; i < n; i++) { intvec_free(&parents[i]); intvec_free(&children[i]); }
    free(parents); free(children);
    return out;
}
