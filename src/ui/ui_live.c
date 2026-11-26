/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_live.c — the live view.
 *
 *   grid              1, 4, 9 or 16 tiles over the whole screen
 *                     (ui_layout.c). A tile is a hole the channel's video
 *                     shows through (hal_ui_hole), with small opaque chips
 *                     on it: the camera's name with REC and motion icons,
 *                     its state ("Connecting...", "No video") or its stream
 *   clock             date and time in one corner (CLOCK_CORNER)
 *   navigation bar    over the bottom of the grid: shown when the pointer
 *                     reaches the bottom edge, hidden 3 s after it leaves,
 *                     or pinned
 *
 * Changing the layout or the page, and double-clicking a tile (that channel
 * full screen, and back), moves the video through model->show_tiles. The
 * right button opens a context menu with the same actions. Events that
 * need attention become toasts (event_toast).
 *
 * Keyboard: the grid is an lv_gridnav container, so the arrows select a
 * tile (a ring inside its edge, ui_st_focus_in) and Enter shows it full
 * screen; Tab reaches the navigation bar, which comes up while it has the
 * focus. The shortcuts (1, 4, pages, menu) are in ui_live_key.
 */
#include <stdio.h>
#include <string.h>

#include "hal_ui.h"
#include "icons.h"
#include "ui_app.h"
#include "ui_layout.h"
#include "ui_theme.h"

#define NAV_EDGE   3            /* px from the bottom that bring the bar up */
#define NAV_POLL   100          /* ms */
#define ARROW_EDGE 80           /* px from the left or right edge that bring up a page arrow */
#define ARROW_IDLE 2000         /* ms without the pointer moving that put it away */

/* Where the clock is: LV_ALIGN_TOP_LEFT, _TOP_RIGHT, _BOTTOM_LEFT,
 * _BOTTOM_RIGHT, or LV_ALIGN_DEFAULT for none. A constant until the UI
 * has settings of its own. */
#define CLOCK_CORNER LV_ALIGN_TOP_RIGHT

#define N_EVENT_ICONS 4

typedef struct {
    int         ch;
    lv_obj_t   *obj;
    lv_obj_t   *name_chip, *name;      /* bottom left */
    lv_obj_t   *icons, *rec;           /* bottom right: REC, then the events */
    lv_obj_t   *ev[N_EVENT_ICONS];
    lv_obj_t   *mid, *mid_l1, *mid_l2; /* centre: the state, when there is no picture */
    lv_obj_t   *add;                   /* centre: "+" on an empty slot */
    lv_obj_t   *flash;
    lv_timer_t *flash_timer;
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
    cell_t     cells[UI_LAYOUT_MAX];
    int        n_cells;
    uint32_t   click_ms;
    int        click_ch;
    int        nav_shown;
    uint32_t   nav_seen_ms;             /* the pointer was last on the edge or the bar */
    lv_color_t hole;
    lv_opa_t   hole_opa;
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

const char *ui_live_clock_corner(void)
{
    switch (CLOCK_CORNER) {
    case LV_ALIGN_TOP_LEFT:     return "top left";
    case LV_ALIGN_TOP_RIGHT:    return "top right";
    case LV_ALIGN_BOTTOM_LEFT:  return "bottom left";
    case LV_ALIGN_BOTTOM_RIGHT: return "bottom right";
    default:                    return "off";
    }
}

/* The settings section with `key`, or -1. */
static int section_of(const char *key)
{
    for (int i = 0; i < ui_model()->n_sections; ++i)
        if (!strcmp(ui_model()->sections[i].key, key)) return i;
    return -1;
}

/* ---- one tile ----------------------------------------------------------------------- */

static lv_obj_t *chip(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    ui_plain(c);
    lv_obj_add_style(c, &ui_st_chip, 0);
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
    case UI_CH_DISABLED:   *col = UI_C(TEXT2); return "Disabled";
    case UI_CH_CONNECTING: *col = UI_C(WARN); return "Connecting...";
    case UI_CH_LIVE:       *col = UI_C(WARN); return "No decoder";      /* live, nothing to show it */
    case UI_CH_NO_VIDEO:   return "No video";
    case UI_CH_OFFLINE:    return "Offline";
    case UI_CH_AUTH:       return "Authentication error";
    case UI_CH_CODEC:      return "Unsupported codec";
    default:               *detail = st->state; return "No video";
    }
}

