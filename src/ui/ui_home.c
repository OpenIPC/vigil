/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_home.c — DVR home screen.
 *
 * Layout:
 *   header (40 px): per-channel REC chips, centered clock, role + totals
 *   quad   (600 px): 2x2 grid of colorkey tiles (video shows through), each
 *                    with a dark chip: REC dot, "CAM N", one status line
 *   strip  (80 px): event log (built separately in ui_events.c)
 *
 * The REC dots, the chip text and the header totals come from the role's
 * status source (ui_status_fn), polled once a second.
 *
 * Theme: dark (#1a1a1a bg, #333333 dividers, white text). REC red,
 * motion yellow, healthy green.
 *
 * Spotlight: every 45 s a 3 px white outline rotates through the 4 tiles
 * to draw the operator's eye — a standard signage-mode pattern.
 */

#include <stdio.h>
#include <time.h>

#include "lvgl.h"
#include "ui_home.h"
#include "ui_events.h"
#include "hal_ui.h"        /* how a tile lets the video show through */

/* -------- Design tokens ------------------------------------------------ */
#define COL_BG          lv_color_hex(0x1a1a1a)
#define COL_DIV         lv_color_hex(0x333333)
#define COL_TEXT        lv_color_hex(0xffffff)
#define COL_REC         lv_color_hex(0xff3333)
#define COL_MOTION      lv_color_hex(0xffcc00)
#define COL_HEALTHY     lv_color_hex(0x33ff33)
#define COL_CHIP        lv_color_hex(0x101010)
#define COL_DIM         lv_color_hex(0xa0a0a0)

#define HDR_HEIGHT      40
#define STRIP_HEIGHT    80
#define TILE_AREA_TOP   HDR_HEIGHT
#define TILE_AREA_HT    (720 - HDR_HEIGHT - STRIP_HEIGHT)  /* = 600 */

#define SPOTLIGHT_PERIOD_MS  (45 * 1000)
#define FLASH_DURATION_MS    3000
#define CLOCK_TICK_MS        1000

/* -------- State -------------------------------------------------------- */
typedef struct {
    lv_obj_t *tile;          /* the tile root for ch[i] */
    lv_obj_t *rec_led;       /* small REC LED top-left */
    lv_obj_t *label;         /* "CAM N" text */
    lv_obj_t *info;          /* status line under the label */
    lv_obj_t *flash_overlay; /* yellow border, hidden by default */
    lv_timer_t *flash_timer;
} channel_t;

static channel_t  g_ch[UI_HOME_CHANNELS];
static lv_obj_t  *g_clock_label;
static lv_obj_t  *g_status_label;
static lv_obj_t  *g_header_recs[UI_HOME_CHANNELS];
static lv_obj_t  *g_spotlight;
static int        g_spotlight_idx;
static const char *g_role = "";
static ui_status_fn g_status;

/* -------- Helpers ------------------------------------------------------ */
static void style_dark_pad0(lv_obj_t *obj)
{
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_margin_all(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
}

static lv_obj_t *small_led(lv_obj_t *parent, lv_color_t color, int size)
{
    lv_obj_t *led = lv_led_create(parent);
    lv_led_set_color(led, color);
    lv_obj_set_size(led, size, size);
    lv_led_on(led);
    return led;
}

static void clock_tick_cb(lv_timer_t *t)
{
    (void)t;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[16];
    snprintf(buf, sizeof buf, "%02d:%02d:%02d",
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    lv_label_set_text(g_clock_label, buf);
}

static void set_led(lv_obj_t *led, int on)
{
    lv_led_set_color(led, on ? COL_REC : COL_DIV);
    if (on) lv_led_on(led); else lv_led_off(led);
}

/* Refresh the REC dots, the chip status lines and the header totals. */
static void status_tick_cb(lv_timer_t *t)
{
    (void)t;
    uint64_t total = 0;
    int live = 0, rec = 0;
    for (int i = 0; i < UI_HOME_CHANNELS; ++i) {
        ui_chan_status_t st = {0};
        if (g_status) g_status(i, &st);
        set_led(g_header_recs[i], st.recording);
        set_led(g_ch[i].rec_led, st.recording);
        lv_label_set_text(g_ch[i].info, st.info[0] ? st.info : "NO CAMERA");
        lv_obj_set_style_text_color(g_ch[i].info, st.live ? COL_HEALTHY : COL_DIM, 0);
        live += st.live;
        rec += st.recording;
        total += st.rec_bytes;
    }
    char buf[64];
    snprintf(buf, sizeof buf, "%s   LIVE %d/%d   REC %d/%d   %llu MiB",
             g_role, live, UI_HOME_CHANNELS, rec, UI_HOME_CHANNELS,
             (unsigned long long)(total >> 20));
    lv_label_set_text(g_status_label, buf);
}

static void flash_clear_cb(lv_timer_t *t)
{
    int idx = (int)(intptr_t)lv_timer_get_user_data(t);
    if (idx >= 0 && idx < UI_HOME_CHANNELS) {
        lv_obj_add_flag(g_ch[idx].flash_overlay, LV_OBJ_FLAG_HIDDEN);
        g_ch[idx].flash_timer = NULL;
    }
    lv_timer_delete(t);
}

void ui_home_flash_channel(int ch)
{
    if (ch < 1 || ch > UI_HOME_CHANNELS) return;
    int idx = ch - 1;
    lv_obj_clear_flag(g_ch[idx].flash_overlay, LV_OBJ_FLAG_HIDDEN);
    if (g_ch[idx].flash_timer) {
        lv_timer_delete(g_ch[idx].flash_timer);
    }
    g_ch[idx].flash_timer = lv_timer_create(flash_clear_cb, FLASH_DURATION_MS,
                                            (void *)(intptr_t)idx);
    lv_timer_set_repeat_count(g_ch[idx].flash_timer, 1);
}

static void spotlight_tick_cb(lv_timer_t *t)
{
    (void)t;
    g_spotlight_idx = (g_spotlight_idx + 1) % UI_HOME_CHANNELS;
    lv_obj_t *tile = g_ch[g_spotlight_idx].tile;
    lv_area_t a;
    lv_obj_get_coords(tile, &a);
    /* Position the spotlight border to wrap the tile precisely. */
    lv_obj_set_pos(g_spotlight, a.x1, a.y1);
    lv_obj_set_size(g_spotlight,
                    a.x2 - a.x1 + 1,
                    a.y2 - a.y1 + 1);
    lv_obj_clear_flag(g_spotlight, LV_OBJ_FLAG_HIDDEN);
}

/* -------- Builders ----------------------------------------------------- */
static lv_obj_t *make_tile(lv_obj_t *parent, int idx, int gx, int gy)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_grid_cell(tile,
                         LV_GRID_ALIGN_STRETCH, gx, 1,
                         LV_GRID_ALIGN_STRETCH, gy, 1);
    style_dark_pad0(tile);
    lv_obj_set_style_border_color(tile, COL_DIV, 0);
    lv_obj_set_style_border_width(tile, 1, 0);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    /* The tile is a hole onto the video layer below: a colorkey colour on
     * HiSilicon hifb, full transparency on planes with alpha. A plain fill
     * (no gradient), or the key would not match every pixel. */
    lv_color_t hole;
    lv_opa_t hole_opa;
    hal_ui_hole(&hole, &hole_opa);
    lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_color(tile, hole, 0);
    lv_obj_set_style_bg_opa(tile, hole_opa, 0);

    /* Dark chip, top-left: REC dot + "CAM N" + status line. Opaque so the
     * anti-aliased text edges blend into it, not into the magenta colorkey
     * (which would leave a pink fringe over the video). */
    lv_obj_t *chip = lv_obj_create(tile);
    style_dark_pad0(chip);
    lv_obj_set_style_bg_color(chip, COL_CHIP, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);   /* blending would tint it magenta */
    lv_obj_set_style_pad_all(chip, 6, 0);
    lv_obj_set_style_radius(chip, 4, 0);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(chip, LV_ALIGN_TOP_LEFT, 8, 8);

    lv_obj_t *rec = small_led(chip, COL_REC, 12);
    lv_obj_set_style_shadow_width(rec, 0, 0);
    lv_obj_set_pos(rec, 0, 3);

    lv_obj_t *lbl = lv_label_create(chip);
    char buf[12];
    snprintf(buf, sizeof buf, "CAM %d", idx + 1);
    lv_label_set_text(lbl, buf);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(lbl, 20, 0);

    lv_obj_t *info = lv_label_create(chip);
    lv_label_set_text(info, "");
    lv_obj_set_style_text_color(info, COL_DIM, 0);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(info, 0, 22);

    /* Hidden yellow border for motion flash. */
    lv_obj_t *flash = lv_obj_create(tile);
    lv_obj_set_size(flash, LV_PCT(100), LV_PCT(100));
    lv_obj_align(flash, LV_ALIGN_CENTER, 0, 0);
    style_dark_pad0(flash);
    lv_obj_set_style_bg_opa(flash, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(flash, COL_MOTION, 0);
    lv_obj_set_style_border_width(flash, 3, 0);
    lv_obj_add_flag(flash, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(flash, LV_OBJ_FLAG_HIDDEN);

    g_ch[idx].tile          = tile;
    g_ch[idx].rec_led       = rec;
    g_ch[idx].label         = lbl;
    g_ch[idx].info          = info;
    g_ch[idx].flash_overlay = flash;
    g_ch[idx].flash_timer   = NULL;
    return tile;
}

static lv_obj_t *make_header(lv_obj_t *parent)
{
    lv_obj_t *hdr = lv_obj_create(parent);
    lv_obj_set_size(hdr, LV_PCT(100), HDR_HEIGHT);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 0);
    style_dark_pad0(hdr);
    lv_obj_set_style_bg_color(hdr, COL_BG, 0);
    lv_obj_set_style_border_color(hdr, COL_DIV, 0);
    lv_obj_set_style_border_width(hdr, 0, 0);
    lv_obj_set_style_border_side(hdr, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(hdr, 1, 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    /* Left: per-channel REC chips (REC: ● ● ● ●). */
    int x = 16;
    for (int i = 0; i < UI_HOME_CHANNELS; ++i) {
        char buf[16];
        snprintf(buf, sizeof buf, "REC:");
        lv_obj_t *lab = lv_label_create(hdr);
        lv_label_set_text(lab, buf);
        lv_obj_set_style_text_color(lab, COL_TEXT, 0);
        lv_obj_set_style_text_font(lab, &lv_font_montserrat_14, 0);
        lv_obj_set_pos(lab, x, 12);
        x += 40;
        lv_obj_t *led = small_led(hdr, COL_REC, 10);
        lv_obj_set_pos(led, x, 15);
        g_header_recs[i] = led;
        x += 26;
    }

    /* Center: clock. */
    g_clock_label = lv_label_create(hdr);
    lv_label_set_text(g_clock_label, "--:--:--");
    lv_obj_set_style_text_color(g_clock_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(g_clock_label, &lv_font_montserrat_24, 0);
    lv_obj_align(g_clock_label, LV_ALIGN_CENTER, 0, 0);

    /* Right: status. */
    g_status_label = lv_label_create(hdr);
    lv_label_set_text(g_status_label, "");
    lv_obj_set_style_text_color(g_status_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(g_status_label, &lv_font_montserrat_14, 0);
    lv_obj_align(g_status_label, LV_ALIGN_RIGHT_MID, -16, 0);

    return hdr;
}

static lv_obj_t *make_quad(lv_obj_t *parent)
{
    lv_obj_t *quad = lv_obj_create(parent);
    lv_obj_set_size(quad, LV_PCT(100), TILE_AREA_HT);
    lv_obj_set_pos(quad, 0, TILE_AREA_TOP);
    style_dark_pad0(quad);
    lv_obj_set_style_bg_color(quad, COL_BG, 0);
    lv_obj_clear_flag(quad, LV_OBJ_FLAG_SCROLLABLE);

    static lv_coord_t cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t rows[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(quad, cols, rows);
    lv_obj_set_layout(quad, LV_LAYOUT_GRID);

    make_tile(quad, 0, 0, 0);
    make_tile(quad, 1, 1, 0);
    make_tile(quad, 2, 0, 1);
    make_tile(quad, 3, 1, 1);
    return quad;
}

/* -------- Public entry ------------------------------------------------ */
void ui_home_create(const char *role, ui_status_fn status)
{
    g_role = role;
    g_status = status;

    lv_obj_t *scr = lv_screen_active();
    style_dark_pad0(scr);
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    make_header(scr);
    make_quad(scr);
    ui_events_create(scr, STRIP_HEIGHT);

    /* Spotlight overlay — owned by the screen, parked off-screen until
     * the first tick fires. */
    g_spotlight = lv_obj_create(scr);
    lv_obj_set_size(g_spotlight, 10, 10);
    lv_obj_set_pos(g_spotlight, -100, -100);
    style_dark_pad0(g_spotlight);
    lv_obj_set_style_bg_opa(g_spotlight, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(g_spotlight, COL_TEXT, 0);
    lv_obj_set_style_border_width(g_spotlight, 3, 0);
    lv_obj_add_flag(g_spotlight, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(g_spotlight, LV_OBJ_FLAG_HIDDEN);

    /* Timers: clock + spotlight rotation + status poll. */
    lv_timer_create(clock_tick_cb, CLOCK_TICK_MS, NULL);
    clock_tick_cb(NULL);                /* paint immediately */
    lv_timer_create(spotlight_tick_cb, SPOTLIGHT_PERIOD_MS, NULL);
    g_spotlight_idx = -1;               /* next tick lands on tile 0 */
    lv_timer_create(status_tick_cb, 1000, NULL);
    status_tick_cb(NULL);
}
