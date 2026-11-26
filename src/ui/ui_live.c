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
 */
#include <stdio.h>
#include <string.h>

#include "hal_ui.h"
#include "ui_app.h"
#include "ui_layout.h"
#include "ui_theme.h"

#define NAV_EDGE   3            /* px from the bottom that bring the bar up */
#define NAV_POLL   100          /* ms */

/* Where the clock is: LV_ALIGN_TOP_LEFT, _TOP_RIGHT, _BOTTOM_LEFT,
 * _BOTTOM_RIGHT, or LV_ALIGN_DEFAULT for none. A constant until the UI
 * has settings of its own. */
#define CLOCK_CORNER LV_ALIGN_TOP_RIGHT

typedef struct {
    int         ch;
    lv_obj_t   *obj;
    lv_obj_t   *title;          /* top-left chip: name, REC, motion */
    lv_obj_t   *rec, *motion;
    lv_obj_t   *mid, *mid_l1, *mid_l2; /* centre chip: state */
    lv_obj_t   *info, *info_l;  /* bottom-left chip: stream */
    lv_obj_t   *flash;
    lv_timer_t *flash_timer;
} cell_t;

static const int k_layouts[] = { 1, 4, 9, 16 };
#define N_LAYOUTS (int)(sizeof k_layouts / sizeof k_layouts[0])

