/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_smart.c — identity and SMART health of a disk (sys_smart.h). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sys.h"
#include "sys_ata.h"
#include "sys_smart.h"

#define ATA_IDENTIFY     0xEC
#define ATA_SMART        0xB0
#define SMART_READ_DATA  0xD0
#define SMART_READ_THR   0xD1
#define SMART_EXEC       0xD4
#define SMART_READ_LOG   0xD5
#define SMART_STATUS     0xDA
#define SMART_LBA        0xC24F00u  /* LBA mid 4Fh, high C2h: the SMART signature */
#define LOG_SELFTEST     0x06
#define CMD_TIMEOUT_MS   15000      /* a spun-down disk spins up first */

static uint16_t le16(const uint8_t *b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t le32(const uint8_t *b) { return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24; }

/* ---- names --------------------------------------------------------------------- */

static const struct { uint8_t id; const char *hdd, *ssd; } NAMES[] = {
    { 1, "Raw read error rate", NULL },
    { 2, "Throughput performance", NULL },
    { 3, "Spin-up time", NULL },
    { 4, "Start/stop count", NULL },
    { 5, "Reallocated sectors", "Reallocated blocks" },
    { 7, "Seek error rate", NULL },
    { 8, "Seek time performance", NULL },
    { 9, "Power-on hours", NULL },
    { 10, "Spin retry count", NULL },
    { 11, "Calibration retries", NULL },
    { 12, "Power cycles", NULL },
    { 13, "Soft read error rate", NULL },
    { 22, "Helium level", NULL },
    { 170, "Available reserved space", NULL },
    { 171, "Program fail count", NULL },
    { 172, "Erase fail count", NULL },
    { 173, "Wear leveling count", NULL },
    { 174, "Unexpected power loss", NULL },
    { 175, "Program fail count (chip)", NULL },
    { 177, "Wear leveling count", NULL },
    { 179, "Used reserved blocks", NULL },
    { 180, "Unused reserved blocks", NULL },
    { 181, "Program fail count", NULL },
    { 182, "Erase fail count", NULL },
    { 183, "Runtime bad blocks", NULL },
    { 184, "End-to-end errors", NULL },
    { 187, "Reported uncorrectable", NULL },
    { 188, "Command timeouts", NULL },
    { 189, "High fly writes", NULL },
    { 190, "Airflow temperature", NULL },
    { 191, "G-sense error rate", NULL },
    { 192, "Power-off retracts", "Unsafe shutdowns" },
    { 193, "Load/unload cycles", NULL },
    { 194, "Temperature", NULL },
    { 195, "Hardware ECC recovered", NULL },
    { 196, "Reallocation events", NULL },
    { 197, "Current pending sectors", NULL },
    { 198, "Offline uncorrectable", NULL },
    { 199, "Interface CRC errors", NULL },
    { 200, "Multi-zone error rate", "Write error rate" },
    { 202, "Data address mark errors", "Percent lifetime remaining" },
    { 206, "Flying height", NULL },
    { 220, "Disk shift", NULL },
    { 222, "Loaded hours", NULL },
    { 223, "Load retries", NULL },
    { 224, "Load friction", NULL },
    { 225, "Load cycles", "Host writes (32 MiB)" },
    { 226, "Load-in time", NULL },
    { 230, "Head amplitude", "Drive life protection" },
    { 231, "Life left", "SSD life left" },
    { 232, "Endurance remaining", "Available reserved space" },
    { 233, "Media wearout indicator", NULL },
    { 234, "Average erase count", NULL },
    { 235, "Good block count", NULL },
    { 240, "Head flying hours", NULL },
    { 241, "Total LBAs written", NULL },
    { 242, "Total LBAs read", NULL },
    { 250, "Read error retry rate", NULL },
    { 254, "Free fall events", NULL },
};

const char *smart_attr_name(uint8_t id, int ssd)
{
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; ++i)
        if (NAMES[i].id == id) return ssd && NAMES[i].ssd ? NAMES[i].ssd : NAMES[i].hdd;
    return "Vendor specific";
}

