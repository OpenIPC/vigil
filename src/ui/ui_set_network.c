/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ui_set_network.c — Network: Ethernet, Wi-Fi, LTE, Ports, DDNS, UPnP,
 * SMTP, Alarm center, QoS, P2P (docs/management.md).
 *
 * Ethernet and Wi-Fi change the system through the model's management
 * calls (jobs); an Ethernet change is undone unless "Keep" is pressed
 * within the time the question counts down. DDNS, UPnP, SMTP and the
 * alarm center are settings of vigil.yaml: the schema's forms, with the
 * service's last result and its action (update now, a test mail). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "ui_i18n.h"
#include "ui_settings.h"
#include "ui_theme.h"

#define STATUS_MS 2000

static const ui_mgmt_t *MG(void) { return ui_model()->mgmt; }

static int managed(void) { return MG() && MG()->managed && MG()->managed(); }

static void not_managed_note(lv_obj_t *pane)
{
    if (!managed()) ui_set_note(pane, LV_SYMBOL_WARNING "  " "vigild does not manage this system (it is not "
                                      "OpenIPC firmware): the settings are shown, not changed.", UI_C(WARN));
}

static const char *or_dash(const char *s) { return s && *s ? s : "-"; }

/* ---- interface status --------------------------------------------------------------- */

typedef struct {
    lv_obj_t *link, *mac, *addr, *gw, *dns;
} status_w_t;

static void status_build(lv_obj_t *pane, status_w_t *w)
{
    lv_obj_t *sec = ui_set_section(pane, "Status");
    w->link = ui_set_info(sec, "Link", "");
    w->mac = ui_set_info(sec, "MAC address", "");
    w->addr = ui_set_info(sec, "Address", "");
    w->gw = ui_set_info(sec, "Gateway", "");
    w->dns = ui_set_info(sec, "DNS", "");
}

static void status_fill(status_w_t *w, const ui_net_status_t *st)
{
    if (!st->present) lv_label_set_text(w->link, "no such interface");
    else if (!st->up) lv_label_set_text(w->link, "down");
    else if (!st->link) lv_label_set_text(w->link, "no cable");
    else if (st->speed) lv_label_set_text_fmt(w->link, "up, %d Mb/s %s", st->speed, st->duplex);
    else lv_label_set_text(w->link, "up");
    lv_obj_set_style_text_color(w->link, st->link ? UI_C(OK) : UI_C(WARN), 0);
    lv_label_set_text(w->mac, or_dash(st->mac));
    if (st->address[0]) lv_label_set_text_fmt(w->addr, "%s / %s", st->address, st->netmask);
    else lv_label_set_text(w->addr, "-");
    lv_label_set_text(w->gw, or_dash(st->gateway));
    if (st->dns2[0]) lv_label_set_text_fmt(w->dns, "%s, %s", st->dns1, st->dns2);
    else lv_label_set_text(w->dns, or_dash(st->dns1));
}

/* ---- "Keep these settings?" ------------------------------------------------------------ */

static lv_timer_t *g_keep_timer;
static lv_obj_t   *g_keep_label;
typedef struct eth eth_t;
static eth_t      *g_eth;           /* the Ethernet sub-page while it is open */
static void        eth_load(eth_t *e);

static void keep_end(const char *toast, ui_toast_t kind)
{
    if (g_keep_timer) lv_timer_delete(g_keep_timer);
    g_keep_timer = NULL;
    g_keep_label = NULL;
    ui_popup_close();
    if (toast) ui_toast(kind, "%s", toast);
    if (g_eth) eth_load(g_eth);
}

static void keep_tick(lv_timer_t *t)
{
    (void)t;
    int secs = 0;
    if (!MG()->net_pending(&secs)) {
        keep_end("No confirmation: the previous network settings are back", UI_TOAST_WARN);
        return;
    }
    if (g_keep_label) lv_label_set_text_fmt(g_keep_label, "Undone in %d s unless you keep them.", secs);
}

static void keep_cb(lv_event_t *e)
{
    int keep = (int)(intptr_t)lv_event_get_user_data(e);
    if (keep) {
        MG()->net_confirm();
        keep_end("Network settings kept", UI_TOAST_OK);
    } else {
        MG()->net_revert();
        keep_end("Undoing the network change", UI_TOAST_WARN);
    }
}

/* The countdown question, on whatever screen is open: the change waits
 * for it even when the user has moved on. */
static void keep_ask(void)
{
    if (g_keep_timer) return;
    lv_obj_t *p = ui_popup_open(NULL, 520);
    if (!p) return;
    lv_obj_set_style_pad_all(p, VG_SPACE_LG, 0);
    lv_obj_set_style_pad_row(p, VG_SPACE_MD, 0);
    ui_label(p, TR(S_KEEP_Q), VG_FONT_LG_LV, UI_C(TEXT));
    g_keep_label = ui_label(p, "", VG_FONT_MD_LV, UI_C(TEXT2));
    lv_obj_t *row = ui_set_hbox(p);
    lv_obj_t *undo = ui_btn(row, LV_SYMBOL_REFRESH, TR(S_UNDO));
    lv_obj_add_event_cb(undo, keep_cb, LV_EVENT_CLICKED, (void *)0);
    lv_obj_t *keep = ui_btn(row, LV_SYMBOL_OK, TR(S_KEEP));
    ui_btn_accent(keep);
    lv_obj_add_event_cb(keep, keep_cb, LV_EVENT_CLICKED, (void *)1);
    ui_popup_place(NULL);
    ui_popup_focus(row, keep, LV_GRIDNAV_CTRL_NONE);
    g_keep_timer = lv_timer_create(keep_tick, 500, NULL);
    keep_tick(NULL);
}

/* ---- Ethernet ------------------------------------------------------------------------ */

struct eth {
    ui_set_t     *s;
    ui_net_cfg_t  cfg;
    status_w_t    st;
    lv_obj_t     *dhcp, *addr, *mask, *gw, *dns1, *dns2, *mtu, *job;
    lv_timer_t   *timer;
};

