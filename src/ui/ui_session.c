/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_session.c — who is using the UI, and Exit.
 *
 * With ui.login off (the default) everyone may do everything, as before.
 * With it on, the live view stays, but the menus and settings ask for a
 * user and its password first (vigil's users, users.h; the system account
 * always logs in with every permission, so nobody is locked out), and
 * each needs its permission (ui_with_perm). After ui.idle_logout minutes
 * without a key or the mouse, or with Exit > Logout, the session ends and
 * the UI goes back to the live view.
 *
 * Exit (the context menu's last item) offers Logout, Reboot and Shutdown;
 * the last two ask first, then vigild stops cleanly (the recordings are
 * closed) and the board reboots or powers off (model->mgmt->power).
 */
#include <stdio.h>
#include <string.h>

#include "ui_app.h"
#include "ui_i18n.h"
#include "ui_osk.h"
#include "ui_theme.h"

#define USERS_SHOWN 33              /* vigil's users and the system account */

static struct {
    int         required;           /* ui.login */
    int         idle_min;           /* ui.idle_logout */
    int         in;                 /* logged in */
    char        user[32];
    unsigned    perms;
    lv_timer_t *idle;
    /* the login dialog */
    void      (*then)(void *);      /* what it was opened for */
    void       *then_arg;
    unsigned    then_perm;
    lv_obj_t   *user_dd, *pass_ta, *err, *osk;
    char        users[USERS_SHOWN][32];
    int         n_users;
} S;

static const ui_mgmt_t *MG(void) { return ui_model()->mgmt; }

void ui_session_config(int required, int idle_min)
{
    if (required && !S.required) S.in = 0;          /* turned on: log in from now */
    S.required = required;
    S.idle_min = idle_min;
}

int ui_login_required(void) { return S.required; }
int ui_logged_in(void) { return S.in; }
const char *ui_session_user(void) { return S.in ? S.user : ""; }

int ui_allowed(unsigned perm)
{
    if (!S.required) return 1;
    return S.in && (S.perms & perm) == perm;
}

static void close_all(void)
{
    ui_popup_close();
    for (ui_page_t *p; (p = ui_page_top()) != NULL;) ui_page_close(p);
}

void ui_logout(int idle)
{
    if (!S.in) return;
    S.in = 0;
    S.perms = 0;
    close_all();
    if (idle) ui_toast(UI_TOAST_OK, "%s (%d min without use)", TR(S_LOGGED_OUT), S.idle_min);
    else ui_toast(UI_TOAST_OK, "%s", TR(S_LOGGED_OUT));
}

static void idle_cb(lv_timer_t *t)
{
    (void)t;
    if (!S.required || !S.in || S.idle_min <= 0) return;
    if (lv_display_get_inactive_time(NULL) >= (uint32_t)S.idle_min * 60000u) ui_logout(1);
}

void ui_session_init(void)
{
    if (!S.idle) S.idle = lv_timer_create(idle_cb, 1000, NULL);
}

/* ---- the login dialog ---------------------------------------------------------------------- */

static void denied(void)
{
    ui_toast(UI_TOAST_BAD, "%s: %s", S.user, TR(S_NO_PERMISSION));
}

static void login_try(void)
{
    char user[32], why[160] = "";
    lv_dropdown_get_selected_str(S.user_dd, user, sizeof user);
    const char *pass = lv_textarea_get_text(S.pass_ta);
    unsigned perms = 0;
    int rc = MG() && MG()->login ? MG()->login(user, pass, &perms, why, sizeof why) : -1;
    if (rc != 1) {
        lv_label_set_text(S.err, rc == 0 ? TR(S_WRONG_LOGIN) : why[0] ? why : TR(S_WRONG_LOGIN));
        lv_obj_remove_flag(S.err, LV_OBJ_FLAG_HIDDEN);
        lv_textarea_set_text(S.pass_ta, "");
        return;
    }
    S.in = 1;
    S.perms = perms;
    snprintf(S.user, sizeof S.user, "%s", user);
    void (*then)(void *) = S.then;
    void *arg = S.then_arg;
    unsigned need = S.then_perm;
    S.then = NULL;
    ui_popup_close();
    if (!then) return;
    if ((perms & need) == need) then(arg);
    else denied();
}

static void login_cb(lv_event_t *e)
{
    (void)e;
    login_try();
}

static void cancel_cb(lv_event_t *e)
{
    (void)e;
    S.then = NULL;
    ui_popup_close();
}

/* Enter in the password: log in. A click on it: the on-screen keyboard,
 * for a mouse user. */
static void pass_ready_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (in) lv_indev_wait_release(in);
    login_try();
}

