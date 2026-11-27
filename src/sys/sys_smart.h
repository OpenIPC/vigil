/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_smart.h — a disk's identity and SMART health, read in C (no
 * smartmontools on the firmware): IDENTIFY DEVICE, SMART READ DATA,
 * READ THRESHOLDS, RETURN STATUS and the self-test log, through ATA
 * PASS-THROUGH (sys_ata.h). Works for a SATA disk (libata) and for most
 * USB enclosures (their bridge translates, SAT); a USB stick, a memory
 * card or a bridge without SAT gets "not available" and why.
 *
 * The parsing is separate from the reading, so the tests feed it the
 * 512-byte sectors a drive answers with.
 *
 * The verdict, for a person deciding whether to trust a disk with
 * recordings:
 *   Failing   the drive says so (SMART RETURN STATUS), an attribute is at
 *             or below its threshold, or the last self-test failed;
 *   Warning   a raw count that only grows on a sick disk is not zero:
 *             reallocated (5) or pending (197) sectors, reallocation
 *             events (196), offline uncorrectable (198), reported
 *             uncorrectable (187), command timeouts (188), spin retries
 *             (10), end-to-end errors (184); an attribute that went to
 *             its threshold once (worst); interface CRC errors (199),
 *             which are usually the cable or the connection, not the
 *             disk; too hot (194/190); an SSD worn out;
 *   Good      none of that.
 */
#ifndef SYS_SMART_H
#define SYS_SMART_H

#include <stddef.h>
#include <stdint.h>

#define SMART_ATTRS 30
#define SMART_TESTS 21

/* Verdicts, and an attribute's level. */
enum { SMART_GOOD = 0, SMART_WARN = 1, SMART_FAIL = 2, SMART_UNKNOWN = 3 };

/* Above this (°C) a disk is too hot. */
#define SMART_HOT_HDD 55
#define SMART_HOT_SSD 70

typedef struct {
    uint8_t  id;
    uint16_t flags;                 /* bit 0: pre-failure */
    uint8_t  value, worst, thresh;
    uint8_t  rawb[6];
    uint64_t raw;                   /* the 48-bit raw value */
    char     name[32];
    char     raw_text[40];          /* decoded: "38 °C (min 21, max 46)", "12345 h" */
    int      level;                 /* SMART_GOOD / WARN / FAIL */
    int      critical;              /* one of the counts that matter (above) */
    char     note[96];              /* why it is highlighted */
} smart_attr_t;

typedef struct {
    uint8_t  type;                  /* the subcommand: 1 short, 2 extended, 3 conveyance, 0x81.. captive */
    uint8_t  status;                /* the self-test execution status byte */
    uint16_t hours;                 /* power-on hours at the test */
    uint32_t lba;                   /* first failure */
} smart_test_t;

typedef struct {
    int  available;                 /* IDENTIFY answered */
    char why[160];                  /* when not, or what is missing */
    char transport[24];             /* "SATA", "USB" */
    char model[44], serial[24], firmware[12];
    uint64_t sectors;               /* logical sectors */
    unsigned sector_size, phys_sector_size;
    int  rpm;                       /* 0 unknown, 1 solid state, else rpm */
    int  sata_max, sata_cur;        /* SATA generation: 1 1.5 Gb/s, 2 3.0, 3 6.0; 0 unknown */
    int  lba48;

    int  smart_supported, smart_enabled, has_data, has_thresh;
    int  health;                    /* SMART RETURN STATUS: 1 passed, 0 failing, -1 unknown */
    int  n_attrs;
    smart_attr_t attrs[SMART_ATTRS];
    int  temp;                      /* °C, -1 unknown */
    long hours;                     /* power on, -1 unknown */

    int  test_status;               /* the current self-test, data byte 363 (>> 4) */
    int  test_remaining;            /* % left while one runs (status 15) */
    int  short_min, ext_min;        /* how long the self-tests take */
    int  selftest_supported;
    int  n_tests;                   /* newest first */
    smart_test_t tests[SMART_TESTS];

    int  verdict;                   /* SMART_GOOD / WARN / FAIL, UNKNOWN when not available */
    char reason[200];
} smart_info_t;

/* ---- parsing (no device) ---------------------------------------------------- */

/* IDENTIFY DEVICE data (256 little-endian words as read). 0, or -1 when
 * it is not a plausible answer (all zeros, no model). */
int  smart_parse_identify(const uint8_t id[512], smart_info_t *s);
/* SMART READ DATA and READ THRESHOLDS (NULL when the drive gave none):
 * the attribute table, the self-test status and times. Checksums are
 * not enforced (some drives get them wrong); returns -1 on an empty
 * table. */
int  smart_parse_data(const uint8_t data[512], const uint8_t *thresh, smart_info_t *s);
/* The SMART self-test log (log address 06h). */
int  smart_parse_selftest_log(const uint8_t log[512], smart_info_t *s);
/* Every attribute's level and note, and the verdict with its reason. */
void smart_evaluate(smart_info_t *s);

const char *smart_attr_name(uint8_t id, int ssd);
const char *smart_test_type_text(uint8_t type);
/* "Completed without error", "Aborted by host", "Failed: read element",
 * "In progress (40% left)" */
void smart_test_status_text(uint8_t status, char *out, size_t cap);
const char *smart_verdict_name(int v);     /* "good", "warning", "failing", "unknown" */

/* ---- the drive ---------------------------------------------------------------- */

/* Everything above for /dev/<name> (under the root, sys.h). 0 when the
 * drive answered IDENTIFY (SMART may still be off or missing: see
 * `why`), -1 when nothing could be read (`why`). */
int smart_read(const char *name, smart_info_t *s);

/* Start a self-test: 1 short, 2 extended; 0 aborts the one running.
 * 0, or -1 with `err`. */
int smart_selftest(const char *name, int type, char *err, size_t cap);

#endif /* SYS_SMART_H */
