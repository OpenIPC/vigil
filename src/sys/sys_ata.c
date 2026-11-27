/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_ata.c — ATA PASS-THROUGH over SG_IO (sys_ata.h). */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <scsi/sg.h>

#include "sys_ata.h"

static ata_transport_t g_transport;

void ata_set_transport(ata_transport_t t) { g_transport = t; }

void ata_string(const uint16_t *w, int words, char *out, size_t cap)
{
    size_t n = 0;
    for (int i = 0; i < words && n + 2 < cap; ++i) {
        out[n++] = (char)(w[i] >> 8);
        out[n++] = (char)(w[i] & 0xff);
    }
    out[n] = 0;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)out[i] < 0x20 || (unsigned char)out[i] > 0x7e) out[i] = ' ';
    while (n && out[n - 1] == ' ') out[--n] = 0;
    size_t s = 0;
    while (out[s] == ' ') ++s;
    if (s) memmove(out, out + s, n - s + 1);
}

/* SAT's protocol field. */
static int sat_protocol(int proto) { return proto == ATA_PIO_IN ? 4 : proto == ATA_PIO_OUT ? 5 : 3; }

/* Byte 2: CK_COND, T_DIR, BYT_BLOK, T_LENGTH (the length is in the
 * sector count, in 512-byte blocks). */
static uint8_t sat_flags(const ata_tf_t *tf, int proto)
{
    uint8_t f = tf->want_regs ? 0x20 : 0;
    if (proto == ATA_PIO_IN) f |= 0x08 | 0x04 | 0x02;
    else if (proto == ATA_PIO_OUT) f |= 0x04 | 0x02;
    return f;
}

static int build16(uint8_t *c, const ata_tf_t *tf, int proto)
{
    memset(c, 0, 16);
    c[0] = 0x85;
    c[1] = (uint8_t)(sat_protocol(proto) << 1 | (tf->ext ? 1 : 0));
    c[2] = sat_flags(tf, proto);
    uint64_t l = tf->lba;
    if (tf->ext) {
        c[3] = (uint8_t)(tf->features >> 8);
        c[5] = (uint8_t)(tf->count >> 8);
        c[7] = (uint8_t)(l >> 24);
        c[9] = (uint8_t)(l >> 32);
        c[11] = (uint8_t)(l >> 40);
        c[13] = tf->device;
    } else {
        c[13] = (uint8_t)(tf->device | ((l >> 24) & 0x0f));
    }
    c[4] = (uint8_t)tf->features;
    c[6] = (uint8_t)tf->count;
    c[8] = (uint8_t)l;
    c[10] = (uint8_t)(l >> 8);
    c[12] = (uint8_t)(l >> 16);
    c[14] = tf->command;
    return 16;
}

static int build12(uint8_t *c, const ata_tf_t *tf, int proto)
{
    memset(c, 0, 12);
    c[0] = 0xA1;
    c[1] = (uint8_t)(sat_protocol(proto) << 1);
    c[2] = sat_flags(tf, proto);
    c[3] = (uint8_t)tf->features;
    c[4] = (uint8_t)tf->count;
    c[5] = (uint8_t)tf->lba;
    c[6] = (uint8_t)(tf->lba >> 8);
    c[7] = (uint8_t)(tf->lba >> 16);
    c[8] = (uint8_t)(tf->device | ((tf->lba >> 24) & 0x0f));
    c[9] = tf->command;
    return 12;
}

/* The ATA registers from the sense data: the ATA Status Return
 * descriptor (09h) of descriptor-format sense, which libata sends, or
 * the fixed format's "ATA PASS-THROUGH INFORMATION AVAILABLE" (00h/1Dh)
 * some bridges send. Returns the sense key, -1 without sense. */
static int parse_sense(const uint8_t *s, int len, ata_tf_t *tf, int *asc)
{
    *asc = 0;
    if (len < 8) return -1;
    int code = s[0] & 0x7f, key;
    if (code == 0x72 || code == 0x73) {
        key = s[1] & 0x0f;
        *asc = s[2];
        int end = 8 + s[7];
        if (end > len) end = len;
        for (int i = 8; i + 1 < end; i += 2 + s[i + 1]) {
            const uint8_t *d = s + i;
            if (d[0] != 0x09 || i + 14 > end) continue;
            tf->have_regs = 1;
            tf->error = d[3];
            tf->out_count = (uint16_t)(d[5] | ((d[2] & 1) ? d[4] << 8 : 0));
            tf->out_lba = (uint64_t)d[7] | (uint64_t)d[9] << 8 | (uint64_t)d[11] << 16;
            if (d[2] & 1) tf->out_lba |= (uint64_t)d[6] << 24 | (uint64_t)d[8] << 32 | (uint64_t)d[10] << 40;
            tf->status = d[13];
            break;
        }
        return key;
    }
    if (code == 0x70 || code == 0x71) {
        if (len < 14) return -1;
        key = s[2] & 0x0f;
        *asc = s[12];
        if (s[12] == 0x00 && s[13] == 0x1d) {
            tf->have_regs = 1;
            tf->error = s[3];
            tf->status = s[4];
            tf->out_count = s[6];
            tf->out_lba = (uint64_t)s[9] | (uint64_t)s[10] << 8 | (uint64_t)s[11] << 16;
        }
        return key;
    }
    return -1;
}

