/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_ui.c — the on-screen UI without a board: LVGL renders into memory,
 * a pipe stands in for the mouse (through the real evdev reader), the
 * settings go through the real configuration store (a temporary file),
 * and the channels are fakes. Each step checks widget states and writes
 * a screenshot (PPM) that tests/ui_compare.py compares with the reference
 * images in tests/ui/.
 *
 *   test_ui OUTDIR
 */
#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>

#include "lvgl.h"

#include "channel.h"
#include "config.h"
#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "hal_ui.h"
#include "input_evdev.h"
#include "ui_app.h"
#include "ui_bridge.h"
#include "ui_layout.h"

#define W 1280
#define H 720

static int g_fail, g_checks;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- a platform: four decoders, a UI layer with black (video) holes -------------------- */

static const hal_caps_t g_caps = { .name = "test", .display = 1, .decoder = 1, .dec_max_w = 1920,
                                   .dec_max_h = 1080, .max_channels = 4 };
const hal_caps_t *hal_caps(void) { return &g_caps; }
int  hal_dec_open(int ch, vg_codec_t c, const vg_stream_info_t *si) { (void)ch; (void)c; (void)si; return -1; }
int  hal_dec_send(int ch, const vg_au_t *au) { (void)ch; (void)au; return -1; }
void hal_dec_reset(int ch) { (void)ch; }
void hal_dec_close(int ch) { (void)ch; }
int  hal_display_set_tile(int ch, const vg_rect_t *r) { (void)ch; (void)r; return 0; }
void hal_display_clear_tile(int ch) { (void)ch; }
int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }
int  hal_cap_signal(int in) { (void)in; return 0; }
void hal_ui_hole(lv_color_t *c, lv_opa_t *o)
{
    *c = lv_color_hex(0x000000);
    *o = LV_OPA_COVER;
}

/* ---- the model: the real bridge, fake channels -------------------------------------- */

static ui_model_t g_model;
static ui_rect_t  g_tiles[UI_MAX_CHANNELS];
static int        g_tile_calls;

static void fake_status(int idx, ui_chan_t *st)
{
    switch (idx) {
    case 0:
        *st = (ui_chan_t){ .configured = 1, .live = 1, .recording = 1, .rec_bytes = 734u << 20,
                           .name = "Gate", .codec = "H.265", .width = 1920, .height = 1080, .fps = 25 };
        break;
    case 1:
        *st = (ui_chan_t){ .configured = 1, .name = "Yard", .state = "CONNECTING" };
        break;
    case 2:
        *st = (ui_chan_t){ .configured = 1, .state = "connect: Connection refused" };
        break;
    default:
        break;
    }
}

static const ui_event_t k_events[] = {
    { 1, 0, "online" }, { 3, 0, "offline" }, { 0, 0, "config applied" },
};
static int g_ev;

static time_t fake_now(void) { return 1795703525; }     /* 2026-11-26 14:32:05 UTC */

static int fake_event(ui_event_t *ev)
{
    if (g_ev >= (int)(sizeof k_events / sizeof k_events[0])) return 0;
    *ev = k_events[g_ev++];
    ev->time = fake_now() - 60 + 20 * g_ev;
    return 1;
}

static void fake_tiles(const ui_rect_t *t, int n)
{
    memset(g_tiles, 0, sizeof g_tiles);
    memcpy(g_tiles, t, sizeof *t * (size_t)n);
    ++g_tile_calls;
}

static int fake_info(ui_info_t *info, int max)
{
    static const ui_info_t k[] = {
        { "Version", "vigild 0.1.0" }, { "Platform", "test" },
        { "Decoder", "H.264 / H.265, up to 1920x1080" }, { "Channels", "4" },
        { "Uptime", "0 d 01:02:03" }, { "Memory", "71 MiB available of 113 MiB; vigild uses 9 MiB" },
        { "Cameras", "3 configured, 1 live" }, { "Recording", "off" },
    };
    int n = (int)(sizeof k / sizeof k[0]);
    if (n > max) n = max;
    memcpy(info, k, sizeof *info * (size_t)n);
    return n;
}

/* ---- display, clock and mouse ------------------------------------------------------------ */

static uint32_t g_fb[W * H];
static uint32_t g_ms;
static int      g_mouse[2];             /* pipe: test -> evdev reader */
static int      g_px = W / 2, g_py = H / 2;
static const char *g_out;

