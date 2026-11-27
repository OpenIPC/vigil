/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_live.c — the live view.
 *
 *   grid              1, 4, 9 or 16 tiles over the whole screen
 *                     (ui_layout.c). A tile is a hole the channel's video
 *                     shows through (hal_ui_hole), with text and icons on
 *                     it: the camera's name and the REC and event icons in
 *                     corners, its state ("Connecting...", "No video") in
 *                     the middle
 *   clock             date and time at a corner or an edge's middle
 *
 * Where the clock, the names and the icons go, their size and what they
 * stand on (an outline, nothing, a box) are display.* (ui_osd_get); a
 * change rebuilds them at once.
 *   navigation bar    over the bottom of the grid: shown when the pointer
 *                     reaches the bottom edge, hidden 3 s after it leaves,
 *                     or pinned
 *
 * Changing the layout or the page, and double-clicking a tile (that channel
 * full screen, and back), moves the video through model->show_tiles. The
 * right button opens a context menu with the same actions. The layout and
 * the page the user picks are kept in live.* (saved SAVE_DELAY after the
 * last change, and by ui_live_flush when vigild stops), so a restart comes
 * back to them; a full screen by double click is a moment's look and is
 * not kept, the grid it came from is. live.* set through the API is shown
 * at once. Events that
 * need attention become toasts (event_toast).
 *
 * Keyboard: the grid is an lv_gridnav container, so the arrows select a
 * tile (a ring inside its edge, ui_st_focus_in) and Enter shows it full
 * screen; Tab reaches the navigation bar, which comes up while it has the
 * focus. The shortcuts (1, 4, pages, menu) are in ui_live_key.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hal_ui.h"
#include "icons.h"
#include "ui_app.h"
#include "ui_i18n.h"
#include "ui_settings.h"
#include "ui_layout.h"
#include "ui_theme.h"

#define NAV_EDGE   3            /* px from the bottom that bring the bar up */
#define NAV_POLL   100          /* ms */
#define ARROW_EDGE 80           /* px from the left or right edge that bring up a page arrow */
#define ARROW_IDLE 2000         /* ms without the pointer moving that put it away */
#define SAVE_DELAY 3000         /* ms after the last layout or page change that it is saved */

#define N_EVENT_ICONS 4
#define NAME_W        200       /* px a name takes at most, for keeping it off the clock */
#define ICONS_W       140       /* ... the icons, all of them shown */
#define ICON_PX       20        /* the icons' size (icons.h) */

typedef struct {
    int         ch;
    lv_obj_t   *obj;
    lv_obj_t   *name;                  /* a corner (display.name_position); NULL: none */
    lv_obj_t   *icons, *rec;           /* a corner: REC, then the events */
    lv_obj_t   *ev[N_EVENT_ICONS];
    lv_obj_t   *mid, *mid_l1, *mid_l2; /* centre: the state when there is no picture, outlined */
    lv_obj_t   *add, *add_caption;     /* centre: a small "+" on an empty slot */
    lv_obj_t   *flash;
    lv_timer_t *flash_timer;
    lv_obj_t   *alert;                 /* display.alert_frame: on while the camera reports an event */
    int         tiny;                  /* small tile: one line of state, a smaller "+" */
} cell_t;

#define N_LAYOUTS 7                    /* ui_n_layouts */

static struct {
    lv_obj_t  *scr, *grid, *clock, *summary;
    lv_group_t *group;                  /* the grid, then the navigation bar */
    int        sel_ch;                  /* the tile the keyboard selected (its channel) */
    lv_obj_t  *nav, *nav_page, *nav_prev, *nav_next, *nav_pin;
    lv_obj_t  *arrow[2];                /* previous / next page, at the screen's edges */
    lv_point_t arrow_pt;                /* where the pointer was */
    uint32_t   arrow_moved_ms;          /* ... and when it last moved */
    lv_obj_t  *layout_btn[N_LAYOUTS];
    int        sw, sh;
    int        layout, page;
    int        fs, fs_layout, fs_page;  /* full screen by double click: where back goes */
    int        want_layout, want_page;  /* the grid to keep in live.* (never the double click's) */
    int        cfg_layout, cfg_page;    /* live.* as last read (0: none) */
    lv_timer_t *save_timer;             /* want_* waits to be saved */
    cell_t     cells[UI_LAYOUT_MAX];
    int        n_cells;
    uint32_t   click_ms;
    int        click_ch;
    int        nav_shown;
    uint32_t   nav_seen_ms;             /* the pointer was last on the edge or the bar */
    lv_color_t hole;
    lv_opa_t   hole_opa;
    ui_osd_t   osd;                     /* display.*, as the screen shows it */
    lv_timer_t *alert_timer;            /* the alarm frames' blink */
    unsigned   alert_tick;
} L;

static int n_channels(void)
{
    int n = ui_model()->channels;
    return n > UI_MAX_CHANNELS ? UI_MAX_CHANNELS : n < 1 ? 1 : n;
}

/* A layout is offered when the platform can show that many channels:
 * more windows than channels would only add empty ones. */
static int layout_offered(int layout)
{
    if (ui_model()->fixed_tiles) return layout == 4;
    return ui_layout_valid(layout) && layout <= n_channels();
}

int ui_live_layout_offered(int layout) { return layout_offered(layout); }

/* ---- display.* --------------------------------------------------------------------- */

const char *const ui_pos_keys[UI_N_POS] = { "off", "top-left", "top-center", "top-right", "bottom-left",
                                            "bottom-center", "bottom-right" };
const char *const ui_size_keys[UI_N_SIZES] = { "small", "medium", "large", "xlarge" };
const char *const ui_bg_keys[UI_N_BGS] = { "outline", "none", "box" };

static int key_index(const char *const *keys, int n, const char *v, int def)
{
    for (int i = 0; i < n; ++i)
        if (!strcmp(keys[i], v)) return i;
    return def;
}

void ui_osd_get(ui_osd_t *o)
{
    *o = (ui_osd_t){ .clock = UI_POS_TR, .clock_size = 1, .clock_bg = UI_OSD_OUTLINE, .name = UI_POS_BL,
                     .name_size = 1, .name_bg = UI_OSD_OUTLINE, .icons = UI_POS_BR, .icons_bg = UI_OSD_OUTLINE,
                     .rec = 1, .alarms = 1, .dividers = 1, .alert_frame = 0 };
    const ui_model_t *m = ui_model();
    for (int s = 0; s < m->n_sections; ++s) {
        if (strcmp(m->sections[s].key, "display")) continue;
        for (int f = 0; f < m->sections[s].n_fields; ++f) {
            const char *k = m->sections[s].fields[f].key;
            char v[24];
            m->get_value(s, -1, f, v, sizeof v);
            if (!strcmp(k, "clock_position")) o->clock = key_index(ui_pos_keys, UI_N_POS, v, o->clock);
            else if (!strcmp(k, "clock_size")) o->clock_size = key_index(ui_size_keys, UI_N_SIZES, v, 1);
            else if (!strcmp(k, "clock_background")) o->clock_bg = key_index(ui_bg_keys, UI_N_BGS, v, 0);
            else if (!strcmp(k, "name_position")) o->name = key_index(ui_pos_keys, UI_N_POS, v, o->name);
            else if (!strcmp(k, "name_size")) o->name_size = key_index(ui_size_keys, UI_N_SIZES, v, 1);
            else if (!strcmp(k, "name_background")) o->name_bg = key_index(ui_bg_keys, UI_N_BGS, v, 0);
            else if (!strcmp(k, "icons_position")) o->icons = key_index(ui_pos_keys, UI_N_POS, v, o->icons);
            else if (!strcmp(k, "icons_background")) o->icons_bg = key_index(ui_bg_keys, UI_N_BGS, v, 0);
            else if (!strcmp(k, "rec_icon")) o->rec = v[0] == '1';
            else if (!strcmp(k, "alarm_icons")) o->alarms = v[0] == '1';
            else if (!strcmp(k, "tile_dividers")) o->dividers = v[0] == '1';
            else if (!strcmp(k, "alert_frame")) o->alert_frame = v[0] == '1';
        }
    }
    /* In a tile there is no middle of an edge, and the icons are always shown. */
    if (o->name == UI_POS_TC) o->name = UI_POS_TL;
    if (o->name == UI_POS_BC) o->name = UI_POS_BL;
    if (o->icons == UI_POS_OFF || o->icons == UI_POS_TC || o->icons == UI_POS_BC) o->icons = UI_POS_BR;
}