static void pass_click_cb(lv_event_t *e)
{
    if (!ui_keyboard_in_use() && S.osk) ui_osk_show(S.osk, lv_event_get_target(e), 0);
}

static void osk_done_cb(lv_event_t *e)
{
    ui_osk_hide(lv_event_get_target(e));
    if (lv_event_get_code(e) == LV_EVENT_READY) login_try();
}

/* The users who can log in: the system account first (it always works),
 * then vigil's enabled users. */
static void users_load(void)
{
    char sys[32] = "root";
    if (MG() && MG()->system_user) MG()->system_user(sys, sizeof sys);
    snprintf(S.users[0], sizeof S.users[0], "%.31s", sys);
    S.n_users = 1;
    ui_user_t u[USERS_SHOWN - 1];
    int n = MG() && MG()->users ? MG()->users(u, USERS_SHOWN - 1) : 0;
    for (int i = 0; i < n && S.n_users < USERS_SHOWN; ++i)
        if (u[i].enabled && strcmp(u[i].name, sys)) {
            int k = S.n_users++;
            snprintf(S.users[k], sizeof S.users[k], "%.31s", u[i].name);
        }
}

void ui_login_open(void (*then)(void *), void *arg, unsigned perm)
{
    users_load();
    lv_obj_t *panel = ui_popup_open(NULL, 440);
    lv_obj_set_style_pad_all(panel, VG_SPACE_XL, 0);
    lv_obj_set_style_pad_row(panel, VG_SPACE_MD, 0);
    lv_obj_set_style_border_color(panel, UI_C(ACCENT), 0);
    ui_label(panel, TR(S_LOG_IN), VG_FONT_LG_LV, UI_C(TEXT));

    ui_label(panel, TR(S_USER), VG_FONT_SM_LV, UI_C(TEXT2));
    S.user_dd = lv_dropdown_create(panel);
    lv_obj_set_width(S.user_dd, LV_PCT(100));
    char opts[USERS_SHOWN * 33] = "";
    for (int i = 0; i < S.n_users; ++i) {
        size_t l = strlen(opts);
        snprintf(opts + l, sizeof opts - l, "%s%s", i ? "\n" : "", S.users[i]);
    }
    lv_dropdown_set_options(S.user_dd, opts);
    ui_focus_style(S.user_dd);

    ui_label(panel, TR(S_PASSWORD), VG_FONT_SM_LV, UI_C(TEXT2));
    S.pass_ta = lv_textarea_create(panel);
    lv_textarea_set_one_line(S.pass_ta, 1);
    lv_textarea_set_password_mode(S.pass_ta, 1);
    lv_obj_set_width(S.pass_ta, LV_PCT(100));
    lv_obj_add_event_cb(S.pass_ta, pass_ready_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(S.pass_ta, pass_click_cb, LV_EVENT_CLICKED, NULL);
    ui_focus_style(S.pass_ta);

    S.err = ui_label(panel, "", VG_FONT_SM_LV, UI_C(BAD));
    lv_obj_add_flag(S.err, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *row = lv_obj_create(panel);
    ui_plain(row);
    ui_ring_room(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, VG_SPACE_MD, 0);
    lv_obj_t *no = ui_btn(row, NULL, TR(S_CANCEL));
    lv_obj_add_event_cb(no, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok = ui_btn(row, NULL, TR(S_LOG_IN));
    ui_btn_accent(ok);
    lv_obj_add_event_cb(ok, login_cb, LV_EVENT_CLICKED, NULL);

    /* The keyboard's order: the user, the password, the buttons. */
    ui_popup_focus_add(S.user_dd);
    ui_popup_focus_add(S.pass_ta);
    ui_popup_focus(row, ok, LV_GRIDNAV_CTRL_ROLLOVER);
    ui_popup_focus_obj(S.pass_ta);

    /* The on-screen keyboard over the popup, gone with it. */
    S.osk = ui_osk_create(lv_obj_get_parent(panel));
    if (S.osk) {
        lv_obj_add_event_cb(S.osk, osk_done_cb, LV_EVENT_READY, NULL);
        lv_obj_add_event_cb(S.osk, osk_done_cb, LV_EVENT_CANCEL, NULL);
    }
    S.then = then;
    S.then_arg = arg;
    S.then_perm = perm;
    ui_popup_place(NULL);
}

void ui_with_perm(unsigned perm, void (*fn)(void *), void *arg)
{
    if (ui_allowed(perm)) {
        fn(arg);
        return;
    }
    if (S.in) {                                     /* logged in, without this permission */
        denied();
        return;
    }
    ui_login_open(fn, arg, perm);
}

/* ---- Exit ------------------------------------------------------------------------------------ */

static int g_power_reboot;

static void power_now(void *u)
{
    (void)u;
    int rc = MG() && MG()->power ? MG()->power(g_power_reboot) : -1;
    if (rc < 0) ui_toast(UI_TOAST_BAD, "%s", TR(S_POWER_UNMANAGED));
    else ui_toast(UI_TOAST_WARN, "%s", g_power_reboot ? TR(S_REBOOTING) : TR(S_SHUTTING_DOWN));
}

static void power_ask(void *u)
{
    (void)u;
    if (g_power_reboot) ui_confirm(TR(S_REBOOT_Q), TR(S_REBOOT), power_now, NULL);
    else ui_confirm(TR(S_SHUTDOWN_Q), TR(S_SHUTDOWN), power_now, NULL);
}

static void exit_cb(lv_event_t *e)
{
    int what = (int)(intptr_t)lv_event_get_user_data(e);
    ui_popup_close();
    if (what == 0) {
        ui_logout(0);
        return;
    }
    if (what == 3) return;
    g_power_reboot = what == 1;
    ui_with_perm(UI_PERM_SYSTEM, power_ask, NULL);
}

static lv_obj_t *exit_item(lv_obj_t *panel, const char *sym, const char *text, int what)
{
    lv_obj_t *b = ui_btn(panel, sym, text);
    lv_obj_set_width(b, LV_PCT(100));
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(b, exit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)what);
    return b;
}

void ui_exit_open(void)
{
    lv_obj_t *panel = ui_popup_open(NULL, 320);
    lv_obj_set_style_pad_all(panel, VG_SPACE_LG, 0);
    lv_obj_set_style_border_color(panel, UI_C(ACCENT), 0);
    lv_obj_t *t = ui_label(panel, TR(S_EXIT), VG_FONT_LG_LV, UI_C(TEXT));
    lv_obj_set_style_pad_bottom(t, VG_SPACE_SM, 0);
    lv_obj_t *items = lv_obj_create(panel);
    ui_plain(items);
    ui_ring_room(items);
    lv_obj_set_size(items, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(items, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(items, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(items, VG_SPACE_XS, 0);
    lv_obj_t *out = exit_item(items, LV_SYMBOL_LEFT, TR(S_LOGOUT), 0);
    if (!S.required || !S.in) {
        /* No session to end: the on-screen login is off, or nobody is in. */
        lv_obj_add_state(out, LV_STATE_DISABLED);
        ui_inert(out);
    }
    lv_obj_t *first = exit_item(items, LV_SYMBOL_REFRESH, TR(S_REBOOT), 1);
    exit_item(items, LV_SYMBOL_POWER, TR(S_SHUTDOWN), 2);
    exit_item(items, LV_SYMBOL_CLOSE, TR(S_CANCEL), 3);
    if (!S.required) {
        lv_obj_t *n = ui_label(panel, TR(S_LOGIN_OFF), VG_FONT_SM_LV, UI_C(TEXT2));
        lv_obj_set_width(n, LV_PCT(100));
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    }
    ui_popup_focus(items, S.in && S.required ? out : first, LV_GRIDNAV_CTRL_ROLLOVER);
    ui_popup_place(NULL);
}
