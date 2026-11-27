/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_sys_maint.c — Disk Maintenance on a fake system: SMART parsing,
 * decoding and the verdict from the 512-byte sectors drives answer with,
 * smart_read through a fake ATA transport, and the surface scan against
 * a plain file standing in for /dev/sdz, with errors and slow blocks put
 * in by its test hook (sys_smart.h, sys_scan.h, mgmt.h).
 *
 * The SMART tables are those of real drives as smartctl -A showed them
 * (a WD Purple, a Seagate Barracuda with bad sectors, a Samsung 860 EVO,
 * a WD Red whose cable went bad), packed back into the drives' format. */
#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "mgmt.h"
#include "sys.h"
#include "sys_ata.h"
#include "sys_fake.h"
#include "sys_scan.h"
#include "sys_smart.h"

void test_sys_smart(void);
void test_sys_scan(void);

/* ---- making the drives' sectors ------------------------------------------------------ */

typedef struct {
    uint8_t  id;
    uint16_t flags;
    uint8_t  value, worst, thresh;
    uint64_t raw;
} attr_t;

static void put_string(uint8_t *id, int word, int words, const char *s)
{
    char b[80];
    memset(b, ' ', sizeof b);
    memcpy(b, s, strlen(s));
    for (int i = 0; i < words; ++i) {
        id[2 * (word + i)] = (uint8_t)b[2 * i + 1];
        id[2 * (word + i) + 1] = (uint8_t)b[2 * i];
    }
}

static void put_word(uint8_t *id, int word, uint16_t v)
{
    id[2 * word] = (uint8_t)v;
    id[2 * word + 1] = (uint8_t)(v >> 8);
}

/* IDENTIFY DEVICE of a SATA drive: `rpm` 1 for an SSD; 4K physical
 * sectors when `phys4k`; SATA 3 (6 Gb/s) negotiated at `gen`. */
static void identify(uint8_t *id, const char *model, const char *serial, const char *fw, uint64_t sectors,
                     uint16_t rpm, int phys4k, int gen, int smart_on)
{
    memset(id, 0, 512);
    put_word(id, 0, 0x0040);
    put_string(id, 10, 10, serial);
    put_string(id, 23, 4, fw);
    put_string(id, 27, 20, model);
    put_word(id, 49, 0x2f00);
    put_word(id, 60, (uint16_t)(sectors > 0x0fffffff ? 0xffff : sectors));
    put_word(id, 61, (uint16_t)(sectors > 0x0fffffff ? 0x0fff : sectors >> 16));
    put_word(id, 76, 0x0e | 0x0100 | 0x0400 | 0x1000);         /* Gen1-3, NCQ */
    put_word(id, 77, (uint16_t)(gen << 1));
    put_word(id, 82, 0x746b);                                  /* SMART supported (bit 0) */
    put_word(id, 83, 0x7f61);                                  /* 48-bit (bit 10) */
    put_word(id, 84, 0x6163);                                  /* SMART self-test (bit 1) */
    put_word(id, 85, smart_on ? 0x7469 : 0x7468);
    put_word(id, 87, 0x6163);
    for (int i = 0; i < 4; ++i) put_word(id, 100 + i, (uint16_t)(sectors >> (16 * i)));
    put_word(id, 106, phys4k ? 0x6003 : 0x4000);
    put_word(id, 217, rpm);
    put_word(id, 255, 0x00a5);
    uint8_t sum = 0;
    for (int i = 0; i < 511; ++i) sum += id[i];
    id[511] = (uint8_t)-sum;
}

static void smart_data(uint8_t *data, uint8_t *thr, const attr_t *a, int n, uint8_t selftest_status)
{
    memset(data, 0, 512);
    memset(thr, 0, 512);
    data[0] = thr[0] = 0x10;
    for (int i = 0; i < n; ++i) {
        uint8_t *e = data + 2 + 12 * i, *t = thr + 2 + 12 * i;
        e[0] = a[i].id;
        e[1] = (uint8_t)a[i].flags;
        e[2] = (uint8_t)(a[i].flags >> 8);
        e[3] = a[i].value;
        e[4] = a[i].worst;
        for (int b = 0; b < 6; ++b) e[5 + b] = (uint8_t)(a[i].raw >> (8 * b));
        t[0] = a[i].id;
        t[1] = a[i].thresh;
    }
    data[362] = 0x82;
    data[367] = 0x5b;                                           /* self-tests among them */
    data[363] = selftest_status;
    data[372] = 2;
    data[373] = 0xff;
    data[375] = 0x9a;                                           /* 410 minutes */
    data[376] = 0x01;
    uint8_t s1 = 0, s2 = 0;
    for (int i = 0; i < 511; ++i) s1 += data[i], s2 += thr[i];
    data[511] = (uint8_t)-s1;
    thr[511] = (uint8_t)-s2;
}

