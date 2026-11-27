/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gop_cache.c — the current GOP of a stream (see gop_cache.h). */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "gop_cache.h"

#define GOP_FIRST_BYTES (64u << 10)     /* a CIF GOP fits; doubled from there */
#define GOP_FIRST_AUS   64

static atomic_size_t g_used;
static size_t        g_budget = GOP_CACHE_BUDGET;

size_t gop_cache_used(void) { return atomic_load(&g_used); }
size_t gop_cache_budget(void) { return g_budget; }
void   gop_cache_set_budget(size_t bytes) { g_budget = bytes; }

/* Take `more` bytes of the budget; 0, or -1 when that would exceed it. */
static int take(size_t more)
{
    size_t u = atomic_load(&g_used);
    do {
        if (u + more > g_budget) return -1;
    } while (!atomic_compare_exchange_weak(&g_used, &u, u + more));
    return 0;
}

static void give(size_t less) { atomic_fetch_sub(&g_used, less); }

/* What the cache holds of the budget. */
static size_t held(const gop_cache_t *g) { return g->cap + (size_t)g->au_cap * sizeof *g->au; }

void gop_cache_init(gop_cache_t *g, size_t limit)
{
    memset(g, 0, sizeof *g);
    g->limit = limit;
}

void gop_cache_free(gop_cache_t *g)
{
    give(held(g));
    free(g->buf);
    free(g->au);
    gop_cache_init(g, g->limit);
}

/* Room for `bytes` more data and one more AU, within the cap and the
 * budget. */
static int grow(gop_cache_t *g, size_t bytes)
{
    size_t need = g->len + bytes;
    if (g->n >= GOP_CACHE_AUS) return GOP_OVER_CAP;
    size_t cap = g->cap, au_cap = g->au_cap;
    if (need > cap) {
        if (!cap) cap = GOP_FIRST_BYTES;
        while (cap < need) cap *= 2;
    }
    if (g->n >= au_cap) au_cap = au_cap ? au_cap * 2 : GOP_FIRST_AUS;
    if (cap == g->cap && au_cap == g->au_cap) return GOP_KEPT;
    size_t index = (size_t)au_cap * sizeof *g->au;
    if (cap + index > g->limit) {
        /* The cap, not a power of two: what is left of it after the index. */
        if (need + index > g->limit) return GOP_OVER_CAP;
        cap = g->limit - index;
    }
    size_t more = cap + index - held(g);
    if (take(more) < 0) return GOP_OVER_BUDGET;
    /* Out of memory counts as out of budget: what was taken and not
     * used goes back, the rest with the cache (gop_cache_free). */
    if (cap != g->cap) {
        uint8_t *b = realloc(g->buf, cap);
        if (!b) {
            give(more);
            return GOP_OVER_BUDGET;
        }
        g->buf = b;
        more -= cap - g->cap;
        g->cap = cap;
    }
    if (au_cap != g->au_cap) {
        gop_au_t *a = realloc(g->au, index);
        if (!a) {
            give(more);
            return GOP_OVER_BUDGET;
        }
        g->au = a;
        g->au_cap = au_cap;
    }
    return GOP_KEPT;
}

int gop_cache_add(gop_cache_t *g, const vg_au_t *au, unsigned gen)
{
    if (au->key) {
        g->len = 0;
        g->n = 0;
        g->valid = 1;
        g->codec = au->codec;
        g->gen = gen;
    } else if (!g->valid) {
        return GOP_IDLE;
    }
    /* A new stream without a keyframe: nothing a decoder could start from. */
    int rc = au->codec != g->codec || gen != g->gen ? GOP_OVER_CAP
           : au->len > UINT32_MAX ? GOP_OVER_CAP : grow(g, au->len);
    if (rc != GOP_KEPT) {
        gop_cache_free(g);
        return rc;
    }
    memcpy(g->buf + g->len, au->data, au->len);
    g->au[g->n++] = (gop_au_t){ (uint32_t)g->len, (uint32_t)au->len, au->pts_us };
    g->len += au->len;
    return GOP_KEPT;
}

void gop_cache_get(const gop_cache_t *g, unsigned i, vg_au_t *au)
{
    memset(au, 0, sizeof *au);
    if (i >= g->n) return;
    au->codec = g->codec;
    au->data = g->buf + g->au[i].off;
    au->len = g->au[i].len;
    au->pts_us = g->au[i].pts_us;
    au->key = i == 0;
}
