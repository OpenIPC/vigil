/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_set_account.c — Account: Users and Groups (vigil's own accounts,
 * docs/api.md "Accounts and permissions").
 *
 * Users act at once (add, change, remove: each is one write of the
 * accounts file), so the page has no Save; the editor for one user opens
 * above the list, in the pane, where the on-screen keyboard can reach
 * its fields. Groups are a grid of permissions saved together. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "ui_i18n.h"
#include "ui_settings.h"
#include "ui_theme.h"

#define UI_USERS_MAX  32
#define UI_GROUPS_MAX 16
#define POLL_MS       1000

static const ui_mgmt_t *MG(void) { return ui_model()->mgmt; }

static int have_accounts(void)
{
    return MG() && MG()->users && MG()->groups && MG()->user_add && MG()->group_set;
}

static void no_accounts(ui_set_t *s, lv_obj_t *pane)
{
    ui_set_hide_actions(s);
    lv_obj_t *sec = ui_set_section(pane, NULL);
    ui_label(sec, "Accounts need vigild built with the HTTP API", VG_FONT_LG_LV, UI_C(TEXT));
    ui_set_note(sec, "This vigild was built without it (-DVIGIL_API=OFF), and with it its accounts file.",
                UI_C(TEXT2));
}

/* Permission names as people read them. */
static const char *perm_label(const char *name)
{
    static const struct { const char *k, *v; } m[] = {
        { "live", "Live" }, { "playback", "Playback" }, { "ptz", "PTZ" }, { "cameras", "Cameras" },
        { "storage", "Storage" }, { "network", "Network" }, { "system", "System" }, { "users", "Users" },
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; ++i)
        if (!strcmp(m[i].k, name)) return m[i].v;
    return name;
}

static lv_obj_t *cell(lv_obj_t *row, const char *text, int w, lv_color_t c)
{
    lv_obj_t *l = ui_label(row, text, VG_FONT_MD_LV, c);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    return l;
}