/* WD Purple WD10PURZ, 16234 hours, nothing wrong. */
static const attr_t WD_PURPLE[] = {
    { 1, 0x002f, 200, 200, 51, 0 },       { 3, 0x0027, 172, 169, 21, 2383 },
    { 4, 0x0032, 100, 100, 0, 312 },      { 5, 0x0033, 200, 200, 140, 0 },
    { 7, 0x002e, 200, 200, 0, 0 },        { 9, 0x0032, 78, 78, 0, 16234 },
    { 10, 0x0032, 100, 100, 0, 0 },       { 11, 0x0032, 100, 100, 0, 0 },
    { 12, 0x0032, 100, 100, 0, 310 },     { 192, 0x0032, 200, 200, 0, 41 },
    { 193, 0x0032, 200, 200, 0, 1043 },   { 194, 0x0022, 113, 102, 0, 0x002d00140025ull },
    { 196, 0x0032, 200, 200, 0, 0 },      { 197, 0x0032, 200, 200, 0, 0 },
    { 198, 0x0030, 100, 253, 0, 0 },      { 199, 0x0032, 200, 200, 0, 0 },
    { 200, 0x0008, 100, 253, 0, 0 },
};

/* Seagate ST1000DM003 with remapped and pending sectors, and three
 * 16-bit command timeout counters in 188. */
static const attr_t ST_BAD[] = {
    { 1, 0x000f, 112, 99, 6, 46031352 },  { 3, 0x0003, 96, 95, 0, 0 },
    { 4, 0x0032, 97, 97, 20, 3640 },      { 5, 0x0033, 89, 89, 10, 1704 },
    { 7, 0x000f, 78, 60, 30, 72466921 },  { 9, 0x0032, 63, 63, 0, 32876 },
    { 10, 0x0013, 100, 100, 97, 0 },      { 12, 0x0032, 97, 97, 20, 3608 },
    { 183, 0x0032, 100, 100, 0, 0 },      { 184, 0x0032, 100, 100, 99, 0 },
    { 187, 0x0032, 1, 1, 0, 214 },        { 188, 0x0032, 100, 97, 0, 0x000000030003ull },
    { 189, 0x003a, 100, 100, 0, 0 },      { 190, 0x0022, 64, 52, 45, 0x2c160024ull },
    { 194, 0x0022, 36, 48, 0, 0x000000120024ull },  { 195, 0x001a, 39, 16, 0, 46031352 },
    { 197, 0x0012, 100, 100, 0, 8 },      { 198, 0x0010, 100, 100, 0, 8 },
    { 199, 0x003e, 200, 200, 0, 0 },      { 240, 0x0000, 100, 253, 0, 32760 },
};

/* Samsung SSD 860 EVO 250GB. */
static const attr_t EVO[] = {
    { 5, 0x0033, 100, 100, 10, 0 },       { 9, 0x0032, 97, 97, 0, 12045 },
    { 12, 0x0032, 99, 99, 0, 112 },       { 177, 0x0013, 95, 95, 0, 41 },
    { 179, 0x0013, 100, 100, 10, 0 },     { 181, 0x0032, 100, 100, 10, 0 },
    { 182, 0x0032, 100, 100, 10, 0 },     { 183, 0x0013, 100, 100, 10, 0 },
    { 187, 0x0032, 100, 100, 0, 0 },      { 190, 0x0032, 67, 52, 0, 33 },
    { 195, 0x001a, 200, 200, 0, 0 },      { 199, 0x003e, 100, 100, 0, 0 },
    { 235, 0x0012, 99, 99, 0, 87 },       { 241, 0x0032, 99, 99, 0, 23541789012ull },
};

/* WD Red WD40EFRX, healthy but for a bad SATA cable. */
static const attr_t WD_RED_CRC[] = {
    { 1, 0x002f, 200, 200, 51, 0 },       { 3, 0x0027, 181, 177, 21, 7933 },
    { 5, 0x0033, 200, 200, 140, 0 },      { 9, 0x0032, 51, 51, 0, 36011 },
    { 194, 0x0022, 118, 104, 0, 34 },     { 196, 0x0032, 200, 200, 0, 0 },
    { 197, 0x0032, 200, 200, 0, 0 },      { 198, 0x0030, 100, 253, 0, 0 },
    { 199, 0x0032, 200, 200, 0, 1317 },
};

/* A failing one: the reallocated sectors at their threshold, too hot. */
static const attr_t DYING[] = {
    { 1, 0x002f, 199, 51, 51, 2451 },     { 5, 0x0033, 1, 1, 36, 4095 },
    { 9, 0x0032, 20, 20, 0, 70321 },      { 194, 0x0022, 99, 90, 0, 61 },
    { 197, 0x0032, 200, 200, 0, 312 },
};

