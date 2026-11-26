/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sha1.h — SHA-1 (RFC 3174) and base64 (RFC 4648), for ONVIF's WS-Security
 * UsernameToken digest: base64(sha1(nonce + created + password)). */
#ifndef SHA1_H
#define SHA1_H

#include <stddef.h>
#include <stdint.h>

void sha1_raw(const void *data, size_t len, uint8_t out[20]);

/* base64 of `len` bytes into `out` (NUL-terminated); -1 if it does not fit
 * (cap must be at least 4 * ceil(len / 3) + 1). */
int base64_encode(const void *data, size_t len, char *out, size_t cap);

#endif /* SHA1_H */
