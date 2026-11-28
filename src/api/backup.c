/* SPDX-License-Identifier: GPL-3.0-or-later */
/* backup.c — configuration export and import, and the resets (see backup.h). */
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include "backup.h"
#include "cJSON.h"
#include "config.h"
#include "events.h"
#include "log.h"
#include "mgmt.h"
#include "secret.h"
#include "sys.h"
#include "sys_nas.h"
#include "sys_net.h"
#include "sys_time.h"
#include "tar.h"
#include "users.h"

#ifndef VIGIL_VERSION
#define VIGIL_VERSION "dev"
#endif

#define ETH          "eth0"
#define F_MANIFEST   "manifest.json"
#define F_CONFIG     "vigil.yaml"
#define F_USERS      "vigil.users.yaml"
#define F_SYSTEM     "system.json"
#define CHECK_TEXT   "vigil-config"
#define KDF_NAME     "PBKDF2-HMAC-SHA256"
#define KDF_MIN      100000u        /* what an export uses at least */
#define KDF_READ_MIN 1000u          /* what an import accepts: a hand-made file */
#define KDF_READ_MAX 10000000u      /* ... and at most: tens of seconds on an A7 */
#define SALT_LEN     16
#define IFACES_DIR   "/etc/network/interfaces.d"
#define ROM_IFACES   "/rom/etc/network/interfaces.d"

static pthread_mutex_t g_busy = PTHREAD_MUTEX_INITIALIZER;
static char            g_platform[48] = "generic";
static char            g_defaults[256];         /* "": /rom + config_path() */
static unsigned        g_kdf_ms = 1000;

void backup_set_platform(const char *name) { snprintf(g_platform, sizeof g_platform, "%s", name ? name : ""); }
void backup_set_defaults(const char *path) { snprintf(g_defaults, sizeof g_defaults, "%s", path ? path : ""); }
void backup_set_kdf_ms(unsigned ms) { g_kdf_ms = ms; }

/* ---- small helpers ---------------------------------------------------------------- */

static void line(backup_summary_t *s, int kind, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void line(backup_summary_t *s, int kind, const char *fmt, ...)
{
    if (s->n_lines >= BACKUP_LINES) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->line[s->n_lines], sizeof s->line[0], fmt, ap);
    va_end(ap);
    s->kind[s->n_lines++] = kind;
}

static void sha256_hex(const void *data, size_t len, char out[65])
{
    unsigned char d[32];
    mbedtls_sha256(data, len, d, 0);
    for (int i = 0; i < 32; ++i) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

/* KEY=value of /etc/os-release (quotes taken off), "" when none. */
static void os_release(const char *key, char *out, size_t cap)
{
    char buf[2048];
    out[0] = 0;
    if (sys_read("/etc/os-release", buf, sizeof buf) < 0) return;
    size_t kl = strlen(key);
    for (char *l = buf; l && *l;) {
        char *nl = strchr(l, '\n');
        if (nl) *nl = 0;
        if (!strncmp(l, key, kl) && l[kl] == '=') {
            char *v = l + kl + 1;
            size_t n = strlen(v);
            if (n >= 2 && v[0] == '"' && v[n - 1] == '"') {
                v[n - 1] = 0;
                ++v;
            }
            snprintf(out, cap, "%s", v);
            return;
        }
        l = nl ? nl + 1 : NULL;
    }
}

/* "1.2.3" against "1.10": < 0, 0, > 0. */
static int version_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        char *ea, *eb;
        long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (ea == a || eb == b) return strcmp(a, b);
        if (x != y) return x < y ? -1 : 1;
        a = *ea == '.' ? ea + 1 : ea;
        b = *eb == '.' ? eb + 1 : eb;
        if ((*a && (*a < '0' || *a > '9')) || (*b && (*b < '0' || *b > '9'))) return strcmp(a, b);
    }
    return 0;
}

/* A whole small file (under the real root) into a malloc'd buffer. */
static char *slurp(const char *path, size_t max, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(max + 1);
    size_t n = buf ? fread(buf, 1, max + 1, f) : 0;
    int bad = !buf || ferror(f) || n > max;
    fclose(f);
    if (bad) {
        free(buf);
        errno = bad && buf ? EFBIG : ENOMEM;
        return NULL;
    }
    buf[n] = 0;
    if (len) *len = n;
    return buf;
}

static void wipe_free(char *p, size_t n)
{
    if (p) secret_wipe(p, n);
    free(p);
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}

/* ---- the export's key, and its passwords ------------------------------------------- */

typedef struct {
    int           secrets;          /* sealed with `key`; 0: left out */
    unsigned char key[SECRET_KEY_LEN];
    int           cam_pw, other_pw; /* passwords met on the way out */
    int           failed;           /* one did not open with `key` */
    int           foreign;          /* one sealed with a recorder's own key */
} xkey_t;

static void xkey_wipe(xkey_t *x) { secret_wipe(x, sizeof *x); }

static int seal_out(void *ctx, const char *aad, const char *in, char *out, size_t cap)
{
    xkey_t *x = ctx;
    if (!strncmp(aad, "config:cameras[", 15)) ++x->cam_pw;
    else if (!strncmp(aad, "config:", 7)) ++x->other_pw;
    if (!x->secrets) {
        out[0] = 0;
        return 0;
    }
    return secret_seal_with(x->key, aad, in, out, cap) == SECRET_OK ? 0 : -1;
}

static int open_in(void *ctx, const char *aad, const char *in, char *out, size_t cap)
{
    xkey_t *x = ctx;
    if (secret_is_export_sealed(in)) {
        if (x->secrets && secret_open_with(x->key, aad, in, out, cap) == SECRET_OK) return 0;
        x->failed = 1;
        return -1;
    }
    if (secret_is_sealed(in)) {
        x->foreign = 1;
        return -1;
    }
    /* In the clear: typed into the file by hand. It is sealed with this
     * board's key as it is saved. */
    if (strlen(in) >= cap) return -1;
    snprintf(out, cap, "%s", in);
    return 0;
}

/* Iterations that take about g_kdf_ms here: measured on a short run. */
static unsigned kdf_iterations(void)
{
    unsigned char salt[SALT_LEN] = { 0 }, key[SECRET_KEY_LEN];
    const unsigned probe = 4096;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    secret_derive("calibration", salt, sizeof salt, probe, key);
    clock_gettime(CLOCK_MONOTONIC, &b);
    secret_wipe(key, sizeof key);
    double ms = (double)(b.tv_sec - a.tv_sec) * 1e3 + (double)(b.tv_nsec - a.tv_nsec) / 1e6;
    if (ms < 0.01) ms = 0.01;
    double it = probe * (double)g_kdf_ms / ms;
    if (it < KDF_MIN) it = KDF_MIN;
    if (it > KDF_READ_MAX / 2) it = KDF_READ_MAX / 2;
    return (unsigned)it / 1000 * 1000;
}

/* ---- export ------------------------------------------------------------------------ */

