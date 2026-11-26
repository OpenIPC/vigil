/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_nas.h — network storage: an SMB (CIFS) or NFS share as a disk.
 *
 * The share is mounted with mount(2) directly, not mount(8): OpenIPC's
 * busybox is built without its NFS helper, and a password given to the
 * kernel in the mount data never shows on a command line (/proc/PID/
 * cmdline) or in /proc/mounts (cifs hides it). The kernel cannot resolve
 * host names, so the server is resolved here and passed as an address.
 *
 *   cifs  "//IP/share[/dir]", unc=\\IP\share, ip=IP, the user and the
 *         password (commas doubled, the kernel's escape), or guest;
 *         vers=2.1 unless the options say otherwise (kernel 4.9 offers
 *         SMB1 by default, which current servers refuse).
 *   nfs   "IP:/export", addr=IP, nolock (no statd on the board), vers=3.
 *         The ssr621q kernel builds NFS with only version 2
 *         (CONFIG_NFS_V3 is not set): sys_nas_support says so.
 *
 * Messages are for the UI and the API: they never hold the password.
 */
#ifndef SYS_NAS_H
#define SYS_NAS_H

#include <stddef.h>

#include "config.h"

/* Where sys_nas_test mounts the share for a moment. */
#define SYS_NAS_TEST_DIR "/tmp/vigil-nas-test"

/* Can this kernel mount `type` ("cifs", "nfs")? 1; 0 with `why` naming
 * the kernel option that is missing. */
int sys_nas_support(const char *type, char *why, size_t cap);

/* The mount(2) source and data for `n`, with `ip` as the server's
 * address (the data holds the password: clear it after use). 0, or -1
 * with `why`. */
int sys_nas_mount_args(const config_nas_t *n, const char *ip, char *src, size_t scap, char *data,
                       size_t dcap, char *why, size_t wcap);

/* Mount `n` at `target` (made if missing). 0 or -1, with `msg`. */
int sys_nas_mount(const config_nas_t *n, const char *target, char *msg, size_t cap);

/* Unmount `target`. 0 or -1, with `msg`. */
int sys_nas_umount(const char *target, char *msg, size_t cap);

/* Mount `n` at SYS_NAS_TEST_DIR, write, read back and delete a file,
 * unmount. 0 when all of it worked, -1 otherwise; `msg` says how far it
 * got. */
int sys_nas_test(const config_nas_t *n, char *msg, size_t cap);

/* Is a cifs or nfs share mounted at `target` (/proc/mounts)? */
int sys_nas_mounted(const char *target);

#endif /* SYS_NAS_H */