static const smart_attr_t *attr(const smart_info_t *s, int id)
{
    for (int i = 0; i < s->n_attrs; ++i)
        if (s->attrs[i].id == id) return &s->attrs[i];
    return NULL;
}

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void parse(smart_info_t *s, const char *model, const char *serial, uint16_t rpm, const attr_t *a, int n)
{
    uint8_t id[512], data[512], thr[512];
    memset(s, 0, sizeof *s);
    s->health = -1;
    identify(id, model, serial, "01.01A01", 1953525168ull, rpm, 1, 3, 1);
    s->available = smart_parse_identify(id, s) == 0;
    smart_data(data, thr, a, n, 0);
    smart_parse_data(data, thr, s);
    s->health = 1;
    smart_evaluate(s);
}

static void test_parse(void)
{
    smart_info_t *s = calloc(1, sizeof *s);
    uint8_t id[512];
    identify(id, "WDC WD10PURZ-85U8XY0", "WD-WCC4J1234567", "01.01A01", 1953525168ull, 5400, 1, 3, 1);
    CHECK(smart_parse_identify(id, s) == 0, "IDENTIFY parsed");
    CHECK(!strcmp(s->model, "WDC WD10PURZ-85U8XY0") && !strcmp(s->serial, "WD-WCC4J1234567") &&
          !strcmp(s->firmware, "01.01A01"), "strings: '%s' '%s' '%s'", s->model, s->serial, s->firmware);
    CHECK(s->sectors == 1953525168ull && s->sector_size == 512 && s->phys_sector_size == 4096 && s->lba48,
          "capacity %llu, %u/%u", (unsigned long long)s->sectors, s->sector_size, s->phys_sector_size);
    CHECK(s->rpm == 5400 && s->sata_max == 3 && s->sata_cur == 3, "rpm %d, SATA %d/%d", s->rpm, s->sata_max,
          s->sata_cur);
    CHECK(s->smart_supported && s->smart_enabled && s->selftest_supported, "SMART and self-test supported");
    /* QEMU's IDENTIFY does not say self-tests; its SMART data does. */
    id[2 * 84] = 0x61;
    smart_info_t *q = calloc(1, sizeof *q);
    uint8_t qd[512], qt[512];
    smart_parse_identify(id, q);
    CHECK(!q->selftest_supported, "word 84 without bit 1");
    smart_data(qd, qt, WD_PURPLE, 3, 0);
    smart_parse_data(qd, qt, q);
    CHECK(q->selftest_supported, "self-tests from the data's capability byte");
    free(q);
    memset(id, 0, sizeof id);
    CHECK(smart_parse_identify(id, s) < 0, "all zeros (a bridge that answers nothing): refused");

    parse(s, "WDC WD10PURZ-85U8XY0", "WD-1", 5400, WD_PURPLE, N(WD_PURPLE));
    CHECK(s->verdict == SMART_GOOD && strstr(s->reason, "No reallocated"), "WD Purple: good (%s)", s->reason);
    CHECK(s->temp == 37 && s->hours == 16234, "temp %d, hours %ld", s->temp, s->hours);
    const smart_attr_t *a = attr(s, 194);
    CHECK(a && !strcmp(a->raw_text, "37 °C (min 20, max 45)"), "194 decoded: %s", a ? a->raw_text : "");
    a = attr(s, 3);
    CHECK(a && !strcmp(a->raw_text, "2383 ms") && !strcmp(a->name, "Spin-up time"), "3: %s", a ? a->raw_text : "");
    a = attr(s, 5);
    CHECK(a && a->thresh == 140 && a->critical && a->level == SMART_GOOD, "5: threshold and critical");
    CHECK(s->short_min == 2 && s->ext_min == 410, "self-test times %d/%d min", s->short_min, s->ext_min);

    parse(s, "ST1000DM003-1CH162", "Z1D5ABCD", 7200, ST_BAD, N(ST_BAD));
    CHECK(s->verdict == SMART_WARN, "Seagate: warning (%d)", s->verdict);
    static const char first[] = "1704 reallocated sectors, 8 pending sectors, 8 offline uncorrectable sectors, ...";
    CHECK(!strncmp(s->reason, first, sizeof first - 1), "the counts that matter first: %s", s->reason);
    a = attr(s, 188);
    CHECK(a && !strcmp(a->raw_text, "0 3 3") && a->level == SMART_WARN, "188: three counters: %s",
          a ? a->raw_text : "");
    a = attr(s, 197);
    CHECK(a && a->level == SMART_WARN && strstr(a->note, "remapped"), "197 highlighted: %s", a ? a->note : "");
    a = attr(s, 190);
    CHECK(a && a->level == SMART_GOOD && !strcmp(a->raw_text, "36 °C"), "190: %s (Seagate's min/max layout "
          "is not guessed)", a ? a->raw_text : "");
    CHECK(s->temp == 36, "194 is the temperature: %d", s->temp);
    a = attr(s, 1);
    CHECK(a && a->level == SMART_GOOD && !strcmp(a->raw_text, "46031352"), "1: Seagate's raw is not an error count");

    parse(s, "Samsung SSD 860 EVO 250GB", "S3YJNB0K123456A", 1, EVO, N(EVO));
    CHECK(s->rpm == 1 && s->verdict == SMART_GOOD && s->temp == 33, "860 EVO: an SSD, good, 33 °C (%s)", s->reason);
    a = attr(s, 177);
    CHECK(a && !strcmp(a->name, "Wear leveling count"), "177 named");
    attr_t worn[N(EVO)];
    memcpy(worn, EVO, sizeof worn);
    worn[3].value = worn[3].worst = 8;
    parse(s, "Samsung SSD 860 EVO 250GB", "S3YJNB0K123456A", 1, worn, N(worn));
    CHECK(s->verdict == SMART_WARN && strstr(s->reason, "Worn out (8% left)"), "worn SSD: %s", s->reason);

    parse(s, "WDC WD40EFRX-68N32N0", "WD-WCC7K0", 5400, WD_RED_CRC, N(WD_RED_CRC));
    CHECK(s->verdict == SMART_WARN && strstr(s->reason, "1317 interface CRC errors") &&
          strstr(s->reason, "cable or the connection, not the disk"), "CRC only: %s", s->reason);
    a = attr(s, 199);
    CHECK(a && a->level == SMART_WARN && strstr(a->note, "cable"), "199's note: %s", a ? a->note : "");

    parse(s, "WDC WD20EARS-00MVWB0", "WD-WMAZA0", 5400, DYING, N(DYING));
    CHECK(s->verdict == SMART_FAIL && strstr(s->reason, "Reallocated sectors is at or below"), "dying: %s",
          s->reason);
    a = attr(s, 5);
    CHECK(a && a->level == SMART_FAIL, "5 failing");
    a = attr(s, 194);
    CHECK(a && a->level == SMART_WARN && strstr(a->note, "Hotter"), "61 °C: too hot (%s)", a ? a->note : "");
    a = attr(s, 1);
    CHECK(a && a->level == SMART_WARN && strstr(a->note, "threshold once"), "worst at threshold: %s",
          a ? a->note : "");

    parse(s, "WDC WD10PURZ-85U8XY0", "WD-1", 5400, WD_PURPLE, N(WD_PURPLE));
    s->health = 0;
    smart_evaluate(s);
    CHECK(s->verdict == SMART_FAIL && strstr(s->reason, "reports that it is failing"), "RETURN STATUS failing");

    /* The self-test log: three entries, the index at the newest. */
    uint8_t log[512];
    memset(log, 0, sizeof log);
    log[0] = 1;
    uint8_t *e = log + 2;
    e[0] = 1, e[1] = 0x00, e[2] = 0x10, e[3] = 0x3f;           /* short, OK, 16144 h */
    e += 24;
    e[0] = 2, e[1] = 0x00, e[2] = 0x20, e[3] = 0x3f;           /* extended, OK */
    e += 24;
    e[0] = 2, e[1] = 0x79, e[2] = 0x6a, e[3] = 0x3f;           /* extended, read failure, 90% left */
    e[5] = 0x40, e[6] = 0xe2, e[7] = 0x01;                     /* LBA 123456 */
    log[508] = 3;
    parse(s, "WDC WD10PURZ-85U8XY0", "WD-1", 5400, WD_PURPLE, N(WD_PURPLE));
    CHECK(smart_parse_selftest_log(log, s) == 3 && s->tests[0].lba == 123456 && s->tests[2].type == 1,
          "log: newest first (%d)", s->n_tests);
    smart_evaluate(s);
    CHECK(s->verdict == SMART_FAIL && strstr(s->reason, "self-test failed at LBA 123456"), "last test: %s",
          s->reason);
    char t[64];
    smart_test_status_text(0x79, t, sizeof t);
    CHECK(!strcmp(t, "Failed: read element"), "status text: %s", t);
    smart_test_status_text(0xf4, t, sizeof t);
    CHECK(!strcmp(t, "In progress (40% left)"), "in progress: %s", t);

    /* No SMART at all, or SMART off. */
    identify(id, "VBOX HARDDISK", "VB1", "1.0", 1000000, 0, 0, 0, 0);
    memset(s, 0, sizeof *s);
    s->available = smart_parse_identify(id, s) == 0;
    s->health = -1;
    smart_evaluate(s);
    CHECK(s->verdict == SMART_UNKNOWN && strstr(s->reason, "turned off"), "SMART off: %s", s->reason);
    free(s);
}