const char *smart_test_type_text(uint8_t type)
{
    switch (type) {
    case 0x00: return "Offline";
    case 0x01: return "Short";
    case 0x02: return "Extended";
    case 0x03: return "Conveyance";
    case 0x04: return "Selective";
    case 0x81: return "Short (captive)";
    case 0x82: return "Extended (captive)";
    case 0x83: return "Conveyance (captive)";
    case 0x84: return "Selective (captive)";
    default:   return "Vendor";
    }
}

void smart_test_status_text(uint8_t status, char *out, size_t cap)
{
    static const char *const S[] = {
        "Completed without error", "Aborted by host", "Interrupted by a reset", "Failed (fatal error)",
        "Failed: unknown element", "Failed: electrical element", "Failed: servo/seek element",
        "Failed: read element", "Failed: handling damage",
    };
    int st = status >> 4;
    if (st == 15) snprintf(out, cap, "In progress (%d%% left)", (status & 0x0f) * 10);
    else if (st < (int)(sizeof S / sizeof S[0])) snprintf(out, cap, "%s", S[st]);
    else snprintf(out, cap, "Unknown (%d)", st);
}

const char *smart_verdict_name(int v)
{
    return v == SMART_GOOD ? "good" : v == SMART_WARN ? "warning" : v == SMART_FAIL ? "failing" : "unknown";
}

static int test_failed(uint8_t status)
{
    int st = status >> 4;
    return st >= 3 && st <= 8;
}

/* ---- parsing --------------------------------------------------------------------- */

int smart_parse_identify(const uint8_t id[512], smart_info_t *s)
{
    uint16_t w[256];
    int any = 0;
    for (int i = 0; i < 256; ++i) {
        w[i] = le16(id + 2 * i);
        any |= w[i] != 0;
    }
    if (!any) return -1;
    ata_string(w + 10, 10, s->serial, sizeof s->serial);
    ata_string(w + 23, 4, s->firmware, sizeof s->firmware);
    ata_string(w + 27, 20, s->model, sizeof s->model);
    if (!s->model[0] && !s->serial[0]) return -1;

    /* A word pair or set is valid when bit 14 is set and bit 15 clear. */
    int v83 = (w[83] & 0xC000) == 0x4000, v84 = (w[84] & 0xC000) == 0x4000;
    s->lba48 = v83 && (w[83] & (1u << 10));
    if (s->lba48)
        s->sectors = (uint64_t)w[100] | (uint64_t)w[101] << 16 | (uint64_t)w[102] << 32 | (uint64_t)w[103] << 48;
    if (!s->sectors) s->sectors = (uint64_t)w[60] | (uint64_t)w[61] << 16;
    s->sector_size = s->phys_sector_size = 512;
    if ((w[106] & 0xC000) == 0x4000) {
        if (w[106] & (1u << 12)) {
            uint32_t words = w[117] | (uint32_t)w[118] << 16;
            if (words >= 256 && words <= 32768) s->sector_size = words * 2;
        }
        s->phys_sector_size = s->sector_size;
        if (w[106] & (1u << 13)) s->phys_sector_size = s->sector_size << (w[106] & 0x0f);
    }
    if (w[217] == 1) s->rpm = 1;
    else if (w[217] >= 0x0401 && w[217] <= 0xfffe) s->rpm = w[217];
    if (w[76] && w[76] != 0xffff) {
        s->sata_max = w[76] & (1u << 3) ? 3 : w[76] & (1u << 2) ? 2 : w[76] & (1u << 1) ? 1 : 0;
        int cur = (w[77] >> 1) & 7;
        s->sata_cur = w[77] != 0xffff && cur >= 1 && cur <= 3 ? cur : 0;
    }
    s->smart_supported = (w[82] & 1) && w[82] != 0xffff;
    s->smart_enabled = s->smart_supported && (w[85] & 1);
    s->selftest_supported = s->smart_supported && v84 && (w[84] & 2);
    return 0;
}