static void eth_fields_enable(eth_t *e)
{
    int dhcp = lv_obj_has_state(e->dhcp, LV_STATE_CHECKED);
    lv_obj_t *w[] = { e->addr, e->mask, e->gw, e->dns1, e->dns2 };
    for (size_t i = 0; i < sizeof w / sizeof w[0]; ++i) {
        if (dhcp) lv_obj_add_state(w[i], LV_STATE_DISABLED);
        else lv_obj_remove_state(w[i], LV_STATE_DISABLED);
    }
}

static void eth_dhcp_cb(lv_event_t *ev) { eth_fields_enable(lv_event_get_user_data(ev)); }

static void eth_values(eth_t *e, ui_net_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->dhcp = lv_obj_has_state(e->dhcp, LV_STATE_CHECKED);
    snprintf(c->address, sizeof c->address, "%s", lv_textarea_get_text(e->addr));
    snprintf(c->netmask, sizeof c->netmask, "%s", lv_textarea_get_text(e->mask));
    snprintf(c->gateway, sizeof c->gateway, "%s", lv_textarea_get_text(e->gw));
    snprintf(c->dns1, sizeof c->dns1, "%s", lv_textarea_get_text(e->dns1));
    snprintf(c->dns2, sizeof c->dns2, "%s", lv_textarea_get_text(e->dns2));
    c->mtu = ui_set_number_get(e->mtu);
}

static void eth_load(eth_t *e)
{
    ui_net_status_t st;
    MG()->eth_get(&e->cfg, &st);
    if (e->cfg.dhcp) lv_obj_add_state(e->dhcp, LV_STATE_CHECKED);
    else lv_obj_remove_state(e->dhcp, LV_STATE_CHECKED);
    /* With DHCP the fields show what the lease gave, greyed. */
    lv_textarea_set_text(e->addr, e->cfg.dhcp ? st.address : e->cfg.address);
    lv_textarea_set_text(e->mask, e->cfg.dhcp ? st.netmask : e->cfg.netmask);
    lv_textarea_set_text(e->gw, e->cfg.dhcp ? st.gateway : e->cfg.gateway);
    lv_textarea_set_text(e->dns1, e->cfg.dhcp ? st.dns1 : e->cfg.dns1);
    lv_textarea_set_text(e->dns2, e->cfg.dhcp ? st.dns2 : e->cfg.dns2);
    char m[16];
    snprintf(m, sizeof m, "%d", e->cfg.mtu);
    lv_textarea_set_text(e->mtu, m);
    eth_fields_enable(e);
    status_fill(&e->st, &st);
}

static void eth_tick(lv_timer_t *t)
{
    eth_t *e = lv_timer_get_user_data(t);
    ui_net_cfg_t c;
    ui_net_status_t st;
    MG()->eth_get(&c, &st);
    status_fill(&e->st, &st);
}

static void *eth_open(ui_set_t *s, lv_obj_t *pane)
{
    if (!MG() || !MG()->eth_get) return NULL;
    eth_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    e->s = s;
    not_managed_note(pane);
    status_build(pane, &e->st);
    lv_obj_t *sec = ui_set_section(pane, "Settings");
    e->dhcp = ui_set_switch(s, ui_set_row(sec, "DHCP", "Get the address from the network's DHCP server."), 0);
    lv_obj_add_event_cb(e->dhcp, eth_dhcp_cb, LV_EVENT_VALUE_CHANGED, e);
    e->addr = ui_set_text(s, ui_set_row(sec, "IP address", NULL), "", 15, 240);
    e->mask = ui_set_text(s, ui_set_row(sec, "Netmask", NULL), "", 15, 240);
    e->gw = ui_set_text(s, ui_set_row(sec, "Gateway", NULL), "", 15, 240);
    e->dns1 = ui_set_text(s, ui_set_row(sec, "DNS 1", NULL), "", 15, 240);
    e->dns2 = ui_set_text(s, ui_set_row(sec, "DNS 2", NULL), "", 15, 240);
    e->mtu = ui_set_number(s, ui_set_row(sec, "MTU", "0: the interface's default."), 0, 0, 9000);
    e->job = ui_set_note(pane, "", UI_C(TEXT2));
    ui_set_note(pane, "A change is undone after 60 s unless you keep it: if the new settings cut the "
                      "recorder off, it comes back by itself.", UI_C(TEXT2));
    eth_load(e);
    e->timer = lv_timer_create(eth_tick, STATUS_MS, e);
    g_eth = e;
    return e;
}

static int eth_dirty(void *st)
{
    eth_t *e = st;
    ui_net_cfg_t c;
    eth_values(e, &c);
    if (c.dhcp != e->cfg.dhcp || c.mtu != e->cfg.mtu) return 1;
    if (c.dhcp) return 0;
    return strcmp(c.address, e->cfg.address) || strcmp(c.netmask, e->cfg.netmask) ||
           strcmp(c.gateway, e->cfg.gateway) || strcmp(c.dns1, e->cfg.dns1) || strcmp(c.dns2, e->cfg.dns2);
}

static void eth_done(void *user, int ok, const char *msg)
{
    (void)user;
    if (!ok) {
        ui_toast(UI_TOAST_BAD, "Not applied: %s", msg);
        if (g_eth) eth_load(g_eth);
        return;
    }
    keep_ask();
}

static lv_obj_t *eth_field(eth_t *e, const char *f)
{
    if (!strcmp(f, "address")) return e->addr;
    if (!strcmp(f, "netmask")) return e->mask;
    if (!strcmp(f, "gateway")) return e->gw;
    if (!strcmp(f, "dns1")) return e->dns1;
    if (!strcmp(f, "dns2")) return e->dns2;
    if (!strcmp(f, "mtu")) return e->mtu;
    return NULL;
}