static uint32_t fake_ms(void) { return g_ms; }

static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a;
    (void)px;
    lv_display_flush_ready(d);
}

static void run(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        g_ms += 5;
        lv_timer_handler();
    }
}

static void ev(int type, int code, int value)
{
    struct input_event e = { .type = (unsigned short)type, .code = (unsigned short)code, .value = value };
    if (write(g_mouse[1], &e, sizeof e) != (ssize_t)sizeof e) abort();
}

static void move_to(int x, int y)
{
    ev(EV_REL, REL_X, x - g_px);
    ev(EV_REL, REL_Y, y - g_py);
    ev(EV_SYN, SYN_REPORT, 0);
    g_px = x < 0 ? 0 : x > W - 1 ? W - 1 : x;
    g_py = y < 0 ? 0 : y > H - 1 ? H - 1 : y;
    run(60);
}

static void button(int code)
{
    ev(EV_KEY, code, 1);
    ev(EV_SYN, SYN_REPORT, 0);
    run(40);
    ev(EV_KEY, code, 0);
    ev(EV_SYN, SYN_REPORT, 0);
    run(60);
}

static void click_at(int x, int y)
{
    move_to(x, y);
    button(BTN_LEFT);
}

static void shot(const char *name)
{
    run(400);                           /* animations end, timers settle */
    lv_refr_now(NULL);
    char path[512];
    snprintf(path, sizeof path, "%s/%s.ppm", g_out, name);
    FILE *f = fopen(path, "wb");
    if (!f) {
        CHECK(0, "cannot write %s", path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    static uint8_t row[W * 3];
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint32_t p = g_fb[y * W + x];
            row[x * 3] = (uint8_t)(p >> 16);
            row[x * 3 + 1] = (uint8_t)(p >> 8);
            row[x * 3 + 2] = (uint8_t)p;
        }
        fwrite(row, 1, sizeof row, f);
    }
    fclose(f);
}

/* ---- finding widgets ------------------------------------------------------------------------- */

static int visible(lv_obj_t *o)
{
    for (; o; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return 0;
    return 1;
}

static lv_obj_t *find_in(lv_obj_t *o, const char *text, int contains)
{
    if (lv_obj_check_type(o, &lv_label_class) && visible(o)) {
        const char *t = lv_label_get_text(o);
        if (contains ? strstr(t, text) != NULL : !strcmp(t, text)) return o;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
        lv_obj_t *r = find_in(lv_obj_get_child(o, (int32_t)i), text, contains);
        if (r) return r;
    }
    return NULL;
}

/* A visible label: on the top layer (popups, toasts) or the screen. */
static lv_obj_t *find(const char *text, int contains)
{
    lv_obj_t *o = find_in(lv_layer_top(), text, contains);
    return o ? o : find_in(lv_screen_active(), text, contains);
}

/* The nth (0-based) visible label with exactly this text, in tree order. */
static lv_obj_t *nth_in(lv_obj_t *o, const char *text, int *n)
{
    if (lv_obj_check_type(o, &lv_label_class) && visible(o) && !strcmp(lv_label_get_text(o), text) &&
        (*n)-- == 0)
        return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
        lv_obj_t *r = nth_in(lv_obj_get_child(o, (int32_t)i), text, n);
        if (r) return r;
    }
    return NULL;
}

static lv_obj_t *find_nth(lv_obj_t *root, const char *text, int n) { return nth_in(root, text, &n); }

static void center_of(lv_obj_t *o, int *x, int *y)
{
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    *x = (a.x1 + a.x2) / 2;
    *y = (a.y1 + a.y2) / 2;
}

/* Click the widget showing `text` (a button's label clicks the button). */
static int click_text(const char *text)
{
    lv_obj_t *l = find(text, 0);
    if (!l) l = find(text, 1);
    CHECK(l, "no '%s' on screen", text);
    if (!l) return -1;
    int x, y;
    center_of(l, &x, &y);
    click_at(x, y);
    return 0;
}

static lv_obj_t *find_textarea(lv_obj_t *o, int nth, int *seen)
{
    if (lv_obj_check_type(o, &lv_textarea_class) && (*seen)++ == nth) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
        lv_obj_t *r = find_textarea(lv_obj_get_child(o, (int32_t)i), nth, seen);
        if (r) return r;
    }
    return NULL;
}