static void decode_raw(smart_attr_t *a, int ssd, int *temp, long *hours)
{
    const uint8_t *r = a->rawb;
    switch (a->id) {
    case 9: {
        /* Some drives keep minutes or milliseconds in the bytes above. */
        unsigned long h = le32(r);
        snprintf(a->raw_text, sizeof a->raw_text, "%lu h", h);
        *hours = (long)h;
        return;
    }
    case 3:
        snprintf(a->raw_text, sizeof a->raw_text, "%u ms", le16(r));
        return;
    case 190:
    case 194: {
        int t = r[0], lo = r[2], hi = r[4];
        if (t > 0 && t < 128) {
            if (lo && hi && lo <= t && t <= hi && hi < 128)
                snprintf(a->raw_text, sizeof a->raw_text, "%d °C (min %d, max %d)", t, lo, hi);
            else snprintf(a->raw_text, sizeof a->raw_text, "%d °C", t);
            /* 194 is the drive's own; 190 only when there is no 194. */
            if (a->id == 194 || *temp < 0) *temp = t;
            return;
        }
        break;
    }
    case 188:
        /* Seagate keeps three 16-bit counters here. */
        if (a->raw > 0xffff) {
            snprintf(a->raw_text, sizeof a->raw_text, "%u %u %u", le16(r + 4), le16(r + 2), le16(r));
            return;
        }
        break;
    default:
        break;
    }
    (void)ssd;
    snprintf(a->raw_text, sizeof a->raw_text, "%llu", (unsigned long long)a->raw);
}

int smart_parse_data(const uint8_t data[512], const uint8_t *thresh, smart_info_t *s)
{
    s->n_attrs = 0;
    s->temp = -1;
    s->hours = -1;
    int ssd = s->rpm == 1;
    for (int i = 0; i < SMART_ATTRS; ++i) {
        const uint8_t *e = data + 2 + 12 * i;
        if (!e[0]) continue;
        smart_attr_t *a = &s->attrs[s->n_attrs++];
        memset(a, 0, sizeof *a);
        a->id = e[0];
        a->flags = le16(e + 1);
        a->value = e[3];
        a->worst = e[4];
        memcpy(a->rawb, e + 5, 6);
        for (int k = 5; k >= 0; --k) a->raw = a->raw << 8 | a->rawb[k];
        if (thresh)
            for (int k = 0; k < SMART_ATTRS; ++k)
                if (thresh[2 + 12 * k] == a->id) a->thresh = thresh[2 + 12 * k + 1];
        snprintf(a->name, sizeof a->name, "%s", smart_attr_name(a->id, ssd));
        decode_raw(a, ssd, &s->temp, &s->hours);
    }
    s->has_data = s->n_attrs > 0;
    s->has_thresh = thresh != NULL;
    /* Offline data collection capability, bit 4: self-tests (what
     * smartctl goes by; some drives, QEMU's among them, leave IDENTIFY
     * word 84 bit 1 clear). */
    if (data[367] & 0x10) s->selftest_supported = 1;
    s->test_status = data[363] >> 4;
    s->test_remaining = s->test_status == 15 ? (data[363] & 0x0f) * 10 : 0;
    s->short_min = data[372];
    s->ext_min = data[373] == 0xff ? le16(data + 375) : data[373];
    return s->n_attrs ? 0 : -1;
}

int smart_parse_selftest_log(const uint8_t log[512], smart_info_t *s)
{
    s->n_tests = 0;
    int idx = log[508];
    if (idx < 1 || idx > SMART_TESTS) return 0;
    for (int k = 0; k < SMART_TESTS; ++k) {
        int i = ((idx - 1 - k) % SMART_TESTS + SMART_TESTS) % SMART_TESTS;
        const uint8_t *e = log + 2 + 24 * i;
        int empty = 1;
        for (int b = 0; b < 24; ++b) empty &= e[b] == 0;
        if (empty) break;
        smart_test_t *t = &s->tests[s->n_tests++];
        t->type = e[0];
        t->status = e[1];
        t->hours = le16(e + 2);
        t->lba = le32(e + 5);
    }
    return s->n_tests;
}

/* ---- the verdict ----------------------------------------------------------------- */

/* The counts that only grow on a sick disk, and what they mean. */
static const struct { uint8_t id; const char *what, *note; } CRITICAL[] = {
    { 5, "reallocated sectors", "Sectors moved to spares: the surface is wearing" },
    { 196, "reallocation events", "Times the drive moved sectors to spares" },
    { 197, "pending sectors", "Unreadable sectors waiting to be remapped: data on them may be lost" },
    { 198, "offline uncorrectable sectors", "Sectors the drive's own scan could not read" },
    { 187, "reported uncorrectable errors", "Reads the drive could not correct" },
    { 188, "command timeouts", "Commands that timed out: power, cable or the drive" },
    { 10, "spin retries", "The motor needed more than one try to spin up" },
    { 184, "end-to-end errors", "Data corrupted inside the drive, between its cache and the host" },
    { 199, "interface CRC errors", "Usually the cable or the connection, not the disk" },
};

