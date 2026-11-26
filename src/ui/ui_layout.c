/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_layout.c — live view geometry (see ui_layout.h). */
#include "ui_layout.h"

const int ui_layouts[] = { 1, 4, 8, 9, 16, 32, 64 };
const int ui_n_layouts = (int)(sizeof ui_layouts / sizeof ui_layouts[0]);

/* 8: windows as (column, row, columns, rows) of a 4 x 4 grid. */
static const unsigned char k_eight[8][4] = {
    { 0, 0, 3, 3 }, { 3, 0, 1, 1 }, { 3, 1, 1, 1 }, { 3, 2, 1, 1 },
    { 0, 3, 1, 1 }, { 1, 3, 1, 1 }, { 2, 3, 1, 1 }, { 3, 3, 1, 1 },
};

/* The grid a layout is cut from. */
static int grid_of(int layout, int *cols, int *rows)
{
    switch (layout) {
    case 1:  *cols = *rows = 1; return 1;
    case 4:  *cols = *rows = 2; return 1;
    case 8:  *cols = *rows = 4; return 1;
    case 9:  *cols = *rows = 3; return 1;
    case 16: *cols = *rows = 4; return 1;
    case 32: *cols = 8; *rows = 4; return 1;
    case 64: *cols = *rows = 8; return 1;
    }
    return 0;
}

int ui_layout_valid(int layout)
{
    int c, r;
    return grid_of(layout, &c, &r);
}

const char *ui_layout_name(int layout)
{
    switch (layout) {
    case 1:  return "1x1";
    case 4:  return "2x2";
    case 8:  return "1+7";
    case 9:  return "3x3";
    case 16: return "4x4";
    case 32: return "8x4";
    case 64: return "8x8";
    }
    return "?";
}

int ui_layout_pages(int layout, int n_ch)
{
    if (!ui_layout_valid(layout) || n_ch <= 0) return 1;
    return (n_ch + layout - 1) / layout;
}

int ui_layout_page_of(int layout, int ch)
{
    if (!ui_layout_valid(layout) || ch < 0) return 0;
    return ch / layout;
}

/* Edge k of n across `size`: aligned as the video layers want them, the
 * last one the screen's edge, so the cells add up with no gap. */
static int edge(int k, int n, int size, int align)
{
    return k >= n ? size : (k * size / n) & ~(align - 1);
}

int ui_layout_cells(int sw, int sh, int layout, int page, int n_ch, ui_cell_t *cells)
{
    int cols, rows;
    if (!grid_of(layout, &cols, &rows)) return 0;
    int pages = ui_layout_pages(layout, n_ch);
    if (page < 0) page = 0;
    if (page >= pages) page = pages - 1;
    for (int i = 0; i < layout; ++i) {
        int c = i % cols, r = i / cols, w = 1, h = 1;
        if (layout == 8) {
            c = k_eight[i][0];
            r = k_eight[i][1];
            w = k_eight[i][2];
            h = k_eight[i][3];
        }
        int x0 = edge(c, cols, sw, 16), x1 = edge(c + w, cols, sw, 16);
        int y0 = edge(r, rows, sh, 2), y1 = edge(r + h, rows, sh, 2);
        int ch = page * layout + i;
        cells[i].ch = ch < n_ch ? ch : -1;
        cells[i].x = x0;
        cells[i].y = y0;
        cells[i].w = x1 - x0;
        cells[i].h = y1 - y0;
    }
    return layout;
}