static lv_obj_t *trow(lv_obj_t *parent, int header)
{
    lv_obj_t *r = lv_obj_create(parent);
    ui_plain(r);
    ui_ring_room(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, 40, 0);
    lv_obj_set_style_pad_ver(r, header ? VG_SPACE_XS : 2, 0);
    lv_obj_set_style_border_color(r, UI_C(LINE), 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_layout(r, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, VG_SPACE_MD, 0);
    return r;
}

/* ---- Users ------------------------------------------------------------------------- */

typedef struct {
    ui_set_t  *s;
    unsigned   gen;
    lv_obj_t  *list, *editor;
    /* the editor */
    int        adding;
    char       name[32];
    lv_obj_t  *name_ta, *group_dd, *en_sw, *pass, *pass2, *err[3], *title;
    char       groups[UI_GROUPS_MAX][32];
    int        n_groups;
    ui_user_t  users[UI_USERS_MAX];
    int        n_users;
    int        remove_i;
    lv_timer_t *timer;
} users_t;

static void users_list(users_t *u);

static void editor_close(users_t *u)
{
    lv_obj_clean(u->editor);
    lv_obj_add_flag(u->editor, LV_OBJ_FLAG_HIDDEN);
    u->name_ta = NULL;
}

static void editor_err(users_t *u, const char *field, const char *msg)
{
    int i = !strcmp(field, "name") ? 0 : !strcmp(field, "group") ? 1 : 2;
    lv_obj_t *w = i == 0 ? u->name_ta : i == 1 ? u->group_dd : u->pass;
    if (w) lv_obj_add_state(w, LV_STATE_USER_1);
    lv_label_set_text_fmt(u->err[i], LV_SYMBOL_CLOSE "  %s", msg);
    lv_obj_remove_flag(u->err[i], LV_OBJ_FLAG_HIDDEN);
}

static void editor_save_cb(lv_event_t *e)
{
    users_t *u = lv_event_get_user_data(e);
    for (int i = 0; i < 3; ++i) lv_obj_add_flag(u->err[i], LV_OBJ_FLAG_HIDDEN);
    if (u->name_ta) lv_obj_remove_state(u->name_ta, LV_STATE_USER_1);
    lv_obj_remove_state(u->pass, LV_STATE_USER_1);
    const char *p1 = lv_textarea_get_text(u->pass), *p2 = lv_textarea_get_text(u->pass2);
    if (strcmp(p1, p2)) {
        editor_err(u, "password", "the two passwords differ");
        return;
    }
    if (u->adding && !*p1) {
        editor_err(u, "password", "a new user needs a password");
        return;
    }
    char group[32], field[16] = "", err[160] = "";
    lv_dropdown_get_selected_str(u->group_dd, group, sizeof group);
    int en = lv_obj_has_state(u->en_sw, LV_STATE_CHECKED);
    int rc;
    if (u->adding)
        rc = MG()->user_add(lv_textarea_get_text(u->name_ta), group, p1, en, field, sizeof field, err, sizeof err);
    else
        rc = MG()->user_update(u->name, group, en, *p1 ? p1 : NULL, field, sizeof field, err, sizeof err);
    if (rc < 0) {
        editor_err(u, field[0] ? field : "name", err);
        ui_toast(UI_TOAST_BAD, "Not saved: %s", err);
        return;
    }
    ui_toast(UI_TOAST_OK, u->adding ? "User %s added" : "User %s changed",
             u->adding ? lv_textarea_get_text(u->name_ta) : u->name);
    editor_close(u);
    users_list(u);
}

static void editor_cancel_cb(lv_event_t *e) { editor_close(lv_event_get_user_data(e)); }

/* Open the editor for user `i` (-1: a new one). */
static void editor_open(users_t *u, int i)
{
    editor_close(u);
    lv_obj_remove_flag(u->editor, LV_OBJ_FLAG_HIDDEN);
    u->adding = i < 0;
    const ui_user_t *x = i >= 0 ? &u->users[i] : NULL;
    snprintf(u->name, sizeof u->name, "%s", x ? x->name : "");
    ui_set_t *s = u->s;
    lv_obj_t *sec = ui_set_section(u->editor, x ? "Change the user" : "A new user");
    lv_obj_t *box = ui_set_row(sec, "Name", x ? NULL : "Letters, digits, '.', '_' and '-'; not root.");
    if (x) {
        ui_label(box, x->name, VG_FONT_MD_LV, UI_C(TEXT));
    } else {
        u->name_ta = ui_set_text(s, box, "", 31, 320);
    }
    u->err[0] = ui_set_note(lv_obj_get_parent(box), "", UI_C(BAD));
    char o[UI_GROUPS_MAX * 34] = "";
    int sel = 0;
    for (int k = 0; k < u->n_groups; ++k) {
        size_t l = strlen(o);
        snprintf(o + l, sizeof o - l, "%s%s", k ? "\n" : "", u->groups[k]);
        if (!strcmp(u->groups[k], x ? x->group : "user")) sel = k;
    }
    box = ui_set_row(sec, "Group", "What the user may do: Account > Groups.");
    u->group_dd = ui_set_dropdown(s, box, o, sel, 240);
    u->err[1] = ui_set_note(lv_obj_get_parent(box), "", UI_C(BAD));
    u->en_sw = ui_set_switch(s, ui_set_row(sec, "Enabled", "A disabled user cannot log in."), x ? x->enabled : 1);
    box = ui_set_row(sec, "Password", x ? "Empty: unchanged." : "At least four characters.");
    u->pass = ui_set_password(s, box, 128, 320);
    u->err[2] = ui_set_note(lv_obj_get_parent(box), "", UI_C(BAD));
    u->pass2 = ui_set_password(s, ui_set_row(sec, "Repeat it", NULL), 128, 320);
    for (int k = 0; k < 3; ++k) lv_obj_add_flag(u->err[k], LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *row = ui_set_hbox(u->editor);
    lv_obj_t *cancel = ui_set_button(s, row, LV_SYMBOL_CLOSE, TR(S_CANCEL));
    lv_obj_add_event_cb(cancel, editor_cancel_cb, LV_EVENT_CLICKED, u);
    lv_obj_t *ok = ui_set_button(s, row, LV_SYMBOL_OK, x ? "Save user" : "Add user");
    ui_btn_accent(ok);
    lv_obj_add_event_cb(ok, editor_save_cb, LV_EVENT_CLICKED, u);
    if (u->name_ta) lv_group_focus_obj(u->name_ta);
    lv_obj_scroll_to_y(lv_obj_get_parent(u->editor), 0, LV_ANIM_OFF);
}

static void add_cb(lv_event_t *e) { editor_open(lv_event_get_user_data(e), -1); }

static void edit_cb(lv_event_t *e)
{
    users_t *u = lv_event_get_user_data(e);
    editor_open(u, (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
}

static void remove_yes(void *user)
{
    users_t *u = user;
    if (u->remove_i < 0 || u->remove_i >= u->n_users) return;
    char err[160] = "";
    if (MG()->user_delete(u->users[u->remove_i].name, err, sizeof err) < 0)
        ui_toast(UI_TOAST_BAD, "Not removed: %s", err);
    else ui_toast(UI_TOAST_OK, "User %s removed", u->users[u->remove_i].name);
    editor_close(u);
    users_list(u);
}

static void remove_cb(lv_event_t *e)
{
    users_t *u = lv_event_get_user_data(e);
    u->remove_i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    char q[96];
    snprintf(q, sizeof q, "Remove the user %s?", u->users[u->remove_i].name);
    ui_confirm(q, TR(S_REMOVE), remove_yes, u);
}

static void users_list(users_t *u)
{
    u->gen = MG()->users_gen ? MG()->users_gen() : 0;
    ui_group_t g[UI_GROUPS_MAX];
    u->n_groups = MG()->groups(g, UI_GROUPS_MAX);
    for (int i = 0; i < u->n_groups; ++i) snprintf(u->groups[i], sizeof u->groups[i], "%.31s", g[i].name);
    u->n_users = MG()->users(u->users, UI_USERS_MAX);
    lv_obj_clean(u->list);
    lv_obj_t *h = trow(u->list, 1);
    cell(h, "Name", 220, UI_C(TEXT2));
    cell(h, "Group", 180, UI_C(TEXT2));
    cell(h, "Enabled", 110, UI_C(TEXT2));
    cell(h, "Password", 120, UI_C(TEXT2));
    if (!u->n_users) ui_set_note(u->list, "No users yet: only the system account logs in.", UI_C(TEXT2));
    for (int i = 0; i < u->n_users; ++i) {
        const ui_user_t *x = &u->users[i];
        lv_obj_t *r = trow(u->list, 0);
        cell(r, x->name, 220, UI_C(TEXT));
        cell(r, x->group, 180, UI_C(TEXT));
        cell(r, x->enabled ? "yes" : "no", 110, x->enabled ? UI_C(OK) : UI_C(WARN));
        cell(r, x->password_set ? "set" : "none", 120, UI_C(TEXT2));
        lv_obj_t *ed = ui_set_button(u->s, r, LV_SYMBOL_EDIT, TR(S_EDIT));
        lv_obj_set_user_data(ed, (void *)(intptr_t)i);
        lv_obj_add_event_cb(ed, edit_cb, LV_EVENT_CLICKED, u);
        lv_obj_t *rm = ui_set_button(u->s, r, LV_SYMBOL_TRASH, TR(S_REMOVE));
        lv_obj_set_user_data(rm, (void *)(intptr_t)i);
        lv_obj_add_event_cb(rm, remove_cb, LV_EVENT_CLICKED, u);
    }
}

/* Changed elsewhere (the HTTP API): the list again, unless the editor is
 * open. */
static void users_poll(lv_timer_t *t)
{
    users_t *u = lv_timer_get_user_data(t);
    if (!MG()->users_gen || lv_obj_has_flag(u->editor, LV_OBJ_FLAG_HIDDEN) == 0) return;
    if (MG()->users_gen() != u->gen) users_list(u);
}

static void *users_open(ui_set_t *s, lv_obj_t *pane)
{
    if (!have_accounts()) {
        no_accounts(s, pane);
        return NULL;
    }
    ui_set_hide_actions(s);
    users_t *u = calloc(1, sizeof *u);
    if (!u) return NULL;
    u->s = s;
    u->remove_i = -1;
    ui_set_note(pane, "The system account (root) always logs in to the HTTP API with every permission, and so "
                      "does 'admin' until a user of that name exists. The on-screen UI does not ask for a login "
                      "yet; these accounts are enforced on the HTTP API.", UI_C(TEXT2));
    lv_obj_t *add = ui_set_button(s, ui_set_hbox(pane), LV_SYMBOL_PLUS, "Add user");
    lv_obj_add_event_cb(add, add_cb, LV_EVENT_CLICKED, u);
    u->editor = lv_obj_create(pane);
    ui_plain(u->editor);
    ui_ring_room(u->editor);
    lv_obj_set_size(u->editor, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(u->editor, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(u->editor, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(u->editor, VG_SPACE_MD, 0);
    lv_obj_add_flag(u->editor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *sec = ui_set_section(pane, TR(S_USERS));
    u->list = lv_obj_create(sec);
    ui_plain(u->list);
    ui_ring_room(u->list);
    lv_obj_set_size(u->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(u->list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(u->list, LV_FLEX_FLOW_COLUMN);
    users_list(u);
    u->timer = lv_timer_create(users_poll, POLL_MS, u);
    return u;
}

static void users_close(void *st)
{
    users_t *u = st;
    if (!u) return;
    lv_timer_delete(u->timer);
    free(u);
}

/* ---- Groups ------------------------------------------------------------------------ */

typedef struct {
    ui_set_t   *s;
    ui_group_t  g[UI_GROUPS_MAX];
    int         n;
    lv_obj_t   *list, *new_ta;
    lv_obj_t   *box[UI_GROUPS_MAX][32];
    int         remove_i;
} groups_t;

static unsigned row_perms(groups_t *g, int i)
{
    unsigned p = 0;
    for (int b = 0; b < MG()->n_perms && b < 32; ++b)
        if (g->box[i][b] && lv_obj_has_state(g->box[i][b], LV_STATE_CHECKED)) p |= 1u << b;
    return p;
}

static void tick_text(lv_obj_t *b)
{
    lv_label_set_text(ui_btn_label(b), lv_obj_has_state(b, LV_STATE_CHECKED) ? LV_SYMBOL_OK : " ");
}

static void tick_cb(lv_event_t *e) { tick_text(lv_event_get_target(e)); }

static void groups_list(groups_t *g);

static void group_remove_yes(void *user)
{
    groups_t *g = user;
    if (g->remove_i < 0 || g->remove_i >= g->n) return;
    char err[160] = "";
    if (MG()->group_delete(g->g[g->remove_i].name, err, sizeof err) < 0)
        ui_toast(UI_TOAST_BAD, "Not removed: %s", err);
    else ui_toast(UI_TOAST_OK, "Group %s removed", g->g[g->remove_i].name);
    groups_list(g);
}

static void group_remove_cb(lv_event_t *e)
{
    groups_t *g = lv_event_get_user_data(e);
    g->remove_i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    char q[96];
    snprintf(q, sizeof q, "Remove the group %s?", g->g[g->remove_i].name);
    ui_confirm(q, TR(S_REMOVE), group_remove_yes, g);
}

static void group_add_cb(lv_event_t *e)
{
    groups_t *g = lv_event_get_user_data(e);
    char field[16] = "", err[160] = "";
    const char *name = lv_textarea_get_text(g->new_ta);
    lv_obj_remove_state(g->new_ta, LV_STATE_USER_1);
    if (MG()->group_set(name, 0, 1, field, sizeof field, err, sizeof err) < 0) {
        lv_obj_add_state(g->new_ta, LV_STATE_USER_1);
        ui_toast(UI_TOAST_BAD, "Not added: %s", err);
        return;
    }
    ui_toast(UI_TOAST_OK, "Group %s added: tick what it may do, then Save", name);
    groups_list(g);
}

static void groups_list(groups_t *g)
{
    g->n = MG()->groups(g->g, UI_GROUPS_MAX);
    memset(g->box, 0, sizeof g->box);
    lv_obj_clean(g->list);
    int np = MG()->n_perms;
    lv_obj_t *h = trow(g->list, 1);
    cell(h, "Group", 130, UI_C(TEXT2));
    cell(h, "Users", 56, UI_C(TEXT2));
    for (int b = 0; b < np; ++b) {
        lv_obj_t *l = cell(h, perm_label(MG()->perm_names[b]), 70, UI_C(TEXT2));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, VG_FONT_SM_LV, 0);
    }
    for (int i = 0; i < g->n; ++i) {
        const ui_group_t *x = &g->g[i];
        lv_obj_t *r = trow(g->list, 0);
        cell(r, x->name, 130, UI_C(TEXT));
        char m[16];
        snprintf(m, sizeof m, "%d", x->members);
        cell(r, m, 56, UI_C(TEXT2));
        for (int b = 0; b < np && b < 32; ++b) {
            lv_obj_t *t = ui_btn(r, NULL, " ");
            lv_obj_set_size(t, 70, 32);
            lv_obj_add_flag(t, LV_OBJ_FLAG_CHECKABLE);
            if (x->perms & (1u << b)) lv_obj_add_state(t, LV_STATE_CHECKED);
            if (!x->editable) lv_obj_add_state(t, LV_STATE_DISABLED);
            else lv_group_add_obj(ui_set_group(g->s), t);
            tick_text(t);
            lv_obj_add_event_cb(t, tick_cb, LV_EVENT_VALUE_CHANGED, NULL);
            g->box[i][b] = t;
        }
        if (!x->builtin) {
            lv_obj_t *rm = ui_set_button(g->s, r, LV_SYMBOL_TRASH, NULL);
            lv_obj_set_user_data(rm, (void *)(intptr_t)i);
            lv_obj_add_event_cb(rm, group_remove_cb, LV_EVENT_CLICKED, g);
        } else {
            ui_label(r, "built-in", VG_FONT_SM_LV, UI_C(TEXT2));
        }
    }
}

static void *groups_open(ui_set_t *s, lv_obj_t *pane)
{
    if (!have_accounts()) {
        no_accounts(s, pane);
        return NULL;
    }
    groups_t *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->s = s;
    g->remove_i = -1;
    ui_set_note(pane, "A group is what its users may do. admin may do everything and cannot be changed; user "
                      "cannot be removed.", UI_C(TEXT2));
    lv_obj_t *sec = ui_set_section(pane, TR(S_GROUPS));
    g->list = lv_obj_create(sec);
    ui_plain(g->list);
    ui_ring_room(g->list);
    lv_obj_set_size(g->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(g->list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(g->list, LV_FLEX_FLOW_COLUMN);
    groups_list(g);
    lv_obj_t *box = ui_set_row(ui_set_section(pane, "Add a group"), "Name", NULL);
    g->new_ta = ui_set_text(s, box, "", 31, 240);
    lv_obj_t *add = ui_set_button(s, box, LV_SYMBOL_PLUS, TR(S_ADD));
    lv_obj_add_event_cb(add, group_add_cb, LV_EVENT_CLICKED, g);
    return g;
}

static int groups_dirty(void *st)
{
    groups_t *g = st;
    if (!g) return 0;
    for (int i = 0; i < g->n; ++i)
        if (g->g[i].editable && row_perms(g, i) != g->g[i].perms) return 1;
    return 0;
}

static int groups_save(void *st)
{
    groups_t *g = st;
    if (!g) return 0;
    int failed = 0, saved = 0;
    for (int i = 0; i < g->n; ++i) {
        unsigned p = row_perms(g, i);
        if (!g->g[i].editable || p == g->g[i].perms) continue;
        char field[16] = "", err[160] = "";
        if (MG()->group_set(g->g[i].name, p, 0, field, sizeof field, err, sizeof err) < 0) {
            ui_toast(UI_TOAST_BAD, "%s not saved: %s", g->g[i].name, err);
            failed = 1;
        } else {
            ++saved;
        }
    }
    if (!failed) ui_toast(UI_TOAST_OK, "%s", TR(S_SAVED));
    groups_list(g);
    return failed ? -1 : 0;
}

static void groups_cancel(void *st)
{
    if (st) groups_list(st);
}

/* ---- the screen ------------------------------------------------------------------------ */

static ui_set_item_t g_items[2];

void ui_account_open(int first)
{
    const ui_set_item_t items[] = {
        { TR(S_USERS), &ui_icon_nav_users, users_open, NULL, NULL, NULL, users_close },
        { TR(S_GROUPS), &ui_icon_nav_groups, groups_open, groups_dirty, groups_save, groups_cancel, free },
    };
    memcpy(g_items, items, sizeof items);
    ui_settings_open(TR(S_ACCOUNT), g_items, 2, first);
}
