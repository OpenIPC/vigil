/* SPDX-License-Identifier: GPL-3.0-or-later */
/* sys_nas.c — network storage (see sys_nas.h). */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "sys.h"
#include "sys_nas.h"

/* ---- what the kernel has -------------------------------------------------------------- */

static int fs_registered(const char *fs)
{
    char buf[4096];
    if (sys_read("/proc/filesystems", buf, sizeof buf) <= 0) return 0;
    for (char *save = NULL, *l = strtok_r(buf, "\n", &save); l; l = strtok_r(NULL, "\n", &save)) {
        char *t = strrchr(l, '\t');
        t = t ? t + 1 : l;
        while (*t == ' ') ++t;
        if (!strcmp(t, fs)) return 1;
    }
    return 0;
}

/* Is <mod>.ko anywhere under /lib/modules? (The tree is small on a
 * board; the depth is bounded all the same.) */
static int find_ko(const char *dir, const char *ko, int depth)
{
    char p[512];
    DIR *d = opendir(sys_path(p, sizeof p, dir));
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        if (!strcmp(e->d_name, ko)) {
            found = 1;
            break;
        }
        char sub[512];
        struct stat st;
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        if (depth > 0 && stat(sys_path(p, sizeof p, sub), &st) == 0 && S_ISDIR(st.st_mode))
            found = find_ko(sub, ko, depth - 1);
    }
    closedir(d);
    return found;
}

static int have_module(const char *mod)
{
    char ko[64];
    snprintf(ko, sizeof ko, "%s.ko", mod);
    return find_ko("/lib/modules", ko, 6);
}

int sys_nas_support(const char *type, char *why, size_t cap)
{
    if (!strcmp(type, "cifs")) {
        if (fs_registered("cifs") || have_module("cifs")) return 1;
        snprintf(why, cap, "the kernel has no SMB/CIFS client: it needs CONFIG_CIFS");
        return 0;
    }
    if (!strcmp(type, "nfs")) {
        /* NFS_V3 is a module of its own when NFS is a module, so the
         * module tree tells; a kernel with NFS built in cannot be asked
         * which versions it has, and is taken at its word. */
        if (have_module("nfsv3")) return 1;
        if (have_module("nfs") || have_module("nfsv2")) {
            snprintf(why, cap, "the kernel's NFS client has no version 3: it needs CONFIG_NFS_V3");
            return 0;
        }
        if (fs_registered("nfs")) return 1;
        snprintf(why, cap, "the kernel has no NFS client: it needs CONFIG_NFS_FS and CONFIG_NFS_V3");
        return 0;
    }
    snprintf(why, cap, "unknown network storage type '%s'", type);
    return 0;
}

/* ---- the mount ---------------------------------------------------------------------- */

/* `s` with each comma doubled: the cifs option parser's escape. */
static int doubled(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    for (; *s; ++s) {
        if (n + 3 > cap) return -1;
        out[n++] = *s;
        if (*s == ',') out[n++] = ',';
    }
    out[n] = 0;
    return 0;
}

int sys_nas_mount_args(const config_nas_t *n, const char *ip, char *src, size_t scap, char *data,
                       size_t dcap, char *why, size_t wcap)
{
    int r;
    if (!strcmp(n->type, "cifs")) {
        const char *share = n->share;
        while (*share == '/') ++share;
        size_t sl = strcspn(share, "/");
        const char *prefix = share[sl] ? share + sl + 1 : "";
        if (!sl) {
            snprintf(why, wcap, "no share given");
            return -1;
        }
        if (strchr(n->user, ',')) {
            snprintf(why, wcap, "the user name must not contain a comma");
            return -1;
        }
        char pw[160];
        if (doubled(n->password, pw, sizeof pw) < 0) {
            snprintf(why, wcap, "the password is too long");
            return -1;
        }
        char cred[300];
        if (n->user[0]) snprintf(cred, sizeof cred, "username=%s,password=%s", n->user, pw);
        else snprintf(cred, sizeof cred, "guest");
        memset(pw, 0, sizeof pw);
        snprintf(src, scap, "//%s/%s", ip, share);
        r = snprintf(data, dcap, "unc=\\\\%s\\%.*s,ip=%s,%s%s%s%s%s%s", ip, (int)sl, share, ip, cred,
                     *prefix ? ",prefixpath=" : "", prefix, strstr(n->options, "vers=") ? "" : ",vers=2.1",
                     n->options[0] ? "," : "", n->options);
        memset(cred, 0, sizeof cred);
    } else if (!strcmp(n->type, "nfs")) {
        if (n->share[0] != '/') {
            snprintf(why, wcap, "an NFS share is the exported path (/export/nvr)");
            return -1;
        }
        snprintf(src, scap, "%s:%s", ip, n->share);
        r = snprintf(data, dcap, "addr=%s,nolock%s%s%s", ip, strstr(n->options, "vers=") ? "" : ",vers=3",
                     n->options[0] ? "," : "", n->options);
    } else {
        snprintf(why, wcap, "no network storage type set");
        return -1;
    }
    if (r < 0 || (size_t)r >= dcap) {
        snprintf(why, wcap, "the mount options are too long");
        return -1;
    }
    return 0;
}

