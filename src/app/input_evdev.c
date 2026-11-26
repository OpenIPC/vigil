/* SPDX-License-Identifier: GPL-3.0-or-later */
/* input_evdev.c — mice, touch screens and keyboards for the UI, from Linux
 * evdev.
 *
 * Every /dev/input/event* that is a pointer (relative X/Y with a left
 * button, or absolute X/Y) feeds one LVGL pointer, and every keyboard
 * (letter keys and Enter) one LVGL keypad: plugging in a second mouse or
 * keyboard just works, and a device plugged in later is found by a rescan
 * every 2 s (no udev on these boards). The left button (or a touch)
 * clicks; the right button calls hooks.right, which LVGL's own evdev
 * driver cannot do: it reports only BTN_MOUSE/BTN_TOUCH, and no keys. The
 * pointer is drawn by LVGL (ui_cursor.c), shown while a pointer is there.
 *
 * Keys are translated here, with a US layout: Shift, Caps Lock (letters
 * only) and Num Lock (the keypad's digits, or its arrows, Home, End, Page
 * Up/Down and Delete; on at start), as ui_keys.h describes them. No XKB:
 * other layouts type US characters. Autorepeat is the kernel's (EV_KEY
 * value 2, at its rate). Each press reaches LVGL as a press and a release,
 * so a key held down on one keyboard never swallows a key on another, and
 * a keyboard unplugged with Shift held leaves nothing held.
 *
 * Here, not in src/core (which must build without Linux-only headers) and
 * not in src/ui (LVGL only): it is plain Linux, not vendor code, so every
 * platform with a display uses it unchanged.
 */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/input.h>

#include "input_evdev.h"
#include "log.h"
#include "ui_cursor.h"
#include "ui_keys.h"

#define DEVS_MAX   8
#define IGNORE_MAX 32
#define QUEUE_MAX  16
#define KEYS_MAX   32
#define RESCAN_MS  2000
#define INPUT_DIR  "/dev/input"

#define BITS_LONG       (8 * sizeof(unsigned long))
#define NBITS(n)        (((n) + BITS_LONG - 1) / BITS_LONG)
#define TEST_BIT(b, a)  (((a)[(b) / BITS_LONG] >> ((b) % BITS_LONG)) & 1)

/* Modifier keys held on one keyboard. */
enum { HELD_LSHIFT = 1, HELD_RSHIFT = 2, HELD_LCTRL = 4, HELD_RCTRL = 8, HELD_LALT = 16, HELD_RALT = 32 };

typedef struct {
    int      fd;
    char     path[64];
    ino_t    ino;
    int      ptr, kbd;              /* a pointer, a keyboard (or both) */
    int      abs;                   /* absolute X/Y (touch screen, tablet) */
    int      min_x, max_x, min_y, max_y;
    int      left;                  /* this device's button (or touch) is down */
    unsigned held;                  /* HELD_*: this keyboard's modifiers */
} dev_t_;

typedef struct {
    int x, y, pressed;
} press_t;

typedef struct {
    uint32_t key;
    unsigned mods;
} key_t_;

static struct {
    dev_t_        dev[DEVS_MAX];
    int           n, n_ptr, n_kbd;
    struct { char path[64]; ino_t ino; } ignored[IGNORE_MAX];
    int           n_ignored;
    int           w, h;
    int           x, y, left;       /* left: any device's button is down */
    press_t       queue[QUEUE_MAX]; /* button changes not yet given to LVGL */
    int           q_head, q_len;
    int           right;            /* right presses not yet handled */
    key_t_        keys[KEYS_MAX];   /* key presses not yet given to LVGL */
    int           k_head, k_len;
    uint32_t      k_last;           /* the key LVGL saw last */
    int           k_down;           /* ... and it still has to see its release */
    int           caps, numlock;
    lv_indev_t   *indev, *keypad;
    lv_obj_t     *cursor;
    lv_timer_t   *scan;
    input_hooks_t hooks;
} g;

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void cursor_visible(void)
{
    if (!g.cursor) return;
    if (g.n_ptr) lv_obj_remove_flag(g.cursor, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g.cursor, LV_OBJ_FLAG_HIDDEN);
}

