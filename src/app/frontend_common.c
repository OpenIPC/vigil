/* SPDX-License-Identifier: GPL-3.0-or-later */
/* frontend_common.c — the quad-view geometry shared by the UI and the
 * decoders' tiles. */
#include "frontend.h"

void frontend_tiles(vg_rect_t *tiles, int n)
{
    int half_w = SCREEN_W / 2;
    int half_h = (SCREEN_H - HEADER_H - STRIP_H) / 2;
    for (int i = 0; i < n; ++i) {
        int q = i % 4;                          /* quad view: tiles 5+ repeat */
        tiles[i].x = (q % 2) * half_w;
        tiles[i].y = HEADER_H + (q / 2) * half_h;
        tiles[i].w = half_w & ~1;
        tiles[i].h = half_h & ~1;
    }
}
