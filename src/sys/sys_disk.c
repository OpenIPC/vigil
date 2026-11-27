/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_disk.c — the recorder's disks (see sys_disk.h). */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/statvfs.h>

#include "sys.h"
#include "sys_disk.h"

#define MIB          (1024u * 1024u)
#define PART_START   2048u                  /* sectors: 1 MiB, aligned for any flash */
#define MBR_MAX_SECT 0xFFFFFFFFull          /* an MBR entry counts sectors in 32 bits */
#define MIN_BYTES    (16ull * MIB)
#define MKFS_SECS    900                    /* a large disk takes minutes */

/* The filesystems a medium may be mounted as, in automount.sh's order:
 * never `auto`, which on these kernels reaches yaffs before vfat and can
 * oops the kernel on a card it does not recognise. */
static const char *const MOUNT_TYPES[] = {
    "vfat", "exfat", "ext4", "ext3", "ext2", "f2fs", "xfs", "msdos", "ntfs",
};

/* ---- /proc/mounts and the kernel's command line ----------------------------------- */

typedef struct {
    char dev[64];
    char mnt[128];
    char fs[16];
    int  ro;
} mnt_t;

#define MOUNTS_MAX 64

/* /proc/mounts escapes space, tab, newline and backslash as \040 ... */
static void unescape(char *s)
{
    char *o = s;
    for (char *p = s; *p; ++p) {
        if (p[0] == '\\' && p[1] >= '0' && p[1] <= '3' && p[2] >= '0' && p[2] <= '7' && p[3] >= '0' &&
            p[3] <= '7') {
            *o++ = (char)((p[1] - '0') * 64 + (p[2] - '0') * 8 + (p[3] - '0'));
            p += 3;
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
}

static int read_mounts(mnt_t *m, int max)
{
    char *buf = malloc(32768);
    if (!buf) return 0;
    int n = 0;
    if (sys_read("/proc/mounts", buf, 32768) > 0) {
        char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line && n < max; line = strtok_r(NULL, "\n", &save)) {
            char dev[128], mnt[256], fs[32], opts[256];
            if (sscanf(line, "%127s %255s %31s %255s", dev, mnt, fs, opts) != 4) continue;
            unescape(dev);
            unescape(mnt);
            snprintf(m[n].dev, sizeof m[n].dev, "%.63s", dev);
            snprintf(m[n].mnt, sizeof m[n].mnt, "%.127s", mnt);
            snprintf(m[n].fs, sizeof m[n].fs, "%.15s", fs);
            m[n].ro = !strncmp(opts, "ro", 2) && (opts[2] == 0 || opts[2] == ',');
            ++n;
        }
    }
    free(buf);
    return n;
}

/* root= on the kernel's command line, without "/dev/": the root
 * filesystem shows as /dev/root in /proc/mounts, which names no device. */
static void cmdline_root(char *out, size_t cap)
{
    char buf[2048];
    out[0] = 0;
    if (sys_read("/proc/cmdline", buf, sizeof buf) <= 0) return;
    for (char *save = NULL, *w = strtok_r(buf, " \t\n", &save); w; w = strtok_r(NULL, " \t\n", &save))
        if (!strncmp(w, "root=", 5)) {
            const char *v = w + 5;
            if (!strncmp(v, "/dev/", 5)) v += 5;
            snprintf(out, cap, "%s", v);
        }
}

static int under_mnt(const char *mnt) { return !strncmp(mnt, "/mnt/", 5) && mnt[5]; }

/* Is the recording directory on this mount? */
static int holds(const char *mnt, const char *path)
{
    size_t n = strlen(mnt);
    return path && n && !strncmp(path, mnt, n) && (path[n] == 0 || path[n] == '/');
}

/* ---- the superblock --------------------------------------------------------------- */

/* The filesystem on /dev/<name>, by its magic; "" when none is known. */
static void probe_fs(const char *name, char *fs, size_t cap)
{
    char dev[64], p[512];
    unsigned char b[4096];
    fs[0] = 0;
    snprintf(dev, sizeof dev, "/dev/%s", name);
    int fd = open(sys_path(p, sizeof p, dev), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t n = pread(fd, b, sizeof b, 0);
    close(fd);
    if (n < 1100) return;
    if (!memcmp(b + 3, "EXFAT   ", 8)) snprintf(fs, cap, "exfat");
    else if (!memcmp(b + 3, "NTFS    ", 8)) snprintf(fs, cap, "ntfs");
    else if (!memcmp(b, "XFSB", 4)) snprintf(fs, cap, "xfs");
    else if (b[0x438] == 0x53 && b[0x439] == 0xEF) snprintf(fs, cap, "ext4");
    else if (b[0x400] == 0x10 && b[0x401] == 0x20 && b[0x402] == 0xF5 && b[0x403] == 0xF2)
        snprintf(fs, cap, "f2fs");
    else if (b[510] == 0x55 && b[511] == 0xAA &&
             (!memcmp(b + 0x52, "FAT32   ", 8) || !memcmp(b + 0x36, "FAT16   ", 8) ||
              !memcmp(b + 0x36, "FAT12   ", 8)))
        snprintf(fs, cap, "vfat");
}

/* ---- listing ----------------------------------------------------------------------- */

static int skipped(const char *name)
{
    static const char *const SKIP[] = { "loop", "ram", "zram", "mtdblock", "nbd", "mtd" };
    for (size_t i = 0; i < sizeof SKIP / sizeof SKIP[0]; ++i)
        if (!strncmp(name, SKIP[i], strlen(SKIP[i]))) return 1;
    return name[0] == '.';
}

static int by_name(const void *a, const void *b)
{
    const char *x = a, *y = b;
    /* sda2 before sda10 */
    size_t lx = strlen(x), ly = strlen(y);
    return lx != ly ? (lx < ly ? -1 : 1) : strcmp(x, y);
}

/* Names in directory `dir` (under the root) for which keep() holds. */
static int names(const char *dir, char out[][32], int max, int (*keep)(const char *dir, const char *name))
{
    char p[512];
    DIR *d = opendir(sys_path(p, sizeof p, dir));
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max)
        if (strlen(e->d_name) < 32 && keep(dir, e->d_name)) snprintf(out[n++], 32, "%.31s", e->d_name);
    closedir(d);
    qsort(out, (size_t)n, 32, by_name);
    return n;
}

static int keep_disk(const char *dir, const char *name)
{
    (void)dir;
    return !skipped(name);
}

static int keep_part(const char *dir, const char *name)
{
    char rel[256];
    snprintf(rel, sizeof rel, "%s/%s/partition", dir, name);
    return name[0] != '.' && sys_exists(rel);
}

static void model_of(const char *disk, char *out, size_t cap)
{
    char rel[128], vendor[64], model[64];
    snprintf(rel, sizeof rel, "/sys/block/%s/device/vendor", disk);
    sys_read_line(rel, vendor, sizeof vendor);
    snprintf(rel, sizeof rel, "/sys/block/%s/device/model", disk);
    sys_read_line(rel, model, sizeof model);
    char *v = vendor, *m = model;
    while (*v == ' ') ++v;
    while (*m == ' ') ++m;
    snprintf(out, cap, "%.31s%s%.31s", v, *v && *m ? " " : "", m);
}

static int is_system(const char *name, const mnt_t *m, int nm, const char *root)
{
    char dev[64];
    snprintf(dev, sizeof dev, "/dev/%s", name);
    if (root[0] && !strcmp(root, name)) return 1;
    for (int i = 0; i < nm; ++i)
        if (!strcmp(m[i].dev, dev) && !under_mnt(m[i].mnt)) return 1;
    return 0;
}

static void fill(sys_disk_t *d, const char *name, const char *parent, const mnt_t *m, int nm,
                 const char *root, const char *rec_path)
{
    char rel[160];
    memset(d, 0, sizeof *d);
    snprintf(d->name, sizeof d->name, "%s", name);
    snprintf(d->parent, sizeof d->parent, "%s", parent ? parent : "");
    d->is_part = parent != NULL;
    if (parent) snprintf(rel, sizeof rel, "/sys/block/%s/%s/size", parent, name);
    else snprintf(rel, sizeof rel, "/sys/block/%s/size", name);
    long long sect = sys_read_ll(rel, 0);
    d->size = sect > 0 ? (unsigned long long)sect * 512ull : 0;
    const char *disk = parent ? parent : name;
    snprintf(rel, sizeof rel, "/sys/block/%s/removable", disk);
    d->removable = sys_read_ll(rel, 0) == 1;
    snprintf(rel, sizeof rel, "/sys/block/%s/ro", disk);
    d->ro = sys_read_ll(rel, 0) == 1;
    if (!parent) model_of(name, d->model, sizeof d->model);

    char dev[64];
    snprintf(dev, sizeof dev, "/dev/%s", name);
    for (int i = 0; i < nm; ++i)
        if (!strcmp(m[i].dev, dev)) {
            snprintf(d->mnt, sizeof d->mnt, "%s", m[i].mnt);
            snprintf(d->fs, sizeof d->fs, "%s", m[i].fs);
            if (m[i].ro) d->ro = 1;
            break;
        }
    if (!d->fs[0] && d->size) probe_fs(name, d->fs, sizeof d->fs);
    if (d->mnt[0]) {
        char p[512];
        struct statvfs fs;
        if (statvfs(sys_path(p, sizeof p, d->mnt), &fs) == 0) {
            d->total = (unsigned long long)fs.f_blocks * fs.f_frsize;
            d->avail = (unsigned long long)fs.f_bavail * fs.f_frsize;
        }
    }
    d->system = is_system(name, m, nm, root);

    const char *st;
    if (d->system) st = "system";
    else if (!d->size) st = "no medium";
    else if (d->mnt[0]) st = holds(d->mnt, rec_path) ? "recording" : d->ro ? "read-only" : "mounted";
    else if (!d->fs[0]) st = "unformatted";
    else st = "not mounted";
    snprintf(d->state, sizeof d->state, "%s", st);
}

int sys_disk_list(sys_disk_t *out, int max, const char *rec_path)
{
    mnt_t *m = calloc(MOUNTS_MAX, sizeof *m);
    if (!m) return 0;
    int nm = read_mounts(m, MOUNTS_MAX);
    char root[64];
    cmdline_root(root, sizeof root);
    char disks[SYS_DISK_MAX][32];
    int nd = names("/sys/block", disks, SYS_DISK_MAX, keep_disk), n = 0;
    for (int i = 0; i < nd && n < max; ++i) {
        sys_disk_t *d = &out[n++];
        fill(d, disks[i], NULL, m, nm, root, rec_path);
        char dir[64], parts[SYS_DISK_MAX][32];
        snprintf(dir, sizeof dir, "/sys/block/%s", disks[i]);
        int np = names(dir, parts, SYS_DISK_MAX, keep_part);
        int rec = 0, mounted = 0;
        for (int k = 0; k < np && n < max; ++k) {
            sys_disk_t *p = &out[n++];
            fill(p, parts[k], disks[i], m, nm, root, rec_path);
            if (p->system) d->system = 1;
            rec |= !strcmp(p->state, "recording");
            mounted |= p->mnt[0] != 0;
        }
        /* A disk with partitions is what its partitions are; its own
         * superblock is the partition table. */
        if (np) {
            d->fs[0] = 0;
            const char *st = d->system ? "system" : !d->size ? "no medium" : rec ? "recording"
                           : mounted ? "mounted" : d->mnt[0] ? d->state : "not mounted";
            snprintf(d->state, sizeof d->state, "%s", st);
        }
    }
    free(m);
    return n;
}

/* One device by name, with its disk's entry when it is a partition. */
static int find(const char *name, sys_disk_t *all, int n, sys_disk_t **dev, sys_disk_t **disk)
{
    *dev = *disk = NULL;
    for (int i = 0; i < n; ++i)
        if (!strcmp(all[i].name, name)) {
            *dev = &all[i];
            for (int k = i; k >= 0; --k)
                if (!all[k].is_part) {
                    *disk = &all[k];
                    break;
                }
            return 0;
        }
    return -1;
}

/* ---- what can be made ---------------------------------------------------------------- */

/* The first one there is the default. ext4 where the firmware can make
 * it: a journal, so a power cut while recording costs the open segment,
 * not the filesystem. e2fsprogs 1.47 turns on orphan_file and
 * metadata_csum_seed, which the 4.9 kernel mounted in QEMU. exFAT
 * otherwise (files over 4 GiB, read by any computer), FAT32 last. */
static const struct { const char *fs, *tool; } MKFS[] = {
    { "ext4", "mkfs.ext4" }, { "exfat", "mkfs.exfat" }, { "vfat", "mkfs.vfat" },
};

int sys_disk_formats(char out[][8], int max)
{
    int n = 0;
    for (size_t i = 0; i < sizeof MKFS / sizeof MKFS[0] && n < max; ++i)
        if (sys_have(MKFS[i].tool)) snprintf(out[n++], 8, "%s", MKFS[i].fs);
    return n;
}

static int can_format(const char *name, sys_disk_t *all, int n, char *why, size_t cap)
{
    sys_disk_t *dev, *disk;
    if (!name || !*name || strchr(name, '/') || find(name, all, n, &dev, &disk) < 0) {
        if (name && skipped(name)) snprintf(why, cap, "%s is not a disk (flash, RAM or loop device)", name);
        else snprintf(why, cap, "there is no disk %s", name ? name : "");
        return -1;
    }
    if (dev->system || (disk && disk->system)) {
        snprintf(why, cap, "%s holds the system and cannot be formatted", name);
        return -1;
    }
    if (!dev->size) {
        snprintf(why, cap, "%s has no medium", name);
        return -1;
    }
    if (dev->ro) {
        snprintf(why, cap, "%s is read-only", name);
        return -1;
    }
    if (dev->size < MIN_BYTES) {
        snprintf(why, cap, "%s is too small to record onto", name);
        return -1;
    }
    return 0;
}

int sys_disk_can_format(const char *name, char *why, size_t cap)
{
    sys_disk_t *all = calloc(SYS_DISK_MAX, sizeof *all);
    if (!all) {
        snprintf(why, cap, "out of memory");
        return -1;
    }
    int n = sys_disk_list(all, SYS_DISK_MAX, NULL);
    int rc = can_format(name, all, n, why, cap);
    free(all);
    return rc;
}

int sys_disk_mounts(const char *name, char mnts[][128], int max)
{
    mnt_t *m = calloc(MOUNTS_MAX, sizeof *m);
    if (!m) return 0;
    int nm = read_mounts(m, MOUNTS_MAX), n = 0;
    size_t nl = strlen(name);
    for (int i = 0; i < nm && n < max; ++i) {
        if (strncmp(m[i].dev, "/dev/", 5)) continue;
        const char *d = m[i].dev + 5;
        char rel[128];
        snprintf(rel, sizeof rel, "/sys/block/%s/%s/partition", name, d);
        /* The device itself, or one of its partitions per sysfs (sda1 is
         * sda's, but sda is not sd's: the prefix alone does not say). */
        if (!strcmp(d, name) || (!strncmp(d, name, nl) && sys_exists(rel)))
            snprintf(mnts[n++], 128, "%s", m[i].mnt);
    }
    free(m);
    return n;
}

/* ---- doing ------------------------------------------------------------------------------ */

/* Zeros over the first `bytes` of /dev/<name>. */
static int wipe(const char *name, unsigned long long bytes, char *msg, size_t cap)
{
    char dev[64], p[512];
    snprintf(dev, sizeof dev, "/dev/%s", name);
    int fd = open(sys_path(p, sizeof p, dev), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(msg, cap, "cannot open %s: %s", dev, strerror(errno));
        return -1;
    }
    static const unsigned char zero[65536];
    int ok = 1;
    for (unsigned long long off = 0; ok && off < bytes; off += sizeof zero) {
        size_t len = bytes - off < sizeof zero ? (size_t)(bytes - off) : sizeof zero;
        ok = pwrite(fd, zero, len, (off_t)off) == (ssize_t)len;
    }
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) < 0) ok = 0;
    if (!ok) snprintf(msg, cap, "cannot write to %s: %s", dev, strerror(errno));
    return ok ? 0 : -1;
}

