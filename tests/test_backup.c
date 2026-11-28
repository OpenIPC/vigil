/* SPDX-License-Identifier: GPL-3.0-or-later */
/* test_backup.c — the configuration export and import, and the resets
 * (src/api/backup.c), on a fake system (sys_fake.h): two recorders with
 * keys of their own, the U-Boot environment in memory. */
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

#include "backup.h"
#include "cJSON.h"
#include "config.h"
#include "mgmt.h"
#include "secret.h"
#include "sys.h"
#include "sys_fake.h"
#include "sys_net.h"
#include "tar.h"
#include "users.h"



static const char *const k_secrets[] = { "camPass1", "subPass2", "mailPass9", "annaPass7", "rootPass5",
                                         "wifiPass88", "nasPass3" };

/* ---- the U-Boot environment: fw_printenv / fw_setenv in memory --------------------- */

static char g_env[8][2][96];
static sys_ops_t g_fake_ops;             /* the recorder's, copied: sys_set_ops copies too */

static char *env_slot(const char *name, int create)
{
    for (int i = 0; i < 8; ++i)
        if (!strcmp(g_env[i][0], name)) return g_env[i][1];
    if (!create) return NULL;
    for (int i = 0; i < 8; ++i)
        if (!g_env[i][0][0]) {
            snprintf(g_env[i][0], sizeof g_env[i][0], "%s", name);
            return g_env[i][1];
        }
    return NULL;
}

static int env_run(const char *const *argv, const char *input, int t, char *out, size_t cap)
{
    if (!strcmp(argv[0], "fw_printenv") && argv[1] && !strcmp(argv[1], "-n") && argv[2]) {
        char *v = env_slot(argv[2], 0);
        if (cap) out[0] = 0;
        if (!v || !v[0]) return 1;
        snprintf(out, cap, "%s\n", v);
        return 0;
    }
    if (!strcmp(argv[0], "fw_setenv") && argv[1]) {
        char *v = env_slot(argv[1], 1);
        if (v) snprintf(v, 96, "%s", argv[2] ? argv[2] : "");
    }
    return g_fake_ops.run(argv, input, t, out, cap);
}

static void env_set(const char *name, const char *value) { snprintf(env_slot(name, 1), 96, "%s", value); }
static const char *env_get(const char *name)
{
    const char *v = env_slot(name, 0);
    return v ? v : "";
}

/* ---- a recorder ------------------------------------------------------------------- */

static char g_cfg[300], g_users[300], g_shadow[300], g_key[300];

static void path_of(char *out, size_t cap, const char *rel) { snprintf(out, cap, "%s%s", fake_root, rel); }

static void device(const char *host, const char *mac, const char *cfg_yaml)
{
    fake_init();
    static sys_ops_t ops;
    g_fake_ops = *sys_ops();
    ops = g_fake_ops;
    ops.run = env_run;
    sys_set_ops(&ops);
    memset(g_env, 0, sizeof g_env);
    path_of(g_cfg, sizeof g_cfg, "/etc/vigil.yaml");
    path_of(g_users, sizeof g_users, "/etc/vigil.users.yaml");
    path_of(g_shadow, sizeof g_shadow, "/etc/shadow");
    path_of(g_key, sizeof g_key, "/etc/vigil.key");
    fake_file("/etc/os-release", "OPENIPC_VERSION=2.6\nBUILD_PLATFORM=ssr621q_lite\nBUILD_ID=test\n");
    char t[1200];
    snprintf(t, sizeof t, "%s\n", host);
    fake_file("/etc/hostname", t);
    fake_file("/etc/TZ", "GMT0\n");
    fake_file("/etc/timezone", "Etc/GMT\n");
    fake_file("/etc/ntp.conf", "server 0.pool.ntp.org iburst\n");
    fake_file("/etc/network/interfaces.d/eth0", "iface eth0 inet dhcp\n    hwaddress ether $(get_mac eth0)\n");
    fake_file("/rom/etc/network/interfaces.d/eth0", "iface eth0 inet dhcp\n    hwaddress ether $(get_mac eth0)\n");
    fake_file("/rom/etc/network/interfaces.d/wlan0", "iface wlan0 inet dhcp\n");
    snprintf(t, sizeof t, "%s\n", mac);
    fake_file("/sys/class/net/eth0/address", t);
    fake_file("/sys/class/net/eth0/operstate", "up\n");
    fake_ifaddr("eth0", "192.168.1.10", "255.255.255.0");
    fake_file("/etc/shadow", "root::19000:0:99999:7:::\n");
    /* A USB stick at /mnt/sdb1 for the file exports. */
    fake_file("/sys/block/sdb/size", "262144\n");
    fake_file("/sys/block/sdb/removable", "1\n");
    fake_file("/sys/block/sdb/ro", "0\n");
    fake_file("/sys/block/sdb/device/model", "Stick\n");
    fake_file("/sys/block/sdb/sdb1/partition", "1\n");
    fake_file("/sys/block/sdb/sdb1/size", "260096\n");
    fake_mkdir("/mnt/sdb1");
    fake_file("/proc/mounts", "/dev/root / squashfs ro 0 0\n/dev/sdb1 /mnt/sdb1 vfat rw 0 0\n");
    snprintf(t, sizeof t,
             "version: 1\n%s\napi:\n  users: \"%s\"\n  shadow: \"%s\"\n", cfg_yaml, g_users, g_shadow);
    FILE *f = fopen(g_cfg, "w");
    fputs(t, f);
    fclose(f);
    secret_set_key_path(NULL);
    CHECK(config_init(g_cfg) == 0, "config of %s", host);
    CHECK(secret_ensure_key() == SECRET_OK, "a key for %s", host);
    users_reload();
}