static void cell_update(cell_t *c)
{
    if (c->ch < 0) {                    /* past the platform's channels */
        show(c->name_chip, 0);
        show(c->icons, 0);
        show(c->mid, 0);
        show(c->add, 0);
        return;
    }
    const ui_chan_t *st = ui_channel(c->ch);
    char buf[64];
    ui_chan_title(c->ch, buf, sizeof buf);
    lv_label_set_text(c->name, buf);
    int empty = st->status == UI_CH_EMPTY;
    lv_obj_set_style_text_color(c->name, empty ? UI_C(TEXT2) : UI_C(TEXT), 0);

    show(c->rec, st->recording);
    int any = st->recording;
    for (int i = 0; i < N_EVENT_ICONS; ++i) {
        show(c->ev[i], (st->events >> i) & 1);
        any |= (st->events >> i) & 1;
    }
    show(c->icons, any);

    show(c->add, empty);
    if (empty || (st->status == UI_CH_LIVE && !st->no_decoder)) {
        show(c->mid, 0);
        return;
    }
    lv_color_t col;
    const char *detail;
    lv_label_set_text(c->mid_l1, status_text(st, &col, &detail));
    lv_obj_set_style_text_color(c->mid_l1, col, 0);
    lv_label_set_text(c->mid_l2, detail);
    show(c->mid_l2, *detail && !c->tiny);
    show(c->mid, 1);
}

static void cell_observer_cb(lv_observer_t *o, lv_subject_t *s)
{
    (void)s;
    cell_update(lv_observer_get_user_data(o));
}

static void set_layout(int layout, int page);

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
        set_layout(1, ch);
        L.fs = 1;
    }
}

/* After the click's event: the new layout deletes the tile clicked. */
static void full_screen_async(void *ch) { toggle_full_screen((int)(intptr_t)ch); }

/* Channels -> a new camera on channel `ch` (after the click's event). */
static void add_async(void *ch)
{
    int sec = section_of("cameras");
    if (sec >= 0) ui_form_open_slot(sec, (int)(intptr_t)ch);
}

static void add_click_cb(lv_event_t *e)
{
    cell_t *c = lv_event_get_user_data(e);
    if (c->ch >= 0) lv_async_call(add_async, (void *)(intptr_t)c->ch);
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
    L.sel_ch = c->ch;
}

static void cell_deleted_cb(lv_event_t *e)
{
    cell_t *c = lv_event_get_user_data(e);
    if (c->flash_timer) lv_timer_delete(c->flash_timer);
    c->flash_timer = NULL;
}

/* Room the clock takes from a chip about `w` wide at the tile's left
 * edge, at its top (top != 0) or bottom: the chip moves past the clock
 * when the two would overlap. */