static const lv_font_t *osd_font(int size)
{
    switch (size) {
    case 0:  return VG_FONT_SM_LV;
    case 1:  return VG_FONT_MD_LV;
    case 2:  return VG_FONT_LG_LV;
    default: return VG_FONT_XL_LV;
    }
}

static int pos_top(int p) { return p == UI_POS_TL || p == UI_POS_TC || p == UI_POS_TR; }
static int pos_left(int p) { return p == UI_POS_TL || p == UI_POS_BL; }
static int pos_right(int p) { return p == UI_POS_TR || p == UI_POS_BR; }

/* `o` at position `p` of its parent, SPACE_SM in from the edges and `dy`
 * further in, up or down. */
static void osd_place(lv_obj_t *o, int p, int dy)
{
    static const lv_align_t align[UI_N_POS] = { LV_ALIGN_DEFAULT,     LV_ALIGN_TOP_LEFT,   LV_ALIGN_TOP_MID,
                                                LV_ALIGN_TOP_RIGHT,   LV_ALIGN_BOTTOM_LEFT, LV_ALIGN_BOTTOM_MID,
                                                LV_ALIGN_BOTTOM_RIGHT };
    int x = pos_left(p) ? VG_SPACE_SM : pos_right(p) ? -VG_SPACE_SM : 0;
    int y = pos_top(p) ? VG_SPACE_SM + dy : -VG_SPACE_SM - dy;
    lv_obj_align(o, align[p], x, y);
}

/* The settings section with `key`, or -1. */
static int section_of(const char *key)
{
    for (int i = 0; i < ui_model()->n_sections; ++i)
        if (!strcmp(ui_model()->sections[i].key, key)) return i;
    return -1;
}

/* ---- one tile ----------------------------------------------------------------------- */

/* A row of icons over the video: on a chip (box), or with room for their
 * outline around them. */
static lv_obj_t *icon_row(lv_obj_t *parent, int bg)
{
    lv_obj_t *c = lv_obj_create(parent);
    ui_plain(c);
    if (bg == UI_OSD_BOX) {
        lv_obj_add_style(c, &ui_st_chip, 0);
        lv_obj_set_style_pad_ver(c, VG_SPACE_XS, 0);
    } else {
        lv_obj_set_style_pad_all(c, 1, 0);
        lv_obj_set_style_pad_column(c, VG_SPACE_SM, 0);
    }
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(c, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);    /* clicks go to the tile */
    return c;
}

static void show(lv_obj_t *o, int on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* What the centre says when there is no picture. */
static const char *status_text(const ui_chan_t *st, lv_color_t *col, const char **detail)
{
    *col = UI_C(BAD);
    *detail = "";
    switch (st->status) {
    case UI_CH_DISABLED:   *col = UI_C(TEXT2); return TR(S_ST_DISABLED);
    case UI_CH_CONNECTING: *col = UI_C(WARN); return TR(S_ST_CONNECTING);
    case UI_CH_LIVE:       *col = UI_C(WARN); return TR(S_ST_NO_DECODER);  /* live, nothing to show it */
    case UI_CH_NO_VIDEO:   return TR(S_ST_NO_VIDEO);
    case UI_CH_OFFLINE:    return TR(S_ST_OFFLINE);
    case UI_CH_AUTH:       return TR(S_ST_AUTH);
    case UI_CH_CODEC:      return TR(S_ST_CODEC);
    default:               *detail = st->state; return TR(S_ST_NO_VIDEO);
    }
}

/* The alarm frame: while the camera reports an event and
 * display.alert_frame is on, blinking (L.alert_tick: 3 of 4 on). On and
 * off, never fading: a translucent edge over a colour-keyed hole would
 * show the key colour through it. */
static void alert_show(cell_t *c)
{
    int on = L.osd.alert_frame && c->ch >= 0 && ui_channel(c->ch)->events && L.alert_tick % 4 != 3;
    if (on == !lv_obj_has_flag(c->alert, LV_OBJ_FLAG_HIDDEN)) return;   /* no redraw */
    show(c->alert, on);
}

static void alert_tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!L.osd.alert_frame) return;
    ++L.alert_tick;
    for (int i = 0; i < L.n_cells; ++i) alert_show(&L.cells[i]);
}

static void cell_update(cell_t *c)
{
    if (c->ch < 0) {                    /* past the platform's channels */
        if (c->name) show(c->name, 0);
        show(c->icons, 0);
        show(c->mid, 0);
        show(c->add, 0);
        show(c->alert, 0);
        return;
    }
    const ui_chan_t *st = ui_channel(c->ch);
    char buf[64];
    ui_chan_title(c->ch, buf, sizeof buf);
    int empty = st->status == UI_CH_EMPTY;
    if (c->name) {
        ui_outlined_set_text(c->name, buf);
        ui_outlined_set_color(c->name, empty ? UI_C(TEXT2) : UI_C(TEXT));
    }

    int rec = L.osd.rec && st->recording, any = rec;
    show(c->rec, rec);
    for (int i = 0; i < N_EVENT_ICONS; ++i) {
        int on = L.osd.alarms && ((st->events >> i) & 1);
        show(c->ev[i], on);
        any |= on;
    }
    show(c->icons, any);
    alert_show(c);

    show(c->add, empty);
    if (!empty) show(c->add_caption, 0);
    if (empty || (st->status == UI_CH_LIVE && !st->no_decoder)) {
        show(c->mid, 0);
        return;
    }
    lv_color_t col;
    const char *detail;
    ui_outlined_set_text(c->mid_l1, status_text(st, &col, &detail));
    ui_outlined_set_color(c->mid_l1, col);
    ui_outlined_set_text(c->mid_l2, detail);
    show(c->mid_l2, *detail && !c->tiny);
    show(c->mid, 1);
}

static void cell_observer_cb(lv_observer_t *o, lv_subject_t *s)
{
    (void)s;
    cell_update(lv_observer_get_user_data(o));
}

static void set_layout(int layout, int page);
static void show_layout(int layout, int page);