static int sg_once(int fd, ata_tf_t *tf, int proto, void *buf, size_t len, int timeout_ms, int cdb16, char *why,
                   size_t cap)
{
    uint8_t cdb[16], sense[64];
    memset(sense, 0, sizeof sense);
    sg_io_hdr_t io;
    memset(&io, 0, sizeof io);
    io.interface_id = 'S';
    io.cmd_len = (unsigned char)(cdb16 ? build16(cdb, tf, proto) : build12(cdb, tf, proto));
    io.cmdp = cdb;
    io.dxfer_direction = proto == ATA_PIO_IN ? SG_DXFER_FROM_DEV : proto == ATA_PIO_OUT ? SG_DXFER_TO_DEV
                                                                                        : SG_DXFER_NONE;
    io.dxfer_len = (unsigned)len;
    io.dxferp = len ? buf : NULL;
    io.mx_sb_len = sizeof sense;
    io.sbp = sense;
    io.timeout = (unsigned)timeout_ms;
    tf->have_regs = 0;
    if (ioctl(fd, SG_IO, &io) < 0) {
        int e = errno;
        snprintf(why, cap, "SG_IO: %s", strerror(e));
        return e == ENOTTY || e == EINVAL || e == ENOSYS || e == EOPNOTSUPP ? ATA_EUNSUP : ATA_EIO;
    }
    if (io.host_status) {
        snprintf(why, cap, "the command did not complete (host status %d%s)", io.host_status,
                 io.host_status == 3 ? ": timed out" : "");
        return ATA_EIO;
    }
    int asc = 0;
    int key = io.sb_len_wr ? parse_sense(sense, io.sb_len_wr, tf, &asc) : -1;
    if (tf->have_regs) {
        if (tf->status & (ATA_ST_ERR | ATA_ST_DF)) {
            snprintf(why, cap, "the drive refused it (status 0x%02x, error 0x%02x)", tf->status, tf->error);
            return ATA_EDEV;
        }
        return ATA_OK;
    }
    if (key < 0) {
        int drv = io.driver_status & 0x0f;
        if ((io.status & 0x7e) == 0 && (drv == 0 || drv == 8)) return ATA_OK;
        snprintf(why, cap, "SCSI status 0x%02x, driver status 0x%02x", io.status, io.driver_status);
        return ATA_EIO;
    }
    switch (key) {
    case 0x0:
    case 0x1:
        return ATA_OK;
    case 0x5:
        snprintf(why, cap, "the device does not pass ATA commands (ILLEGAL REQUEST, ASC 0x%02x)", asc);
        return ATA_EUNSUP;
    case 0x3:
        snprintf(why, cap, "medium error (ASC 0x%02x)", asc);
        return ATA_EDEV;
    case 0x4:
        snprintf(why, cap, "hardware error (ASC 0x%02x)", asc);
        return ATA_EDEV;
    case 0xb:
        snprintf(why, cap, "the drive aborted the command (ASC 0x%02x)", asc);
        return ATA_EDEV;
    default:
        snprintf(why, cap, "sense key 0x%x, ASC 0x%02x", key, asc);
        return ATA_EIO;
    }
}

int ata_cmd(int fd, ata_tf_t *tf, int proto, void *buf, size_t len, int timeout_ms, char *why, size_t cap)
{
    why[0] = 0;
    if (g_transport) {
        int rc = g_transport(fd, tf, proto, buf, len, timeout_ms);
        if (rc && !why[0]) snprintf(why, cap, "%s", rc == ATA_EUNSUP ? "no ATA behind it" : "the command failed");
        return rc;
    }
    int rc = sg_once(fd, tf, proto, buf, len, timeout_ms, 1, why, cap);
    /* Older USB bridges know only the 12-byte command; it has no room
     * for a 48-bit address. */
    if (rc == ATA_EUNSUP && !tf->ext) {
        char w2[160];
        int rc12 = sg_once(fd, tf, proto, buf, len, timeout_ms, 0, w2, sizeof w2);
        if (rc12 != ATA_EUNSUP) {
            snprintf(why, cap, "%s", w2);
            return rc12;
        }
    }
    return rc;
}