static cJSON *system_doc(xkey_t *x, int *wifi_pw, char *err, size_t cap)
{
    cJSON *d = cJSON_CreateObject();
    char host[64];
    host_get(host, sizeof host);
    cJSON_AddStringToObject(d, "hostname", host);
    sys_clock_t c;
    clock_read(&c);
    cJSON *tz = cJSON_AddObjectToObject(d, "timezone");
    cJSON_AddStringToObject(tz, "zone", c.zone);
    cJSON_AddStringToObject(tz, "tz", c.tz);
    cJSON *ntp = cJSON_AddArrayToObject(d, "ntp_servers");
    for (int i = 0; i < c.n_servers; ++i) cJSON_AddItemToArray(ntp, cJSON_CreateString(c.servers[i]));
    net_cfg_t n;
    char e[160];
    if (net_read(ETH, &n, e, sizeof e) == 0) {
        cJSON *ifs = cJSON_AddObjectToObject(d, "interfaces");
        cJSON *o = cJSON_AddObjectToObject(ifs, ETH);
        cJSON_AddBoolToObject(o, "dhcp", n.dhcp);
        cJSON_AddStringToObject(o, "address", n.address);
        cJSON_AddStringToObject(o, "netmask", n.netmask);
        cJSON_AddStringToObject(o, "gateway", n.gateway);
        cJSON_AddStringToObject(o, "dns1", n.dns1);
        cJSON_AddStringToObject(o, "dns2", n.dns2);
        cJSON_AddNumberToObject(o, "mtu", n.mtu);
    }
    wifi_env_t w;
    wifi_env_get(&w);
    cJSON *wo = cJSON_AddObjectToObject(d, "wifi");
    cJSON_AddStringToObject(wo, "ssid", w.ssid);
    cJSON_AddStringToObject(wo, "wlandev", w.profile);
    char sealed[SECRET_SEALED_SIZE(sizeof w.password)] = "";
    *wifi_pw = w.password[0] != 0;
    if (w.password[0] && x->secrets &&
        secret_seal_with(x->key, "system:wifi.password", w.password, sealed, sizeof sealed) != SECRET_OK) {
        snprintf(err, cap, "cannot seal the Wi-Fi passphrase for the export");
        cJSON_Delete(d);
        d = NULL;
    }
    if (d) cJSON_AddStringToObject(wo, "password", sealed);
    secret_wipe(&w, sizeof w);
    return d;
}

static int export_locked(const char *password, char **out, size_t *len, char *name, size_t ncap, char *err,
                         size_t cap)
{
    xkey_t x = { 0 };
    unsigned char salt[SALT_LEN];
    unsigned iter = 0;
    x.secrets = password && *password;
    if (x.secrets) {
        if (strlen(password) < 4) {
            snprintf(err, cap, "the export's password needs at least 4 characters");
            return BACKUP_EINVAL;
        }
        iter = kdf_iterations();
        if (secret_random(salt, sizeof salt) < 0 || secret_derive(password, salt, sizeof salt, iter, x.key) < 0) {
            snprintf(err, cap, "cannot derive a key from the password");
            return BACKUP_EIO;
        }
    }
    int rc = BACKUP_EIO;
    char *cfg_text = NULL, *users_text = NULL, *sys_text = NULL, *man_text = NULL;
    cJSON *sys = NULL, *man = NULL;
    tar_out_t tar = { 0 };
    vigil_config_t *c = malloc(sizeof *c);
    if (!c) {
        snprintf(err, cap, "out of memory");
        goto out;
    }
    config_get(c);
    int n_cams = c->n_cameras;
    if (!(cfg_text = config_export_text(c, seal_out, &x, err, cap))) goto out;
    secret_wipe(c, sizeof *c);
    int n_users = 0, n_upw = 0, root = 0;
    if (!(users_text = users_export_text(x.secrets ? seal_out : NULL, &x, &n_users, &n_upw, &root, err, cap)))
        goto out;
    int managed = sys_managed(), wifi_pw = 0;
    if (managed) {
        if (!(sys = system_doc(&x, &wifi_pw, err, cap))) goto out;
        if (!(sys_text = cJSON_Print(sys))) goto oom;
    }

    char host[64], mac[18] = "", created[32], board[64], fw[64], build[64];
    host_get(host, sizeof host);
    net_status_t ns;
    net_status(ETH, &ns);
    snprintf(mac, sizeof mac, "%s", ns.mac);
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(created, sizeof created, "%Y-%m-%dT%H:%M:%SZ", &tm);
    os_release("BUILD_PLATFORM", board, sizeof board);
    os_release("OPENIPC_VERSION", fw, sizeof fw);
    os_release("BUILD_ID", build, sizeof build);

    man = cJSON_CreateObject();
    cJSON_AddStringToObject(man, "format", BACKUP_FORMAT);
    cJSON_AddNumberToObject(man, "format_version", BACKUP_FORMAT_VERSION);
    cJSON_AddStringToObject(man, "vigil_version", VIGIL_VERSION);
    cJSON_AddNumberToObject(man, "config_version", CONFIG_VERSION);
    cJSON_AddStringToObject(man, "platform", g_platform);
    cJSON_AddStringToObject(man, "board", board);
    cJSON_AddStringToObject(man, "firmware", fw);
    cJSON_AddStringToObject(man, "build", build);
    cJSON_AddStringToObject(man, "hostname", host);
    cJSON_AddStringToObject(man, "mac", mac);
    cJSON_AddStringToObject(man, "created", created);
    cJSON_AddStringToObject(man, "secrets", x.secrets ? "password" : "none");
    struct { const char *name, *text; } files[] = {
        { F_CONFIG, cfg_text }, { F_USERS, users_text }, { F_SYSTEM, sys_text },
    };
    char digest[512] = "";
    cJSON *fl = cJSON_AddArrayToObject(man, "files");
    for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i) {
        if (!files[i].text) continue;
        char h[65];
        sha256_hex(files[i].text, strlen(files[i].text), h);
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "name", files[i].name);
        cJSON_AddNumberToObject(f, "size", (double)strlen(files[i].text));
        cJSON_AddStringToObject(f, "sha256", h);
        cJSON_AddItemToArray(fl, f);
        size_t dl = strlen(digest);
        snprintf(digest + dl, sizeof digest - dl, "%s %s\n", files[i].name, h);
    }
    if (x.secrets) {
        char b64[48], check[SECRET_SEALED_SIZE(sizeof CHECK_TEXT)], seal[SECRET_SEALED_SIZE(sizeof digest)];
        size_t bl = 0;
        mbedtls_base64_encode((unsigned char *)b64, sizeof b64, &bl, salt, sizeof salt);
        b64[bl] = 0;
        cJSON *k = cJSON_AddObjectToObject(man, "kdf");
        cJSON_AddStringToObject(k, "algorithm", KDF_NAME);
        cJSON_AddNumberToObject(k, "iterations", iter);
        cJSON_AddStringToObject(k, "salt", b64);
        cJSON_AddStringToObject(man, "cipher", "AES-256-GCM");
        if (secret_seal_with(x.key, "export:check", CHECK_TEXT, check, sizeof check) != SECRET_OK ||
            secret_seal_with(x.key, "export:files", digest, seal, sizeof seal) != SECRET_OK) {
            snprintf(err, cap, "cannot seal the manifest");
            goto out;
        }
        cJSON_AddStringToObject(man, "check", check);
        cJSON_AddStringToObject(man, "files_sealed", seal);
    }
    cJSON *inc = cJSON_AddObjectToObject(man, "included");
    cJSON_AddNumberToObject(inc, "cameras", n_cams);
    cJSON_AddNumberToObject(inc, "camera_passwords", x.secrets ? x.cam_pw : 0);
    cJSON_AddNumberToObject(inc, "service_passwords", x.secrets ? x.other_pw : 0);
    cJSON_AddNumberToObject(inc, "users", n_users);
    cJSON_AddNumberToObject(inc, "user_passwords", n_upw);
    cJSON_AddBoolToObject(inc, "root_password", root);
    cJSON_AddBoolToObject(inc, "system", managed);
    cJSON_AddBoolToObject(inc, "wifi_password", x.secrets && wifi_pw);
    if (!x.secrets) {
        cJSON *lo = cJSON_AddObjectToObject(man, "left_out");
        cJSON_AddNumberToObject(lo, "camera_passwords", x.cam_pw);
        cJSON_AddNumberToObject(lo, "service_passwords", x.other_pw);
        cJSON_AddBoolToObject(lo, "wifi_password", wifi_pw);
    }
    cJSON *not = cJSON_AddArrayToObject(man, "not_included");
    static const char *const never[] = {
        "recordings", "logs", "vigil.key (the recorder's own key)", "SSH host keys and authorized keys",
        "the U-Boot environment other than wlanssid, wlanpass and wlandev",
        "which recorder owns the recording disks", "crypt(3) password hashes (made again on import)",
    };
    for (size_t i = 0; i < sizeof never / sizeof never[0]; ++i)
        cJSON_AddItemToArray(not, cJSON_CreateString(never[i]));
    if (!(man_text = cJSON_Print(man))) goto oom;

    if (tar_add(&tar, F_MANIFEST, man_text, strlen(man_text), now) < 0) goto oom;
    for (size_t i = 0; i < sizeof files / sizeof files[0]; ++i)
        if (files[i].text && tar_add(&tar, files[i].name, files[i].text, strlen(files[i].text), now) < 0) goto oom;
    if (tar_finish(&tar) < 0) goto oom;
    struct tm lt;
    localtime_r(&now, &lt);
    char date[20];
    strftime(date, sizeof date, "%Y%m%d-%H%M", &lt);
    snprintf(name, ncap, "vigil-config-%s-%s.tar", host[0] ? host : "recorder", date);
    *out = tar.data;
    *len = tar.len;
    tar.data = NULL;
    rc = BACKUP_OK;
    vg_log("configuration exported (%s): %d camera(s), %d user(s)%s", x.secrets ? "passwords sealed" : "no passwords",
           n_cams, n_users, root ? ", root's password" : "");
    goto out;