static void toggle_full_screen(int ch)
{
    if (ui_model()->fixed_tiles || ch < 0) return;
    if (L.fs) {
        L.fs = 0;
        set_layout(L.fs_layout, ui_layout_page_of(L.fs_layout, ch));
    } else if (L.layout == 1) {
        int back = layout_offered(4) ? 4 : 1;
        set_layout(back, ui_layout_page_of(back, ch));
    } else {
        L.fs_layout = L.layout;
        L.fs_page = L.page;
        show_layout(1, ch);
        L.fs = 1;
    }
}

/* After the click's event: the new layout deletes the tile clicked. */
static void full_screen_async(void *ch) { toggle_full_screen((int)(intptr_t)ch); }

/* An empty tile's "+": Channels, as the context menu opens it, with this
 * channel marked, so a camera found can go straight there (after the
 * click's event). */
static void add_now(void *ch) { ui_channels_open_at((int)(intptr_t)ch); }

static void add_async(void *ch) { ui_with_perm(UI_PERM_CAMERAS, add_now, ch); }

static void channels_now(void *u)
{
    (void)u;
    ui_channels_open();
}

static void record_now(void *u)
{
    (void)u;
    ui_storage_open(UI_STORAGE_BASIC);
}

static void display_now(void *u)
{
    (void)u;
    ui_display_open();
}

static void add_click_cb(lv_event_t *e)
{
    cell_t *c = lv_event_get_user_data(e);
    if (c->ch >= 0) lv_async_call(add_async, (void *)(intptr_t)c->ch);
}

/* "Add camera" under the "+": while the pointer is on it, or the
 * keyboard on its tile. */
static void add_caption(cell_t *c, int on)
{
    show(c->add_caption, on && c->ch >= 0 && ui_channel(c->ch)->status == UI_CH_EMPTY);
}

static void add_hover_cb(lv_event_t *e)
{
    cell_t *c = lv_event_get_user_data(e);
    add_caption(c, lv_event_get_code(e) == LV_EVENT_HOVER_OVER ||
                       (lv_obj_has_state(c->obj, LV_STATE_FOCUSED) && ui_keyboard_in_use()));
}

static void cell_click_cb(lv_event_t *e)
{
    cell_t *c = lv_event_get_user_data(e);
    lv_indev_t *in = lv_indev_active();
    if (in && lv_indev_get_type(in) == LV_INDEV_TYPE_KEYPAD) {
        /* Enter on the selected tile (gridnav passes the click on): an
         * empty one adds a camera there, another goes full screen. */
        if (c->ch >= 0 && ui_channel(c->ch)->status == UI_CH_EMPTY)
            lv_async_call(add_async, (void *)(intptr_t)c->ch);
        else if (c->ch >= 0)
            lv_async_call(full_screen_async, (void *)(intptr_t)c->ch);
        return;
    }
    /* The keyboard's selection follows the mouse: Enter after a click
     * means the tile clicked. */
    ui_gridnav_focus(L.grid, c->obj);
    L.sel_ch = c->ch;
    /* LVGL 9.2 has no double-click event: two clicks on the same tile
     * within the token's interval are one. */
    if (c->ch >= 0 && c->ch == L.click_ch && lv_tick_elaps(L.click_ms) <= VG_DURATION_DOUBLE_CLICK) {
        L.click_ch = -1;
        lv_async_call(full_screen_async, (void *)(intptr_t)c->ch);
        return;
    }
    L.click_ch = c->ch;
    L.click_ms = lv_tick_get();
}

static void flash_done_cb(lv_timer_t *t)
{
    cell_t *c = lv_timer_get_user_data(t);
    lv_obj_add_flag(c->flash, LV_OBJ_FLAG_HIDDEN);
    c->flash_timer = NULL;
}

static void cell_focused_cb(lv_event_t *e)
{
    cell_t *c = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_FOCUSED) L.sel_ch = c->ch;
    add_caption(c, lv_event_get_code(e) == LV_EVENT_FOCUSED && ui_keyboard_in_use());
}

static void cell_deleted_cb(lv_event_t *e)
{
    cell_t *c = lv_event_get_user_data(e);
    if (c->flash_timer) lv_timer_delete(c->flash_timer);
    c->flash_timer = NULL;
}

/* How far something about w x h at corner `p` of tile `g` moves in (up
 * from the bottom, down from the top) to clear the clock, which can be at
 * any corner of the screen or the middle of its top or bottom edge: 0
 * when the two do not meet. */
static int clock_offset(const ui_cell_t *g, int p, int w, int h)
{
    if (!L.clock) return 0;
    lv_area_t c;
    lv_obj_get_coords(L.clock, &c);
    int top = pos_top(p);
    int x0 = pos_left(p) ? g->x + VG_SPACE_SM : g->x + g->w - VG_SPACE_SM - w, x1 = x0 + w;
    int y0 = top ? g->y + VG_SPACE_SM : g->y + g->h - VG_SPACE_SM - h, y1 = y0 + h;
    /* the outline reaches a pixel past either */
    if (x1 + 1 < c.x1 - 1 || x0 - 1 > c.x2 + 1 || y1 + 1 < c.y1 - 1 || y0 - 1 > c.y2 + 1) return 0;
    return top ? c.y2 + VG_SPACE_XS + 2 - y0 : y1 - (c.y1 - VG_SPACE_XS - 2);
}

