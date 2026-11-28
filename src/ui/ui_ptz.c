/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_ptz.c — the PTZ panel over a camera's picture, full screen.
 *
 *   picture           press and hold to move that way, faster further from
 *                     the middle; the wheel zooms; a chip at the top says
 *                     what moves
 *   panel (right)     the camera and its state; a direction pad (eight
 *                     directions, home or stop in the middle); zoom, focus
 *                     and iris -/+; the speed; the presets (go to one by a
 *                     click on it; store, rename, delete; a new one at the
 *                     position now); the tours
 *
 * What the camera cannot do is not shown (ui_ptz_t.caps, from the core's
 * probe). A move lasts while something holds it: a button or the picture
 * pressed, a key down, the wheel turning. The panel repeats it every
 * VG_DURATION_PTZ_REPEAT ms, each repeat good for VG_DURATION_PTZ_HOLD
 * ms in the core, and stops it on the release, when the pointer leaves
 * what it pressed (PRESS_LOST), when the panel closes and when the
 * screen changes; if all of that were lost, the core's hold runs out.
 * The keys (ui_ptz_key): arrows move, + and - zoom, PgUp and PgDn focus,
 * Home the home position, digits the presets, Esc closes. Their release
 * (ui_ptz_key_up) stops; a keyboard that never says it (unplugged) is
 * stopped by the hold.
 *
 * Everything over the video is opaque and square (ui_theme.h).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "ui_app.h"
#include "ui_osk.h"
#include "ui_theme.h"

#define PAD_GAP     4
#define LENS_BTN    40
#define ROW_H       36
#define DEAD        0.08                /* of the half picture: no move near the middle */
#define WHEEL_MS    400                 /* one notch of the wheel zooms this long */
#define SPEED_DEF   5
#define HOLD_LIMIT  120000              /* ms: a move held longer without a change ends */

enum { HOLD_NONE, HOLD_BUTTON, HOLD_PICTURE, HOLD_KEY, HOLD_WHEEL };
enum { K_UP = 1, K_DOWN = 2, K_LEFT = 4, K_RIGHT = 8, K_IN = 16, K_OUT = 32, K_FAR = 64, K_NEAR = 128 };

/* The pad's buttons, row by row: pan, tilt; the middle is home (or stop). */
static const struct {
    const lv_image_dsc_t *img;
    signed char           pan, tilt;
} k_pad[9] = {
    { &ui_icon_ptz_up_left, -1, 1 },  { &ui_icon_ptz_up, 0, 1 },   { &ui_icon_ptz_up_right, 1, 1 },
    { &ui_icon_ptz_left, -1, 0 },     { &ui_icon_ptz_home, 0, 0 }, { &ui_icon_ptz_right, 1, 0 },
    { &ui_icon_ptz_down_left, -1, -1 }, { &ui_icon_ptz_down, 0, -1 }, { &ui_icon_ptz_down_right, 1, -1 },
};

static struct {
    lv_obj_t   *root, *pad, *panel, *body, *chip, *chip_label;
    lv_obj_t   *title, *status, *speed, *speed_val, *list, *tour_dd;
    lv_obj_t   *home_btn;
    lv_group_t *group;
    lv_timer_t *timer;
    void      (*on_close)(void);
    int         open, ch, sw, sh, panel_x;
    ui_ptz_t    info;
    unsigned    built_caps;
    int         built_state, built_tours;
    char        built_presets[UI_PTZ_PRESETS * 64];
    unsigned    err_seen;
    int         hold;
    double      v[5];                   /* pan, tilt, zoom, focus, iris */
    unsigned    keys;                   /* K_*: held */
    uint32_t    key_ms;                 /* a key's press or repeat last came */
    uint32_t    hold_ms;                /* the move held began (or changed) */
    uint32_t    wheel_until;
    /* the name dialog */
    lv_obj_t   *name_ta, *osk;
    char        name_token[32];
    int         name_new;
} P;

static int g_speed = SPEED_DEF;         /* the speed slider's, kept from one opening to the next */

static int speed_now(void) { return g_speed; }
static int has(unsigned cap) { return (P.info.caps & cap) != 0; }

static const char *dir_word(double pan, double tilt)
{
    static const char *const w[8] = { "right", "up right", "up", "up left", "left", "down left", "down", "down right" };
    if (fabs(pan) < 0.15 && fabs(tilt) < 0.15) return "";
    int k = (int)lround(atan2(tilt, pan) * 4.0 / M_PI);
    return w[(k % 8 + 8) % 8];
}

/* ---- moving ------------------------------------------------------------------------------ */

