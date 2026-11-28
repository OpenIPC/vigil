/* SPDX-License-Identifier: GPL-3.0-or-later */
/* ptz.h — moving the cameras: pan, tilt, zoom, focus, iris, presets,
 * tours and the home position, over the camera's own protocol (a backend:
 * NETIP's OPPTZControl, ONVIF's PTZ and Imaging services).
 *
 * Each channel has a backend, chosen like its events are: `ptz: auto`
 * takes NETIP for a netip:// camera and ONVIF for the others when the
 * camera has a PTZ service; netip / onvif force one; off, none. The
 * capabilities (what the panel offers) come from the backend's probe,
 * which runs the first time anyone asks (ptz_info).
 *
 * Every call here returns at once: the commands go out from a worker
 * thread of the channel's own (started on first use, connection closed
 * when idle), never from the caller's (the UI's main thread, an API
 * thread). A continuous move (ptz_move, ptz_focus, ptz_iris) lasts
 * `hold_ms` from the call and is stopped then unless called again: the
 * UI repeats it while a button, a key or the mouse is held and stops it
 * on release, the API's clients repeat it or send a stop. A stop the
 * camera did not take is sent again, and after PTZ_STOP_TRIES the
 * connection is dropped (majestic ends a move when its client hangs up);
 * so a lost stop or a client that went away never leaves a camera
 * spinning.
 *
 * Presets are tokens: NETIP's are numbers ("1".."255"), ONVIF's the
 * camera's own strings. Their names are kept by vigild (PTZ names file,
 * ptz_start) for NETIP, whose cameras keep none, and for a rename, which
 * ONVIF can only do by storing the position again; ONVIF's own names
 * show otherwise.
 */
#ifndef PTZ_H
#define PTZ_H

#include <stddef.h>

#include "channel.h"

#define PTZ_CH_MAX       CHANNEL_MAX
#define PTZ_PRESETS_MAX  32
#define PTZ_TOKEN_LEN    32
#define PTZ_NAME_LEN     32
#define PTZ_TOURS_MAX    8
#define PTZ_HOLD_MS      1000       /* a move's default life without a repeat */
#define PTZ_HOLD_MAX_MS  10000      /* ... and the longest one asked for */
#define PTZ_STOP_TRIES   3

typedef enum {
    PTZ_OFF = 0,                    /* no camera, disabled, or ptz: off */
    PTZ_PROBING,                    /* asking the camera what it can do */
    PTZ_READY,
    PTZ_NONE,                       /* the camera has no PTZ */
    PTZ_ERROR,                      /* it could not be reached or refused us: `error` */
} ptz_state_t;

/* "off", "probing", "ready", "none", "error". */
const char *ptz_state_name(ptz_state_t s);

enum {
    PTZ_CAP_PAN_TILT   = 1u << 0,
    PTZ_CAP_ZOOM       = 1u << 1,
    PTZ_CAP_FOCUS      = 1u << 2,
    PTZ_CAP_FOCUS_AUTO = 1u << 3,
    PTZ_CAP_IRIS       = 1u << 4,
    PTZ_CAP_PRESETS    = 1u << 5,
    PTZ_CAP_TOURS      = 1u << 6,
    PTZ_CAP_HOME       = 1u << 7,
};
#define PTZ_N_CAPS 8
/* "pan_tilt", "zoom", "focus", "focus_auto", "iris", "presets", "tours", "home". */
const char *ptz_cap_name(int bit);

typedef enum { PTZ_BACKEND_NONE = 0, PTZ_BACKEND_NETIP, PTZ_BACKEND_ONVIF } ptz_backend_t;
const char *ptz_backend_name(ptz_backend_t b);     /* "", "netip", "onvif" */

typedef struct {
    char token[PTZ_TOKEN_LEN];
    char name[PTZ_NAME_LEN];        /* "" : none */
} ptz_preset_t;

typedef struct {
    ptz_state_t   state;
    ptz_backend_t backend;
    unsigned      caps;             /* PTZ_CAP_* (READY) */
    char          error[96];        /* why ERROR or NONE, or the last command's failure */
    unsigned      error_seq;        /* moves with every new failure */
    int           n_presets;
    ptz_preset_t  presets[PTZ_PRESETS_MAX];
    int           tours;            /* tours that can be started (1..tours), 0 none */
    int           moving;           /* a continuous move is in force */
    unsigned      gen;              /* moves whenever anything above does */
} ptz_info_t;

/* One channel's camera as the configuration has it. */
typedef struct {
    char url[512];                  /* "" no camera */
    char onvif_url[160];            /* the ONVIF device service; "" found from the URL's host */
    char mode[8];                   /* "auto", "netip", "onvif", "off" */
    int  enabled;
} ptz_camera_t;

/* Start (once, before ptz_apply): `names_path` is the file the preset
 * names are kept in (NULL: kept in memory only). 0, or -1. */
int  ptz_start(const char *names_path);
/* Stop every move, end the workers (waiting for them). */
void ptz_shutdown(void);

/* The cameras, channel by channel (n <= PTZ_CH_MAX), from the main
 * thread when the configuration changes: a channel whose camera changed
 * is stopped (a move in force is stopped first) and probed again when
 * next used. Never blocks on the network. */
void ptz_apply(const ptz_camera_t *cams, int n);

/* The channel's state (0-based `ch`); asking starts the probe when it
 * has not run yet (or failed more than a few seconds ago). 0, or -1 for
 * a channel that does not exist. */
