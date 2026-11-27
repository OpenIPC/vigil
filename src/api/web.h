/* SPDX-License-Identifier: GPL-3.0-or-later */
/* web.h — the zero channel's web server (web.c, docs/api.md "Zero
 * channel"): the monitor's picture and its mouse and keyboard in a
 * browser, at web.listen (port 80), for users with the remote permission.
 * A build with -DVIGIL_API=OFF has none (api_none.c). */
#ifndef WEB_H
#define WEB_H

#include <stddef.h>

/* After the UI is up (kvm.h). 0 when serving or off (logged), -1 when it
 * could not listen (logged; vigild runs on without it). */
int  web_start(void);
void web_stop(void);

/* As api_restart and api_listening (api.h), for web.listen. */
int  web_restart(const char *listen, char *err, size_t cap);
int  web_listening(char *listen, size_t cap, char *why, size_t wcap);

#endif /* WEB_H */
