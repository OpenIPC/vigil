/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_ata.h — ATA commands to a disk behind Linux's SCSI layer.
 *
 * SMART (sys_smart.h) and the surface scan's Verify mode (sys_scan.h)
 * talk to the drive itself, not to its blocks: IDENTIFY DEVICE, SMART,
 * READ VERIFY SECTORS. Every disk the recorder sees is a SCSI disk to
 * the kernel (/dev/sdX): libata translates for a SATA disk, and a USB
 * enclosure's bridge does it in its firmware. Both pass an ATA command
 * through SCSI's ATA PASS-THROUGH (SAT: ATA PASS-THROUGH(16), opcode
 * 0x85, and the older (12), 0xA1, that some USB bridges know instead),
 * sent with the SG_IO ioctl. No smartctl: the firmware has no room for
 * smartmontools, and these few commands are all vigild needs.
 *
 * libata answers HDIO_DRIVE_CMD too, but by building the same ATA
 * PASS-THROUGH internally, and a USB disk not at all, so SG_IO is the
 * one way in.
 *
 * A memory card (mmcblk) or a USB stick has no ATA behind it: its
 * driver refuses SG_IO, or the stick answers ILLEGAL REQUEST, and the
 * caller says "not available".
 */
#ifndef SYS_ATA_H
#define SYS_ATA_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    /* in */
    uint8_t  command;
    uint16_t features, count;
    uint64_t lba;                   /* 28 or 48 bits (ext) */
    uint8_t  device;
    int      ext;                   /* a 48-bit command: only ATA PASS-THROUGH(16) */
    int      want_regs;             /* ask for the result registers (CK_COND) */
    /* out */
    int      have_regs;             /* the registers below came back */
    uint8_t  status, error;
    uint16_t out_count;
    uint64_t out_lba;
} ata_tf_t;

enum { ATA_NONDATA, ATA_PIO_IN, ATA_PIO_OUT };

enum {
    ATA_OK = 0,
    ATA_EDEV = -1,                  /* the drive answered with an error (ABRT, UNC, ...) */
    ATA_EUNSUP = -2,                /* nothing passes ATA commands to it (no SAT, not SCSI) */
    ATA_EIO = -3,                   /* the command did not complete: a timeout, a reset, a gone disk */
};

/* ATA status and error bits. */
#define ATA_ST_ERR  0x01
#define ATA_ST_DF   0x20
#define ATA_ER_ABRT 0x04
#define ATA_ER_UNC  0x40
#define ATA_ER_IDNF 0x10

/* Send `tf` to the drive behind `fd` (an open block device), moving
 * `len` bytes (a multiple of 512) in or out of `buf` per `proto`. ATA
 * PASS-THROUGH(16) first, (12) when the bridge does not know (16) and
 * the command fits. One of ATA_*; `why` says what went wrong. */
int ata_cmd(int fd, ata_tf_t *tf, int proto, void *buf, size_t len, int timeout_ms, char *why, size_t cap);

/* The transport, for the tests: a fake drive in place of SG_IO. NULL:
 * the real one. Same contract as ata_cmd. */
typedef int (*ata_transport_t)(int fd, ata_tf_t *tf, int proto, void *buf, size_t len, int timeout_ms);
void ata_set_transport(ata_transport_t t);

/* An ATA string (byte pairs swapped, blank-padded) of `words` words at
 * `w`, trimmed, into `out`. */
void ata_string(const uint16_t *w, int words, char *out, size_t cap);

#endif /* SYS_ATA_H */
