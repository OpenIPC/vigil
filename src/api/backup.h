/* SPDX-License-Identifier: GPL-3.0-or-later */
/* backup.h — the recorder's configuration as one file, and the resets
 * (docs/management.md, "Configuration"; docs/security.md, "Configuration
 * export").
 *
 * An export is a ustar archive (tar.h), vigil-config-<host>-<date>.tar:
 *
 *   manifest.json      what it is: format, vigil and firmware versions,
 *                      board, host name, MAC, date, what is inside and
 *                      what is not, the key derivation, checksums
 *   vigil.yaml         vigil's settings (the cameras, recording, the
 *                      services, the UI, the servers)
 *   vigil.users.yaml   vigil's users and groups
 *   system.json        what vigil manages of the system: host name, time
 *                      zone, NTP servers, the Ethernet interface, the
 *                      Wi-Fi network and wlandev of the U-Boot environment
 *
 * No password is ever in it in the clear, and vigil.key never: with a
 * password typed at export, every password (cameras', the services', the
 * users', root's, the Wi-Fi passphrase) is opened with vigil.key and
 * sealed again (AES-256-GCM, the field as additional data, "enc:p1:")
 * with a key derived from that password (PBKDF2-HMAC-SHA256, a random
 * salt, the iterations measured to take about a second here), and the
 * manifest seals a check value and the files' SHA-256 the same way, so a
 * wrong password and a changed file are told apart and refused. Without a
 * password, the passwords are left out.
 *
 * An import reads and checks everything first (versions, board,
 * checksums, the password, every value), says what would change
 * (backup_summary_t), and only then replaces the users file and the
 * configuration (each atomically; the users file put back if the
 * configuration is refused), with every password sealed with this
 * board's own key. The host name, the zone and NTP apply at once; the
 * Ethernet and Wi-Fi settings are written for the next boot.
 *
 * One operation at a time (BACKUP_EBUSY). Thread-safe otherwise.
 */
#ifndef BACKUP_H
#define BACKUP_H

#include <stddef.h>

#define BACKUP_FORMAT         "vigil-config"
#define BACKUP_FORMAT_VERSION 1
#define BACKUP_FILE_MAX       (1u << 20)        /* an import is refused beyond this */

typedef enum {
    BACKUP_OK = 0,
    BACKUP_EINVAL = -1,             /* not an export, or a value in it is wrong: `err` */
    BACKUP_EPASSWORD = -2,          /* the export needs its password, or it is not this one */
    BACKUP_ECHANGED = -3,           /* the file was changed or damaged since it was made */
    BACKUP_EVERSION = -4,           /* made by a newer vigil: not read */
    BACKUP_EIO = -5,                /* reading, writing or applying failed; nothing changed: `err` */
    BACKUP_EUNMANAGED = -6,         /* not a system vigild manages (the full factory reset) */
    BACKUP_EBUSY = -7,              /* another export, import or reset is under way */
} backup_rc_t;

/* What the platform calls itself (hal_caps()->name), for the manifest. */
void backup_set_platform(const char *name);
/* The image's vigil.yaml, which a reset goes back to (default: "/rom"
 * and the configuration's path); tests point it elsewhere. */
void backup_set_defaults(const char *path);
/* How long the key derivation should take (default 1000 ms; at least
 * 100000 iterations whatever the measurement). */
void backup_set_kdf_ms(unsigned ms);

/* ---- export --------------------------------------------------------------------- */

/* The export as bytes (`*out`, malloc'd, `*len`) and its file name.
 * `password` NULL or "": without the passwords. */
int backup_export(const char *password, char **out, size_t *len, char *name, size_t ncap, char *err, size_t cap);
/* The same, written into directory `dir` (a mounted disk); `path` gets
 * the file. */
int backup_export_to(const char *dir, const char *password, char *path, size_t pcap, char *err, size_t cap);

/* Where an export can go and come from: the mounted, writable disks
 * (partitions under /mnt) and the network storage when mounted. */
typedef struct {
    char path[128];
    char label[128];                /* "sdb1  exFAT  12.3 GB free  SanDisk Ultra" */
} backup_place_t;

typedef struct {
    char      path[256];
    char      name[100];
    long long size, mtime;
} backup_file_t;

int backup_places(backup_place_t *out, int max);
/* vigil-config*.tar at the top of each place, the newest first. */
int backup_files(backup_file_t *out, int max);

/* ---- import --------------------------------------------------------------------- */

#define BACKUP_LINES 24

typedef struct {
    /* the export */
    char from_host[64], from_mac[18], from_version[32], from_board[48], created[32];
    int  secrets;                   /* 1: passwords sealed with a password; 0: left out */
    /* what changes */
    int  cameras_now, cameras_new;
    int  users_now, users_new, users_no_password, groups_new;
    int  root_password;             /* root's password is set from the export */
    int  system;                    /* the export has the system's settings, and they apply here */
    int  hostname_change, tz_change, ntp_change, net_change, wifi_change;
    int  net_cuts;                  /* after the reboot the recorder may be at another address */
    int  reboot;                    /* network settings apply at the next boot */
    int  applied;                   /* backup_import with apply: done */
    char host_now[64], host_new[64];
    /* in words, for the screen and the API: kind 0 information, 1 warning */
    int  n_lines;
    int  kind[BACKUP_LINES];
    char line[BACKUP_LINES][200];
} backup_summary_t;

typedef struct {
    const char *password;           /* the export's, when it has one */
    int         apply;              /* 0: only check and summarise (a dry run) */
    int         hostname;           /* take the export's host name (default 1) */
} backup_import_opts_t;

/* Check (and with o->apply, apply) the export `data`. The summary is
 * filled as far as the file could be read, also on errors. */
int backup_import(const void *data, size_t len, const backup_import_opts_t *o, backup_summary_t *s, char *err,
                  size_t cap);
/* The same from a file (a path backup_files listed). */
int backup_import_file(const char *path, const backup_import_opts_t *o, backup_summary_t *s, char *err,
                       size_t cap);

/* ---- reset ---------------------------------------------------------------------- */

/* vigil's settings back to the image's (backup_set_defaults), no users, a
 * new vigil.key; in force at once. With `network`, also the interfaces'
 * stanzas back to the image's and the Wi-Fi network forgotten, and a
 * reboot asked for (mgmt_power_request). `*reboot` says whether one was. */
int backup_reset_vigil(int network, int *reboot, char *err, size_t cap);
/* OpenIPC's factory reset (firstboot: the overlay erased, then a reboot),
 * after vigild has stopped cleanly. Recordings on the disks stay. */
int backup_reset_factory(char *err, size_t cap);

#endif /* BACKUP_H */