oom:
    snprintf(err, cap, "out of memory");
out:
    if (c) secret_wipe(c, sizeof *c);
    free(c);
    if (cfg_text) wipe_free(cfg_text, strlen(cfg_text));
    if (users_text) wipe_free(users_text, strlen(users_text));
    if (sys_text) wipe_free(sys_text, strlen(sys_text));
    free(man_text);
    cJSON_Delete(sys);
    cJSON_Delete(man);
    tar_out_free(&tar);
    xkey_wipe(&x);
    return rc;
}

int backup_export(const char *password, char **out, size_t *len, char *name, size_t ncap, char *err, size_t cap)
{
    *out = NULL;
    *len = 0;
    if (pthread_mutex_trylock(&g_busy) != 0) {
        snprintf(err, cap, "another export, import or reset is under way");
        return BACKUP_EBUSY;
    }
    int rc = export_locked(password, out, len, name, ncap, err, cap);
    pthread_mutex_unlock(&g_busy);
    return rc;
}

/* `data` as the new file `path` (under the root): written beside it and
 * renamed, so a pulled stick has the old file or the whole new one. Not
 * fchmod'd: FAT refuses modes, and mkstemp makes the file 0600. */
static int write_file(const char *path, const char *data, size_t len, char *err, size_t cap)
{
    char p[512], tmp[540];
    sys_path(p, sizeof p, path);
    snprintf(tmp, sizeof tmp, "%s.XXXXXX", p);
    int fd = mkstemp(tmp);
    if (fd < 0) {
        snprintf(err, cap, "cannot write to %.200s: %s", path, strerror(errno));
        return -1;
    }
    int ok = 1;
    for (size_t done = 0; ok && done < len;) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = 0;
        else done += (size_t)n;
    }
    if (ok) ok = fsync(fd) == 0;
    int e = errno;
    if (close(fd) < 0 && ok) {
        ok = 0;
        e = errno;
    }
    if (!ok || rename(tmp, p) < 0) {
        if (ok) e = errno;
        unlink(tmp);
        snprintf(err, cap, "cannot write %.200s: %s", path, strerror(e));
        return -1;
    }
    char dir[512];
    snprintf(dir, sizeof dir, "%s", p);
    int dfd = open(dirname(dir), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return 0;
}

int backup_export_to(const char *dir, const char *password, char *path, size_t pcap, char *err, size_t cap)
{
    path[0] = 0;
    backup_place_t pl[16];
    int n = backup_places(pl, 16), ok = 0;
    for (int i = 0; i < n && !ok; ++i) ok = !strcmp(pl[i].path, dir);
    if (!ok) {
        snprintf(err, cap, "%.120s is not a mounted disk vigil can write to", dir);
        return BACKUP_EINVAL;
    }
    char *data = NULL, name[128];
    size_t len = 0;
    int rc = backup_export(password, &data, &len, name, sizeof name, err, cap);
    if (rc != BACKUP_OK) return rc;
    snprintf(path, pcap, "%s/%s", dir, name);
    if (write_file(path, data, len, err, cap) < 0) rc = BACKUP_EIO;
    else vg_log("configuration export written to %s", path);
    wipe_free(data, len);
    if (rc != BACKUP_OK) path[0] = 0;
    return rc;
}

int backup_places(backup_place_t *out, int max)
{
    sys_disk_t *d = calloc(SYS_DISK_MAX, sizeof *d);
    int nd = d ? mgmt_disks(d, SYS_DISK_MAX) : 0, n = 0;
    for (int i = 0; i < nd && n < max; ++i) {
        if (!d[i].mnt[0] || d[i].ro || d[i].system || strncmp(d[i].mnt, "/mnt/", 5)) continue;
        backup_place_t *p = &out[n++];
        char free_s[32];
        unsigned long long a = d[i].avail;
        if (a >= 1ull << 30) snprintf(free_s, sizeof free_s, "%.1f GB free", (double)a / (1 << 30));
        else snprintf(free_s, sizeof free_s, "%llu MB free", a >> 20);
        snprintf(p->path, sizeof p->path, "%s", d[i].mnt);
        snprintf(p->label, sizeof p->label, "%.16s  %.8s  %.20s%s%.50s", d[i].name, d[i].fs, free_s,
                 d[i].model[0] ? "  " : "", d[i].model);
    }
    free(d);
    if (n < max && sys_nas_mounted(MGMT_NAS_MOUNT)) {
        snprintf(out[n].path, sizeof out[n].path, "%s", MGMT_NAS_MOUNT);
        snprintf(out[n].label, sizeof out[n].label, "network storage");
        ++n;
    }
    return n;
}

static int newest_first(const void *a, const void *b)
{
    const backup_file_t *x = a, *y = b;
    return x->mtime < y->mtime ? 1 : x->mtime > y->mtime ? -1 : strcmp(x->name, y->name);
}

