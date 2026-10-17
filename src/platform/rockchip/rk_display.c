/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rk_display.c — DRM/KMS output on Rockchip VOP2 (see rk_display.h).
 *
 * VOP2 (RK3588) gives each video port only a few windows, and only the
 * Esmart ones scan out NV12 — two per port on the Orange Pi 5 Plus — so
 * the cameras cannot each have a plane. The video is composed (RGA) into
 * one NV12 mosaic instead, shown on an Esmart overlay plane; the UI goes
 * on the port's primary (Cluster) plane above it, ARGB8888 with
 * per-pixel alpha so tiles are see-through.
 *
 * Everything is set with atomic commits. A capture of the composed output
 * goes through the writeback connector (DRM_MODE_CONNECTOR_WRITEBACK):
 * that is how a board with no monitor attached can still be checked.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "rk_display.h"

#define CARD "/dev/dri/card0"

static struct {
    int       fd;
    uint32_t  crtc_id, crtc_idx, conn_id, wb_id;
    uint32_t  video_plane, ui_plane;
    uint32_t  mode_blob;
    unsigned  w, h;
    int       connected;
    rk_buf_t  mosaic[2];
    uint32_t  mosaic_fb[2], mosaic_handle[2];
    int       front;
    rk_buf_t  ui;
    uint32_t  ui_fb, ui_handle;
    pthread_mutex_t shot_lock;
    char      shot_path[256];
} g = { .fd = -1, .shot_lock = PTHREAD_MUTEX_INITIALIZER };

/* ---- property helpers ---------------------------------------------------- */

