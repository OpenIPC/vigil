/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_form.c — settings pages built from the schema at run time.
 *
 * A form edits one object section (Recording, HTTP API) or one item of an
 * array section (a camera); the list page shows an array section's items
 * with Add, Edit and Remove. Widgets follow the field's kind:
 *
 *   string  text area; the on-screen keyboard opens on it, unless a
 *           keyboard is in use
 *   integer text area for digits with - and + (kept within min..max);
 *           Up and Down step it
 *   boolean switch
 *   enum    drop-down list of the values
 *
 * The page's focus order is Back, the fields, Cancel and Save; Enter in a
 * text field goes to the next field (from the last one, to Save), and
 * Ctrl+S saves (ui_page_t.save).
 *
 * with the schema's label and help, and "applies after restart" for fields
 * that are not live. Save sends the whole section to model->save, the path
 * the HTTP API takes (validate, save atomically, bump the generation; the
 * main loop applies it), and shows the store's errors under their fields.
 * Secret values (camera URLs) are shown redacted, as the API shows them,
 * and an unchanged one keeps the stored password.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_app.h"
#include "ui_theme.h"

#define FIELDS_MAX 24
#define LABEL_W    320
#define KB_H       280

typedef struct {
    int        sec, item;           /* item: -1 for an object section */
    int        is_new;              /* item is being added */
    int        n_items, n_fields;
    char     **vals;                /* n_items x n_fields, as stored (redacted) */
    unsigned   gen;                 /* the generation `vals` came from */
    ui_page_t *page;
    lv_obj_t  *w[FIELDS_MAX];       /* the value widget */
    lv_obj_t  *err[FIELDS_MAX];
    lv_obj_t  *general;             /* errors without a field */
    lv_obj_t  *kb;
} form_t;

static const ui_section_t *section(int sec) { return &ui_model()->sections[sec]; }

static char *dup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static void vals_free(char **v, int n)
{
    if (!v) return;
    for (int i = 0; i < n; ++i) free(v[i]);
    free(v);
}

/* The section as stored, plus a new item with the defaults when adding;
 * `gen`: the generation they were read from (taken first: if it moves
 * while reading, the save is refused as stale, never mixed). */
static char **vals_load(int sec, int add, int *n_items, unsigned *gen)
{
    const ui_section_t *s = section(sec);
    *gen = ui_model()->config_generation ? ui_model()->config_generation() : 0;
    int n = s->is_array ? ui_model()->item_count(sec) : 1;
    int total = n + (add ? 1 : 0);
    char **v = calloc((size_t)(total * s->n_fields + 1), sizeof *v);
    if (!v) return NULL;
    char buf[1024];
    for (int k = 0; k < total; ++k)
        for (int f = 0; f < s->n_fields; ++f) {
            if (k < n) ui_model()->get_value(sec, s->is_array ? k : -1, f, buf, sizeof buf);
            else snprintf(buf, sizeof buf, "%s", s->fields[f].def ? s->fields[f].def : "");
            if (!(v[k * s->n_fields + f] = dup(buf))) {
                vals_free(v, total * s->n_fields);
                return NULL;
            }
        }
    *n_items = total;
    return v;
}

/* `origin`: as model->save (NULL for an object section). */
static ui_save_rc_t save_section(int sec, int n_items, char **v, const int *origin, unsigned gen,
                                 ui_errors_t *errs)
{
    memset(errs, 0, sizeof *errs);
    return ui_model()->save(sec, n_items, (const char *const *)v, origin, gen, errs);
}

static void report(ui_save_rc_t rc, const ui_errors_t *errs, int restart_needed)
{
    switch (rc) {
    case UI_SAVE_OK:
        if (restart_needed) ui_toast(UI_TOAST_WARN, "Saved. Some changes apply after vigild restarts.");
        else ui_toast(UI_TOAST_OK, "Saved");
        break;
    case UI_SAVE_UNSYNCED:
        ui_toast(UI_TOAST_WARN, "%s", errs->n ? errs->e[0].msg : "Saved, but may not survive a power cut");
        break;
    case UI_SAVE_INVALID:
        ui_toast(UI_TOAST_BAD, "Not saved: %d problem%s", errs->total, errs->total == 1 ? "" : "s");
        break;
    case UI_SAVE_FAILED:
        ui_toast(UI_TOAST_BAD, "Not saved: %s", errs->n ? errs->e[0].msg : "error");
        break;
    case UI_SAVE_STALE:
        ui_toast(UI_TOAST_WARN, "Not saved: the settings were changed elsewhere meanwhile "
                                "(HTTP API). Showing them now; make the change again.");
        break;
    }
}

