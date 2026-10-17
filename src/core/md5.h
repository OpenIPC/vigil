/* SPDX-License-Identifier: GPL-3.0-or-later */
/* md5.h — minimal MD5 (RFC 1321): RTSP Digest auth, NETIP password hash. */
#ifndef MD5_H
#define MD5_H

#include <stddef.h>
#include <stdint.h>

/* Raw 16-byte digest of `len` bytes at `data`. */
void md5_raw(const void *data, size_t len, uint8_t out[16]);

/* Lower-case hex digest of `len` bytes at `data` into `hex` (33 bytes). */
void md5_hex(const void *data, size_t len, char hex[33]);

#endif /* MD5_H */
