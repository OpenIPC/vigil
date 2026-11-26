/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_layout.h — where the tiles of the live view are: 1, 4, 8, 9, 16, 32
 * or 64 windows over the whole screen. Plain C (no LVGL), shared by the
 * UI, which paints the tiles as holes, and the app, which places the
 * channels' video in the same rectangles (channels_set_tiles), so the two
 * always agree.
 *
 *   1, 4, 9, 16, 64   N x N grids
 *   8                 a 4 x 4 grid: one window 3 x 3 cells at the top
 *                     left, three down the right, four along the bottom
 *   32                an 8 x 4 grid (equal windows on a 16:9 screen)
 */
#ifndef UI_LAYOUT_H
#define UI_LAYOUT_H

#define UI_LAYOUT_MAX 64

typedef struct {
    int ch;                 /* channel shown in this cell (0-based), -1: none */
    int x, y, w, h;
} ui_cell_t;

/* The layouts, smallest first, and their number. */
extern const int ui_layouts[];
extern const int ui_n_layouts;

/* Is `layout` (windows per page) one of ui_layouts? */
int ui_layout_valid(int layout);

/* A short name of the grid: "1x1", "2x2", "1+7", "3x3", "4x4", "8x4", "8x8". */
const char *ui_layout_name(int layout);

/* Pages needed to show `n_ch` channels `layout` at a time (at least 1). */
int ui_layout_pages(int layout, int n_ch);

/* The cells of page `page` (clamped to the pages there are) on a sw x sh
 * screen: `layout` of them, the large window of 8 first and then row by
 * row, channel page * layout + i in cell i (-1 past n_ch). The cells
 * cover the screen exactly, with no gaps; vertical edges are on multiples
 * of 16 pixels and horizontal ones even, which every platform's video
 * layer can place exactly (SigmaStar's display windows start on 16-pixel
 * columns). Returns the number of cells, 0 for an invalid layout. */
int ui_layout_cells(int sw, int sh, int layout, int page, int n_ch, ui_cell_t *cells);

/* The page of `layout` that shows channel `ch`. */
int ui_layout_page_of(int layout, int ch);

#endif /* UI_LAYOUT_H */
