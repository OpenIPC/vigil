/* SPDX-License-Identifier: GPL-3.0-or-later */
/* overscan.c — the inner rectangle the output's margins leave, and the
 * screen's coordinates placed in it (overscan.h). */
#include "overscan.h"

#include "aspect.h"

int overscan_none(const vg_margins_t *m)
{
    return !m || (!m->top && !m->bottom && !m->left && !m->right);
}

static unsigned edge(unsigned size, unsigned pct, unsigned align)
{
    if (pct > OVERSCAN_MAX_PCT) pct = OVERSCAN_MAX_PCT;
    unsigned px = (unsigned)((unsigned long long)size * pct / 100);
    return px / align * align;
}

vg_rect_t overscan_inner(unsigned w, unsigned h, const vg_margins_t *m, unsigned align)
{
    if (!align) align = 1;
    if (overscan_none(m)) return (vg_rect_t){ 0, 0, (int)w, (int)h };
    unsigned l = edge(w, m->left, align), r = edge(w, m->right, align);
    unsigned t = edge(h, m->top, align), b = edge(h, m->bottom, align);
    unsigned iw = (w - l - r) / align * align, ih = (h - t - b) / align * align;
    return (vg_rect_t){ (int)l, (int)t, (int)iw, (int)ih };
}

vg_rect_t overscan_map(const vg_rect_t *r, unsigned from_w, unsigned from_h, const vg_rect_t *inner,
                       unsigned align)
{
    vg_rect_t o = aspect_map(r, from_w, from_h, (unsigned)inner->w, (unsigned)inner->h, align);
    o.x += inner->x;
    o.y += inner->y;
    return o;
}

void overscan_point(int x, int y, unsigned from_w, unsigned from_h, const vg_rect_t *inner, int *ox, int *oy)
{
    *ox = inner->x + (from_w ? (int)((long long)x * inner->w / (long long)from_w) : x);
    *oy = inner->y + (from_h ? (int)((long long)y * inner->h / (long long)from_h) : y);
}

static long long unscale(long long v, int at, int size, unsigned to)
{
    v -= at;
    if (v < 0) v = 0;
    if (v > size) v = size;
    return size > 0 ? v * to / size : 0;
}

vg_rect_t overscan_unmap(const vg_rect_t *r, const vg_rect_t *inner, unsigned to_w, unsigned to_h)
{
    long long x0 = unscale(r->x, inner->x, inner->w, to_w), x1 = unscale((long long)r->x + r->w, inner->x, inner->w, to_w);
    long long y0 = unscale(r->y, inner->y, inner->h, to_h), y1 = unscale((long long)r->y + r->h, inner->y, inner->h, to_h);
    return (vg_rect_t){ (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0) };
}