int backup_files(backup_file_t *out, int max)
{
    backup_place_t pl[16];
    int np = backup_places(pl, 16), n = 0;
    for (int i = 0; i < np && n < max; ++i) {
        char p[512];
        DIR *dir = opendir(sys_path(p, sizeof p, pl[i].path));
        if (!dir) continue;
        struct dirent *e;
        while ((e = readdir(dir)) != NULL && n < max) {
            size_t l = strlen(e->d_name);
            if (strncmp(e->d_name, "vigil-config", 12) || l < 17 || strcmp(e->d_name + l - 4, ".tar") || l >= 100)
                continue;
            char rel[256], full[512];
            if (snprintf(rel, sizeof rel, "%.127s/%.99s", pl[i].path, e->d_name) >= (int)sizeof rel) continue;
            struct stat st;
            if (stat(sys_path(full, sizeof full, rel), &st) < 0 || !S_ISREG(st.st_mode) ||
                st.st_size > (off_t)BACKUP_FILE_MAX)
                continue;
            backup_file_t *f = &out[n++];
            memcpy(f->path, rel, sizeof f->path);
            memcpy(f->name, e->d_name, l + 1);          /* l < 100 */
            f->size = st.st_size;
            f->mtime = st.st_mtime;
        }
        closedir(dir);
    }
    qsort(out, (size_t)n, sizeof *out, newest_first);
    return n;
}

/* ---- import ------------------------------------------------------------------------ */

typedef struct {
    int          has;               /* system.json is there and this system is managed */
    char         host[64];
    char         zone[64], tz[96];
    char         ntp[4][64];
    int          n_ntp, has_ntp;
    int          has_eth;
    net_cfg_t    eth;
    int          has_wifi;
    wifi_env_t   wifi;
} sysimp_t;

/* system.json into `si`, every value checked. 0, or -1 with `err`. */
static int read_system(const char *text, size_t len, xkey_t *x, sysimp_t *si, char *err, size_t cap)
{
    cJSON *d = cJSON_ParseWithLength(text, len);
    if (!cJSON_IsObject(d)) {
        cJSON_Delete(d);
        snprintf(err, cap, F_SYSTEM ": not a JSON object");
        return -1;
    }
    int rc = -1;
    char e[160];
    const char *h = jstr(d, "hostname");
    if (*h && host_check(h, e, sizeof e) < 0) {
        snprintf(err, cap, F_SYSTEM ": hostname: %s", e);
        goto out;
    }
    snprintf(si->host, sizeof si->host, "%s", h);
    const cJSON *tz = cJSON_GetObjectItemCaseSensitive(d, "timezone");
    snprintf(si->zone, sizeof si->zone, "%s", jstr(tz, "zone"));
    snprintf(si->tz, sizeof si->tz, "%s", jstr(tz, "tz"));
    if (si->zone[0] && !tz_find(si->zone)) si->zone[0] = 0;     /* "custom", or a zone not offered here */
    tz_rule_t r;
    if (!si->zone[0] && si->tz[0] && tz_parse(si->tz, &r) < 0) {
        snprintf(err, cap, F_SYSTEM ": timezone: '%.60s' is not a POSIX time zone vigil can set", si->tz);
        goto out;
    }
    const cJSON *ntp = cJSON_GetObjectItemCaseSensitive(d, "ntp_servers");
    si->has_ntp = cJSON_IsArray(ntp);
    for (const cJSON *v = si->has_ntp ? ntp->child : NULL; v; v = v->next) {
        if (!cJSON_IsString(v) || !ntp_server_ok(v->valuestring) || si->n_ntp >= 4 ||
            strlen(v->valuestring) >= sizeof si->ntp[0]) {
            snprintf(err, cap, F_SYSTEM ": ntp_servers: up to 4 host names or addresses");
            goto out;
        }
        snprintf(si->ntp[si->n_ntp++], sizeof si->ntp[0], "%s", v->valuestring);
    }
    const cJSON *ifs = cJSON_GetObjectItemCaseSensitive(d, "interfaces");
    const cJSON *eth = cJSON_GetObjectItemCaseSensitive(ifs, ETH);
    if (cJSON_IsObject(eth)) {
        net_cfg_t *n = &si->eth;
        snprintf(n->ifname, sizeof n->ifname, ETH);
        n->dhcp = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(eth, "dhcp"));
        snprintf(n->address, sizeof n->address, "%s", jstr(eth, "address"));
        snprintf(n->netmask, sizeof n->netmask, "%s", jstr(eth, "netmask"));
        snprintf(n->gateway, sizeof n->gateway, "%s", jstr(eth, "gateway"));
        snprintf(n->dns1, sizeof n->dns1, "%s", jstr(eth, "dns1"));
        snprintf(n->dns2, sizeof n->dns2, "%s", jstr(eth, "dns2"));
        const cJSON *mtu = cJSON_GetObjectItemCaseSensitive(eth, "mtu");
        n->mtu = cJSON_IsNumber(mtu) ? mtu->valueint : 0;
        char field[32];
        if (net_validate(n, field, sizeof field, e, sizeof e) < 0) {
            snprintf(err, cap, F_SYSTEM ": " ETH ".%s: %s", field, e);
            goto out;
        }
        si->has_eth = 1;
    }
    const cJSON *w = cJSON_GetObjectItemCaseSensitive(d, "wifi");
    if (cJSON_IsObject(w)) {
        wifi_env_t *we = &si->wifi;
        const char *ssid = jstr(w, "ssid"), *prof = jstr(w, "wlandev"), *pw = jstr(w, "password");
        if (strlen(ssid) > 32 || strlen(prof) >= sizeof we->profile ||
            strspn(prof, "abcdefghijklmnopqrstuvwxyz0123456789-_.") != strlen(prof)) {
            snprintf(err, cap, F_SYSTEM ": wifi: not an SSID and a profile vigil can set");
            goto out;
        }
        snprintf(we->ssid, sizeof we->ssid, "%s", ssid);
        snprintf(we->profile, sizeof we->profile, "%s", prof);
        if (*pw && open_in(x, "system:wifi.password", pw, we->password, sizeof we->password) < 0) {
            snprintf(err, cap, F_SYSTEM ": the Wi-Fi passphrase does not open: the file was changed");
            goto out;
        }
        size_t pl = strlen(we->password);
        if (pl && pl != 64 && (pl < 8 || pl > 63)) {
            snprintf(err, cap, F_SYSTEM ": the Wi-Fi passphrase is not 8 to 63 characters");
            goto out;
        }
        si->has_wifi = 1;
    }
    si->has = 1;
    rc = 0;
out:
    cJSON_Delete(d);
    return rc;
}

static void net_words(const net_cfg_t *n, char *out, size_t cap)
{
    if (n->dhcp) snprintf(out, cap, "DHCP");
    else snprintf(out, cap, "static %s/%s%s%s", n->address, n->netmask, n->gateway[0] ? " via " : "", n->gateway);
}

static int net_same(const net_cfg_t *a, const net_cfg_t *b)
{
    if (a->dhcp != b->dhcp || a->mtu != b->mtu) return 0;
    if (a->dhcp) return 1;
    return !strcmp(a->address, b->address) && !strcmp(a->netmask, b->netmask) && !strcmp(a->gateway, b->gateway) &&
           !strcmp(a->dns1, b->dns1) && !strcmp(a->dns2, b->dns2);
}