static int eth_save(void *st)
{
    eth_t *e = st;
    ui_net_cfg_t c;
    eth_values(e, &c);
    if (c.dhcp) c.address[0] = c.netmask[0] = c.gateway[0] = c.dns1[0] = c.dns2[0] = 0;
    char field[32] = "", err[200] = "";
    lv_obj_t *w[] = { e->addr, e->mask, e->gw, e->dns1, e->dns2, e->mtu };
    for (size_t i = 0; i < sizeof w / sizeof w[0]; ++i) lv_obj_remove_state(w[i], LV_STATE_USER_1);
    int rc = MG()->eth_apply(&c, field, sizeof field, err, sizeof err);
    if (rc == UI_MGMT_EUNMANAGED) {
        ui_toast(UI_TOAST_BAD, "Not changed: vigild does not manage this system");
        return -1;
    }
    if (rc <= 0) {
        lv_obj_t *bad = eth_field(e, field);
        if (bad) lv_obj_add_state(bad, LV_STATE_USER_1);
        ui_toast(UI_TOAST_BAD, "Not applied: %s%s%s", field, field[0] ? ": " : "", err);
        return -1;
    }
    e->cfg = c;                         /* nothing left to save while it runs */
    lv_label_set_text(e->job, LV_SYMBOL_REFRESH "  Applying...");
    /* Followed without a label: the question must come even if this
     * sub-page has been left meanwhile. */
    ui_set_follow_job(e->s, rc, NULL, eth_done, NULL);
    return 0;
}

static void eth_cancel(void *st) { eth_load(st); }

static void eth_close(void *st)
{
    eth_t *e = st;
    lv_timer_delete(e->timer);
    if (g_eth == e) g_eth = NULL;
    free(e);
}

/* ---- Wi-Fi ------------------------------------------------------------------------- */

typedef struct {
    ui_set_t  *s;
    ui_wifi_t  w;
    status_w_t st;
    lv_obj_t  *ssid, *pass, *list, *job, *scan_note;
    char       profile[48];         /* wlandev to set: the adapter's when none is */
} wifi_t;

static void net_pick_cb(lv_event_t *ev)
{
    wifi_t *w = lv_event_get_user_data(ev);
    lv_obj_t *b = lv_event_get_target(ev);
    lv_textarea_set_text(w->ssid, lv_label_get_text(lv_obj_get_child(b, -1)));
}

static void scan_cb(lv_event_t *ev)
{
    wifi_t *w = lv_event_get_user_data(ev);
    ui_wifi_net_t nets[16];
    char err[160] = "";
    int n = MG()->wifi_scan(nets, 16, err, sizeof err);
    lv_obj_clean(w->list);
    if (n < 0) {
        ui_set_note(w->list, err, UI_C(BAD));
        return;
    }
    if (!n) ui_set_note(w->list, "No networks found.", UI_C(TEXT2));
    for (int i = 0; i < n; ++i) {
        char t[96];
        snprintf(t, sizeof t, "%s %d dBm", nets[i].secured ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_WIFI, nets[i].signal);
        lv_obj_t *b = ui_set_button(w->s, w->list, NULL, t);
        ui_label(b, nets[i].ssid, VG_FONT_MD_LV, UI_C(TEXT));
        lv_obj_add_event_cb(b, net_pick_cb, LV_EVENT_CLICKED, w);
    }
}

/* A USB adapter or modem: what it is, its driver and the firmware profile
 * that loads it. */
static void radio_rows(lv_obj_t *sec, const ui_radio_t *r)
{
    char t[96];
    snprintf(t, sizeof t, "%s (USB %s)", r->name[0] ? r->name : "unnamed", r->id);
    ui_set_info(sec, "Device", t);
    if (r->driver[0]) snprintf(t, sizeof t, "%s%s", r->driver, r->bound ? ", loaded" : ", not loaded yet");
    else snprintf(t, sizeof t, "none in this firmware");
    ui_set_info(sec, "Driver", t);
    ui_set_info(sec, "Firmware profile", r->profile[0] ? r->profile : "none for it");
}