/* ---- a fake drive behind the ATA transport ----------------------------------------------- */

static struct {
    int      unsup;                 /* answers nothing (a USB stick) */
    int      failing;               /* RETURN STATUS: threshold exceeded */
    uint8_t  id[512], data[512], thr[512], log[512];
    int      exec;                  /* the last self-test subcommand */
    int      verify_unsup;
    uint64_t verify_bad;            /* READ VERIFY fails over this LBA (0: never) */
    int      verifies;
    int      verify_ms;             /* each READ VERIFY takes this long (a weak area) */
} D;

static int fake_ata(int fd, ata_tf_t *tf, int proto, void *buf, size_t len, int timeout_ms)
{
    (void)fd;
    (void)timeout_ms;
    if (D.unsup) return ATA_EUNSUP;
    if (tf->command == 0xEC && proto == ATA_PIO_IN && len == 512) {
        memcpy(buf, D.id, 512);
        return ATA_OK;
    }
    if (tf->command == 0x42) {
        if (D.verify_unsup) return ATA_EUNSUP;
        ++D.verifies;
        if (D.verify_ms) usleep((useconds_t)D.verify_ms * 1000);
        if (D.verify_bad && D.verify_bad >= tf->lba && D.verify_bad < tf->lba + (tf->count ? tf->count : 65536u)) {
            tf->have_regs = 1;
            tf->status = ATA_ST_ERR;
            tf->error = ATA_ER_UNC;
            return ATA_EDEV;
        }
        return ATA_OK;
    }
    if (tf->command != 0xB0 || (tf->lba & 0xffff00) != 0xc24f00) return ATA_EDEV;
    switch (tf->features) {
    case 0xD0: memcpy(buf, D.data, 512); return ATA_OK;
    case 0xD1: memcpy(buf, D.thr, 512); return ATA_OK;
    case 0xD5: memcpy(buf, D.log, 512); return (tf->lba & 0xff) == 6 ? ATA_OK : ATA_EDEV;
    case 0xD4: D.exec = (int)(tf->lba & 0xff); return ATA_OK;
    case 0xDA:
        if (!tf->want_regs) return ATA_EDEV;
        tf->have_regs = 1;
        tf->status = 0x50;
        tf->out_lba = D.failing ? 0x2cf400 : 0xc24f00;
        return ATA_OK;
    default: return ATA_EDEV;
    }
}

