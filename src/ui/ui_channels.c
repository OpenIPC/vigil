/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_channels.c — Channels: finding cameras on the network and putting
 * them on channels, in the two panes of an NVR's camera page.
 *
 *   Search    protocol, Search, the devices found (check one or more):
 *             Add, Change IP..., Mass change..., Auto add all; the user
 *             and password Add uses
 *   Channels  every channel: name, source (password hidden), protocol,
 *             state, enabled; Edit, Clear, "+ Add" on an empty one, and
 *             with one device checked, Add/Replace puts it there
 *
 * A device row's "Add to..." opens a small menu of the empty channels.
 * Change IP, Mass change and Auto add are pages of their own with the
 * credentials and the result of the job. All network work is a discovery
 * job of the model's (ui_model.h): this page starts one and follows it on
 * a timer; the screen never waits.
 *
 * Keyboard: Tab goes Back, the search row, the credentials, the device
 * table, the channel table, the header's actions. In the device table the
 * arrows move and Enter or Space checks a device; in the channel table
 * Enter edits (or adds), Delete clears, A or + puts the checked device on
 * the channel.
 */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "ui_app.h"
#include "ui_osk.h"
#include "ui_theme.h"

#define POLL_MS     200
#define DEV_H       150             /* the device table's rows (four), scrolled */
#define QUEUE_MAX   UI_DISC_MAX

typedef struct {
    ui_page_t     *page;
    int            sec;             /* the cameras section */
    lv_obj_t      *kb;
    lv_obj_t      *proto, *search, *status, *user, *pass, *hint;
    lv_obj_t      *add, *chip, *mass, *autoadd, *all;
    lv_obj_t      *devs, *chans;
    lv_obj_t      *state_cell[UI_MAX_CHANNELS];
    lv_timer_t    *timer;
    ui_disc_dev_t  dev[UI_DISC_MAX];
    int            n_dev;
    char           checked[UI_DISC_MAX][24];
    int            n_checked;
    unsigned       job;             /* the job this page started and waits for */
    ui_disc_job_t *jb;
    /* Add with several devices checked: one assign job after another. */
    char           queue[QUEUE_MAX][24];
    int            q_n, q_i, q_ok, q_fail;
    char           q_err[96];
    int            pick_ch;         /* "Add to..." menu: the device's index */
    unsigned       gen;
} chans_t;

static chans_t *g_ch;               /* the open page, for the model's job timer */

static const ui_model_t *M(void) { return ui_model(); }

static const char *proto_word(unsigned p)
{
    return p == UI_DISC_NETIP ? "NETIP" : p == UI_DISC_ONVIF ? "ONVIF" : p == UI_DISC_DHIP ? "DHIP" : "-";
}

static void protos_text(unsigned bits, char *out, size_t cap)
{
    out[0] = 0;
    for (unsigned b = UI_DISC_NETIP; b <= UI_DISC_DHIP; b <<= 1)
        if (bits & b) {
            size_t n = strlen(out);
            snprintf(out + n, cap - n, "%s%s", n ? ", " : "", proto_word(b));
        }
}

/* ---- small widgets ------------------------------------------------------------------- */