static struct {
    lv_obj_t  *scr, *grid, *clock, *summary;
    lv_obj_t  *nav, *nav_page, *nav_prev, *nav_next, *nav_pin;
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

/* A layout is offered when the next smaller one cannot show every channel
 * on one page. */
static int layout_offered(int layout)
{
    if (ui_model()->fixed_tiles) return layout == 4;
    for (int i = 0; i < N_LAYOUTS; ++i)
        if (k_layouts[i] == layout) return i == 0 || n_channels() > k_layouts[i - 1];
    return 0;
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

static void cell_update(cell_t *c)
{
    if (c->ch < 0) {
        lv_obj_add_flag(lv_obj_get_parent(c->title), LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c->mid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c->info, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const ui_chan_t *st = ui_channel(c->ch);
    char buf[64];
    ui_chan_title(c->ch, buf, sizeof buf);
    lv_label_set_text(c->title, buf);
    if (st->recording) lv_obj_remove_flag(c->rec, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(c->rec, LV_OBJ_FLAG_HIDDEN);
    if (st->motion) lv_obj_remove_flag(c->motion, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(c->motion, LV_OBJ_FLAG_HIDDEN);

    if (st->configured && st->live) {
        lv_obj_add_flag(c->mid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(c->info, LV_OBJ_FLAG_HIDDEN);
        if (st->width)
            snprintf(buf, sizeof buf, "%s  %ux%u  %u fps", st->codec, st->width, st->height, st->fps);
        else
            snprintf(buf, sizeof buf, "%u fps", st->fps);
        lv_label_set_text(c->info_l, buf);
        return;
    }
    lv_obj_add_flag(c->info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(c->mid, LV_OBJ_FLAG_HIDDEN);
    const char *l1, *l2 = "";
    lv_color_t col;
    if (!st->configured) {
        l1 = "No camera";
        col = UI_C(TEXT2);
    } else if (!st->state[0] || !strcmp(st->state, "CONNECTING")) {
        l1 = "Connecting...";
        col = UI_C(WARN);
    } else {
        l1 = "No video";
        l2 = st->state;
        col = UI_C(BAD);
    }
    lv_label_set_text(c->mid_l1, l1);
    lv_obj_set_style_text_color(c->mid_l1, col, 0);
    lv_label_set_text(c->mid_l2, l2);
    if (*l2) lv_obj_remove_flag(c->mid_l2, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(c->mid_l2, LV_OBJ_FLAG_HIDDEN);
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

static void cell_click_cb(lv_event_t *e)
{
    cell_t *c = lv_event_get_user_data(e);
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

static void cell_deleted_cb(lv_event_t *e)
{
    cell_t *c = lv_event_get_user_data(e);
    if (c->flash_timer) lv_timer_delete(c->flash_timer);
    c->flash_timer = NULL;
}

/* Room the clock takes from a chip in the tile's corner `corner`. */
static int clock_offset(const ui_cell_t *g, lv_align_t corner)
{
    if (!L.clock || corner != CLOCK_CORNER) return 0;
    int left = corner == LV_ALIGN_TOP_LEFT || corner == LV_ALIGN_BOTTOM_LEFT;
    int top = corner == LV_ALIGN_TOP_LEFT || corner == LV_ALIGN_TOP_RIGHT;
    if ((left ? g->x != 0 : g->x + g->w != L.sw) || (top ? g->y != 0 : g->y + g->h != L.sh)) return 0;
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
    lv_obj_add_event_cb(o, cell_click_cb, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(o, cell_deleted_cb, LV_EVENT_DELETE, c);
    c->obj = o;

    int small = g->w < 400;
    lv_obj_t *t = chip(o);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, VG_SPACE_SM, VG_SPACE_SM + clock_offset(g, LV_ALIGN_TOP_LEFT));
    c->title = ui_label(t, "", small ? VG_FONT_SM_LV : VG_FONT_MD_LV, UI_C(TEXT));
    /* REC: a red dot and the word, as on a camcorder. */
    c->rec = lv_obj_create(t);
    ui_plain(c->rec);
    lv_obj_remove_flag(c->rec, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(c->rec, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(c->rec, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(c->rec, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c->rec, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c->rec, VG_SPACE_XS, 0);
    lv_obj_t *dot = lv_obj_create(c->rec);
    ui_plain(dot);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dot, VG_SIZE_ICON_DOT, VG_SIZE_ICON_DOT);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, UI_C(BAD), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    ui_label(c->rec, "REC", VG_FONT_SM_LV, UI_C(BAD));
    c->motion = ui_label(t, LV_SYMBOL_EYE_OPEN, VG_FONT_SM_LV, UI_C(WARN));

    c->mid = chip(o);
    lv_obj_set_flex_flow(c->mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c->mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(c->mid, VG_SPACE_MD, 0);
    lv_obj_set_style_pad_row(c->mid, VG_SPACE_XS, 0);
    lv_obj_center(c->mid);
    c->mid_l1 = ui_label(c->mid, "", small ? VG_FONT_MD_LV : VG_FONT_LG_LV, UI_C(TEXT2));
    c->mid_l2 = ui_label(c->mid, "", VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_set_style_max_width(c->mid_l2, g->w - 4 * VG_SPACE_LG, 0);
    lv_obj_set_style_text_align(c->mid_l2, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(c->mid_l2, LV_LABEL_LONG_WRAP);

    c->info = chip(o);
    lv_obj_align(c->info, LV_ALIGN_BOTTOM_LEFT, VG_SPACE_SM,
                 -VG_SPACE_SM - clock_offset(g, LV_ALIGN_BOTTOM_LEFT));
    c->info_l = ui_label(c->info, "", VG_FONT_SM_LV, UI_C(TEXT2));

    c->flash = lv_obj_create(o);
    ui_plain(c->flash);
    lv_obj_set_size(c->flash, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_border_color(c->flash, UI_C(WARN), 0);
    lv_obj_set_style_border_width(c->flash, 3, 0);
    lv_obj_remove_flag(c->flash, LV_OBJ_FLAG_CLICKABLE);
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
        if (k_layouts[i] == L.layout) lv_obj_add_state(L.layout_btn[i], LV_STATE_CHECKED);
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
    for (int i = 0; i < L.n_cells; ++i) {
        cell_build(&L.cells[i], &g[i]);
        if (g[i].ch >= 0) tiles[g[i].ch] = (ui_rect_t){ g[i].x, g[i].y, g[i].w, g[i].h };
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

enum { ACT_MENU = 1, ACT_PREV, ACT_NEXT, ACT_PIN, ACT_FULL, ACT_LAYOUT = 100 };

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

static void nav_poll_cb(lv_timer_t *t)
{
    (void)t;
    lv_point_t p;
    if (lv_screen_active() != L.scr) return;
    if (lv_subject_get_int(&ui_subj_nav_pinned)) return;
    if (!ui_pointer(&p)) return;
    if (p.y >= L.sh - NAV_EDGE) {
        nav_show(1);
        L.nav_seen_ms = lv_tick_get();
    } else if (L.nav_shown && (p.y >= L.sh - VG_SIZE_NAVBAR || ui_popup_is_open())) {
        L.nav_seen_ms = lv_tick_get();
    } else if (L.nav_shown && lv_tick_elaps(L.nav_seen_ms) >= VG_DURATION_NAVBAR_HIDE) {
        nav_show(0);
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
    static const char *const names[] = { "1x1", "2x2", "3x3", "4x4" };
    int any = 0;
    for (int i = 0; i < N_LAYOUTS; ++i) {
        if (!layout_offered(k_layouts[i]) || ui_model()->fixed_tiles) continue;
        L.layout_btn[i] = nav_btn(NULL, names[i], ACT_LAYOUT + k_layouts[i]);
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
    lv_obj_add_event_cb(b, action_cb, LV_EVENT_CLICKED, (void *)(intptr_t)act);
    return b;
}

static void menu_sep(lv_obj_t *panel)
{
    lv_obj_t *s = lv_obj_create(panel);
    ui_plain(s);
    lv_obj_set_size(s, LV_PCT(100), 1);
    lv_obj_set_style_bg_color(s, UI_C(LINE), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
}

/* The channel in the tile under `at`, or -1. */
static int channel_at(const lv_point_t *at)
{
    for (int i = 0; at && i < L.n_cells; ++i) {
        lv_area_t a;
        lv_obj_get_coords(L.cells[i].obj, &a);
        if (at->x >= a.x1 && at->x <= a.x2 && at->y >= a.y1 && at->y <= a.y2) return L.cells[i].ch;
    }
    return -1;
}

void ui_live_context_menu(const lv_point_t *at)
{
    lv_obj_t *panel = ui_popup_open(at, 260);
    menu_item(panel, LV_SYMBOL_LIST, "Main menu", ACT_MENU, -1);
    if (!ui_model()->fixed_tiles) {
        int ch = channel_at(at);
        char buf[64], t[40];
        if (ch >= 0) {
            ui_chan_title(ch, t, sizeof t);
            snprintf(buf, sizeof buf, L.layout == 1 ? "Back to the grid" : "Full screen: %s", t);
            menu_item(panel, LV_SYMBOL_IMAGE, buf, ACT_FULL, ch);
        }
        menu_sep(panel);
        static const char *const names[] = { "Single view", "2x2 view", "3x3 view", "4x4 view" };
        for (int i = 0; i < N_LAYOUTS; ++i) {
            if (!layout_offered(k_layouts[i])) continue;
            lv_obj_t *b = menu_item(panel, NULL, names[i], ACT_LAYOUT + k_layouts[i], -1);
            if (k_layouts[i] == L.layout) lv_obj_set_style_text_color(b, UI_C(ACCENT), 0);
        }
        if (ui_layout_pages(L.layout, n_channels()) > 1) {
            menu_sep(panel);
            menu_item(panel, LV_SYMBOL_LEFT, "Previous page", ACT_PREV, -1);
            menu_item(panel, LV_SYMBOL_RIGHT, "Next page", ACT_NEXT, -1);
        }
    }
    menu_sep(panel);
    menu_item(panel, LV_SYMBOL_DOWNLOAD,
              lv_subject_get_int(&ui_subj_nav_pinned) ? "Unpin navigation bar" : "Pin navigation bar",
              ACT_PIN, -1);
    ui_popup_place(at);
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
 * configuration that could not be applied, and a camera going offline or
 * coming back while its tile is not on the screen. "REC on", "added",
 * "config applied" and the rest are left to the tiles. The same text for
 * several cameras within a toast's time is one toast ("3 cameras: ..."). */
static void event_toast(const ui_event_t *ev)
{
    static char last[sizeof ev->text];
    static uint32_t last_ms;
    static int count;
    const char *t = ev->text;
    ui_toast_t kind;
    if (!strncmp(t, "REC ", 4) && strcmp(t, "REC on"))
        kind = UI_TOAST_WARN;
    else if (!strcmp(t, "no decoder") || !strcmp(t, "bad URL") || (!strncmp(t, "config:", 7) && strstr(t, "failed")))
        kind = UI_TOAST_BAD;
    else if ((!strcmp(t, "offline") || !strcmp(t, "online")) && ev->ch > 0 && !on_screen(ev->ch - 1))
        kind = !strcmp(t, "online") ? UI_TOAST_OK : UI_TOAST_WARN;
    else
        return;
    if (ev->ch > 0 && !strcmp(last, t) && lv_tick_elaps(last_ms) < VG_DURATION_TOAST)
        ++count;
    else
        count = 1;
    snprintf(last, sizeof last, "%s", t);
    last_ms = lv_tick_get();
    char who[40];
    if (ev->ch <= 0) ui_toast(kind, "%s", t);
    else if (count > 1) ui_toast(kind, "%d cameras: %s", count, t);
    else {
        ui_chan_title(ev->ch - 1, who, sizeof who);
        ui_toast(kind, "%s: %s", who, t);
    }
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
    L.sw = lv_display_get_horizontal_resolution(NULL);
    L.sh = lv_display_get_vertical_resolution(NULL);
    hal_ui_hole(&L.hole, &L.hole_opa);

    L.scr = lv_obj_create(NULL);
    ui_plain(L.scr);
    lv_obj_add_style(L.scr, &ui_st_screen, 0);

    L.grid = lv_obj_create(L.scr);
    ui_plain(L.grid);
    lv_obj_set_size(L.grid, L.sw, L.sh);
    /* With alpha holes (not a colour key), whatever lies under a tile must
     * be transparent too, or it would paint over the video. */
    if (L.hole_opa < LV_OPA_COVER) lv_obj_set_style_bg_opa(L.scr, LV_OPA_TRANSP, 0);

    make_clock();
    make_nav();
    set_layout(n_channels() > 1 && layout_offered(4) ? 4 : 1, 0);
    return L.scr;
}
