/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_curl.h — mail, dynamic DNS and the alarm center, through curl.
 *
 * OpenIPC images carry curl built against mbedTLS with HTTP(S) and SMTP(S)
 * (libcurl-openipc; IMAP, POP3 and the like are left out). vigild uses it
 * rather than linking a TLS library of its own. Passwords never go on a
 * command line (any user can read those in /proc): curl reads its
 * options from stdin (-K -), and the message or payload from a file
 * vigild writes with mode 0600 and removes afterwards.
 */
#ifndef SYS_CURL_H
#define SYS_CURL_H

#include <stddef.h>

#include "config.h"

/* Is curl there? (Without it nothing here works; the pages say so.) */
int curl_available(void);

/* One mail to every address of s->to. 0 when the server took it, -1 with
 * `msg` (curl's own words: "Login denied", "SSL connect error", ...). */
int mail_send(const config_smtp_t *s, const char *subject, const char *body, char *msg, size_t cap);

/* The same with files attached (JPEGs, image/jpeg in base64, each named
 * by its base name), at most MAIL_FILES_MAX of 8 MiB each. */
#define MAIL_FILES_MAX 10
int mail_send_files(const config_smtp_t *s, const char *subject, const char *body, const char *const *files, int n,
                    char *msg, size_t cap);

/* Tell the dynamic DNS service this host's address (the service sees
 * where the request comes from). 0 on "good"/"nochg" (or a 2xx for a
 * custom URL), -1 with `msg`. */
int ddns_update(const config_ddns_t *d, char *msg, size_t cap);

/* POST `json` to `url` (http://host:port/path). 0 on a 2xx. */
int http_post_json(const char *url, const char *json, char *msg, size_t cap);

#endif /* SYS_CURL_H */