/* ---- keys ------------------------------------------------------------------------------- */

/* US layout: the character without and with Shift. */
static const char k_chars[][2] = {
    [KEY_1] = { '1', '!' }, [KEY_2] = { '2', '@' }, [KEY_3] = { '3', '#' }, [KEY_4] = { '4', '$' },
    [KEY_5] = { '5', '%' }, [KEY_6] = { '6', '^' }, [KEY_7] = { '7', '&' }, [KEY_8] = { '8', '*' },
    [KEY_9] = { '9', '(' }, [KEY_0] = { '0', ')' }, [KEY_MINUS] = { '-', '_' }, [KEY_EQUAL] = { '=', '+' },
    [KEY_Q] = { 'q', 'Q' }, [KEY_W] = { 'w', 'W' }, [KEY_E] = { 'e', 'E' }, [KEY_R] = { 'r', 'R' },
    [KEY_T] = { 't', 'T' }, [KEY_Y] = { 'y', 'Y' }, [KEY_U] = { 'u', 'U' }, [KEY_I] = { 'i', 'I' },
    [KEY_O] = { 'o', 'O' }, [KEY_P] = { 'p', 'P' }, [KEY_LEFTBRACE] = { '[', '{' },
    [KEY_RIGHTBRACE] = { ']', '}' }, [KEY_A] = { 'a', 'A' }, [KEY_S] = { 's', 'S' },
    [KEY_D] = { 'd', 'D' }, [KEY_F] = { 'f', 'F' }, [KEY_G] = { 'g', 'G' }, [KEY_H] = { 'h', 'H' },
    [KEY_J] = { 'j', 'J' }, [KEY_K] = { 'k', 'K' }, [KEY_L] = { 'l', 'L' },
    [KEY_SEMICOLON] = { ';', ':' }, [KEY_APOSTROPHE] = { '\'', '"' }, [KEY_GRAVE] = { '`', '~' },
    [KEY_BACKSLASH] = { '\\', '|' }, [KEY_Z] = { 'z', 'Z' }, [KEY_X] = { 'x', 'X' },
    [KEY_C] = { 'c', 'C' }, [KEY_V] = { 'v', 'V' }, [KEY_B] = { 'b', 'B' }, [KEY_N] = { 'n', 'N' },
    [KEY_M] = { 'm', 'M' }, [KEY_COMMA] = { ',', '<' }, [KEY_DOT] = { '.', '>' },
    [KEY_SLASH] = { '/', '?' }, [KEY_SPACE] = { ' ', ' ' },
};

/* The keypad: with Num Lock, and without. */
static const uint32_t k_pad[][2] = {
    [KEY_KP7] = { '7', LV_KEY_HOME }, [KEY_KP8] = { '8', LV_KEY_UP }, [KEY_KP9] = { '9', UI_KEY_PGUP },
    [KEY_KP4] = { '4', LV_KEY_LEFT }, [KEY_KP5] = { '5', 0 }, [KEY_KP6] = { '6', LV_KEY_RIGHT },
    [KEY_KP1] = { '1', LV_KEY_END }, [KEY_KP2] = { '2', LV_KEY_DOWN }, [KEY_KP3] = { '3', UI_KEY_PGDN },
    [KEY_KP0] = { '0', 0 }, [KEY_KPDOT] = { '.', LV_KEY_DEL },
};

static unsigned mods_now(void)
{
    unsigned held = 0, m = 0;
    for (int i = 0; i < g.n; ++i) held |= g.dev[i].held;
    if (held & (HELD_LSHIFT | HELD_RSHIFT)) m |= UI_MOD_SHIFT;
    if (held & (HELD_LCTRL | HELD_RCTRL)) m |= UI_MOD_CTRL;
    if (held & (HELD_LALT | HELD_RALT)) m |= UI_MOD_ALT;
    return m;
}