#define MIB (1024 * 1024)

/* A fake disk /dev/<name> of `bytes` (a plain file), with sysfs. */
static void fake_disk(const char *name, long long bytes, const char *model)
{
    char rel[128], v[32], p[512];
    snprintf(rel, sizeof rel, "/sys/block/%s/size", name);
    snprintf(v, sizeof v, "%lld\n", bytes / 512);
    fake_file(rel, v);
    snprintf(rel, sizeof rel, "/sys/block/%s/removable", name);
    fake_file(rel, "0\n");
    snprintf(rel, sizeof rel, "/sys/block/%s/ro", name);
    fake_file(rel, "0\n");
    snprintf(rel, sizeof rel, "/sys/block/%s/device/model", name);
    fake_file(rel, model);
    snprintf(rel, sizeof rel, "/sys/block/%s/queue/logical_block_size", name);
    fake_file(rel, "512\n");
    snprintf(rel, sizeof rel, "/dev/%s", name);
    fake_file(rel, "");
    snprintf(p, sizeof p, "%s%s", fake_root, rel);
    if (truncate(p, bytes) < 0) perror(p);
}

static void use_config(const char *yaml)
{
    char cfgpath[300];
    fake_file("/etc/vigil.yaml", yaml);
    snprintf(cfgpath, sizeof cfgpath, "%s/etc/vigil.yaml", fake_root);
    config_init(cfgpath);
}

void test_sys_smart(void)
{
    test_parse();

    fake_init();
    fake_file("/proc/mounts", "/dev/root / squashfs ro 0 0\n");
    fake_file("/proc/cmdline", "root=/dev/mtdblock3\n");
    fake_disk("sdz", 4 * MIB, "WDC WD10PURZ-85U\n");
    fake_disk("sdy", 4 * MIB, "Cruzer Blade\n");
    use_config("record:\n  path: /mnt/sdz1/rec\n");
    ata_set_transport(fake_ata);
    memset(&D, 0, sizeof D);
    identify(D.id, "WDC WD10PURZ-85U8XY0", "WD-WCC4J1234567", "01.01A01", 1953525168ull, 5400, 1, 3, 1);
    smart_data(D.data, D.thr, ST_BAD, N(ST_BAD), 0xf4);
    D.log[508] = 1;
    D.log[2] = 1;
    D.log[4] = 0x10;
    D.log[5] = 0x3f;

    smart_info_t *s = calloc(1, sizeof *s);
    char err[256];
    CHECK(smart_read("sdz", s) == 0 && s->available && s->health == 1 && s->n_attrs == N(ST_BAD),
          "smart_read: %d attributes, health %d (%s)", s->n_attrs, s->health, s->why);
    CHECK(s->verdict == SMART_WARN && s->test_status == 15 && s->test_remaining == 40 && s->n_tests == 1,
          "a self-test in progress, 40%% left; the log read");
    D.failing = 1;
    CHECK(mgmt_smart("sdz", s, err, sizeof err) == MGMT_OK && s->health == 0 && s->verdict == SMART_FAIL,
          "mgmt_smart: failing (%s)", s->reason);
    D.failing = 0;
    time_t when = 0;
    CHECK(mgmt_smart_cached("sdz", s, &when) == 0 && when > 0 && s->health == 0, "kept for the screen");
    CHECK(mgmt_smart_cached("sdy", s, &when) < 0, "nothing kept for sdy");
    CHECK(mgmt_selftest("sdz", 2, err, sizeof err) == MGMT_OK && D.exec == 2, "extended self-test started");
    CHECK(mgmt_selftest("sdz", 0, err, sizeof err) == MGMT_OK && D.exec == 0x7f, "aborted");
    CHECK(mgmt_smart("sdq", s, err, sizeof err) == MGMT_EINVAL && strstr(err, "no disk"), "no such disk: %s", err);

    /* A USB stick: nothing passes ATA. */
    D.unsup = 1;
    CHECK(mgmt_smart("sdy", s, err, sizeof err) == MGMT_OK && !s->available && s->verdict == SMART_UNKNOWN &&
          strstr(s->reason, "not available") && strstr(s->reason, "USB stick"), "a stick: %s", s->reason);
    D.unsup = 0;

    sys_set_managed(0);
    CHECK(mgmt_smart("sdz", s, err, sizeof err) == MGMT_EUNMANAGED, "unmanaged: no SMART");
    sys_set_managed(1);
    free(s);
    ata_set_transport(NULL);
    fake_done();
}