/* One primary partition from PART_START over the rest of the disk. */
static int write_mbr(const char *disk, unsigned long long sectors, unsigned char type, char *msg,
                     size_t cap)
{
    unsigned char mbr[512];
    memset(mbr, 0, sizeof mbr);
    unsigned long long n = sectors - PART_START;
    if (n > MBR_MAX_SECT) n = MBR_MAX_SECT;
    uint32_t sig = (uint32_t)time(NULL) ^ (uint32_t)getpid() << 8;
    for (int i = 0; i < 4; ++i) mbr[440 + i] = (unsigned char)(sig >> (8 * i));
    unsigned char *e = mbr + 446;
    e[0] = 0x00;                                    /* not bootable */
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;          /* CHS: beyond, use the LBA */
    e[4] = type;
    e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    for (int i = 0; i < 4; ++i) {
        e[8 + i] = (unsigned char)(PART_START >> (8 * i));
        e[12 + i] = (unsigned char)(n >> (8 * i));
    }
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    char dev[64], p[512];
    snprintf(dev, sizeof dev, "/dev/%s", disk);
    int fd = open(sys_path(p, sizeof p, dev), O_WRONLY | O_CLOEXEC);
    int ok = fd >= 0 && pwrite(fd, mbr, sizeof mbr, 0) == (ssize_t)sizeof mbr && fsync(fd) == 0;
    int e2 = errno;
    if (fd >= 0 && close(fd) < 0) ok = 0;
    if (!ok) snprintf(msg, cap, "cannot write the partition table of %s: %s", disk, strerror(e2));
    return ok ? 0 : -1;
}

