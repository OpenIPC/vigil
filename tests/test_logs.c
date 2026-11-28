/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_logs.c — the alarm and operation logs (logbook.h, logs.h): the
 * line and CSV formats, queries and paging, the disk's files (appending,
 * rotation, reading back past the RAM ring, a restart), the flash copy
 * (how often, what it keeps, read back and merged with the disk without
 * doubles), the operation log's actor, logins and settings saved (field
 * names, never a value), and the alarm feed.
 *
 *   test_logs DIR    DIR: a directory on a disk (the build tree), since
 *                    logs_disk_dir refuses RAM and flash filesystems */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "channel.h"
#include "config.h"
#include "events.h"
#include "hal.h"
#include "hal_capture.h"
#include "hal_decoder.h"
#include "hal_display.h"
#include "logbook.h"
#include "logs.h"
#include "secret.h"

static int g_fail, g_checks;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { ++g_fail; \
    fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* channel.c (the alarm feed reads the channels) wants a platform. */
static const hal_caps_t g_caps = { .name = "test", .max_channels = 4 };
const hal_caps_t *hal_caps(void) { return &g_caps; }
int  hal_dec_open(int ch, vg_codec_t c, const vg_stream_info_t *si) { (void)ch; (void)c; (void)si; return -1; }
int  hal_dec_send(int ch, const vg_au_t *au) { (void)ch; (void)au; return -1; }
void hal_dec_reset(int ch) { (void)ch; }
void hal_dec_close(int ch) { (void)ch; }
int  hal_display_set_tile(int ch, const vg_rect_t *r) { (void)ch; (void)r; return 0; }
void hal_display_clear_tile(int ch) { (void)ch; }
void hal_display_set_aspect(int ch, unsigned w, unsigned h, vg_aspect_t how) { (void)ch; (void)w; (void)h; (void)how; }
int  hal_cap_start(int in, int ch, hal_cap_cb cb, void *user) { (void)in; (void)ch; (void)cb; (void)user; return -1; }
void hal_cap_stop(int in) { (void)in; }
int  hal_cap_signal(int in) { (void)in; return 0; }

static char g_dir[512];

static void rm_rf(const char *path)
{
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", path);
    if (system(cmd)) {}
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static int file_lines(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = 0, c;
    while ((c = fgetc(f)) != EOF) n += c == '\n';
    fclose(f);
    return n;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc((size_t)n + 1);
    if (s && fread(s, 1, (size_t)n, f) != (size_t)n) n = 0;
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

static void rec(logrec_t *r, int ch, const char *type, const char *detail)
{
    memset(r, 0, sizeof *r);
    r->ch = ch;
    r->active = -1;
    snprintf(r->source, sizeof r->source, "test");
    snprintf(r->type, sizeof r->type, "%s", type);
    snprintf(r->detail, sizeof r->detail, "%s", detail);
}

static void push_n(logbook_t *b, int from, int n)
{
    for (int i = from; i < from + n; ++i) {
        logrec_t r;
        char d[32];
        snprintf(d, sizeof d, "n%d", i);
        rec(&r, i % 4 + 1, i % 2 ? "odd" : "even", d);
        r.time = 1000 + i;
        logbook_push(b, &r);
    }
}

/* The detail numbers of a query's records ("n17" -> 17), -1 for others. */
static int num(const logrec_t *r) { return r->detail[0] == 'n' ? atoi(r->detail + 1) : -1; }

static void test_formats(void)
{
    logrec_t r, back;
    rec(&r, 3, "login_failed", "tab\there, line\nthere, back\\slash");
    snprintf(r.id, sizeof r.id, "0000abcd.7");
    snprintf(r.who, sizeof r.who, "an\tna");
    r.time = 1764300000;
    r.active = 1;
    char line[600];
    int n = logbook_format(&r, line, sizeof line);
    CHECK(n > 0 && line[n - 1] == '\n' && !strchr(line, '\r'), "one line: %s", line);
    int tabs = 0;
    for (int i = 0; i < n; ++i) tabs += line[i] == '\t';
    CHECK(tabs == 7, "8 fields: %d tabs", tabs);
    CHECK(logbook_parse(line, (size_t)n - 1, &back) == 0 && !strcmp(back.detail, r.detail) &&
              !strcmp(back.who, r.who) && back.time == r.time && back.ch == 3 && back.active == 1 &&
              !strcmp(back.id, r.id) && !strcmp(back.type, r.type),
          "round trip: %s / %s", back.detail, back.who);
    CHECK(logbook_parse("garbage", 7, &back) < 0 && logbook_parse("a\tb\tc", 5, &back) < 0, "not a record");

    char csv[1200];
    rec(&r, 0, "login_failed", "=HYPERLINK(\"x\"), \"quoted\"");
    snprintf(r.who, sizeof r.who, "-bob");
    r.time = 0;
    CHECK(logbook_csv(&r, 1, csv, sizeof csv) > 0, "csv");
    CHECK(!strncmp(csv, "time,channel,state,source,user,type,detail,id\n", 46), "header: %s", csv);
    CHECK(strstr(csv, ",'-bob,") && strstr(csv, "\"'=HYPERLINK(\"\"x\"\"), \"\"quoted\"\"\""),
          "formula guarded and quoted: %s", csv);
}

static void test_ram(void)
{
    logbook_conf_t c = { .name = "t", .ram = 10, .file_cap = 4096, .files = 2 };
    logbook_t *b = logbook_create(&c);
    push_n(b, 0, 30);
    logrec_t out[20];
    int more = 0;
    int n = logbook_query(b, NULL, 0, 5, out, &more);
    CHECK(n == 5 && more && num(&out[0]) == 29 && num(&out[4]) == 25, "newest first: %d %d", n, num(&out[0]));
    n = logbook_query(b, NULL, 5, 20, out, &more);
    CHECK(n == 5 && !more && num(&out[4]) == 20, "the ring keeps 10 without a disk: %d", n);
    logfilter_t f = { .ch = -1, .type = "odd" };
    n = logbook_query(b, &f, 0, 20, out, &more);
    CHECK(n == 5 && num(&out[0]) == 29, "type filter: %d", n);
    f = (logfilter_t){ .ch = 2, .type = "odd,even" };
    n = logbook_query(b, &f, 0, 20, out, NULL);
    CHECK(n == 3 && out[0].ch == 2 && num(&out[0]) == 29 && num(&out[2]) == 21, "channel filter: %d %d", n,
          num(&out[0]));
    f = (logfilter_t){ .ch = -1, .text = "N2" };
    n = logbook_query(b, &f, 0, 20, out, NULL);
    CHECK(n == 10, "text, any case: %d", n);
    f = (logfilter_t){ .ch = -1, .from = 1022, .to = 1024 };
    n = logbook_query(b, &f, 0, 20, out, NULL);
    CHECK(n == 3 && num(&out[0]) == 24 && num(&out[2]) == 22, "time range: %d", n);
    CHECK(logbook_count(b) == 30, "count");
    logbook_destroy(b);
}

static void test_disk(void)
{
    char dir[600], p[700];
    snprintf(dir, sizeof dir, "%s/disk", g_dir);
    rm_rf(dir);
    logbook_conf_t c = { .name = "ops", .ram = 10, .file_cap = 4096, .files = 3 };
    logbook_t *b = logbook_create(&c);
    push_n(b, 0, 5);                            /* before the disk: written when it comes */
    CHECK(logbook_set_disk(b, dir) == 0, "set disk: %s", strerror(errno));
    snprintf(p, sizeof p, "%s/ops.log", dir);
    CHECK(file_lines(p) == 5, "the RAM's records written at once: %d", file_lines(p));
    for (int i = 5; i < 405; i += 8) {        /* fewer than the ring holds between two syncs */
        push_n(b, i, 8);
        logbook_sync(b, i, 0);
    }
    logbook_sync(b, 1000, 1);
    long total = 0;
    for (int g = 0; g < 4; ++g) {
        snprintf(p, sizeof p, g ? "%s/ops.log.%d" : "%s/ops.log", dir, g);
        long s = file_size(p);
        if (g < 3) CHECK(s > 0 && s < 4096 + 512, "file %d: %ld bytes", g, s);
        else CHECK(s < 0, "no fourth file");
        if (s > 0) total += s;
    }
    CHECK(total < 3 * (4096 + 512), "bounded: %ld", total);

    /* Past the ring: the disk, in order, without a gap or a double. */
    logrec_t out[60];
    int more = 0;
    int n = logbook_query(b, NULL, 0, 60, out, &more);
    int ok = n == 60 && more;
    for (int i = 0; i < n && ok; ++i) ok = num(&out[i]) == 404 - i;
    CHECK(ok, "60 newest across RAM and disk: n %d, first %d, last %d", n, num(&out[0]), num(&out[n - 1]));
    n = logbook_query(b, NULL, 200, 60, out, &more);
    CHECK(n == 60 && num(&out[0]) == 204 && num(&out[59]) == 145, "a page deep in the files: %d..%d",
          num(&out[0]), num(&out[59]));
    logfilter_t ro = { .ch = -1, .ram_only = 1 };
    n = logbook_query(b, &ro, 0, 60, out, &more);
    CHECK(n == 10 && more, "RAM only: %d, more %d", n, more);
    int oldest = 0;
    for (int off = 0;; off += 60) {
        n = logbook_query(b, NULL, off, 60, out, &more);
        if (n) oldest = num(&out[n - 1]);
        if (!more) break;
    }
    CHECK(oldest > 0 && oldest < 300, "the oldest kept: %d (older ones rotated away)", oldest);
    logbook_destroy(b);

    /* A restart: the ring is the disk's newest, the rest still there. */
    b = logbook_create(&c);
    CHECK(logbook_set_disk(b, dir) == 0, "again");
    n = logbook_query(b, NULL, 0, 60, out, &more);
    CHECK(n == 60 && more && num(&out[0]) == 404 && num(&out[10]) == 394, "after a restart: %d %d", n,
          num(&out[0]));
    push_n(b, 405, 1);
    logbook_sync(b, 1, 1);
    snprintf(p, sizeof p, "%s/ops.log", dir);
    char *t = slurp(p);
    CHECK(t && strstr(t, "\tn405\n") && !strstr(strstr(t, "\tn404\n") + 1, "\tn404\n"), "appended once");
    free(t);
    logbook_destroy(b);

    /* A disk that is not writable: the book lets it go, keeps its records. */
    b = logbook_create(&c);
    snprintf(p, sizeof p, "%s/no/such/dir", dir);
    CHECK(logbook_set_disk(b, p) < 0, "a directory that cannot be made");
    char cur[300];
    logbook_disk(b, cur, sizeof cur);
    CHECK(!cur[0], "none in use");
    logbook_destroy(b);
}

static void test_flash(void)
{
    char dir[600], fl[700], disk[700];
    snprintf(dir, sizeof dir, "%s/flash", g_dir);
    rm_rf(dir);
    mkdir(dir, 0755);
    snprintf(fl, sizeof fl, "%s/operations.log", dir);
    snprintf(disk, sizeof disk, "%s/disk", dir);
    logbook_conf_t c = { .name = "operations", .ram = 50, .flash_keep = 5, .flash_secs = 600, .file_cap = 1 << 20,
                         .files = 2 };
    logbook_t *b = logbook_create(&c);
    logbook_set_flash(b, fl);
    push_n(b, 0, 3);
    logbook_sync(b, 100, 0);
    CHECK(file_size(fl) < 0, "not at once: the flash is written at most every 600 s");
    logbook_sync(b, 699, 0);
    CHECK(file_size(fl) < 0, "not before the period");
    logbook_sync(b, 700, 0);
    CHECK(file_lines(fl) == 3, "after it: %d", file_lines(fl));
    struct stat st;
    CHECK(stat(fl, &st) == 0 && (st.st_mode & 077) == 0, "0600: %o", (unsigned)st.st_mode & 0777);
    long m1 = (long)st.st_mtime;
    logbook_sync(b, 2000, 0);
    CHECK(stat(fl, &st) == 0 && (long)st.st_mtime == m1, "nothing new: not written again");
    push_n(b, 3, 7);
    logbook_sync(b, 2001, 1);                   /* the clean stop: at once */
    CHECK(file_lines(fl) == 5, "the newest 5 only: %d", file_lines(fl));
    logbook_destroy(b);

    /* The next start: the flash copy is in RAM, before what is new. */
    b = logbook_create(&c);
    push_n(b, 100, 1);
    logbook_set_flash(b, fl);
    logrec_t out[60];
    int n = logbook_query(b, NULL, 0, 60, out, NULL);
    CHECK(n == 6 && num(&out[0]) == 100 && num(&out[1]) == 9 && num(&out[5]) == 5, "flash + new: %d %d %d", n,
          num(&out[0]), num(&out[1]));
    /* A disk: the records it lacks go there, once. */
    CHECK(logbook_set_disk(b, disk) == 0, "disk");
    char p[800];
    snprintf(p, sizeof p, "%s/operations.log", disk);
    CHECK(file_lines(p) == 6, "written: %d", file_lines(p));
    logbook_destroy(b);
    b = logbook_create(&c);
    logbook_set_flash(b, fl);                   /* the same 5, on the disk already */
    CHECK(logbook_set_disk(b, disk) == 0 && file_lines(p) == 6, "no doubles: %d", file_lines(p));
    n = logbook_query(b, NULL, 0, 60, out, NULL);
    CHECK(n == 6 && num(&out[0]) == 100, "the ring is the disk's: %d", n);
    logbook_destroy(b);

    /* flash_no_disk: the alarms' copy only while there is no disk. */
    logbook_conf_t a = c;
    a.name = "alarms";
    a.flash_no_disk = 1;
    snprintf(fl, sizeof fl, "%s/alarms.log", dir);
    b = logbook_create(&a);
    logbook_set_flash(b, fl);
    logbook_set_disk(b, disk);
    push_n(b, 0, 3);
    logbook_sync(b, 1, 1);
    CHECK(file_size(fl) < 0, "with a disk: no flash copy");
    logbook_set_disk(b, NULL);
    logbook_sync(b, 2, 1);
    CHECK(file_lines(fl) == 3, "without: %d", file_lines(fl));
    logbook_destroy(b);
}

static int find_op(const char *type, const char *needle, logrec_t *out)
{
    logrec_t r[LOGS_OPS_RAM];
    logfilter_t f = { .ch = -1, .type = type };
    int n = logbook_query(logs_ops(), &f, 0, LOGS_OPS_RAM, r, NULL);
    for (int i = 0; i < n; ++i)
        if (!needle || strstr(r[i].detail, needle)) {
            if (out) *out = r[i];
            return 1;
        }
    return 0;
}

static int ops_count(const char *type)
{
    logrec_t r[LOGS_OPS_RAM];
    logfilter_t f = { .ch = -1, .type = type };
    return logbook_query(logs_ops(), &f, 0, LOGS_OPS_RAM, r, NULL);
}

static void test_oplog(void)
{
    logrec_t r;
    oplog_push(NULL, NULL, "plain", "x");
    CHECK(find_op("plain", NULL, &r) && !strcmp(r.source, "system") && !r.who[0], "nobody: the system");
    oplog_actor("", "api", "10.0.0.5");
    oplog_login("anna", 1, 0, 1800);
    oplog_login("anna", 1, 0, 1800);
    CHECK(ops_count("login") == 1 && find_op("login", "from 10.0.0.5", &r) && !strcmp(r.who, "anna") &&
              !strcmp(r.source, "api"),
          "an API login once in its quiet time: %d", ops_count("login"));
    oplog_push(NULL, NULL, "reboot", NULL);
    CHECK(find_op("reboot", NULL, &r) && !strcmp(r.who, "anna") && !strcmp(r.source, "api"), "as the actor");
    oplog_actor("", "screen", "");
    oplog_login("bob", 1, 0, 0);
    oplog_login("bob", 1, 0, 0);
    CHECK(ops_count("login") == 3, "the screen's: each one");
    oplog_actor("", "rtsp", "10.0.0.9");
    oplog_login("mal\x01lory", 0, 4, 0);
    CHECK(find_op("login_failed", "from 10.0.0.9, 4 in a row", &r) && !strcmp(r.who, "mal?lory") &&
              !strcmp(r.source, "rtsp"),
          "a failure: %s %s", r.who, r.detail);
    oplog_lockout(5, 32);
    CHECK(find_op("lockout", "refused for 32 s after 5", &r) && !strcmp(r.source, "rtsp"), "lockout: %s", r.detail);
    oplog_actor(NULL, "system", "");
}

static void test_config_saved(void)
{
    char path[600];
    snprintf(path, sizeof path, "%s/vigil.yaml", g_dir);
    FILE *f = fopen(path, "w");
    fputs("version: 1\ncameras:\n  - url: \"rtsp://admin:s3cretpass@192.168.1.10/stream1\"\n    name: Gate\n", f);
    fclose(f);
    CHECK(config_init(path) == 0, "config");
    oplog_actor("root", "api", "10.0.0.5");
    schema_errors_t e = { 0 };
    const char *body = "{\"path\":\"/mnt/sda1/rec\",\"segment\":120}";
    CHECK(config_put_json("record", body, strlen(body), &e) == CONFIG_OK, "put record");
    logrec_t r;
    CHECK(find_op("config_saved", "record: path, segment", &r) && !strcmp(r.who, "root"), "fields: %s", r.detail);
    body = "[{\"url\":\"rtsp://admin:n3wsecret@192.168.1.10/stream1\",\"name\":\"Gate\"},"
           "{\"url\":\"rtsp://192.168.1.11/x\"}]";
    CHECK(config_put_json("cameras", body, strlen(body), &e) == CONFIG_OK, "put cameras: %s",
          e.n ? e.e[0].msg : "");
    CHECK(find_op("config_saved", "cameras: 1 -> 2 items; [1] url", &r), "cameras: %s", r.detail);
    /* No value of any secret, old or new, anywhere in the log. */
    logrec_t all[LOGS_OPS_RAM];
    int n = logbook_query(logs_ops(), NULL, 0, LOGS_OPS_RAM, all, NULL);
    int leak = 0;
    for (int i = 0; i < n; ++i) leak |= strstr(all[i].detail, "s3cret") || strstr(all[i].detail, "n3wsecret");
    CHECK(!leak, "no secret in the operation log");
    oplog_actor("", "system", "");
    unlink(path);
}

static void test_alarm_feed(void)
{
    char type[32];
    int a;
    CHECK(alarm_feed_classify(2, "down", type, sizeof type, &a) && !strcmp(type, "offline") && a == 1, "down");
    CHECK(alarm_feed_classify(2, "online", type, sizeof type, &a) && !strcmp(type, "offline") && a == 0, "online");
    CHECK(alarm_feed_classify(2, "REC failed", type, sizeof type, &a) && !strcmp(type, "rec_failed") && a == -1,
          "rec failed");
    CHECK(alarm_feed_classify(1, "REC waiting for disk", type, sizeof type, &a) && !strcmp(type, "rec_off") && a,
          "waiting for disk");
    CHECK(alarm_feed_classify(0, "disk sda1: format failed", type, sizeof type, &a) &&
              !strcmp(type, "disk_error"),
          "format failed");
    CHECK(!alarm_feed_classify(0, "config applied", type, sizeof type, &a) &&
              !alarm_feed_classify(3, "REC recording", type, sizeof type, &a),
          "nothing for the log");

    unsigned long before = logbook_count(logs_alarms());
    events_push(3, "online");               /* the first connection: no alarm */
    events_push(3, "down");
    events_push(3, "down");                 /* said once */
    events_push(3, "online");
    events_push(1, "REC disk full");
    events_push(1, "REC on");
    events_push(2, "REC waiting for disk");
    events_push(2, "REC on");
    alarm_feed_poll(0);
    logrec_t r[16];
    int n = logbook_query(logs_alarms(), NULL, 0, 16, r, NULL);
    CHECK(logbook_count(logs_alarms()) - before == 6, "six alarms: %lu", logbook_count(logs_alarms()) - before);
    CHECK(n >= 6 && !strcmp(r[5].type, "offline") && r[5].active == 1 && r[5].ch == 3 &&
              !strcmp(r[4].type, "offline") && r[4].active == 0 && !strcmp(r[3].type, "disk_full") &&
              r[3].active == 1 && !strcmp(r[2].type, "disk_full") && r[2].active == 0 &&
              !strcmp(r[1].type, "rec_off") && r[1].active == 1 && !strcmp(r[1].detail, "waiting for disk") &&
              !strcmp(r[0].type, "rec_off") && r[0].active == 0,
          "in order: %s/%d %s/%d %s/%d", r[5].type, r[5].active, r[3].type, r[3].active, r[1].type, r[1].active);
    alarmlog_push(0, ALARM_DISK_SMART, 1, "sda: failing");
    n = logbook_query(logs_alarms(), NULL, 0, 1, r, NULL);
    CHECK(n == 1 && !strcmp(r[0].type, "disk_smart") && r[0].ch == 0, "pushed directly");
}

static void test_disk_dir(void)
{
    char rec[600], out[700], want[700];
    snprintf(rec, sizeof rec, "%s/mnt/rec", g_dir);
    rm_rf(rec);
    CHECK(logs_disk_dir(rec, 1, out, sizeof out) < 0, "not until the recording directory is there");
    char up[600];
    snprintf(up, sizeof up, "%s/mnt", g_dir);
    mkdir(up, 0755);
    mkdir(rec, 0755);
    snprintf(want, sizeof want, "%s/mnt/vigil-logs", g_dir);
    CHECK(logs_disk_dir(rec, 1, out, sizeof out) == 0 && !strcmp(out, want), "beside it: %s", out);
    CHECK(logs_disk_dir("", 1, out, sizeof out) < 0 && logs_disk_dir("rel", 1, out, sizeof out) < 0, "none");
    /* The whole thread once, on that directory: a record pushed reaches it. */
    char *path = config_path() ? strdup(config_path()) : NULL;
    snprintf(up, sizeof up, "%s/vigil2.yaml", g_dir);
    FILE *f = fopen(up, "w");
    fprintf(f, "version: 1\nrecord:\n  path: \"%s\"\n  allow_rootfs: true\n", rec);
    fclose(f);
    CHECK(config_init(up) == 0, "config with record.path");
    char flash[600];
    snprintf(flash, sizeof flash, "%s/etc-vigil-logs", g_dir);
    rm_rf(flash);
    rm_rf(want);
    CHECK(logs_start(flash, NULL) == 0, "start");
    oplog_push("root", "api", "disk_format", "sdz1 as ext4");
    logs_stop();
    char p[800];
    snprintf(p, sizeof p, "%s/operations.log", want);
    char *t = slurp(p);
    CHECK(t && strstr(t, "\tdisk_format\tsdz1 as ext4\n"), "on the disk");
    free(t);
    snprintf(p, sizeof p, "%s/operations-recent.log", flash);
    t = slurp(p);
    CHECK(t && strstr(t, "\tdisk_format\tsdz1 as ext4\n"), "and on the flash at the stop");
    free(t);
    unlink(up);
    free(path);
}

int main(int argc, char **argv)
{
    char tmpl[600];
    snprintf(tmpl, sizeof tmpl, "%s/logs-XXXXXX", argc > 1 ? argv[1] : ".");
    if (!mkdtemp(tmpl)) {
        perror(tmpl);
        return 1;
    }
    if (!realpath(tmpl, g_dir)) return 1;     /* record.path is absolute */
    char key[700];
    snprintf(key, sizeof key, "%s/vigil.key", g_dir);
    secret_set_key_path(key);
    setenv("TZ", "UTC", 1);
    test_formats();
    test_ram();
    test_disk();
    test_flash();
    test_oplog();
    test_config_saved();
    test_alarm_feed();
    test_disk_dir();
    rm_rf(g_dir);
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