int ptz_info(int ch, ptz_info_t *out);

/* Results of the commands (queued; the outcome shows in ptz_info). */
enum { PTZ_OK = 0, PTZ_EINVAL = -1, PTZ_ENOTSUP = -2, PTZ_EBUSY = -3, PTZ_ENOPTZ = -4 };
const char *ptz_rc_text(int rc);

/* A continuous move: pan (+ right), tilt (+ up), zoom (+ in, tele), each
 * -1..1 (the speed; the backend rounds it to what it can), for hold_ms
 * (0: PTZ_HOLD_MS; at most PTZ_HOLD_MAX_MS) from now. All zero: stop. */
int ptz_move(int ch, double pan, double tilt, double zoom, int hold_ms);
/* Focus (+ far, - near) and iris (+ open, - close), -1..1, the same way.
 * A move of one kind ends the others. */
int ptz_focus(int ch, double dir, int hold_ms);
int ptz_iris(int ch, double dir, int hold_ms);
/* Stop whatever moves. */
int ptz_stop(int ch);
int ptz_focus_auto(int ch);
int ptz_home(int ch);
/* `token` NULL or "" for ptz_preset_set: a new preset (NETIP: the lowest
 * free number); `name` may be NULL. */
int ptz_preset_goto(int ch, const char *token);
int ptz_preset_set(int ch, const char *token, const char *name);
int ptz_preset_remove(int ch, const char *token);
int ptz_preset_rename(int ch, const char *token, const char *name);
/* Tour `n` (1..tours) started, or stopped. */
int ptz_tour(int ch, int n, int start);

/* ---- for the backends and the tests ------------------------------------------------- */

/* The axes of a continuous move. */
enum { PTZ_AX_PAN, PTZ_AX_TILT, PTZ_AX_ZOOM, PTZ_AX_FOCUS, PTZ_AX_IRIS, PTZ_AXES };

enum { PTZ_PRESET_GOTO, PTZ_PRESET_SET, PTZ_PRESET_REMOVE };

/* A backend: a link to one camera. Every call blocks (on the channel's
 * worker), with its own time limits, and gives up early once *quit is
 * set. 0, -1 with `err`, PTZ_ENOTSUP. */
typedef struct ptz_drv {
    const char *name;
    /* Parse only (no connection): NULL with `err` when the camera cannot
     * be reached this way. */
    void *(*create)(const ptz_camera_t *cam, const volatile int *quit, char *err, size_t cap);
    void  (*destroy)(void *l);
    /* Connect and find out what the camera can do: caps, tours, and its
     * presets when it keeps a list (*n_presets -1: it keeps none). 0; 1:
     * the camera has no PTZ (`err` says why); -1 with `err`. */
    int   (*probe)(void *l, unsigned *caps, int *tours, ptz_preset_t *presets, int *n_presets, char *err,
                   size_t cap);
    /* From the move in force `from` to `to` (PTZ_AXES values each; all
     * zero: stopped). */
    int   (*motion)(void *l, const double from[PTZ_AXES], const double to[PTZ_AXES], char *err, size_t cap);
    /* PTZ_PRESET_*; SET with token "" makes a new one (`out` gets its
     * token; NETIP takes a token always, the core picks it). */
    int   (*preset)(void *l, int op, const char *token, const char *name, char *out, size_t ocap, char *err,
                    size_t cap);
    /* The camera's presets again (after a change), or -1 / n_presets -1. */
    int   (*presets)(void *l, ptz_preset_t *presets, int *n_presets, char *err, size_t cap);
    int   (*home)(void *l, char *err, size_t cap);
    int   (*tour)(void *l, int n, int start, char *err, size_t cap);
    int   (*focus_auto)(void *l, char *err, size_t cap);
    /* While a move lasts, `motion` is called again with from == to every
     * refresh_ms (0: never), for cameras that end a move on their own. */
    int   refresh_ms;
    /* Nothing to send: keep the link alive or let it go after idle_ms
     * without a command. */
    void  (*idle)(void *l, long long idle_ms);
    /* Drop the connection (a stop that did not get through). */
    void  (*hangup)(void *l);
} ptz_drv_t;

extern const ptz_drv_t ptz_drv_netip;
extern const ptz_drv_t ptz_drv_onvif;

/* Replace the backends (tests: a fake that records what it is asked);
 * NULL: the real one. */
void ptz_set_drivers(const ptz_drv_t *netip, const ptz_drv_t *onvif);

/* The backend a camera gets: auto picks NETIP for netip:// and ONVIF for
 * rtsp:// (capture: and the rest, none). */
ptz_backend_t ptz_backend_for(const ptz_camera_t *cam);

/* NETIP: the command a move is (NULL: none) and its Step (1..8), the
 * OPPTZControl body for it. Pan/tilt first, then zoom, focus, iris: a
 * NETIP camera does one at a time. */
const char *ptz_netip_command(const double v[PTZ_AXES], int *step);
/* {"Name":"OPPTZControl","SessionID":...,"OPPTZControl":{"Command":cmd,
 * "Parameter":{...,"Channel":ch,"Preset":preset,"Step":step,"Tour":tour}}};
 * length, or -1 when it does not fit. */
int ptz_netip_body(char *out, size_t cap, unsigned session, const char *cmd, int ch, int preset, int step,
                   int tour);

#endif /* PTZ_H */