/* What the system part changes, into the summary. */
static void system_summary(const sysimp_t *si, const backup_import_opts_t *o, backup_summary_t *s)
{
    host_get(s->host_now, sizeof s->host_now);
    snprintf(s->host_new, sizeof s->host_new, "%s", si->host);
    s->hostname_change = si->host[0] && strcmp(si->host, s->host_now) != 0;
    if (s->hostname_change)
        line(s, 0, "Host name: %s (now %s)%s", si->host, s->host_now, o->hostname ? "" : "; kept, as asked");
    if (!o->hostname) s->hostname_change = 0;
    sys_clock_t c;
    clock_read(&c);
    s->tz_change = (si->zone[0] || si->tz[0]) && (strcmp(si->zone, c.zone) || strcmp(si->tz, c.tz));
    if (s->tz_change) line(s, 0, "Time zone: %s (now %s)", si->zone[0] ? si->zone : si->tz, c.zone[0] ? c.zone : c.tz);
    s->ntp_change = si->has_ntp && si->n_ntp && si->n_ntp != c.n_servers;
    for (int i = 0; si->has_ntp && i < si->n_ntp && !s->ntp_change; ++i)
        s->ntp_change = strcmp(si->ntp[i], c.servers[i]) != 0;
    if (s->ntp_change) line(s, 0, "NTP servers: %s%s%s", si->ntp[0], si->n_ntp > 1 ? ", ..." : "", "");
    if (si->has_eth) {
        net_cfg_t cur;
        char e[160], now_w[96], new_w[96];
        if (net_read(ETH, &cur, e, sizeof e) < 0) memset(&cur, 0, sizeof cur), cur.dhcp = 1;
        s->net_change = !net_same(&si->eth, &cur);
        net_status_t st;
        net_status(ETH, &st);
        if (s->net_change) {
            net_words(&si->eth, new_w, sizeof new_w);
            net_words(&cur, now_w, sizeof now_w);
            line(s, 0, "Ethernet: %s (now %s), from the next boot", new_w, now_w);
            if (!si->eth.dhcp && strcmp(si->eth.address, st.address)) {
                s->net_cuts = 1;
                line(s, 1, "After the reboot the recorder is at %s, not %s: it can only be reached there.",
                     si->eth.address, st.address[0] ? st.address : "the address it has now");
            } else if (si->eth.dhcp && !cur.dhcp) {
                s->net_cuts = 1;
                line(s, 1, "After the reboot the recorder takes its address from DHCP: it may no longer be at %s.",
                     st.address[0] ? st.address : cur.address);
            }
        }
    }
    if (si->has_wifi) {
        wifi_env_t cur;
        wifi_env_get(&cur);
        s->wifi_change = strcmp(cur.ssid, si->wifi.ssid) || strcmp(cur.profile, si->wifi.profile) ||
                         (si->wifi.password[0] && strcmp(cur.password, si->wifi.password));
        if (s->wifi_change)
            line(s, 0, "Wi-Fi: %s%s, from the next boot", si->wifi.ssid[0] ? si->wifi.ssid : "none",
                 si->wifi.ssid[0] && !si->wifi.password[0] && !s->secrets ? " (its passphrase left out of the export)"
                                                                          : "");
        secret_wipe(&cur, sizeof cur);
    }
    s->reboot = s->net_change || s->wifi_change;
}

typedef struct {
    const tar_file_t *man, *cfg, *users, *sys;
    tar_file_t        files[TAR_FILES_MAX];
    int               n;
} pack_t;

/* The archive's files, its manifest checked against them. */
static int read_pack(const void *data, size_t len, pack_t *p, cJSON **man, backup_summary_t *s, char *err,
                     size_t cap)
{
    *man = NULL;
    if (len > BACKUP_FILE_MAX) {
        snprintf(err, cap, "too large for a configuration export");
        return BACKUP_EINVAL;
    }
    p->n = tar_read(data, len, p->files, TAR_FILES_MAX, err, cap);
    if (p->n < 0) return BACKUP_EINVAL;
    p->man = tar_find(p->files, p->n, F_MANIFEST);
    p->cfg = tar_find(p->files, p->n, F_CONFIG);
    p->users = tar_find(p->files, p->n, F_USERS);
    p->sys = tar_find(p->files, p->n, F_SYSTEM);
    if (!p->man || !p->cfg) {
        snprintf(err, cap, "not a vigil configuration export (no %s)", p->man ? F_CONFIG : F_MANIFEST);
        return BACKUP_EINVAL;
    }
    cJSON *m = cJSON_ParseWithLength(p->man->data, p->man->len);
    if (!cJSON_IsObject(m) || strcmp(jstr(m, "format"), BACKUP_FORMAT)) {
        cJSON_Delete(m);
        snprintf(err, cap, "not a vigil configuration export (its manifest says otherwise)");
        return BACKUP_EINVAL;
    }
    *man = m;
    snprintf(s->from_host, sizeof s->from_host, "%s", jstr(m, "hostname"));
    snprintf(s->from_mac, sizeof s->from_mac, "%s", jstr(m, "mac"));
    snprintf(s->from_version, sizeof s->from_version, "%s", jstr(m, "vigil_version"));
    snprintf(s->from_board, sizeof s->from_board, "%s", jstr(m, "board"));
    snprintf(s->created, sizeof s->created, "%s", jstr(m, "created"));
    s->secrets = !strcmp(jstr(m, "secrets"), "password");
    const cJSON *fv = cJSON_GetObjectItemCaseSensitive(m, "format_version");
    const cJSON *cv = cJSON_GetObjectItemCaseSensitive(m, "config_version");
    if (!cJSON_IsNumber(fv) || fv->valuedouble > BACKUP_FORMAT_VERSION ||
        (cJSON_IsNumber(cv) && cv->valuedouble > CONFIG_VERSION)) {
        snprintf(err, cap, "made by a newer vigil (%.30s, export format %d, settings version %d); this is vigil %s: "
                 "update vigil first", s->from_version, cJSON_IsNumber(fv) ? fv->valueint : 0,
                 cJSON_IsNumber(cv) ? cv->valueint : 0, VIGIL_VERSION);
        return BACKUP_EVERSION;
    }
    /* Every file as the manifest lists it: a damaged or edited one is
     * refused before anything is read from it. */
    const cJSON *fl = cJSON_GetObjectItemCaseSensitive(m, "files");
    int listed = 0;
    for (const cJSON *f = cJSON_IsArray(fl) ? fl->child : NULL; f; f = f->next) {
        const tar_file_t *t = tar_find(p->files, p->n, jstr(f, "name"));
        char h[65];
        if (!t) {
            snprintf(err, cap, "the export lacks %.60s, which its manifest lists", jstr(f, "name"));
            return BACKUP_ECHANGED;
        }
        sha256_hex(t->data, t->len, h);
        if (strcmp(h, jstr(f, "sha256"))) {
            snprintf(err, cap, "%.60s was changed or damaged since the export was made", t->name);
            return BACKUP_ECHANGED;
        }
        ++listed;
    }
    if (listed != p->n - 1) {
        snprintf(err, cap, "the export has files its manifest does not list");
        return BACKUP_ECHANGED;
    }
    return BACKUP_OK;
}