static char *read_all(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = malloc(1 << 20);
    size_t n = fread(b, 1, (1 << 20) - 1, f);
    fclose(f);
    b[n] = 0;
    if (len) *len = n;
    return b;
}

static int mem_has(const char *buf, size_t len, const char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i + n <= len; ++i)
        if (!memcmp(buf + i, s, n)) return 1;
    return 0;
}

/* A sealed value with `prefix` ("enc:v1:"), not the word in a comment. */
static const char *sealed_in(const char *buf, size_t len, const char *prefix)
{
    size_t n = strlen(prefix);
    for (size_t i = 0; i + n < len; ++i) {
        unsigned char c = (unsigned char)buf[i + n];
        if (!memcmp(buf + i, prefix, n) && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                            (c >= '0' && c <= '9') || c == '+' || c == '/'))
            return buf + i;
    }
    return NULL;
}

/* The file `name` of the archive, replaced by `text` (the archive rebuilt). */
static char *repack(const char *tar, size_t len, const char *name, const char *text, size_t *out_len)
{
    tar_file_t f[TAR_FILES_MAX];
    char err[200];
    int n = tar_read(tar, len, f, TAR_FILES_MAX, err, sizeof err);
    tar_out_t t = { 0 };
    for (int i = 0; i < n; ++i) {
        if (!strcmp(f[i].name, name)) tar_add(&t, name, text, strlen(text), 0);
        else tar_add(&t, f[i].name, f[i].data, f[i].len, 0);
    }
    tar_finish(&t);
    *out_len = t.len;
    return t.data;
}

static char *file_of(const char *tar, size_t len, const char *name)
{
    tar_file_t f[TAR_FILES_MAX];
    char err[200];
    int n = tar_read(tar, len, f, TAR_FILES_MAX, err, sizeof err);
    const tar_file_t *x = n > 0 ? tar_find(f, n, name) : NULL;
    return x ? strndup(x->data, x->len) : NULL;
}

static void sha_hex(const char *s, char out[65])
{
    unsigned char d[32];
    mbedtls_sha256((const unsigned char *)s, strlen(s), d, 0);
    for (int i = 0; i < 32; ++i) snprintf(out + 2 * i, 3, "%02x", d[i]);
}

/* The manifest with file `name`'s checksum made right for `text`; with
 * `pw`, its sealed checksums sealed again too (as someone who knows the
 * password would). */