static int clock_offset(const ui_cell_t *g, int top, int w)
{
    if (!L.clock) return 0;
    lv_area_t c;
    lv_obj_get_coords(L.clock, &c);
    int x0 = g->x + VG_SPACE_SM, x1 = x0 + w;
    if (x1 < c.x1 || x0 > c.x2) return 0;
    if (top ? !(g->y == 0 && c.y1 < L.sh / 2) : !(g->y + g->h == L.sh && c.y1 > L.sh / 2)) return 0;
    return lv_obj_get_height(L.clock) + VG_SPACE_SM;
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
    lv_obj_set_style_border_color(o, UI_C(LINE), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    /* The focus ring inside the tile's edge, over the video's outermost
     * pixels: outside it would cover the neighbour. */
    lv_obj_add_style(o, &ui_st_focus_in, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(o, cell_click_cb, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(o, cell_focused_cb, LV_EVENT_FOCUSED, c);
    lv_obj_add_event_cb(o, cell_deleted_cb, LV_EVENT_DELETE, c);
    c->obj = o;

    int small = g->w < 400;
    c->tiny = g->w < 240;
    int edge = clock_offset(g, 0, 200);
    c->name_chip = chip(o);
    lv_obj_align(c->name_chip, LV_ALIGN_BOTTOM_LEFT, VG_SPACE_SM, -VG_SPACE_SM - edge);
    c->name = ui_label(c->name_chip, "", small ? VG_FONT_SM_LV : VG_FONT_MD_LV, UI_C(TEXT));
    lv_obj_set_style_max_width(c->name, g->w / 2, 0);
    lv_label_set_long_mode(c->name, LV_LABEL_LONG_CLIP);

    /* Icons only, small, in the design tokens' colours. */
    static const lv_image_dsc_t *const ev_img[N_EVENT_ICONS] = { &ui_icon_ev_motion, &ui_icon_ev_human,
                                                                 &ui_icon_ev_face, &ui_icon_ev_vehicle };
    c->icons = chip(o);
    lv_obj_set_style_pad_ver(c->icons, VG_SPACE_XS, 0);
    lv_obj_align(c->icons, LV_ALIGN_BOTTOM_RIGHT, -VG_SPACE_SM, -VG_SPACE_SM);
    c->rec = ui_icon(c->icons, &ui_icon_rec, VG_COLOR_BAD);
    for (int i = 0; i < N_EVENT_ICONS; ++i) c->ev[i] = ui_icon(c->icons, ev_img[i], VG_COLOR_WARN);

    c->mid = chip(o);
    lv_obj_set_flex_flow(c->mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c->mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(c->mid, c->tiny ? VG_SPACE_XS : VG_SPACE_MD, 0);
    lv_obj_set_style_pad_row(c->mid, VG_SPACE_XS, 0);
    lv_obj_center(c->mid);
    c->mid_l1 = ui_label(c->mid, "", c->tiny ? VG_FONT_SM_LV : small ? VG_FONT_MD_LV : VG_FONT_LG_LV, UI_C(TEXT2));
    c->mid_l2 = ui_label(c->mid, "", VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_set_style_max_width(c->mid_l2, g->w - 4 * VG_SPACE_LG, 0);
    lv_obj_set_style_text_align(c->mid_l2, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(c->mid_l2, LV_LABEL_LONG_WRAP);

    /* An empty slot: a large "+" that adds a camera on this channel. */
    c->add = lv_button_create(o);
    lv_obj_remove_style_all(c->add);
    lv_obj_set_size(c->add, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(c->add, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c->add, UI_C(CHIP), 0);
    lv_obj_set_style_bg_opa(c->add, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(c->add, c->tiny ? 0 : VG_SPACE_SM, 0);
    lv_obj_set_style_border_color(c->add, UI_C(ACCENT), LV_STATE_HOVERED);
    lv_obj_set_style_border_width(c->add, 2, LV_STATE_HOVERED);
    lv_obj_center(c->add);
    lv_obj_add_event_cb(c->add, add_click_cb, LV_EVENT_CLICKED, c);
    /* Two sizes rendered: LVGL does not scale A8 images. */
    ui_icon(c->add, small ? &ui_icon_add_sm : &ui_icon_add, VG_COLOR_TEXT2);
    lv_obj_add_flag(c->add, LV_OBJ_FLAG_HIDDEN);

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

static void set_layout(int layout, int page)
{
    if (!ui_layout_valid(layout)) return;
    int n = n_channels(), pages = ui_layout_pages(layout, n);
    page = (page % pages + pages) % pages;      /* paging wraps around */
    L.layout = layout;
    L.page = page;
    L.fs = 0;
    L.click_ch = -1;

    lv_obj_clean(L.grid);
    ui_cell_t g[UI_LAYOUT_MAX];
    L.n_cells = ui_layout_cells(L.sw, L.sh, layout, page, n, g);
    ui_rect_t tiles[UI_MAX_CHANNELS];
    memset(tiles, 0, sizeof tiles);
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
    if (ui_model()->show_tiles && !ui_model()->fixed_tiles) ui_model()->show_tiles(tiles, n);
    if (L.nav) nav_update();
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

/* The clock over the video: an opaque chip (a translucent one would show
 * as a pink fringe on a colour-keyed layer). */
static void make_clock(void)
{
    if (CLOCK_CORNER == LV_ALIGN_DEFAULT) return;
    L.clock = lv_obj_create(L.scr);
    ui_plain(L.clock);
    lv_obj_add_style(L.clock, &ui_st_chip, 0);
    lv_obj_set_style_pad_hor(L.clock, VG_SPACE_MD, 0);
    lv_obj_set_size(L.clock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(L.clock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = ui_label(L.clock, "", VG_FONT_MD_LV, UI_C(TEXT));
    lv_label_bind_text(l, &ui_subj_clock, NULL);
    int dx = CLOCK_CORNER == LV_ALIGN_TOP_LEFT || CLOCK_CORNER == LV_ALIGN_BOTTOM_LEFT ? VG_SPACE_SM : -VG_SPACE_SM;
    int dy = CLOCK_CORNER == LV_ALIGN_TOP_LEFT || CLOCK_CORNER == LV_ALIGN_TOP_RIGHT ? VG_SPACE_SM : -VG_SPACE_SM;
    lv_obj_align(L.clock, CLOCK_CORNER, dx, dy);
    lv_obj_update_layout(L.clock);
}

/* ---- navigation bar ------------------------------------------------------------------------ */

enum { ACT_MENU = 1, ACT_PREV, ACT_NEXT, ACT_PIN, ACT_FULL, ACT_CONTEXT, ACT_ESC, ACT_CHANNELS, ACT_RECORD,
       ACT_DISPLAY, ACT_LAYOUT = 100 };


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
        if (section_of("cameras") >= 0) ui_list_open(section_of("cameras"));
        break;
    case ACT_RECORD:
        if (section_of("record") >= 0) ui_form_open(section_of("record"), -1);
        break;
    case ACT_DISPLAY:
        ui_display_open();
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
        lv_obj_t *b = lv_button_create(L.scr);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, VG_SIZE_PAGE_ARROW, VG_SIZE_PAGE_ARROW);
        lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(b, UI_C(CHIP), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, UI_C(LINE), 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_bg_color(b, UI_C(ACCENT), LV_STATE_HOVERED);
        lv_obj_set_style_border_color(b, UI_C(ACCENT), LV_STATE_HOVERED);
        lv_obj_add_style(b, &ui_st_btn_pressed, LV_STATE_PRESSED);
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

/* Main menu, Channels, Playback, Record, PTZ, Display, Split: nothing
 * else (full screen is a double click or Enter, pages the edge arrows or
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

    make_clock();
    make_arrows();
    make_nav();
    set_layout(n_channels() > 1 && layout_offered(4) ? 4 : 1, 0);
    return L.scr;
}