/* The server as an IPv4 address (the kernel resolves no names). */
static int resolve(const char *host, char *ip, size_t cap, char *why, size_t wcap)
{
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) == 1) {
        snprintf(ip, cap, "%s", host);
        return 0;
    }
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    int rc = getaddrinfo(host, NULL, &hints, &res);
    if (rc != 0 || !res) {
        snprintf(why, wcap, "cannot find the server %s: %s", host, gai_strerror(rc));
        return -1;
    }
    inet_ntop(AF_INET, &((struct sockaddr_in *)(void *)res->ai_addr)->sin_addr, ip, (socklen_t)cap);
    freeaddrinfo(res);
    return 0;
}

static const char *mount_error(int e)
{
    switch (e) {
    case EACCES:
    case EPERM:        return "the server refused the user or the password";
    case ENOENT:       return "the server has no such share";
    case ENODEV:       return "the kernel cannot mount this kind of share";
    case EHOSTUNREACH:
    case ENETUNREACH:  return "the server cannot be reached";
    case ETIMEDOUT:    return "the server did not answer";
    case ECONNREFUSED: return "the server refused the connection";
    case EPROTONOSUPPORT:
    case EOPNOTSUPP:   return "the server does not speak this protocol version (try vers= in the options)";
    case EBUSY:        return "something is already mounted there";
    default:           return strerror(e);
    }
}

int sys_nas_mount(const config_nas_t *n, const char *target, char *msg, size_t cap)
{
    char why[200], ip[64], src[300], data[700];
    msg[0] = 0;
    if (!sys_nas_support(n->type, why, sizeof why)) {
        snprintf(msg, cap, "%s", why);
        return -1;
    }
    if (!n->server[0]) {
        snprintf(msg, cap, "no server given");
        return -1;
    }
    if (resolve(n->server, ip, sizeof ip, why, sizeof why) < 0 ||
        sys_nas_mount_args(n, ip, src, sizeof src, data, sizeof data, why, sizeof why) < 0) {
        snprintf(msg, cap, "%s", why);
        return -1;
    }
    char p[512], out[256];
    sys_path(p, sizeof p, target);
    if (mkdir(p, 0755) < 0 && errno != EEXIST) {
        snprintf(msg, cap, "cannot create %s: %s", target, strerror(errno));
        memset(data, 0, sizeof data);
        return -1;
    }
    /* The client is a module on the firmware's kernels, and nothing else
     * loads it. */
    if (!strcmp(n->type, "cifs") ? !fs_registered("cifs") : have_module("nfsv3"))
        sys_run(out, sizeof out, 30, "modprobe", !strcmp(n->type, "cifs") ? "cifs" : "nfsv3", NULL);
    int rc = sys_ops()->mount(src, p, n->type, 0, data);
    int e = errno, smb1 = 0;
    memset(data, 0, sizeof data);
    if (rc < 0 && e == EINVAL && !strcmp(n->type, "cifs") && !strstr(n->options, "vers=")) {
        /* A kernel without CONFIG_CIFS_SMB2 (the ssr621q one, 4.9) takes
         * no vers= but 1.0 and answers EINVAL to 2.1, whatever the
         * server: seen in QEMU. Try SMB1 before saying anything. */
        config_nas_t v1 = *n;
        snprintf(v1.options, sizeof v1.options, "vers=1.0%s%.110s", n->options[0] ? "," : "", n->options);
        if (sys_nas_mount_args(&v1, ip, src, sizeof src, data, sizeof data, why, sizeof why) == 0) {
            rc = sys_ops()->mount(src, p, n->type, 0, data);
            e = errno;
            smb1 = 1;
        }
        memset(data, 0, sizeof data);
        memset(v1.password, 0, sizeof v1.password);
    }
    if (rc < 0) {
        snprintf(msg, cap, "cannot mount %s: %s%s", src, mount_error(e),
                 smb1 ? " (this kernel speaks SMB1 only: SMB2/3 need CONFIG_CIFS_SMB2)" : "");
        return -1;
    }
    snprintf(msg, cap, "%s mounted at %s%s", src, target,
             smb1 ? " with SMB1 (this kernel has no SMB2/3: CONFIG_CIFS_SMB2)" : "");
    return 0;
}