/* The key for evdev `code`, 0 for none. */
static uint32_t keymap(unsigned code, unsigned mods)
{
    int shift = (mods & UI_MOD_SHIFT) != 0, plain = (mods & (UI_MOD_CTRL | UI_MOD_ALT)) != 0;
    if (code < sizeof k_chars / sizeof k_chars[0] && k_chars[code][0]) {
        char c = k_chars[code][0];
        if (plain) return (uint32_t)c;                  /* Ctrl+S is 's' with UI_MOD_CTRL */
        if (c >= 'a' && c <= 'z') return (uint32_t)(shift != g.caps ? c - 'a' + 'A' : c);
        return (uint32_t)k_chars[code][shift];
    }
    if (code < sizeof k_pad / sizeof k_pad[0] && (k_pad[code][0] || k_pad[code][1]))
        return k_pad[code][g.numlock && !shift ? 0 : 1];
    if (code >= KEY_F1 && code <= KEY_F10) return UI_KEY_F(code - KEY_F1 + 1);
    switch (code) {
    case KEY_ENTER: case KEY_KPENTER: return LV_KEY_ENTER;
    case KEY_ESC:          return LV_KEY_ESC;
    case KEY_BACKSPACE:    return LV_KEY_BACKSPACE;
    case KEY_DELETE:       return LV_KEY_DEL;
    case KEY_TAB:          return shift ? LV_KEY_PREV : LV_KEY_NEXT;
    case KEY_UP:           return LV_KEY_UP;
    case KEY_DOWN:         return LV_KEY_DOWN;
    case KEY_LEFT:         return LV_KEY_LEFT;
    case KEY_RIGHT:        return LV_KEY_RIGHT;
    case KEY_HOME:         return LV_KEY_HOME;
    case KEY_END:          return LV_KEY_END;
    case KEY_PAGEUP:       return UI_KEY_PGUP;
    case KEY_PAGEDOWN:     return UI_KEY_PGDN;
    case KEY_COMPOSE:      return UI_KEY_MENU;
    case KEY_F11:          return UI_KEY_F(11);
    case KEY_F12:          return UI_KEY_F(12);
    case KEY_KPPLUS:       return '+';
    case KEY_KPMINUS:      return '-';
    case KEY_KPASTERISK:   return '*';
    case KEY_KPSLASH:      return '/';
    case KEY_KPEQUAL:      return '=';
    }
    return 0;
}

static unsigned held_bit(unsigned code)
{
    switch (code) {
    case KEY_LEFTSHIFT:  return HELD_LSHIFT;
    case KEY_RIGHTSHIFT: return HELD_RSHIFT;
    case KEY_LEFTCTRL:   return HELD_LCTRL;
    case KEY_RIGHTCTRL:  return HELD_RCTRL;
    case KEY_LEFTALT:    return HELD_LALT;
    case KEY_RIGHTALT:   return HELD_RALT;
    }
    return 0;
}

/* Caps Lock and Num Lock on every keyboard's lights (best effort: a
 * keyboard opened read-only, or a test pipe, has none). */
static void leds_write(int fd)
{
    struct input_event ev[3] = {
        { .type = EV_LED, .code = LED_CAPSL, .value = g.caps },
        { .type = EV_LED, .code = LED_NUML, .value = g.numlock },
        { .type = EV_SYN, .code = SYN_REPORT },
    };
    if (write(fd, ev, sizeof ev) < 0) { /* no lights */ }
}

static void leds_all(void)
{
    for (int i = 0; i < g.n; ++i)
        if (g.dev[i].kbd) leds_write(g.dev[i].fd);
}

/* Never full: dev_drain reads a keyboard only while the queue has room
 * for every event it reads (the rest waits in the kernel's buffer). */
static void key_push(uint32_t key, unsigned mods)
{
    if (g.k_len == KEYS_MAX) return;
    g.keys[(g.k_head + g.k_len++) % KEYS_MAX] = (key_t_){ key, mods };
}