static uint32_t prop_id(uint32_t obj, uint32_t type, const char *name, uint64_t *value, int *immutable)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(g.fd, obj, type);
    uint32_t id = 0;
    for (uint32_t i = 0; props && i < props->count_props && !id; ++i) {
        drmModePropertyRes *p = drmModeGetProperty(g.fd, props->props[i]);
        if (p && strcmp(p->name, name) == 0) {
            id = p->prop_id;
            if (value) *value = props->prop_values[i];
            if (immutable) *immutable = !!(p->flags & DRM_MODE_PROP_IMMUTABLE);
        }
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

/* Enum property: the value of the entry called `entry`, or -1. */
static int64_t prop_enum(uint32_t obj, uint32_t type, const char *name, const char *entry)
{
    drmModeObjectProperties *props = drmModeObjectGetProperties(g.fd, obj, type);
    int64_t v = -1;
    for (uint32_t i = 0; props && i < props->count_props && v < 0; ++i) {
        drmModePropertyRes *p = drmModeGetProperty(g.fd, props->props[i]);
        if (p && strcmp(p->name, name) == 0)
            for (int e = 0; e < p->count_enums; ++e)
                if (strcmp(p->enums[e].name, entry) == 0) v = (int64_t)p->enums[e].value;
        drmModeFreeProperty(p);
    }
    drmModeFreeObjectProperties(props);
    return v;
}

static int add(drmModeAtomicReq *req, uint32_t obj, uint32_t type, const char *name, uint64_t value)
{
    uint32_t id = prop_id(obj, type, name, NULL, NULL);
    if (!id) return -1;
    return drmModeAtomicAddProperty(req, obj, id, value) < 0 ? -1 : 0;
}

/* ---- buffers --------------------------------------------------------------- */

static int dumb_create(uint32_t w, uint32_t h, uint32_t bpp, uint32_t *handle, rk_buf_t *b)
{
    struct drm_mode_create_dumb c = { .width = w, .height = h, .bpp = bpp };
    if (drmIoctl(g.fd, DRM_IOCTL_MODE_CREATE_DUMB, &c) < 0) return -1;
    struct drm_mode_map_dumb m = { .handle = c.handle };
    if (drmIoctl(g.fd, DRM_IOCTL_MODE_MAP_DUMB, &m) < 0) return -1;
    void *p = mmap(NULL, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, g.fd, (off_t)m.offset);
    if (p == MAP_FAILED) return -1;
    int fd = -1;
    if (drmPrimeHandleToFD(g.fd, c.handle, DRM_CLOEXEC | DRM_RDWR, &fd) < 0) fd = -1;
    *handle = c.handle;
    b->map = p;
    b->fd = fd;
    b->pitch = c.pitch;
    b->width = w;
    return 0;
}

/* NV12 mosaic: one dumb buffer holding Y (h lines) then UV (h/2 lines). */
static int mosaic_create(int i)
{
    rk_buf_t *b = &g.mosaic[i];
    if (dumb_create(g.w, g.h * 3 / 2, 8, &g.mosaic_handle[i], b) < 0) return -1;
    b->height = g.h;
    memset(b->map, 16, (size_t)b->pitch * g.h);                       /* black Y */
    memset(b->map + (size_t)b->pitch * g.h, 128, (size_t)b->pitch * g.h / 2); /* neutral UV */
    uint32_t handles[4] = { g.mosaic_handle[i], g.mosaic_handle[i] };
    uint32_t pitches[4] = { b->pitch, b->pitch };
    uint32_t offsets[4] = { 0, b->pitch * g.h };
    return drmModeAddFB2(g.fd, g.w, g.h, DRM_FORMAT_NV12, handles, pitches, offsets, &g.mosaic_fb[i], 0);
}

static int ui_create(void)
{
    if (dumb_create(g.w, g.h, 32, &g.ui_handle, &g.ui) < 0) return -1;
    g.ui.height = g.h;
    memset(g.ui.map, 0, (size_t)g.ui.pitch * g.h);                    /* fully transparent */
    uint32_t handles[4] = { g.ui_handle }, pitches[4] = { g.ui.pitch }, offsets[4] = { 0 };
    return drmModeAddFB2(g.fd, g.w, g.h, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &g.ui_fb, 0);
}

/* ---- setup ------------------------------------------------------------------ */

/* CEA-861 1280x720p60, for a CRTC with no monitor (writeback only). */
static void mode_720p60(drmModeModeInfo *m)
{
    memset(m, 0, sizeof *m);
    m->clock = 74250;
    m->hdisplay = 1280; m->hsync_start = 1390; m->hsync_end = 1430; m->htotal = 1650;
    m->vdisplay = 720;  m->vsync_start = 725;  m->vsync_end = 730;  m->vtotal = 750;
    m->vrefresh = 60;
    m->flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC;
    m->type = DRM_MODE_TYPE_DRIVER;
    snprintf(m->name, sizeof m->name, "1280x720");
}

static int pick_planes(drmModeRes *res)
{
    drmModePlaneRes *pr = drmModeGetPlaneResources(g.fd);
    if (!pr) return -1;
    for (uint32_t i = 0; i < pr->count_planes; ++i) {
        drmModePlane *p = drmModeGetPlane(g.fd, pr->planes[i]);
        if (!p || !(p->possible_crtcs & (1u << g.crtc_idx))) { drmModeFreePlane(p); continue; }
        uint64_t type = 0;
        prop_id(p->plane_id, DRM_MODE_OBJECT_PLANE, "type", &type, NULL);
        int nv12 = 0, argb = 0;
        for (uint32_t f = 0; f < p->count_formats; ++f) {
            nv12 |= p->formats[f] == DRM_FORMAT_NV12;
            argb |= p->formats[f] == DRM_FORMAT_ARGB8888;
        }
        if (type == DRM_PLANE_TYPE_PRIMARY && argb && !g.ui_plane) g.ui_plane = p->plane_id;
        else if (type == DRM_PLANE_TYPE_OVERLAY && nv12 && !g.video_plane) g.video_plane = p->plane_id;
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
    (void)res;
    return g.ui_plane && g.video_plane ? 0 : -1;
}

int rk_display_connected(void) { return g.connected; }

int rk_display_open(unsigned w, unsigned h)
{
    g.fd = open(CARD, O_RDWR | O_CLOEXEC);
    if (g.fd < 0) { perror(CARD); return -1; }
    if (drmSetClientCap(g.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0 ||
        drmSetClientCap(g.fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0) {
        fprintf(stderr, "rockchip: no atomic modesetting\n");
        return -1;
    }
    drmSetClientCap(g.fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);
    g.w = w;
    g.h = h;

    drmModeRes *res = drmModeGetResources(g.fd);
    if (!res) return -1;
    drmModeModeInfo mode;
    int have_mode = 0;
    for (int i = 0; i < res->count_connectors; ++i) {
        drmModeConnector *c = drmModeGetConnector(g.fd, res->connectors[i]);
        if (!c) continue;
        if (c->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
            g.wb_id = c->connector_id;
        } else if (!g.conn_id && c->connection == DRM_MODE_CONNECTED && c->count_modes > 0) {
            g.conn_id = c->connector_id;
            mode = c->modes[0];                                     /* preferred */
            for (int m = 0; m < c->count_modes; ++m)
                if (c->modes[m].hdisplay == w && c->modes[m].vdisplay == h && c->modes[m].vrefresh == 60) {
                    mode = c->modes[m];
                    break;
                }
            have_mode = 1;
        }
        drmModeFreeConnector(c);
    }
    g.connected = g.conn_id != 0;
    if (!have_mode) mode_720p60(&mode);
    if (!g.conn_id && !g.wb_id) {
        fprintf(stderr, "rockchip: no connected output and no writeback connector\n");
        drmModeFreeResources(res);
        return -1;
    }
    /* The first CRTC (video port 0) carries the primary planes we want. */
    g.crtc_idx = 0;
    g.crtc_id = res->crtcs[0];
    if (pick_planes(res) < 0) {
        fprintf(stderr, "rockchip: no NV12 overlay + ARGB primary plane on CRTC %u\n", g.crtc_id);
        drmModeFreeResources(res);
        return -1;
    }
    drmModeFreeResources(res);
    if (mode.hdisplay != w || mode.vdisplay != h)
        fprintf(stderr, "rockchip: monitor mode is %ux%u; the UI is %ux%u and is scaled\n",
                mode.hdisplay, mode.vdisplay, w, h);

    if (mosaic_create(0) < 0 || mosaic_create(1) < 0 || ui_create() < 0) {
        fprintf(stderr, "rockchip: cannot allocate scan-out buffers\n");
        return -1;
    }
    if (drmModeCreatePropertyBlob(g.fd, &mode, sizeof mode, &g.mode_blob) < 0) return -1;

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    add(req, g.crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", g.mode_blob);
    add(req, g.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1);
    add(req, g.conn_id ? g.conn_id : g.wb_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", g.crtc_id);
    uint32_t planes[2] = { g.video_plane, g.ui_plane }, fbs[2] = { g.mosaic_fb[0], g.ui_fb };
    for (int i = 0; i < 2; ++i) {
        uint32_t p = planes[i];
        add(req, p, DRM_MODE_OBJECT_PLANE, "FB_ID", fbs[i]);
        add(req, p, DRM_MODE_OBJECT_PLANE, "CRTC_ID", g.crtc_id);
        add(req, p, DRM_MODE_OBJECT_PLANE, "SRC_X", 0);
        add(req, p, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0);
        add(req, p, DRM_MODE_OBJECT_PLANE, "SRC_W", (uint64_t)w << 16);
        add(req, p, DRM_MODE_OBJECT_PLANE, "SRC_H", (uint64_t)h << 16);
        add(req, p, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0);
        add(req, p, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
        add(req, p, DRM_MODE_OBJECT_PLANE, "CRTC_W", mode.hdisplay);
        add(req, p, DRM_MODE_OBJECT_PLANE, "CRTC_H", mode.vdisplay);
        add(req, p, DRM_MODE_OBJECT_PLANE, "zpos", (uint64_t)i);    /* video below UI */
    }
    /* LVGL writes straight (not premultiplied) alpha. */
    int64_t cov = prop_enum(g.ui_plane, DRM_MODE_OBJECT_PLANE, "pixel blend mode", "Coverage");
    if (cov >= 0) add(req, g.ui_plane, DRM_MODE_OBJECT_PLANE, "pixel blend mode", (uint64_t)cov);
    int rc = drmModeAtomicCommit(g.fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(req);
    if (rc < 0) {
        fprintf(stderr, "rockchip: modeset failed: %s\n", strerror(errno));
        return -1;
    }
    g.front = 0;
    fprintf(stderr, "rockchip: %s %ux%u@%u, video plane %u, UI plane %u%s\n",
            g.connected ? "output" : "headless (writeback only)", mode.hdisplay, mode.vdisplay,
            mode.vrefresh, g.video_plane, g.ui_plane, g.wb_id ? ", writeback available" : "");
    return 0;
}

rk_buf_t *rk_display_back(void) { return &g.mosaic[!g.front]; }
rk_buf_t *rk_display_ui(void) { return &g.ui; }

void rk_display_request_shot(const char *path)
{
    pthread_mutex_lock(&g.shot_lock);
    snprintf(g.shot_path, sizeof g.shot_path, "%s", path);
    pthread_mutex_unlock(&g.shot_lock);
}

/* One writeback job: ARGB8888 buffer, attached for this commit only. */
static int shot_commit(drmModeAtomicReq *req, const char *path, int back)
{
    rk_buf_t wb;
    uint32_t handle, fb;
    if (dumb_create(g.w, g.h, 32, &handle, &wb) < 0) return -1;
    uint32_t handles[4] = { handle }, pitches[4] = { wb.pitch }, offsets[4] = { 0 };
    /* VOP2 writeback takes BGR888, ARGB8888, RGB565, NV12 — not XRGB8888. */
    if (drmModeAddFB2(g.fd, g.w, g.h, DRM_FORMAT_ARGB8888, handles, pitches, offsets, &fb, 0) < 0) return -1;
    int32_t fence = -1;
    if (!g.connected) {
        /* the writeback connector is already the CRTC's connector */
    } else {
        add(req, g.wb_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", g.crtc_id);
    }
    add(req, g.wb_id, DRM_MODE_OBJECT_CONNECTOR, "WRITEBACK_FB_ID", fb);
    add(req, g.wb_id, DRM_MODE_OBJECT_CONNECTOR, "WRITEBACK_OUT_FENCE_PTR", (uint64_t)(uintptr_t)&fence);
    add(req, g.video_plane, DRM_MODE_OBJECT_PLANE, "FB_ID", g.mosaic_fb[back]);
    int rc = drmModeAtomicCommit(g.fd, req, g.connected ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0, NULL);
    if (rc == 0 && fence >= 0) {
        struct pollfd pf = { .fd = fence, .events = POLLIN };
        poll(&pf, 1, 1000);
        close(fence);
        FILE *f = fopen(path, "wb");
        if (f) {
            for (uint32_t y = 0; y < g.h; ++y) fwrite(wb.map + (size_t)y * wb.pitch, 4, g.w, f);
            fclose(f);
            fprintf(stderr, "rockchip: writeback capture -> %s\n", path);
        }
    } else {
        fprintf(stderr, "rockchip: writeback commit failed: %s\n", rc < 0 ? strerror(errno) : "no fence");
    }
    if (g.connected) {                              /* detach again */
        drmModeAtomicReq *off = drmModeAtomicAlloc();
        add(off, g.wb_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", 0);
        drmModeAtomicCommit(g.fd, off, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
        drmModeAtomicFree(off);
    }
    drmModeRmFB(g.fd, fb);
    munmap(wb.map, (size_t)wb.pitch * g.h);
    if (wb.fd >= 0) close(wb.fd);
    struct drm_mode_destroy_dumb d = { .handle = handle };
    drmIoctl(g.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &d);
    return rc;
}

int rk_display_flip(void)
{
    int back = !g.front;
    char path[256];
    pthread_mutex_lock(&g.shot_lock);
    snprintf(path, sizeof path, "%s", g.shot_path);
    g.shot_path[0] = 0;
    pthread_mutex_unlock(&g.shot_lock);

    drmModeAtomicReq *req = drmModeAtomicAlloc();
    int rc;
    if (path[0] && g.wb_id) {
        rc = shot_commit(req, path, back);
    } else {
        add(req, g.video_plane, DRM_MODE_OBJECT_PLANE, "FB_ID", g.mosaic_fb[back]);
        rc = drmModeAtomicCommit(g.fd, req, 0, NULL);       /* blocks to vblank */
    }
    drmModeAtomicFree(req);
    if (rc == 0) g.front = back;
    return rc;
}

void rk_display_close(void)
{
    if (g.fd < 0) return;
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    add(req, g.video_plane, DRM_MODE_OBJECT_PLANE, "FB_ID", 0);
    add(req, g.video_plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0);
    add(req, g.ui_plane, DRM_MODE_OBJECT_PLANE, "FB_ID", 0);
    add(req, g.ui_plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID", 0);
    add(req, g.conn_id ? g.conn_id : g.wb_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID", 0);
    add(req, g.crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 0);
    add(req, g.crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID", 0);
    drmModeAtomicCommit(g.fd, req, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
    drmModeAtomicFree(req);
    for (int i = 0; i < 2; ++i) if (g.mosaic_fb[i]) drmModeRmFB(g.fd, g.mosaic_fb[i]);
    if (g.ui_fb) drmModeRmFB(g.fd, g.ui_fb);
    if (g.mode_blob) drmModeDestroyPropertyBlob(g.fd, g.mode_blob);
    close(g.fd);
    g.fd = -1;
}
