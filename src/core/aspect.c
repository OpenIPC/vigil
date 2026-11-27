/* SPDX-License-Identifier: GPL-3.0-or-later */
/* aspect.c — the picture's display aspect ratio and its place in a tile
 * (aspect.h). */
#include "aspect.h"

static const vg_ratio_t k_common[] = {
    { 16, 9 }, { 4, 3 }, { 16, 10 }, { 5, 4 }, { 3, 2 }, { 1, 1 }, { 2, 1 }, { 32, 9 },
};

static unsigned long long gcd(unsigned long long a, unsigned long long b)
{
    while (b) {
        unsigned long long t = a % b;
        a = b;
        b = t;
    }
    return a;
}

vg_ratio_t aspect_of_stream(const vg_stream_info_t *si)
{
    vg_ratio_t r = { 0, 0 };
    if (!si || !si->width || !si->height) return r;
    unsigned long long w = si->width, h = si->height;
    if (si->sar_w && si->sar_h) {
        w *= si->sar_w;
        h *= si->sar_h;
    }
    /* |w/h - c| <= 2 % of c */
    for (unsigned i = 0; i < sizeof k_common / sizeof k_common[0]; ++i) {
        unsigned long long a = w * k_common[i].h, b = h * k_common[i].w;
        if ((a > b ? a - b : b - a) * 50 <= b) return k_common[i];
    }
    unsigned long long g = gcd(w, h);
    w /= g;
    h /= g;
    while (w > 0xffffffffull || h > 0xffffffffull) {     /* a SAR of 65535:1 and the like */
        w >>= 1;
        h >>= 1;
    }
    r.w = (unsigned)w;
    r.h = (unsigned)h;
    if (!r.w || !r.h) r = (vg_ratio_t){ 0, 0 };
    return r;
}

vg_ratio_t aspect_of_camera(const vg_stream_info_t *shown, int is_sub, const vg_stream_info_t *main)
{
    if (is_sub && main) {
        vg_ratio_t m = aspect_of_stream(main);
        if (m.w) return m;
    }
    return aspect_of_stream(shown);
}

static unsigned align_down(unsigned v, unsigned a) { return v / a * a; }

/* v rounded to the nearest multiple of a, at least a, at most max */
static unsigned align_near(unsigned long long v, unsigned a, unsigned max)
{
    unsigned long long r = (v + a / 2) / a * a;
    if (r < a) r = a;
    if (r > max) r = max;
    return (unsigned)r;
}

vg_rect_t aspect_fit(const vg_rect_t *t, vg_ratio_t a, unsigned align)
{
    if (!align) align = 1;
    unsigned tw = t->w > 0 ? (unsigned)t->w : 0, th = t->h > 0 ? (unsigned)t->h : 0;
    unsigned fw = align_down(tw, align), fh = align_down(th, align);
    if (fw < align) fw = align;
    if (fh < align) fh = align;
    unsigned w = fw, h = fh;
    if (a.w && a.h) {
        if ((unsigned long long)a.w * fh > (unsigned long long)a.h * fw)
            h = align_near((unsigned long long)fw * a.h / a.w, align, fh);   /* bars above and below */
        else
            w = align_near((unsigned long long)fh * a.w / a.h, align, fw);   /* bars at the sides */
    }
    vg_rect_t r = { t->x, t->y, (int)w, (int)h };
    if (tw > w) r.x += (int)align_down((tw - w) / 2, align);
    if (th > h) r.y += (int)align_down((th - h) / 2, align);
    return r;
}

vg_rect_t aspect_cover(const vg_rect_t *t, vg_ratio_t a)
{
    vg_rect_t r = *t;
    if (!a.w || !a.h || t->w <= 0 || t->h <= 0) return r;
    unsigned long long tw = (unsigned)t->w, th = (unsigned)t->h;
    if (a.w * th > a.h * tw)
        r.w = (int)((th * a.w + a.h / 2) / a.h);      /* wider: over the sides */
    else
        r.h = (int)((tw * a.h + a.w / 2) / a.w);      /* taller: above and below */
    r.x -= (r.w - t->w) / 2;
    r.y -= (r.h - t->h) / 2;
    return r;
}

/* The num/den share of `src` pixels that shows, centred: its offset and
 * size, multiples of `align` (the size at least `align`). */
static void crop1(unsigned src, unsigned long long num, unsigned long long den, unsigned align, int *off,
                  int *len)
{
    unsigned most = align_down(src, align);
    if (most < align) {                             /* a picture smaller than align */
        *off = 0;
        *len = (int)src;
        return;
    }
    unsigned v = align_near((src * num + den / 2) / den, align, most);
    *len = (int)v;
    *off = (int)align_down((src - v) / 2, align);
}

vg_rect_t aspect_crop(unsigned src_w, unsigned src_h, vg_ratio_t a, unsigned tile_w, unsigned tile_h,
                      unsigned align)
{
    if (!align) align = 1;
    vg_rect_t r = { 0, 0, (int)src_w, (int)src_h };
    if (!src_w || !src_h || !tile_w || !tile_h) return r;
    if (!a.w || !a.h) a = (vg_ratio_t){ src_w, src_h };
    /* shown at a, the picture is a.w/a.h wide for its height; the tile
     * tile_w/tile_h: the share of the picture that shows one way is the
     * ratio of the two */
    unsigned long long p = (unsigned long long)a.w * tile_h, q = (unsigned long long)a.h * tile_w;
    if (p > q)
        crop1(src_w, q, p, align, &r.x, &r.w);      /* wider: the sides cut */
    else if (p < q)
        crop1(src_h, p, q, align, &r.y, &r.h);      /* taller: top and bottom cut */
    return r;
}

vg_rect_t aspect_map(const vg_rect_t *r, unsigned from_w, unsigned from_h, unsigned to_w, unsigned to_h,
                     unsigned align)
{
    if (!align) align = 1;
    if (!from_w || !from_h) return *r;
    long long x0 = (long long)r->x * to_w / from_w, x1 = (long long)(r->x + r->w) * to_w / from_w;
    long long y0 = (long long)r->y * to_h / from_h, y1 = (long long)(r->y + r->h) * to_h / from_h;
    x0 = x0 / align * align;
    x1 = x1 / align * align;
    y0 = y0 / align * align;
    y1 = y1 / align * align;
    return (vg_rect_t){ (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0) };
}

static unsigned scaled1(unsigned src, unsigned want, unsigned align, unsigned max_shrink)
{
    if (!want || want >= src) return src;
    unsigned o = align_down(want, align);
    unsigned least = max_shrink ? (src + max_shrink - 1) / max_shrink : 1;
    least = (least + align - 1) / align * align;
    if (o < least) o = least;
    if (o < align) o = align;
    return o < src ? o : src;
}

void aspect_scaled_size(unsigned src_w, unsigned src_h, unsigned want_w, unsigned want_h, unsigned align,
                        unsigned max_shrink, unsigned *out_w, unsigned *out_h)
{
    if (!align) align = 1;
    *out_w = scaled1(src_w, want_w, align, max_shrink);
    *out_h = scaled1(src_h, want_h, align, max_shrink);
}