/* ---- the form --------------------------------------------------------------------------- */

static int row_of(const form_t *f) { return f->item < 0 ? 0 : f->item; }

/* The widget's value as text. */
static void widget_value(const form_t *f, int i, char *buf, size_t cap)
{
    const ui_field_t *fd = &section(f->sec)->fields[i];
    lv_obj_t *w = f->w[i];
    switch (fd->kind) {
    case UI_FIELD_STRING:
    case UI_FIELD_INT:
        snprintf(buf, cap, "%s", lv_textarea_get_text(w));
        break;
    case UI_FIELD_BOOL:
        snprintf(buf, cap, "%d", lv_obj_has_state(w, LV_STATE_CHECKED) ? 1 : 0);
        break;
    case UI_FIELD_ENUM:
        snprintf(buf, cap, "%s", fd->values[lv_dropdown_get_selected(w)]);
        break;
    }
}

static int form_dirty(const form_t *f)
{
    char buf[1024];
    for (int i = 0; i < f->n_fields; ++i) {
        widget_value(f, i, buf, sizeof buf);
        if (strcmp(buf, f->vals[row_of(f) * f->n_fields + i])) return 1;
    }
    return f->is_new;
}

static void kb_hide(form_t *f)
{
    if (!f->kb || lv_obj_has_flag(f->kb, LV_OBJ_FLAG_HIDDEN)) return;
    lv_keyboard_set_textarea(f->kb, NULL);
    lv_obj_add_flag(f->kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_bottom(f->page->body, VG_SPACE_XL, 0);
}

static void kb_show(form_t *f, lv_obj_t *ta, int numeric)
{
    lv_keyboard_set_mode(f->kb, numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(f->kb, ta);
    lv_obj_remove_flag(f->kb, LV_OBJ_FLAG_HIDDEN);
    /* Room to scroll the field above the keyboard. */
    lv_obj_set_style_pad_bottom(f->page->body, KB_H + VG_SPACE_XL, 0);
    lv_obj_update_layout(f->page->body);
    lv_obj_scroll_to_view_recursive(ta, LV_ANIM_OFF);
}

/* Clicked (or Enter): the on-screen keyboard, for a mouse user. Someone
 * with a keyboard types into the field directly. */
static void ta_click_cb(lv_event_t *e)
{
    form_t *f = lv_event_get_user_data(e);
    lv_obj_t *ta = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(ta);
    if (ui_keyboard_in_use()) return;
    kb_show(f, ta, section(f->sec)->fields[i].kind == UI_FIELD_INT);
}

/* Enter in a text field: the next field, or Save after the last. */
static void ta_ready_cb(lv_event_t *e)
{
    form_t *f = lv_event_get_user_data(e);
    lv_indev_t *in = lv_indev_active();
    if (!in || lv_indev_get_type(in) != LV_INDEV_TYPE_KEYPAD) return;   /* the on-screen keyboard's OK */
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    lv_obj_t *next = f->page->save;
    for (int k = i + 1; k < f->n_fields; ++k)
        if (f->w[k]) {
            next = f->w[k];
            break;
        }
    /* The Enter's release must not click what gets the focus now. */
    lv_indev_wait_release(in);
    lv_group_focus_obj(next);
}

static void kb_cb(lv_event_t *e)
{
    kb_hide(lv_event_get_user_data(e));
}

static void int_step(form_t *f, int i, int up)
{
    const ui_field_t *fd = &section(f->sec)->fields[i];
    long v = strtol(lv_textarea_get_text(f->w[i]), NULL, 10) + (up ? 1 : -1);
    if (v < fd->min) v = fd->min;
    if (v > fd->max) v = fd->max;
    char buf[16];
    snprintf(buf, sizeof buf, "%ld", v);
    lv_textarea_set_text(f->w[i], buf);
}

static void int_step_cb(lv_event_t *e)
{
    form_t *f = lv_event_get_user_data(e);
    int code = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));    /* field * 2 + (1 for +) */
    int_step(f, code / 2, code & 1);
}