static void key_event(dev_t_ *d, unsigned code, int value)
{
    unsigned bit = held_bit(code);
    if (bit) {
        if (value == 1) d->held |= bit;
        else if (value == 0) d->held &= ~bit;
        return;
    }
    if (value == 0) return;                     /* releases: each press is a press and a release */
    if (code == KEY_CAPSLOCK || code == KEY_NUMLOCK) {
        if (value != 1) return;
        if (code == KEY_CAPSLOCK) g.caps = !g.caps;
        else g.numlock = !g.numlock;
        leds_all();
        return;
    }
    uint32_t key = keymap(code, mods_now());
    if (key) key_push(key, mods_now());
}

/* ---- pointer ------------------------------------------------------------------------------- */

static void queue_press(void)
{
    if (g.q_len == QUEUE_MAX) {                 /* keep the newest */
        g.q_head = (g.q_head + 1) % QUEUE_MAX;
        --g.q_len;
    }
    g.queue[(g.q_head + g.q_len++) % QUEUE_MAX] = (press_t){ g.x, g.y, g.left };
}

/* The pointer is pressed while any device holds its button: one mouse
 * must not release (or swallow) another's press. */
static void buttons_changed(void)
{
    int any = 0;
    for (int i = 0; i < g.n; ++i) any |= g.dev[i].left;
    if (any != g.left) {
        g.left = any;
        queue_press();
    }
}

static void counts_changed(int kbd_before)
{
    g.n_ptr = g.n_kbd = 0;
    for (int i = 0; i < g.n; ++i) {
        g.n_ptr += g.dev[i].ptr;
        g.n_kbd += g.dev[i].kbd;
    }
    cursor_visible();
    if (g.n_kbd != kbd_before && g.hooks.keyboards) g.hooks.keyboards(g.n_kbd);
}

static void dev_remove(int i)
{
    int kbd = g.n_kbd;
    vg_log("input: %s gone", g.dev[i].path);
    close(g.dev[i].fd);
    g.dev[i] = g.dev[--g.n];
    buttons_changed();                          /* unplugged while pressed: released */
    counts_changed(kbd);                        /* and its modifiers are gone with it */
    /* With no keyboard left, nothing typed is still to come: a key typed
     * just before the unplug must not make the UI a keyboard one again. */
    if (!g.n_kbd) g.k_len = 0;
}

static int scale(int v, int lo, int hi, int size)
{
    if (hi <= lo) return clamp(v, 0, size - 1);
    return clamp((int)((long long)(v - lo) * (size - 1) / (hi - lo)), 0, size - 1);
}

/* Everything the device has queued. Returns -1 when it is gone. */
static int dev_drain(dev_t_ *d)
{
    struct input_event ev[32];
    for (;;) {
        /* A keyboard: no more events than keys the queue can take. */
        size_t room = d->kbd ? (size_t)(KEYS_MAX - g.k_len) : 32;
        if (room > 32) room = 32;
        if (!room) return 0;
        ssize_t n = read(d->fd, ev, room * sizeof ev[0]);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n <= 0) return -1;                  /* ENODEV: unplugged; 0: end of a test pipe */
        for (size_t i = 0; i < (size_t)n / sizeof ev[0]; ++i) {
            const struct input_event *e = &ev[i];
            if (e->type == EV_REL && d->ptr) {
                if (e->code == REL_X) g.x = clamp(g.x + e->value, 0, g.w - 1);
                else if (e->code == REL_Y) g.y = clamp(g.y + e->value, 0, g.h - 1);
            } else if (e->type == EV_ABS && d->ptr) {
                if (e->code == ABS_X) g.x = scale(e->value, d->min_x, d->max_x, g.w);
                else if (e->code == ABS_Y) g.y = scale(e->value, d->min_y, d->max_y, g.h);
            } else if (e->type == EV_KEY) {
                if (d->ptr && (e->code == BTN_LEFT || e->code == BTN_TOUCH)) {
                    d->left = e->value != 0;
                    buttons_changed();
                } else if (d->ptr && e->code == BTN_RIGHT && e->value == 1) {
                    ++g.right;
                } else if (d->kbd && e->code < BTN_MISC) {
                    key_event(d, e->code, e->value);
                }
            }
        }
    }
}