int sys_nas_umount(const char *target, char *msg, size_t cap)
{
    char p[512];
    if (sys_ops()->umount(sys_path(p, sizeof p, target), 0) < 0) {
        if (errno == EBUSY) snprintf(msg, cap, "%s is in use; stop recording onto it first", target);
        else snprintf(msg, cap, "cannot unmount %s: %s", target, strerror(errno));
        return -1;
    }
    snprintf(msg, cap, "%s unmounted", target);
    return 0;
}

int sys_nas_mounted(const char *target)
{
    char *buf = malloc(32768);
    int found = 0;
    if (buf && sys_read("/proc/mounts", buf, 32768) > 0)
        for (char *save = NULL, *l = strtok_r(buf, "\n", &save); l && !found; l = strtok_r(NULL, "\n", &save)) {
            char dev[256], mnt[256], fs[32];
            if (sscanf(l, "%255s %255s %31s", dev, mnt, fs) != 3) continue;
            found = !strcmp(mnt, target) && (!strcmp(fs, "cifs") || !strncmp(fs, "nfs", 3) || !strcmp(fs, "smb3"));
        }
    free(buf);
    return found;
}

int sys_nas_test(const config_nas_t *n, char *msg, size_t cap)
{
    char m[300], p[512];
    if (sys_nas_mount(n, SYS_NAS_TEST_DIR, m, sizeof m) < 0) {
        snprintf(msg, cap, "%s", m);
        rmdir(sys_path(p, sizeof p, SYS_NAS_TEST_DIR));
        return -1;
    }
    char rel[128], f[600];
    snprintf(rel, sizeof rel, "%s/.vigil-test-%d", SYS_NAS_TEST_DIR, (int)getpid());
    sys_path(f, sizeof f, rel);
    static const char probe[] = "vigil network storage test\n";
    const char *fail = NULL;
    int e = 0;
    int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 || write(fd, probe, sizeof probe - 1) != (ssize_t)(sizeof probe - 1) || fsync(fd) < 0) {
        e = errno;
        fail = "cannot write to it";
    }
    if (fd >= 0) close(fd);
    if (!fail) {
        char back[64] = "";
        fd = open(f, O_RDONLY | O_CLOEXEC);
        ssize_t r = fd >= 0 ? read(fd, back, sizeof back - 1) : -1;
        e = errno;
        if (fd >= 0) close(fd);
        if (r != (ssize_t)(sizeof probe - 1) || memcmp(back, probe, sizeof probe - 1)) fail = "what was written did not read back";
    }
    unlink(f);
    char um[200];
    int urc = sys_nas_umount(SYS_NAS_TEST_DIR, um, sizeof um);
    rmdir(sys_path(p, sizeof p, SYS_NAS_TEST_DIR));
    if (fail) {
        snprintf(msg, cap, "mounted, but %s%s%s", fail, e && e != ENOENT ? ": " : "",
                 e && e != ENOENT ? (e == EROFS ? "the share is read-only" : e == EACCES ? "no permission" : strerror(e)) : "");
        return -1;
    }
    if (urc < 0) {
        snprintf(msg, cap, "mounted, written and read, but %s", um);
        return -1;
    }
    snprintf(msg, cap, "mounted, written to, read back and unmounted: it can be recorded onto");
    return 0;
}