static void *wifi_open(ui_set_t *s, lv_obj_t *pane)
{
    if (!MG() || !MG()->wifi_get) return NULL;
    wifi_t *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->s = s;
    ui_net_status_t st;
    MG()->wifi_get(&w->w, &st);
    if (!w->w.ifname[0] && !w->w.n_adapters) {
        ui_set_hide_actions(s);
        lv_obj_t *sec = ui_set_section(pane, NULL);
        ui_label(sec, "No Wi-Fi adapter", VG_FONT_LG_LV, UI_C(TEXT));
        ui_set_note(sec, "No wireless interface and no USB Wi-Fi adapter this firmware has a driver for. "
                         "Plug one in: it shows up here.", UI_C(TEXT2));
        return w;
    }
    not_managed_note(pane);
    const ui_radio_t *ad = w->w.n_adapters ? &w->w.adapters[0] : NULL;
    if (ad) {
        lv_obj_t *sec = ui_set_section(pane, "Adapter");
        radio_rows(sec, ad);
        snprintf(w->profile, sizeof w->profile, "%s", w->w.profile[0] ? w->w.profile : ad->profile);
        ui_set_info(sec, "wlandev", w->w.profile[0] ? w->w.profile : "not set");
        if (!ad->driver[0] || !ad->profile[0]) {
            ui_set_hide_actions(s);
            ui_set_note(pane, LV_SYMBOL_WARNING "  This firmware has no driver profile for this adapter "
                              "(/etc/wireless/usb), so it cannot be used.", UI_C(WARN));
            return w;
        }
        if (!w->w.ifname[0])
            ui_set_note(pane, "No wireless interface yet: Save sets wlandev to the profile above, loads the "
                              "driver the firmware's way and joins the network; the firmware starts it by itself "
                              "at every boot from then on.", UI_C(TEXT2));
    }
    if (w->w.ifname[0]) {
        status_build(pane, &w->st);
        status_fill(&w->st, &st);
    }
    lv_obj_t *sec = ui_set_section(pane, "Network to join");
    lv_obj_t *box = ui_set_row(sec, "SSID", NULL);
    w->ssid = ui_set_text(s, box, w->w.ssid, 32, 320);
    lv_obj_t *scan = ui_set_button(s, box, LV_SYMBOL_REFRESH, "Scan");
    lv_obj_add_event_cb(scan, scan_cb, LV_EVENT_CLICKED, w);
    if (!w->w.can_scan) {
        lv_obj_add_state(scan, LV_STATE_DISABLED);
        ui_set_note(box, w->w.ifname[0] ? "Scanning needs iw, which this firmware does not have."
                                        : "Scanning works once the adapter's interface is up.", UI_C(TEXT2));
    }
    w->list = lv_obj_create(sec);
    ui_plain(w->list);
    lv_obj_set_size(w->list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(w->list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(w->list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(w->list, VG_SPACE_SM, 0);
    w->pass = ui_set_password(s, ui_set_row(sec, "Password", w->w.password_set
                                                                  ? "Set. Type a new one to change it; empty: an "
                                                                    "open network."
                                                                  : "Empty: an open network."),
                              64, 320);
    w->job = ui_set_note(pane, "", UI_C(TEXT2));
    return w;
}

static int wifi_dirty(void *st)
{
    wifi_t *w = st;
    if (!w->ssid) return 0;
    return strcmp(lv_textarea_get_text(w->ssid), w->w.ssid) || lv_textarea_get_text(w->pass)[0];
}

static void wifi_done(void *user, int ok, const char *msg)
{
    (void)user;
    ui_toast(ok ? UI_TOAST_OK : UI_TOAST_BAD, "%s", msg);
}

static int wifi_save(void *st)
{
    wifi_t *w = st;
    if (!w->ssid) return 0;
    char err[200] = "";
    int rc = MG()->wifi_set(lv_textarea_get_text(w->ssid), lv_textarea_get_text(w->pass), w->profile, err,
                            sizeof err);
    if (rc <= 0) {
        ui_toast(UI_TOAST_BAD, "Not saved: %s", rc == UI_MGMT_EUNMANAGED ? "vigild does not manage this system"
                                                                         : err);
        return -1;
    }
    snprintf(w->w.ssid, sizeof w->w.ssid, "%s", lv_textarea_get_text(w->ssid));
    lv_textarea_set_text(w->pass, "");
    ui_set_follow_job(w->s, rc, w->job, wifi_done, w);
    return 0;
}

static void wifi_cancel(void *st)
{
    wifi_t *w = st;
    if (!w->ssid) return;
    lv_textarea_set_text(w->ssid, w->w.ssid);
    lv_textarea_set_text(w->pass, "");
}

/* ---- LTE --------------------------------------------------------------------------- */

static void lte_profile_cb(lv_event_t *ev)
{
    const char *profile = lv_event_get_user_data(ev);
    char msg[200] = "";
    int rc = MG()->lte_profile ? MG()->lte_profile(profile, msg, sizeof msg) : UI_MGMT_EFAIL;
    if (rc == UI_MGMT_EUNMANAGED) ui_toast(UI_TOAST_BAD, "Not changed: vigild does not manage this system");
    else ui_toast(rc < 0 ? UI_TOAST_BAD : UI_TOAST_OK, "%s", msg[0] ? msg : "failed");
}

static ui_lte_t g_lte;              /* the page's: profile buttons point into it */

static void *lte_open(ui_set_t *s, lv_obj_t *pane)
{
    ui_set_hide_actions(s);
    if (!MG() || !MG()->lte_get) return NULL;
    ui_net_status_t st;
    MG()->lte_get(&g_lte, &st);
    ui_lte_t *l = &g_lte;
    if (!l->ifname[0] && !l->n_modems) {
        lv_obj_t *sec = ui_set_section(pane, NULL);
        ui_label(sec, "No modem found", VG_FONT_LG_LV, UI_C(TEXT));
        ui_set_note(sec, "No LTE modem's network interface (RNDIS, ECM, NCM or QMI) and no USB modem this "
                         "firmware has a driver or a profile for. Plug one in: it shows up here.", UI_C(TEXT2));
        if (l->profile[0]) ui_set_info(sec, "wlandev", l->profile);
        return NULL;
    }
    for (int i = 0; i < l->n_modems; ++i) {
        const ui_radio_t *r = &l->modems[i];
        lv_obj_t *sec = ui_set_section(pane, i ? NULL : "Modem");
        radio_rows(sec, r);
        if (!r->profile[0]) {
            ui_set_note(sec, "No profile of /etc/wireless/modem names this modem: the firmware loads its "
                             "driver only if the device's class matches (RNDIS, ECM, NCM).", UI_C(TEXT2));
        } else if (!strcmp(r->profile, l->profile)) {
            ui_set_note(sec, "wlandev names this profile: the firmware starts the modem at boot.", UI_C(OK));
        } else {
            lv_obj_t *b = ui_set_button(s, sec, LV_SYMBOL_OK, "Use this profile");
            lv_obj_add_event_cb(b, lte_profile_cb, LV_EVENT_CLICKED, (void *)r->profile);
            ui_set_note(sec, "Sets wlandev; the modem starts at the next boot. Wi-Fi and the modem share "
                             "wlandev: a Wi-Fi profile set there is replaced.", UI_C(TEXT2));
        }
    }
    if (l->ifname[0]) {
        lv_obj_t *sec = ui_set_section(pane, "Interface");
        ui_set_info(sec, "Interface", l->ifname);
        ui_set_info(sec, "Driver", or_dash(l->driver));
        ui_set_info(sec, "wlandev", or_dash(l->profile));
        status_w_t w;
        status_build(pane, &w);
        status_fill(&w, &st);
    }
    return NULL;
}

/* ---- Ports ------------------------------------------------------------------------- */

/* The ports of vigild's servers: the web server (web.listen), the API
 * (api.listen) and ONVIF (onvif_server.port), which move at once when
 * saved (main.c restarts the first two, onvif.c's thread follows its
 * setting; a port that cannot be had is refused by the store, or put
 * back, and said here), and those of the RTSP and NETIP servers vigild
 * does not have yet (rtsp_server.port, netip_server.port), kept for them.
 * No two on one port. */

#define PORTS_N   5
#define SERVERS_N 3                     /* the first ones: running servers */
enum { P_WEB, P_API, P_ONVIF, P_RTSP, P_NETIP };
static const char *const k_port_sec[PORTS_N] = { "web", "api", "onvif_server", "rtsp_server", "netip_server" };
static const char *const k_port_key[PORTS_N] = { "listen", "listen", "port", "port", "port" };
static const char *const k_port_name[PORTS_N] = { "Web server", "HTTP API", "ONVIF", "RTSP", "NETIP" };
static const int         k_port_server[SERVERS_N] = { UI_SERVER_WEB, UI_SERVER_API, UI_SERVER_ONVIF };

typedef struct {
    ui_set_t   *s;
    char        stored[PORTS_N][64];    /* the setting as stored: "0.0.0.0:80", "554" */
    int         port[PORTS_N];          /* its port; 0: off (an empty listen) */
    lv_obj_t   *num[PORTS_N], *state[SERVERS_N];
    lv_timer_t *timer;
} ports_t;

static int listen_port(const char *v)
{
    const char *colon = strrchr(v, ':');
    return *v ? atoi(colon ? colon + 1 : v) : 0;
}

static int sec_field(int sec, const char *key)
{
    const ui_section_t *u = &ui_model()->sections[sec];
    for (int f = 0; f < u->n_fields; ++f)
        if (!strcmp(u->fields[f].key, key)) return f;
    return -1;
}

static void ports_state(ports_t *p)
{
    for (int i = 0; i < SERVERS_N; ++i) {
        if (!p->state[i]) continue;
        ui_listen_t l;
        if (!MG() || !MG()->listening) {
            lv_label_set_text(p->state[i], "");
            continue;
        }
        MG()->listening(k_port_server[i], &l);
        if (l.on && l.why[0]) lv_label_set_text_fmt(p->state[i], LV_SYMBOL_WARNING "  Listening on %s; %s", l.listen, l.why);
        else if (l.on) lv_label_set_text_fmt(p->state[i], LV_SYMBOL_OK "  Listening on %s", l.listen);
        else lv_label_set_text_fmt(p->state[i], LV_SYMBOL_WARNING "  Not listening: %s", l.why[0] ? l.why : "not started");
        lv_obj_set_style_text_color(p->state[i], l.on && !l.why[0] ? UI_C(OK) : UI_C(WARN), 0);
    }
}

static void ports_tick(lv_timer_t *t) { ports_state(lv_timer_get_user_data(t)); }

static void ports_load(ports_t *p)
{
    for (int i = 0; i < PORTS_N; ++i) {
        int sec = ui_set_section_index(k_port_sec[i]), f = sec < 0 ? -1 : sec_field(sec, k_port_key[i]);
        p->stored[i][0] = 0;
        if (f >= 0) ui_model()->get_value(sec, -1, f, p->stored[i], sizeof p->stored[i]);
        p->port[i] = listen_port(p->stored[i]);
        if (!p->num[i]) continue;
        lv_obj_remove_state(p->num[i], LV_STATE_USER_1);
        char t[8];
        snprintf(t, sizeof t, "%d", p->port[i]);
        lv_textarea_set_text(p->num[i], t);
    }
    ports_state(p);
}

static void ports_enable(lv_obj_t *num, int on)
{
    lv_obj_t *box = lv_obj_get_parent(num);
    for (uint32_t i = 0; i < lv_obj_get_child_count(box); ++i) {
        if (on) lv_obj_remove_state(lv_obj_get_child(box, (int32_t)i), LV_STATE_DISABLED);
        else lv_obj_add_state(lv_obj_get_child(box, (int32_t)i), LV_STATE_DISABLED);
    }
}

static void *ports_open(ui_set_t *s, lv_obj_t *pane)
{
    ports_t *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->s = s;
    ports_load(p);
    static const char *const helps[PORTS_N] = {
        "The zero channel: this screen in a browser (web.listen). 80 is HTTP's own port.",
        "The HTTP API (api.listen): apps, scripts, the API's event WebSocket.",
        "ONVIF for video management software and other recorders (onvif_server). 8899 is XM's ONVIF "
        "port; 80, ONVIF's usual one, is the web server's here.",
        "554 is RTSP's own.",
        "34567: XM's NETIP, for CMS and VMS software.",
    };
    lv_obj_t *sec = ui_set_section(pane, "Servers");
    int users = ui_allowed(UI_PERM_USERS);
    for (int i = 0; i < SERVERS_N; ++i) {
        lv_obj_t *box = ui_set_row(sec, k_port_name[i], helps[i]);
        p->num[i] = ui_set_number(s, box, p->port[i], 1, 65535);
        p->state[i] = ui_set_note(box, "", UI_C(TEXT2));
        if (i == P_ONVIF) continue;     /* the network permission's, as the rest of this screen */
        if (!p->port[i] || !users) ports_enable(p->num[i], 0);
        if (!p->port[i])
            ui_label(box, i == P_WEB ? "Off: web.listen is empty (System -> Zero channel)."
                                      : "Off: api.listen is empty (System -> HTTP API).",
                     VG_FONT_SM_LV, UI_C(TEXT2));
    }
    if (!users)
        ui_set_note(pane, "Changing the web server's and the API's ports needs the users permission, as their "
                          "sections (api, web) do.",
                    UI_C(TEXT2));
    ui_set_note(pane, "Saved, a server moves to its new port at once; its open connections close (a browser "
                      "showing the zero channel reconnects at the new address; ONVIF clients find the new port "
                      "by WS-Discovery). A port that cannot be had is refused, and said why.", UI_C(TEXT2));
    lv_obj_t *later = ui_set_section(pane, "Services not available yet");
    for (int i = SERVERS_N; i < PORTS_N; ++i)
        p->num[i] = ui_set_number(s, ui_set_row(later, k_port_name[i], helps[i]), p->port[i], 1, 65535);
    ui_set_note(pane, LV_SYMBOL_WARNING "  vigild has no RTSP or NETIP server yet: nothing listens on these ports. "
                                        "They are kept for those servers, and no other server of vigild's may "
                                        "take them.", UI_C(WARN));
    ports_load(p);
    p->timer = lv_timer_create(ports_tick, 1000, p);
    return p;
}

static int ports_dirty(void *st)
{
    ports_t *p = st;
    for (int i = 0; i < PORTS_N; ++i)
        if (p->port[i] && ui_set_number_get(p->num[i]) != p->port[i]) return 1;
    return 0;
}

/* `stored` with its port replaced: "0.0.0.0:80" -> "0.0.0.0:8081", "554" -> "8554". */
static void with_port(const char *stored, int port, char *out, size_t cap)
{
    const char *colon = strrchr(stored, ':');
    if (colon) snprintf(out, cap, "%.*s:%d", (int)(colon - stored), stored, port);
    else snprintf(out, cap, "%d", port);
}

static int ports_save_one(ports_t *p, int i, int port)
{
    int sec = ui_set_section_index(k_port_sec[i]);
    if (sec < 0) return -1;
    const ui_section_t *u = &ui_model()->sections[sec];
    char buf[16][192];
    const char *v[16];
    if (u->n_fields > 16) return -1;
    for (int f = 0; f < u->n_fields; ++f) {
        ui_model()->get_value(sec, -1, f, buf[f], sizeof buf[f]);
        if (!strcmp(u->fields[f].key, k_port_key[i])) with_port(p->stored[i], port, buf[f], sizeof buf[f]);
        v[f] = buf[f];
    }
    unsigned gen = ui_model()->config_generation ? ui_model()->config_generation() : 0;
    ui_errors_t errs;
    memset(&errs, 0, sizeof errs);
    ui_save_rc_t rc = ui_model()->save(sec, 1, v, NULL, gen, &errs);
    if (rc == UI_SAVE_OK || rc == UI_SAVE_UNSYNCED) return 0;
    lv_obj_add_state(p->num[i], LV_STATE_USER_1);
    ui_toast(UI_TOAST_BAD, "%s port not saved: %s", k_port_name[i], errs.n ? errs.e[0].msg : "the settings changed meanwhile");
    return -1;
}

static int ports_save(void *st)
{
    ports_t *p = st;
    int want[PORTS_N];
    for (int i = 0; i < PORTS_N; ++i) {
        lv_obj_remove_state(p->num[i], LV_STATE_USER_1);
        want[i] = p->port[i] ? ui_set_number_get(p->num[i]) : 0;
    }
    for (int i = 0; i < PORTS_N; ++i)
        for (int k = 0; k < i; ++k)
            if (want[i] && want[i] == want[k]) {
                /* the one that was changed onto the other's port */
                lv_obj_add_state(p->num[want[i] != p->port[i] ? i : k], LV_STATE_USER_1);
                ui_toast(UI_TOAST_BAD, "%s and %s cannot both be on port %d", k_port_name[k], k_port_name[i], want[i]);
                return -1;
            }
    /* One section at a time, each checked against the others as stored:
     * first those whose new port no other holds now, so that moving one
     * onto a port another leaves works. Swapping two takes a free port in
     * between. */
    int left = 0;
    for (int i = 0; i < PORTS_N; ++i) left += want[i] != p->port[i];
    for (int pass = 0; left && pass < PORTS_N; ++pass)
        for (int i = 0; i < PORTS_N; ++i) {
            if (want[i] == p->port[i]) continue;
            int held = 0;
            for (int k = 0; k < PORTS_N; ++k) held |= k != i && p->port[k] == want[i];
            if (held && pass < PORTS_N - 1) continue;
            /* Refused: what was typed stays, marked, with the toast;
             * the ones saved before it are saved. */
            if (ports_save_one(p, i, want[i]) < 0) return -1;
            p->port[i] = want[i];
            --left;
        }
    ports_load(p);
    ui_toast(UI_TOAST_OK, "Ports saved");
    return 0;
}

static void ports_cancel(void *st) { ports_load(st); }

static void ports_close(void *st)
{
    ports_t *p = st;
    lv_timer_delete(p->timer);
    free(p);
}

/* ---- services: a form of vigil.yaml and the service's state --------------------------- */

typedef struct {
    ui_set_t  *s;
    ui_form_t *form;
    lv_obj_t  *state, *job;
    int        which;               /* 0 ddns, 1 smtp, 2 alarm, 3 upnp */
} svc_t;

static void svc_state(svc_t *v)
{
    if (!v->state || !MG() || !MG()->services) return;
    ui_svc_t d, m, a, n, *x = v->which == 0 ? &d : v->which == 1 ? &m : &a;
    MG()->services(&d, &m, &a, &n);
    char when[48];
    ui_set_time_text(x->last, when, sizeof when);
    if (!x->last) lv_label_set_text(v->state, "Last: never");
    else lv_label_set_text_fmt(v->state, "Last: %s, %s%s%s", when, x->ok ? "ok" : "failed", x->result[0] ? ": " : "",
                               x->result);
    lv_obj_set_style_text_color(v->state, !x->last ? UI_C(TEXT2) : x->ok ? UI_C(OK) : UI_C(BAD), 0);
}

static void svc_done(void *user, int ok, const char *msg)
{
    svc_t *v = user;
    (void)ok;
    (void)msg;
    svc_state(v);
}

static void svc_action_cb(lv_event_t *ev)
{
    svc_t *v = lv_event_get_user_data(ev);
    if (ui_form_embed_dirty(v->form)) {
        ui_toast(UI_TOAST_WARN, "Save the settings first: the action uses the saved ones");
        return;
    }
    int id = v->which == 0 ? MG()->ddns_update() : MG()->smtp_test();
    if (id <= 0) {
        ui_toast(UI_TOAST_BAD, "Cannot start it now");
        return;
    }
    ui_set_follow_job(v->s, id, v->job, svc_done, v);
}

static svc_t *svc_open(ui_set_t *s, lv_obj_t *pane, int which, const char *key, const char *note,
                       const char *action)
{
    svc_t *v = calloc(1, sizeof *v);
    if (!v) return NULL;
    v->s = s;
    v->which = which;
    if (note) ui_set_note(pane, note, UI_C(WARN));
    if (which <= 1 && MG() && MG()->curl && !MG()->curl())
        ui_set_note(pane, LV_SYMBOL_WARNING "  This firmware has no curl: nothing can be sent.", UI_C(WARN));
    /* The service's state and action first, where they are seen. */
    lv_obj_t *action_btn = NULL;
    if (which <= 2) {
        lv_obj_t *row = ui_set_hbox(pane);
        if (action && MG()) action_btn = ui_btn(row, LV_SYMBOL_UPLOAD, action);
        v->state = ui_label(row, "", VG_FONT_SM_LV, UI_C(TEXT2));
        v->job = ui_set_note(pane, "", UI_C(TEXT2));
        svc_state(v);
    }
    v->form = ui_set_form(s, pane, key);
    /* In the keyboard's order after the form: Tab goes through the
     * settings first. */
    if (action_btn) {
        lv_obj_add_event_cb(action_btn, svc_action_cb, LV_EVENT_CLICKED, v);
        lv_group_add_obj(ui_set_group(s), action_btn);
    }
    return v;
}

static void *ddns_open(ui_set_t *s, lv_obj_t *pane)
{
    return svc_open(s, pane, 0, "ddns", NULL, "Update now");
}

static void *smtp_open(ui_set_t *s, lv_obj_t *pane)
{
    return svc_open(s, pane, 1, "smtp", NULL, "Send test mail");
}

static void *alarm_open(ui_set_t *s, lv_obj_t *pane)
{
    return svc_open(s, pane, 2, "alarm", NULL, NULL);
}

static void *upnp_open(ui_set_t *s, lv_obj_t *pane)
{
    return svc_open(s, pane, 3, "upnp",
                    LV_SYMBOL_WARNING "  Not active yet: vigild has no UPnP client, so no port is forwarded. "
                                      "The settings are kept for when it has one.", NULL);
}

static int svc_dirty(void *st) { return st && ui_form_embed_dirty(((svc_t *)st)->form); }
static int svc_save(void *st) { return st ? ui_form_embed_save(((svc_t *)st)->form) : 0; }
static void svc_cancel(void *st)
{
    if (st) ui_form_embed_reset(((svc_t *)st)->form);
}
static void svc_close(void *st)
{
    if (!st) return;
    ui_form_embed_free(((svc_t *)st)->form);
    free(st);
}

/* ---- QoS --------------------------------------------------------------------------- */

/* vigil.yaml's `qos`: the type, and a DSCP per traffic class. "standard"
 * is the fields' defaults (the schema's), shown greyed; "custom", the
 * fields themselves. Saved like a form: through the store, applied at
 * once (main.c marks the open sockets again). */

#define QOS_N 4
static const char *const k_qos_keys[QOS_N] = { "video", "management", "camera", "alarm" };
static const char *const k_qos_types[] = { "off", "standard", "custom" };

typedef struct {
    int       sec;                  /* the section's index in the model */
    int       type;                 /* stored: index in k_qos_types */
    int       val[QOS_N], def[QOS_N];   /* stored custom values; the defaults */
    lv_obj_t *type_dd, *classes, *classes_title, *off_note;
    lv_obj_t *num[QOS_N], *name[QOS_N];
} qos_t;

/* The common names of DSCP values (RFC 2474, 2597, 3246, 5865, 8622). */
static const char *dscp_name(int v)
{
    static const char *const af[] = { "AF11", "AF12", "AF13", "AF21", "AF22", "AF23",
                                      "AF31", "AF32", "AF33", "AF41", "AF42", "AF43" };
    static const char *const cs[] = { "CS0 (best effort)", "CS1", "CS2", "CS3", "CS4", "CS5", "CS6", "CS7" };
    if (v % 8 == 0 && v >= 0 && v <= 56) return cs[v / 8];
    if (v >= 10 && v <= 38 && v % 2 == 0 && (v % 8) != 0 && (v % 8) <= 6) return af[(v / 8 - 1) * 3 + (v % 8) / 2 - 1];
    if (v == 46) return "EF";
    if (v == 44) return "VOICE-ADMIT";
    if (v == 1) return "LE (lower effort)";
    return "";
}

static int qos_field(const qos_t *q, const char *key)
{
    const ui_section_t *u = &ui_model()->sections[q->sec];
    for (int f = 0; f < u->n_fields; ++f)
        if (!strcmp(u->fields[f].key, key)) return f;
    return -1;
}

static void qos_name_cb(lv_event_t *ev)
{
    lv_obj_t *name = lv_event_get_user_data(ev);
    lv_obj_t *ta = lv_event_get_target(ev);
    lv_label_set_text(name, dscp_name(ui_set_number_get(ta)));
}

static void qos_set_num(qos_t *q, int k, int v)
{
    char t[8];
    snprintf(t, sizeof t, "%d", v);
    lv_textarea_set_text(q->num[k], t);
    lv_label_set_text(q->name[k], dscp_name(v));
}

/* The rows as the type chosen shows them. */
static void show_if(lv_obj_t *o, int shown)
{
    if (shown) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void qos_show(qos_t *q, int type)
{
    show_if(q->classes, type != 0);
    show_if(q->classes_title, type != 0);
    show_if(q->off_note, type == 0);
    for (int k = 0; k < QOS_N; ++k) {
        qos_set_num(q, k, type == 1 ? q->def[k] : q->val[k]);
        lv_obj_t *box = lv_obj_get_parent(q->num[k]);
        for (uint32_t i = 0; i < lv_obj_get_child_count(box); ++i) {
            lv_obj_t *c = lv_obj_get_child(box, (int32_t)i);
            if (type == 2) lv_obj_remove_state(c, LV_STATE_DISABLED);
            else lv_obj_add_state(c, LV_STATE_DISABLED);
        }
    }
}

static void qos_type_cb(lv_event_t *ev)
{
    qos_t *q = lv_event_get_user_data(ev);
    qos_show(q, (int)lv_dropdown_get_selected(q->type_dd));
}

static void qos_load(qos_t *q)
{
    const ui_section_t *u = &ui_model()->sections[q->sec];
    char v[32];
    int f = qos_field(q, "type");
    q->type = 0;
    if (f >= 0) {
        ui_model()->get_value(q->sec, -1, f, v, sizeof v);
        for (int i = 0; i < 3; ++i)
            if (!strcmp(v, k_qos_types[i])) q->type = i;
    }
    for (int k = 0; k < QOS_N; ++k) {
        f = qos_field(q, k_qos_keys[k]);
        q->val[k] = q->def[k] = 0;
        if (f < 0) continue;
        ui_model()->get_value(q->sec, -1, f, v, sizeof v);
        q->val[k] = atoi(v);
        q->def[k] = u->fields[f].def ? atoi(u->fields[f].def) : 0;
    }
    lv_dropdown_set_selected(q->type_dd, (uint32_t)q->type);
    qos_show(q, q->type);
}

static void *qos_open(ui_set_t *s, lv_obj_t *pane)
{
    int sec = ui_set_section_index("qos");
    if (sec < 0) return NULL;
    qos_t *q = calloc(1, sizeof *q);
    if (!q) return NULL;
    q->sec = sec;
    static const char *const labels[QOS_N] = { "Video", "Management", "Cameras", "Alarms and events" };
    static const char *const helps[QOS_N] = {
        "The zero channel's picture: the WebSocket of the web server (web.listen, port 80).",
        "The HTTP API, the web pages and logins, the ONVIF service and WS-Discovery, DDNS, camera searches.",
        "vigild's connections to cameras: RTSP and NETIP control, and the ACKs of the video they send.",
        "The API's event WebSocket, the alarm center's POSTs and the mails.",
    };
    lv_obj_t *sec_o = ui_set_section(pane, "QoS (DSCP marking)");
    q->type_dd = ui_set_dropdown(s, ui_set_row(sec_o, "QoS type",
                                               "Marks the packets vigild sends with a DSCP class, for switches "
                                               "and routers that queue by it."),
                                 "Off\nStandard (RFC 4594)\nCustom", 0, 320);
    lv_obj_add_event_cb(q->type_dd, qos_type_cb, LV_EVENT_VALUE_CHANGED, q);
    q->off_note = ui_set_note(pane, "Nothing is marked: every packet goes with DSCP 0 (best effort).", UI_C(TEXT2));
    q->classes = ui_set_section(pane, "DSCP per traffic class");
    q->classes_title = lv_obj_get_child(pane, lv_obj_get_index(q->classes) - 1);
    for (int k = 0; k < QOS_N; ++k) {
        q->num[k] = ui_set_number(s, ui_set_row(q->classes, labels[k], helps[k]), 0, 0, 63);
        q->name[k] = ui_label(lv_obj_get_parent(q->num[k]), "", VG_FONT_MD_LV, UI_C(TEXT));
        lv_obj_add_event_cb(q->num[k], qos_name_cb, LV_EVENT_VALUE_CHANGED, q->name[k]);
    }
    ui_set_note(pane, "Applied at once to new connections, to the cameras' sessions and to open WebSockets; an "
                      "open web page or API connection keeps its old mark until it reconnects. Only vigild's own "
                      "packets are marked, not other programs' (sshd, ntpd).", UI_C(TEXT2));
    qos_load(q);
    return q;
}

static int qos_dirty(void *st)
{
    qos_t *q = st;
    if (!q) return 0;
    int type = (int)lv_dropdown_get_selected(q->type_dd);
    if (type != q->type) return 1;
    if (type != 2) return 0;
    for (int k = 0; k < QOS_N; ++k)
        if (ui_set_number_get(q->num[k]) != q->val[k]) return 1;
    return 0;
}

static int qos_save(void *st)
{
    qos_t *q = st;
    if (!q) return 0;
    const ui_section_t *u = &ui_model()->sections[q->sec];
    int type = (int)lv_dropdown_get_selected(q->type_dd);
    char buf[8][32];
    const char *v[8];
    if (u->n_fields > 8) return -1;
    for (int f = 0; f < u->n_fields; ++f) {
        ui_model()->get_value(q->sec, -1, f, buf[f], sizeof buf[f]);
        if (!strcmp(u->fields[f].key, "type")) snprintf(buf[f], sizeof buf[f], "%s", k_qos_types[type]);
        /* Only "custom" takes the numbers: the others keep the stored ones
         * for when it is chosen again. */
        for (int k = 0; k < QOS_N && type == 2; ++k)
            if (!strcmp(u->fields[f].key, k_qos_keys[k]))
                snprintf(buf[f], sizeof buf[f], "%d", ui_set_number_get(q->num[k]));
        v[f] = buf[f];
    }
    unsigned gen = ui_model()->config_generation ? ui_model()->config_generation() : 0;
    ui_errors_t errs;
    memset(&errs, 0, sizeof errs);
    ui_save_rc_t rc = ui_model()->save(q->sec, 1, v, NULL, gen, &errs);
    if (rc != UI_SAVE_OK && rc != UI_SAVE_UNSYNCED) {
        ui_toast(UI_TOAST_BAD, "Not saved: %s", errs.n ? errs.e[0].msg : "the settings changed meanwhile");
        return -1;
    }
    ui_toast(UI_TOAST_OK, "QoS: %s", type == 0 ? "off" : "applied");
    qos_load(q);
    return 0;
}

static void qos_cancel(void *st)
{
    if (st) qos_load(st);
}

/* ---- P2P --------------------------------------------------------------------------- */

static void *p2p_open(ui_set_t *s, lv_obj_t *pane)
{
    ui_set_hide_actions(s);
    lv_obj_t *sec = ui_set_section(pane, NULL);
    ui_label(sec, "P2P is not available", VG_FONT_LG_LV, UI_C(TEXT));
    ui_set_note(sec, "vigil has no P2P (cloud relay) service. Reach the recorder over the network: its "
                     "HTTP API, a VPN, or DDNS with a forwarded port.", UI_C(TEXT2));
    return NULL;
}

/* ---- the screen ------------------------------------------------------------------------ */

static void free_st(void *st) { free(st); }

static ui_set_item_t g_items[10];

void ui_network_open(int first)
{
    const ui_set_item_t items[] = {
        { TR(S_ETHERNET), &ui_icon_nav_ethernet, eth_open, eth_dirty, eth_save, eth_cancel, eth_close },
        { TR(S_WIFI), &ui_icon_nav_wifi, wifi_open, wifi_dirty, wifi_save, wifi_cancel, free_st },
        { TR(S_LTE), &ui_icon_nav_lte, lte_open, NULL, NULL, NULL, NULL },
        { TR(S_PORTS), &ui_icon_nav_ports, ports_open, ports_dirty, ports_save, ports_cancel, ports_close },
        { TR(S_DDNS), &ui_icon_nav_ddns, ddns_open, svc_dirty, svc_save, svc_cancel, svc_close },
        { TR(S_UPNP), &ui_icon_nav_upnp, upnp_open, svc_dirty, svc_save, svc_cancel, svc_close },
        { TR(S_SMTP), &ui_icon_nav_smtp, smtp_open, svc_dirty, svc_save, svc_cancel, svc_close },
        { TR(S_ALARM_CENTER), &ui_icon_nav_alarm, alarm_open, svc_dirty, svc_save, svc_cancel, svc_close },
        { TR(S_QOS), &ui_icon_nav_qos, qos_open, qos_dirty, qos_save, qos_cancel, free_st },
        { TR(S_P2P), &ui_icon_nav_p2p, p2p_open, NULL, NULL, NULL, NULL },
    };
    memcpy(g_items, items, sizeof items);
    ui_settings_open(TR(S_NETWORK), g_items, (int)(sizeof items / sizeof items[0]), first);
}