/* sda -> sda1, mmcblk0 -> mmcblk0p1, nvme0n1 -> nvme0n1p1 */
static void first_part(const char *disk, char *out, size_t cap)
{
    size_t n = strlen(disk);
    snprintf(out, cap, "%s%s1", disk, n && disk[n - 1] >= '0' && disk[n - 1] <= '9' ? "p" : "");
}

static int unmount_one(const char *mnt, char *msg, size_t cap)
{
    char p[512];
    if (sys_ops()->umount(sys_path(p, sizeof p, mnt), 0) < 0) {
        if (errno == EBUSY)
            snprintf(msg, cap, "%s is in use (files are open on it); stop what uses it and try again", mnt);
        else
            snprintf(msg, cap, "cannot unmount %s: %s", mnt, strerror(errno));
        return -1;
    }
    /* As automount.sh: an empty mount point left behind would take
     * whatever is written there next onto the flash. */
    if (under_mnt(mnt)) rmdir(p);
    return 0;
}

static int mkdir_mnt(const char *mnt, char *msg, size_t cap)
{
    char p[512];
    sys_path(p, sizeof p, "/mnt");
    mkdir(p, 0755);
    if (mkdir(sys_path(p, sizeof p, mnt), 0755) < 0 && errno != EEXIST) {
        snprintf(msg, cap, "cannot create %s: %s", mnt, strerror(errno));
        return -1;
    }
    return 0;
}

