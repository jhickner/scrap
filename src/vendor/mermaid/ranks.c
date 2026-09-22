#include <stdlib.h>
#include <string.h>

#include "internal.h"

typedef struct { IntVec *items; size_t n; } VecList;

static void dfs_dag(int start, IntVec *children, unsigned char *color, IntVec *dag, IntVec *order) {
    typedef struct { int u; size_t k; } Frame;
    Frame *stack = malloc(sizeof(Frame) * 4096);
    size_t cap = 4096, top = 0;
    stack[top++] = (Frame){ start, 0 };
    color[start] = 1;
    while (top > 0) {
        Frame *f = &stack[top - 1];
        int u = f->u;
        if (f->k < (size_t)children[u].n) {
            int v = children[u].data[f->k];
            f->k++;
            if (color[v] == 1) continue;
            intvec_push(&dag[u], v);
            if (color[v] == 0) {
                color[v] = 1;
                if (top == cap) { cap *= 2; stack = realloc(stack, sizeof(Frame) * cap); f = &stack[top - 1]; }
                stack[top++] = (Frame){ v, 0 };
            }
        } else {
            color[u] = 2;
            intvec_push(order, u);
            top--;
        }
    }
    free(stack);
}

size_t *compute_ranks(const Graph *g) {
    size_t n = g->n_nodes;
    size_t *rank = calloc(n ? n : 1, sizeof(size_t));
    if (n == 0) return rank;

    IntVec *children = calloc(n, sizeof(IntVec));
    int *indeg = calloc(n, sizeof(int));
    for (size_t i = 0; i < n; i++) intvec_init(&children[i]);
    for (size_t e = 0; e < g->n_edges; e++) {
        const Edge *ed = &g->edges[e];
        if (ed->from != ed->to) { intvec_push(&children[ed->from], ed->to); indeg[ed->to]++; }
    }

    unsigned char *color = calloc(n, 1);
    IntVec *dag = calloc(n, sizeof(IntVec));
    IntVec order;
    for (size_t i = 0; i < n; i++) intvec_init(&dag[i]);
    intvec_init(&order);

    for (size_t i = 0; i < n; i++) {
        if (indeg[i] == 0 && color[i] == 0) dfs_dag((int)i, children, color, dag, &order);
    }
    for (size_t i = 0; i < n; i++) {
        if (color[i] == 0) dfs_dag((int)i, children, color, dag, &order);
    }

    for (long i = (long)order.n - 1; i >= 0; i--) {
        int u = order.data[i];
        for (size_t k = 0; k < (size_t)dag[u].n; k++) {
            int v = dag[u].data[k];
            if (rank[v] < rank[u] + 1) rank[v] = rank[u] + 1;
        }
    }

    for (size_t i = 0; i < n; i++) { intvec_free(&children[i]); intvec_free(&dag[i]); }
    free(children); free(dag); free(indeg); free(color);
    intvec_free(&order);
    return rank;
}

size_t count_crossings(const Graph *g, const size_t *ranks, const size_t *pos) {
    typedef struct { size_t rank, p1, p2; } Adj;
    Adj *adj = malloc(sizeof(Adj) * (g->n_edges + 1));
    size_t na = 0;
    for (size_t e = 0; e < g->n_edges; e++) {
        const Edge *ed = &g->edges[e];
        if (ed->from != ed->to && ranks[ed->to] == ranks[ed->from] + 1) {
            adj[na++] = (Adj){ ranks[ed->from], pos[ed->from], pos[ed->to] };
        }
    }
    size_t crossings = 0;
    for (size_t i = 0; i < na; i++) {
        for (size_t j = i + 1; j < na; j++) {
            if (adj[i].rank == adj[j].rank) {
                bool a = adj[i].p1 < adj[j].p1 && adj[i].p2 > adj[j].p2;
                bool b = adj[i].p1 > adj[j].p1 && adj[i].p2 < adj[j].p2;
                if (a || b) crossings++;
            }
        }
    }
    free(adj);
    return crossings;
}