static lv_obj_t *textarea(int nth)
{
    int seen = 0;
    return find_textarea(lv_screen_active(), nth, &seen);
}

/* ---- the steps ---------------------------------------------------------------------------- */

#define SAVE LV_SYMBOL_SAVE "  Save"

static void test_layout_math(void)
{
    ui_cell_t c[UI_LAYOUT_MAX];
    CHECK(ui_layout_cells(W, H, 4, 0, 4, c) == 4, "4 cells");
    CHECK(c[3].x == 640 && c[3].y == 40 + 300 && c[3].w == 640 && c[3].h == 300, "cell 4 %d,%d %dx%d",
          c[3].x, c[3].y, c[3].w, c[3].h);
    CHECK(ui_layout_pages(1, 4) == 4 && ui_layout_pages(4, 4) == 1 && ui_layout_pages(9, 10) == 2, "pages");
    ui_layout_cells(W, H, 1, 2, 4, c);
    CHECK(c[0].ch == 2 && c[0].w == W && c[0].h == 600, "page 3 of 1x1 shows CAM3");
    ui_layout_cells(W, H, 9, 0, 4, c);
    int area = 0;
    for (int i = 0; i < 9; ++i) area += c[i].w * c[i].h;
    CHECK(area == W * 600 && c[4].ch == -1, "3x3 covers the grid, no channel past 4");
    CHECK(!ui_layout_valid(2) && ui_layout_cells(W, H, 2, 0, 4, c) == 0, "2 is not a layout");
}

static void test_live(void)
{
    CHECK(find("Gate", 0) && find("Yard", 0) && find("CAM 3", 0) && find("CAM 4", 0), "tile titles");
    CHECK(find("H.265  1920x1080  25 fps", 0), "stream line of a live tile");
    CHECK(find("Connecting...", 0) && find("No video", 0) && find("No camera", 0), "tile states");
    CHECK(find("LIVE 1/3", 1), "header totals");
    CHECK(find("2026-11-26  14:32:05", 0), "clock");
    CHECK(find("offline", 1), "event strip");
    CHECK(!find("Main menu", 0), "navigation bar hidden at first");
    shot("live");
}

static void test_navbar(void)
{
    move_to(W / 2, H + 50);                     /* clamped to the bottom edge */
    lv_obj_t *menu = find("Main menu", 1);
    int x, y;
    CHECK(menu, "navigation bar on the bottom edge");
    if (menu) {
        center_of(menu, &x, &y);
        CHECK(y > H - VG_SIZE_NAVBAR, "bar at the bottom (y %d)", y);
    }
    shot("navbar");
    move_to(W / 2, H / 2);
    run(VG_DURATION_NAVBAR_HIDE + 300);
    menu = find("Main menu", 1);
    if (menu) center_of(menu, &x, &y);
    CHECK(!menu || y >= H, "bar hidden %d ms after the pointer left", VG_DURATION_NAVBAR_HIDE);
}

static void test_context_and_full_screen(void)
{
    move_to(300, 200);                          /* over CAM1 */
    button(BTN_RIGHT);
    run(50);
    CHECK(ui_popup_is_open() && find("Full screen: Gate", 1) && find("Pin navigation bar", 1),
          "context menu with the tile's channel");
    shot("context");
    button(BTN_RIGHT);                          /* right again: closed */
    run(50);
    CHECK(!ui_popup_is_open(), "right button closes the context menu");

    int calls = g_tile_calls;
    click_at(900, 200);                         /* CAM2 */
    run(100);
    click_at(900, 200);
    run(50);
    CHECK(g_tile_calls == calls + 1 && g_tiles[1].w == W && g_tiles[1].h == 600 && g_tiles[0].w == 0,
          "double click: CAM2 full screen (%dx%d)", g_tiles[1].w, g_tiles[1].h);
    CHECK(find("Yard", 0) && !find("Gate", 0), "one tile");
    shot("fullscreen");
    click_at(640, 300);
    run(100);
    click_at(640, 300);
    run(50);
    CHECK(g_tiles[0].w == 640 && g_tiles[1].x == 640 && g_tiles[1].w == 640, "double click again: 2x2");

    /* Two clicks too far apart are not a double click. */
    calls = g_tile_calls;
    click_at(300, 200);
    run(VG_DURATION_DOUBLE_CLICK + 100);
    click_at(300, 200);
    run(50);
    CHECK(g_tile_calls == calls, "slow clicks keep the layout");

    /* Paging in single view through the context menu. */
    move_to(300, 200);
    button(BTN_RIGHT);
    click_text("Single view");
    CHECK(g_tiles[0].w == W && g_tiles[1].w == 0, "single view shows CAM1");
    button(BTN_RIGHT);
    click_text("Next page");
    CHECK(g_tiles[1].w == W && g_tiles[0].w == 0, "next page shows CAM2");
    button(BTN_RIGHT);
    click_text("2x2 view");
    CHECK(g_tiles[0].w == 640 && g_tiles[3].w == 640, "back to 2x2");
}