/* Is /dev/<dev> mounted at `mnt` already? */
static int mounted_at(const char *dev, const char *mnt)
{
    mnt_t *m = calloc(MOUNTS_MAX, sizeof *m);
    if (!m) return 0;
    int n = read_mounts(m, MOUNTS_MAX), found = 0;
    for (int i = 0; i < n && !found; ++i) found = !strcmp(m[i].dev, dev) && !strcmp(m[i].mnt, mnt);
    free(m);
    return found;
}

/* A new partition's uevent sends the firmware's automount.sh after it at
 * the same time (it tries its filesystems one by one): two mounts of one
 * device at once both fail with EBUSY. Seen in QEMU on ssr621q after a
 * format, with nothing mounted in the end. So EBUSY is waited out, and a
 * mount automount.sh made at the same place is as good as ours. */
#define MOUNT_BUSY_TRIES 8

static int try_mount(const char *dev, const char *mnt, const char *fs)
{
    char ps[512], pt[512];
    sys_path(ps, sizeof ps, dev);
    sys_path(pt, sizeof pt, mnt);
    for (int i = 0;; ++i) {
        if (sys_ops()->mount(ps, pt, fs, 0, NULL) == 0) return 0;
        int e = errno;
        /* automount.sh removed the mount point after its own try */
        if (e == ENOENT && i + 1 < MOUNT_BUSY_TRIES && mkdir(pt, 0755) == 0) continue;
        errno = e;
        if (errno != EBUSY) break;
        if (mounted_at(dev, mnt)) return 0;
        if (i + 1 >= MOUNT_BUSY_TRIES) {
            errno = EBUSY;
            return -1;
        }
        sleep(1);
    }
    if (errno != ENODEV) return -1;
    /* exfat, ext4 and ntfs are modules on the firmware's kernels; nothing
     * else loads them. msdos and vfat share the fat driver. */
    char out[256];
    sys_run(out, sizeof out, 30, "modprobe", fs, NULL);
    if (sys_ops()->mount(ps, pt, fs, 0, NULL) == 0) return 0;
    return -1;
}