/* Up and Down on a number step it, before the text area moves its cursor. */
static void int_key_cb(lv_event_t *e)
{
    uint32_t k = lv_event_get_key(e);
    if (k != LV_KEY_UP && k != LV_KEY_DOWN) return;
    form_t *f = lv_event_get_user_data(e);
    int_step(f, (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)), k == LV_KEY_UP);
    lv_event_stop_processing(e);
}

static lv_obj_t *step_btn(lv_obj_t *parent, form_t *f, int i, int up)
{
    lv_obj_t *b = ui_btn(parent, up ? LV_SYMBOL_PLUS : LV_SYMBOL_MINUS, NULL);
    lv_obj_set_width(b, VG_SIZE_BUTTON);
    lv_obj_set_user_data(b, (void *)(intptr_t)(i * 2 + up));
    lv_obj_add_event_cb(b, int_step_cb, LV_EVENT_CLICKED, f);
    lv_obj_add_event_cb(b, int_step_cb, LV_EVENT_LONG_PRESSED_REPEAT, f);
    return b;
}

static lv_obj_t *text_area(lv_obj_t *parent, form_t *f, int i, const char *value)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, 1);
    lv_textarea_set_text(ta, value);
    lv_obj_set_user_data(ta, (void *)(intptr_t)i);
    lv_obj_add_event_cb(ta, ta_click_cb, LV_EVENT_CLICKED, f);
    lv_obj_add_event_cb(ta, ta_ready_cb, LV_EVENT_READY, f);
    lv_obj_set_style_border_color(ta, UI_C(BAD), LV_PART_MAIN | LV_STATE_USER_1);
    lv_obj_set_style_border_width(ta, 2, LV_PART_MAIN | LV_STATE_USER_1);
    return ta;
}

