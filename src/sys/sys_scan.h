/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_scan.h — a surface scan of a disk, like Victoria or MHDD: every
 * block from a start LBA to an end LBA, timed, on a worker thread.
 *
 *   Read     O_DIRECT reads (no page cache), one block at a time.
 *   Verify   ATA READ VERIFY SECTORS EXT (sys_ata.h): the drive reads
 *            the block into nothing, so no data crosses the bus. A USB
 *            bridge may not pass it: the scan then reads, and says so.
 *   Write    destructive: a pattern (each sector's LBA in its first
 *            bytes) to every block, then a second pass reads each block
 *            back and compares. Everything on the disk is lost, the
 *            partition table first; the device is opened O_EXCL, which
 *            the kernel refuses while anything on it is mounted.
 *
 * Each block's time puts it in a class, Victoria's: < 5, < 20, < 50,
 * < 150, < 500, >= 500 ms, or an error (the block, a range of LBAs,
 * goes to the bad list). The map is SCAN_MAP cells over the range (each
 * cell some blocks), holding the worst class seen in it.
 *
 * One scan at a time, in vigild. It goes on when nobody looks (the page
 * reattaches with scan_status), can be paused and stopped, and
 * scan_shutdown stops it before vigild exits. Read and Verify of a disk
 * in use may run at the idle I/O priority (`idle`), so recording goes
 * first; latencies are then the disk's and the queue's.
 */
#ifndef SYS_SCAN_H
#define SYS_SCAN_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

enum { SCAN_READ, SCAN_VERIFY, SCAN_WRITE };
enum { SCAN_IDLE, SCAN_RUNNING, SCAN_PAUSED, SCAN_DONE, SCAN_STOPPED, SCAN_FAILED };

/* The classes: SCAN_C5 .. SCAN_CSLOW by time, SCAN_CERR an error. */
enum { SCAN_C5, SCAN_C20, SCAN_C50, SCAN_C150, SCAN_C500, SCAN_CSLOW, SCAN_CERR, SCAN_NCLASS };
/* The upper bounds of the timed classes, ms. */
#define SCAN_LIMITS { 5, 20, 50, 150, 500 }

#define SCAN_MAP     2000
#define SCAN_BAD_MAX 64
#define SCAN_BLOCK_MAX 65536u       /* sectors: READ VERIFY's count */

typedef struct {
    uint64_t lba, count;
} scan_range_t;

typedef struct {
    int      state;                 /* SCAN_IDLE .. SCAN_FAILED */
    int      mode;                  /* SCAN_READ / VERIFY / WRITE */
    int      read_instead;          /* Verify asked, the device did not pass it: reading */
    char     disk[32], model[64], serial[24];
    uint64_t start, end;            /* LBAs, [start, end) */
    unsigned block;                 /* sectors per block */
    unsigned sector;                /* bytes per sector */
    int      pass, passes;          /* 1-based; Write has 2 */
    uint64_t lba;                   /* where it is */
    uint64_t blocks_done, blocks_total;     /* over every pass */
    uint64_t counts[SCAN_NCLASS];
    double   mbps;                  /* lately */
    long     elapsed, eta;          /* seconds; eta -1 unknown */
    time_t   started, finished;     /* wall clock; 0 */
    char     msg[200];
    int      n_bad;
    uint64_t bad_more;              /* ranges past SCAN_BAD_MAX */
    scan_range_t bad[SCAN_BAD_MAX];
    int      cells;                 /* of the map in use (<= SCAN_MAP) */
    uint8_t  map[SCAN_MAP];         /* 0 not yet; 1 + class */
} scan_status_t;

typedef struct {
    char     disk[32];              /* /dev/<disk> under the root (sys.h) */
    char     model[64], serial[24]; /* for the status, the events */
    int      mode;
    uint64_t start, end;            /* LBAs; end 0: the end of the disk */
    unsigned block;                 /* sectors; 0: 256 */
    int      idle;                  /* idle I/O priority */
    /* Called on the scan's thread when it ends (any end). */
    void   (*done)(const scan_status_t *st);
} scan_req_t;

/* Start a scan. 0, or -1 with `err` (one is under way, the disk cannot
 * be opened, the range is wrong). */
int  scan_start(const scan_req_t *r, char *err, size_t cap);
void scan_status(scan_status_t *out);
/* Is a scan running or paused (on `disk`; NULL: any)? Its mode, or -1. */
int  scan_active(const char *disk);
int  scan_pause(int on);            /* 0, -1 when none runs */
int  scan_stop(void);               /* 0, -1 when none runs; it ends shortly */
/* Stop it (vigild's exit): the thread is waited for 2 s at most, then
 * left to end with the process (a block under way can take 30 s). */
void scan_shutdown(void);

const char *scan_mode_name(int mode);       /* "read", "verify", "write" */
const char *scan_state_name(int state);     /* "idle", "running", ... */
int         scan_mode_parse(const char *s); /* -1 when none */
int         scan_class_of(double ms);

/* The sectors of /dev/<disk> (logical) and their size, from sysfs. 0
 * when unknown. */
uint64_t scan_disk_sectors(const char *disk, unsigned *sector);

/* For the tests: after each block's I/O, `hook` may turn it into an
 * error (return -1) or change its time (`ms`). Pass is 1-based. */
typedef int (*scan_hook_t)(int mode, int pass, uint64_t lba, unsigned count, double *ms);
void scan_set_hook(scan_hook_t h);

#endif /* SYS_SCAN_H */