/* The key of a password-protected export, and the sealed check of its files. */
static int open_key(const cJSON *m, const pack_t *p, const char *password, xkey_t *x, char *err, size_t cap)
{
    if (!password || !*password) {
        snprintf(err, cap, "this export is protected by a password: type it");
        return BACKUP_EPASSWORD;
    }
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(m, "kdf");
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(k, "iterations");
    const char *salt64 = jstr(k, "salt");
    unsigned char salt[64];
    size_t sl = 0;
    if (strcmp(jstr(k, "algorithm"), KDF_NAME) || !cJSON_IsNumber(it) || it->valuedouble < KDF_READ_MIN ||
        it->valuedouble > KDF_READ_MAX ||
        mbedtls_base64_decode(salt, sizeof salt, &sl, (const unsigned char *)salt64, strlen(salt64)) || sl < 8) {
        snprintf(err, cap, "the export's key derivation is not one vigil reads (%s, 1000 to 10000000 iterations)",
                 KDF_NAME);
        return BACKUP_EINVAL;
    }
    if (secret_derive(password, salt, sl, (unsigned)it->valuedouble, x->key) < 0) {
        snprintf(err, cap, "cannot derive the key");
        return BACKUP_EIO;
    }
    x->secrets = 1;
    char check[64];
    if (secret_open_with(x->key, "export:check", jstr(m, "check"), check, sizeof check) != SECRET_OK ||
        strcmp(check, CHECK_TEXT)) {
        snprintf(err, cap, "wrong password for this export");
        return BACKUP_EPASSWORD;
    }
    /* The files' checksums, sealed: a file changed and its checksum with
     * it in the manifest does not get past this. */
    char want[512] = "", have[600];
    const cJSON *fl = cJSON_GetObjectItemCaseSensitive(m, "files");
    for (const cJSON *f = cJSON_IsArray(fl) ? fl->child : NULL; f; f = f->next) {
        size_t l = strlen(want);
        snprintf(want + l, sizeof want - l, "%s %s\n", jstr(f, "name"), jstr(f, "sha256"));
    }
    if (secret_open_with(x->key, "export:files", jstr(m, "files_sealed"), have, sizeof have) != SECRET_OK ||
        strcmp(have, want)) {
        snprintf(err, cap, "the export was changed since it was made (its sealed checksums do not match)");
        return BACKUP_ECHANGED;
    }
    return BACKUP_OK;
}

typedef struct {
    char *path;
    char *text;                     /* NULL: there was no file */
    size_t len;
} snap_t;

static void snap_take(snap_t *sn, const char *path)
{
    sn->path = strdup(path);
    sn->text = slurp(path, 1u << 20, &sn->len);
}

/* Put a file back as it was (or remove it, if it was not there). */
static int snap_restore(const snap_t *sn)
{
    char err[300];
    if (!sn->path) return -1;
    if (!sn->text) return unlink(sn->path) == 0 || errno == ENOENT ? 0 : -1;
    int rc = config_write_atomic(sn->path, sn->text, sn->len, err, sizeof err);
    if (rc < 0) vg_log("backup: cannot put %s back: %s", sn->path, err);
    return rc < 0 ? -1 : 0;
}

static void snap_free(snap_t *sn)
{
    if (sn->text) secret_wipe(sn->text, sn->len);
    free(sn->text);
    free(sn->path);
    memset(sn, 0, sizeof *sn);
}