static lv_obj_t *row_box(lv_obj_t *parent, int gap)
{
    lv_obj_t *r = lv_obj_create(parent);
    ui_plain(r);
    ui_ring_room(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(r, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    return r;
}

static lv_obj_t *pane(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);
    ui_plain(p);
    lv_obj_add_style(p, &ui_st_panel, 0);
    lv_obj_set_size(p, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(p, VG_SPACE_MD, 0);
    lv_obj_set_style_pad_row(p, VG_SPACE_SM, 0);
    lv_obj_set_layout(p, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    return p;
}

static void pane_title(lv_obj_t *row, const lv_image_dsc_t *icon, const char *title)
{
    ui_icon(row, icon, VG_COLOR_ACCENT);
    ui_label(row, title, VG_FONT_LG_LV, UI_C(TEXT));
}

/* A table: rows of cells, the header first, the rows scrolled inside. */
static lv_obj_t *table_rows(lv_obj_t *parent, int h)
{
    lv_obj_t *t = lv_obj_create(parent);
    ui_plain(t);
    lv_obj_set_width(t, LV_PCT(100));
    if (h > 0) lv_obj_set_height(t, h);
    else lv_obj_set_flex_grow(t, 1);
    lv_obj_add_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(t, LV_DIR_VER);
    lv_obj_set_layout(t, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(t, VG_SPACE_XS, 0);     /* room for the focus ring */
    return t;
}

static lv_obj_t *trow(lv_obj_t *table, int header)
{
    lv_obj_t *r = lv_obj_create(table);
    ui_plain(r);
    lv_obj_set_size(r, LV_PCT(100), header ? LV_SIZE_CONTENT : 36);
    lv_obj_set_style_pad_ver(r, header ? VG_SPACE_XS : 0, 0);
    lv_obj_set_style_pad_hor(r, VG_SPACE_SM, 0);
    lv_obj_set_style_pad_column(r, VG_SPACE_MD, 0);
    lv_obj_set_style_border_color(r, UI_C(LINE), 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_layout(r, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (header) ui_inert(r);
    else {
        lv_obj_add_style(r, &ui_st_focus_in, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(r, UI_C(HOVER), LV_STATE_HOVERED);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_HOVERED);
    }
    return r;
}

/* A cell `w` wide (0: takes the rest). */
static lv_obj_t *tcell(lv_obj_t *row, const char *text, int w, lv_color_t c)
{
    lv_obj_t *l = ui_label(row, text, VG_FONT_SM_LV, c);
    /* One line, cut with "...": a label wraps unless its height is set. */
    lv_obj_set_height(l, lv_font_get_line_height(VG_FONT_SM_LV));
    if (w) lv_obj_set_width(l, w);
    else {
        lv_obj_set_width(l, 1);
        lv_obj_set_flex_grow(l, 1);
    }
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

/* A small button in a table row. */
static lv_obj_t *tbtn(lv_obj_t *row, const char *sym, const char *text, lv_event_cb_t cb, void *u, int v)
{
    lv_obj_t *b = ui_btn(row, sym, text);
    lv_obj_set_height(b, 30);
    lv_obj_set_style_pad_hor(b, VG_SPACE_SM, 0);
    lv_obj_set_style_text_font(b, VG_FONT_SM_LV, 0);
    lv_obj_set_user_data(b, (void *)(intptr_t)v);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, u);
    return b;
}

/* The check box of a device row: a bordered square with a tick. */
static lv_obj_t *tick(lv_obj_t *row, int on)
{
    lv_obj_t *b = lv_obj_create(row);
    ui_plain(b);
    ui_inert(b);
    lv_obj_set_size(b, 20, 20);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, on ? UI_C(ACCENT) : UI_C(TEXT2), 0);
    lv_obj_set_style_radius(b, VG_RADIUS_SM, 0);
    lv_obj_set_style_bg_color(b, UI_C(ACCENT), 0);
    lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    if (on) {
        lv_obj_t *l = ui_label(b, LV_SYMBOL_OK, VG_FONT_SM_LV, UI_C(TEXT));
        lv_obj_center(l);
    }
    return b;
}

/* ---- text fields and the on-screen keyboard -------------------------------------------- */

static void ta_click_cb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_user_data(e);
    if (!ui_keyboard_in_use() && kb) ui_osk_show(kb, lv_event_get_target(e), 0);
}

static void kb_done_cb(lv_event_t *e)
{
    ui_osk_hide(lv_event_get_target(e));
}

static lv_obj_t *osk_for(ui_page_t *p)
{
    lv_obj_t *kb = ui_osk_create(p->scr);
    if (kb) {
        lv_obj_add_event_cb(kb, kb_done_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(kb, kb_done_cb, LV_EVENT_CANCEL, NULL);
    }
    return kb;
}

static lv_obj_t *field(lv_obj_t *parent, ui_page_t *p, lv_obj_t *kb, const char *text, int w, int secret,
                       const char *accept, int max)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, 1);
    if (accept) lv_textarea_set_accepted_chars(ta, accept);
    if (max) lv_textarea_set_max_length(ta, (uint32_t)max);
    lv_textarea_set_text(ta, text);
    if (secret) lv_textarea_set_password_mode(ta, 1);
    lv_obj_set_width(ta, w);
    lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, kb);
    ui_focus_style(ta);
    lv_group_add_obj(p->group, ta);
    return ta;
}

/* A label and a field in a form row. */
static lv_obj_t *labelled(lv_obj_t *parent, ui_page_t *p, lv_obj_t *kb, const char *label, const char *text,
                          int secret, const char *accept, int max)
{
    lv_obj_t *r = row_box(parent, VG_SPACE_LG);
    lv_obj_t *l = ui_label(r, label, VG_FONT_MD_LV, UI_C(TEXT));
    lv_obj_set_width(l, 220);
    return field(r, p, kb, text, 320, secret, accept, max);
}

static int osk_back(lv_obj_t *kb)
{
    if (kb && ui_osk_shown(kb)) {
        ui_osk_hide(kb);
        return 1;
    }
    return 0;
}

#define IP_CHARS "0123456789."

/* ---- jobs ------------------------------------------------------------------------------ */

/* Start and remember a job; -1 with a toast. */
static int started(chans_t *c, int id, const char *err)
{
    if (id < 0) {
        ui_toast(UI_TOAST_BAD, "%s", err[0] ? err : "Cannot start");
        return -1;
    }
    c->job = (unsigned)id;
    return 0;
}

static int device_index(const chans_t *c, const char *key)
{
    for (int i = 0; i < c->n_dev; ++i)
        if (!strcmp(c->dev[i].key, key)) return i;
    return -1;
}

static int is_checked(const chans_t *c, const char *key)
{
    for (int i = 0; i < c->n_checked; ++i)
        if (!strcmp(c->checked[i], key)) return 1;
    return 0;
}

static void devs_build(chans_t *c);
static void chans_build(chans_t *c);
static void buttons_sync(chans_t *c);

/* ---- the device table ----------------------------------------------------------------- */

static void check_toggle(chans_t *c, int i)
{
    if (i < 0 || i >= c->n_dev) return;
    const char *key = c->dev[i].key;
    for (int k = 0; k < c->n_checked; ++k)
        if (!strcmp(c->checked[k], key)) {
            memmove(c->checked[k], c->checked[k + 1], sizeof c->checked[0] * (size_t)(c->n_checked - k - 1));
            --c->n_checked;
            devs_build(c);
            chans_build(c);
            return;
        }
    if (c->n_checked < UI_DISC_MAX) snprintf(c->checked[c->n_checked++], sizeof c->checked[0], "%s", key);
    devs_build(c);
    chans_build(c);
}

/* After the click's event: the table it came from is built anew. */
static void toggle_async(void *u)
{
    chans_t *c = g_ch;
    if (c) check_toggle(c, (int)(intptr_t)u);
}

static void dev_click_cb(lv_event_t *e)
{
    lv_async_call(toggle_async, lv_obj_get_user_data(lv_event_get_current_target(e)));
}

static void dev_key_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) != ' ') return;
    lv_async_call(toggle_async, lv_obj_get_user_data(lv_event_get_target(e)));
}

static void assign_start(chans_t *c, const char *key, int ch, int replace);

static void pick_cb(lv_event_t *e)
{
    chans_t *c = lv_event_get_user_data(e);
    int ch = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    ui_popup_close();
    if (c->pick_ch >= 0 && c->pick_ch < c->n_dev) assign_start(c, c->dev[c->pick_ch].key, ch, 0);
}