static char *fix_manifest(const char *man, const char *name, const char *text, const char *pw)
{
    cJSON *m = cJSON_Parse(man);
    cJSON *fl = cJSON_GetObjectItem(m, "files");
    char digest[600] = "";
    for (cJSON *f = fl->child; f; f = f->next) {
        if (!strcmp(cJSON_GetObjectItem(f, "name")->valuestring, name)) {
            char h[65];
            sha_hex(text, h);
            cJSON_ReplaceItemInObject(f, "sha256", cJSON_CreateString(h));
            cJSON_ReplaceItemInObject(f, "size", cJSON_CreateNumber((double)strlen(text)));
        }
        size_t l = strlen(digest);
        snprintf(digest + l, sizeof digest - l, "%s %s\n", cJSON_GetObjectItem(f, "name")->valuestring,
                 cJSON_GetObjectItem(f, "sha256")->valuestring);
    }
    if (pw) {
        cJSON *k = cJSON_GetObjectItem(m, "kdf");
        unsigned char salt[64], key[32];
        size_t sl = 0;
        const char *s64 = cJSON_GetObjectItem(k, "salt")->valuestring;
        mbedtls_base64_decode(salt, sizeof salt, &sl, (const unsigned char *)s64, strlen(s64));
        secret_derive(pw, salt, sl, (unsigned)cJSON_GetObjectItem(k, "iterations")->valuedouble, key);
        char sealed[1024];
        secret_seal_with(key, "export:files", digest, sealed, sizeof sealed);
        cJSON_ReplaceItemInObject(m, "files_sealed", cJSON_CreateString(sealed));
    }
    char *out = cJSON_Print(m);
    cJSON_Delete(m);
    return out;
}

static int has_line(const backup_summary_t *s, const char *sub)
{
    for (int i = 0; i < s->n_lines; ++i)
        if (strstr(s->line[i], sub)) return 1;
    return 0;
}

static const char k_cams[] =
    "cameras:\n"
    "  - url: \"rtsp://admin:camPass1@10.0.0.2/stream\"\n"
    "    sub_url: \"rtsp://admin:subPass2@10.0.0.2/sub\"\n"
    "    name: Gate\n"
    "  - url: \"rtsp://10.0.0.3/open\"\n"
    "    name: Yard\n"
    "smtp:\n  server: mail.example.org\n  user: nvr\n  password: mailPass9\n"
    "nas:\n  type: cifs\n  server: 10.0.0.9\n  share: rec\n  user: nvr\n  password: nasPass3\n";

