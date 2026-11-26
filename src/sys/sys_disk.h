/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_disk.h — the disks of the recorder: list, format, mount, unmount.
 *
 * What the Disk Manager page shows and does, the way OpenIPC firmware
 * handles removable media (its /lib/mdev/automount.sh): a disk or a card
 * is mounted at /mnt/<device> (sda1, mmcblk0p1, or sda for one without a
 * partition table), as one of a named list of filesystems, never `auto`.
 *
 *   list     /sys/block (disks, and partitions: the directories with a
 *            `partition` file), /proc/mounts, /proc/cmdline; the type of
 *            a filesystem not mounted is read from its superblock.
 *   format   a whole disk gets an MBR with one partition from sector 2048
 *            (a disk over 2 TiB, which an MBR cannot describe, none: the
 *            filesystem goes on the whole disk, which automount.sh mounts
 *            as it is); then mkfs and a mount at /mnt/<device>.
 *   mount / unmount   at /mnt/<device>; the mount point is removed after
 *            an unmount, as automount.sh does, so nothing written there
 *            later lands on the flash unseen.
 *
 * A device that holds the system -- mounted at /, /rom or /overlay, named
 * by root= on the kernel's command line, or mounted anywhere outside /mnt
 * -- is never formatted or unmounted, and neither is a disk with such a
 * partition. Flash (mtdblock), loop, RAM and network block devices are
 * not listed at all.
 *
 * Every function blocks (mkfs on a large disk takes minutes): call them
 * from a worker thread, with recording onto the device's mount points
 * held first (sys_disk_mounts, channels_rec_hold), since a mount with a
 * segment open on it cannot be unmounted. Paths go through sys.h (the
 * root, the runner), so the tests run all of it on a fake system.
 */
#ifndef SYS_DISK_H
#define SYS_DISK_H

#include <stddef.h>

#define SYS_DISK_MAX 32

typedef struct {
    char name[32];                  /* "sda", "sda1", "mmcblk0p1" */
    char parent[32];                /* the disk of a partition; "" for a disk */
    int  is_part;
    unsigned long long size;        /* bytes */
    char model[64];                 /* vendor and model (disks) */
    int  removable, ro;
    char fs[16];                    /* mounted as, else read from the superblock:
                                       vfat, exfat, ext4, ntfs, xfs, f2fs; "" unknown */
    char mnt[128];                  /* mount point, "" when not mounted */
    unsigned long long total, avail;    /* of the mounted filesystem */
    int  system;                    /* holds the system: never formatted or unmounted */
    char state[24];                 /* "recording" (the recording directory is on it),
                                       "mounted", "read-only", "not mounted",
                                       "unformatted", "no medium", "system", "error" */
} sys_disk_t;

/* Every disk, each followed by its partitions; `rec_path` (may be NULL or
 * "") marks the one recorded onto. Returns how many were filled. */
int sys_disk_list(sys_disk_t *out, int max, const char *rec_path);

/* The filesystems this system can make, best first: exfat (mkfs.exfat),
 * vfat (mkfs.vfat), ext4 (mkfs.ext4); only those whose tool is there.
 * Returns how many. */
int sys_disk_formats(char out[][8], int max);

/* May `name` be formatted? 0, or -1 with `why` (a system device, not a
 * disk, no medium, no such device). */
int sys_disk_can_format(const char *name, char *why, size_t cap);

/* The mount points of `name` and, for a disk, of its partitions (as
 * /proc/mounts names them). Returns how many. */
int sys_disk_mounts(const char *name, char mnts[][128], int max);

/* Format `name` (a disk: a new partition table, one partition; a
 * partition: just the filesystem) as `fs` (sys_disk_formats) and mount it
 * at /mnt/<device>. Everything on it is lost. 0 with `msg` saying what was
 * done, or -1 with why not. */
int sys_disk_format(const char *name, const char *fs, char *msg, size_t cap);

/* Mount `name` (a disk: each of its partitions that has a filesystem) at
 * /mnt/<device>. 0 or -1, with `msg`. */
int sys_disk_mount(const char *name, char *msg, size_t cap);

/* Unmount `name` (a disk: all its partitions) and remove the mount
 * points. 0 or -1, with `msg` (a mount still in use is named). */
int sys_disk_unmount(const char *name, char *msg, size_t cap);

#endif /* SYS_DISK_H */