/* "Add to...": the empty channels, in a small menu by the button. */
static void add_to_cb(lv_event_t *e)
{
    chans_t *c = lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    c->pick_ch = (int)(intptr_t)lv_obj_get_user_data(btn);
    lv_area_t a;
    lv_obj_get_coords(btn, &a);
    lv_point_t at = { a.x1, a.y2 };
    lv_obj_t *panel = ui_popup_open(&at, 220);
    lv_obj_set_style_max_height(panel, 360, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    int n = M()->item_count(c->sec), shown = 0;
    lv_obj_t *first = NULL;
    char buf[1024], t[32];
    for (int ch = 0; ch < M()->channels && ch < UI_MAX_CHANNELS; ++ch) {
        if (ch < n) {
            M()->get_value(c->sec, ch, 0, buf, sizeof buf);
            if (buf[0]) continue;
        }
        snprintf(t, sizeof t, "Add to channel %d", ch + 1);
        lv_obj_t *b = ui_btn(panel, LV_SYMBOL_PLUS, t);
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_set_user_data(b, (void *)(intptr_t)(ch + 1));
        lv_obj_add_event_cb(b, pick_cb, LV_EVENT_CLICKED, c);
        if (!first) first = b;
        ++shown;
    }
    if (!shown) ui_label(panel, "No empty channel", VG_FONT_MD_LV, UI_C(TEXT2));
    ui_popup_focus(panel, first, LV_GRIDNAV_CTRL_NONE);
    ui_popup_place(&at);
}

static void devs_build(chans_t *c)
{
    lv_obj_t *t = c->devs;
    int32_t sy = lv_obj_get_scroll_y(t);
    lv_obj_clean(t);
    if (!c->n_dev) {
        lv_obj_t *e = ui_label(t, M()->disc_scan ? "No devices yet: Search finds the cameras on the network."
                                                 : "This vigild cannot search for cameras.",
                               VG_FONT_MD_LV, UI_C(TEXT2));
        lv_obj_set_style_pad_ver(e, VG_SPACE_LG, 0);
    }
    lv_obj_t *first = NULL;
    for (int i = 0; i < c->n_dev; ++i) {
        const ui_disc_dev_t *d = &c->dev[i];
        lv_obj_t *r = trow(t, 0);
        lv_obj_set_user_data(r, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r, dev_click_cb, LV_EVENT_CLICKED, c);
        lv_obj_add_event_cb(r, dev_key_cb, LV_EVENT_KEY, c);
        if (!first) first = r;
        int on = is_checked(c, d->key);
        tick(r, on);
        char port[16], vm[160], pr[48], st[32];
        snprintf(port, sizeof port, "%d", d->port);
        const char *who = d->model[0] ? d->model : d->name;
        snprintf(vm, sizeof vm, "%s%s%s", d->vendor, d->vendor[0] && who[0] ? " / " : "", who);
        protos_text(d->protos, pr, sizeof pr);
        if (d->channel) snprintf(st, sizeof st, "On channel %d", d->channel);
        else snprintf(st, sizeof st, "New");
        tcell(r, d->ip, 130, UI_C(TEXT));
        tcell(r, d->port ? port : "-", 56, UI_C(TEXT));
        tcell(r, d->mac[0] ? d->mac : "-", 150, UI_C(TEXT));
        tcell(r, vm[0] ? vm : "-", 0, UI_C(TEXT));
        tcell(r, pr, 150, UI_C(TEXT));
        tcell(r, st, 140, d->channel ? UI_C(OK) : UI_C(ACCENT));
        lv_obj_t *b = tbtn(r, LV_SYMBOL_PLUS, "Add to...", add_to_cb, c, i);
        lv_obj_set_width(b, 96);
        if (d->channel) lv_obj_add_state(b, LV_STATE_DISABLED);
    }
    if (first) ui_gridnav_focus(t, first);
    lv_obj_scroll_to_y(t, sy, LV_ANIM_OFF);
}

typedef struct {
    const char *t;
    int         w;
} col_t;

static const col_t k_dev_cols[] = {
    { "", 20 }, { "IP address", 130 }, { "Port", 56 }, { "MAC", 150 }, { "Vendor / model", 0 },
    { "Protocols", 150 }, { "Status", 140 }, { "", 96 },
};
static const col_t k_ch_cols[] = {
    { "#", 28 }, { "Name", 150 }, { "Source", 0 }, { "Protocol", 70 }, { "State", 150 }, { "Enabled", 62 },
    { "", 292 },
};

/* A table's header, above its scrolled rows (so it stays). */
static void header(lv_obj_t *parent, const col_t *cols, size_t n)
{
    lv_obj_t *h = trow(parent, 1);
    lv_obj_set_style_pad_hor(h, VG_SPACE_SM + VG_SPACE_XS, 0);  /* the rows' own and their container's */
    for (size_t i = 0; i < n; ++i) tcell(h, cols[i].t, cols[i].w, UI_C(TEXT2));
}

/* ---- the channel table ---------------------------------------------------------------- */

static int field_of(int sec, const char *key)
{
    const ui_section_t *s = &M()->sections[sec];
    for (int f = 0; f < s->n_fields; ++f)
        if (!strcmp(s->fields[f].key, key)) return f;
    return -1;
}

static void slot_value(chans_t *c, int ch, const char *key, char *buf, size_t cap)
{
    int f = field_of(c->sec, key);
    buf[0] = 0;
    if (f >= 0 && ch < M()->item_count(c->sec)) M()->get_value(c->sec, ch, f, buf, cap);
}

static int slot_empty(chans_t *c, int ch)
{
    char buf[1024];
    slot_value(c, ch, "url", buf, sizeof buf);
    return !buf[0];
}

/* The one device checked, or -1. */
static int one_checked(const chans_t *c)
{
    return c->n_checked == 1 ? device_index(c, c->checked[0]) : -1;
}

static void edit_cb(lv_event_t *e)
{
    chans_t *c = lv_event_get_user_data(e);
    ui_form_open_slot(c->sec, (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
}

static void put_here(chans_t *c, int ch)
{
    int i = one_checked(c);
    if (i < 0) {
        ui_toast(UI_TOAST_WARN, "Check one device above first");
        return;
    }
    assign_start(c, c->dev[i].key, ch + 1, !slot_empty(c, ch));
}

/* "+ Add" on an empty channel: the checked device, or the camera form. */
static void add_here_cb(lv_event_t *e)
{
    chans_t *c = lv_event_get_user_data(e);
    int ch = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (one_checked(c) >= 0) put_here(c, ch);
    else ui_form_open_slot(c->sec, ch);
}

static void replace_cb(lv_event_t *e)
{
    chans_t *c = lv_event_get_user_data(e);
    put_here(c, (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
}

typedef struct {
    chans_t *c;
    int      ch;
} clear_t;

static clear_t g_clear;

/* Clear channel `ch`: its slot emptied (the cameras after it stay). */
static void clear_now(void *u)
{
    clear_t *k = u;
    chans_t *c = k->c;
    const ui_section_t *s = &M()->sections[c->sec];
    int n = M()->item_count(c->sec);
    if (k->ch >= n || n > UI_MAX_ITEMS) return;
    char **v = calloc((size_t)(n * s->n_fields), sizeof *v);
    int origin[UI_MAX_ITEMS];
    char buf[1024];
    int ok = v != NULL;
    for (int i = 0; ok && i < n; ++i) {
        origin[i] = i == k->ch ? -1 : i;
        for (int f = 0; f < s->n_fields; ++f) {
            if (i == k->ch) snprintf(buf, sizeof buf, "%s", f == 0 || !s->fields[f].def ? "" : s->fields[f].def);
            else M()->get_value(c->sec, i, f, buf, sizeof buf);
            if (!(v[i * s->n_fields + f] = strdup(buf))) ok = 0;
        }
    }
    ui_errors_t errs;
    memset(&errs, 0, sizeof errs);
    ui_save_rc_t rc = ok ? M()->save(c->sec, n, (const char *const *)v, origin, c->gen, &errs) : UI_SAVE_FAILED;
    for (int i = 0; v && i < n * s->n_fields; ++i) free(v[i]);
    free(v);
    if (rc == UI_SAVE_OK) ui_toast(UI_TOAST_OK, "Channel %d cleared", k->ch + 1);
    else if (rc == UI_SAVE_STALE) ui_toast(UI_TOAST_WARN, "Not cleared: the channels changed meanwhile");
    else ui_toast(UI_TOAST_BAD, "Not cleared: %s", errs.n ? errs.e[0].msg : "error");
}

static void clear_ask(chans_t *c, int ch)
{
    if (slot_empty(c, ch)) return;
    g_clear = (clear_t){ c, ch };
    char q[96];
    snprintf(q, sizeof q, "Clear channel %d? Its camera is removed at once.", ch + 1);
    ui_confirm(q, "Clear", clear_now, &g_clear);
}

static void clear_cb(lv_event_t *e)
{
    clear_ask(lv_event_get_user_data(e), (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
}

static void chan_click_cb(lv_event_t *e)
{
    chans_t *c = lv_event_get_user_data(e);
    lv_indev_t *in = lv_indev_active();
    /* A pointer click on the row itself does nothing (the buttons do);
     * Enter edits, or adds on an empty channel. */
    if (!in || lv_indev_get_type(in) != LV_INDEV_TYPE_KEYPAD) return;
    int ch = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (slot_empty(c, ch) && one_checked(c) >= 0) put_here(c, ch);
    else ui_form_open_slot(c->sec, ch);
}

static void chan_key_cb(lv_event_t *e)
{
    chans_t *c = lv_event_get_user_data(e);
    int ch = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    uint32_t k = lv_event_get_key(e);
    if (k == LV_KEY_DEL) clear_ask(c, ch);
    else if (k == 'a' || k == '+') put_here(c, ch);
}

static const char *scheme_word(const char *url)
{
    if (!strncmp(url, "netip://", 8)) return "NETIP";
    if (!strncmp(url, "rtsp://", 7)) return "RTSP";
    if (!strncmp(url, "capture:", 8)) return "Local";
    return url[0] ? "?" : "-";
}

static lv_color_t state_color(const ui_chan_t *st)
{
    return st->status == UI_CH_LIVE ? UI_C(OK)
         : st->status == UI_CH_EMPTY || st->status == UI_CH_DISABLED ? UI_C(TEXT2)
         : st->status == UI_CH_CONNECTING ? UI_C(WARN) : UI_C(BAD);
}

static void chans_build(chans_t *c)
{
    lv_obj_t *t = c->chans;
    int32_t sy = lv_obj_get_scroll_y(t);
    lv_obj_clean(t);
    memset(c->state_cell, 0, sizeof c->state_cell);
    c->gen = M()->config_generation ? M()->config_generation() : 0;
    int put = one_checked(c) >= 0;
    lv_obj_t *first = NULL;
    char url[1024], name[64], en[8], idx[8];
    for (int ch = 0; ch < M()->channels && ch < UI_MAX_CHANNELS; ++ch) {
        lv_obj_t *r = trow(t, 0);
        lv_obj_set_user_data(r, (void *)(intptr_t)ch);
        lv_obj_add_event_cb(r, chan_click_cb, LV_EVENT_CLICKED, c);
        lv_obj_add_event_cb(r, chan_key_cb, LV_EVENT_KEY, c);
        if (!first) first = r;
        snprintf(idx, sizeof idx, "%d", ch + 1);
        tcell(r, idx, 28, UI_C(TEXT));
        slot_value(c, ch, "url", url, sizeof url);
        const ui_chan_t *st = ui_channel(ch);
        if (!url[0]) {
            tcell(r, "-", 150, UI_C(TEXT2));
            tcell(r, "Empty", 0, UI_C(TEXT2));
            tcell(r, "-", 70, UI_C(TEXT2));
            c->state_cell[ch] = tcell(r, "-", 150, UI_C(TEXT2));
            tcell(r, "-", 62, UI_C(TEXT2));
            lv_obj_t *box = row_box(r, VG_SPACE_SM);
            lv_obj_set_width(box, 292);
            tbtn(box, LV_SYMBOL_PLUS, put ? "Add checked" : "Add", add_here_cb, c, ch);
            continue;
        }
        slot_value(c, ch, "name", name, sizeof name);
        slot_value(c, ch, "enabled", en, sizeof en);
        if (!name[0]) ui_chan_title(ch, name, sizeof name);
        tcell(r, name, 150, UI_C(TEXT));
        tcell(r, url, 0, UI_C(TEXT));
        tcell(r, scheme_word(url), 70, UI_C(TEXT));
        c->state_cell[ch] = tcell(r, ui_chan_status_word(st), 150, state_color(st));
        int on = !strcmp(en, "1");
        tcell(r, on ? "On" : "Off", 62, on ? UI_C(OK) : UI_C(TEXT2));
        lv_obj_t *box = row_box(r, VG_SPACE_SM);
        lv_obj_set_width(box, 292);
        tbtn(box, LV_SYMBOL_EDIT, "Edit", edit_cb, c, ch);
        tbtn(box, LV_SYMBOL_TRASH, "Clear", clear_cb, c, ch);
        if (put) tbtn(box, LV_SYMBOL_DOWNLOAD, "Replace", replace_cb, c, ch);
    }
    if (first) ui_gridnav_focus(t, first);
    lv_obj_scroll_to_y(t, sy, LV_ANIM_OFF);
}

/* The states only, every poll. */
static void chans_states(chans_t *c)
{
    for (int ch = 0; ch < M()->channels && ch < UI_MAX_CHANNELS; ++ch) {
        lv_obj_t *l = c->state_cell[ch];
        if (!l || slot_empty(c, ch)) continue;
        const ui_chan_t *st = ui_channel(ch);
        const char *w = ui_chan_status_word(st);
        if (strcmp(lv_label_get_text(l), w)) lv_label_set_text(l, w);
        lv_obj_set_style_text_color(l, state_color(st), 0);
    }
}

/* ---- adding ----------------------------------------------------------------------------- */

static void creds(const chans_t *c, const char **user, const char **pass)
{
    *user = lv_textarea_get_text(c->user);
    *pass = lv_textarea_get_text(c->pass);
    if (!**user) *user = "admin";
}

static void assign_start(chans_t *c, const char *key, int ch, int replace)
{
    if (!M()->disc_assign) return;
    const char *user, *pass;
    creds(c, &user, &pass);
    char err[96] = "";
    c->q_n = c->q_i = c->q_ok = c->q_fail = 0;
    c->q_err[0] = 0;
    started(c, M()->disc_assign(key, ch, user, pass, replace, err, sizeof err), err);
    buttons_sync(c);
}

/* Add: the checked devices, each to the next empty channel. */
static void add_next(chans_t *c)
{
    const char *user, *pass;
    creds(c, &user, &pass);
    while (c->q_i < c->q_n) {
        char err[96] = "";
        int id = M()->disc_assign(c->queue[c->q_i++], 0, user, pass, 0, err, sizeof err);
        if (id >= 0) {
            c->job = (unsigned)id;
            return;
        }
        ++c->q_fail;
        snprintf(c->q_err, sizeof c->q_err, "%s", err);
    }
}

static void add_cb(lv_event_t *e)
{
    chans_t *c = lv_event_get_user_data(e);
    if (!c->n_checked || !M()->disc_assign) {
        ui_toast(UI_TOAST_WARN, "Check the devices to add first");
        return;
    }
    c->q_n = c->q_i = c->q_ok = c->q_fail = 0;
    c->q_err[0] = 0;
    for (int k = 0; k < c->n_checked; ++k) {
        int i = device_index(c, c->checked[k]);
        if (i >= 0 && !c->dev[i].channel) snprintf(c->queue[c->q_n++], sizeof c->queue[0], "%s", c->checked[k]);
    }
    if (!c->q_n) {
        ui_toast(UI_TOAST_WARN, "The devices checked are on channels already");
        return;
    }
    add_next(c);
    buttons_sync(c);
}

static void select_all_cb(lv_event_t *e)
{
    chans_t *c = lv_event_get_user_data(e);
    int all = c->n_checked == c->n_dev;
    c->n_checked = 0;
    for (int i = 0; !all && i < c->n_dev; ++i) snprintf(c->checked[c->n_checked++], sizeof c->checked[0], "%s", c->dev[i].key);
    devs_build(c);
    chans_build(c);
    buttons_sync(c);
}

static void search_cb(lv_event_t *e)
{
    chans_t *c = lv_event_get_user_data(e);
    if (!M()->disc_scan) return;
    static const unsigned bits[] = { UI_DISC_NETIP | UI_DISC_ONVIF | UI_DISC_DHIP, UI_DISC_NETIP, UI_DISC_ONVIF,
                                     UI_DISC_DHIP };
    uint32_t sel = lv_dropdown_get_selected(c->proto);
    char err[96] = "";
    started(c, M()->disc_scan(bits[sel < 4 ? sel : 0], err, sizeof err), err);
    buttons_sync(c);
}

/* Change IP, Mass change, Auto add: pages of their own (below). */
static void change_ip_open(chans_t *c, int dev);
static void mass_open(chans_t *c);
static void autoadd_open(chans_t *c);

static void change_ip_cb(lv_event_t *e)
{
    chans_t *c = lv_event_get_user_data(e);
    int i = c->n_checked == 1 ? device_index(c, c->checked[0]) : -1;
    if (i < 0) {
        ui_toast(UI_TOAST_WARN, "Check one device to change its address");
        return;
    }
    change_ip_open(c, i);
}

static void mass_cb(lv_event_t *e) { mass_open(lv_event_get_user_data(e)); }
static void autoadd_cb(lv_event_t *e) { autoadd_open(lv_event_get_user_data(e)); }

static void settings_cb(lv_event_t *e)
{
    (void)e;
    for (int s = 0; s < M()->n_sections; ++s)
        if (!strcmp(M()->sections[s].key, "discovery")) ui_form_open(s, -1);
}

static void buttons_sync(chans_t *c)
{
    int busy = 0;
    if (M()->disc_job && c->jb) busy = c->jb->running || (c->job && c->jb->id != c->job);
    lv_obj_t *need_dev[] = { c->add, c->chip };
    for (size_t i = 0; i < sizeof need_dev / sizeof need_dev[0]; ++i) {
        int off = busy || !c->n_checked || (need_dev[i] == c->chip && c->n_checked != 1);
        if (off) lv_obj_add_state(need_dev[i], LV_STATE_DISABLED);
        else lv_obj_remove_state(need_dev[i], LV_STATE_DISABLED);
    }
    lv_obj_t *other[] = { c->search, c->mass, c->autoadd };
    for (size_t i = 0; i < sizeof other / sizeof other[0]; ++i) {
        if (busy || !M()->disc_scan || (other[i] != c->search && !c->n_dev)) lv_obj_add_state(other[i], LV_STATE_DISABLED);
        else lv_obj_remove_state(other[i], LV_STATE_DISABLED);
    }
    char h[96];
    if (c->n_checked) snprintf(h, sizeof h, "%d checked", c->n_checked);
    else snprintf(h, sizeof h, "%d found", c->n_dev);
    lv_label_set_text(c->hint, h);
}

static void devices_load(chans_t *c)
{
    c->n_dev = M()->disc_devices ? M()->disc_devices(c->dev, UI_DISC_MAX) : 0;
    /* Forget checks of devices no longer there. */
    int k = 0;
    for (int i = 0; i < c->n_checked; ++i)
        if (device_index(c, c->checked[i]) >= 0) memmove(c->checked[k++], c->checked[i], sizeof c->checked[0]);
    c->n_checked = k;
}

/* A job of this page's ended. */
static void job_done(chans_t *c)
{
    ui_disc_job_t *j = c->jb;
    c->job = 0;
    if (j->kind == UI_JOB_SCAN) {
        devices_load(c);
        devs_build(c);
        chans_build(c);
    } else if (j->kind == UI_JOB_ASSIGN) {
        const ui_disc_res_t *r = j->n ? &j->r[0] : NULL;
        if (c->q_n) {
            if (r && r->ok) ++c->q_ok;
            else {
                ++c->q_fail;
                snprintf(c->q_err, sizeof c->q_err, "%s: %.64s", r ? r->ip : "?", r ? r->msg : j->msg);
            }
            if (c->q_i < c->q_n) add_next(c);
            if (!c->job) {
                if (c->q_fail) ui_toast(UI_TOAST_BAD, "%d added, %d not: %s", c->q_ok, c->q_fail, c->q_err);
                else ui_toast(UI_TOAST_OK, "%d added", c->q_ok);
                c->q_n = 0;
            }
        } else if (r && r->ok) {
            ui_toast(UI_TOAST_OK, "%s added to channel %d", r->ip, r->channel);
        } else {
            ui_toast(UI_TOAST_BAD, "Not added: %s", r ? r->msg : j->msg);
        }
        devices_load(c);
        c->n_checked = c->job ? c->n_checked : 0;
        devs_build(c);
        chans_build(c);
    }
}

static void poll_cb(lv_timer_t *t)
{
    chans_t *c = lv_timer_get_user_data(t);
    if (M()->disc_job) {
        M()->disc_job(c->jb);
        const ui_disc_job_t *j = c->jb;
        char s[128];
        if (j->running && j->kind == UI_JOB_SCAN) snprintf(s, sizeof s, LV_SYMBOL_REFRESH " Searching... %d%%", j->progress);
        else if (j->running) snprintf(s, sizeof s, LV_SYMBOL_REFRESH " %s", j->msg);
        else if (j->kind == UI_JOB_SCAN) snprintf(s, sizeof s, "%s", j->msg);
        else s[0] = 0;
        if (strcmp(lv_label_get_text(c->status), s)) lv_label_set_text(c->status, s);
        if (c->job && j->id == c->job && !j->running) job_done(c);
    }
    chans_states(c);
    buttons_sync(c);
}

static void config_cb(lv_observer_t *o, lv_subject_t *s)
{
    (void)s;
    chans_t *c = lv_observer_get_user_data(o);
    devices_load(c);                /* "On channel N" follows the cameras */
    devs_build(c);
    chans_build(c);
}

static int chans_back(ui_page_t *p) { return osk_back(((chans_t *)p->user)->kb); }

static void chans_close(ui_page_t *p)
{
    chans_t *c = p->user;
    if (c->timer) lv_timer_delete(c->timer);
    if (g_ch == c) g_ch = NULL;
    lv_free(c->jb);
    lv_free(c);
}

/* Returning from a sub-page: what it changed shows. */
static void refresh_async(void *u)
{
    chans_t *c = u;
    if (g_ch != c) return;
    devices_load(c);
    devs_build(c);
    chans_build(c);
}

static lv_obj_t *toolbar_btn(lv_obj_t *row, chans_t *c, const char *sym, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = ui_btn(row, sym, text);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, c);
    lv_group_add_obj(c->page->group, b);
    return b;
}

void ui_channels_open(void)
{
    int sec = -1;
    for (int s = 0; s < M()->n_sections; ++s)
        if (!strcmp(M()->sections[s].key, "cameras")) sec = s;
    if (sec < 0) return;
    chans_t *c = lv_malloc_zeroed(sizeof *c);
    if (!c) return;
    c->jb = lv_malloc_zeroed(sizeof *c->jb);
    ui_page_t *p = ui_page_push("Channels");
    if (!p || !c->jb) {
        lv_free(c->jb);
        lv_free(c);
        return;
    }
    g_ch = c;
    c->page = p;
    c->sec = sec;
    p->user = c;
    p->on_back = chans_back;
    p->on_close = chans_close;
    lv_obj_remove_flag(p->body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(p->body, VG_SPACE_LG, 0);
    lv_obj_set_style_pad_row(p->body, VG_SPACE_MD, 0);
    c->kb = osk_for(p);

    /* Search */
    lv_obj_t *sp = pane(p->body);
    lv_obj_t *r1 = row_box(sp, VG_SPACE_SM);
    pane_title(r1, &ui_icon_search, "Search");
    c->proto = lv_dropdown_create(r1);
    lv_dropdown_set_options(c->proto, "All protocols\nNETIP\nONVIF\nDHIP");
    lv_obj_set_width(c->proto, 160);
    ui_focus_style(c->proto);
    lv_group_add_obj(p->group, c->proto);
    c->search = toolbar_btn(r1, c, LV_SYMBOL_REFRESH, "Search", search_cb);
    ui_btn_accent(c->search);
    c->status = ui_label(r1, "", VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_set_width(c->status, 1);
    lv_obj_set_flex_grow(c->status, 1);
    lv_label_set_long_mode(c->status, LV_LABEL_LONG_DOT);
    c->add = toolbar_btn(r1, c, LV_SYMBOL_PLUS, "Add", add_cb);
    c->chip = toolbar_btn(r1, c, LV_SYMBOL_EDIT, "Change IP...", change_ip_cb);
    c->mass = toolbar_btn(r1, c, LV_SYMBOL_LIST, "Mass change...", mass_cb);
    c->autoadd = toolbar_btn(r1, c, LV_SYMBOL_SHUFFLE, "Auto add all", autoadd_cb);

    lv_obj_t *r2 = row_box(sp, VG_SPACE_SM);
    c->all = toolbar_btn(r2, c, LV_SYMBOL_OK, "Select all", select_all_cb);
    c->hint = ui_label(r2, "", VG_FONT_SM_LV, UI_C(TEXT2));
    lv_obj_set_width(c->hint, 1);
    lv_obj_set_flex_grow(c->hint, 1);
    ui_label(r2, "Add with user", VG_FONT_SM_LV, UI_C(TEXT2));
    c->user = field(r2, p, c->kb, "admin", 150, 0, NULL, 63);
    ui_label(r2, "password", VG_FONT_SM_LV, UI_C(TEXT2));
    c->pass = field(r2, p, c->kb, "", 170, 1, NULL, 63);

    header(sp, k_dev_cols, sizeof k_dev_cols / sizeof k_dev_cols[0]);
    c->devs = table_rows(sp, DEV_H);
    ui_gridnav(c->devs, LV_GRIDNAV_CTRL_NONE);
    lv_group_add_obj(p->group, c->devs);

    /* Channels */
    lv_obj_t *cp = pane(p->body);
    lv_obj_set_flex_grow(cp, 1);
    lv_obj_t *r3 = row_box(cp, VG_SPACE_SM);
    pane_title(r3, &ui_icon_channels, "Channels");
    lv_obj_t *ht = ui_label(r3, "Check one device above, then Add or Replace on a channel row.", VG_FONT_SM_LV,
                            UI_C(TEXT2));
    lv_obj_set_width(ht, 1);
    lv_obj_set_flex_grow(ht, 1);
    lv_label_set_long_mode(ht, LV_LABEL_LONG_DOT);
    header(cp, k_ch_cols, sizeof k_ch_cols / sizeof k_ch_cols[0]);
    c->chans = table_rows(cp, 0);
    lv_subject_add_observer_obj(&ui_subj_config, config_cb, c->chans, c);
    ui_gridnav(c->chans, LV_GRIDNAV_CTRL_NONE);
    lv_group_add_obj(p->group, c->chans);

    for (int s = 0; s < M()->n_sections; ++s)
        if (!strcmp(M()->sections[s].key, "discovery")) {
            lv_obj_t *b = ui_btn(p->actions, LV_SYMBOL_SETTINGS, "Search settings");
            lv_obj_add_event_cb(b, settings_cb, LV_EVENT_CLICKED, c);
        }

    devices_load(c);
    devs_build(c);
    chans_build(c);
    if (M()->disc_job) M()->disc_job(c->jb);
    buttons_sync(c);
    c->timer = lv_timer_create(poll_cb, POLL_MS, c);
    if (c->kb) lv_obj_move_foreground(c->kb);       /* over the panes */
    ui_page_ready(p);
}

/* ---- Change IP, Mass change, Auto add ------------------------------------------------------ */

typedef enum { SUB_IP, SUB_MASS, SUB_AUTO } sub_kind_t;

typedef struct {
    chans_t      *c;
    sub_kind_t    kind;
    ui_page_t    *page;
    lv_obj_t     *kb;
    lv_obj_t     *ip, *mask, *gw, *user, *pass;
    lv_obj_t     *run, *preview, *status, *table;
    lv_timer_t   *timer;
    unsigned      job;
    ui_disc_job_t *jb;
    ui_disc_ip_t  items[UI_DISC_MAX];
    int           n;
    int           planned;          /* Mass change: a preview to apply */
} sub_t;

static int sub_back(ui_page_t *p) { return osk_back(((sub_t *)p->user)->kb); }

static void sub_close(ui_page_t *p)
{
    sub_t *s = p->user;
    if (s->timer) lv_timer_delete(s->timer);
    lv_async_call(refresh_async, s->c);
    lv_free(s->jb);
    lv_free(s);
}

/* The job's results as a table: device, address(es), protocol, outcome. */
static void sub_results(sub_t *s)
{
    lv_obj_t *t = s->table;
    lv_obj_clean(t);
    const ui_disc_job_t *j = s->jb;
    lv_obj_t *h = trow(t, 1);
    tcell(h, "Device", 150, UI_C(TEXT2));
    tcell(h, "Address", 130, UI_C(TEXT2));
    tcell(h, s->kind == SUB_AUTO ? "Channel" : "New address", 130, UI_C(TEXT2));
    tcell(h, "Protocol", 80, UI_C(TEXT2));
    tcell(h, "Result", 0, UI_C(TEXT2));
    for (int i = 0; i < j->n; ++i) {
        const ui_disc_res_t *r = &j->r[i];
        lv_obj_t *row = trow(t, 0);
        ui_inert(row);
        char ch[16];
        snprintf(ch, sizeof ch, "%d", r->channel);
        tcell(row, r->key, 150, UI_C(TEXT));
        tcell(row, r->ip, 130, UI_C(TEXT));
        tcell(row, s->kind == SUB_AUTO ? (r->channel ? ch : "-") : r->new_ip, 130, UI_C(TEXT));
        tcell(row, proto_word(r->proto), 80, UI_C(TEXT));
        const char *word = !r->done ? "waiting" : r->msg;
        lv_color_t col = !r->done ? UI_C(TEXT2) : r->ok ? (j->dry_run ? UI_C(TEXT) : UI_C(OK)) : UI_C(BAD);
        tcell(row, word, 0, col);
    }
}

static void sub_poll_cb(lv_timer_t *t)
{
    sub_t *s = lv_timer_get_user_data(t);
    if (!s->job || !M()->disc_job) return;
    M()->disc_job(s->jb);
    if (s->jb->id != s->job) return;
    char m[128];
    snprintf(m, sizeof m, "%s%s", s->jb->running ? LV_SYMBOL_REFRESH " " : "", s->jb->msg);
    lv_label_set_text(s->status, m);
    sub_results(s);
    if (s->jb->running) return;
    s->job = 0;
    lv_obj_remove_state(s->run, LV_STATE_DISABLED);
    if (s->preview) lv_obj_remove_state(s->preview, LV_STATE_DISABLED);
    if (s->kind == SUB_MASS && s->jb->dry_run) {
        /* The plan: applied as it is shown. */
        s->planned = 0;
        for (int i = 0; i < s->jb->n && i < s->n; ++i) {
            snprintf(s->items[i].ip, sizeof s->items[i].ip, "%s", s->jb->r[i].ok ? s->jb->r[i].new_ip : "");
            s->planned += s->jb->r[i].ok;
        }
        if (s->planned) lv_obj_remove_state(s->run, LV_STATE_DISABLED);
        else lv_obj_add_state(s->run, LV_STATE_DISABLED);
    } else if (s->kind == SUB_MASS) {
        s->planned = 0;
        lv_obj_add_state(s->run, LV_STATE_DISABLED);
    }
}

static void sub_run(sub_t *s, int dry)
{
    char err[96] = "";
    const char *user = lv_textarea_get_text(s->user), *pass = lv_textarea_get_text(s->pass);
    int id = -1;
    if (s->kind == SUB_AUTO) {
        id = M()->disc_autoadd ? M()->disc_autoadd(user, pass, err, sizeof err) : -1;
    } else {
        const char *start = s->kind == SUB_MASS ? lv_textarea_get_text(s->ip) : "";
        ui_disc_ip_t items[UI_DISC_MAX];
        memcpy(items, s->items, sizeof items);
        if (s->kind == SUB_IP) snprintf(items[0].ip, sizeof items[0].ip, "%s", lv_textarea_get_text(s->ip));
        if (s->kind == SUB_MASS && dry)
            for (int i = 0; i < s->n; ++i) items[i].ip[0] = 0;
        if (s->kind == SUB_MASS && !dry) {
            /* Only those the plan gave an address. */
            int k = 0;
            for (int i = 0; i < s->n; ++i)
                if (items[i].ip[0]) items[k++] = items[i];
            if (!k) return;
            id = M()->disc_ip(items, k, "", lv_textarea_get_text(s->mask), lv_textarea_get_text(s->gw), user,
                              pass, 0, err, sizeof err);
        } else {
            id = M()->disc_ip ? M()->disc_ip(items, s->n, start, lv_textarea_get_text(s->mask),
                                              lv_textarea_get_text(s->gw), user, pass, dry, err, sizeof err)
                              : -1;
        }
    }
    if (id < 0) {
        ui_toast(UI_TOAST_BAD, "%s", err[0] ? err : "Cannot start");
        return;
    }
    s->job = (unsigned)id;
    lv_obj_add_state(s->run, LV_STATE_DISABLED);
    if (s->preview) lv_obj_add_state(s->preview, LV_STATE_DISABLED);
    lv_label_set_text(s->status, LV_SYMBOL_REFRESH " Starting...");
}

static void run_cb(lv_event_t *e) { sub_run(lv_event_get_user_data(e), 0); }
static void preview_cb(lv_event_t *e) { sub_run(lv_event_get_user_data(e), 1); }

static sub_t *sub_open(chans_t *c, sub_kind_t kind, const char *title)
{
    sub_t *s = lv_malloc_zeroed(sizeof *s);
    if (!s) return NULL;
    s->jb = lv_malloc_zeroed(sizeof *s->jb);
    ui_page_t *p = ui_page_push(title);
    if (!p || !s->jb) {
        lv_free(s->jb);
        lv_free(s);
        return NULL;
    }
    s->c = c;
    s->kind = kind;
    s->page = p;
    p->user = s;
    p->on_back = sub_back;
    p->on_close = sub_close;
    lv_obj_set_style_pad_row(p->body, VG_SPACE_MD, 0);
    s->kb = osk_for(p);
    return s;
}

/* The fields every sub-page has, the table and the timer. */
static void sub_finish(sub_t *s, const char *run_text)
{
    ui_page_t *p = s->page;
    const char *u, *pw;
    creds(s->c, &u, &pw);
    s->user = labelled(p->body, p, s->kb, "User", u, 0, NULL, 63);
    s->pass = labelled(p->body, p, s->kb, "Password", pw, 1, NULL, 63);
    lv_obj_t *r = row_box(p->body, VG_SPACE_MD);
    if (s->kind == SUB_MASS) {
        s->preview = ui_btn(r, LV_SYMBOL_EYE_OPEN, "Preview");
        lv_obj_add_event_cb(s->preview, preview_cb, LV_EVENT_CLICKED, s);
        lv_group_add_obj(p->group, s->preview);
    }
    s->run = ui_btn(r, LV_SYMBOL_OK, run_text);
    ui_btn_accent(s->run);
    lv_obj_add_event_cb(s->run, run_cb, LV_EVENT_CLICKED, s);
    lv_group_add_obj(p->group, s->run);
    if (s->kind == SUB_MASS) lv_obj_add_state(s->run, LV_STATE_DISABLED);
    s->status = ui_label(r, "", VG_FONT_MD_LV, UI_C(TEXT2));
    lv_obj_set_flex_grow(s->status, 1);
    lv_obj_t *panel = lv_obj_create(p->body);
    ui_plain(panel);
    lv_obj_add_style(panel, &ui_st_panel, 0);
    lv_obj_set_size(panel, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(panel, VG_SPACE_SM, 0);
    s->table = table_rows(panel, -1);
    lv_obj_set_height(s->table, LV_SIZE_CONTENT);
    s->timer = lv_timer_create(sub_poll_cb, POLL_MS, s);
    if (s->kb) lv_obj_move_foreground(s->kb);
    ui_page_ready(p);
}

static void change_ip_open(chans_t *c, int i)
{
    const ui_disc_dev_t *d = &c->dev[i];
    char title[64];
    snprintf(title, sizeof title, "Change IP: %s", d->ip);
    sub_t *s = sub_open(c, SUB_IP, title);
    if (!s) return;
    ui_page_t *p = s->page;
    snprintf(s->items[0].key, sizeof s->items[0].key, "%s", d->key);
    s->n = 1;
    char about[200], pr[48];
    protos_text(d->protos, pr, sizeof pr);
    snprintf(about, sizeof about, "%s %s  %s  MAC %s  (%s)", d->vendor, d->model, d->ip, d->mac[0] ? d->mac : "-", pr);
    ui_label(p->body, about, VG_FONT_MD_LV, UI_C(TEXT2));
    s->ip = labelled(p->body, p, s->kb, "New IP address", d->ip, 0, IP_CHARS, 15);
    s->mask = labelled(p->body, p, s->kb, "Subnet mask", d->mask[0] ? d->mask : "255.255.255.0", 0, IP_CHARS, 15);
    s->gw = labelled(p->body, p, s->kb, "Gateway", d->gateway, 0, IP_CHARS, 15);
    sub_finish(s, "Change");
}

static void mass_open(chans_t *c)
{
    sub_t *s = sub_open(c, SUB_MASS, "Mass change of addresses");
    if (!s) return;
    ui_page_t *p = s->page;
    /* The devices checked, or all of them. */
    for (int i = 0; i < c->n_dev; ++i)
        if (!c->n_checked || is_checked(c, c->dev[i].key))
            snprintf(s->items[s->n++].key, sizeof s->items[0].key, "%s", c->dev[i].key);
    char about[200];
    snprintf(about, sizeof about, "%d device%s get addresses one after another from the start address; "
             "addresses in use are skipped. Preview first, then Apply.", s->n, s->n == 1 ? "" : "s");
    lv_obj_t *l = ui_label(p->body, about, VG_FONT_MD_LV, UI_C(TEXT2));
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    const ui_disc_dev_t *d0 = s->n ? &c->dev[device_index(c, s->items[0].key)] : NULL;
    s->ip = labelled(p->body, p, s->kb, "Start address", d0 ? d0->ip : "", 0, IP_CHARS, 15);
    s->mask = labelled(p->body, p, s->kb, "Subnet mask", d0 && d0->mask[0] ? d0->mask : "255.255.255.0", 0,
                       IP_CHARS, 15);
    s->gw = labelled(p->body, p, s->kb, "Gateway", d0 ? d0->gateway : "", 0, IP_CHARS, 15);
    sub_finish(s, "Apply");
}

static void autoadd_open(chans_t *c)
{
    sub_t *s = sub_open(c, SUB_AUTO, "Auto add all");
    if (!s) return;
    int fresh = 0, empty = 0;
    for (int i = 0; i < c->n_dev; ++i) fresh += !c->dev[i].channel;
    for (int ch = 0; ch < M()->channels && ch < UI_MAX_CHANNELS; ++ch) empty += slot_empty(c, ch);
    char about[160];
    snprintf(about, sizeof about, "%d new device%s go to the %d empty channel%s in address order, with this "
             "user and password.", fresh, fresh == 1 ? "" : "s", empty, empty == 1 ? "" : "s");
    lv_obj_t *l = ui_label(s->page->body, about, VG_FONT_MD_LV, UI_C(TEXT2));
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    sub_finish(s, "Add all");
}