static void chip_show(const char *text)
{
    if (!P.chip) return;
    if (!text || !*text) {
        lv_obj_add_flag(P.chip, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(P.chip_label, text);
    lv_obj_remove_flag(P.chip, LV_OBJ_FLAG_HIDDEN);
}

static void chip_update(void)
{
    char t[64] = "";
    double *v = P.v;
    int pct = (int)lround(100 * fmax(fmax(fabs(v[0]), fabs(v[1])), fmax(fabs(v[2]), fmax(fabs(v[3]), fabs(v[4])))));
    if (fabs(v[0]) >= 0.15 || fabs(v[1]) >= 0.15)
        snprintf(t, sizeof t, "Moving %s  %d %%", dir_word(v[0], v[1]), pct);
    else if (fabs(v[2]) >= 0.01)
        snprintf(t, sizeof t, "Zoom %s", v[2] > 0 ? "in" : "out");
    else if (fabs(v[3]) >= 0.01)
        snprintf(t, sizeof t, "Focus %s", v[3] > 0 ? "far" : "near");
    else if (fabs(v[4]) >= 0.01)
        snprintf(t, sizeof t, "Iris %s", v[4] > 0 ? "open" : "close");
    chip_show(t);
}

static void refused(int rc)
{
    if (rc == 0) return;
    ui_toast(UI_TOAST_WARN, "PTZ: %s", rc == -2 ? "the camera cannot do that" : rc == -4 ? "no PTZ on this camera"
                                       : rc == -3 ? "busy, try again" : "refused");
}

/* P.v to the camera, good for VG_DURATION_PTZ_HOLD: a lens move or a
 * pan/tilt/zoom one; nothing: a stop. */
static void send_v(void)
{
    const ui_model_t *m = ui_model();
    if (!m->ptz_move) return;
    int rc;
    if (fabs(P.v[3]) > 0.01) rc = m->ptz_lens(P.ch, UI_PTZ_FOCUS, P.v[3], VG_DURATION_PTZ_HOLD);
    else if (fabs(P.v[4]) > 0.01) rc = m->ptz_lens(P.ch, UI_PTZ_IRIS, P.v[4], VG_DURATION_PTZ_HOLD);
    else if (fabs(P.v[0]) > 0.01 || fabs(P.v[1]) > 0.01 || fabs(P.v[2]) > 0.01)
        rc = m->ptz_move(P.ch, P.v[0], P.v[1], P.v[2], VG_DURATION_PTZ_HOLD);
    else rc = m->ptz_stop(P.ch);
    if (rc) {
        memset(P.v, 0, sizeof P.v);
        P.hold = HOLD_NONE;
        refused(rc);
    }
    chip_update();
}

static void hold_start(int how, double pan, double tilt, double zoom, double focus, double iris)
{
    double v[5] = { pan, tilt, zoom, focus, iris };
    int changed = memcmp(v, P.v, sizeof v) != 0 || P.hold != how;
    memcpy(P.v, v, sizeof v);
    P.hold = how;
    if (changed) {
        P.hold_ms = lv_tick_get();
        send_v();
    }
}

/* Whatever moves, stopped now. */
static void stop_all(void)
{
    int was = P.hold != HOLD_NONE;
    for (int i = 0; i < 5; ++i) was |= P.v[i] != 0;
    P.hold = HOLD_NONE;
    P.keys = 0;
    memset(P.v, 0, sizeof P.v);
    if (was && ui_model()->ptz_stop) ui_model()->ptz_stop(P.ch);
    chip_show(NULL);
}

/* ---- the picture ------------------------------------------------------------------------- */

/* The pointer as a move: from the middle of the picture to it, -1..1 on
 * each axis at the picture's edge; nothing near the middle. */
static int picture_vector(const lv_point_t *p, double *pan, double *tilt)
{
    double cx = P.sw / 2.0, cy = P.sh / 2.0;
    double x = (p->x - cx) / cx, y = (cy - p->y) / cy;
    if (p->x < 0 || p->x >= P.panel_x || p->y < 0 || p->y >= P.sh) return -1;   /* off the picture */
    x = x < -1 ? -1 : x > 1 ? 1 : x;
    y = y < -1 ? -1 : y > 1 ? 1 : y;
    if (hypot(x, y) < DEAD) x = y = 0;
    *pan = x;
    *tilt = y;
    return 0;
}

static void pad_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (P.hold == HOLD_PICTURE) stop_all();
        return;
    }
    if (!in || lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER || !has(UI_PTZ_CAP_PAN_TILT)) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    double pan, tilt;
    if (picture_vector(&p, &pan, &tilt) < 0) {
        if (P.hold == HOLD_PICTURE) stop_all();
        return;
    }
    if (code == LV_EVENT_PRESSED || (code == LV_EVENT_PRESSING && P.hold == HOLD_PICTURE)) {
        /* A small movement of the pointer is not a new command. */
        if (P.hold == HOLD_PICTURE && fabs(pan - P.v[0]) < 0.04 && fabs(tilt - P.v[1]) < 0.04) return;
        hold_start(HOLD_PICTURE, pan, tilt, 0, 0, 0);
    }
}

int ui_ptz_wheel(int steps)
{
    lv_point_t p;
    if (!P.open || !ui_pointer(&p) || p.x >= P.panel_x || ui_popup_is_open()) return 0;
    if (!has(UI_PTZ_CAP_ZOOM) || P.hold == HOLD_PICTURE || P.hold == HOLD_BUTTON) return 1;
    int n = abs(steps);
    P.wheel_until = lv_tick_get() + (uint32_t)(WHEEL_MS * (n > 4 ? 4 : n));
    hold_start(HOLD_WHEEL, 0, 0, (steps > 0 ? 1 : -1) * speed_now() / 8.0, 0, 0);
    return 1;
}

/* ---- the panel's controls ----------------------------------------------------------------- */

static lv_obj_t *icon_btn(lv_obj_t *parent, const lv_image_dsc_t *img, int w, int h)
{
    lv_obj_t *b = ui_btn(parent, NULL, NULL);
    lv_obj_delete(lv_obj_get_child(b, 0));
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_min_height(b, 0, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_PRESS_LOCK);  /* sliding off it is a release */
    lv_obj_center(ui_icon(b, img, VG_COLOR_TEXT));
    lv_group_add_obj(P.group, b);
    return b;
}