int main(void)
{
    backup_set_kdf_ms(1);              /* the floor: 100000 iterations */
    char err[300];
    users_err_t ue;

    /* ---- recorder A: two cameras, a user, root's password, Wi-Fi ---- */
    device("nvr-a", "aa:bb:cc:00:00:01", k_cams);
    env_set("wlanssid", "HomeNet");
    env_set("wlanpass", "wifiPass88");
    env_set("wlandev", "mt7601u-generic");
    CHECK(users_add("anna", "user", "annaPass7", 1, &ue) == USERS_OK, "anna: %s", ue.msg);
    CHECK(users_set_system_password("rootPass5", &ue) == USERS_OK, "root's password: %s", ue.msg);
    size_t klen = 0;
    char *key_a = read_all(g_key, &klen);

    char *tar = NULL, name[160];
    size_t len = 0;
    CHECK(backup_export("abc", &tar, &len, name, sizeof name, err, sizeof err) == BACKUP_EINVAL,
          "a password of 3 characters is refused");
    CHECK(backup_export("export-PW", &tar, &len, name, sizeof name, err, sizeof err) == BACKUP_OK, "export: %s", err);
    CHECK(!strncmp(name, "vigil-config-nvr-a-", 19) && strstr(name, ".tar"), "file name %s", name);
    for (size_t i = 0; i < sizeof k_secrets / sizeof k_secrets[0]; ++i)
        CHECK(!mem_has(tar, len, k_secrets[i]), "no %s in the export", k_secrets[i]);
    CHECK(!mem_has(tar, len, "export-PW"), "nor its password");
    CHECK(key_a && !mem_has(tar, len, key_a) && !sealed_in(tar, len, "enc:v1:"),
          "nor the key, nor device-sealed values");
    CHECK(sealed_in(tar, len, "enc:p1:") && !mem_has(tar, len, "$6$"), "sealed for the export; no hashes");
    char *man = file_of(tar, len, "manifest.json");
    CHECK(man && strstr(man, "\"secrets\":\t\"password\"") && strstr(man, "PBKDF2-HMAC-SHA256") &&
              strstr(man, "\"board\":\t\"ssr621q_lite\"") && strstr(man, "aa:bb:cc:00:00:01") &&
              strstr(man, "\"root_password\":\ttrue") && strstr(man, "\"camera_passwords\":\t2"),
          "manifest: %s", man ? man : "(none)");
    char *sys_a = file_of(tar, len, "system.json");
    CHECK(sys_a && strstr(sys_a, "HomeNet") && strstr(sys_a, "nvr-a") && strstr(sys_a, "mt7601u-generic"),
          "system.json: %s", sys_a ? sys_a : "(none)");
    free(sys_a);

    char *open_tar = NULL;
    size_t open_len = 0;
    CHECK(backup_export(NULL, &open_tar, &open_len, name, sizeof name, err, sizeof err) == BACKUP_OK, "plain export");
    for (size_t i = 0; i < sizeof k_secrets / sizeof k_secrets[0]; ++i)
        CHECK(!mem_has(open_tar, open_len, k_secrets[i]), "no %s in the export without passwords", k_secrets[i]);
    CHECK(!sealed_in(open_tar, open_len, "enc:v1:") && !sealed_in(open_tar, open_len, "enc:p1:"),
          "nothing sealed in it either");

    char path[400];
    CHECK(backup_export_to("/mnt/sdb1", "export-PW", path, sizeof path, err, sizeof err) == BACKUP_OK,
          "to the stick: %s", err);
    CHECK(backup_export_to("/etc", "export-PW", path, sizeof path, err, sizeof err) == BACKUP_EINVAL,
          "only onto a mounted disk");
    backup_file_t fl[8];
    CHECK(backup_files(fl, 8) == 1 && !strncmp(fl[0].path, "/mnt/sdb1/vigil-config-nvr-a-", 29), "found on the stick");

    /* ---- checks that change nothing ---- */
    backup_summary_t s;
    backup_import_opts_t o = { .password = "export-PW", .hostname = 1 };
    CHECK(backup_import(tar, len, &o, &s, err, sizeof err) == BACKUP_OK && !s.applied, "dry run: %s", err);
    CHECK(s.secrets && s.cameras_new == 2 && s.users_new == 1 && s.root_password && !s.net_change,
          "summary: same recorder");
    o.password = "wrong-PW";
    CHECK(backup_import(tar, len, &o, &s, err, sizeof err) == BACKUP_EPASSWORD && strstr(err, "wrong password"),
          "wrong password: %s", err);
    o.password = NULL;
    CHECK(backup_import(tar, len, &o, &s, err, sizeof err) == BACKUP_EPASSWORD, "no password: %s", err);
    o.password = "export-PW";

    /* A changed file: its checksum, then the sealed checksums, then the
     * value's own GCM tag. */
    char *cfg = file_of(tar, len, "vigil.yaml");
    char *p1 = (char *)sealed_in(cfg, strlen(cfg), "enc:p1:");
    p1[10] = p1[10] == 'A' ? 'B' : 'A';
    size_t tl;
    char *t1 = repack(tar, len, "vigil.yaml", cfg, &tl);
    CHECK(backup_import(t1, tl, &o, &s, err, sizeof err) == BACKUP_ECHANGED, "changed file: %s", err);
    char *m2 = fix_manifest(man, "vigil.yaml", cfg, NULL);
    size_t tl2;
    char *t2 = repack(t1, tl, "manifest.json", m2, &tl2);
    CHECK(backup_import(t2, tl2, &o, &s, err, sizeof err) == BACKUP_ECHANGED && strstr(err, "sealed checksums"),
          "changed file and manifest: %s", err);
    char *m3 = fix_manifest(man, "vigil.yaml", cfg, "export-PW");
    size_t tl3;
    char *t3 = repack(t1, tl, "manifest.json", m3, &tl3);
    CHECK(backup_import(t3, tl3, &o, &s, err, sizeof err) == BACKUP_ECHANGED && strstr(err, "does not open"),
          "a sealed value changed (GCM): %s", err);
    free(t1), free(t2), free(t3), free(m2), free(m3), free(cfg);

    /* Newer versions. */
    char *mv = strdup(man);
    char *fv = strstr(mv, "\"format_version\":\t1");
    fv[strlen("\"format_version\":\t")] = '2';
    char *t4 = repack(tar, len, "manifest.json", mv, &tl);
    CHECK(backup_import(t4, tl, &o, &s, err, sizeof err) == BACKUP_EVERSION && strstr(err, "newer vigil"),
          "a newer format: %s", err);
    free(t4);
    free(mv);
    mv = strdup(man);
    fv = strstr(mv, "\"config_version\":\t1");
    fv[strlen("\"config_version\":\t")] = '7';
    t4 = repack(tar, len, "manifest.json", mv, &tl);
    CHECK(backup_import(t4, tl, &o, &s, err, sizeof err) == BACKUP_EVERSION, "newer settings: %s", err);
    free(t4);
    free(mv);
    mv = strdup(man);
    fv = strstr(mv, "\"vigil_version\":\t\"");
    memcpy(fv + strlen("\"vigil_version\":\t\""), "9", 1);
    t4 = repack(tar, len, "manifest.json", mv, &tl);
    CHECK(backup_import(t4, tl, &o, &s, err, sizeof err) == BACKUP_OK && has_line(&s, "newer vigil"),
          "a newer vigil, same format: a warning (%s)", err);
    free(t4);
    free(mv);
    CHECK(backup_import("not a tar", 9, &o, &s, err, sizeof err) == BACKUP_EINVAL, "not an export: %s", err);

    /* ---- recorder B: another key, another MAC, a static address ---- */
    device("nvr-b", "aa:bb:cc:00:00:02", "cameras: []\n");
    fake_file("/etc/network/interfaces.d/eth0",
              "iface eth0 inet static\n    address 192.168.1.10\n    netmask 255.255.255.0\n"
              "    gateway 192.168.1.1\n    hwaddress ether $(get_mac eth0)\n");
    char *key_b = read_all(g_key, NULL);
    CHECK(key_b && strcmp(key_a, key_b), "B has a key of its own");
    CHECK(backup_import(tar, len, &o, &s, err, sizeof err) == BACKUP_OK, "dry run on B: %s", err);
    CHECK(has_line(&s, "another recorder") && has_line(&s, "nvr-a (now nvr-b)") && s.net_change && s.net_cuts &&
              s.wifi_change && s.reboot && s.cameras_now == 0,
          "B's summary: host name, address, Wi-Fi");
    CHECK(has_line(&s, "DHCP: it may no longer be at 192.168.1.10"), "the address warning");
    o.apply = 1;
    o.hostname = 0;                     /* keep B's name */
    CHECK(backup_import(tar, len, &o, &s, err, sizeof err) == BACKUP_OK && s.applied, "import on B: %s", err);
    vigil_config_t *c = malloc(sizeof *c);
    config_get(c);
    CHECK(c->n_cameras == 2 && !strcmp(c->cameras[0].url, "rtsp://admin:camPass1@10.0.0.2/stream") &&
              !strcmp(c->cameras[0].sub_url, "rtsp://admin:subPass2@10.0.0.2/sub") &&
              !strcmp(c->smtp.password, "mailPass9") && !strcmp(c->nas.password, "nasPass3"),
          "the cameras' and services' passwords in force on B");
    CHECK(!strcmp(c->api.users, g_users) && !strcmp(c->api.shadow, g_shadow), "B's own account files");
    size_t fl_b;
    char *file_b = read_all(g_cfg, &fl_b);
    CHECK(file_b && !mem_has(file_b, fl_b, "camPass1") && sealed_in(file_b, fl_b, "enc:v1:") &&
              !sealed_in(file_b, fl_b, "enc:p1:"),
          "B's file: sealed with B's key");
    vigil_config_t *c2 = malloc(sizeof *c2);
    schema_errors_t se = { 0 };
    CHECK(config_load_file(g_cfg, c2, &se) == 0 && !strcmp(c2->cameras[0].url, c->cameras[0].url),
          "and it opens with B's key");
    unsigned perms;
    char why[200];
    CHECK(users_login("anna", "annaPass7", &perms, why, sizeof why) == 1, "anna logs in on B");
    char ha1[33];
    CHECK(users_digest("anna", ha1, &perms) == 1, "anna's sealed password opens on B (RTSP, ONVIF, NETIP)");
    CHECK(users_login("root", "rootPass5", &perms, why, sizeof why) == 1, "root's password set on B: %s", why);
    char *sh = read_all(g_shadow, NULL);
    CHECK(sh && !strncmp(sh, "root:$", 6), "in B's shadow file");
    free(sh);
    CHECK(!strcmp(env_get("wlanssid"), "HomeNet") && !strcmp(env_get("wlanpass"), "wifiPass88") &&
              !strcmp(env_get("wlandev"), "mt7601u-generic"),
          "Wi-Fi in B's U-Boot environment");
    char *st = fake_read("/etc/network/interfaces.d/eth0");
    CHECK(st && strstr(st, "inet dhcp") && strstr(st, "hwaddress"), "B's stanza for the next boot: %s", st);
    free(st);
    CHECK(!fake_ran("ifdown") && !fake_ran("ifup"), "the interface is left as it is until the reboot");
    char *hn = fake_read("/etc/hostname");
    CHECK(hn && !strcmp(hn, "nvr-b\n"), "host name kept, as asked");
    free(hn);

    /* Without the passwords: dropped, and said so. */
    backup_import_opts_t o2 = { .apply = 1, .hostname = 1 };
    CHECK(backup_import(open_tar, open_len, &o2, &s, err, sizeof err) == BACKUP_OK, "plain import: %s", err);
    CHECK(has_line(&s, "without passwords") && !s.root_password, "summary says so");
    config_get(c);
    CHECK(!strcmp(c->cameras[0].url, "rtsp://admin@10.0.0.2/stream") && !c->smtp.password[0],
          "the cameras without their passwords: %s", c->cameras[0].url);
    CHECK(users_login("anna", "annaPass7", &perms, why, sizeof why) == 0, "anna needs a new password");
    CHECK(users_digest("root", ha1, &perms) == 1, "root's password as it was, still known to RTSP, ONVIF, NETIP");
    CHECK(users_login("root", "rootPass5", &perms, why, sizeof why) == 1, "root's password as it was");
    hn = fake_read("/etc/hostname");
    CHECK(hn && !strcmp(hn, "nvr-a\n"), "host name taken this time");
    free(hn);

    /* ---- reset vigil's settings ---- */
    char rom[600];
    path_of(rom, sizeof rom, "/rom/etc/vigil.yaml");
    fake_file("/rom/etc/vigil.yaml", "version: 1\ncameras: []\nrecord:\n  segment: 120\n");
    backup_set_defaults(rom);
    CHECK(users_add("bob", "user", "bobPass11", 1, &ue) == USERS_OK, "bob");
    int reboot = 1;
    CHECK(backup_reset_vigil(0, &reboot, err, sizeof err) == BACKUP_OK && !reboot, "reset: %s", err);
    config_get(c);
    CHECK(c->n_cameras == 0 && c->record.segment == 120 && !strcmp(c->api.users, g_users), "the image's settings");
    users_user_t ul[4];
    CHECK(users_list(ul, 4) == 0, "no users");
    char *key_c = read_all(g_key, NULL);
    CHECK(key_c && strcmp(key_c, key_b), "a new key");
    CHECK(users_login("root", "rootPass5", &perms, why, sizeof why) == 1, "root still logs in");
    fake_file("/etc/network/interfaces.d/usb0", "iface usb0 inet dhcp\n");
    CHECK(backup_reset_vigil(1, &reboot, err, sizeof err) == BACKUP_OK && reboot, "reset with the network: %s", err);
    CHECK(!fake_exists("/etc/network/interfaces.d/usb0") && fake_exists("/etc/network/interfaces.d/wlan0") &&
              !env_get("wlanssid")[0] && !strcmp(env_get("wlandev"), "mt7601u-generic"),
          "the image's interfaces, no Wi-Fi network, the adapter kept");
    CHECK(mgmt_power_pending() == MGMT_POWER_REBOOT, "a reboot asked for");

    /* ---- the full factory reset: firstboot after the stop ---- */
    CHECK(backup_reset_factory(err, sizeof err) == BACKUP_OK && mgmt_power_pending() == MGMT_POWER_FACTORY,
          "factory reset asked for: %s", err);
    fake_clear_log();
    fake_answer("firstboot", 1, "erase failed");
    mgmt_power_run(MGMT_POWER_FACTORY);
    CHECK(fake_ran("firstboot") && fake_ran("reboot"), "firstboot, then a reboot when it returns: %s", fake_log);

    free(c);
    free(c2);
    free(file_b);
    free(key_a);
    free(key_b);
    free(key_c);
    free(man);
    free(tar);
    free(open_tar);
    fake_done();
    printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
