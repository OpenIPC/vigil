/* SPDX-License-Identifier: GPL-3.0-or-later */
/* input_evdev.c — mice and touch screens for the UI, from Linux evdev.
 *
 * Every /dev/input/event* that is a pointer (relative X/Y with a left
 * button, or absolute X/Y) feeds one LVGL pointer: plugging in a second
 * mouse just works, and a device plugged in later is found by a rescan
 * every 2 s (no udev on these boards). The left button (or a touch)
 * clicks; the right button calls `on_right`, which LVGL's own evdev driver
 * cannot do: it reports only BTN_MOUSE/BTN_TOUCH. The pointer is drawn by
 * LVGL (ui_cursor.c), shown while a device is there.
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

#define DEVS_MAX   8
#define IGNORE_MAX 32
#define QUEUE_MAX  16
#define RESCAN_MS  2000
#define INPUT_DIR  "/dev/input"

#define BITS_LONG       (8 * sizeof(unsigned long))
#define NBITS(n)        (((n) + BITS_LONG - 1) / BITS_LONG)
#define TEST_BIT(b, a)  (((a)[(b) / BITS_LONG] >> ((b) % BITS_LONG)) & 1)

typedef struct {
    int   fd;
    char  path[64];
    ino_t ino;
    int   abs;                      /* absolute X/Y (touch screen, tablet) */
    int   min_x, max_x, min_y, max_y;
    int   left;                     /* this device's button (or touch) is down */
} dev_t_;

typedef struct {
    int x, y, pressed;
} press_t;

static struct {
    dev_t_       dev[DEVS_MAX];
    int          n;
    struct { char path[64]; ino_t ino; } ignored[IGNORE_MAX];
    int          n_ignored;
    int          w, h;
    int          x, y, left;        /* left: any device's button is down */
    press_t      queue[QUEUE_MAX];  /* button changes not yet given to LVGL */
    int          q_head, q_len;
    int          right;             /* right presses not yet handled */
    lv_indev_t  *indev;
    lv_obj_t    *cursor;
    lv_timer_t  *scan;
    void        (*on_right)(void);
} g;

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void cursor_visible(void)
{
    if (!g.cursor) return;
    if (g.n) lv_obj_remove_flag(g.cursor, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(g.cursor, LV_OBJ_FLAG_HIDDEN);
}

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

static void dev_remove(int i)
{
    vg_log("input: %s gone", g.dev[i].path);
    close(g.dev[i].fd);
    g.dev[i] = g.dev[--g.n];
    buttons_changed();                          /* unplugged while pressed: released */
    cursor_visible();
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
        ssize_t n = read(d->fd, ev, sizeof ev);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n <= 0) return -1;                  /* ENODEV: unplugged; 0: end of a test pipe */
        for (size_t i = 0; i < (size_t)n / sizeof ev[0]; ++i) {
            const struct input_event *e = &ev[i];
            if (e->type == EV_REL) {
                if (e->code == REL_X) g.x = clamp(g.x + e->value, 0, g.w - 1);
                else if (e->code == REL_Y) g.y = clamp(g.y + e->value, 0, g.h - 1);
            } else if (e->type == EV_ABS) {
                if (e->code == ABS_X) g.x = scale(e->value, d->min_x, d->max_x, g.w);
                else if (e->code == ABS_Y) g.y = scale(e->value, d->min_y, d->max_y, g.h);
            } else if (e->type == EV_KEY) {
                if (e->code == BTN_LEFT || e->code == BTN_TOUCH) {
                    d->left = e->value != 0;
                    buttons_changed();
                } else if (e->code == BTN_RIGHT && e->value == 1) {
                    ++g.right;
                }
            }
        }
    }
}

static void right_cb(void *arg)
{
    (void)arg;
    if (g.on_right) g.on_right();
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    for (int i = 0; i < g.n; ++i)
        if (dev_drain(&g.dev[i]) < 0) dev_remove(i--);
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

int input_evdev_attach(int fd, const char *name, int abs, int min_x, int max_x, int min_y, int max_y)
{
    if (g.n == DEVS_MAX) return -1;
    dev_t_ *d = &g.dev[g.n++];
    memset(d, 0, sizeof *d);
    d->fd = fd;
    snprintf(d->path, sizeof d->path, "%s", name);
    d->abs = abs;
    d->min_x = min_x;
    d->max_x = max_x;
    d->min_y = min_y;
    d->max_y = max_y;
    cursor_visible();
    return 0;
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

/* Open `path` if it is a pointer device. */
static void probe(const char *path, ino_t ino)
{
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
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
    if (!is_rel && !is_abs) {
        close(fd);
        ignore(path, ino);
        return;
    }
    struct input_absinfo ax = { 0 }, ay = { 0 };
    if (!is_rel) {
        ioctl(fd, EVIOCGABS(ABS_X), &ax);
        ioctl(fd, EVIOCGABS(ABS_Y), &ay);
    }
    char name[64] = "?";
    ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
    if (input_evdev_attach(fd, path, !is_rel, ax.minimum, ax.maximum, ay.minimum, ay.maximum) < 0) {
        close(fd);
        return;
    }
    g.dev[g.n - 1].ino = ino;
    vg_log("input: %s (%s): %s", path, name, is_rel ? "mouse" : "touch / absolute pointer");
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

lv_indev_t *input_evdev_create(lv_display_t *disp, void (*on_right)(void), int scan)
{
    memset(&g, 0, sizeof g);
    g.w = lv_display_get_horizontal_resolution(disp);
    g.h = lv_display_get_vertical_resolution(disp);
    g.x = g.w / 2;
    g.y = g.h / 2;
    g.on_right = on_right;
    g.indev = lv_indev_create();
    lv_indev_set_type(g.indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(g.indev, disp);
    lv_indev_set_read_cb(g.indev, read_cb);
    g.cursor = ui_cursor_create();
    lv_indev_set_cursor(g.indev, g.cursor);
    cursor_visible();
    if (scan) {
        scan_cb(NULL);
        g.scan = lv_timer_create(scan_cb, RESCAN_MS, NULL);
    }
    return g.indev;
}

void input_evdev_delete(void)
{
    if (g.scan) lv_timer_delete(g.scan);
    if (g.indev) lv_indev_delete(g.indev);
    for (int i = 0; i < g.n; ++i) close(g.dev[i].fd);
    memset(&g, 0, sizeof g);
}