/* Mount /dev/<name> at /mnt/<name> as `fs`, or as each type in turn. */
static int mount_one(const char *name, const char *fs, char *msg, size_t cap)
{
    char dev[64], mnt[128];
    snprintf(dev, sizeof dev, "/dev/%s", name);
    snprintf(mnt, sizeof mnt, "/mnt/%s", name);
    if (mkdir_mnt(mnt, msg, cap) < 0) return -1;
    int err = 0;
    if (fs && *fs) {
        if (try_mount(dev, mnt, fs) == 0) goto ok;
        err = errno;
    } else {
        for (size_t i = 0; i < sizeof MOUNT_TYPES / sizeof MOUNT_TYPES[0]; ++i) {
            if (try_mount(dev, mnt, MOUNT_TYPES[i]) == 0) {
                fs = MOUNT_TYPES[i];
                goto ok;
            }
            /* The first refusal says the most (automount.sh's reasoning):
             * the later ones only name drivers this kernel lacks. */
            if (!err) err = errno;
        }
    }
    char p[512];
    rmdir(sys_path(p, sizeof p, mnt));
    if (err == ENODEV) snprintf(msg, cap, "this kernel cannot mount %s (no %s driver)", name, fs && *fs ? fs : "matching");
    else snprintf(msg, cap, "cannot mount %s: %s", name, strerror(err));
    return -1;
ok:
    snprintf(msg, cap, "%s mounted at %s (%s)", name, mnt, fs);
    return 0;
}