static int import_locked(const void *data, size_t len, const backup_import_opts_t *o, backup_summary_t *s,
                         char *err, size_t cap)
{
    pack_t p;
    cJSON *man = NULL;
    xkey_t x = { 0 };
    users_import_t *im = NULL;
    vigil_config_t *nc = calloc(1, sizeof *nc), *cur = calloc(1, sizeof *cur);
    sysimp_t *si = calloc(1, sizeof *si);
    int rc = BACKUP_EIO;
    if (!nc || !cur || !si) {
        snprintf(err, cap, "out of memory");
        goto out;
    }
    rc = read_pack(data, len, &p, &man, s, err, cap);
    if (rc != BACKUP_OK) goto out;
    if (s->secrets && (rc = open_key(man, &p, o->password, &x, err, cap)) != BACKUP_OK) goto out;
    if (!s->secrets && strcmp(jstr(man, "secrets"), "none")) {
        snprintf(err, cap, "the export's manifest names no way its passwords are kept");
        rc = BACKUP_EINVAL;
        goto out;
    }

    /* The settings, as the file is loaded: a key this vigild does not
     * know is skipped (a newer vigil), one missing gets its default. */
    schema_errors_t errs = { 0 };
    if (config_import_text(p.cfg->data, p.cfg->len, open_in, &x, nc, &errs) < 0) {
        if (x.failed || x.foreign) {
            snprintf(err, cap, x.foreign ? "a password in the export is sealed with a recorder's own key, not the "
                                           "export's: the file was changed"
                                         : "a password in the export does not open with its password: the file was "
                                           "changed");
            rc = BACKUP_ECHANGED;
        } else {
            snprintf(err, cap, F_CONFIG ": %.60s%s%.200s", errs.n ? errs.e[0].path : "",
                     errs.n && errs.e[0].path[0] ? ": " : "", errs.n ? errs.e[0].msg : "not valid");
            rc = BACKUP_EINVAL;
        }
        goto out;
    }
    config_get(cur);
    /* Where this recorder keeps its accounts is its own business. */
    snprintf(nc->api.users, sizeof nc->api.users, "%s", cur->api.users);
    snprintf(nc->api.shadow, sizeof nc->api.shadow, "%s", cur->api.shadow);

    if (p.users) {
        im = users_import_prepare(p.users->data, p.users->len, open_in, &x, nc->api.user, err, cap);
        if (!im) {
            rc = x.failed || x.foreign ? BACKUP_ECHANGED : BACKUP_EINVAL;
            goto out;
        }
    }
    if (p.sys && sys_managed() && read_system(p.sys->data, p.sys->len, &x, si, err, cap) < 0) {
        rc = x.failed ? BACKUP_ECHANGED : BACKUP_EINVAL;
        goto out;
    }

    /* ---- the summary ---- */
    s->cameras_now = cur->n_cameras;
    s->cameras_new = nc->n_cameras;
    users_user_t ul[USERS_MAX];
    s->users_now = users_list(ul, USERS_MAX);
    line(s, 0, "From %s%s%s, made %s by vigil %s%s%s", s->from_host[0] ? s->from_host : "a recorder",
         s->from_mac[0] ? " " : "", s->from_mac, s->created, s->from_version, s->from_board[0] ? " on " : "",
         s->from_board);
    if (version_cmp(s->from_version, VIGIL_VERSION) > 0)
        line(s, 1, "Made by a newer vigil (%s; this is %s): settings this one does not know are left out.",
             s->from_version, VIGIL_VERSION);
    char board[64];
    os_release("BUILD_PLATFORM", board, sizeof board);
    const char *plat = jstr(man, "platform");
    if ((s->from_board[0] && board[0] && strcmp(s->from_board, board)) || (*plat && strcmp(plat, g_platform)))
        line(s, 1, "Made on another kind of recorder (%s, %s; this is %s, %s): check the display and recording "
             "settings afterwards.", s->from_board[0] ? s->from_board : "?", *plat ? plat : "?",
             board[0] ? board : "?", g_platform);
    char mac_now[18] = "";
    {
        net_status_t st;
        net_status(ETH, &st);
        snprintf(mac_now, sizeof mac_now, "%s", st.mac);
    }
    if (s->from_mac[0] && mac_now[0] && strcasecmp(s->from_mac, mac_now))
        line(s, 0, "From another recorder (MAC %s; this one is %s).", s->from_mac, mac_now);
    line(s, 0, "Cameras: %d (now %d)", s->cameras_new, s->cameras_now);
    if (im) {
        int nu, np, ng, root;
        users_import_info(im, &nu, &np, &ng, &root);
        s->users_new = nu;
        s->users_no_password = nu - np;
        s->groups_new = ng;
        s->root_password = root && s->secrets;
        char names[160] = "";
        for (int i = 0; i < nu; ++i) {
            int hp;
            const char *n = users_import_user(im, i, &hp);
            size_t l = strlen(names);
            if (l < sizeof names - 40) snprintf(names + l, sizeof names - l, "%s%s", i ? ", " : "", n);
            else if (!strstr(names, "...")) snprintf(names + l, sizeof names - l, ", ...");
        }
        line(s, 0, "Users: %d (now %d)%s%s; groups: %d", nu, s->users_now, nu ? ": " : "", names, ng);
    }
    if (!s->secrets) {
        const cJSON *lo = cJSON_GetObjectItemCaseSensitive(man, "left_out");
        const cJSON *cpj = cJSON_GetObjectItemCaseSensitive(lo, "camera_passwords");
        int cp = cJSON_IsNumber(cpj) ? cpj->valueint : 0;
        char had[40] = "";
        if (cp) snprintf(had, sizeof had, " (%d had one)", cp);
        line(s, 1, "Exported without passwords: enter the cameras' passwords again%s and give every user a new "
             "one; mail, DDNS and network storage passwords too. root's password stays as it is.", had);
    } else if (s->users_no_password) {
        line(s, 1, "%d user(s) without a password in the export: they cannot log in until one is set.",
             s->users_no_password);
    }
    if (s->root_password) line(s, 1, "root's password (the console's, SSH's and the system account's) is set from "
                                     "the export.");
    else if (s->secrets) line(s, 0, "root's password is not in the export: it stays as it is.");
    if (strcmp(nc->api.listen, cur->api.listen))
        line(s, 1, "The API moves to %s at once (now %s).", nc->api.listen, cur->api.listen);
    if (strcmp(nc->web.listen, cur->web.listen))
        line(s, 1, "The web page (zero channel) moves to %s at once (now %s).", nc->web.listen, cur->web.listen);
    if (si->has) {
        s->system = 1;
        system_summary(si, o, s);
        if (s->reboot) line(s, 1, "The network settings apply after a reboot.");
    } else if (p.sys) {
        line(s, 0, "The system's settings in the export (host name, network) are not applied: vigild does not "
                   "manage this system.");
    }

    if (!o->apply) {
        rc = BACKUP_OK;
        goto out;
    }

    /* ---- apply: the accounts, then the settings; the accounts put back
     * if the settings are refused ---- */
    snap_t users_snap = { 0 };
    snap_take(&users_snap, cur->api.users);
    users_err_t ue;
    if (im && users_import_commit(im, &ue) != USERS_OK) {
        snprintf(err, cap, "the accounts were not saved: %s", ue.msg);
        snap_free(&users_snap);
        rc = BACKUP_EIO;
        goto out;
    }
    errs = (schema_errors_t){ 0 };
    config_rc_t cr = config_replace(nc, &errs);
    if (cr != CONFIG_OK && cr != CONFIG_EUNSYNCED) {
        snprintf(err, cap, "the settings were not saved (nothing changed): %.60s%s%.200s", errs.n ? errs.e[0].path : "",
                 errs.n && errs.e[0].path[0] ? ": " : "", errs.n ? errs.e[0].msg : "refused");
        if (im) {
            snap_restore(&users_snap);
            users_reload();
        }
        snap_free(&users_snap);
        rc = cr == CONFIG_EINVAL ? BACKUP_EINVAL : BACKUP_EIO;
        goto out;
    }
    snap_free(&users_snap);
    s->applied = 1;
    rc = BACKUP_OK;
    char e[200];
    if (im && users_import_root(im)[0] && s->secrets) {
        if (users_set_system_password(users_import_root(im), &ue) != USERS_OK)
            line(s, 1, "root's password was not set: %s", ue.msg);
    }
    if (si->has) {
        if (s->hostname_change && mgmt_hostname_set(si->host, e, sizeof e) != MGMT_OK)
            line(s, 1, "The host name was not set: %s", e);
        if (s->tz_change && mgmt_time_set(si->zone, si->zone[0] ? NULL : si->tz, 0, 0, e, sizeof e) != MGMT_OK)
            line(s, 1, "The time zone was not set: %s", e);
        if (s->ntp_change &&
            mgmt_ntp_set(nc->time.ntp, si->ntp, si->n_ntp, nc->time.ntp_interval, e, sizeof e) != MGMT_OK)
            line(s, 1, "The NTP servers were not set: %s", e);
        if (s->net_change && net_write(&si->eth, e, sizeof e) < 0) {
            line(s, 1, "The Ethernet settings were not written: %s", e);
            s->net_change = s->net_cuts = 0;
        }
        if (s->wifi_change) {
            wifi_env_t w = si->wifi;
            if (!w.password[0] && !s->secrets) {
                wifi_env_t now;
                wifi_env_get(&now);
                if (!strcmp(now.ssid, w.ssid)) snprintf(w.password, sizeof w.password, "%s", now.password);
                secret_wipe(&now, sizeof now);
            }
            if (wifi_env_set(&w, e, sizeof e) < 0) {
                line(s, 1, "The Wi-Fi settings were not written: %s", e);
                s->wifi_change = 0;
            }
            secret_wipe(&w, sizeof w);
        }
        s->reboot = s->net_change || s->wifi_change;
    }
    vg_log("configuration imported from %s (%s): %d camera(s), %d user(s)%s%s", s->from_host[0] ? s->from_host : "?",
           s->created, s->cameras_new, s->users_new, s->root_password ? ", root's password" : "",
           s->reboot ? "; the network at the next boot" : "");
    events_push(0, "configuration imported");
out:
    users_import_free(im);
    cJSON_Delete(man);
    if (nc) secret_wipe(nc, sizeof *nc);
    if (cur) secret_wipe(cur, sizeof *cur);
    if (si) secret_wipe(si, sizeof *si);
    free(nc);
    free(cur);
    free(si);
    xkey_wipe(&x);
    return rc;
}

int backup_import(const void *data, size_t len, const backup_import_opts_t *o, backup_summary_t *s, char *err,
                  size_t cap)
{
    memset(s, 0, sizeof *s);
    err[0] = 0;
    if (pthread_mutex_trylock(&g_busy) != 0) {
        snprintf(err, cap, "another export, import or reset is under way");
        return BACKUP_EBUSY;
    }
    int rc = import_locked(data, len, o, s, err, cap);
    pthread_mutex_unlock(&g_busy);
    if (rc != BACKUP_OK) vg_log("configuration import refused: %s", err);
    return rc;
}