/* ---- the surface scan ------------------------------------------------------------------- */

static struct {
    uint64_t err_lba, slow_lba, mid_lba;    /* blocks starting here: an error, 600 ms, 60 ms */
    int      err_pass;                       /* 0: any */
    uint64_t err_from;                       /* every block from here an error (0: none) */
    int      delay_us;
} H;

static int hook(int mode, int pass, uint64_t lba, unsigned n, double *ms)
{
    (void)mode;
    (void)n;
    if (H.delay_us) usleep((useconds_t)H.delay_us);
    if (lba == H.slow_lba) *ms = 600;
    else if (lba == H.mid_lba) *ms = 60;
    else *ms = 1;
    if (H.err_from && lba >= H.err_from) return -1;
    return lba == H.err_lba && (!H.err_pass || H.err_pass == pass) ? -1 : 0;
}

static int wait_scan(scan_status_t *st, int secs)
{
    for (int i = 0; i < secs * 100; ++i) {
        scan_status(st);
        if (st->state >= SCAN_DONE) return st->state;
        usleep(10 * 1000);
    }
    return st->state;
}

static unsigned le64_at(const char *dev, uint64_t off, uint64_t *v)
{
    char p[512];
    snprintf(p, sizeof p, "%s/dev/%s", fake_root, dev);
    int fd = open(p, O_RDONLY);
    uint8_t b[8] = { 0 };
    ssize_t r = fd >= 0 ? pread(fd, b, 8, (off_t)off) : -1;
    if (fd >= 0) close(fd);
    *v = 0;
    for (int i = 7; i >= 0; --i) *v = *v << 8 | b[i];
    return r == 8;
}

