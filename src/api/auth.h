/* SPDX-License-Identifier: GPL-3.0-or-later */
/* auth.h — the API's password check: HTTP Basic credentials against the
 * system account's hash in a shadow file, through crypt(3). */
#ifndef AUTH_H
#define AUTH_H

#include <stddef.h>

/* "Basic dXNlcjpwYXNz" -> user, password. Returns 0, or -1 if the header
 * is not Basic or does not decode. */
int auth_parse_basic(const char *header, char *user, size_t ul, char *pass, size_t pl);

/* Base64 (RFC 4648, padding optional). Returns the decoded length, or -1. */
int auth_base64_decode(const char *in, size_t len, unsigned char *out, size_t cap);

/* Check `password` against `user`'s hash in `shadow`. Returns 1 when it
 * matches, 0 when it does not, -1 when it cannot be checked (no file, no
 * such user, a locked account or one without a password) with `why`. */
int auth_check_shadow(const char *shadow, const char *user, const char *password,
                      char *why, size_t cap);

/* crypt(3) of `password` with `setting` (a hash, or "$6$salt"), copied
 * into `out` under the lock crypt's static buffer needs. 0, or -1 when
 * the C library cannot (no such method: NULL or "*..."). */
int auth_crypt(const char *password, const char *setting, char *out, size_t cap);

/* Equal strings, in a time that depends only on their lengths. */
int auth_equal(const char *a, const char *b);

#endif /* AUTH_H */