static int mkfs(const char *name, const char *fs, int whole, char *msg, size_t cap)
{
    char dev[64], p[512], out[512];
    snprintf(dev, sizeof dev, "/dev/%s", name);
    sys_path(p, sizeof p, dev);
    int rc;
    if (!strcmp(fs, "exfat"))
        rc = sys_run(out, sizeof out, MKFS_SECS, "mkfs.exfat", "-L", "VIGIL", p, NULL);
    else if (!strcmp(fs, "vfat"))
        /* busybox's mkfs.vfat refuses a whole disk without -I */
        rc = whole ? sys_run(out, sizeof out, MKFS_SECS, "mkfs.vfat", "-I", "-F", "32", "-n", "VIGIL", p, NULL)
                   : sys_run(out, sizeof out, MKFS_SECS, "mkfs.vfat", "-F", "32", "-n", "VIGIL", p, NULL);
    else
        rc = sys_run(out, sizeof out, MKFS_SECS, "mkfs.ext4", "-F", "-L", "vigil", p, NULL);
    if (rc != 0) {
        sys_trim(out);
        const char *last = strrchr(out, '\n');
        snprintf(msg, cap, "making %s on %s failed%s%.160s", fs, name, out[0] ? ": " : "",
                 last ? last + 1 : out);
        return -1;
    }
    return 0;
}

static unsigned char part_type(const char *fs)
{
    return !strcmp(fs, "vfat") ? 0x0c : !strcmp(fs, "ext4") ? 0x83 : 0x07;
}

static int g_settle_ms = 3000;

void sys_disk_set_settle(int ms) { g_settle_ms = ms; }

static ino_t node_ino(const char *name)
{
    char dev[64], p[512];
    struct stat st;
    snprintf(dev, sizeof dev, "/dev/%s", name);
    return stat(sys_path(p, sizeof p, dev), &st) == 0 ? st.st_ino : 0;
}

