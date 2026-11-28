/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_set_backup.c — Operation -> Configuration: the recorder's settings
 * into one file and back (docs/management.md, "Configuration"), and the
 * two factory resets.
 *
 *   Export          onto a mounted disk; the passwords sealed with one
 *                   typed here, or left out
 *   Import          a file found on the disks: Check says what changes
 *                   (and refuses a wrong password, a changed file, a newer
 *                   vigil), Import applies it; a reboot for the network
 *   Factory reset   vigil's settings (and the network), or the whole
 *                   firmware; each with a word typed to confirm
 *
 * The work is the model's (cfg_*), on a thread of its own; the pages poll
 * for the outcome. Needs the system and users permissions. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "ui_i18n.h"
#include "ui_settings.h"
#include "ui_theme.h"

#define POLL_MS     250
#define PLACES_MAX  8
#define FILES_MAX   16
#define WORD_VIGIL  "RESET"
#define WORD_FIRM   "FACTORY"

static const ui_mgmt_t *MG(void) { return ui_model()->mgmt; }

static int have(void) { return MG() && MG()->cfg_export && MG()->cfg_import && MG()->cfg_poll; }

static int managed(void) { return MG() && MG()->managed && MG()->managed(); }

static void unavailable(ui_set_t *s, lv_obj_t *pane)
{
    ui_set_hide_actions(s);
    lv_obj_t *sec = ui_set_section(pane, NULL);
    ui_label(sec, "The configuration's export needs vigild built with the HTTP API", VG_FONT_LG_LV, UI_C(TEXT));
    ui_set_note(sec, "This vigild was built without it (-DVIGIL_API=OFF), and with it its accounts.", UI_C(TEXT2));
}

static void enable(lv_obj_t *o, int on)
{
    if (on) lv_obj_remove_state(o, LV_STATE_DISABLED);
    else lv_obj_add_state(o, LV_STATE_DISABLED);
}