static void poll_devices(void)
{
    for (int i = 0; i < g.n; ++i)
        if (dev_drain(&g.dev[i]) < 0) dev_remove(i--);
}

static void right_cb(void *arg)
{
    (void)arg;
    if (g.hooks.right) g.hooks.right();
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    poll_devices();
    /* Not from inside LVGL's input processing: back may change screens. */
    for (; g.right; --g.right) lv_async_call(right_cb, NULL);
    if (g.q_len) {
        press_t p = g.queue[g.q_head];
        g.q_head = (g.q_head + 1) % QUEUE_MAX;
        --g.q_len;
        data->point.x = p.x;
        data->point.y = p.y;
        data->state = p.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = g.q_len > 0;
        return;
    }
    data->point.x = g.x;
    data->point.y = g.y;
    data->state = g.left ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

static void key_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    poll_devices();
    data->key = g.k_last;
    data->state = LV_INDEV_STATE_RELEASED;
    if (g.k_down) {                             /* the release of the last press */
        g.k_down = 0;
        data->continue_reading = g.k_len > 0;
        return;
    }
    if (!g.k_len) return;
    key_t_ k = g.keys[g.k_head];
    g.k_head = (g.k_head + 1) % KEYS_MAX;
    --g.k_len;
    uint32_t key = g.hooks.key ? g.hooks.key(k.key, k.mods) : k.key;
    if (!key) return;                           /* taken by the UI: the rest after what it started */
    g.k_last = key;
    g.k_down = 1;
    data->key = key;
    data->state = LV_INDEV_STATE_PRESSED;
    data->continue_reading = 1;
}

/* ---- devices -------------------------------------------------------------------------------- */

static int dev_add(int fd, const char *name, int ptr, int abs, int min_x, int max_x, int min_y, int max_y,
                   int kbd)
{
    if (g.n == DEVS_MAX) return -1;
    int kbd_before = g.n_kbd;
    dev_t_ *d = &g.dev[g.n++];
    memset(d, 0, sizeof *d);
    d->fd = fd;
    snprintf(d->path, sizeof d->path, "%s", name);
    d->ptr = ptr;
    d->kbd = kbd;
    d->abs = abs;
    d->min_x = min_x;
    d->max_x = max_x;
    d->min_y = min_y;
    d->max_y = max_y;
    if (kbd) leds_write(fd);
    counts_changed(kbd_before);
    return 0;
}

int input_evdev_attach(int fd, const char *name, int abs, int min_x, int max_x, int min_y, int max_y)
{
    return dev_add(fd, name, 1, abs, min_x, max_x, min_y, max_y, 0);
}

int input_evdev_attach_keyboard(int fd, const char *name)
{
    return dev_add(fd, name, 0, 0, 0, 0, 0, 0, 1);
}

static int is_open(const char *path, ino_t ino)
{
    for (int i = 0; i < g.n; ++i)
        if (!strcmp(g.dev[i].path, path) && g.dev[i].ino == ino) return 1;
    for (int i = 0; i < g.n_ignored; ++i)
        if (!strcmp(g.ignored[i].path, path) && g.ignored[i].ino == ino) return 1;
    return 0;
}

static void ignore(const char *path, ino_t ino)
{
    if (g.n_ignored == IGNORE_MAX) g.n_ignored = 0;     /* forget, look again */
    snprintf(g.ignored[g.n_ignored].path, sizeof g.ignored[0].path, "%s", path);
    g.ignored[g.n_ignored++].ino = ino;
}

/* Open `path` if it is a pointer device or a keyboard. Read-write when
 * allowed, for a keyboard's Caps Lock and Num Lock lights. */
