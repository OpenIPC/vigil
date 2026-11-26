/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_layout.c — live view geometry (see ui_layout.h). */
#include "ui_layout.h"

static int side_of(int layout)
{
    switch (layout) {
    case 1: return 1;
    case 4: return 2;
    case 9: return 3;
    case 16: return 4;
    }
    return 0;
}

int ui_layout_valid(int layout) { return side_of(layout) > 0; }

int ui_layout_pages(int layout, int n_ch)
{
    if (!side_of(layout) || n_ch <= 0) return 1;
    return (n_ch + layout - 1) / layout;
}

int ui_layout_page_of(int layout, int ch)
{
    if (!side_of(layout) || ch < 0) return 0;
    return ch / layout;
}

int ui_layout_cells(int sw, int sh, int layout, int page, int n_ch, ui_cell_t *cells)
{
    int side = side_of(layout);
    if (!side) return 0;
    int pages = ui_layout_pages(layout, n_ch);
    if (page < 0) page = 0;
    if (page >= pages) page = pages - 1;
    int top = UI_HEADER_H, gh = sh - UI_HEADER_H - UI_STRIP_H;
    for (int i = 0; i < layout; ++i) {
        int col = i % side, row = i / side;
        /* Edges at k * size / side, so the cells add up to the grid with
         * no gap; even, as every video layer wants. */
        int x0 = (col * sw / side) & ~1, x1 = ((col + 1) * sw / side) & ~1;
        int y0 = (row * gh / side) & ~1, y1 = ((row + 1) * gh / side) & ~1;
        if (col == side - 1) x1 = sw;
        if (row == side - 1) y1 = gh;
        int ch = page * layout + i;
        cells[i].ch = ch < n_ch ? ch : -1;
        cells[i].x = x0;
        cells[i].y = top + y0;
        cells[i].w = x1 - x0;
        cells[i].h = y1 - y0;
    }
    return layout;
}
