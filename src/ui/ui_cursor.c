/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_cursor.c — the mouse pointer's arrow: an LVGL image where LVGL draws
 * the pointer, the same pixels for a platform's cursor plane
 * (hal_ui_cursor_create). Pixels are either opaque or fully transparent: a
 * blended edge on a colour-keyed layer would show as a pink fringe, and a
 * cursor plane may have one bit of alpha. */
#include <string.h>

#include "ui_cursor.h"

/* 12 x 19 arrow: '#' outline (black), '.' fill (white). */
static const char *const k_arrow[] = {
    "#           ",
    "##          ",
    "#.#         ",
    "#..#        ",
    "#...#       ",
    "#....#      ",
    "#.....#     ",
    "#......#    ",
    "#.......#   ",
    "#........#  ",
    "#.........# ",
    "#......#####",
    "#...#..#    ",
    "#..# #..#   ",
    "#.#  #..#   ",
    "##    #..#  ",
    "#     #..#  ",
    "       #..# ",
    "        ##  ",
};
#define ARROW_W UI_CURSOR_WIDTH
#define ARROW_H UI_CURSOR_HEIGHT

/* RGB565 pixels then an A8 plane: drawable on 16 and 32 bpp displays. */
static uint8_t          g_px[ARROW_W * ARROW_H * 3];
static lv_image_dsc_t   g_img;

lv_obj_t *ui_cursor_create(void)
{
    uint16_t *rgb = (uint16_t *)(void *)g_px;
    uint8_t *alpha = g_px + ARROW_W * ARROW_H * 2;
    for (int y = 0; y < ARROW_H; ++y)
        for (int x = 0; x < ARROW_W; ++x) {
            char c = k_arrow[y][x];
            rgb[y * ARROW_W + x] = c == '.' ? 0xFFFF : 0x0000;
            alpha[y * ARROW_W + x] = c == ' ' ? 0 : 0xFF;
        }
    memset(&g_img, 0, sizeof g_img);
    g_img.header.magic = LV_IMAGE_HEADER_MAGIC;
    g_img.header.cf = LV_COLOR_FORMAT_RGB565A8;
    g_img.header.w = ARROW_W;
    g_img.header.h = ARROW_H;
    g_img.header.stride = ARROW_W * 2;
    g_img.data = g_px;
    g_img.data_size = sizeof g_px;

    lv_obj_t *img = lv_image_create(lv_layer_sys());
    lv_image_set_src(img, &g_img);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

void ui_cursor_argb(uint32_t *px)
{
    for (int y = 0; y < ARROW_H; ++y)
        for (int x = 0; x < ARROW_W; ++x) {
            char c = k_arrow[y][x];
            px[y * ARROW_W + x] = c == ' ' ? 0 : c == '.' ? 0xFFFFFFFFu : 0xFF000000u;
        }
}