static void pad_btn_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (i == 4) {                                    /* the middle: home, or stop */
        if (code != LV_EVENT_CLICKED) return;
        stop_all();
        if (has(UI_PTZ_CAP_HOME)) refused(ui_model()->ptz_home(P.ch));
        return;
    }
    if (code == LV_EVENT_PRESSED) {
        double s = speed_now() / 8.0;
        hold_start(HOLD_BUTTON, k_pad[i].pan * s, k_pad[i].tilt * s, 0, 0, 0);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (P.hold == HOLD_BUTTON) stop_all();
    }
}

/* Zoom, focus or iris: user data = axis * 2 + (1: +). */
static void lens_btn_cb(lv_event_t *e)
{
    int u = (int)(intptr_t)lv_event_get_user_data(e), ax = u / 2;
    double d = (u & 1) ? 1 : -1;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        double v[5] = { 0 };
        v[ax] = ax == 2 ? d * speed_now() / 8.0 : d;
        hold_start(HOLD_BUTTON, v[0], v[1], v[2], v[3], v[4]);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (P.hold == HOLD_BUTTON) stop_all();
    }
}

static void af_cb(lv_event_t *e)
{
    (void)e;
    stop_all();
    refused(ui_model()->ptz_focus_auto(P.ch));
}

static void speed_cb(lv_event_t *e)
{
    (void)e;
    if (P.speed) g_speed = (int)lv_slider_get_value(P.speed);
    lv_label_set_text_fmt(P.speed_val, "%d", speed_now());
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    ui_ptz_close();
}

/* ---- presets ------------------------------------------------------------------------------ */

static const ui_ptz_preset_t *preset_of(const char *token)
{
    for (int i = 0; i < P.info.n_presets; ++i)
        if (!strcmp(P.info.presets[i].token, token)) return &P.info.presets[i];
    return NULL;
}

static void preset_label(const ui_ptz_preset_t *p, char *buf, size_t cap)
{
    if (p->name[0]) snprintf(buf, cap, "%s (%s)", p->token, p->name);
    else snprintf(buf, cap, "%s", p->token);
}

static void goto_token(const char *token)
{
    stop_all();
    int rc = ui_model()->ptz_preset(P.ch, UI_PTZ_PRESET_GOTO, token, NULL);
    refused(rc);
    if (!rc) {
        const ui_ptz_preset_t *p = preset_of(token);
        char l[80];
        if (p) preset_label(p, l, sizeof l);
        ui_toast(UI_TOAST_OK, "Going to preset %s", p ? l : token);
    }
}

static void preset_go_cb(lv_event_t *e)
{
    const char *token = lv_event_get_user_data(e);
    goto_token(token);
}

static void store_now(void *u)
{
    const char *token = u;
    refused(ui_model()->ptz_preset(P.ch, UI_PTZ_PRESET_SET, token, NULL));
}

static void preset_save_cb(lv_event_t *e)
{
    const char *token = lv_event_get_user_data(e);
    const ui_ptz_preset_t *p = preset_of(token);
    char q[128], l[80];
    if (!p) return;
    preset_label(p, l, sizeof l);
    stop_all();
    snprintf(q, sizeof q, "Store the camera's position now as preset %s?", l);
    ui_confirm(q, "Store", store_now, (void *)p->token);
}

static void remove_now(void *u)
{
    refused(ui_model()->ptz_preset(P.ch, UI_PTZ_PRESET_REMOVE, (const char *)u, NULL));
}

static void preset_delete_cb(lv_event_t *e)
{
    const char *token = lv_event_get_user_data(e);
    const ui_ptz_preset_t *p = preset_of(token);
    char q[128], l[80];
    if (!p) return;
    preset_label(p, l, sizeof l);
    snprintf(q, sizeof q, "Delete preset %s?", l);
    ui_confirm(q, "Delete", remove_now, (void *)p->token);
}

/* The name dialog: a new preset at the position now, or a new name. */
static void name_done(int ok)
{
    char name[32];
    snprintf(name, sizeof name, "%s", P.name_ta ? lv_textarea_get_text(P.name_ta) : "");
    int is_new = P.name_new;
    char token[32];
    snprintf(token, sizeof token, "%s", P.name_token);
    ui_popup_close();
    P.name_ta = P.osk = NULL;
    if (!ok) return;
    int rc = is_new ? ui_model()->ptz_preset(P.ch, UI_PTZ_PRESET_SET, NULL, name)
                    : ui_model()->ptz_preset(P.ch, UI_PTZ_PRESET_RENAME, token, name);
    if (rc == -1) ui_toast(UI_TOAST_BAD, "PTZ: a name of up to 31 characters, no control characters");
    else refused(rc);
}

static void name_ok_cb(lv_event_t *e) { (void)e; name_done(1); }
static void name_cancel_cb(lv_event_t *e) { (void)e; name_done(0); }

static void name_ready_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);
    name_done(1);
}

static void name_click_cb(lv_event_t *e)
{
    if (!ui_keyboard_in_use() && P.osk) ui_osk_show(P.osk, lv_event_get_target(e), 0);
}

static void name_osk_cb(lv_event_t *e)
{
    ui_osk_hide(lv_event_get_target(e));
    if (lv_event_get_code(e) == LV_EVENT_READY) name_done(1);
}