static void test_menu_and_settings(void)
{
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    CHECK(ui_page_top() && find("Recording", 0) && find("Cameras", 0) && find("System", 0), "main menu");
    shot("menu");

    lv_obj_t *rec = lv_obj_get_parent(find("Recording", 0));
    int rx, ry;
    center_of(rec, &rx, &ry);
    move_to(rx, ry);
    CHECK(lv_obj_has_state(rec, LV_STATE_HOVERED), "the tile under the pointer is hovered");
    click_text("Recording");
    CHECK(find("Recording directory", 0) && find("Segment length (s)", 0), "recording form from the schema");
    lv_obj_t *path = textarea(0), *seg = textarea(1);
    CHECK(path && seg && !strcmp(lv_textarea_get_text(seg), "60"), "values from the store");

    /* Save an invalid value: nothing changes, the store's error shows
     * under its field. */
    unsigned gen = config_generation();
    lv_textarea_set_text(path, "rec");
    click_text(SAVE);
    run(50);
    CHECK(config_generation() == gen, "invalid: not saved");
    CHECK(find("must be an absolute path", 1), "error under the field");
    CHECK(find("Not saved: 1 problem", 1), "toast");
    shot("error");

    /* A valid change: saved through the store, the generation moves. */
    lv_textarea_set_text(path, "/tmp");
    click_text(LV_SYMBOL_PLUS);
    click_text(LV_SYMBOL_PLUS);
    click_text(SAVE);
    run(50);
    vigil_config_t c;
    config_get(&c);
    CHECK(config_generation() == gen + 1 && c.record.segment == 62 && !strcmp(c.record.path, "/tmp"),
          "saved: segment %d, path %s", c.record.segment, c.record.path);
    CHECK(!find("must be an absolute path", 1) && find("Saved", 1), "errors gone, toast");
    shot("saved");
    button(BTN_RIGHT);                          /* back to the menu */
    CHECK(find("Main menu", 0), "back from the form");

    click_text("HTTP API");
    CHECK(find("applies after restart", 1), "non-live setting marked");
    click_text("Listen on");                    /* the label: no keyboard */
    lv_obj_t *listen = textarea(0);
    int x, y;
    center_of(listen, &x, &y);
    click_at(x, y);
    CHECK(find("Listen on", 0) && lv_obj_check_type(lv_obj_get_child(lv_screen_active(), -1),
                                                    &lv_keyboard_class) &&
          visible(lv_obj_get_child(lv_screen_active(), -1)), "keyboard on a text field");
    run(VG_DURATION_TOAST);                     /* the "Saved" toast is gone */
    shot("keyboard");
    button(BTN_RIGHT);                          /* closes the keyboard */
    button(BTN_RIGHT);                          /* leaves the page (unchanged) */
    CHECK(find("System", 0) && !ui_popup_is_open(), "back in the menu");
}