static void show(lv_obj_t *o, int on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static lv_obj_t *danger_button(ui_set_t *s, lv_obj_t *box, const char *sym, const char *text)
{
    lv_obj_t *b = ui_set_button(s, box, sym, text);
    lv_obj_set_style_bg_color(b, UI_C(BAD), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    return b;
}

/* A job's outcome: polled until it comes. */
typedef struct {
    lv_timer_t *timer;
    int         waiting;
    void (*done)(void *st, const ui_cfg_result_t *r);
    void       *st;
} poll_t;

static void poll_cb(lv_timer_t *t)
{
    poll_t *p = lv_timer_get_user_data(t);
    if (!p->waiting) return;
    ui_cfg_result_t *r = malloc(sizeof *r);
    if (!r) return;
    if (MG()->cfg_poll(r) == 0) {
        p->waiting = 0;
        p->done(p->st, r);
    }
    free(r);
}

static void poll_start(poll_t *p, void (*done)(void *, const ui_cfg_result_t *), void *st)
{
    p->done = done;
    p->st = st;
    p->timer = lv_timer_create(poll_cb, POLL_MS, p);
}

static const char *rc_words(int rc)
{
    switch (rc) {
    case UI_CFG_EPASSWORD:  return "Password";
    case UI_CFG_ECHANGED:   return "Changed file";
    case UI_CFG_EVERSION:   return "Newer vigil";
    case UI_CFG_EBUSY:      return "Busy";
    case UI_CFG_EUNMANAGED: return "Not managed";
    default:                return "Not done";
    }
}

/* ---- Export ---------------------------------------------------------------------- */

typedef struct {
    ui_set_t      *s;
    poll_t         poll;
    ui_cfg_place_t pl[PLACES_MAX];
    int            n;
    lv_obj_t      *dest, *mode, *pw_sec, *pw, *pw2, *go, *status;
} exp_t;

static void exp_mode_cb(lv_event_t *e)
{
    exp_t *x = lv_event_get_user_data(e);
    show(x->pw_sec, lv_dropdown_get_selected(x->mode) == 0);
}

static void exp_done(void *st, const ui_cfg_result_t *r)
{
    exp_t *x = st;
    enable(x->go, x->n > 0);
    lv_label_set_text_fmt(x->status, "%s  %s", r->rc == UI_CFG_OK ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, r->msg);
    lv_obj_set_style_text_color(x->status, r->rc == UI_CFG_OK ? UI_C(OK) : UI_C(BAD), 0);
    if (r->rc == UI_CFG_OK) ui_toast(UI_TOAST_OK, "Configuration exported");
    else ui_toast(UI_TOAST_BAD, "Not exported: %s", r->msg);
}

static void exp_go_cb(lv_event_t *e)
{
    exp_t *x = lv_event_get_user_data(e);
    int with = lv_dropdown_get_selected(x->mode) == 0;
    const char *p1 = lv_textarea_get_text(x->pw), *p2 = lv_textarea_get_text(x->pw2);
    int i = (int)lv_dropdown_get_selected(x->dest);
    if (i < 0 || i >= x->n) return;
    if (with && strlen(p1) < 4) {
        ui_toast(UI_TOAST_BAD, "The export's password needs at least 4 characters");
        return;
    }
    if (with && strcmp(p1, p2)) {
        ui_toast(UI_TOAST_BAD, "The two passwords differ");
        return;
    }
    char err[200] = "";
    if (MG()->cfg_export(x->pl[i].path, with ? p1 : NULL, err, sizeof err) < 0) {
        ui_toast(UI_TOAST_BAD, "Not exported: %s", err);
        return;
    }
    lv_textarea_set_text(x->pw, "");
    lv_textarea_set_text(x->pw2, "");
    enable(x->go, 0);
    lv_label_set_text(x->status, LV_SYMBOL_REFRESH "  Exporting...");
    lv_obj_set_style_text_color(x->status, UI_C(TEXT2), 0);
    x->poll.waiting = 1;
}

static void *exp_open(ui_set_t *s, lv_obj_t *pane)
{
    if (!have()) {
        unavailable(s, pane);
        return NULL;
    }
    ui_set_hide_actions(s);
    exp_t *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->s = s;
    lv_obj_t *sec = ui_set_section(pane, "Export the configuration");
    ui_set_note(sec, "One file (vigil-config-<name>-<date>.tar) with vigil's settings (the cameras, recording, "
                     "the services, the screen), its users and groups, and what vigil sets on the system: the "
                     "host name, the time zone and NTP servers, the Ethernet settings and the Wi-Fi network.",
                UI_C(TEXT));
    ui_set_note(sec, "Not in it: recordings, logs, the recorder's own key (vigil.key), SSH keys, which "
                     "recorder owns the disks. No password is ever in it in the clear.", UI_C(TEXT2));
    x->n = MG()->cfg_places ? MG()->cfg_places(x->pl, PLACES_MAX) : 0;
    char opts[PLACES_MAX * 132] = "";
    for (int i = 0; i < x->n; ++i) {
        size_t l = strlen(opts);
        snprintf(opts + l, sizeof opts - l, "%s%s  (%s)", i ? "\n" : "", x->pl[i].path, x->pl[i].label);
    }
    lv_obj_t *box = ui_set_row(sec, "Save to", "A USB stick or disk, mounted. From a computer: the HTTP API, "
                                               "POST /api/v1/system/config/export.");
    if (x->n) {
        x->dest = ui_set_dropdown(s, box, opts, 0, 600);
    } else {
        ui_set_note(box, LV_SYMBOL_WARNING "  No disk to write to: plug in a USB stick (Storage > Disk Manager "
                                           "mounts it).", UI_C(WARN));
    }
    box = ui_set_row(sec, "Passwords", "Included: the cameras', the services', the users' and root's, sealed "
                                       "with the password below; the import asks for it. Left out: they are "
                                       "entered again after an import.");
    x->mode = ui_set_dropdown(s, box, "Included, protected by a password\nLeft out", 0, 380);
    lv_obj_add_event_cb(x->mode, exp_mode_cb, LV_EVENT_VALUE_CHANGED, x);
    x->pw_sec = ui_set_section(pane, NULL);
    x->pw = ui_set_password(s, ui_set_row(x->pw_sec, "Export password", "At least four characters. Without it "
                                                                         "the passwords in the file cannot be "
                                                                         "opened, here or anywhere."),
                            128, 320);
    x->pw2 = ui_set_password(s, ui_set_row(x->pw_sec, "Repeat it", NULL), 128, 320);
    lv_obj_t *row = ui_set_hbox(pane);
    x->go = ui_set_button(s, row, LV_SYMBOL_SAVE, "Export now");
    ui_btn_accent(x->go);
    lv_obj_add_event_cb(x->go, exp_go_cb, LV_EVENT_CLICKED, x);
    enable(x->go, x->n > 0);
    x->status = ui_set_note(pane, "", UI_C(TEXT2));
    poll_start(&x->poll, exp_done, x);
    return x;
}

static void exp_close(void *st)
{
    exp_t *x = st;
    if (!x) return;
    lv_timer_delete(x->poll.timer);
    free(x);
}

/* ---- Import ---------------------------------------------------------------------- */

typedef struct {
    ui_set_t     *s;
    poll_t        poll;
    ui_cfg_file_t f[FILES_MAX];
    int           n, applying, checked;     /* checked: the file and password the summary is for */
    char          checked_path[256];
    lv_obj_t     *file, *pw, *host, *check, *apply, *status, *sum_box, *summary;
} imp_t;

static void imp_unchecked(imp_t *m)
{
    m->checked = 0;
    enable(m->apply, 0);
}

static void imp_changed_cb(lv_event_t *e) { imp_unchecked(lv_event_get_user_data(e)); }

static const char *imp_path(imp_t *m)
{
    int i = m->file ? (int)lv_dropdown_get_selected(m->file) : -1;
    return i >= 0 && i < m->n ? m->f[i].path : NULL;
}

static void reboot_yes(void *u)
{
    (void)u;
    if (MG()->power && MG()->power(1) < 0) ui_toast(UI_TOAST_BAD, "vigild does not reboot this system");
}

static void imp_summary(imp_t *m, const ui_cfg_result_t *r)
{
    lv_obj_clean(m->summary);
    show(m->sum_box, r->n_lines > 0);
    for (int i = 0; i < r->n_lines; ++i) {
        char t[220];
        snprintf(t, sizeof t, "%s%s", r->kind[i] ? LV_SYMBOL_WARNING "  " : "", r->line[i]);
        ui_set_note(m->summary, t, r->kind[i] ? UI_C(WARN) : UI_C(TEXT));
    }
}

static void imp_done(void *st, const ui_cfg_result_t *r)
{
    imp_t *m = st;
    enable(m->check, m->n > 0);
    imp_summary(m, r);
    int ok = r->rc == UI_CFG_OK;
    lv_label_set_text_fmt(m->status, "%s  %s%s%s", ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, ok ? "" : rc_words(r->rc),
                          ok ? "" : ": ", r->msg);
    lv_obj_set_style_text_color(m->status, ok ? UI_C(OK) : UI_C(BAD), 0);
    if (!m->applying) {
        m->checked = ok;
        if (ok) snprintf(m->checked_path, sizeof m->checked_path, "%s", imp_path(m) ? imp_path(m) : "");
        enable(m->apply, ok);
        if (ok) lv_label_set_text(m->status, LV_SYMBOL_OK "  Checked: this is what the import changes.");
        if (r->rc == UI_CFG_EPASSWORD) lv_group_focus_obj(m->pw);
        return;
    }
    m->applying = 0;
    imp_unchecked(m);
    if (!ok) {
        ui_toast(UI_TOAST_BAD, "Not imported: %s", r->msg);
        return;
    }
    ui_toast(UI_TOAST_OK, "Configuration imported");
    lv_label_set_text(m->status, LV_SYMBOL_OK "  Imported: the settings are in force.");
    if (r->reboot)
        ui_confirm(r->net_cuts ? "Imported. The network settings apply after a reboot, and the recorder may then "
                                 "be at another address (see above). Reboot now?"
                               : "Imported. The network settings apply after a reboot. Reboot now?",
                   "Reboot", reboot_yes, NULL);
}

static void imp_run(imp_t *m, int apply)
{
    const char *path = imp_path(m);
    if (!path) return;
    char err[200] = "";
    if (MG()->cfg_import(path, lv_textarea_get_text(m->pw), lv_obj_has_state(m->host, LV_STATE_CHECKED), apply,
                         err, sizeof err) < 0) {
        ui_toast(UI_TOAST_BAD, "%s", err);
        return;
    }
    m->applying = apply;
    enable(m->check, 0);
    enable(m->apply, 0);
    lv_label_set_text(m->status, apply ? LV_SYMBOL_REFRESH "  Importing..." : LV_SYMBOL_REFRESH "  Checking...");
    lv_obj_set_style_text_color(m->status, UI_C(TEXT2), 0);
    m->poll.waiting = 1;
}

static void imp_check_cb(lv_event_t *e) { imp_run(lv_event_get_user_data(e), 0); }

static void imp_apply_yes(void *u) { imp_run(u, 1); }

static void imp_apply_cb(lv_event_t *e)
{
    imp_t *m = lv_event_get_user_data(e);
    if (!m->checked || !imp_path(m) || strcmp(m->checked_path, imp_path(m))) return;
    ui_confirm("Replace this recorder's settings, users and groups with the export's? The cameras reconnect "
               "with the imported settings. Recordings stay.",
               "Import", imp_apply_yes, m);
}

static void *imp_open(ui_set_t *s, lv_obj_t *pane)
{
    if (!have()) {
        unavailable(s, pane);
        return NULL;
    }
    ui_set_hide_actions(s);
    imp_t *m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->s = s;
    lv_obj_t *sec = ui_set_section(pane, "Import a configuration");
    ui_set_note(sec, "Replaces vigil's settings, users and groups with an export's, from this recorder or "
                     "another. Check first: it says what changes, and nothing changes until Import.", UI_C(TEXT));
    m->n = MG()->cfg_files ? MG()->cfg_files(m->f, FILES_MAX) : 0;
    char opts[FILES_MAX * 110] = "";
    for (int i = 0; i < m->n; ++i) {
        size_t l = strlen(opts);
        snprintf(opts + l, sizeof opts - l, "%s%s", i ? "\n" : "", m->f[i].path);
    }
    lv_obj_t *box = ui_set_row(sec, "File", "vigil-config-*.tar at the top of a mounted disk, the newest first. "
                                            "From a computer: POST /api/v1/system/config/import.");
    if (m->n) {
        m->file = ui_set_dropdown(s, box, opts, 0, 600);
        lv_obj_add_event_cb(m->file, imp_changed_cb, LV_EVENT_VALUE_CHANGED, m);
    } else {
        ui_set_note(box, LV_SYMBOL_WARNING "  No export found on the disks: plug in the USB stick that has one.",
                    UI_C(WARN));
    }
    box = ui_set_row(sec, "Export password", "The one typed when it was exported, if its passwords were included.");
    m->pw = ui_set_password(s, box, 128, 320);
    lv_obj_add_event_cb(m->pw, imp_changed_cb, LV_EVENT_VALUE_CHANGED, m);
    m->host = ui_set_switch(s, ui_set_row(sec, "Take its host name", "Off: this recorder keeps its own name."), 1);
    lv_obj_add_event_cb(m->host, imp_changed_cb, LV_EVENT_VALUE_CHANGED, m);
    lv_obj_t *row = ui_set_hbox(pane);
    m->check = ui_set_button(s, row, LV_SYMBOL_EYE_OPEN, "Check");
    lv_obj_add_event_cb(m->check, imp_check_cb, LV_EVENT_CLICKED, m);
    m->apply = ui_set_button(s, row, LV_SYMBOL_DOWNLOAD, "Import now");
    ui_btn_accent(m->apply);
    lv_obj_add_event_cb(m->apply, imp_apply_cb, LV_EVENT_CLICKED, m);
    enable(m->check, m->n > 0);
    enable(m->apply, 0);
    m->status = ui_set_note(pane, "", UI_C(TEXT2));
    /* The summary, under its title; both hidden until a check. */
    m->sum_box = lv_obj_create(pane);
    ui_plain(m->sum_box);
    lv_obj_set_size(m->sum_box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(m->sum_box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(m->sum_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(m->sum_box, VG_SPACE_MD, 0);
    m->summary = ui_set_section(m->sum_box, "What changes");
    show(m->sum_box, 0);
    poll_start(&m->poll, imp_done, m);
    return m;
}

static void imp_close(void *st)
{
    imp_t *m = st;
    if (!m) return;
    lv_timer_delete(m->poll.timer);
    free(m);
}

/* ---- Factory reset ----------------------------------------------------------------- */

typedef struct {
    ui_set_t *s;
    poll_t    poll;
    lv_obj_t *net, *net_warn, *word_v, *go_v, *word_f, *go_f, *status;
} rst_t;

static void rst_check(rst_t *r)
{
    enable(r->go_v, !strcmp(lv_textarea_get_text(r->word_v), WORD_VIGIL));
    enable(r->go_f, managed() && !strcmp(lv_textarea_get_text(r->word_f), WORD_FIRM));
    show(r->net_warn, lv_obj_has_state(r->net, LV_STATE_CHECKED));
}

static void rst_changed_cb(lv_event_t *e) { rst_check(lv_event_get_user_data(e)); }

static void rst_done(void *st, const ui_cfg_result_t *res)
{
    rst_t *r = st;
    int ok = res->rc == UI_CFG_OK;
    lv_label_set_text_fmt(r->status, "%s  %s", ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, res->msg);
    lv_obj_set_style_text_color(r->status, ok ? UI_C(OK) : UI_C(BAD), 0);
    if (ok) ui_toast(UI_TOAST_OK, "%s", res->msg);
    else ui_toast(UI_TOAST_BAD, "Not reset: %s", res->msg);
    lv_textarea_set_text(r->word_v, "");
    lv_textarea_set_text(r->word_f, "");
    rst_check(r);
}

static void rst_go(rst_t *r, int level)
{
    char err[200] = "";
    if (MG()->cfg_reset(level, level == 0 && lv_obj_has_state(r->net, LV_STATE_CHECKED), err, sizeof err) < 0) {
        ui_toast(UI_TOAST_BAD, "Not reset: %s", err);
        return;
    }
    enable(r->go_v, 0);
    enable(r->go_f, 0);
    lv_label_set_text(r->status, LV_SYMBOL_REFRESH "  Resetting...");
    lv_obj_set_style_text_color(r->status, UI_C(TEXT2), 0);
    r->poll.waiting = 1;
}

static void rst_vigil_cb(lv_event_t *e) { rst_go(lv_event_get_user_data(e), 0); }
static void rst_firm_cb(lv_event_t *e) { rst_go(lv_event_get_user_data(e), 1); }

static void *rst_open(ui_set_t *s, lv_obj_t *pane)
{
    if (!have() || !MG()->cfg_reset) {
        unavailable(s, pane);
        return NULL;
    }
    ui_set_hide_actions(s);
    rst_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->s = s;
    ui_set_note(pane, LV_SYMBOL_OK "  Recordings on the disks are never touched by either reset.", UI_C(OK));

    lv_obj_t *sec = ui_set_section(pane, "vigil's settings");
    ui_set_note(sec, "vigil's settings go back to the image's: no cameras, no users or groups of your own, the "
                     "screen and the services as they came; a new key for the passwords. The system account "
                     "(root) keeps its password. Applies at once.", UI_C(TEXT));
    r->net = ui_set_switch(s, ui_set_row(sec, "Also the network", "The interfaces as the image has them (DHCP), "
                                                                  "no Wi-Fi network; then a reboot."), 0);
    lv_obj_add_event_cb(r->net, rst_changed_cb, LV_EVENT_VALUE_CHANGED, r);
    r->net_warn = ui_set_note(sec, LV_SYMBOL_WARNING "  After the reboot the recorder takes its address from DHCP: "
                                                     "it may no longer be where it is now.", UI_C(WARN));
    r->word_v = ui_set_text(s, ui_set_row(sec, "Type " WORD_VIGIL " to confirm", NULL), "", 15, 200);
    lv_textarea_set_placeholder_text(r->word_v, WORD_VIGIL);
    lv_obj_add_event_cb(r->word_v, rst_changed_cb, LV_EVENT_VALUE_CHANGED, r);
    r->go_v = danger_button(s, ui_set_hbox(sec), LV_SYMBOL_REFRESH, "Reset vigil's settings");
    lv_obj_add_event_cb(r->go_v, rst_vigil_cb, LV_EVENT_CLICKED, r);

    sec = ui_set_section(pane, "The whole firmware");
    lv_obj_set_style_border_color(sec, UI_C(BAD), 0);
    ui_set_note(sec, "The firmware as it was flashed: OpenIPC's firstboot erases every setting of the system "
                     "(vigil's, the network, SSH keys, root's password) and reboots. The recorder then comes "
                     "back unclaimed, as new; the U-Boot environment (MAC, Wi-Fi network) stays.", UI_C(TEXT));
    if (!managed())
        ui_set_note(sec, LV_SYMBOL_WARNING "  vigild does not manage this system (it is not OpenIPC firmware): "
                                           "no factory reset here.", UI_C(WARN));
    r->word_f = ui_set_text(s, ui_set_row(sec, "Type " WORD_FIRM " to confirm", NULL), "", 15, 200);
    lv_textarea_set_placeholder_text(r->word_f, WORD_FIRM);
    lv_obj_add_event_cb(r->word_f, rst_changed_cb, LV_EVENT_VALUE_CHANGED, r);
    r->go_f = danger_button(s, ui_set_hbox(sec), LV_SYMBOL_WARNING, "Reset the firmware");
    lv_obj_add_event_cb(r->go_f, rst_firm_cb, LV_EVENT_CLICKED, r);
    r->status = ui_set_note(pane, "", UI_C(TEXT2));
    rst_check(r);
    poll_start(&r->poll, rst_done, r);
    return r;
}

static void rst_close(void *st)
{
    rst_t *r = st;
    if (!r) return;
    lv_timer_delete(r->poll.timer);
    free(r);
}

/* ---- the screen -------------------------------------------------------------------- */

static ui_set_item_t g_items[3];

static void open_now(void *arg)
{
    const ui_set_item_t items[] = {
        { "Export", &ui_icon_nav_basic, exp_open, NULL, NULL, NULL, exp_close },
        { "Import", &ui_icon_nav_disks, imp_open, NULL, NULL, NULL, imp_close },
        { "Factory reset", &ui_icon_nav_maint, rst_open, NULL, NULL, NULL, rst_close },
    };
    memcpy(g_items, items, sizeof items);
    ui_settings_open("Configuration", g_items, 3, (int)(intptr_t)arg);
}

void ui_backup_open(int first)
{
    /* The whole configuration, the accounts in it. */
    ui_with_perm(UI_PERM_SYSTEM | UI_PERM_USERS, open_now, (void *)(intptr_t)first);
}