static void probe(const char *path, ino_t ino)
{
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;                         /* not ignored: it may open on a later scan */
    unsigned long ev[NBITS(EV_MAX + 1)] = { 0 }, rel[NBITS(REL_MAX + 1)] = { 0 },
                  abs[NBITS(ABS_MAX + 1)] = { 0 }, key[NBITS(KEY_MAX + 1)] = { 0 };
    ioctl(fd, EVIOCGBIT(0, sizeof ev), ev);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof rel), rel);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
    int is_rel = TEST_BIT(EV_REL, ev) && TEST_BIT(REL_X, rel) && TEST_BIT(REL_Y, rel) &&
                 TEST_BIT(EV_KEY, ev) && TEST_BIT(BTN_LEFT, key);
    int is_abs = TEST_BIT(EV_ABS, ev) && TEST_BIT(ABS_X, abs) && TEST_BIT(ABS_Y, abs) &&
                 TEST_BIT(EV_KEY, ev) && (TEST_BIT(BTN_TOUCH, key) || TEST_BIT(BTN_LEFT, key));
    /* Letters and Enter: a keyboard, not a power button or a remote with
     * a few keys. */
    int is_kbd = TEST_BIT(EV_KEY, ev) && TEST_BIT(KEY_A, key) && TEST_BIT(KEY_Z, key) &&
                 TEST_BIT(KEY_ENTER, key);
    if (!is_rel && !is_abs && !is_kbd) {
        close(fd);
        ignore(path, ino);
        return;
    }
    struct input_absinfo ax = { 0 }, ay = { 0 };
    if (is_abs && !is_rel) {
        ioctl(fd, EVIOCGABS(ABS_X), &ax);
        ioctl(fd, EVIOCGABS(ABS_Y), &ay);
    }
    char name[64] = "?";
    ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
    int ptr = is_rel || is_abs;
    if (dev_add(fd, path, ptr, ptr && !is_rel, ax.minimum, ax.maximum, ay.minimum, ay.maximum, is_kbd) < 0) {
        close(fd);
        return;
    }
    g.dev[g.n - 1].ino = ino;
    vg_log("input: %s (%s): %s%s%s", path, name,
           is_rel ? "mouse" : is_abs ? "touch / absolute pointer" : "",
           ptr && is_kbd ? ", " : "", is_kbd ? "keyboard" : "");
}

static void scan_cb(lv_timer_t *t)
{
    (void)t;
    DIR *d = opendir(INPUT_DIR);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char path[64];
        struct stat st;
        snprintf(path, sizeof path, INPUT_DIR "/%.40s", e->d_name);
        if (stat(path, &st) < 0 || !S_ISCHR(st.st_mode) || is_open(path, st.st_ino)) continue;
        probe(path, st.st_ino);
    }
    closedir(d);
}

lv_indev_t *input_evdev_create(lv_display_t *disp, const input_hooks_t *hooks, int scan)
{
    memset(&g, 0, sizeof g);
    if (hooks) g.hooks = *hooks;
    g.w = lv_display_get_horizontal_resolution(disp);
    g.h = lv_display_get_vertical_resolution(disp);
    g.x = g.w / 2;
    g.y = g.h / 2;
    g.numlock = 1;
    g.indev = lv_indev_create();
    lv_indev_set_type(g.indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(g.indev, disp);
    lv_indev_set_read_cb(g.indev, read_cb);
    g.keypad = lv_indev_create();
    lv_indev_set_type(g.keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(g.keypad, disp);
    lv_indev_set_read_cb(g.keypad, key_read_cb);
    g.cursor = ui_cursor_create();
    lv_indev_set_cursor(g.indev, g.cursor);
    cursor_visible();
    if (scan) {
        scan_cb(NULL);
        g.scan = lv_timer_create(scan_cb, RESCAN_MS, NULL);
    }
    return g.indev;
}

lv_indev_t *input_evdev_keypad(void) { return g.keypad; }

void input_evdev_delete(void)
{
    if (g.scan) lv_timer_delete(g.scan);
    if (g.indev) lv_indev_delete(g.indev);
    if (g.keypad) lv_indev_delete(g.keypad);
    for (int i = 0; i < g.n; ++i) close(g.dev[i].fd);
    memset(&g, 0, sizeof g);
}