static int critical_index(uint8_t id)
{
    for (size_t i = 0; i < sizeof CRITICAL / sizeof CRITICAL[0]; ++i)
        if (CRITICAL[i].id == id) return (int)i;
    return -1;
}

/* An SSD's life left, by a normalised value. */
static int wear_attr(uint8_t id) { return id == 231 || id == 233 || id == 202 || id == 177; }

/* The count that matters of a critical attribute (188's three counters
 * are summed). */
static unsigned long long count_of(const smart_attr_t *a)
{
    if (a->id == 188 && a->raw > 0xffff)
        return (unsigned long long)le16(a->rawb) + le16(a->rawb + 2) + le16(a->rawb + 4);
    return a->raw;
}

static void add_reason(char *r, size_t cap, int *n, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void add_reason(char *r, size_t cap, int *n, const char *fmt, ...)
{
    size_t l = strlen(r);
    if (*n == 3) {
        if (l + 6 < cap) snprintf(r + l, cap - l, ", ...");
        ++*n;
        return;
    }
    if (*n > 3) return;
    if (*n && l + 2 < cap) {
        snprintf(r + l, cap - l, ", ");
        l += 2;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r + l, cap - l, fmt, ap);
    va_end(ap);
    ++*n;
}

void smart_evaluate(smart_info_t *s)
{
    s->reason[0] = 0;
    if (!s->available) {
        s->verdict = SMART_UNKNOWN;
        snprintf(s->reason, sizeof s->reason, "SMART not available: %s", s->why[0] ? s->why : "no answer");
        return;
    }
    int ssd = s->rpm == 1, hot = ssd ? SMART_HOT_SSD : SMART_HOT_HDD;
    const smart_attr_t *first_fail = NULL;
    int n_fail = 0;
    for (int i = 0; i < s->n_attrs; ++i) {
        smart_attr_t *a = &s->attrs[i];
        a->level = SMART_GOOD;
        a->note[0] = 0;
        int ci = critical_index(a->id);
        a->critical = ci >= 0;
        /* A threshold of 0 never fails; FEh/FFh are "always failing" and
         * "always passing" placeholders some drives use. */
        int has_thr = s->has_thresh && a->thresh > 0 && a->thresh < 0xfe;
        if (has_thr && a->value <= a->thresh) {
            a->level = SMART_FAIL;
            snprintf(a->note, sizeof a->note, "At or below its threshold (%u <= %u)%s", a->value, a->thresh,
                     a->flags & 1 ? ": the drive may fail soon" : "");
            if (!first_fail) first_fail = a;
            ++n_fail;
            continue;
        }
        if (has_thr && a->worst <= a->thresh) {
            a->level = SMART_WARN;
            snprintf(a->note, sizeof a->note, "Was at its threshold once (worst %u)", a->worst);
        }
        if (ci >= 0 && count_of(a)) {
            a->level = SMART_WARN;
            snprintf(a->note, sizeof a->note, "%s", CRITICAL[ci].note);
        }
        if ((a->id == 194 || a->id == 190) && s->temp > hot && a->rawb[0] == s->temp) {
            a->level = SMART_WARN;
            snprintf(a->note, sizeof a->note, "Hotter than %d °C: check the airflow", hot);
        }
        if (ssd && wear_attr(a->id) && a->value <= 10) {
            a->level = SMART_WARN;
            snprintf(a->note, sizeof a->note, "Worn out: %u%% of its life left", a->value);
        }
    }

    const smart_test_t *last = s->n_tests ? &s->tests[0] : NULL;
    if (s->health == 0) {
        s->verdict = SMART_FAIL;
        snprintf(s->reason, sizeof s->reason, "The drive reports that it is failing (SMART status): copy what "
                                              "matters off it and replace it");
        return;
    }
    if (first_fail) {
        s->verdict = SMART_FAIL;
        snprintf(s->reason, sizeof s->reason, "%s is at or below its threshold%s", first_fail->name,
                 n_fail > 1 ? " (and more)" : "");
        return;
    }
    if (last && test_failed(last->status)) {
        s->verdict = SMART_FAIL;
        snprintf(s->reason, sizeof s->reason, "The last self-test failed at LBA %lu", (unsigned long)last->lba);
        return;
    }
    if (!s->smart_supported || !s->has_data) {
        s->verdict = SMART_UNKNOWN;
        snprintf(s->reason, sizeof s->reason, "%s",
                 !s->smart_supported ? "The drive does not support SMART"
                 : !s->smart_enabled ? "SMART is turned off on this drive"
                                     : "The drive gave no SMART data");
        return;
    }

    int nw = 0, only_crc = 1;
    /* The counts in the order that matters (remapped and pending
     * sectors first), then the rest in the drive's order. */
    for (size_t c = 0; c < sizeof CRITICAL / sizeof CRITICAL[0]; ++c)
        for (int i = 0; i < s->n_attrs; ++i) {
            const smart_attr_t *a = &s->attrs[i];
            if (a->id != CRITICAL[c].id || a->level != SMART_WARN || !count_of(a)) continue;
            if (a->id != 199) only_crc = 0;
            add_reason(s->reason, sizeof s->reason, &nw, "%llu %s", count_of(a), CRITICAL[c].what);
        }
    for (int i = 0; i < s->n_attrs; ++i) {
        const smart_attr_t *a = &s->attrs[i];
        if (a->level != SMART_WARN || (critical_index(a->id) >= 0 && count_of(a))) continue;
        only_crc = 0;
        if ((a->id == 194 || a->id == 190) && s->temp > hot)
            add_reason(s->reason, sizeof s->reason, &nw, "too hot (%d °C)", s->temp);
        else if (ssd && wear_attr(a->id) && a->value <= 10)
            add_reason(s->reason, sizeof s->reason, &nw, "worn out (%u%% left)", a->value);
        else
            add_reason(s->reason, sizeof s->reason, &nw, "%s was at its threshold once", a->name);
    }
    if (nw) {
        s->verdict = SMART_WARN;
        if (only_crc) {
            size_t l = strlen(s->reason);
            snprintf(s->reason + l, sizeof s->reason - l, ": usually the cable or the connection, not the disk");
        } else {
            s->reason[0] = (char)(s->reason[0] >= 'a' && s->reason[0] <= 'z' ? s->reason[0] - 32 : s->reason[0]);
        }
        return;
    }
    s->verdict = SMART_GOOD;
    snprintf(s->reason, sizeof s->reason, "No reallocated, pending or uncorrectable sectors%s",
             s->health == 1 ? "; the drive's own check passed" : "");
}

/* ---- the drive --------------------------------------------------------------------- */

static void transport_of(const char *name, char *out, size_t cap)
{
    char rel[128], p[512], real[PATH_MAX];
    snprintf(rel, sizeof rel, "/sys/block/%s/device", name);
    out[0] = 0;
    if (!realpath(sys_path(p, sizeof p, rel), real)) return;
    if (strstr(real, "/usb")) snprintf(out, cap, "USB");
    else if (strstr(real, "/ata")) snprintf(out, cap, "SATA");
    else if (strstr(real, "/mmc")) snprintf(out, cap, "memory card");
}

static int smart_cmd(int fd, uint8_t feature, uint8_t lba_low, int proto, uint8_t *buf, ata_tf_t *tf, char *why,
                     size_t cap)
{
    memset(tf, 0, sizeof *tf);
    tf->command = ATA_SMART;
    tf->features = feature;
    tf->count = proto == ATA_PIO_IN ? 1 : 0;
    tf->lba = SMART_LBA | lba_low;
    tf->want_regs = feature == SMART_STATUS;
    return ata_cmd(fd, tf, proto, buf, proto == ATA_PIO_IN ? 512 : 0, CMD_TIMEOUT_MS, why, cap);
}

static int open_dev(const char *name, char *err, size_t cap)
{
    if (!name || !*name || strchr(name, '/')) {
        snprintf(err, cap, "no such disk");
        return -1;
    }
    char rel[64], p[512];
    snprintf(rel, sizeof rel, "/dev/%s", name);
    int fd = open(sys_path(p, sizeof p, rel), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) snprintf(err, cap, "cannot open %s: %s", rel, strerror(errno));
    return fd;
}

int smart_read(const char *name, smart_info_t *s)
{
    memset(s, 0, sizeof *s);
    s->health = -1;
    s->temp = -1;
    s->hours = -1;
    transport_of(name, s->transport, sizeof s->transport);
    int fd = open_dev(name, s->why, sizeof s->why);
    if (fd < 0) {
        smart_evaluate(s);
        return -1;
    }
    uint8_t *buf = malloc(512 * 4);
    if (!buf) {
        close(fd);
        snprintf(s->why, sizeof s->why, "out of memory");
        smart_evaluate(s);
        return -1;
    }
    uint8_t *id = buf, *data = buf + 512, *thr = buf + 1024, *log = buf + 1536;
    memset(buf, 0, 512 * 4);
    char why[160];
    ata_tf_t tf = { .command = ATA_IDENTIFY, .count = 1 };
    int rc = ata_cmd(fd, &tf, ATA_PIO_IN, id, 512, CMD_TIMEOUT_MS, why, sizeof why);
    if (rc != ATA_OK || smart_parse_identify(id, s) < 0) {
        if (rc == ATA_EUNSUP || rc == ATA_OK)
            snprintf(s->why, sizeof s->why, "%s does not pass ATA commands (a USB stick, a memory card, or a USB "
                                            "enclosure without SAT)", name);
        else snprintf(s->why, sizeof s->why, "IDENTIFY failed: %.140s", why);
        free(buf);
        close(fd);
        smart_evaluate(s);
        return -1;
    }
    s->available = 1;
    if (!s->smart_supported) snprintf(s->why, sizeof s->why, "the drive does not support SMART");
    else if (!s->smart_enabled) snprintf(s->why, sizeof s->why, "SMART is turned off on the drive");
    if (s->smart_enabled) {
        if (smart_cmd(fd, SMART_READ_DATA, 0, ATA_PIO_IN, data, &tf, why, sizeof why) == ATA_OK) {
            int got_thr = smart_cmd(fd, SMART_READ_THR, 0, ATA_PIO_IN, thr, &tf, why, sizeof why) == ATA_OK;
            if (smart_parse_data(data, got_thr ? thr : NULL, s) < 0)
                snprintf(s->why, sizeof s->why, "the drive gave an empty attribute table");
        } else {
            snprintf(s->why, sizeof s->why, "SMART READ DATA failed: %.130s", why);
        }
        if (smart_cmd(fd, SMART_STATUS, 0, ATA_NONDATA, NULL, &tf, why, sizeof why) == ATA_OK && tf.have_regs) {
            unsigned mid = (unsigned)(tf.out_lba >> 8) & 0xff, high = (unsigned)(tf.out_lba >> 16) & 0xff;
            if (mid == 0x4f && high == 0xc2) s->health = 1;
            else if (mid == 0xf4 && high == 0x2c) s->health = 0;
        }
        if (s->selftest_supported &&
            smart_cmd(fd, SMART_READ_LOG, LOG_SELFTEST, ATA_PIO_IN, log, &tf, why, sizeof why) == ATA_OK)
            smart_parse_selftest_log(log, s);
    }
    free(buf);
    close(fd);
    smart_evaluate(s);
    return 0;
}

int smart_selftest(const char *name, int type, char *err, size_t cap)
{
    err[0] = 0;
    if (type < 0 || type > 2) {
        snprintf(err, cap, "no such self-test");
        return -1;
    }
    int fd = open_dev(name, err, cap);
    if (fd < 0) return -1;
    ata_tf_t tf;
    char why[160];
    int rc = smart_cmd(fd, SMART_EXEC, (uint8_t)(type == 0 ? 0x7f : type), ATA_NONDATA, NULL, &tf, why, sizeof why);
    close(fd);
    if (rc != ATA_OK) {
        snprintf(err, cap, "%s: %s", type ? "the self-test did not start" : "could not abort the self-test", why);
        return -1;
    }
    return 0;
}