/* /dev/<part> as the kernel numbers it (/sys/block/<disk>/<part>/dev),
 * made when it is missing. mdev handles each uevent in a process of its
 * own, so the "remove" of the old partition can run after the "add" of
 * the new one and delete the node it just made: seen in QEMU on ssr621q,
 * /dev/sda1 gone while /proc/partitions listed sda1. */
static int ensure_node(const char *disk, const char *part)
{
    char rel[160], buf[32], p[512];
    if (node_ino(part)) return 0;
    snprintf(rel, sizeof rel, "/sys/block/%s/%s/dev", disk, part);
    unsigned maj, min;
    if (sys_read(rel, buf, sizeof buf) <= 0 || sscanf(buf, "%u:%u", &maj, &min) != 2) return -1;
    snprintf(rel, sizeof rel, "/dev/%s", part);
    if (mknod(sys_path(p, sizeof p, rel), S_IFBLK | 0660, makedev(maj, min)) < 0 && errno != EEXIST) return -1;
    chmod(p, 0660);                         /* as mdev.conf makes it, whatever our umask */
    return 0;
}

/* After a new partition table the kernel announces the partitions again:
 * mdev removes /dev/<part> (if it was there) and makes it anew, and
 * automount.sh, run for each, tries to mount the new partition and then
 * removes /mnt/<part>. Working on the node before that is over loses the
 * race: in QEMU on ssr621q the mkfs went to a node about to be replaced
 * and the mount point vanished under the mount. So: wait for a node that
 * is not the old one, then let automount.sh's attempt pass. */
static int wait_for(const char *disk, const char *name, ino_t old)
{
    int tries = g_settle_ms ? 50 : 1;
    for (int i = 0; i < tries; ++i) {
        ino_t now = node_ino(name);
        if (now && (now != old || !g_settle_ms)) break;
        usleep(100 * 1000);
    }
    if (g_settle_ms) usleep((useconds_t)g_settle_ms * 1000);
    return ensure_node(disk, name);
}

int sys_disk_format(const char *name, const char *fs, char *msg, size_t cap)
{
    msg[0] = 0;
    sys_disk_t *all = calloc(SYS_DISK_MAX, sizeof *all);
    if (!all) {
        snprintf(msg, cap, "out of memory");
        return -1;
    }
    int rc = -1, n = sys_disk_list(all, SYS_DISK_MAX, NULL);
    sys_disk_t *dev, *disk;
    char kinds[3][8];
    int nk = sys_disk_formats(kinds, 3), known = 0;
    for (int i = 0; fs && i < nk; ++i) known |= !strcmp(kinds[i], fs);
    if (can_format(name, all, n, msg, cap) < 0) goto out;
    if (!known) {
        snprintf(msg, cap, "cannot make %s here (no mkfs.%s)", fs ? fs : "?", fs ? fs : "?");
        goto out;
    }
    find(name, all, n, &dev, &disk);

    char mnts[SYS_DISK_MAX][128];
    int nmnt = sys_disk_mounts(name, mnts, SYS_DISK_MAX);
    for (int i = 0; i < nmnt; ++i)
        if (unmount_one(mnts[i], msg, cap) < 0) goto out;

    char target[32];
    int whole = 0;
    if (!dev->is_part) {
        unsigned long long sectors = dev->size / 512;
        /* The old partition's start is wiped too: the new one starts
         * there, and automount.sh mounts whatever it finds on its uevent. */
        if (wipe(name, 2ull * MIB, msg, cap) < 0) goto out;
        if (sectors - PART_START > MBR_MAX_SECT) {
            /* Over 2 TiB an MBR cannot describe it: the filesystem goes
             * on the whole disk, which automount.sh mounts as it is. */
            snprintf(target, sizeof target, "%s", name);
            whole = 1;
        } else {
            if (write_mbr(name, sectors, part_type(fs), msg, cap) < 0) goto out;
            first_part(name, target, sizeof target);
        }
        char dp[64], p[512];
        snprintf(dp, sizeof dp, "/dev/%s", name);
        ino_t old_node = whole ? 0 : node_ino(target);
        if (sys_ops()->reread_parts(sys_path(p, sizeof p, dp)) < 0) {
            snprintf(msg, cap, "the kernel kept the old partition table of %s (%s); unplug it and try again",
                     name, strerror(errno));
            goto out;
        }
        if (!whole && wait_for(name, target, old_node) < 0) {
            snprintf(msg, cap, "%s did not appear after the new partition table", target);
            goto out;
        }
        /* automount.sh may have tried the new partition meanwhile. */
        char again[SYS_DISK_MAX][128];
        int na = sys_disk_mounts(name, again, SYS_DISK_MAX);
        for (int i = 0; i < na; ++i)
            if (unmount_one(again[i], msg, cap) < 0) goto out;
    } else {
        snprintf(target, sizeof target, "%s", name);
    }
    if (wipe(target, MIB, msg, cap) < 0) goto out;
    if (mkfs(target, fs, whole, msg, cap) < 0) goto out;
    char mmsg[160];
    if (mount_one(target, fs, mmsg, sizeof mmsg) < 0) {
        snprintf(msg, cap, "%s formatted as %s, but %s", name, fs, mmsg);
        goto out;
    }
    snprintf(msg, cap, "%s formatted as %s and mounted at /mnt/%s", name, fs, target);
    rc = 0;
out:
    free(all);
    return rc;
}