static int cmp_key(const void *a, const void *b) {
    const double *ka = a, *kb = b;
    if (ka[0] < kb[0]) return -1;
    if (ka[0] > kb[0]) return 1;
    return 0;
}

static void sort_by_barycenter(int *row, size_t row_n, IntVec *neigh, const size_t *pos) {
    double (*keyed)[2] = malloc(sizeof(double[2]) * row_n);
    for (size_t i = 0; i < row_n; i++) {
        int v = row[i];
        double key;
        if (neigh[v].n == 0) key = (double)pos[v];
        else {
            double sum = 0;
            for (size_t k = 0; k < neigh[v].n; k++) sum += (double)pos[neigh[v].data[k]];
            key = sum / (double)neigh[v].n;
        }
        keyed[i][0] = key;
        keyed[i][1] = (double)v;
    }
    qsort(keyed, row_n, sizeof(double[2]), cmp_key);
    for (size_t i = 0; i < row_n; i++) row[i] = (int)keyed[i][1];
    free(keyed);
}

void order_ranks(size_t **by_rank, size_t *by_rank_n, size_t n_ranks, const Graph *g, const size_t *ranks) {
    size_t n = g->n_nodes;
    if (n_ranks < 2 || n < 3) return;

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

    size_t *pos = calloc(n, sizeof(size_t));
    for (size_t r = 0; r < n_ranks; r++)
        for (size_t i = 0; i < by_rank_n[r]; i++) pos[by_rank[r][i]] = i;

    size_t **best = malloc(sizeof(size_t *) * n_ranks);
    size_t *best_n = malloc(sizeof(size_t) * n_ranks);
    for (size_t r = 0; r < n_ranks; r++) {
        best_n[r] = by_rank_n[r];
        best[r] = malloc(sizeof(size_t) * (best_n[r] ? best_n[r] : 1));
        memcpy(best[r], by_rank[r], sizeof(size_t) * best_n[r]);
    }
    size_t best_crossings = count_crossings(g, ranks, pos);

    if (best_crossings != 0) {
        int *row_i = malloc(sizeof(int) * n);
        for (int it = 0; it < 8 && best_crossings != 0; it++) {
            if (it % 2 == 0) {
                for (size_t r = 1; r < n_ranks; r++) {
                    for (size_t k = 0; k < by_rank_n[r]; k++) row_i[k] = (int)by_rank[r][k];
                    sort_by_barycenter(row_i, by_rank_n[r], parents, pos);
                    for (size_t k = 0; k < by_rank_n[r]; k++) { by_rank[r][k] = (size_t)row_i[k]; pos[by_rank[r][k]] = k; }
                }
            } else {
                for (long r = (long)n_ranks - 2; r >= 0; r--) {
                    for (size_t k = 0; k < by_rank_n[r]; k++) row_i[k] = (int)by_rank[r][k];
                    sort_by_barycenter(row_i, by_rank_n[r], children, pos);
                    for (size_t k = 0; k < by_rank_n[r]; k++) { by_rank[r][k] = (size_t)row_i[k]; pos[by_rank[r][k]] = k; }
                }
            }
            size_t crossings = count_crossings(g, ranks, pos);
            if (crossings < best_crossings) {
                best_crossings = crossings;
                for (size_t r = 0; r < n_ranks; r++) memcpy(best[r], by_rank[r], sizeof(size_t) * by_rank_n[r]);
            }
        }
        free(row_i);
    }

    for (size_t r = 0; r < n_ranks; r++) { memcpy(by_rank[r], best[r], sizeof(size_t) * by_rank_n[r]); free(best[r]); }
    free(best); free(best_n); free(pos);
    for (size_t i = 0; i < n; i++) { intvec_free(&parents[i]); intvec_free(&children[i]); }
    free(parents); free(children);
}