static void test_cameras(void)
{
    click_text("Cameras");
    CHECK(find("rtsp://admin:***@192.168.1.10/stream1", 0), "URL redacted as in the API");
    shot("cameras");
    lv_obj_t *edit = find("Edit", 1);
    int x, y;
    center_of(edit, &x, &y);
    click_at(x, y);
    lv_obj_t *url = textarea(0), *name = textarea(1);
    CHECK(url && !strcmp(lv_textarea_get_text(url), "rtsp://admin:***@192.168.1.10/stream1"), "form shows ***");
    lv_textarea_set_text(name, "Front gate");
    click_text(SAVE);
    run(50);
    vigil_config_t c;
    config_get(&c);
    CHECK(!strcmp(c.cameras[0].name, "Front gate") &&
          !strcmp(c.cameras[0].url, "rtsp://admin:secret@192.168.1.10/stream1"),
          "name saved, password kept: %s", c.cameras[0].url);

    /* A change, then back: asked first. */
    lv_textarea_set_text(name, "Changed");
    button(BTN_RIGHT);
    CHECK(ui_popup_is_open() && find("Discard the changes", 1), "discard question");
    click_text("Discard");
    CHECK(find("Add", 1) && find("Front gate", 0), "back in the list, updated");

    /* Remove the third camera, confirmed. */
    lv_obj_t *rm = find_nth(lv_screen_active(), LV_SYMBOL_TRASH "  Remove", 2);
    CHECK(rm, "third Remove button");
    if (rm) {
        center_of(rm, &x, &y);
        click_at(x, y);
        CHECK(ui_popup_is_open() && find("Remove item 3 from Cameras", 1), "remove question");
        lv_obj_t *yes = find_in(lv_layer_top(), "Remove", 0);
        CHECK(yes, "Remove button in the question");
        if (yes) {
            center_of(yes, &x, &y);
            click_at(x, y);
        }
    }
    config_get(&c);
    CHECK(c.n_cameras == 2, "camera removed (%d left)", c.n_cameras);
    button(BTN_RIGHT);
}

static void test_system(void)
{
    run(VG_DURATION_TOAST);
    click_text("System");
    CHECK(find("vigild 0.1.0", 0) && find("0 d 01:02:03", 0), "system page");
    shot("system");
    button(BTN_RIGHT);
    button(BTN_RIGHT);
    CHECK(!ui_page_top(), "back on the live view");
}

/* Two cameras whose URLs differ only in the password redact the same way;
 * saving one must not give it (or the other) the other's password. */
static void test_same_redacted_urls(void)
{
    static const char cams[] = "[{\"url\":\"rtsp://admin:one@192.168.1.20/s\",\"name\":\"A\"},"
                               "{\"url\":\"rtsp://admin:two@192.168.1.20/s\",\"name\":\"B\"}]";
    schema_errors_t se = { 0 };
    CHECK(config_put_json("cameras", cams, strlen(cams), &se) == CONFIG_OK, "two cameras");
    run(300);
    move_to(W / 2, H - 1);
    run(300);
    click_text("Main menu");
    click_text("Cameras");
    lv_obj_t *edit = find_nth(lv_screen_active(), LV_SYMBOL_EDIT "  Edit", 1);
    int x, y;
    center_of(edit, &x, &y);
    click_at(x, y);
    lv_textarea_set_text(textarea(1), "B2");
    click_text(SAVE);
    vigil_config_t c;
    config_get(&c);
    CHECK(!strcmp(c.cameras[0].url, "rtsp://admin:one@192.168.1.20/s") &&
          !strcmp(c.cameras[1].url, "rtsp://admin:two@192.168.1.20/s") && !strcmp(c.cameras[1].name, "B2"),
          "each camera keeps its own password: %s, %s", c.cameras[0].url, c.cameras[1].url);

    /* Changed through the API while the form is open: refused, nothing
     * of the API's change lost, back to the (current) list. */
    lv_textarea_set_text(textarea(1), "B3");
    static const char cams2[] = "[{\"url\":\"rtsp://admin:two@192.168.1.20/s\",\"name\":\"B\"}]";
    CHECK(config_put_json("cameras", cams2, strlen(cams2), &se) == CONFIG_OK, "API change");
    click_text(SAVE);
    config_get(&c);
    CHECK(c.n_cameras == 1 && !strcmp(c.cameras[0].name, "B"), "stale camera form not saved");
    CHECK(find("changed elsewhere", 1) && find("Add", 1), "toast, back in the list");
    button(BTN_RIGHT);                          /* to the menu */
}

static void test_stale_and_cancel(void)
{
    click_text("Recording");
    lv_obj_t *seg = textarea(1);
    lv_textarea_set_text(seg, "300");
    schema_errors_t se = { 0 };
    CHECK(config_put_json("record", "{\"keep\":7}", 10, &se) == CONFIG_OK, "API change");
    click_text(SAVE);
    vigil_config_t c;
    config_get(&c);
    CHECK(c.record.keep == 7 && c.record.segment == 60, "stale form not saved (keep %d, segment %d)",
          c.record.keep, c.record.segment);
    CHECK(!strcmp(lv_textarea_get_text(textarea(2)), "7") && !strcmp(lv_textarea_get_text(seg), "60"),
          "the form shows the settings now");
    lv_textarea_set_text(seg, "120");
    click_text("Cancel");
    CHECK(ui_popup_is_open() && find("Discard the changes", 1), "Cancel with changes asks");
    click_text("Discard");
    CHECK(find("Main menu", 0) && !find("Recording directory", 0), "discarded");
    button(BTN_RIGHT);
    CHECK(!ui_page_top(), "back on the live view");
}