int sys_disk_mount(const char *name, char *msg, size_t cap)
{
    msg[0] = 0;
    sys_disk_t *all = calloc(SYS_DISK_MAX, sizeof *all);
    if (!all) {
        snprintf(msg, cap, "out of memory");
        return -1;
    }
    int rc = -1, n = sys_disk_list(all, SYS_DISK_MAX, NULL), done = 0;
    sys_disk_t *dev, *disk;
    if (!name || strchr(name, '/') || find(name, all, n, &dev, &disk) < 0) {
        snprintf(msg, cap, "there is no disk %s", name ? name : "");
        goto out;
    }
    if (dev->system) {
        snprintf(msg, cap, "%s holds the system", name);
        goto out;
    }
    if (!dev->size) {
        snprintf(msg, cap, "%s has no medium", name);
        goto out;
    }
    int idx = (int)(dev - all);
    int has_parts = !dev->is_part && idx + 1 < n && all[idx + 1].is_part;
    if (!has_parts) {
        if (dev->mnt[0]) {
            snprintf(msg, cap, "%s is already mounted at %s", name, dev->mnt);
            rc = 0;
        } else {
            rc = mount_one(name, dev->fs, msg, cap);
        }
        goto out;
    }
    /* A disk: each partition with a filesystem; say the last answer. */
    for (int i = idx + 1; i < n && all[i].is_part; ++i) {
        if (all[i].mnt[0] || !all[i].fs[0] || all[i].system || !all[i].size) continue;
        if (mount_one(all[i].name, all[i].fs, msg, cap) == 0) ++done;
        else goto out;
    }
    if (!done) snprintf(msg, cap, "nothing on %s to mount", name);
    rc = done ? 0 : -1;
out:
    free(all);
    return rc;
}

int sys_disk_unmount(const char *name, char *msg, size_t cap)
{
    msg[0] = 0;
    sys_disk_t *all = calloc(SYS_DISK_MAX, sizeof *all);
    if (!all) {
        snprintf(msg, cap, "out of memory");
        return -1;
    }
    int rc = -1, n = sys_disk_list(all, SYS_DISK_MAX, NULL);
    sys_disk_t *dev, *disk;
    if (!name || strchr(name, '/') || find(name, all, n, &dev, &disk) < 0) {
        snprintf(msg, cap, "there is no disk %s", name ? name : "");
        goto out;
    }
    if (dev->system || (!dev->is_part && disk && disk->system)) {
        snprintf(msg, cap, "%s holds the system and stays mounted", name);
        goto out;
    }
    char mnts[SYS_DISK_MAX][128];
    int nm = sys_disk_mounts(name, mnts, SYS_DISK_MAX);
    if (!nm) {
        snprintf(msg, cap, "%s is not mounted", name);
        rc = 0;
        goto out;
    }
    for (int i = 0; i < nm; ++i)
        if (unmount_one(mnts[i], msg, cap) < 0) goto out;
    snprintf(msg, cap, "%s unmounted; it can be removed", name);
    rc = 0;
out:
    free(all);
    return rc;
}