static void name_dialog(const char *token)
{
    stop_all();
    P.name_new = token == NULL;
    snprintf(P.name_token, sizeof P.name_token, "%s", token ? token : "");
    const ui_ptz_preset_t *p = token ? preset_of(token) : NULL;
    lv_obj_t *panel = ui_popup_open(NULL, 440);
    lv_obj_set_style_pad_all(panel, VG_SPACE_XL, 0);
    lv_obj_set_style_pad_row(panel, VG_SPACE_MD, 0);
    lv_obj_set_style_border_color(panel, UI_C(ACCENT), 0);
    lv_obj_t *t = ui_label(panel, "", VG_FONT_LG_LV, UI_C(TEXT));
    if (P.name_new) lv_label_set_text(t, "New preset");
    else lv_label_set_text_fmt(t, "Rename preset %s", token);
    ui_label(panel, P.name_new ? "The camera's position now, named:" : "Name", VG_FONT_SM_LV, UI_C(TEXT2));
    P.name_ta = lv_textarea_create(panel);
    lv_textarea_set_one_line(P.name_ta, 1);
    lv_textarea_set_max_length(P.name_ta, 31);
    lv_textarea_set_text(P.name_ta, p ? p->name : "");
    lv_obj_set_width(P.name_ta, LV_PCT(100));
    lv_obj_add_event_cb(P.name_ta, name_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(P.name_ta, name_click_cb, LV_EVENT_CLICKED, NULL);
    ui_focus_style(P.name_ta);
    lv_obj_t *row = lv_obj_create(panel);
    ui_plain(row);
    ui_ring_room(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, VG_SPACE_MD, 0);
    lv_obj_t *no = ui_btn(row, NULL, "Cancel");
    lv_obj_add_event_cb(no, name_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok = ui_btn(row, NULL, P.name_new ? "Store" : "Rename");
    ui_btn_accent(ok);
    lv_obj_add_event_cb(ok, name_ok_cb, LV_EVENT_CLICKED, NULL);
    ui_popup_focus_add(P.name_ta);
    ui_popup_focus(row, ok, LV_GRIDNAV_CTRL_ROLLOVER);
    ui_popup_focus_obj(P.name_ta);
    P.osk = ui_osk_create(lv_obj_get_parent(panel));
    if (P.osk) {
        lv_obj_add_event_cb(P.osk, name_osk_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(P.osk, name_osk_cb, LV_EVENT_CANCEL, NULL);
    }
    ui_popup_place(NULL);
}

static void preset_new_cb(lv_event_t *e)
{
    (void)e;
    name_dialog(NULL);
}

static void preset_edit_cb(lv_event_t *e) { name_dialog(lv_event_get_user_data(e)); }

static void presets_build(void)
{
    lv_obj_clean(P.list);
    if (!P.info.n_presets) {
        lv_obj_t *l = ui_label(P.list, has(UI_PTZ_CAP_PRESETS) ? "None yet: move the camera, then +." : "",
                               VG_FONT_SM_LV, UI_C(TEXT2));
        lv_obj_set_style_pad_top(l, VG_SPACE_SM, 0);
        return;
    }
    for (int i = 0; i < P.info.n_presets; ++i) {
        const ui_ptz_preset_t *p = &P.info.presets[i];
        lv_obj_t *row = lv_obj_create(P.list);
        ui_plain(row);
        ui_ring_room(row);
        lv_obj_set_size(row, LV_PCT(100), ROW_H);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, PAD_GAP, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        char text[80];
        snprintf(text, sizeof text, "%s   %s", p->token, p->name[0] ? p->name : "");
        lv_obj_t *go = ui_btn(row, NULL, text);
        lv_obj_set_height(go, ROW_H - 4);
        lv_obj_set_style_min_height(go, 0, 0);
        lv_obj_set_flex_grow(go, 1);
        lv_obj_set_flex_align(go, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *gl = ui_btn_label(go);
        lv_label_set_long_mode(gl, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(gl, 1);
        lv_obj_add_event_cb(go, preset_go_cb, LV_EVENT_CLICKED, (void *)p->token);
        lv_group_add_obj(P.group, go);
        lv_obj_t *b = icon_btn(row, &ui_icon_ptz_save, ROW_H - 4, ROW_H - 4);
        lv_obj_add_event_cb(b, preset_save_cb, LV_EVENT_CLICKED, (void *)p->token);
        b = icon_btn(row, &ui_icon_ptz_edit, ROW_H - 4, ROW_H - 4);
        lv_obj_add_event_cb(b, preset_edit_cb, LV_EVENT_CLICKED, (void *)p->token);
        b = icon_btn(row, &ui_icon_ptz_delete, ROW_H - 4, ROW_H - 4);
        lv_obj_add_event_cb(b, preset_delete_cb, LV_EVENT_CLICKED, (void *)p->token);
    }
}

static void tour_cb(lv_event_t *e)
{
    int start = (int)(intptr_t)lv_event_get_user_data(e);
    stop_all();
    refused(ui_model()->ptz_tour(P.ch, (int)lv_dropdown_get_selected(P.tour_dd) + 1, start));
}

/* ---- the panel ------------------------------------------------------------------------------ */

static lv_obj_t *hbox(lv_obj_t *parent, int h)
{
    lv_obj_t *r = lv_obj_create(parent);
    ui_plain(r);
    ui_ring_room(r);
    lv_obj_set_size(r, LV_PCT(100), h);
    lv_obj_set_layout(r, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, PAD_GAP, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

/* Zoom, focus or iris: its icon, - and +. */
static void lens_row(lv_obj_t *parent, int ax, const lv_image_dsc_t *img, const char *what)
{
    lv_obj_t *r = hbox(parent, LENS_BTN);
    lv_obj_set_width(r, LV_SIZE_CONTENT);
    lv_obj_t *i = ui_icon(r, img, VG_COLOR_TEXT2);
    lv_obj_set_style_margin_right(i, VG_SPACE_XS, 0);
    (void)what;
    for (int k = 0; k < 2; ++k) {
        lv_obj_t *b = icon_btn(r, k ? &ui_icon_ptz_plus : &ui_icon_ptz_minus, LENS_BTN, LENS_BTN);
        lv_obj_add_event_cb(b, lens_btn_cb, LV_EVENT_PRESSED, (void *)(intptr_t)(ax * 2 + k));
        lv_obj_add_event_cb(b, lens_btn_cb, LV_EVENT_RELEASED, (void *)(intptr_t)(ax * 2 + k));
        lv_obj_add_event_cb(b, lens_btn_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)(ax * 2 + k));
    }
}

/* The controls for what the camera can do (P.info), anew. */
static void body_build(void)
{
    lv_obj_clean(P.body);
    /* the group: the close button stays first */
    while (lv_group_get_obj_count(P.group) > 1) lv_group_remove_obj(lv_group_get_obj_by_index(P.group, 1));
    P.speed = P.list = P.tour_dd = P.home_btn = NULL;
    P.built_caps = P.info.caps;
    P.built_state = P.info.state;
    P.built_tours = P.info.tours;
    if (P.info.state != UI_PTZ_READY) return;

    lv_obj_t *top = hbox(P.body, 3 * VG_SIZE_PTZ_PAD + 2 * PAD_GAP);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    if (has(UI_PTZ_CAP_PAN_TILT) || has(UI_PTZ_CAP_HOME)) {
        lv_obj_t *pad = lv_obj_create(top);
        ui_plain(pad);
        ui_ring_room(pad);
        lv_obj_set_size(pad, 3 * VG_SIZE_PTZ_PAD + 2 * PAD_GAP, 3 * VG_SIZE_PTZ_PAD + 2 * PAD_GAP);
        lv_obj_remove_flag(pad, LV_OBJ_FLAG_CLICKABLE);
        for (int i = 0; i < 9; ++i) {
            if (i != 4 && !has(UI_PTZ_CAP_PAN_TILT)) continue;
            lv_obj_t *b = icon_btn(pad, i == 4 && !has(UI_PTZ_CAP_HOME) ? &ui_icon_ptz_stop : k_pad[i].img,
                                   VG_SIZE_PTZ_PAD, VG_SIZE_PTZ_PAD);
            lv_obj_set_pos(b, (i % 3) * (VG_SIZE_PTZ_PAD + PAD_GAP), (i / 3) * (VG_SIZE_PTZ_PAD + PAD_GAP));
            if (i == 4) {
                lv_obj_add_event_cb(b, pad_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                P.home_btn = b;
            } else {
                lv_obj_add_event_cb(b, pad_btn_cb, LV_EVENT_PRESSED, (void *)(intptr_t)i);
                lv_obj_add_event_cb(b, pad_btn_cb, LV_EVENT_RELEASED, (void *)(intptr_t)i);
                lv_obj_add_event_cb(b, pad_btn_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)i);
            }
        }
    }
    lv_obj_t *lens = lv_obj_create(top);
    ui_plain(lens);
    ui_ring_room(lens);
    lv_obj_set_size(lens, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_layout(lens, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(lens, LV_FLEX_FLOW_COLUMN);
    int with_pad = has(UI_PTZ_CAP_PAN_TILT) || has(UI_PTZ_CAP_HOME);
    lv_obj_set_style_pad_row(lens, with_pad ? (3 * VG_SIZE_PTZ_PAD + 2 * PAD_GAP - 3 * LENS_BTN) / 2 : PAD_GAP, 0);
    if (!with_pad) lv_obj_set_height(top, LV_SIZE_CONTENT);
    lv_obj_remove_flag(lens, LV_OBJ_FLAG_CLICKABLE);
    if (has(UI_PTZ_CAP_ZOOM)) lens_row(lens, 2, &ui_icon_ptz_zoom, "Zoom");
    if (has(UI_PTZ_CAP_FOCUS)) lens_row(lens, 3, &ui_icon_ptz_focus, "Focus");
    if (has(UI_PTZ_CAP_IRIS)) lens_row(lens, 4, &ui_icon_ptz_iris, "Iris");

    /* the speed, and the camera's own focus */
    lv_obj_t *r = hbox(P.body, VG_SIZE_BUTTON);
    ui_label(r, "Speed", VG_FONT_SM_LV, UI_C(TEXT2));
    P.speed = lv_slider_create(r);
    lv_slider_set_range(P.speed, 1, 8);
    lv_slider_set_value(P.speed, speed_now(), LV_ANIM_OFF);
    lv_obj_set_width(P.speed, has(UI_PTZ_CAP_FOCUS_AUTO) ? 136 : 192);
    lv_obj_set_style_margin_hor(P.speed, VG_SPACE_MD, 0);
    ui_focus_style(P.speed);
    lv_obj_add_event_cb(P.speed, speed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_group_add_obj(P.group, P.speed);
    P.speed_val = ui_label(r, "", VG_FONT_MD_LV, UI_C(TEXT));
    lv_obj_set_style_min_width(P.speed_val, 12, 0);
    speed_cb(NULL);
    if (has(UI_PTZ_CAP_FOCUS_AUTO)) {
        lv_obj_t *af = ui_btn(r, NULL, "AF");
        lv_obj_add_event_cb(af, af_cb, LV_EVENT_CLICKED, NULL);
        lv_group_add_obj(P.group, af);
    }

    if (has(UI_PTZ_CAP_PRESETS)) {
        lv_obj_t *h = hbox(P.body, VG_SIZE_BUTTON);
        lv_obj_set_flex_align(h, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        ui_label(h, "Presets", VG_FONT_MD_LV, UI_C(TEXT));
        lv_obj_t *add = icon_btn(h, &ui_icon_ptz_plus, VG_SIZE_BUTTON, VG_SIZE_BUTTON - 4);
        lv_obj_add_event_cb(add, preset_new_cb, LV_EVENT_CLICKED, NULL);
        P.list = lv_obj_create(P.body);
        ui_plain(P.list);
        ui_ring_room(P.list);
        lv_obj_add_flag(P.list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(P.list, LV_DIR_VER);
        ui_scrollbar(P.list);
        lv_obj_set_width(P.list, LV_PCT(100));
        lv_obj_set_flex_grow(P.list, 1);
        lv_obj_set_layout(P.list, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(P.list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(P.list, 2, 0);
        presets_build();
    }

    if (has(UI_PTZ_CAP_TOURS) && P.info.tours > 0) {
        lv_obj_t *t = hbox(P.body, VG_SIZE_BUTTON);
        ui_label(t, "Tour", VG_FONT_SM_LV, UI_C(TEXT2));
        P.tour_dd = lv_dropdown_create(t);
        char opts[64] = "";
        for (int i = 1; i <= P.info.tours && i <= 8; ++i)
            snprintf(opts + strlen(opts), sizeof opts - strlen(opts), "%s%d", i > 1 ? "\n" : "", i);
        lv_dropdown_set_options(P.tour_dd, opts);
        lv_obj_set_width(P.tour_dd, 72);
        lv_obj_set_flex_grow(P.tour_dd, 1);
        ui_focus_style(P.tour_dd);
        lv_group_add_obj(P.group, P.tour_dd);
        lv_obj_t *b = icon_btn(t, &ui_icon_ptz_play, VG_SIZE_BUTTON, VG_SIZE_BUTTON - 4);
        lv_obj_add_event_cb(b, tour_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
        b = icon_btn(t, &ui_icon_ptz_stop, VG_SIZE_BUTTON, VG_SIZE_BUTTON - 4);
        lv_obj_add_event_cb(b, tour_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
    }
    lv_obj_t *hint = ui_label(P.body, has(UI_PTZ_CAP_PAN_TILT) ? "On the picture: hold to move, wheel to zoom."
                                                               : "On the picture: the wheel zooms.",
                              VG_FONT_SM_LV, UI_C(TEXT2));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, LV_PCT(100));
    if (P.home_btn) lv_group_focus_obj(P.home_btn);
}

static void presets_key(char *out, size_t cap)
{
    out[0] = 0;
    for (int i = 0; i < P.info.n_presets; ++i)
        snprintf(out + strlen(out), cap - strlen(out), "%s\x1f%s\x1e", P.info.presets[i].token,
                 P.info.presets[i].name);
}

static void status_update(void)
{
    char t[160];
    lv_color_t c = UI_C(TEXT2);
    switch (P.info.state) {
    case UI_PTZ_OFF:     snprintf(t, sizeof t, "No PTZ for this camera"); break;
    case UI_PTZ_PROBING: snprintf(t, sizeof t, "Asking the camera what it can do..."); c = UI_C(WARN); break;
    case UI_PTZ_READY:
        snprintf(t, sizeof t, "%s%s", P.info.backend[0] ? (strcmp(P.info.backend, "netip") ? "ONVIF" : "NETIP") : "",
                 P.info.moving ? ", moving" : "");
        break;
    case UI_PTZ_NONE:    snprintf(t, sizeof t, "This camera has no PTZ%s%s", P.info.error[0] ? ": " : "", P.info.error);
                         c = UI_C(WARN); break;
    default:             snprintf(t, sizeof t, "Cannot reach its PTZ: %s", P.info.error); c = UI_C(BAD); break;
    }
    lv_label_set_text(P.status, t);
    lv_obj_set_style_text_color(P.status, c, 0);
}

/* Every VG_DURATION_PTZ_REPEAT ms: the camera's state, the controls when
 * it changed, and the move held kept going (or ended: the wheel). */
static void tick_cb(lv_timer_t *t)
{
    (void)t;
    if (!P.open) return;
    const ui_chan_t *st = ui_channel(P.ch);
    if (st->status == UI_CH_EMPTY) {
        ui_ptz_close();
        return;
    }
    /* Not ours any more: another screen over the live view. */
    if (ui_page_top()) {
        stop_all();
        return;
    }
    ui_model()->ptz_info(P.ch, &P.info);
    if (P.info.error_seq != P.err_seen) {
        if (P.info.state == UI_PTZ_READY && P.info.error[0]) ui_toast(UI_TOAST_BAD, "PTZ: %s", P.info.error);
        P.err_seen = P.info.error_seq;
    }
    status_update();
    if (P.info.state != P.built_state || P.info.caps != P.built_caps || P.info.tours != P.built_tours) {
        body_build();
        presets_key(P.built_presets, sizeof P.built_presets);
    } else if (P.list) {
        char k[sizeof P.built_presets];
        presets_key(k, sizeof k);
        if (strcmp(k, P.built_presets)) {
            snprintf(P.built_presets, sizeof P.built_presets, "%s", k);
            presets_build();
        }
    }
    /* The wheel's notches run out; a key is kept going by its own
     * repeats (a keyboard unplugged with it down never says it is up);
     * a button or the picture by this, until LVGL says released. */
    uint32_t now = lv_tick_get();
    if (P.hold == HOLD_WHEEL && (int32_t)(now - P.wheel_until) >= 0) stop_all();
    else if (P.hold == HOLD_KEY && lv_tick_elaps(P.key_ms) >= VG_DURATION_PTZ_HOLD) stop_all();
    else if (P.hold != HOLD_NONE && lv_tick_elaps(P.hold_ms) >= HOLD_LIMIT) stop_all();
    else if (P.hold != HOLD_NONE && P.hold != HOLD_KEY) send_v();
}

int ui_ptz_is_open(void) { return P.open; }
int ui_ptz_channel(void) { return P.open ? P.ch : -1; }
lv_group_t *ui_ptz_group(void) { return P.open ? P.group : NULL; }

void ui_ptz_open(lv_obj_t *scr, int ch, void (*on_close)(void))
{
    if (P.open) ui_ptz_close();
    if (!ui_model()->ptz_info) {
        ui_not_yet("PTZ");
        return;
    }
    memset(&P, 0, sizeof P);
    P.open = 1;
    P.ch = ch;
    P.on_close = on_close;
    P.sw = lv_obj_get_width(scr);
    P.sh = lv_obj_get_height(scr);
    P.panel_x = P.sw - VG_SIZE_PTZ_PANEL;
    P.group = lv_group_create();
    P.built_state = -1;

    P.root = lv_obj_create(scr);
    ui_plain(P.root);
    lv_obj_set_size(P.root, P.sw, P.sh);
    lv_obj_add_flag(P.root, LV_OBJ_FLAG_CLICKABLE);     /* nothing reaches the tiles under it */
    lv_obj_remove_flag(P.root, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    P.pad = lv_obj_create(P.root);
    ui_plain(P.pad);
    lv_obj_set_size(P.pad, P.panel_x, P.sh);
    lv_obj_add_flag(P.pad, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(P.pad, LV_OBJ_FLAG_PRESS_LOCK);  /* the pointer onto the panel: a release */
    lv_obj_remove_flag(P.pad, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_add_event_cb(P.pad, pad_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(P.pad, pad_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(P.pad, pad_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(P.pad, pad_cb, LV_EVENT_PRESS_LOST, NULL);

    /* What moves, on a chip at the top of the picture. */
    P.chip = lv_obj_create(P.root);
    ui_plain(P.chip);
    lv_obj_add_style(P.chip, &ui_st_chip, 0);
    lv_obj_set_style_radius(P.chip, 0, 0);
    lv_obj_set_style_pad_hor(P.chip, VG_SPACE_MD, 0);
    lv_obj_set_style_pad_ver(P.chip, VG_SPACE_XS, 0);
    lv_obj_set_size(P.chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    ui_inert(P.chip);
    P.chip_label = ui_label(P.chip, "", VG_FONT_MD_LV, UI_C(TEXT));
    lv_obj_align(P.chip, LV_ALIGN_TOP_MID, (P.panel_x - P.sw) / 2, VG_SPACE_LG);
    lv_obj_add_flag(P.chip, LV_OBJ_FLAG_HIDDEN);

    P.panel = lv_obj_create(P.root);
    ui_plain(P.panel);
    lv_obj_add_style(P.panel, &ui_st_panel, 0);
    lv_obj_set_style_radius(P.panel, 0, 0);             /* over the video: no soft corner */
    lv_obj_set_style_border_side(P.panel, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_pad_all(P.panel, VG_SPACE_LG, 0);
    lv_obj_set_style_pad_row(P.panel, VG_SPACE_SM, 0);
    /* The right edge, top to bottom: over the clock's corner too. */
    lv_obj_set_pos(P.panel, P.panel_x, 0);
    lv_obj_set_size(P.panel, VG_SIZE_PTZ_PANEL, P.sh);
    lv_obj_set_layout(P.panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(P.panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(P.panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(P.panel, LV_OBJ_FLAG_CLICK_FOCUSABLE);

    lv_obj_t *h = hbox(P.panel, VG_SIZE_BUTTON);
    lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    ui_label(h, "PTZ", VG_FONT_LG_LV, UI_C(ACCENT));
    char name[64];
    ui_chan_title(ch, name, sizeof name);
    P.title = ui_label(h, name, VG_FONT_MD_LV, UI_C(TEXT));
    lv_obj_set_style_margin_left(P.title, VG_SPACE_SM, 0);
    lv_label_set_long_mode(P.title, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(P.title, 1);
    lv_obj_t *x = icon_btn(h, &ui_icon_ptz_close, VG_SIZE_BUTTON, VG_SIZE_BUTTON - 4);
    lv_obj_add_event_cb(x, close_cb, LV_EVENT_CLICKED, NULL);
    P.status = ui_label(P.panel, "", VG_FONT_SM_LV, UI_C(TEXT2));
    lv_label_set_long_mode(P.status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(P.status, LV_PCT(100));

    P.body = lv_obj_create(P.panel);
    ui_plain(P.body);
    ui_ring_room(P.body);
    lv_obj_set_width(P.body, LV_PCT(100));
    lv_obj_set_flex_grow(P.body, 1);
    lv_obj_set_layout(P.body, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(P.body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(P.body, VG_SPACE_SM, 0);
    lv_obj_remove_flag(P.body, LV_OBJ_FLAG_CLICKABLE);

    ui_model()->ptz_info(ch, &P.info);
    P.err_seen = P.info.error_seq;
    status_update();
    body_build();
    presets_key(P.built_presets, sizeof P.built_presets);
    P.timer = lv_timer_create(tick_cb, VG_DURATION_PTZ_REPEAT, NULL);
    ui_app_focus_changed();
}

void ui_ptz_close(void)
{
    if (!P.open) return;
    stop_all();
    if (ui_popup_is_open()) ui_popup_close();
    P.open = 0;
    if (P.timer) lv_timer_delete(P.timer);
    P.timer = NULL;
    lv_group_t *g = P.group;
    P.group = NULL;
    if (P.root) lv_obj_delete(P.root);
    P.root = P.chip = P.panel = P.body = P.list = P.tour_dd = P.home_btn = P.speed = NULL;
    if (g) lv_group_delete(g);
    ui_app_focus_changed();
    void (*cb)(void) = P.on_close;
    P.on_close = NULL;
    if (cb) cb();
}

/* ---- keys ------------------------------------------------------------------------------------ */

/* The keys held, as a move (arrows together: a diagonal). */
static void keys_apply(void)
{
    double s = speed_now() / 8.0;
    unsigned k = P.keys;
    double pan = ((k & K_RIGHT) ? 1 : 0) - ((k & K_LEFT) ? 1 : 0);
    double tilt = ((k & K_UP) ? 1 : 0) - ((k & K_DOWN) ? 1 : 0);
    double zoom = ((k & K_IN) ? 1 : 0) - ((k & K_OUT) ? 1 : 0);
    double focus = ((k & K_FAR) ? 1 : 0) - ((k & K_NEAR) ? 1 : 0);
    if (!has(UI_PTZ_CAP_PAN_TILT)) pan = tilt = 0;
    if (!has(UI_PTZ_CAP_ZOOM)) zoom = 0;
    if (!has(UI_PTZ_CAP_FOCUS)) focus = 0;
    if (!pan && !tilt && !zoom && !focus) {
        if (P.hold == HOLD_KEY) stop_all();
        return;
    }
    if (pan || tilt) zoom = focus = 0;
    else if (zoom) focus = 0;
    hold_start(HOLD_KEY, pan * s, tilt * s, zoom * s, focus, 0);
}

static unsigned key_bit(uint32_t key)
{
    switch (key) {
    case LV_KEY_UP:    return K_UP;
    case LV_KEY_DOWN:  return K_DOWN;
    case LV_KEY_LEFT:  return K_LEFT;
    case LV_KEY_RIGHT: return K_RIGHT;
    case '+': case '=': return K_IN;
    case '-': case '_': return K_OUT;
    case UI_KEY_PGUP:  return K_FAR;
    case UI_KEY_PGDN:  return K_NEAR;
    }
    return 0;
}

int ui_ptz_key(uint32_t key, unsigned mods)
{
    (void)mods;
    if (!P.open) return 0;
    if (key == LV_KEY_ESC) {
        ui_ptz_close();
        return 1;
    }
    /* A focused slider or list takes the arrows. */
    lv_obj_t *foc = P.group ? lv_group_get_focused(P.group) : NULL;
    int arrows_to_it = foc && (lv_obj_check_type(foc, &lv_slider_class) || lv_obj_check_type(foc, &lv_dropdown_class));
    unsigned bit = key_bit(key);
    if (bit && !(arrows_to_it && (bit & (K_UP | K_DOWN | K_LEFT | K_RIGHT)))) {
        if (P.hold != HOLD_KEY && P.hold != HOLD_NONE) return 1;     /* the mouse holds a move */
        if (P.info.state != UI_PTZ_READY) return 1;
        int repeat = (P.keys & bit) != 0;
        P.keys |= bit;
        P.key_ms = lv_tick_get();
        if (repeat && P.hold == HOLD_KEY) send_v();     /* the kernel's repeat: keep it going */
        else keys_apply();
        return 1;
    }
    if (key == LV_KEY_HOME) {
        stop_all();
        if (has(UI_PTZ_CAP_HOME)) refused(ui_model()->ptz_home(P.ch));
        return 1;
    }
    if (key >= '0' && key <= '9' && has(UI_PTZ_CAP_PRESETS)) {
        /* The preset numbered so; else the one at that place in the list. */
        int n = key == '0' ? 10 : (int)(key - '0');
        char t[8];
        snprintf(t, sizeof t, "%d", n);
        if (preset_of(t)) goto_token(t);
        else if (n <= P.info.n_presets) goto_token(P.info.presets[n - 1].token);
        else ui_toast(UI_TOAST_WARN, "PTZ: no preset %d", n);
        return 1;
    }
    return 0;
}

void ui_ptz_key_up(uint32_t key)
{
    unsigned bit = key_bit(key);
    if (!P.open || !bit || !(P.keys & bit)) return;
    P.keys &= ~bit;
    keys_apply();
}
