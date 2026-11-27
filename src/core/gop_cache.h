/* SPDX-License-Identifier: GPL-3.0-or-later */
/* gop_cache.h — the current GOP of a stream, for a decoder that opens
 * mid-GOP.
 *
 * A decoder can only start at a keyframe; a camera sends one every 1-4 s.
 * A session that keeps the access units since its last keyframe (as
 * au_norm hands them out: the keyframe carries its parameter sets) can
 * open a decoder at once and feed it the GOP so far, then go on live, so
 * switching between a camera's streams, or bringing a channel back on
 * screen, does not wait for the camera's next keyframe.
 *
 * Each cache has a cap, and all caches together a budget (gop_cache_budget):
 * a GOP that outgrows either is dropped and nothing is kept until the next
 * keyframe; a switch meanwhile waits for that keyframe as it would
 * without a cache. The AUs go back to back into one buffer per cache, kept
 * from GOP to GOP and grown by doubling, so caching is a copy per AU and
 * no allocation once a GOP's size is reached.
 *
 * One thread per cache; the budget is shared (atomic).
 */
#ifndef GOP_CACHE_H
#define GOP_CACHE_H

#include <stddef.h>
#include <stdint.h>

#include "vg_types.h"

/* All caches together, unless gop_cache_set_budget says otherwise. */
#ifndef GOP_CACHE_BUDGET
#define GOP_CACHE_BUDGET (24u << 20)
#endif
/* The most AUs a GOP may have (20 s at 25 fps and then some). */
#define GOP_CACHE_AUS    1024

typedef struct {
    uint32_t off, len;
    uint64_t pts_us;
} gop_au_t;

typedef struct {
    size_t      limit;              /* this cache's cap, data and index */
    uint8_t    *buf;                /* the AUs, back to back */
    size_t      len, cap;
    gop_au_t   *au;
    unsigned    n, au_cap;
    vg_codec_t  codec;
    unsigned    gen;                /* au_norm info_gen at the keyframe */
    int         valid;              /* holds a keyframe and every AU since */
} gop_cache_t;

/* What gop_cache_add did. */
enum { GOP_KEPT = 0, GOP_IDLE = 1, GOP_OVER_CAP = -1, GOP_OVER_BUDGET = -2 };

void gop_cache_init(gop_cache_t *g, size_t limit);
/* Give back the memory; nothing is kept until the next keyframe. */
void gop_cache_free(gop_cache_t *g);

/* One normalized AU of the stream (`gen`: its au_norm info_gen). A
 * keyframe starts the GOP over. GOP_IDLE: not kept, there is no keyframe
 * yet; GOP_OVER_*: the GOP did not fit and is dropped (its memory given
 * back) until the next keyframe. */
int  gop_cache_add(gop_cache_t *g, const vg_au_t *au, unsigned gen);

/* A whole GOP so far: its keyframe and every AU after it. */
static inline int gop_cache_ready(const gop_cache_t *g) { return g->valid && g->n > 0; }

/* AU i of the GOP (0: the keyframe), pointing into the cache. */
void gop_cache_get(const gop_cache_t *g, unsigned i, vg_au_t *au);

/* Bytes all caches hold now, and their budget. Set the budget before
 * caching starts (tests). */
size_t gop_cache_used(void);
void   gop_cache_set_budget(size_t bytes);

#endif /* GOP_CACHE_H */
