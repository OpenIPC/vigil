/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_layout.h — where the tiles of the live view are: a grid of 1, 4, 9 or
 * 16 tiles over the whole screen. Plain C (no LVGL), shared by
 * the UI, which paints the tiles as holes, and the app, which places the
 * channels' video in the same rectangles (channels_set_tiles), so the two
 * always agree. */
#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

#define UI_LAYOUT_MAX 16

typedef struct {
    int ch;                 /* channel shown in this cell (0-based), -1: none */
    int x, y, w, h;
} ui_cell_t;

/* Is `layout` (tiles per page) one of 1, 4, 9, 16? */
int ui_layout_valid(int layout);

/* Pages needed to show `n_ch` channels `layout` at a time (at least 1). */
int ui_layout_pages(int layout, int n_ch);

/* The cells of page `page` (clamped to the pages there are) on a sw x sh
 * screen: `layout` of them, row by row, channel page * layout + i in cell
 * i (-1 past n_ch). The grid has no gaps, so the cells cover the screen
 * exactly; vertical edges are on multiples of 16 pixels and horizontal
 * ones even, which every platform's video layer can place exactly
 * (SigmaStar's display windows start on 16-pixel columns).
 * Returns the number of cells, 0 for an invalid layout. */
int ui_layout_cells(int sw, int sh, int layout, int page, int n_ch, ui_cell_t *cells);

/* The page of `layout` that shows channel `ch`. */
int ui_layout_page_of(int layout, int ch);

#endif /* UI_LAYOUT_H */