static void field_row(lv_obj_t *list, form_t *f, int i)
{
    const ui_field_t *fd = &section(f->sec)->fields[i];
    const char *value = f->vals[row_of(f) * f->n_fields + i];

    lv_obj_t *row = lv_obj_create(list);
    ui_plain(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, VG_SPACE_MD, 0);
    lv_obj_set_style_border_color(row, UI_C(LINE), 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(row, i ? 1 : 0, 0);
    ui_ring_room(row);                  /* the focus ring */
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, VG_SPACE_XL, 0);

    lv_obj_t *left = lv_obj_create(row);
    ui_plain(left);
    lv_obj_set_size(left, LABEL_W, LV_SIZE_CONTENT);
    lv_obj_set_layout(left, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, VG_SPACE_XS, 0);
    lv_obj_set_style_pad_top(left, VG_SPACE_SM, 0);
    lv_obj_t *l = ui_label(left, fd->label ? fd->label : fd->key, VG_FONT_MD_LV, UI_C(TEXT));
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    if (!fd->live) ui_label(left, LV_SYMBOL_REFRESH "  applies after restart", VG_FONT_SM_LV, UI_C(WARN));

    lv_obj_t *right = lv_obj_create(row);
    ui_plain(right);
    lv_obj_set_height(right, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(right, 1);
    ui_ring_room(right);
    lv_obj_set_layout(right, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, VG_SPACE_XS, 0);

    char range[48] = "";
    switch (fd->kind) {
    case UI_FIELD_STRING:
        f->w[i] = text_area(right, f, i, value);
        lv_obj_set_width(f->w[i], LV_PCT(100));
        if (fd->max_len > 0) lv_textarea_set_max_length(f->w[i], (uint32_t)fd->max_len);
        break;
    case UI_FIELD_INT: {
        lv_obj_t *box = lv_obj_create(right);
        ui_plain(box);
        lv_obj_set_size(box, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        ui_ring_room(box);
        lv_obj_set_layout(box, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(box, VG_SPACE_SM, 0);
        step_btn(box, f, i, 0);
        f->w[i] = text_area(box, f, i, value);
        lv_textarea_set_accepted_chars(f->w[i], fd->min < 0 ? "-0123456789" : "0123456789");
        lv_textarea_set_max_length(f->w[i], 11);
        lv_obj_set_width(f->w[i], 140);
        lv_obj_set_style_text_align(f->w[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(f->w[i], UI_FLAG_ARROWS);
        lv_obj_add_event_cb(f->w[i], int_key_cb, LV_EVENT_KEY | LV_EVENT_PREPROCESS, f);
        step_btn(box, f, i, 1);
        snprintf(range, sizeof range, "%d ... %d", fd->min, fd->max);
        ui_label(box, range, VG_FONT_SM_LV, UI_C(TEXT2));
        break;
    }
    case UI_FIELD_BOOL:
        f->w[i] = lv_switch_create(right);
        if (!strcmp(value, "1")) lv_obj_add_state(f->w[i], LV_STATE_CHECKED);
        break;
    case UI_FIELD_ENUM: {
        f->w[i] = lv_dropdown_create(right);
        char opts[512] = "";
        int sel = 0;
        for (int k = 0; fd->values && fd->values[k]; ++k) {
            size_t n = strlen(opts);
            snprintf(opts + n, sizeof opts - n, "%s%s", k ? "\n" : "", fd->values[k]);
            if (!strcmp(fd->values[k], value)) sel = k;
        }
        lv_dropdown_set_options(f->w[i], opts);
        lv_dropdown_set_selected(f->w[i], (uint32_t)sel);
        lv_obj_set_width(f->w[i], 320);
        break;
    }
    }
    ui_focus_style(f->w[i]);
    lv_group_add_obj(f->page->group, f->w[i]);
    if (fd->help && *fd->help) {
        lv_obj_t *h = ui_label(right, fd->help, VG_FONT_SM_LV, UI_C(TEXT2));
        lv_obj_set_width(h, LV_PCT(100));
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    }
    f->err[i] = ui_label(right, "", VG_FONT_SM_LV, UI_C(BAD));
    lv_obj_set_width(f->err[i], LV_PCT(100));
    lv_label_set_long_mode(f->err[i], LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(f->err[i], LV_OBJ_FLAG_HIDDEN);
}

static void errors_clear(form_t *f)
{
    for (int i = 0; i < f->n_fields; ++i) {
        lv_obj_add_flag(f->err[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(f->w[i], LV_STATE_USER_1);
    }
    lv_obj_add_flag(f->general, LV_OBJ_FLAG_HIDDEN);
}

static void errors_show(form_t *f, const ui_errors_t *errs)
{
    char general[600] = "";
    lv_obj_t *first = NULL;
    for (int k = 0; k < errs->n; ++k) {
        const ui_error_t *e = &errs->e[k];
        int mine = section(f->sec)->is_array ? e->item == f->item : 1;
        if (mine && e->field >= 0 && e->field < f->n_fields) {
            lv_label_set_text_fmt(f->err[e->field], LV_SYMBOL_CLOSE "  %s", e->msg);
            lv_obj_remove_flag(f->err[e->field], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_state(f->w[e->field], LV_STATE_USER_1);
            if (!first) first = f->w[e->field];
        } else {
            size_t n = strlen(general);
            snprintf(general + n, sizeof general - n, "%s%s", n ? "\n" : "", e->msg);
        }
    }
    if (errs->total > errs->n) {
        size_t n = strlen(general);
        snprintf(general + n, sizeof general - n, "%s%d more", n ? "\n" : "", errs->total - errs->n);
    }
    if (general[0]) {
        lv_label_set_text(f->general, general);
        lv_obj_remove_flag(f->general, LV_OBJ_FLAG_HIDDEN);
        first = f->general;
    }
    if (first) lv_obj_scroll_to_view_recursive(first, LV_ANIM_OFF);
}

static void form_title(form_t *f)
{
    const ui_section_t *s = section(f->sec);
    if (!s->is_array) lv_label_set_text(f->page->title, s->label);
    else if (f->is_new) lv_label_set_text_fmt(f->page->title, "%s: new", s->label);
    else lv_label_set_text_fmt(f->page->title, "%s: %d", s->label, f->item + 1);
}

/* The widgets show f->vals' row again. */
static void widgets_from_vals(form_t *f)
{
    const ui_section_t *s = section(f->sec);
    for (int i = 0; i < f->n_fields; ++i) {
        const char *v = f->vals[row_of(f) * f->n_fields + i];
        switch (s->fields[i].kind) {
        case UI_FIELD_STRING:
        case UI_FIELD_INT:
            lv_textarea_set_text(f->w[i], v);
            break;
        case UI_FIELD_BOOL:
            if (!strcmp(v, "1")) lv_obj_add_state(f->w[i], LV_STATE_CHECKED);
            else lv_obj_remove_state(f->w[i], LV_STATE_CHECKED);
            break;
        case UI_FIELD_ENUM:
            for (int k = 0; s->fields[i].values && s->fields[i].values[k]; ++k)
                if (!strcmp(s->fields[i].values[k], v)) lv_dropdown_set_selected(f->w[i], (uint32_t)k);
            break;
        }
    }
}

static void save_cb(lv_event_t *e)
{
    form_t *f = lv_event_get_user_data(e);
    const ui_section_t *s = section(f->sec);
    kb_hide(f);
    errors_clear(f);

    /* The section as stored, with this form's fields in their row. */
    int total = f->n_items * f->n_fields;
    char **v = calloc((size_t)total + 1, sizeof *v);
    if (!v) return;
    int restart = 0, ok = 1;
    for (int k = 0; k < total && ok; ++k) {
        int r = k / f->n_fields, i = k % f->n_fields;
        if (r == row_of(f)) {
            char buf[1024];
            widget_value(f, i, buf, sizeof buf);
            if (!s->fields[i].live && strcmp(buf, f->vals[k])) restart = 1;
            v[k] = dup(buf);
        } else {
            v[k] = dup(f->vals[k]);
        }
        ok = v[k] != NULL;
    }
    /* Item k was read from stored item k; a new one from none. */
    int origin[UI_MAX_ITEMS];
    for (int k = 0; k < f->n_items && k < UI_MAX_ITEMS; ++k)
        origin[k] = f->is_new && k == f->n_items - 1 ? -1 : k;
    ui_errors_t errs;
    ui_save_rc_t rc = ok && f->n_items <= UI_MAX_ITEMS
                          ? save_section(f->sec, f->n_items, v, s->is_array ? origin : NULL, f->gen, &errs)
                          : UI_SAVE_FAILED;
    if (rc == UI_SAVE_FAILED && !(ok && f->n_items <= UI_MAX_ITEMS)) {
        memset(&errs, 0, sizeof errs);
        snprintf(errs.e[0].msg, sizeof errs.e[0].msg, "out of memory");
        errs.n = errs.total = 1;
    }
    vals_free(v, total);
    report(rc, &errs, restart);
    if (rc == UI_SAVE_INVALID) {
        errors_show(f, &errs);
        return;
    }
    if (rc == UI_SAVE_STALE && s->is_array) {
        /* The list may have been reordered: which camera this form edits
         * is no longer known. Back to the list, which shows it now. */
        ui_page_close(f->page);
        return;
    }
    if (rc != UI_SAVE_OK && rc != UI_SAVE_UNSYNCED && rc != UI_SAVE_STALE) return;
    /* What is stored now (secrets redacted again) is the new baseline. */
    int n;
    unsigned gen;
    char **nv = vals_load(f->sec, 0, &n, &gen);
    if (!nv) return;
    vals_free(f->vals, f->n_items * f->n_fields);
    f->vals = nv;
    f->n_items = n;
    f->gen = gen;
    if (f->is_new) {
        f->is_new = 0;
        f->item = n - 1;
        form_title(f);
    }
    widgets_from_vals(f);
}

static void discard_cb(void *user)
{
    form_t *f = user;
    ui_page_close(f->page);
}

static int form_back(ui_page_t *p)
{
    form_t *f = p->user;
    if (f->kb && !lv_obj_has_flag(f->kb, LV_OBJ_FLAG_HIDDEN)) {
        kb_hide(f);
        return 1;
    }
    if (form_dirty(f)) {
        ui_confirm("Discard the changes on this page?", "Discard", discard_cb, f);
        return 1;
    }
    return 0;
}

/* Cancel: like back, but the keyboard does not keep the page open. */
static void cancel_cb(lv_event_t *e)
{
    form_t *f = lv_event_get_user_data(e);
    kb_hide(f);
    if (!form_back(f->page)) ui_page_close(f->page);
}

static void form_close(ui_page_t *p)
{
    form_t *f = p->user;
    vals_free(f->vals, f->n_items * f->n_fields);
    free(f);
}

/* Storage: what the recording does now, from the channels' states
 * ("Recording: 2 cameras. Waiting for disk: 2 cameras."). */
static void storage_state(lv_obj_t *parent)
{
    const char *states[UI_MAX_CHANNELS];
    int counts[UI_MAX_CHANNELS], n = 0;
    for (int ch = 0; ch < ui_model()->channels && ch < UI_MAX_CHANNELS; ++ch) {
        const ui_chan_t *st = ui_channel(ch);
        if (!st->configured || !st->rec_state[0]) continue;
        int k = 0;
        while (k < n && strcmp(states[k], st->rec_state)) ++k;
        if (k == n) {
            states[n] = st->rec_state;
            counts[n++] = 0;
        }
        ++counts[k];
    }
    char text[256] = "";
    for (int k = 0; k < n; ++k) {
        size_t len = strlen(text);
        snprintf(text + len, sizeof text - len, "%s%c%s: %d camera%s.", k ? "  " : "", toupper((unsigned char)states[k][0]),
                 states[k] + 1, counts[k], counts[k] == 1 ? "" : "s");
    }
    lv_obj_t *l = ui_label(parent, "", VG_FONT_MD_LV, UI_C(TEXT));
    lv_label_set_text_fmt(l, LV_SYMBOL_DRIVE "  %s", n ? text : "Not recording: no directory set.");
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
}

void ui_form_open(int sec, int item)
{
    const ui_section_t *s = section(sec);
    if (s->n_fields > FIELDS_MAX) return;
    form_t *f = calloc(1, sizeof *f);
    if (!f) return;
    f->sec = sec;
    f->n_fields = s->n_fields;
    if (s->is_array) {
        int n = ui_model()->item_count(sec);
        f->is_new = item >= n;
        f->item = f->is_new ? n : item;
        if (f->is_new && n >= s->max_items) {
            free(f);
            ui_toast(UI_TOAST_BAD, "At most %d", s->max_items);
            return;
        }
    } else {
        f->item = -1;
    }
    f->vals = vals_load(sec, f->is_new, &f->n_items, &f->gen);
    ui_page_t *p = f->vals ? ui_page_push(s->label) : NULL;
    if (!p) {
        vals_free(f->vals, f->n_items * f->n_fields);
        free(f);
        return;
    }
    f->page = p;
    p->user = f;
    p->on_back = form_back;
    p->on_close = form_close;
    form_title(f);

    lv_obj_t *cancel = ui_btn(p->actions, LV_SYMBOL_CLOSE, "Cancel");
    lv_obj_add_event_cb(cancel, cancel_cb, LV_EVENT_CLICKED, f);
    lv_obj_t *save = ui_btn(p->actions, LV_SYMBOL_SAVE, "Save");
    ui_btn_accent(save);
    lv_obj_add_event_cb(save, save_cb, LV_EVENT_CLICKED, f);
    p->save = save;

    if (s->help && *s->help) {
        lv_obj_t *h = ui_label(p->body, s->help, VG_FONT_SM_LV, UI_C(TEXT2));
        lv_obj_set_width(h, LV_PCT(100));
        lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
    }
    if (!strcmp(s->key, "record")) storage_state(p->body);
    f->general = ui_label(p->body, "", VG_FONT_MD_LV, UI_C(BAD));
    lv_obj_set_width(f->general, LV_PCT(100));
    lv_label_set_long_mode(f->general, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(f->general, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *list = lv_obj_create(p->body);
    ui_plain(list);
    lv_obj_add_style(list, &ui_st_panel, 0);
    lv_obj_set_style_pad_ver(list, VG_SPACE_SM, 0);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < f->n_fields; ++i) field_row(list, f, i);

    f->kb = lv_keyboard_create(p->scr);
    lv_obj_set_size(f->kb, LV_PCT(100), KB_H);
    lv_obj_align(f->kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(f->kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(f->kb, kb_cb, LV_EVENT_READY, f);
    lv_obj_add_event_cb(f->kb, kb_cb, LV_EVENT_CANCEL, f);
    ui_page_ready(p);
}

/* ---- the list of an array section -------------------------------------------------------------- */

typedef struct {
    int        sec;
    ui_page_t *page;
    lv_obj_t  *table, *add;
    int        remove_item;
    unsigned   gen;                 /* the generation the rows show */
} list_t;

static int col_grow(const ui_field_t *fd)
{
    return fd->kind == UI_FIELD_STRING ? (fd->max_len > 64 ? 4 : 2) : 0;
}

static lv_obj_t *table_row(lv_obj_t *table, int header)
{
    lv_obj_t *r = lv_obj_create(table);
    ui_plain(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(r, header ? VG_SPACE_SM : VG_SPACE_MD, 0);
    lv_obj_set_style_pad_hor(r, VG_SPACE_SM, 0);    /* room for the focus ring */
    lv_obj_set_style_pad_column(r, VG_SPACE_LG, 0);
    lv_obj_set_style_border_color(r, UI_C(LINE), 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_layout(r, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return r;
}

static lv_obj_t *cell(lv_obj_t *row, const ui_field_t *fd, const char *text, lv_color_t c)
{
    lv_obj_t *l = ui_label(row, text, VG_FONT_MD_LV, c);
    int grow = fd ? col_grow(fd) : 0;
    if (grow) {
        lv_obj_set_width(l, 1);
        lv_obj_set_flex_grow(l, grow);
    } else {
        lv_obj_set_width(l, fd ? 90 : 60);
    }
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

static void edit_cb(lv_event_t *e)
{
    list_t *l = lv_event_get_user_data(e);
    ui_form_open(l->sec, (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
}

static void add_cb(lv_event_t *e)
{
    list_t *l = lv_event_get_user_data(e);
    ui_form_open(l->sec, ui_model()->item_count(l->sec));
}

static void remove_now(void *user)
{
    list_t *l = user;
    const ui_section_t *s = section(l->sec);
    int n;
    unsigned gen;
    char **v = vals_load(l->sec, 0, &n, &gen);
    if (!v || l->remove_item >= n || n > UI_MAX_ITEMS) {
        vals_free(v, v ? n * s->n_fields : 0);
        return;
    }
    int origin[UI_MAX_ITEMS];
    for (int k = 0; k < n - 1; ++k) origin[k] = k < l->remove_item ? k : k + 1;
    /* Close the gap: every later item moves up a tile. */
    for (int f = 0; f < s->n_fields; ++f) free(v[l->remove_item * s->n_fields + f]);
    memmove(&v[l->remove_item * s->n_fields], &v[(l->remove_item + 1) * s->n_fields],
            sizeof *v * (size_t)((n - l->remove_item - 1) * s->n_fields));
    ui_errors_t errs;
    /* Refused if the list changed after it was shown: the row the user
     * picked may be another camera now. */
    ui_save_rc_t rc = save_section(l->sec, n - 1, v, origin, l->gen, &errs);
    vals_free(v, (n - 1) * s->n_fields);
    report(rc, &errs, 0);
}

static void remove_ask(list_t *l, int item)
{
    l->remove_item = item;
    char q[160];
    snprintf(q, sizeof q, "Remove item %d from %s? It is saved at once.", l->remove_item + 1,
             section(l->sec)->label);
    ui_confirm(q, "Remove", remove_now, l);
}

static void remove_cb(lv_event_t *e)
{
    remove_ask(lv_event_get_user_data(e), (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
}

/* A row is the keyboard's item: Enter (a click) edits, Delete removes. */
static void row_key_cb(lv_event_t *e)
{
    if (lv_event_get_key(e) == LV_KEY_DEL)
        remove_ask(lv_event_get_user_data(e), (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
}

static void list_build(list_t *l)
{
    const ui_section_t *s = section(l->sec);
    const ui_model_t *m = ui_model();
    lv_obj_clean(l->table);
    l->gen = m->config_generation ? m->config_generation() : 0;
    int n = m->item_count(l->sec);
    if (n >= s->max_items) lv_obj_add_state(l->add, LV_STATE_DISABLED);
    else lv_obj_remove_state(l->add, LV_STATE_DISABLED);

    lv_obj_t *r = table_row(l->table, 1);
    ui_inert(r);
    cell(r, NULL, "#", UI_C(TEXT2));
    for (int f = 0; f < s->n_fields; ++f) cell(r, &s->fields[f], s->fields[f].label, UI_C(TEXT2));
    lv_obj_t *pad = lv_obj_create(r);
    ui_plain(pad);
    lv_obj_set_size(pad, 230, 1);

    if (!n) {
        lv_obj_t *e = ui_label(l->table, "Nothing here yet: add one with \"Add\".", VG_FONT_MD_LV,
                               UI_C(TEXT2));
        lv_obj_set_style_pad_ver(e, VG_SPACE_XL, 0);
    }
    char buf[1024], idx[16];
    lv_obj_t *first = NULL;
    for (int k = 0; k < n; ++k) {
        r = table_row(l->table, 0);
        lv_obj_set_user_data(r, (void *)(intptr_t)k);
        lv_obj_add_style(r, &ui_st_focus_in, LV_STATE_FOCUSED);
        lv_obj_add_event_cb(r, edit_cb, LV_EVENT_CLICKED, l);
        lv_obj_add_event_cb(r, row_key_cb, LV_EVENT_KEY, l);
        if (!first) first = r;
        snprintf(idx, sizeof idx, "%d", k + 1);
        cell(r, NULL, idx, UI_C(TEXT));
        for (int f = 0; f < s->n_fields; ++f) {
            const ui_field_t *fd = &s->fields[f];
            m->get_value(l->sec, k, f, buf, sizeof buf);
            if (fd->kind == UI_FIELD_BOOL) {
                int on = !strcmp(buf, "1");
                cell(r, fd, on ? "On" : "Off", on ? UI_C(OK) : UI_C(TEXT2));
            } else {
                cell(r, fd, buf[0] ? buf : "-", buf[0] ? UI_C(TEXT) : UI_C(TEXT2));
            }
        }
        lv_obj_t *ed = ui_btn(r, LV_SYMBOL_EDIT, "Edit");
        lv_obj_set_user_data(ed, (void *)(intptr_t)k);
        lv_obj_add_event_cb(ed, edit_cb, LV_EVENT_CLICKED, l);
        lv_obj_t *rm = ui_btn(r, LV_SYMBOL_TRASH, "Remove");
        lv_obj_set_user_data(rm, (void *)(intptr_t)k);
        lv_obj_add_event_cb(rm, remove_cb, LV_EVENT_CLICKED, l);
    }
    if (first) ui_gridnav_focus(l->table, first);   /* not the header */
}

/* A new configuration (saved here, by the API or by a form above). */
static void list_config_cb(lv_observer_t *o, lv_subject_t *s)
{
    (void)s;
    list_build(lv_observer_get_user_data(o));
}

static void list_close(ui_page_t *p) { free(p->user); }

void ui_list_open(int sec)
{
    const ui_section_t *s = section(sec);
    list_t *l = calloc(1, sizeof *l);
    if (!l) return;
    ui_page_t *p = ui_page_push(s->label);
    if (!p) {
        free(l);
        return;
    }
    l->sec = sec;
    l->page = p;
    p->user = l;
    p->on_close = list_close;
    l->add = ui_btn(p->actions, LV_SYMBOL_PLUS, "Add");
    ui_btn_accent(l->add);
    lv_obj_add_event_cb(l->add, add_cb, LV_EVENT_CLICKED, l);
    if (s->help && *s->help) ui_label(p->body, s->help, VG_FONT_SM_LV, UI_C(TEXT2));
    l->table = lv_obj_create(p->body);
    ui_plain(l->table);
    lv_obj_add_style(l->table, &ui_st_panel, 0);
    lv_obj_set_style_pad_ver(l->table, VG_SPACE_XS, 0);
    lv_obj_set_size(l->table, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(l->table, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(l->table, LV_FLEX_FLOW_COLUMN);
    lv_subject_add_observer_obj(&ui_subj_config, list_config_cb, l->table, l);
    /* Up and Down go through the rows, and past the ends to Back and Add. */
    ui_gridnav(l->table, LV_GRIDNAV_CTRL_NONE);
    if (lv_obj_get_child_count(l->table) > 1 && lv_obj_has_flag(lv_obj_get_child(l->table, 1), LV_OBJ_FLAG_CLICKABLE))
        ui_gridnav_focus(l->table, lv_obj_get_child(l->table, 1));     /* the first row, not the header */
    lv_group_add_obj(p->group, l->table);
    ui_page_ready(p);
}