static void cell_build(cell_t *c, const ui_cell_t *g)
{
    memset(c, 0, sizeof *c);
    c->ch = g->ch;
    lv_obj_t *o = lv_obj_create(L.grid);
    ui_plain(o);
    lv_obj_set_pos(o, g->x, g->y);
    lv_obj_set_size(o, g->w, g->h);
    /* The hole: a plain fill (no gradient), or a colour key would not
     * match every pixel. */
    lv_obj_set_style_bg_color(o, L.hole, 0);
    lv_obj_set_style_bg_opa(o, L.hole_opa, 0);
    /* The dividers: each tile's edge, 2 px between two. Without them the
     * holes touch and so do the pictures (the tiles share their edges,
     * and so do the video windows they are mapped to). */
    lv_obj_set_style_border_color(o, UI_C(LINE), 0);
    lv_obj_set_style_border_width(o, L.osd.dividers ? 1 : 0, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    /* The focus ring inside the tile's edge, over the video's outermost
     * pixels: outside it would cover the neighbour. */
    lv_obj_add_style(o, &ui_st_focus_in, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(o, cell_click_cb, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(o, cell_focused_cb, LV_EVENT_FOCUSED, c);
    lv_obj_add_event_cb(o, cell_focused_cb, LV_EVENT_DEFOCUSED, c);
    lv_obj_add_event_cb(o, cell_deleted_cb, LV_EVENT_DELETE, c);
    c->obj = o;

    int small = g->w < 400;
    c->tiny = g->w < 240;
    const ui_osd_t *osd = &L.osd;
    /* The name in its corner, a size smaller in a small tile, clear of the
     * clock. */
    int name_dy = 0, name_h = 0;
    if (osd->name != UI_POS_OFF) {
        const lv_font_t *f = osd_font(osd->name_size - (small && osd->name_size > 0));
        c->name = ui_osd_text(o, f, VG_COLOR_TEXT, osd->name_bg);
        lv_obj_set_style_max_width(c->name, g->w / 2, 0);
        lv_label_set_long_mode(c->name, LV_LABEL_LONG_CLIP);
        name_h = lv_font_get_line_height(f) + (osd->name_bg == UI_OSD_BOX ? 2 * VG_SPACE_XS : 0);
        name_dy = clock_offset(g, osd->name, LV_MIN(NAME_W, g->w / 2), name_h);
        osd_place(c->name, osd->name, name_dy);
    }

    /* Icons only, small, in the design tokens' colours; past the name
     * when they share its corner. */
    static const lv_image_dsc_t *const ev_img[N_EVENT_ICONS] = { &ui_icon_ev_motion, &ui_icon_ev_human,
                                                                 &ui_icon_ev_face, &ui_icon_ev_vehicle };
    c->icons = icon_row(o, osd->icons_bg);
    int icons_h = ICON_PX + (osd->icons_bg == UI_OSD_BOX ? 2 * VG_SPACE_XS : 2);
    int icons_dy = osd->icons == osd->name ? name_dy + name_h + VG_SPACE_XS
                                           : clock_offset(g, osd->icons, ICONS_W, icons_h);
    osd_place(c->icons, osd->icons, icons_dy);
    c->rec = ui_osd_icon(c->icons, &ui_icon_rec, VG_COLOR_BAD, osd->icons_bg);
    for (int i = 0; i < N_EVENT_ICONS; ++i) c->ev[i] = ui_osd_icon(c->icons, ev_img[i], VG_COLOR_WARN, osd->icons_bg);

    c->mid = lv_obj_create(o);
    ui_plain(c->mid);
    ui_inert(c->mid);
    lv_obj_set_size(c->mid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c->mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_layout(c->mid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_align(c->mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(c->mid, VG_SPACE_XS, 0);
    lv_obj_center(c->mid);
    c->mid_l1 = ui_outlined(c->mid, c->tiny ? VG_FONT_SM_LV : small ? VG_FONT_MD_LV : VG_FONT_LG_LV, VG_COLOR_TEXT2);
    c->mid_l2 = ui_outlined(c->mid, VG_FONT_SM_LV, VG_COLOR_TEXT);
    lv_obj_set_style_max_width(c->mid_l2, g->w - 4 * VG_SPACE_LG, 0);
    lv_obj_set_style_text_align(c->mid_l2, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(c->mid_l2, LV_LABEL_LONG_WRAP);

    /* An empty slot: a small "+" that adds a camera on this channel, with
     * "Add camera" under it while it is hovered or its tile focused. */
    c->add = ui_disc_button(o, VG_SIZE_ADD_DISC);
    lv_obj_center(c->add);
    lv_obj_add_event_cb(c->add, add_click_cb, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(c->add, add_hover_cb, LV_EVENT_HOVER_OVER, c);
    lv_obj_add_event_cb(c->add, add_hover_cb, LV_EVENT_HOVER_LEAVE, c);
    lv_obj_center(ui_icon(c->add, &ui_icon_add, VG_COLOR_TEXT));
    lv_obj_add_flag(c->add, LV_OBJ_FLAG_HIDDEN);
    c->add_caption = ui_outlined(o, VG_FONT_SM_LV, VG_COLOR_TEXT);
    ui_outlined_set_text(c->add_caption, TR(S_ADD_CAMERA));
    lv_obj_align(c->add_caption, LV_ALIGN_CENTER, 0, VG_SIZE_ADD_DISC / 2 + VG_SPACE_MD);
    lv_obj_add_flag(c->add_caption, LV_OBJ_FLAG_HIDDEN);

    c->alert = lv_obj_create(o);
    ui_plain(c->alert);
    lv_obj_set_size(c->alert, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_border_color(c->alert, UI_C(BAD), 0);
    lv_obj_set_style_border_width(c->alert, 3, 0);
    ui_inert(c->alert);
    lv_obj_add_flag(c->alert, LV_OBJ_FLAG_HIDDEN);

    c->flash = lv_obj_create(o);
    ui_plain(c->flash);
    lv_obj_set_size(c->flash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_border_color(c->flash, UI_C(WARN), 0);
    lv_obj_set_style_border_width(c->flash, 3, 0);
    ui_inert(c->flash);
    lv_obj_add_flag(c->flash, LV_OBJ_FLAG_HIDDEN);

    if (c->ch >= 0)
        lv_subject_add_observer_obj(ui_subject_channel(c->ch), cell_observer_cb, o, c);
    else
        cell_update(c);
}

/* ---- layouts ---------------------------------------------------------------------------- */

static void nav_update(void)
{
    int pages = ui_layout_pages(L.layout, n_channels());
    for (int i = 0; i < N_LAYOUTS; ++i) {
        if (!L.layout_btn[i]) continue;
        if (ui_layouts[i] == L.layout) lv_obj_add_state(L.layout_btn[i], LV_STATE_CHECKED);
        else lv_obj_remove_state(L.layout_btn[i], LV_STATE_CHECKED);
    }
    lv_label_set_text_fmt(L.nav_page, "%d / %d", L.page + 1, pages);
    if (pages > 1) {
        lv_obj_remove_state(L.nav_prev, LV_STATE_DISABLED);
        lv_obj_remove_state(L.nav_next, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(L.nav_prev, LV_STATE_DISABLED);
        lv_obj_add_state(L.nav_next, LV_STATE_DISABLED);
    }
}

/* The tiles of L.layout's page L.page, anew; where each channel's goes
 * into `tiles`. */
static void cells_build(ui_rect_t *tiles)
{
    lv_obj_clean(L.grid);
    ui_cell_t g[UI_LAYOUT_MAX];
    L.n_cells = ui_layout_cells(L.sw, L.sh, L.layout, L.page, n_channels(), g);
    memset(tiles, 0, sizeof *tiles * UI_MAX_CHANNELS);
    int sel = 0;
    for (int i = 0; i < L.n_cells; ++i) {
        cell_build(&L.cells[i], &g[i]);
        if (g[i].ch >= 0) tiles[g[i].ch] = (ui_rect_t){ g[i].x, g[i].y, g[i].w, g[i].h };
        if (g[i].ch == L.sel_ch) sel = i;
    }
    /* The selected camera stays selected when it is still on the screen
     * (full screen and back), else the page's first tile is. */
    if (L.n_cells) {
        ui_gridnav_focus(L.grid, L.cells[sel].obj);
        L.sel_ch = L.cells[sel].ch;
    }
}

/* The layout and page, shown (not kept: the double click's full screen,
 * or what live.* already holds). */
static void show_layout(int layout, int page)
{
    if (!ui_layout_valid(layout)) return;
    int n = n_channels(), pages = ui_layout_pages(layout, n);
    page = (page % pages + pages) % pages;      /* paging wraps around */
    L.layout = layout;
    L.page = page;
    L.fs = 0;
    L.click_ch = -1;

    ui_rect_t tiles[UI_MAX_CHANNELS];
    cells_build(tiles);
    if (ui_model()->show_tiles && !ui_model()->fixed_tiles) ui_model()->show_tiles(tiles, n);
    if (L.nav) nav_update();
}

/* ---- live.*: the grid kept across restarts --------------------------------------------- */

/* live.layout as windows (0: none stored) and live.page from 0. */
static void live_stored(int *layout, int *page)
{
    *layout = *page = 0;
    int s = section_of("live");
    if (s < 0) return;
    const ui_section_t *u = &ui_model()->sections[s];
    for (int f = 0; f < u->n_fields; ++f) {
        char v[16];
        ui_model()->get_value(s, -1, f, v, sizeof v);
        if (!strcmp(u->fields[f].key, "page")) *page = atoi(v) > 1 ? atoi(v) - 1 : 0;
        else if (!strcmp(u->fields[f].key, "layout"))
            for (int i = 0; i < ui_n_layouts; ++i)
                if (!strcmp(ui_layout_name(ui_layouts[i]), v)) *layout = ui_layouts[i];
    }
}

static int default_layout(void) { return n_channels() > 1 && layout_offered(4) ? 4 : 1; }

/* What `layout` windows become on this board: the largest layout offered
 * that is not larger (a file from a board with more channels). */
static int layout_fit(int layout)
{
    int best = 0;
    for (int i = 0; i < ui_n_layouts; ++i)
        if (ui_layouts[i] <= layout && layout_offered(ui_layouts[i])) best = ui_layouts[i];
    return best ? best : default_layout();
}

static int page_fit(int layout, int page)
{
    int pages = ui_layout_pages(layout, n_channels());
    return page < 0 ? 0 : page >= pages ? pages - 1 : page;
}

/* want_* into live.*, now (when it differs). */
static void live_save(void)
{
    if (L.save_timer) lv_timer_delete(L.save_timer);
    L.save_timer = NULL;
    int s = section_of("live"), layout, page;
    if (s < 0 || !L.want_layout) return;
    live_stored(&layout, &page);
    if (layout == L.want_layout && page == L.want_page) return;
    const ui_section_t *u = &ui_model()->sections[s];
    char buf[4][16];
    const char *v[4];
    for (int f = 0; f < u->n_fields && f < 4; ++f) {
        if (!strcmp(u->fields[f].key, "layout")) snprintf(buf[f], sizeof buf[f], "%s", ui_layout_name(L.want_layout));
        else if (!strcmp(u->fields[f].key, "page")) snprintf(buf[f], sizeof buf[f], "%d", L.want_page + 1);
        else ui_model()->get_value(s, -1, f, buf[f], sizeof buf[f]);
        v[f] = buf[f];
    }
    ui_errors_t errs;
    memset(&errs, 0, sizeof errs);
    /* No generation: live.* is the live view's own, the last change wins. */
    ui_save_rc_t rc = u->n_fields <= 4 ? ui_model()->save(s, 1, v, NULL, 0, &errs) : UI_SAVE_FAILED;
    if (rc == UI_SAVE_OK || rc == UI_SAVE_UNSYNCED) {
        L.cfg_layout = L.want_layout;
        L.cfg_page = L.want_page;
    } else {
        ui_toast(UI_TOAST_WARN, "The live view's layout was not saved: %s", errs.n ? errs.e[0].msg : "failed");
    }
}

static void save_timer_cb(lv_timer_t *t)
{
    (void)t;
    L.save_timer = NULL;                /* deleted after this (one run) */
    live_save();
}

/* The grid shown is the one to keep: saved SAVE_DELAY after the last
 * change, so a run of page flips is one write to the flash. */
static void live_remember(void)
{
    if (ui_model()->fixed_tiles || (L.layout == L.want_layout && L.page == L.want_page)) return;
    L.want_layout = L.layout;
    L.want_page = L.page;
    if (L.save_timer) {
        lv_timer_reset(L.save_timer);
        return;
    }
    L.save_timer = lv_timer_create(save_timer_cb, SAVE_DELAY, NULL);
    lv_timer_set_repeat_count(L.save_timer, 1);
}

void ui_live_flush(void)
{
    if (L.save_timer) live_save();
}

/* live.* changed (the API; or our own save coming back): shown at once,
 * over a change of ours still waiting to be saved. */
static void live_cfg_cb(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    int layout, page;
    live_stored(&layout, &page);
    if (!layout || (layout == L.cfg_layout && page == L.cfg_page)) return;
    L.cfg_layout = layout;
    L.cfg_page = page;
    if (L.save_timer) lv_timer_delete(L.save_timer);
    L.save_timer = NULL;
    layout = layout_fit(layout);
    page = page_fit(layout, page);
    L.want_layout = layout;
    L.want_page = page;
    int shown = L.fs ? L.fs_layout : L.layout, shown_page = L.fs ? L.fs_page : L.page;
    if (layout != shown || page != shown_page) show_layout(layout, page);
}

/* A layout or page the user chose: shown and kept. */
static void set_layout(int layout, int page)
{
    show_layout(layout, page);
    live_remember();
}

/* ---- clock and totals ------------------------------------------------------------------- */

static void summary_cb(lv_observer_t *o, lv_subject_t *s)
{
    (void)s;
    lv_obj_t *label = lv_observer_get_target_obj(o);
    int n = n_channels(), cams = 0, live = 0, rec = 0;
    for (int i = 0; i < n; ++i) {
        const ui_chan_t *st = ui_channel(i);
        cams += st->configured;
        live += st->configured && st->live;
        rec += st->recording;
    }
    lv_label_set_text_fmt(label, "LIVE %d/%d   REC %d/%d", live, cams, rec, cams);
}

/* The clock over the video, as display.clock_* say: outlined or plain
 * hard-edged text, or soft text on an opaque chip (never translucent: a
 * blend with the key colour shows as a pink fringe). Over the grid,
 * under the page arrows and the navigation bar. */
static void make_clock(void)
{
    if (L.clock) lv_obj_delete(L.clock);
    L.clock = NULL;
    if (L.osd.clock == UI_POS_OFF) return;
    L.clock = ui_osd_text(L.scr, osd_font(L.osd.clock_size), VG_COLOR_TEXT, L.osd.clock_bg);
    if (L.osd.clock_bg == UI_OSD_BOX) lv_obj_set_style_pad_hor(L.clock, VG_SPACE_MD, 0);
    ui_tnum(L.clock);
    lv_label_bind_text(L.clock, &ui_subj_clock, NULL);
    osd_place(L.clock, L.osd.clock, 0);
    lv_obj_move_to_index(L.clock, 1);
    lv_obj_update_layout(L.clock);
}

/* display.* changed: the clock and the tiles again, the video where it is. */
static void osd_cb(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    (void)s;
    ui_osd_t n;
    ui_osd_get(&n);
    if (!memcmp(&n, &L.osd, sizeof n)) return;
    L.osd = n;
    make_clock();
    ui_rect_t tiles[UI_MAX_CHANNELS];
    cells_build(tiles);
}

/* ---- navigation bar ------------------------------------------------------------------------ */

enum { ACT_MENU = 1, ACT_PREV, ACT_NEXT, ACT_PIN, ACT_FULL, ACT_CONTEXT, ACT_ESC, ACT_CHANNELS, ACT_RECORD,
       ACT_DISPLAY, ACT_EXIT, ACT_LAYOUT = 100 };


static void nav_show(int show)
{
    if (show == L.nav_shown) return;
    L.nav_shown = show;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, L.nav);
    lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_y);
    lv_anim_set_duration(&a, VG_DURATION_FAST);
    lv_anim_set_values(&a, lv_obj_get_y(L.nav), show ? L.sh - VG_SIZE_NAVBAR : L.sh);
    lv_anim_start(&a);
    if (show) L.nav_seen_ms = lv_tick_get();
}

static void do_action(int act, int ch)
{
    int pages = ui_layout_pages(L.layout, n_channels());
    switch (act) {
    case ACT_MENU:
        ui_menu_open();
        break;
    case ACT_CHANNELS:
        ui_with_perm(UI_PERM_CAMERAS, channels_now, NULL);
        break;
    case ACT_RECORD:
        ui_with_perm(UI_PERM_STORAGE, record_now, NULL);
        break;
    case ACT_DISPLAY:
        ui_with_perm(UI_PERM_SYSTEM, display_now, NULL);
        break;
    case ACT_EXIT:
        ui_exit_open();
        break;
    case ACT_PREV:
    case ACT_NEXT:
        if (pages > 1) set_layout(L.layout, L.page + (act == ACT_NEXT ? 1 : -1));
        break;
    case ACT_PIN:
        lv_subject_set_int(&ui_subj_nav_pinned, !lv_subject_get_int(&ui_subj_nav_pinned));
        break;
    case ACT_FULL:
        toggle_full_screen(ch);
        break;
    default:
        if (act >= ACT_LAYOUT) {
            int layout = act - ACT_LAYOUT;
            /* keep the first channel of the page in view */
            set_layout(layout, ui_layout_page_of(layout, L.page * L.layout));
        }
    }
}

static void action_cb(lv_event_t *e)
{
    int act = (int)(intptr_t)lv_event_get_user_data(e);
    int ch = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)) - 1;
    ui_popup_close();
    do_action(act, ch);
}

static void pinned_cb(lv_observer_t *o, lv_subject_t *s)
{
    (void)o;
    int pinned = lv_subject_get_int(s);
    if (pinned) {
        lv_obj_add_state(L.nav_pin, LV_STATE_CHECKED);
        nav_show(1);
    } else {
        lv_obj_remove_state(L.nav_pin, LV_STATE_CHECKED);
        L.nav_seen_ms = lv_tick_get();
    }
}

/* The page arrows: one comes up while the pointer is near its edge
 * around the middle of the screen and moving, when there is more than
 * one page. */
static void arrows_poll(void)
{
    lv_point_t p;
    int has = ui_pointer(&p), side = -1;
    if (has && (p.x != L.arrow_pt.x || p.y != L.arrow_pt.y)) {
        L.arrow_pt = p;
        L.arrow_moved_ms = lv_tick_get();
    }
    if (has && lv_screen_active() == L.scr && !ui_popup_is_open() &&
        ui_layout_pages(L.layout, n_channels()) > 1 && !ui_model()->fixed_tiles &&
        lv_tick_elaps(L.arrow_moved_ms) < ARROW_IDLE && p.y >= L.sh / 3 && p.y < L.sh * 2 / 3)
        side = p.x < ARROW_EDGE ? 0 : p.x >= L.sw - ARROW_EDGE ? 1 : -1;
    for (int i = 0; i < 2; ++i) show(L.arrow[i], i == side);
}

static void nav_poll_cb(lv_timer_t *t)
{
    (void)t;
    lv_point_t p;
    arrows_poll();
    if (lv_screen_active() != L.scr) return;
    if (lv_subject_get_int(&ui_subj_nav_pinned)) return;
    if (!ui_pointer(&p)) p.y = 0;                /* no mouse: up only while it has the focus */
    if (lv_obj_has_state(L.nav, LV_STATE_FOCUSED)) {
        nav_show(1);
        L.nav_seen_ms = lv_tick_get();
    } else if (p.y >= L.sh - NAV_EDGE) {
        nav_show(1);
        L.nav_seen_ms = lv_tick_get();
    } else if (L.nav_shown && (p.y >= L.sh - VG_SIZE_NAVBAR || ui_popup_is_open())) {
        L.nav_seen_ms = lv_tick_get();
    } else if (L.nav_shown && lv_tick_elaps(L.nav_seen_ms) >= VG_DURATION_NAVBAR_HIDE) {
        nav_show(0);
    }
}

/* Tab to the bar: up at once (and kept up by nav_poll_cb while focused). */
static void nav_focused_cb(lv_event_t *e)
{
    (void)e;
    nav_show(1);
}

static void arrow_cb(lv_event_t *e)
{
    int next = (int)(intptr_t)lv_event_get_user_data(e);
    L.arrow_moved_ms = lv_tick_get();               /* stays up for the next click */
    do_action(next ? ACT_NEXT : ACT_PREV, -1);
}

/* Round, opaque (a colour key shows any blend over video as a fringe). */
static void make_arrows(void)
{
    for (int i = 0; i < 2; ++i) {
        lv_obj_t *b = ui_disc_button(L.scr, VG_SIZE_PAGE_ARROW);
        lv_obj_align(b, i ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, i ? -VG_SPACE_LG : VG_SPACE_LG, 0);
        lv_obj_t *l = ui_label(b, i ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT, VG_FONT_LG_LV, UI_C(TEXT));
        lv_obj_center(l);
        lv_obj_add_event_cb(b, arrow_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        L.arrow[i] = b;
    }
}

static lv_obj_t *nav_btn(const char *sym, const char *text, int act)
{
    lv_obj_t *b = ui_btn(L.nav, sym, text);
    lv_obj_add_event_cb(b, action_cb, LV_EVENT_CLICKED, (void *)(intptr_t)act);
    return b;
}

static void nav_sep(void)
{
    lv_obj_t *s = lv_obj_create(L.nav);
    ui_plain(s);
    ui_inert(s);
    lv_obj_set_size(s, 1, VG_SIZE_BUTTON - VG_SPACE_SM);
    lv_obj_set_style_bg_color(s, UI_C(LINE), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
}

static void make_nav(void)
{
    L.nav = lv_obj_create(L.scr);
    ui_plain(L.nav);
    lv_obj_set_size(L.nav, LV_PCT(100), VG_SIZE_NAVBAR);
    lv_obj_set_pos(L.nav, 0, L.sh);                 /* below the screen: hidden */
    lv_obj_set_style_bg_color(L.nav, UI_C(PANEL), 0);
    lv_obj_set_style_bg_opa(L.nav, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(L.nav, UI_C(ACCENT), 0);
    lv_obj_set_style_border_side(L.nav, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(L.nav, 2, 0);
    lv_obj_set_style_pad_hor(L.nav, VG_SPACE_LG, 0);
    lv_obj_set_style_pad_column(L.nav, VG_SPACE_SM, 0);
    lv_obj_set_layout(L.nav, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(L.nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(L.nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *menu = nav_btn(LV_SYMBOL_LIST, "Main menu", ACT_MENU);
    ui_btn_accent(menu);
    nav_sep();
    int any = 0;
    for (int i = 0; i < ui_n_layouts && i < N_LAYOUTS; ++i) {
        if (!layout_offered(ui_layouts[i]) || ui_model()->fixed_tiles) continue;
        L.layout_btn[i] = nav_btn(NULL, ui_layout_name(ui_layouts[i]), ACT_LAYOUT + ui_layouts[i]);
        any = 1;
    }
    if (any) nav_sep();
    L.nav_prev = nav_btn(LV_SYMBOL_LEFT, NULL, ACT_PREV);
    L.nav_page = ui_label(L.nav, "", VG_FONT_MD_LV, UI_C(TEXT));
    lv_obj_set_style_min_width(L.nav_page, 64, 0);
    lv_obj_set_style_text_align(L.nav_page, LV_TEXT_ALIGN_CENTER, 0);
    L.nav_next = nav_btn(LV_SYMBOL_RIGHT, NULL, ACT_NEXT);
    if (ui_model()->fixed_tiles) {
        lv_obj_add_flag(L.nav_prev, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(L.nav_page, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(L.nav_next, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *spring = lv_obj_create(L.nav);
    ui_plain(spring);
    ui_inert(spring);
    lv_obj_set_height(spring, 1);
    lv_obj_set_flex_grow(spring, 1);
    L.nav_pin = nav_btn(LV_SYMBOL_DOWNLOAD, "Pin", ACT_PIN);
    L.summary = ui_label(L.nav, "", VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_set_style_pad_left(L.summary, VG_SPACE_LG, 0);
    for (int i = 0; i < n_channels(); ++i)
        lv_subject_add_observer_obj(ui_subject_channel(i), summary_cb, L.summary, NULL);
    lv_obj_t *clk = ui_label(L.nav, "", VG_FONT_MD_LV, UI_C(TEXT));
    ui_tnum(clk);
    lv_label_bind_text(clk, &ui_subj_clock, NULL);
    lv_obj_set_style_pad_left(clk, VG_SPACE_LG, 0);

    lv_subject_add_observer_obj(&ui_subj_nav_pinned, pinned_cb, L.nav, NULL);
    lv_timer_create(nav_poll_cb, NAV_POLL, NULL);
    ui_gridnav(L.nav, LV_GRIDNAV_CTRL_ROLLOVER);
    lv_obj_add_event_cb(L.nav, nav_focused_cb, LV_EVENT_FOCUSED, NULL);
    lv_group_add_obj(L.group, L.nav);
}

/* ---- context menu --------------------------------------------------------------------------- */

static lv_obj_t *menu_item(lv_obj_t *panel, const char *sym, const char *text, int act, int ch)
{
    lv_obj_t *b = ui_btn(panel, sym, text);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, UI_C(PANEL), 0);
    lv_obj_set_user_data(b, (void *)(intptr_t)(ch + 1));
    if (act) lv_obj_add_event_cb(b, action_cb, LV_EVENT_CLICKED, (void *)(intptr_t)act);
    return b;
}

static void menu_sep(lv_obj_t *panel)
{
    lv_obj_t *s = lv_obj_create(panel);
    ui_plain(s);
    ui_inert(s);
    lv_obj_set_size(s, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(s, UI_C(LINE), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
}

/* Not there yet (playback, PTZ): in the menu, muted, so it already has
 * the product's shape; choosing one says so (ui_not_yet), as the main
 * menu's cards of such features do. */
static void later_cb(lv_event_t *e)
{
    ui_not_yet(lv_event_get_user_data(e));
}

static void menu_item_later(lv_obj_t *panel, const char *sym, const char *text)
{
    lv_obj_t *b = menu_item(panel, sym, text, 0, -1);
    lv_obj_set_style_text_color(b, UI_C(TEXT2), 0);
    lv_obj_add_event_cb(b, later_cb, LV_EVENT_CLICKED, (void *)text);
}

static void split_pick_cb(lv_event_t *e)
{
    int layout = (int)(intptr_t)lv_event_get_user_data(e);
    ui_popup_close();
    /* keep the selected camera in view */
    set_layout(layout, ui_layout_page_of(layout, L.sel_ch < 0 ? 0 : L.sel_ch));
}

static void split_open(lv_obj_t *item)
{
    if (ui_popup_sub_is_open()) {
        ui_popup_sub_close();
        return;
    }
    lv_obj_t *sub = ui_popup_sub(item, 220), *cur = NULL;
    for (int i = 0; i < ui_n_layouts; ++i) {
        int n = ui_layouts[i];
        if (!layout_offered(n)) continue;
        char text[40];
        snprintf(text, sizeof text, "%d window%s  (%s)", n, n == 1 ? "" : "s", ui_layout_name(n));
        lv_obj_t *b = menu_item(sub, n == L.layout ? LV_SYMBOL_OK : NULL, text, 0, -1);
        lv_obj_add_event_cb(b, split_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)n);
        if (n == L.layout) {
            lv_obj_set_style_text_color(b, UI_C(ACCENT), 0);
            cur = b;
        } else {
            lv_obj_set_style_pad_left(b, VG_SPACE_MD + 22, 0);     /* aligned with the checked one */
        }
    }
    ui_popup_sub_place(cur);
}

static void split_click_cb(lv_event_t *e) { split_open(lv_event_get_target(e)); }

static void split_key_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) == LV_KEY_RIGHT && !ui_popup_sub_is_open()) split_open(lv_event_get_target(e));
}

/* Main menu, Channels, Playback, Record, PTZ, Display, Split, Exit:
 * nothing else (full screen is a double click or Enter, pages the edge arrows or
 * PgUp/PgDn, pinning the bar its own button). */
void ui_live_context_menu(const lv_point_t *at)
{
    lv_obj_t *panel = ui_popup_open(at, 260);
    menu_item(panel, LV_SYMBOL_LIST, "Main menu", ACT_MENU, -1);
    if (section_of("cameras") >= 0) menu_item(panel, LV_SYMBOL_VIDEO, "Channels", ACT_CHANNELS, -1);
    menu_item_later(panel, LV_SYMBOL_PLAY, "Playback");
    if (section_of("record") >= 0) menu_item(panel, LV_SYMBOL_SAVE, "Record", ACT_RECORD, -1);
    menu_item_later(panel, LV_SYMBOL_GPS, "PTZ");
    menu_item(panel, LV_SYMBOL_SETTINGS, "Display", ACT_DISPLAY, -1);
    if (!ui_model()->fixed_tiles) {
        lv_obj_t *split = menu_item(panel, LV_SYMBOL_BARS, "Split", 0, -1);
        lv_obj_set_flex_grow(ui_btn_label(split), 1);
        ui_label(split, LV_SYMBOL_RIGHT, VG_FONT_SM_LV, UI_C(TEXT2));
        lv_obj_add_event_cb(split, split_click_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(split, split_key_cb, LV_EVENT_KEY, NULL);
    }
    menu_item(panel, LV_SYMBOL_POWER, TR(S_EXIT), ACT_EXIT, -1);
    /* Left and Right go to the item (Right opens Split). */
    ui_popup_focus(panel, NULL, LV_GRIDNAV_CTRL_ROLLOVER | LV_GRIDNAV_CTRL_VERTICAL_MOVE_ONLY);
    ui_popup_place(at);
}

/* ---- keyboard ----------------------------------------------------------------------------- */

lv_group_t *ui_live_group(void) { return L.group; }

static void key_async(void *arg)
{
    int act = (int)(intptr_t)arg;
    if (lv_screen_active() != L.scr || ui_popup_is_open()) return;  /* something came over it */
    if (act >= ACT_LAYOUT) {
        int layout = act - ACT_LAYOUT;
        /* Single view of the selected camera; the grid around it. */
        set_layout(layout, ui_layout_page_of(layout, L.sel_ch < 0 ? 0 : L.sel_ch));
    } else if (act == ACT_CONTEXT) {
        lv_point_t at = { L.sw / 2, L.sh / 2 };
        for (int i = 0; i < L.n_cells; ++i)
            if (L.cells[i].ch == L.sel_ch && L.sel_ch >= 0) {
                lv_area_t a;
                lv_obj_get_coords(L.cells[i].obj, &a);
                at = (lv_point_t){ (a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2 };
            }
        ui_live_context_menu(&at);
    } else if (act == ACT_ESC) {
        if (lv_obj_has_state(L.nav, LV_STATE_FOCUSED)) lv_group_focus_obj(L.grid);
    } else {
        do_action(act, L.sel_ch);
    }
}

int ui_live_key(uint32_t key, unsigned mods)
{
    (void)mods;
    int act;
    switch (key) {
    case '1': act = ACT_LAYOUT + 1; break;
    case '4': act = ACT_LAYOUT + 4; break;
    case '8': act = ACT_LAYOUT + 8; break;
    case '9': act = ACT_LAYOUT + 9; break;
    case '0': act = ACT_LAYOUT + 16; break;
    case UI_KEY_PGUP: case '[': act = ACT_PREV; break;
    case UI_KEY_PGDN: case ']': act = ACT_NEXT; break;
    case 'm': case 'M': case UI_KEY_F(2): act = ACT_MENU; break;
    case UI_KEY_MENU: act = ACT_CONTEXT; break;
    case LV_KEY_ESC: act = ACT_ESC; break;
    default: return 0;
    }
    if (act >= ACT_LAYOUT && (ui_model()->fixed_tiles || !layout_offered(act - ACT_LAYOUT))) return 1;
    if (act == ACT_ESC) {
        key_async((void *)(intptr_t)act);       /* already outside the input processing */
        return 1;
    }
    lv_async_call(key_async, (void *)(intptr_t)act);
    return 1;
}

/* ---- events --------------------------------------------------------------------------------- */

static int on_screen(int ch)
{
    for (int i = 0; i < L.n_cells; ++i)
        if (L.cells[i].ch == ch) return 1;
    return 0;
}

/* The live view has no event list (the HTTP API's WebSocket has every
 * event): a tile shows its channel's state all the time, so only what a
 * tile does not show becomes a toast. That is recording trouble (failed,
 * waiting for a disk), a channel that cannot run (no decoder, bad URL), a
 * configuration that could not be applied, and a camera failing to
 * connect, going offline or coming back while its tile is not on the
 * screen. "REC on", "added", "config applied" and the rest are left to the
 * tiles. The same text from several cameras within a toast's time is one
 * toast ("3 cameras: ..."); different ones wait their turn. */
static void event_toast(const ui_event_t *ev)
{
    static char     last[sizeof ev->text];
    static uint32_t last_ms;
    static uint64_t chans;              /* the cameras it came from, within a toast's time */
    const char *t = ev->text;
    ui_toast_t kind;
    int off = ev->ch > 0 && !on_screen(ev->ch - 1);
    if (!strncmp(t, "REC ", 4) && strcmp(t, "REC on"))
        kind = UI_TOAST_WARN;
    else if (!strcmp(t, "no decoder") || !strcmp(t, "bad URL") || (!strncmp(t, "config:", 7) && strstr(t, "failed")))
        kind = UI_TOAST_BAD;
    else if (off && (!strcmp(t, "offline") || !strcmp(t, "down")))
        kind = UI_TOAST_WARN;
    else if (off && !strcmp(t, "online"))
        kind = UI_TOAST_OK;
    else
        return;
    if (strcmp(last, t) || lv_tick_elaps(last_ms) >= VG_DURATION_TOAST) chans = 0;
    if (ev->ch > 0) chans |= 1ull << ((ev->ch - 1) & 63);
    snprintf(last, sizeof last, "%s", t);
    last_ms = lv_tick_get();
    int count = 0;
    for (uint64_t m = chans; m; m &= m - 1) ++count;
    char text[sizeof ev->text + 48], who[40];
    if (ev->ch <= 0) {
        snprintf(text, sizeof text, "%s", t);
    } else if (count > 1) {
        snprintf(text, sizeof text, "%d cameras: %s", count, t);
    } else {
        ui_chan_title(ev->ch - 1, who, sizeof who);
        snprintf(text, sizeof text, "%s: %s", who, t);
    }
    ui_toast_event(kind, t, text);
}

void ui_live_event(const ui_event_t *ev)
{
    event_toast(ev);
    for (int i = 0; ev->ch > 0 && i < L.n_cells; ++i) {
        cell_t *c = &L.cells[i];
        if (c->ch != ev->ch - 1) continue;
        lv_obj_remove_flag(c->flash, LV_OBJ_FLAG_HIDDEN);
        if (c->flash_timer) lv_timer_delete(c->flash_timer);
        c->flash_timer = lv_timer_create(flash_done_cb, VG_DURATION_FLASH, c);
        lv_timer_set_repeat_count(c->flash_timer, 1);
        lv_timer_set_auto_delete(c->flash_timer, 1);
    }
}

lv_obj_t *ui_live_create(void)
{
    memset(&L, 0, sizeof L);
    L.click_ch = -1;
    L.sel_ch = 0;
    L.sw = lv_display_get_horizontal_resolution(NULL);
    L.sh = lv_display_get_vertical_resolution(NULL);
    hal_ui_hole(&L.hole, &L.hole_opa);

    L.scr = lv_obj_create(NULL);
    ui_plain(L.scr);
    lv_obj_add_style(L.scr, &ui_st_screen, 0);

    L.grid = lv_obj_create(L.scr);
    ui_plain(L.grid);
    lv_obj_set_size(L.grid, L.sw, L.sh);
    L.group = lv_group_create();
    ui_gridnav(L.grid, LV_GRIDNAV_CTRL_ROLLOVER);
    lv_group_add_obj(L.group, L.grid);
    /* With alpha holes (not a colour key), whatever lies under a tile must
     * be transparent too, or it would paint over the video. */
    if (L.hole_opa < LV_OPA_COVER) lv_obj_set_style_bg_opa(L.scr, LV_OPA_TRANSP, 0);

    ui_osd_get(&L.osd);
    make_clock();
    make_arrows();
    make_nav();
    /* As it was left (live.*), on this board's layouts. */
    live_stored(&L.cfg_layout, &L.cfg_page);
    L.want_layout = L.cfg_layout ? layout_fit(L.cfg_layout) : default_layout();
    L.want_page = page_fit(L.want_layout, L.cfg_page);
    show_layout(L.want_layout, L.want_page);
    lv_subject_add_observer_obj(&ui_subj_config, osd_cb, L.scr, NULL);
    lv_subject_add_observer_obj(&ui_subj_config, live_cfg_cb, L.scr, NULL);
    L.alert_timer = lv_timer_create(alert_tick_cb, 250, NULL);
    return L.scr;
}
