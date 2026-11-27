/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_sys_disk.c — the Disk Manager and network storage on a fake
 * system: listing and states, refusals, formatting (the MBR as written),
 * mounting, unmounting, and the share's mount options. */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "sys.h"
#include "sys_disk.h"
#include "sys_fake.h"
#include "sys_nas.h"

void test_sys_disk(void);

#define MIB (1024 * 1024)

/* A sparse device file of `bytes`, with `magic` written at `off`. */
static void fake_dev(const char *name, long long bytes, long off, const void *magic, size_t len)
{
    char rel[64], p[512];
    snprintf(rel, sizeof rel, "/dev/%s", name);
    fake_file(rel, "");
    snprintf(p, sizeof p, "%s%s", fake_root, rel);
    if (truncate(p, bytes) < 0) perror(p);
    if (magic) {
        int fd = open(p, O_WRONLY);
        if (pwrite(fd, magic, len, off) != (ssize_t)len) perror("pwrite");
        close(fd);
    }
}

static int dev_bytes(const char *name, long off, unsigned char *out, size_t len)
{
    char p[512];
    snprintf(p, sizeof p, "%s/dev/%s", fake_root, name);
    int fd = open(p, O_RDONLY);
    ssize_t n = fd >= 0 ? pread(fd, out, len, off) : -1;
    if (fd >= 0) close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

static int all_zero(const char *name, long off, size_t len)
{
    unsigned char *b = calloc(1, len);
    int z = dev_bytes(name, off, b, len) == 0;
    for (size_t i = 0; z && i < len; ++i) z = b[i] == 0;
    free(b);
    return z;
}

static unsigned le32(const unsigned char *b) { return b[0] | b[1] << 8 | b[2] << 16 | (unsigned)b[3] << 24; }

static const sys_disk_t *get(const sys_disk_t *d, int n, const char *name)
{
    for (int i = 0; i < n; ++i)
        if (!strcmp(d[i].name, name)) return &d[i];
    return NULL;
}

static void setup(void)
{
    fake_init();
    sys_disk_set_settle(0);
    /* a USB stick, one exFAT partition, mounted where automount.sh puts it */
    fake_file("/sys/block/sda/size", "262144\n");
    fake_file("/sys/block/sda/removable", "1\n");
    fake_file("/sys/block/sda/ro", "0\n");
    fake_file("/sys/block/sda/device/vendor", "SanDisk \n");
    fake_file("/sys/block/sda/device/model", "Cruzer Blade    \n");
    fake_file("/sys/block/sda/sda1/partition", "1\n");
    fake_file("/sys/block/sda/sda1/size", "260096\n");
    fake_dev("sda", 3 * MIB, 0, NULL, 0);
    fake_dev("sda1", 2 * MIB, 3, "EXFAT   ", 8);
    fake_mkdir("/mnt/sda1");
    /* the board's own card: the root filesystem is on it */
    fake_file("/sys/block/mmcblk0/size", "7733248\n");
    fake_file("/sys/block/mmcblk0/mmcblk0p1/partition", "1\n");
    fake_file("/sys/block/mmcblk0/mmcblk0p1/size", "65536\n");
    fake_file("/sys/block/mmcblk0/mmcblk0p2/partition", "2\n");
    fake_file("/sys/block/mmcblk0/mmcblk0p2/size", "1048576\n");
    /* flash, an empty card reader, a blank disk, a 5 TiB disk */
    fake_file("/sys/block/mtdblock0/size", "512\n");
    fake_file("/sys/block/loop0/size", "0\n");
    fake_file("/sys/block/sdb/size", "0\n");
    fake_file("/sys/block/sdc/size", "262144\n");
    fake_file("/sys/block/sdc/device/model", "QEMU HARDDISK\n");
    fake_dev("sdc", 3 * MIB, 0, NULL, 0);
    fake_dev("sdc1", 2 * MIB, 100, "junk-from-before", 16);
    fake_file("/sys/block/sdd/size", "10737418240\n");
    fake_dev("sdd", 3 * MIB, 0, NULL, 0);
    fake_file("/proc/mounts", "/dev/root / squashfs ro,relatime 0 0\n"
                              "proc /proc proc rw 0 0\n"
                              "/dev/sda1 /mnt/sda1 exfat rw,relatime 0 0\n"
                              "/dev/mmcblk0p1 /boot vfat rw 0 0\n"
                              "tmpfs /tmp tmpfs rw 0 0\n");
    fake_file("/proc/cmdline", "console=ttyS0,115200 root=/dev/mmcblk0p2 rootfstype=squashfs\n");
    fake_mkdir("/tmp");
}

static void test_list(void)
{
    setup();
    sys_disk_t d[SYS_DISK_MAX];
    int n = sys_disk_list(d, SYS_DISK_MAX, "/mnt/sda1/rec");
    CHECK(n == 8, "8 devices listed (no flash or loop): %d", n);
    CHECK(!get(d, n, "mtdblock0") && !get(d, n, "loop0"), "flash and loop are not listed");
    const sys_disk_t *a = get(d, n, "sda"), *a1 = get(d, n, "sda1");
    CHECK(a && !a->is_part && a->size == 262144ull * 512 && !strcmp(a->model, "SanDisk Cruzer Blade") &&
          a->removable && !strcmp(a->state, "recording"), "sda: %s, %s", a ? a->model : "", a ? a->state : "");
    CHECK(a1 && a1->is_part && !strcmp(a1->parent, "sda") && !strcmp(a1->fs, "exfat") &&
          !strcmp(a1->mnt, "/mnt/sda1") && !strcmp(a1->state, "recording") && a1->total > 0,
          "sda1 recording: %s %s %s", a1 ? a1->fs : "", a1 ? a1->mnt : "", a1 ? a1->state : "");
    CHECK(a1 == a + 1, "a disk is followed by its partitions");
    const sys_disk_t *m = get(d, n, "mmcblk0"), *m1 = get(d, n, "mmcblk0p1"), *m2 = get(d, n, "mmcblk0p2");
    CHECK(m && m->system && !strcmp(m->state, "system"), "the root's card is system");
    CHECK(m1 && m1->system, "mounted outside /mnt: system");
    CHECK(m2 && m2->system, "root= on the command line: system");
    const sys_disk_t *b = get(d, n, "sdb"), *c = get(d, n, "sdc");
    CHECK(b && !strcmp(b->state, "no medium"), "sdb: %s", b ? b->state : "");
    CHECK(c && !strcmp(c->state, "unformatted") && !c->fs[0], "sdc: %s", c ? c->state : "");
    n = sys_disk_list(d, SYS_DISK_MAX, NULL);
    a1 = get(d, n, "sda1");
    CHECK(a1 && !strcmp(a1->state, "mounted"), "without recording: mounted (%s)", a1 ? a1->state : "");
    fake_file("/proc/mounts", "/dev/sda1 /mnt/sda1 exfat ro,relatime 0 0\n");
    n = sys_disk_list(d, SYS_DISK_MAX, NULL);
    a1 = get(d, n, "sda1");
    CHECK(a1 && !strcmp(a1->state, "read-only") && a1->ro, "read-only mount");
    fake_file("/proc/mounts", "");
    n = sys_disk_list(d, SYS_DISK_MAX, NULL);
    a1 = get(d, n, "sda1");
    CHECK(a1 && !strcmp(a1->state, "not mounted") && !strcmp(a1->fs, "exfat"),
          "not mounted, exFAT from the superblock (%s)", a1 ? a1->fs : "");
    a = get(d, n, "sda");
    CHECK(a && !strcmp(a->state, "not mounted"), "the disk follows its partitions");

    /* the superblocks */
    struct { const char *fs; long off; const char *magic; size_t len; } sb[] = {
        { "ntfs", 3, "NTFS    ", 8 }, { "xfs", 0, "XFSB", 4 }, { "ext4", 0x438, "\x53\xEF", 2 },
        { "f2fs", 0x400, "\x10\x20\xF5\xF2", 4 },
    };
    for (size_t i = 0; i < sizeof sb / sizeof sb[0]; ++i) {
        fake_dev("sda1", 2 * MIB, sb[i].off, sb[i].magic, sb[i].len);
        n = sys_disk_list(d, SYS_DISK_MAX, NULL);
        a1 = get(d, n, "sda1");
        CHECK(a1 && !strcmp(a1->fs, sb[i].fs), "probe %s: %s", sb[i].fs, a1 ? a1->fs : "");
    }
    unsigned char fat[512] = { 0 };
    memcpy(fat + 0x52, "FAT32   ", 8);
    fat[510] = 0x55;
    fat[511] = 0xAA;
    fake_dev("sda1", 2 * MIB, 0, fat, sizeof fat);
    n = sys_disk_list(d, SYS_DISK_MAX, NULL);
    CHECK((a1 = get(d, n, "sda1")) && !strcmp(a1->fs, "vfat"), "probe vfat");

    char mnts[8][128];
    fake_file("/proc/mounts", "/dev/sda1 /mnt/sda1 exfat rw 0 0\n/dev/sda10 /mnt/x vfat rw 0 0\n"
                              "/dev/sdc /mnt/sdc vfat rw 0 0\n");
    CHECK(sys_disk_mounts("sda", mnts, 8) == 1 && !strcmp(mnts[0], "/mnt/sda1"),
          "mounts of sda: its partitions only (sda10 is not one of them)");
    CHECK(sys_disk_mounts("sdc", mnts, 8) == 1 && !strcmp(mnts[0], "/mnt/sdc"), "a disk mounted whole");
    fake_done();
}

static void test_refusals(void)
{
    setup();
    char why[200];
    CHECK(sys_disk_can_format("mmcblk0", why, sizeof why) < 0 && strstr(why, "system"), "root disk: %s", why);
    CHECK(sys_disk_can_format("mmcblk0p2", why, sizeof why) < 0 && strstr(why, "system"), "root partition");
    CHECK(sys_disk_can_format("mmcblk0p1", why, sizeof why) < 0, "a system partition");
    CHECK(sys_disk_can_format("mtdblock0", why, sizeof why) < 0 && strstr(why, "not a disk"), "flash: %s", why);
    CHECK(sys_disk_can_format("sdb", why, sizeof why) < 0 && strstr(why, "no medium"), "no medium: %s", why);
    CHECK(sys_disk_can_format("sdz", why, sizeof why) < 0 && strstr(why, "no disk"), "unknown: %s", why);
    CHECK(sys_disk_can_format("../sda", why, sizeof why) < 0, "a path is no name");
    CHECK(sys_disk_can_format("sda", why, sizeof why) == 0 && sys_disk_can_format("sdc", why, sizeof why) == 0,
          "a USB stick and a blank disk may be formatted");
    char msg[200];
    CHECK(sys_disk_format("mmcblk0p2", "vfat", msg, sizeof msg) < 0 && !fake_ran("mkfs"),
          "formatting the root partition is refused before anything runs");
    CHECK(sys_disk_unmount("mmcblk0p1", msg, sizeof msg) < 0 && !fake_ran("umount"),
          "unmounting a system partition is refused");

    fake_have("mkfs.ext4", 0);
    char kinds[4][8];
    int nk = sys_disk_formats(kinds, 4);
    CHECK(nk == 2 && !strcmp(kinds[0], "exfat") && !strcmp(kinds[1], "vfat"), "formats: exfat, vfat (%d)", nk);
    CHECK(sys_disk_format("sdc", "ext4", msg, sizeof msg) < 0 && strstr(msg, "mkfs.ext4") && !fake_ran("mkfs"),
          "ext4 without mkfs.ext4: %s", msg);
    fake_have("mkfs.ext4", 1);
    nk = sys_disk_formats(kinds, 4);
    CHECK(nk == 3 && !strcmp(kinds[2], "ext4"), "ext4 when mkfs.ext4 is there");
    fake_done();
}

static void test_format(void)
{
    setup();
    char msg[300];
    unsigned char mbr[512];

    /* A blank disk: an MBR with one FAT32 partition, the partition wiped
     * before mkfs, mounted where automount.sh would. */
    CHECK(sys_disk_format("sdc", "vfat", msg, sizeof msg) == 0, "format sdc: %s", msg);
    CHECK(dev_bytes("sdc", 0, mbr, 512) == 0 && mbr[510] == 0x55 && mbr[511] == 0xAA, "MBR signature");
    CHECK(mbr[446 + 4] == 0x0c && le32(mbr + 446 + 8) == 2048 && le32(mbr + 446 + 12) == 262144 - 2048 &&
          mbr[446 + 16 + 4] == 0, "one FAT32 LBA partition from sector 2048: type %02x start %u len %u",
          mbr[446 + 4], le32(mbr + 454), le32(mbr + 458));
    CHECK(all_zero("sdc", 512, 2 * MIB - 512), "the rest of the first 2 MiB is wiped");
    CHECK(all_zero("sdc1", 0, MIB), "the new partition's start is wiped before mkfs");
    char line[700];
    snprintf(line, sizeof line, "reread %s/dev/sdc", fake_root);
    CHECK(fake_ran(line), "the kernel re-reads the table");
    snprintf(line, sizeof line, "mkfs.vfat -F 32 -n VIGIL %s/dev/sdc1", fake_root);
    CHECK(fake_ran(line), "mkfs.vfat on sdc1:\n%s", fake_log);
    snprintf(line, sizeof line, "mount %s/dev/sdc1 %s/mnt/sdc1 vfat", fake_root, fake_root);
    CHECK(fake_ran(line), "mounted at /mnt/sdc1");
    CHECK(strstr(msg, "/mnt/sdc1"), "the message says where: %s", msg);

    /* A mounted stick as exFAT: unmounted first, type 0x07. */
    fake_clear_log();
    CHECK(sys_disk_format("sda", "exfat", msg, sizeof msg) == 0, "format sda: %s", msg);
    snprintf(line, sizeof line, "umount %s/mnt/sda1 0", fake_root);
    CHECK(fake_ran(line), "its partition was unmounted first");
    CHECK(strstr(fake_log, "umount") < strstr(fake_log, "mkfs"), "unmounted before mkfs");
    CHECK(dev_bytes("sda", 0, mbr, 512) == 0 && mbr[446 + 4] == 0x07, "exFAT partition type");
    snprintf(line, sizeof line, "mkfs.exfat -L VIGIL %s/dev/sda1", fake_root);
    CHECK(fake_ran(line), "mkfs.exfat on sda1");

    /* Busy: nothing is touched. */
    fake_clear_log();
    fake_umount_errno = EBUSY;
    CHECK(sys_disk_format("sda", "exfat", msg, sizeof msg) < 0 && strstr(msg, "in use") && !fake_ran("mkfs") &&
          !fake_ran("reread"), "busy: %s", msg);
    fake_umount_errno = 0;

    /* A partition alone: no new table. */
    fake_clear_log();
    CHECK(sys_disk_format("sda1", "vfat", msg, sizeof msg) == 0 && !fake_ran("reread"), "format sda1: %s", msg);
    snprintf(line, sizeof line, "mkfs.vfat -F 32 -n VIGIL %s/dev/sda1", fake_root);
    CHECK(fake_ran(line), "mkfs.vfat on sda1");

    /* Over 2 TiB: no table, the whole disk. */
    fake_clear_log();
    CHECK(sys_disk_format("sdd", "exfat", msg, sizeof msg) == 0, "format sdd: %s", msg);
    CHECK(dev_bytes("sdd", 0, mbr, 512) == 0 && mbr[510] == 0 && mbr[511] == 0, "no MBR on a 5 TiB disk");
    snprintf(line, sizeof line, "mkfs.exfat -L VIGIL %s/dev/sdd\n", fake_root);
    CHECK(fake_ran(line), "mkfs on the whole disk:\n%s", fake_log);
    snprintf(line, sizeof line, "mount %s/dev/sdd %s/mnt/sdd exfat", fake_root, fake_root);
    CHECK(fake_ran(line), "mounted at /mnt/sdd");
    fake_clear_log();
    CHECK(sys_disk_format("sdd", "vfat", msg, sizeof msg) == 0 && fake_ran("mkfs.vfat -I -F 32"),
          "busybox mkfs.vfat on a whole disk needs -I");

    /* mkfs failing says so */
    fake_answer("mkfs.exfat", 1, "exfatprogs version : 1.2.2\nfailed to open device");
    CHECK(sys_disk_format("sdc", "exfat", msg, sizeof msg) < 0 && strstr(msg, "failed to open device"),
          "mkfs failure: %s", msg);
    fake_done();
}

static void test_mount(void)
{
    setup();
    char msg[300], line[700];
    fake_file("/proc/mounts", "");
    fake_dev("sdc", 3 * MIB, 0x438, "\x53\xEF", 2);
    fake_mount_errno = ENODEV;
    CHECK(sys_disk_mount("sdc", msg, sizeof msg) < 0 && fake_ran("modprobe ext4") && strstr(msg, "ext4"),
          "ENODEV: modprobe, then the reason: %s", msg);
    CHECK(!fake_exists("/mnt/sdc"), "a failed mount leaves no mount point");
    fake_mount_errno = 0;
    CHECK(sys_disk_mount("sdc", msg, sizeof msg) == 0, "mount sdc: %s", msg);
    snprintf(line, sizeof line, "mount %s/dev/sdc %s/mnt/sdc ext4", fake_root, fake_root);
    CHECK(fake_ran(line), "as the superblock says");

    /* automount.sh mounting the same device at the same moment: EBUSY,
     * and then its mount at /mnt/sdc counts. */
    fake_mount_errno = EBUSY;
    fake_file("/proc/mounts", "/dev/sdc /mnt/sdc ext4 rw 0 0\n");
    CHECK(sys_disk_mount("sdc", msg, sizeof msg) == 0, "EBUSY with automount's mount there: %s", msg);
    fake_file("/proc/mounts", "");
    CHECK(sys_disk_mount("sdc", msg, sizeof msg) < 0 && strstr(msg, "busy"), "EBUSY that lasts: %s", msg);
    fake_mount_errno = 0;

    /* Unknown filesystem: automount.sh's list, never auto. */
    fake_dev("sdc", 3 * MIB, 0x438, "\0\0", 2);
    fake_clear_log();
    fake_mount_errno = EINVAL;
    CHECK(sys_disk_mount("sdc", msg, sizeof msg) < 0, "nothing mounts it");
    CHECK(fake_ran(" vfat ") && fake_ran(" ntfs ") && !fake_ran(" auto"), "each named type tried, no auto");
    fake_mount_errno = 0;

    /* A disk: its partitions. */
    fake_clear_log();
    CHECK(sys_disk_mount("sda", msg, sizeof msg) == 0, "mount sda: %s", msg);
    snprintf(line, sizeof line, "mount %s/dev/sda1 %s/mnt/sda1 exfat", fake_root, fake_root);
    CHECK(fake_ran(line), "sda1 mounted");
    CHECK(sys_disk_mount("sdb", msg, sizeof msg) < 0 && strstr(msg, "no medium"), "no medium: %s", msg);

    /* Unmount: busy says so; done, the mount point goes. */
    fake_file("/proc/mounts", "/dev/sda1 /mnt/sda1 exfat rw 0 0\n");
    fake_mkdir("/mnt/sda1");
    fake_umount_errno = EBUSY;
    CHECK(sys_disk_unmount("sda", msg, sizeof msg) < 0 && strstr(msg, "in use") && fake_exists("/mnt/sda1"),
          "busy: %s", msg);
    fake_umount_errno = 0;
    CHECK(sys_disk_unmount("sda1", msg, sizeof msg) == 0 && !fake_exists("/mnt/sda1"), "unmounted: %s", msg);
    fake_file("/proc/mounts", "");
    CHECK(sys_disk_unmount("sda1", msg, sizeof msg) == 0 && strstr(msg, "not mounted"), "not mounted: %s", msg);
    fake_done();
}

static void test_nas(void)
{
    fake_init();
    char why[200], msg[300], src[300], data[700];
    CHECK(!sys_nas_support("cifs", why, sizeof why) && strstr(why, "CONFIG_CIFS"), "no cifs: %s", why);
    CHECK(!sys_nas_support("nfs", why, sizeof why) && strstr(why, "CONFIG_NFS_V3"), "no nfs: %s", why);
    /* the ssr621q kernel: cifs, and NFS with version 2 only */
    fake_file("/lib/modules/4.9.84/kernel/fs/cifs/cifs.ko", "");
    fake_file("/lib/modules/4.9.84/kernel/fs/nfs/nfs.ko", "");
    fake_file("/lib/modules/4.9.84/kernel/fs/nfs/nfsv2.ko", "");
    CHECK(sys_nas_support("cifs", why, sizeof why), "cifs.ko: usable");
    CHECK(!sys_nas_support("nfs", why, sizeof why) && strstr(why, "CONFIG_NFS_V3") && strstr(why, "version 3"),
          "NFS v2 only: %s", why);
    fake_file("/lib/modules/4.9.84/kernel/fs/nfs/nfsv3.ko", "");
    CHECK(sys_nas_support("nfs", why, sizeof why), "nfsv3.ko: usable");

    config_nas_t n = { .type = "cifs", .server = "10.0.0.5", .share = "rec/nvr1", .user = "cam",
                       .password = "a,b,c", .options = "" };
    CHECK(sys_nas_mount_args(&n, "10.0.0.5", src, sizeof src, data, sizeof data, why, sizeof why) == 0 &&
          !strcmp(src, "//10.0.0.5/rec/nvr1"), "cifs source: %s", src);
    CHECK(!strcmp(data, "unc=\\\\10.0.0.5\\rec,ip=10.0.0.5,username=cam,password=a,,b,,c,prefixpath=nvr1,vers=2.1"),
          "cifs data: %s", data);
    snprintf(n.options, sizeof n.options, "vers=3.0,uid=0");
    n.user[0] = 0;
    snprintf(n.share, sizeof n.share, "/rec");
    sys_nas_mount_args(&n, "10.0.0.5", src, sizeof src, data, sizeof data, why, sizeof why);
    CHECK(!strcmp(data, "unc=\\\\10.0.0.5\\rec,ip=10.0.0.5,user=guest,password=,vers=3.0,uid=0"),
          "guest: a user with an empty password, own vers: %s", data);
    config_nas_t f = { .type = "nfs", .server = "10.0.0.6", .share = "/export/nvr" };
    CHECK(sys_nas_mount_args(&f, "10.0.0.6", src, sizeof src, data, sizeof data, why, sizeof why) == 0 &&
          !strcmp(src, "10.0.0.6:/export/nvr") && !strcmp(data, "addr=10.0.0.6,nolock,vers=3"),
          "nfs: %s %s", src, data);
    snprintf(f.share, sizeof f.share, "export");
    CHECK(sys_nas_mount_args(&f, "10.0.0.6", src, sizeof src, data, sizeof data, why, sizeof why) < 0,
          "an NFS share is a path");

    /* Mount and test, with the password nowhere but the mount data. */
    fake_mkdir("/tmp");
    fake_mkdir("/mnt");
    config_nas_t c = { .type = "cifs", .server = "10.0.0.5", .share = "rec", .user = "cam",
                       .password = "Pa55,word" };
    CHECK(sys_nas_mount(&c, "/mnt/nas", msg, sizeof msg) == 0 && fake_exists("/mnt/nas"), "mount: %s", msg);
    CHECK(strstr(fake_mounted, "password=Pa55,,word") && strstr(fake_mounted, " cifs "), "cifs mount(2)");
    CHECK(fake_ran("modprobe cifs"), "the module is loaded");
    CHECK(!strstr(msg, "Pa55"), "no password in the message");
    /* EINVAL for another reason (the 4.9 client's "No username
     * specified"): no SMB1 retry, no blaming SMB1, the kernel's words. */
    snprintf(fake_klog, sizeof fake_klog, "<3>[  12.0] CIFS VFS: No username specified\n");
    fake_mount_errno_once = EINVAL;
    CHECK(sys_nas_mount(&c, "/mnt/nas", msg, sizeof msg) < 0 && !strstr(fake_mounted, "vers=1.0") &&
          !strstr(msg, "SMB1") && strstr(msg, "No username specified"), "other EINVAL: %s", msg);
    /* A kernel without SMB2 (CONFIG_CIFS_SMB2): "Unknown vers=" for 2.1, then SMB1. */
    snprintf(fake_klog, sizeof fake_klog, "<3>[  13.0] CIFS VFS: Unknown vers= option specified: 2.1\n");
    fake_mount_errno_once = EINVAL;
    CHECK(sys_nas_mount(&c, "/mnt/nas", msg, sizeof msg) == 0 && strstr(fake_mounted, "vers=1.0") &&
          strstr(msg, "SMB1") && !strstr(msg, "Pa55"), "SMB1 fallback: %s", msg);
    fake_mount_errno_once = EINVAL;
    fake_mount_errno = EHOSTUNREACH;
    CHECK(sys_nas_mount(&c, "/mnt/nas", msg, sizeof msg) < 0 && strstr(msg, "CONFIG_CIFS_SMB2") &&
          !strstr(msg, "nvalid"), "SMB1 fallback, the real error: %s", msg);
    fake_klog[0] = 0;
    fake_mount_errno = 0;
    snprintf(c.options, sizeof c.options, "vers=3.0");
    fake_mount_errno_once = EINVAL;
    CHECK(sys_nas_mount(&c, "/mnt/nas", msg, sizeof msg) < 0 && !strstr(fake_mounted, "vers=1.0"),
          "a vers= of one's own is not second-guessed: %s", msg);
    c.options[0] = 0;
    fake_clear_log();
    CHECK(sys_nas_test(&c, msg, sizeof msg) == 0 && strstr(msg, "written"), "test: %s", msg);
    CHECK(fake_ran("umount") && !fake_exists(SYS_NAS_TEST_DIR), "unmounted and removed after the test");
    CHECK(!strstr(msg, "Pa55") && !strstr(fake_log, "Pa55,,word\n") , "no password in the message");
    fake_mount_errno = EACCES;
    CHECK(sys_nas_test(&c, msg, sizeof msg) < 0 && strstr(msg, "refused") && !strstr(msg, "Pa55"),
          "wrong password: %s", msg);
    CHECK(!fake_exists(SYS_NAS_TEST_DIR), "no test directory left behind");
    fake_mount_errno = 0;
    config_nas_t none = { .type = "none" };
    CHECK(sys_nas_mount(&none, "/mnt/nas", msg, sizeof msg) < 0, "type none: %s", msg);

    fake_file("/proc/mounts", "//10.0.0.5/rec /mnt/nas cifs rw,relatime 0 0\n/dev/sda1 /mnt/sda1 vfat rw 0 0\n");
    CHECK(sys_nas_mounted("/mnt/nas") && !sys_nas_mounted("/mnt/sda1") && !sys_nas_mounted("/mnt"),
          "sys_nas_mounted");
    fake_umount_errno = EBUSY;
    CHECK(sys_nas_umount("/mnt/nas", msg, sizeof msg) < 0 && strstr(msg, "in use"), "busy share: %s", msg);
    fake_umount_errno = 0;
    fake_done();
}

void test_sys_disk(void)
{
    test_list();
    test_refusals();
    test_format();
    test_mount();
    test_nas();
}