/* Two pointing devices: one's release must not end the other's press;
 * unplugging a device that is pressed releases. */
static void test_two_mice(lv_indev_t *ptr)
{
    int b[2];
    if (pipe(b) < 0) return;
    fcntl(b[0], F_SETFL, O_NONBLOCK);
    input_evdev_attach(b[0], "second mouse", 0, 0, 0, 0, 0);
    struct input_event dn = { .type = EV_KEY, .code = BTN_LEFT, .value = 1 }, up = dn,
                       syn = { .type = EV_SYN, .code = SYN_REPORT };
    up.value = 0;
    move_to(640, 30);                           /* the header: nothing to click */
    ev(EV_KEY, BTN_LEFT, 1);
    ev(EV_SYN, SYN_REPORT, 0);
    run(40);
    if (write(b[1], &dn, sizeof dn) < 0 || write(b[1], &syn, sizeof syn) < 0) abort();
    run(40);
    ev(EV_KEY, BTN_LEFT, 0);
    ev(EV_SYN, SYN_REPORT, 0);
    run(40);
    CHECK(lv_indev_get_state(ptr) == LV_INDEV_STATE_PRESSED, "still pressed by the second mouse");
    if (write(b[1], &up, sizeof up) < 0 || write(b[1], &syn, sizeof syn) < 0) abort();
    run(40);
    CHECK(lv_indev_get_state(ptr) == LV_INDEV_STATE_RELEASED, "released by both");
    if (write(b[1], &dn, sizeof dn) < 0 || write(b[1], &syn, sizeof syn) < 0) abort();
    run(40);
    close(b[1]);                                /* unplugged while pressed */
    run(80);
    CHECK(lv_indev_get_state(ptr) == LV_INDEV_STATE_RELEASED, "unplugging releases");
}

int main(int argc, char **argv)
{
    g_out = argc > 1 ? argv[1] : ".";
    setenv("TZ", "UTC", 1);
    tzset();
    test_layout_math();

    char path[512];
    snprintf(path, sizeof path, "%s/vigil.yaml", g_out);
    unlink(path);
    FILE *f = fopen(path, "w");
    fputs("version: 1\n"
          "cameras:\n"
          "  - url: \"rtsp://admin:secret@192.168.1.10/stream1\"\n"
          "    name: Gate\n"
          "  - url: \"netip://192.168.1.11/\"\n"
          "    name: Yard\n"
          "  - url: \"rtsp://192.168.1.12/live\"\n"
          "    enabled: false\n", f);
    fclose(f);
    CHECK(config_init(path) == 0, "config");

    lv_init();
    lv_tick_set_cb(fake_ms);
    lv_display_t *disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(disp, g_fb, NULL, sizeof g_fb, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, flush_cb);

    if (ui_bridge_init(&g_model) < 0) return 1;
    g_model.chan_status = fake_status;
    g_model.next_event = fake_event;
    g_model.show_tiles = fake_tiles;
    g_model.now = fake_now;
    g_model.system_info = fake_info;
    ui_app_create(&g_model);

    if (pipe(g_mouse) < 0) return 1;
    fcntl(g_mouse[0], F_SETFL, O_NONBLOCK);
    lv_indev_t *ptr = input_evdev_create(disp, ui_app_back, 0);
    ui_app_set_pointer(ptr);
    input_evdev_attach(g_mouse[0], "test mouse", 0, 0, 0, 0, 0);
    run(300);

    test_live();
    test_navbar();
    test_context_and_full_screen();
    test_menu_and_settings();
    test_cameras();
    test_system();
    test_same_redacted_urls();
    test_stale_and_cancel();
    test_two_mice(ptr);

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    printf("LVGL heap: %u KiB used at most of %u KiB\n", (unsigned)(mon.max_used >> 10),
           (unsigned)(mon.total_size >> 10));
    input_evdev_delete();
    close(g_mouse[1]);
    unlink(path);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