void test_sys_scan(void)
{
    fake_init();
    fake_file("/proc/mounts", "/dev/root / squashfs ro 0 0\n/dev/sdz1 /mnt/sdz1 vfat rw 0 0\n");
    fake_file("/proc/cmdline", "root=/dev/mtdblock3\n");
    fake_disk("sdz", 32 * MIB, "WDC WD10PURZ-85U\n");   /* 65536 sectors: 256 blocks of 256 */
    fake_file("/sys/block/sdz/sdz1/partition", "1\n");
    fake_file("/sys/block/sdz/sdz1/size", "63488\n");
    fake_disk("sdx", 4 * MIB, "Spare\n");
    use_config("record:\n  path: /mnt/sdz1/rec\n");
    ata_set_transport(fake_ata);
    memset(&D, 0, sizeof D);
    scan_set_hook(hook);
    memset(&H, 0, sizeof H);
    H.err_lba = 256 * 10;
    H.slow_lba = 256 * 20;
    H.mid_lba = 256 * 30;

    CHECK(scan_class_of(4.9) == SCAN_C5 && scan_class_of(5) == SCAN_C20 && scan_class_of(149) == SCAN_C150 &&
          scan_class_of(500) == SCAN_CSLOW, "classes");
    unsigned ss = 0;
    CHECK(scan_disk_sectors("sdz", &ss) == 65536 && ss == 512, "sectors from sysfs");

    char err[256];
    scan_status_t *st = calloc(1, sizeof *st);
    /* Read of the recording disk, mounted: allowed, at idle priority. */
    CHECK(mgmt_scan_start("sdz", SCAN_READ, 0, 0, 0, NULL, err, sizeof err) == MGMT_OK, "read started: %s", err);
    CHECK(wait_scan(st, 10) == SCAN_DONE, "read done (%s)", st->msg);
    CHECK(st->blocks_done == 256 && st->blocks_total == 256 && st->cells == 256, "256 blocks, a cell each");
    CHECK(st->counts[SCAN_CERR] == 1 && st->counts[SCAN_CSLOW] == 1 && st->counts[SCAN_C150] == 1 &&
          st->counts[SCAN_C5] == 253, "counts: %llu err, %llu slow, %llu <150, %llu <5",
          (unsigned long long)st->counts[SCAN_CERR], (unsigned long long)st->counts[SCAN_CSLOW],
          (unsigned long long)st->counts[SCAN_C150], (unsigned long long)st->counts[SCAN_C5]);
    CHECK(st->n_bad == 1 && st->bad[0].lba == 2560 && st->bad[0].count == 256, "the bad range");
    CHECK(st->map[10] == 1 + SCAN_CERR && st->map[20] == 1 + SCAN_CSLOW && st->map[30] == 1 + SCAN_C150 &&
          st->map[0] == 1 + SCAN_C5 && st->lba == 65536, "the map");
    CHECK(strstr(st->msg, "1 error in 1 bad range"), "summary: %s", st->msg);

    /* A range and a block size; the map scaled when blocks outnumber cells. */
    CHECK(mgmt_scan_start("sdz", SCAN_READ, 1000, 9000, 8, NULL, err, sizeof err) == MGMT_OK, "a range: %s", err);
    CHECK(wait_scan(st, 10) == SCAN_DONE && st->blocks_done == 1000 && st->cells == 1000 && st->start == 1000 &&
          st->end == 9000, "1000 blocks of 8 sectors (%llu)", (unsigned long long)st->blocks_done);
    CHECK(mgmt_scan_start("sdz", SCAN_READ, 0, 0, 4, NULL, err, sizeof err) == MGMT_OK, "4-sector blocks");
    CHECK(wait_scan(st, 30) == SCAN_DONE && st->blocks_done == 16384 && st->cells == SCAN_MAP &&
          st->map[SCAN_MAP - 1] && st->map[(2560 / 4) * SCAN_MAP / 16384] == 1 + SCAN_CERR,
          "16384 blocks on %d cells", st->cells);
    CHECK(mgmt_scan_start("sdz", SCAN_READ, 9000, 1000, 0, NULL, err, sizeof err) == MGMT_EINVAL, "backwards: %s",
          err);
    CHECK(mgmt_scan_start("sdz1", SCAN_READ, 0, 0, 0, NULL, err, sizeof err) == MGMT_EINVAL &&
          strstr(err, "partition"), "a partition: %s", err);
    CHECK(mgmt_scan_start("sdz", SCAN_READ, 0, 0, 8192, NULL, err, sizeof err) == MGMT_EINVAL, "4 MiB blocks: %s",
          err);

    /* Verify: READ VERIFY EXT through the transport; one bad LBA. */
    D.verify_bad = 5000;
    H.err_lba = UINT64_MAX;
    CHECK(mgmt_scan_start("sdz", SCAN_VERIFY, 0, 0, 0, NULL, err, sizeof err) == MGMT_OK, "verify: %s", err);
    CHECK(wait_scan(st, 10) == SCAN_DONE && !st->read_instead && D.verifies == 257 && st->counts[SCAN_CERR] == 1 &&
          st->bad[0].lba == 4864, "verify: %d commands, the block of LBA 5000 bad", D.verifies);
    /* A bridge without it: reading instead, said. */
    D.verify_unsup = 1;
    CHECK(mgmt_scan_start("sdz", SCAN_VERIFY, 0, 0, 0, NULL, err, sizeof err) == MGMT_OK, "verify 2");
    CHECK(wait_scan(st, 10) == SCAN_DONE && st->read_instead && strstr(st->msg, "READ VERIFY") && st->counts[SCAN_CERR] == 0,
          "fell back to reading: %s", st->msg);
    D.verify_unsup = 0;

    /* Write: refused on the mounted recording disk, without the name. */
    CHECK(mgmt_scan_start("sdz", SCAN_WRITE, 0, 0, 0, "sdz", err, sizeof err) == MGMT_EINVAL &&
          strstr(err, "mounted"), "write: mounted (%s)", err);
    fake_file("/proc/mounts", "/dev/root / squashfs ro 0 0\n");
    CHECK(mgmt_scan_start("sdz", SCAN_WRITE, 0, 0, 0, "sdz", err, sizeof err) == MGMT_EINVAL &&
          strstr(err, "recording directory"), "write: the recording directory (%s)", err);
    CHECK(mgmt_scan_start("sdx", SCAN_WRITE, 0, 0, 0, "", err, sizeof err) == MGMT_EINVAL && strstr(err, "type"),
          "write: the name typed (%s)", err);
    /* Fill sdx with something, write it over. */
    char p[512];
    snprintf(p, sizeof p, "%s/dev/sdx", fake_root);
    int fd = open(p, O_WRONLY);
    char junk[4096];
    memset(junk, 0x33, sizeof junk);
    for (int i = 0; i < 1024; ++i) CHECK(write(fd, junk, sizeof junk) == (ssize_t)sizeof junk, "junk");
    close(fd);
    H.err_lba = 256 * 3;
    H.err_pass = 2;
    fake_clear_log();
    CHECK(mgmt_scan_start("sdx", SCAN_WRITE, 0, 0, 0, "sdx", err, sizeof err) == MGMT_OK, "write: %s", err);
    CHECK(wait_scan(st, 10) == SCAN_DONE && st->passes == 2 && st->blocks_total == 64 && st->blocks_done == 64,
          "write: two passes (%llu blocks)", (unsigned long long)st->blocks_done);
    CHECK(st->counts[SCAN_CERR] == 1 && st->n_bad == 1 && st->bad[0].lba == 768, "the read-back error");
    uint64_t v = 0;
    CHECK(le64_at("sdx", 0, &v) && v == 0, "sector 0 holds LBA 0");
    CHECK(le64_at("sdx", 512 * 4321ull, &v) && v == 4321, "sector 4321 holds its LBA (%llu)", (unsigned long long)v);
    CHECK(fake_ran("reread"), "the partition table re-read");
    H.err_pass = 0;

    /* Pause, resume, one at a time, stop. */
    H.err_lba = UINT64_MAX;
    H.delay_us = 20000;
    CHECK(mgmt_scan_start("sdz", SCAN_READ, 0, 0, 0, NULL, err, sizeof err) == MGMT_OK, "slow read");
    CHECK(mgmt_scan_start("sdx", SCAN_READ, 0, 0, 0, NULL, err, sizeof err) == MGMT_EBUSY &&
          strstr(err, "under way"), "one at a time: %s", err);
    CHECK(mgmt_disk_format("sdz1", "vfat", "sdz1", err, sizeof err) == MGMT_EBUSY && strstr(err, "surface scan"),
          "no format under a scan: %s", err);
    usleep(100 * 1000);
    CHECK(mgmt_scan_pause(1) == MGMT_OK, "pause");
    usleep(100 * 1000);
    scan_status(st);
    uint64_t at = st->blocks_done;
    usleep(150 * 1000);
    scan_status(st);
    CHECK(st->state == SCAN_PAUSED && st->blocks_done == at && at > 0 && at < 256, "paused at %llu",
          (unsigned long long)at);
    CHECK(scan_active("sdz") == SCAN_READ && scan_active("sdx") < 0, "active on sdz");
    CHECK(mgmt_scan_pause(0) == MGMT_OK, "resume");
    usleep(100 * 1000);
    scan_status(st);
    CHECK(st->state == SCAN_RUNNING && st->blocks_done > at, "running again (%llu)",
          (unsigned long long)st->blocks_done);
    CHECK(mgmt_scan_stop() == MGMT_OK && wait_scan(st, 5) == SCAN_STOPPED && st->blocks_done < 256,
          "stopped at %llu", (unsigned long long)st->blocks_done);
    CHECK(mgmt_scan_stop() == MGMT_EINVAL, "nothing to stop");

    /* vigild exits during a scan: stopped and joined. */
    CHECK(mgmt_scan_start("sdz", SCAN_READ, 0, 0, 0, NULL, err, sizeof err) == MGMT_OK, "again");
    usleep(50 * 1000);
    scan_shutdown();
    scan_status(st);
    CHECK(st->state == SCAN_STOPPED, "shutdown: stopped (%s)", scan_state_name(st->state));

    /* ... or in a block that takes long (a VERIFY retrying a weak sector):
     * not waited for beyond a couple of seconds; it stops when it can. */
    D.verify_ms = 4000;
    CHECK(mgmt_scan_start("sdz", SCAN_VERIFY, 0, 0, 0, NULL, err, sizeof err) == MGMT_OK, "a slow verify");
    usleep(50 * 1000);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    scan_shutdown();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double waited = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    CHECK(waited < 3.0, "shutdown in a 4 s block: back after %.1f s", waited);
    CHECK(wait_scan(st, 10) == SCAN_STOPPED, "and the scan stopped after its block (%s)", scan_state_name(st->state));
    D.verify_ms = 0;

    /* Unplugged under the scan: its reads fail and sysfs has no disk. */
    H.err_from = 2048;
    CHECK(mgmt_scan_start("sdx", SCAN_READ, 0, 0, 0, NULL, err, sizeof err) == MGMT_OK, "a scan of sdx");
    char cmd[400];
    snprintf(cmd, sizeof cmd, "rm -rf '%s/sys/block/sdx'", fake_root);
    CHECK(system(cmd) == 0, "sdx unplugged");
    CHECK(wait_scan(st, 10) == SCAN_FAILED && strstr(st->msg, "went away"), "failed: %s", st->msg);
    H.err_from = 0;

    H.delay_us = 0;
    scan_set_hook(NULL);
    ata_set_transport(NULL);
    free(st);
    fake_done();
}
