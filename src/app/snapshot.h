/* SPDX-License-Identifier: GPL-3.0-or-later */
/* snapshot.h — still pictures of the cameras (docs/api.md, Snapshots).
 *
 * Where a picture comes from (snapshot.source, or the caller's choice):
 *   camera  the camera's own JPEG (snap_cam.h): cameras[].snapshot_url
 *           when set, else NETIP's snapshot for a netip:// camera, else
 *           ONVIF's GetSnapshotUri (remembered per camera). Full size, the
 *           camera's encoder. A camera found to have none is not asked
 *           again for SNAPSHOT_ONVIF_RETRY s, by ONVIF nor, under auto, at
 *           all.
 *   nvr     the picture the recorder decodes for the screen
 *           (hal_display_still), encoded here (jpeg_enc.h) at
 *           snapshot.quality: at the size it is decoded at, and only for a
 *           channel that is decoding (on the screen; decoders are scarce).
 *   auto    camera, else nvr.
 * Kept pictures go to the snapshot directory (snap_store.h), the oldest
 * going past snapshot.max_mb. Cameras with snapshot_on_event "on" get
 * snapshot.event_count pictures, snapshot.event_interval s apart, when an
 * event starts (cam_events.h), named after it, mailed when snapshot.mail
 * and smtp.enabled say so.
 *
 * Any thread unless said; the blocking calls take seconds at most.
 */
#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <stddef.h>

#define SNAPSHOT_ONVIF_RETRY 600

typedef enum { SNAPSHOT_AUTO = 0, SNAPSHOT_CAMERA, SNAPSHOT_NVR } snapshot_src_t;

/* "auto", "camera", "nvr" -> the source; -1 for another. */
int snapshot_src(const char *s);

/* Channel `ch`'s (0-based) picture now: 0 with *jpeg (malloc'd) and *len,
 * `used` the way it came ("camera/netip", "camera/onvif", "camera/url",
 * "nvr"); -1 with `err` and *status an HTTP status for it (404 no camera
 * there, 503 no picture to be had, 502 the camera failed). */
int snapshot_take(int ch, snapshot_src_t src, unsigned char **jpeg, size_t *len, char *used, size_t ucap,
                  int *status, char *err, size_t cap);

/* ... and kept, as `kind` ("manual", an event's name): 0 with `name` (the
 * file's, in the snapshot directory) and `used`; -1 as above (507 when
 * there is no disk for it). */
int snapshot_save(int ch, snapshot_src_t src, const char *kind, char *name, size_t ncap, char *used, size_t ucap,
                  int *status, char *err, size_t cap);

/* The worker (event pictures, the screen's requests) and the event
 * subscription. 0, or -1. */
int  snapshot_start(void);
void snapshot_stop(void);

/* A picture of channel `ch` kept, by the worker (the screen's: it must not
 * wait). The request's id, or -1 when the queue is full or not running. */
int snapshot_request(int ch);
/* How request `id` went: 0 still to come; 1 kept, `msg` the file's name;
 * -1 failed (or forgotten), `msg` why. */
int snapshot_result(int id, char *msg, size_t cap);

#endif /* SNAPSHOT_H */
