/* SPDX-License-Identifier: GPL-3.0-or-later */
/* secret.h — the credential store's cipher: passwords kept in vigil's
 * files (camera, mail, DDNS and network storage passwords in vigil.yaml,
 * users' passwords in the users file) are sealed with AES-256-GCM
 * (mbedTLS) under one key of 32 random bytes in a file of its own,
 * vigil.key (docs/security.md).
 *
 * A sealed value is text that fits anywhere a password did:
 *
 *   enc:v1:<base64(nonce[12] | ciphertext | tag[16])>
 *
 * with a fresh random nonce per sealing, and the field it belongs to as
 * the additional data ("config:cameras[0].url", "users:anna.password"):
 * a value moved to another field does not open there.
 *
 * The key file is made the first time something is sealed and there is
 * none (not at start-up: the kernel's random numbers may not be ready
 * then), mode 0600, written atomically; it is never replaced. Without it
 * (deleted, another board's file) sealed values cannot be opened: they
 * count as unknown, and are set again. Thread-safe. */
#ifndef SECRET_H
#define SECRET_H

#include <stddef.h>

#define SECRET_PREFIX   "enc:v1:"
#define SECRET_KEY_LEN  32
#define SECRET_FILE     "vigil.key"
/* How long sealing waits for the kernel's random numbers (vg_random.h)
 * when they are not ready yet: only in the first moments after a first
 * power-on, and only the save that seals waits. */
#define SECRET_RAND_WAIT_MS 5000

/* The sealed length of a value of `len` bytes, with the NUL. */
#define SECRET_SEALED_SIZE(len) (sizeof SECRET_PREFIX + 4 * ((12 + (len) + 16 + 2) / 3))

typedef enum {
    SECRET_OK = 0,
    SECRET_ENOKEY = -1,         /* no key file (open), or none could be made (seal) */
    SECRET_EBADKEY = -2,        /* the key file is there but unusable (size, mode, I/O) */
    SECRET_EAUTH = -3,          /* does not open with this key and field: another key,
                                 * another field's value, or changed */
    SECRET_EFORMAT = -4,        /* not a sealed value, or does not fit `cap` */
    SECRET_ENOMEM = -5,         /* no memory to put a value in place (config.c) */
    SECRET_ENORAND = -6,        /* the kernel's random numbers are not ready yet (seal):
                                 * just after power-on; nothing sealed, try again */
} secret_rc_t;

/* The key file: `path`, or NULL for the default, vigil.key in the
 * directory of the configuration file (config_path()). Forgets the key
 * read so far. */
void secret_set_key_path(const char *path);
/* The key file in force, into `out`. */
const char *secret_key_path(char *out, size_t cap);

/* Read the key, or make it when there is no file (waiting for random
 * numbers as sealing does). SECRET_OK, or the reason. */
secret_rc_t secret_ensure_key(void);

/* `v` is a sealed value (starts with SECRET_PREFIX). */
int secret_is_sealed(const char *v);

/* Seal `plain` for field `aad` into `out` (SECRET_SEALED_SIZE(strlen(plain))
 * bytes are enough). Makes the key file when there is none. */
secret_rc_t secret_seal(const char *aad, const char *plain, char *out, size_t cap);

/* Open the sealed `in` of field `aad` into `out` (NUL-terminated). On
 * any failure `out` is "". */
secret_rc_t secret_open(const char *aad, const char *in, char *out, size_t cap);

/* ---- configuration exports (docs/security.md, "Configuration export") ----------------
 * A value sealed with a key of the caller's (derived from a password with
 * secret_derive) instead of vigil.key: SECRET_EXPORT_PREFIX and the same
 * base64(nonce | ciphertext | tag), the field as additional data. Never
 * opened by secret_open, so a copied export cannot pass for the board's own
 * file. */
#define SECRET_EXPORT_PREFIX "enc:p1:"
secret_rc_t secret_seal_with(const unsigned char key[SECRET_KEY_LEN], const char *aad, const char *plain, char *out,
                             size_t cap);
secret_rc_t secret_open_with(const unsigned char key[SECRET_KEY_LEN], const char *aad, const char *in, char *out,
                             size_t cap);
int         secret_is_export_sealed(const char *v);
/* PBKDF2-HMAC-SHA256 of `password` (mbedTLS): 0, or -1. */
int         secret_derive(const char *password, const unsigned char *salt, size_t salt_len, unsigned iterations,
                          unsigned char key[SECRET_KEY_LEN]);
/* `len` strong random bytes (vg_random.h), waiting for the kernel's pool
 * as sealing does: 0, or -1 (errno EAGAIN: not ready yet). */
int         secret_random(void *buf, size_t len);
/* A new vigil.key in place of the old one (a reset of vigil's settings:
 * every value sealed with the old key is lost with it). SECRET_OK, or the
 * reason (the old key is kept then). */
secret_rc_t secret_new_key(void);

/* For the log: what a secret_rc_t means. */
const char *secret_strerror(secret_rc_t rc);

/* Overwrite `n` bytes at `p` (a password's copy) so the compiler keeps it. */
void secret_wipe(void *p, size_t n);

#endif /* SECRET_H */
