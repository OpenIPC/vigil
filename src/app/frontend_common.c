/* SPDX-License-Identifier: GPL-3.0-or-later */
/* frontend_common.c — the tiles the channels start in: the live view's
 * first layout (2x2, page 1), from the geometry the UI uses (ui_layout.c),
 * so the video and the UI's holes agree. */
#include <string.h>

#include "frontend.h"
#include "ui_layout.h"

void frontend_tiles(vg_rect_t *tiles, int n)
{
    ui_cell_t cells[4];
    int k = ui_layout_cells(SCREEN_W, SCREEN_H, 4, 0, n, cells);
    memset(tiles, 0, sizeof *tiles * (size_t)n);    /* past the first page: not shown */
    for (int i = 0; i < k; ++i)
        if (cells[i].ch >= 0) tiles[cells[i].ch] = (vg_rect_t){ cells[i].x, cells[i].y, cells[i].w, cells[i].h };
}