int backup_import_file(const char *path, const backup_import_opts_t *o, backup_summary_t *s, char *err, size_t cap)
{
    memset(s, 0, sizeof *s);
    backup_file_t f[32];
    int n = backup_files(f, 32), ok = 0;
    for (int i = 0; i < n && !ok; ++i) ok = !strcmp(f[i].path, path);
    if (!ok) {
        snprintf(err, cap, "%.120s is not an export on a mounted disk", path);
        return BACKUP_EINVAL;
    }
    char p[512];
    size_t len = 0;
    char *data = slurp(sys_path(p, sizeof p, path), BACKUP_FILE_MAX, &len);
    if (!data) {
        snprintf(err, cap, "cannot read %.120s: %s", path, strerror(errno));
        return BACKUP_EIO;
    }
    int rc = backup_import(data, len, o, s, err, cap);
    wipe_free(data, len);
    return rc;
}

/* ---- reset ------------------------------------------------------------------------- */

/* The interfaces' stanzas as the image has them (a stanza the image does
 * not have goes), and no Wi-Fi network; the adapter's profile stays. */
static int reset_network(char *err, size_t cap)
{
    char p[512], rel[300], buf[16384];
    int rc = 0;
    DIR *d = opendir(sys_path(p, sizeof p, IFACES_DIR));
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(rel, sizeof rel, ROM_IFACES "/%.60s", e->d_name);
        if (!sys_exists(rel)) {
            snprintf(rel, sizeof rel, IFACES_DIR "/%.60s", e->d_name);
            unlink(sys_path(p, sizeof p, rel));
        }
    }
    if (d) closedir(d);
    d = opendir(sys_path(p, sizeof p, ROM_IFACES));
    while (d && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(rel, sizeof rel, ROM_IFACES "/%.60s", e->d_name);
        long n = sys_read(rel, buf, sizeof buf);
        snprintf(rel, sizeof rel, IFACES_DIR "/%.60s", e->d_name);
        if (n < 0 || sys_write(rel, buf, (size_t)n, 0644, err, cap) < 0) rc = -1;
    }
    if (d) closedir(d);
    wifi_env_t w;
    wifi_env_get(&w);
    w.ssid[0] = w.password[0] = 0;
    if (wifi_env_set(&w, err, cap) < 0) rc = -1;
    secret_wipe(&w, sizeof w);
    return rc;
}

static int reset_locked(int network, int *reboot, char *err, size_t cap)
{
    char defaults[300], keypath[300];
    /* VIGIL_DEFAULTS: the end-to-end tests' image, which has no /rom. */
    const char *env = getenv("VIGIL_DEFAULTS");
    if (g_defaults[0]) snprintf(defaults, sizeof defaults, "%s", g_defaults);
    else if (env && *env) snprintf(defaults, sizeof defaults, "%s", env);
    else snprintf(defaults, sizeof defaults, "/rom%s", config_path());
    vigil_config_t *c = calloc(1, sizeof *c), *cur = calloc(1, sizeof *cur);
    snap_t cfg_snap = { 0 }, users_snap = { 0 }, key_snap = { 0 };
    int rc = BACKUP_EIO;
    if (!c || !cur) {
        snprintf(err, cap, "out of memory");
        goto out;
    }
    schema_errors_t errs = { 0 };
    int lr = config_load_file(defaults, c, &errs);
    if (lr < 0)
        vg_log("reset: %s is invalid (%s); vigil's own defaults instead", defaults, errs.n ? errs.e[0].msg : "?");
    if (lr != 0) config_defaults(c);
    config_get(cur);
    snprintf(c->api.users, sizeof c->api.users, "%s", cur->api.users);
    snprintf(c->api.shadow, sizeof c->api.shadow, "%s", cur->api.shadow);
    secret_key_path(keypath, sizeof keypath);
    snap_take(&cfg_snap, config_path());
    snap_take(&users_snap, cur->api.users);
    snap_take(&key_snap, keypath);

    /* The new key first: nothing sealed with the old one is kept, and
     * the files below are written with the new one. */
    if (secret_new_key() != SECRET_OK) {
        snprintf(err, cap, "cannot make a new key (%s); nothing changed", keypath);
        goto out;
    }
    errs = (schema_errors_t){ 0 };
    config_rc_t cr = config_replace(c, &errs);
    users_err_t ue = { 0 };
    users_rc_t ur = cr == CONFIG_OK || cr == CONFIG_EUNSYNCED ? users_reset(&ue) : USERS_EIO;
    if (ur != USERS_OK) {
        if (cr == CONFIG_OK || cr == CONFIG_EUNSYNCED) {
            snprintf(err, cap, "the users file was not reset (%s); nothing changed", ue.msg);
            snap_restore(&cfg_snap);
            config_init(config_path());
        } else {
            snprintf(err, cap, "the settings were not reset (%s); nothing changed",
                     errs.n ? errs.e[0].msg : "refused");
        }
        snap_restore(&key_snap);
        secret_set_key_path(keypath);
        users_reload();
        goto out;
    }
    rc = BACKUP_OK;
    *reboot = 0;
    vg_log("vigil's settings reset to %s; no users; a new key%s", lr == 0 ? defaults : "vigil's defaults",
           network ? "; the network too, rebooting" : "");
    events_push(0, "vigil's settings reset");
    if (network) {
        char e[200] = "";
        if (reset_network(e, sizeof e) < 0) vg_log("reset: the network: %s", e);
        *reboot = mgmt_power_request(MGMT_POWER_REBOOT) == MGMT_OK;
    }
out:
    snap_free(&cfg_snap);
    snap_free(&users_snap);
    snap_free(&key_snap);
    if (c) secret_wipe(c, sizeof *c);
    if (cur) secret_wipe(cur, sizeof *cur);
    free(c);
    free(cur);
    return rc;
}

int backup_reset_vigil(int network, int *reboot, char *err, size_t cap)
{
    *reboot = 0;
    err[0] = 0;
    if (network && !sys_managed()) {
        snprintf(err, cap, "vigild does not manage this system's network");
        return BACKUP_EUNMANAGED;
    }
    if (pthread_mutex_trylock(&g_busy) != 0) {
        snprintf(err, cap, "another export, import or reset is under way");
        return BACKUP_EBUSY;
    }
    int rc = reset_locked(network, reboot, err, cap);
    pthread_mutex_unlock(&g_busy);
    return rc;
}

int backup_reset_factory(char *err, size_t cap)
{
    err[0] = 0;
    if (!sys_managed()) {
        snprintf(err, cap, "vigild does not manage this system: no factory reset");
        return BACKUP_EUNMANAGED;
    }
    if (!sys_have("firstboot")) {
        snprintf(err, cap, "this firmware has no firstboot");
        return BACKUP_EIO;
    }
    if (pthread_mutex_trylock(&g_busy) != 0) {
        snprintf(err, cap, "another export, import or reset is under way");
        return BACKUP_EBUSY;
    }
    int rc = mgmt_power_request(MGMT_POWER_FACTORY) == MGMT_OK ? BACKUP_OK : BACKUP_EIO;
    /* Held: nothing else is exported or imported while vigild stops. */
    if (rc != BACKUP_OK) pthread_mutex_unlock(&g_busy);
    if (rc != BACKUP_OK) snprintf(err, cap, "the factory reset was not started");
    return rc;
}
