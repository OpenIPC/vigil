/* SPDX-License-Identifier: GPL-3.0-or-later */
/* logbook.c — a log in RAM, on the disk and (a little of it) on flash
 * (see logbook.h). */
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include "config.h"
#include "log.h"
#include "logbook.h"
#include "vg_random.h"

#define CHUNK      16384            /* the disk's files are read backwards this much at a time */
#define LINE_MAX_  2048             /* a longer "line" is not one of ours */
#define FLASH_MAX  (256u << 10)     /* a flash copy larger than this is not read */

struct logbook {
    logbook_conf_t  conf;
    char            name[24];
    pthread_mutex_t lock;           /* the ring and its counters */
    logrec_t       *ring;
    unsigned long   total;          /* records ever in the ring: the newest is ring[(total - 1) % ram] */
    unsigned long   disk_upto;      /* the records before this are on the disk */
    unsigned long   next_n;         /* ids: <boot>.<n> */
    unsigned long   changes;        /* logbook_count */
    int             flash_dirty;
    pthread_mutex_t io;             /* the files */
    char            disk[256];      /* "" none */
    int             fd;
    long            fsize;
    char            flash[256];     /* "" none */
    time_t          flash_at;       /* monotonic; 0: not yet */
    int             lost_told;
};

/* ---- ids ------------------------------------------------------------------------------- */

static pthread_once_t g_boot_once = PTHREAD_ONCE_INIT;
static uint32_t       g_boot;

static void boot_init(void)
{
    /* Weak is enough (vg_random.h): ids only have to differ between boots. */
    vg_random(&g_boot, sizeof g_boot, VG_RAND_WEAK);
}

/* ---- lines ----------------------------------------------------------------------------- */

static size_t put_esc(char *out, size_t cap, size_t at, const char *s)
{
    for (; *s && at + 2 < cap; ++s) {
        unsigned char c = (unsigned char)*s;
        const char *e = c == '\t' ? "\\t" : c == '\n' ? "\\n" : c == '\r' ? "\\r" : c == '\\' ? "\\\\" : NULL;
        if (e) {
            out[at++] = e[0];
            out[at++] = e[1];
        } else {
            out[at++] = c < 0x20 || c == 0x7f ? '?' : (char)c;
        }
    }
    if (*s) return cap;                 /* did not fit */
    return at;
}

int logbook_format(const logrec_t *r, char *out, size_t cap)
{
    int n = snprintf(out, cap, "%s\t%lld\t%d\t%d\t", r->id, (long long)r->time, r->ch, r->active);
    if (n < 0 || (size_t)n >= cap) return -1;
    size_t at = (size_t)n;
    const char *f[4] = { r->source, r->who, r->type, r->detail };
    for (int i = 0; i < 4; ++i) {
        at = put_esc(out, cap, at, f[i]);
        if (at + 2 > cap) return -1;
        out[at++] = i < 3 ? '\t' : '\n';
    }
    out[at] = 0;
    return (int)at;
}

/* One field of `line` up to a tab or the end, unescaped into `out`. */
static const char *field(const char *p, const char *end, char *out, size_t cap)
{
    size_t n = 0;
    while (p < end && *p != '\t') {
        char c = *p++;
        if (c == '\\' && p < end) {
            char e = *p++;
            c = e == 't' ? '\t' : e == 'n' ? '\n' : e == 'r' ? '\r' : e;
        }
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = 0;
    return p < end ? p + 1 : p;
}

int logbook_parse(const char *line, size_t len, logrec_t *r)
{
    const char *p = line, *end = line + len;
    char num[3][24];
    memset(r, 0, sizeof *r);
    p = field(p, end, r->id, sizeof r->id);
    for (int i = 0; i < 3; ++i) p = field(p, end, num[i], sizeof num[i]);
    if (!r->id[0] || !num[0][0] || p >= end) return -1;
    char *e;
    long long t = strtoll(num[0], &e, 10);
    if (*e) return -1;
    r->time = (time_t)t;
    r->ch = atoi(num[1]);
    r->active = atoi(num[2]);
    p = field(p, end, r->source, sizeof r->source);
    p = field(p, end, r->who, sizeof r->who);
    p = field(p, end, r->type, sizeof r->type);
    field(p, end, r->detail, sizeof r->detail);
    return r->type[0] ? 0 : -1;
}

/* A CSV cell: quoted when it must be; a leading = + - @ (a formula to a
 * spreadsheet) gets a ' before it, since a user name or a camera's text
 * is anyone's to choose. */
static size_t csv_cell(char *out, size_t cap, size_t at, const char *s, int last)
{
    int quote = strpbrk(s, ",\"\r\n") != NULL;
    int guard = *s == '=' || *s == '+' || *s == '-' || *s == '@';
    if (at < cap && quote) out[at++] = '"';
    if (at < cap && guard) out[at++] = '\'';
    for (; *s && at + 1 < cap; ++s) {
        if (*s == '"') out[at++] = '"';
        if (at + 1 < cap) out[at++] = *s;
    }
    if (*s) return cap;
    if (at < cap && quote) out[at++] = '"';
    if (at + 1 < cap) out[at++] = last ? '\n' : ',';
    else return cap;
    return at;
}

int logbook_csv_row(const char *const *cells, int n, char *out, size_t cap)
{
    size_t at = 0;
    for (int i = 0; i < n && at < cap; ++i) at = csv_cell(out, cap, at, cells[i] ? cells[i] : "", i == n - 1);
    if (at >= cap) return -1;
    out[at] = 0;
    return (int)at;
}

int logbook_csv(const logrec_t *r, int header, char *out, size_t cap)
{
    size_t at = 0;
    if (header) {
        int n = snprintf(out, cap, "time,channel,state,source,user,type,detail,id\n");
        if (n < 0 || (size_t)n >= cap) return -1;
        at = (size_t)n;
    }
    char when[32] = "", ch[12] = "";
    struct tm tm;
    if (localtime_r(&r->time, &tm)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
    if (r->ch > 0) snprintf(ch, sizeof ch, "%d", r->ch);
    const char *state = r->active == 1 ? "start" : r->active == 0 ? "stop" : "";
    const char *cells[8] = { when, ch, state, r->source, r->who, r->type, r->detail, r->id };
    int n = logbook_csv_row(cells, 8, out + at, cap - at);
    return n < 0 ? -1 : (int)at + n;
}

/* ---- matching -------------------------------------------------------------------------- */

static int has_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; ++hay)
        if (!strncasecmp(hay, needle, n)) return 1;
    return n == 0;
}

static int type_in(const char *type, const char *list)
{
    size_t tl = strlen(type);
    for (const char *p = list; *p;) {
        size_t n = strcspn(p, ",");
        if (n == tl && !strncmp(p, type, n)) return 1;
        p += n;
        if (*p == ',') ++p;
    }
    return 0;
}

int logbook_match(const logrec_t *r, const logfilter_t *f)
{
    if (!f) return 1;
    if (f->from && r->time < f->from) return 0;
    if (f->to && r->time > f->to) return 0;
    if (f->ch >= 0 && r->ch != f->ch) return 0;
    if (f->type && f->type[0] && !type_in(r->type, f->type)) return 0;
    if (f->who && f->who[0] && strcmp(r->who, f->who)) return 0;
    if (f->source && f->source[0] && strcmp(r->source, f->source)) return 0;
    if (f->text && f->text[0] && !has_ci(r->detail, f->text) && !has_ci(r->type, f->text) &&
        !has_ci(r->who, f->text) && !has_ci(r->source, f->text))
        return 0;
    return 1;
}

/* ---- the book -------------------------------------------------------------------------- */

logbook_t *logbook_create(const logbook_conf_t *conf)
{
    pthread_once(&g_boot_once, boot_init);
    if (!conf || conf->ram < 1) return NULL;
    logbook_t *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->conf = *conf;
    snprintf(b->name, sizeof b->name, "%s", conf->name ? conf->name : "log");
    b->conf.name = b->name;
    if (b->conf.files < 1) b->conf.files = 1;
    if (b->conf.file_cap < 4096) b->conf.file_cap = 4096;
    b->ring = calloc((size_t)conf->ram, sizeof *b->ring);
    if (!b->ring) {
        free(b);
        return NULL;
    }
    pthread_mutex_init(&b->lock, NULL);
    pthread_mutex_init(&b->io, NULL);
    b->fd = -1;
    b->next_n = 1;
    return b;
}

void logbook_destroy(logbook_t *b)
{
    if (!b) return;
    if (b->fd >= 0) close(b->fd);
    pthread_mutex_destroy(&b->lock);
    pthread_mutex_destroy(&b->io);
    free(b->ring);
    free(b);
}

static unsigned long kept(const logbook_t *b)
{
    return b->total < (unsigned long)b->conf.ram ? b->total : (unsigned long)b->conf.ram;
}

/* Ring record `i` (0 .. total-1, still kept). Caller holds the lock. */
static logrec_t *at(logbook_t *b, unsigned long i) { return &b->ring[i % (unsigned long)b->conf.ram]; }

void logbook_push(logbook_t *b, const logrec_t *r)
{
    if (!b || !r) return;
    logrec_t c = *r;
    if (!c.time) c.time = time(NULL);
    c.source[sizeof c.source - 1] = c.who[sizeof c.who - 1] = c.type[sizeof c.type - 1] = 0;
    c.detail[sizeof c.detail - 1] = 0;
    pthread_mutex_lock(&b->lock);
    snprintf(c.id, sizeof c.id, "%08x.%lu", g_boot, b->next_n++);
    *at(b, b->total) = c;
    ++b->total;
    ++b->changes;
    b->flash_dirty = 1;
    pthread_mutex_unlock(&b->lock);
}

void logbook_clear(logbook_t *b)
{
    pthread_mutex_lock(&b->lock);
    b->total = b->disk_upto = 0;
    ++b->changes;
    pthread_mutex_unlock(&b->lock);
}

unsigned long logbook_count(logbook_t *b)
{
    pthread_mutex_lock(&b->lock);
    unsigned long n = b->changes;
    pthread_mutex_unlock(&b->lock);
    return n;
}

unsigned long logbook_in_memory(logbook_t *b)
{
    pthread_mutex_lock(&b->lock);
    unsigned long n = kept(b);
    pthread_mutex_unlock(&b->lock);
    return n;
}

/* Make the ring `recs` (oldest first, `n` of them; the newest `ram` are
 * kept). Caller holds the lock. */
static void ring_set(logbook_t *b, const logrec_t *recs, unsigned long n)
{
    unsigned long ram = (unsigned long)b->conf.ram, first = n > ram ? n - ram : 0;
    b->total = 0;
    ++b->changes;
    for (unsigned long i = first; i < n; ++i) *at(b, b->total++) = recs[i];
}

/* ---- the disk's files, backwards ------------------------------------------------------- */


/* Every record of `path`, newest first, to `fn`; 1 when `fn` stopped it. */
static int read_back(const char *path, logrec_fn fn, void *user)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char *buf = malloc(CHUNK + LINE_MAX_ + 1);
    if (!buf) {
        close(fd);
        return 0;
    }
    off_t pos = lseek(fd, 0, SEEK_END);
    size_t pend = 0;                    /* the start of a line, from the chunk after this one */
    int stopped = 0;
    while (!stopped && (pos > 0 || pend)) {
        size_t want = pos > CHUNK ? CHUNK : (size_t)pos;
        if (want) {
            memmove(buf + want, buf, pend);
            pos -= (off_t)want;
            ssize_t got = pread(fd, buf, want, pos);
            if (got != (ssize_t)want) break;
        }
        size_t len = want + pend, end = len;
        for (size_t i = len; i-- > 0 && !stopped;) {
            if (buf[i] != '\n') continue;
            logrec_t r;
            if (end > i + 1 && logbook_parse(buf + i + 1, end - i - 1, &r) == 0) stopped = fn(user, &r);
            end = i;
        }
        if (stopped) break;
        if (pos == 0) {                 /* the file's first line */
            logrec_t r;
            if (end && logbook_parse(buf, end, &r) == 0) stopped = fn(user, &r);
            break;
        }
        pend = end <= LINE_MAX_ ? end : 0;      /* a "line" longer than that is dropped */
    }
    free(buf);
    close(fd);
    return stopped;
}

static void file_name(const logbook_t *b, int gen, char *out, size_t cap)
{
    if (gen) snprintf(out, cap, "%s/%s.log.%d", b->disk, b->name, gen);
    else snprintf(out, cap, "%s/%s.log", b->disk, b->name);
}

/* Every record on the disk, newest first (caller holds io). */
static int disk_back(logbook_t *b, logrec_fn fn, void *user)
{
    for (int g = 0; g < b->conf.files; ++g) {
        char p[320];
        file_name(b, g, p, sizeof p);
        if (read_back(p, fn, user)) return 1;
    }
    return 0;
}

/* ---- queries --------------------------------------------------------------------------- */

typedef struct {
    const logfilter_t *f;
    logrec_fn          fn;
    void              *user;
    char               until[LOG_ID];   /* skip the disk's records until past this one (in RAM) */
} each_t;

static int each_disk_cb(void *user, const logrec_t *r)
{
    each_t *e = user;
    if (e->until[0]) {
        if (!strcmp(r->id, e->until)) e->until[0] = 0;
        return 0;
    }
    return logbook_match(r, e->f) ? e->fn(e->user, r) : 0;
}

int logbook_each(logbook_t *b, const logfilter_t *f, logrec_fn fn, void *user)
{
    if (!b) return 0;
    each_t e = { .f = f, .fn = fn, .user = user };
    /* A copy of the ring, so that `fn` (a network write, a file) runs
     * without the lock that every push takes. */
    pthread_mutex_lock(&b->lock);
    unsigned long n = kept(b);
    logrec_t *copy = malloc((n ? n : 1) * sizeof *copy);
    for (unsigned long i = 0; copy && i < n; ++i) copy[i] = *at(b, b->total - 1 - i);
    if (n) snprintf(e.until, sizeof e.until, "%s", at(b, b->total - n)->id);
    int full = b->total >= (unsigned long)b->conf.ram;
    pthread_mutex_unlock(&b->lock);
    if (!copy) return 0;
    int stopped = 0;
    for (unsigned long i = 0; i < n && !stopped; ++i)
        if (logbook_match(&copy[i], f)) stopped = fn(user, &copy[i]);
    free(copy);
    if (stopped) return 1;
    /* Older ones are on the disk only once the ring has let some go. */
    if (!full) return 0;
    pthread_mutex_lock(&b->io);
    int on_disk = b->disk[0] != 0;
    if (on_disk && !(f && f->ram_only)) stopped = disk_back(b, each_disk_cb, &e);
    pthread_mutex_unlock(&b->io);
    return stopped ? 1 : on_disk && f && f->ram_only ? 2 : 0;
}

typedef struct {
    int       skip, limit, n, more;
    logrec_t *out;
} query_t;

static int query_cb(void *user, const logrec_t *r)
{
    query_t *q = user;
    if (q->skip > 0) {
        --q->skip;
        return 0;
    }
    if (q->n < q->limit) {
        q->out[q->n++] = *r;
        return 0;
    }
    q->more = 1;
    return 1;
}

int logbook_query(logbook_t *b, const logfilter_t *f, int offset, int limit, logrec_t *out, int *more)
{
    query_t q = { .skip = offset < 0 ? 0 : offset, .limit = limit < 0 ? 0 : limit, .out = out };
    int rc = logbook_each(b, f, query_cb, &q);
    if (more) *more = q.more || rc == 2;       /* RAM only: there may be more on the disk */
    return q.n;
}

int logbook_text(const logrec_t *r, char *out, size_t cap)
{
    char when[32] = "-", ch[16] = "", who[LOG_WHO + LOG_SOURCE + 4];
    struct tm tm;
    if (localtime_r(&r->time, &tm)) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", &tm);
    if (r->ch > 0) snprintf(ch, sizeof ch, "CAM%d ", r->ch);
    snprintf(who, sizeof who, "%s%s%s", r->who, r->who[0] ? "/" : "", r->source);
    int n = snprintf(out, cap, "%s  %s%s%s  %s  %s\n", when, ch, r->type,
                     r->active == 1 ? " start" : r->active == 0 ? " stop" : "", who, r->detail);
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

/* ---- the flash copy -------------------------------------------------------------------- */

void logbook_set_flash(logbook_t *b, const char *path)
{
    if (!b) return;
    pthread_mutex_lock(&b->io);
    snprintf(b->flash, sizeof b->flash, "%s", path ? path : "");
    b->flash_at = 0;
    char *text = NULL;
    long len = 0;
    int fd = b->flash[0] ? open(b->flash, O_RDONLY | O_CLOEXEC) : -1;
    struct stat st;
    if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size <= (off_t)FLASH_MAX &&
        (text = malloc((size_t)st.st_size + 1)) != NULL) {
        len = (long)read(fd, text, (size_t)st.st_size);
        if (len < 0) len = 0;
        text[len] = 0;
    }
    if (fd >= 0) close(fd);
    pthread_mutex_unlock(&b->io);
    if (!len) {
        free(text);
        return;
    }
    unsigned long cap = (unsigned long)b->conf.ram;
    logrec_t *recs = calloc(cap, sizeof *recs);
    unsigned long n = 0;
    for (char *p = text; recs && *p;) {
        char *nl = strchr(p, '\n');
        size_t l = nl ? (size_t)(nl - p) : strlen(p);
        if (n < cap && logbook_parse(p, l, &recs[n]) == 0) ++n;
        p += l + (nl != NULL);
    }
    free(text);
    if (!recs) return;
    /* Before what was pushed already (since the start). */
    pthread_mutex_lock(&b->lock);
    unsigned long have = kept(b);
    logrec_t *all = calloc(n + have, sizeof *all);
    if (all) {
        memcpy(all, recs, n * sizeof *recs);
        for (unsigned long i = 0; i < have; ++i) all[n + i] = *at(b, b->total - have + i);
        ring_set(b, all, n + have);
        b->disk_upto = 0;
    }
    pthread_mutex_unlock(&b->lock);
    free(all);
    free(recs);
}

static void flash_write(logbook_t *b)
{
    int keep = b->conf.flash_keep;
    logrec_t *recs = calloc((size_t)keep, sizeof *recs);
    size_t cap = (size_t)keep * (LOG_ID + LOG_SOURCE + LOG_WHO + LOG_TYPE + 2 * LOG_DETAIL + 64);
    char *text = malloc(cap);
    if (!recs || !text) {
        free(recs);
        free(text);
        return;
    }
    pthread_mutex_lock(&b->lock);
    unsigned long n = kept(b);
    if (n > (unsigned long)keep) n = (unsigned long)keep;
    for (unsigned long i = 0; i < n; ++i) recs[i] = *at(b, b->total - n + i);
    b->flash_dirty = 0;
    pthread_mutex_unlock(&b->lock);
    size_t len = 0;
    for (unsigned long i = 0; i < n; ++i) {
        int l = logbook_format(&recs[i], text + len, cap - len);
        if (l > 0) len += (size_t)l;
    }
    char err[300];
    if (config_write_atomic(b->flash, text, len, err, sizeof err) < 0) {
        vg_log("logs: %s: %s", b->name, err);
        pthread_mutex_lock(&b->lock);
        b->flash_dirty = 1;
        pthread_mutex_unlock(&b->lock);
    }
    free(recs);
    free(text);
}

/* ---- the disk -------------------------------------------------------------------------- */

static int write_all(int fd, const char *p, size_t len)
{
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int open_current(logbook_t *b)
{
    char p[320];
    file_name(b, 0, p, sizeof p);
    b->fd = open(p, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (b->fd < 0) return -1;
    struct stat st;
    b->fsize = fstat(b->fd, &st) == 0 ? (long)st.st_size : 0;
    /* A line cut short by a power cut: the next one starts on its own. */
    char last = '\n';
    if (b->fsize > 0 && pread(b->fd, &last, 1, b->fsize - 1) == 1 && last != '\n' && write_all(b->fd, "\n", 1) == 0)
        ++b->fsize;
    return 0;
}

static void rotate(logbook_t *b)
{
    close(b->fd);
    b->fd = -1;
    for (int g = b->conf.files - 1; g > 0; --g) {
        char from[320], to[320];
        file_name(b, g - 1, from, sizeof from);
        file_name(b, g, to, sizeof to);
        if (rename(from, to) < 0 && errno != ENOENT) vg_log("logs: cannot rename %s: %s", from, strerror(errno));
    }
    if (b->conf.files == 1) {
        char p[320];
        file_name(b, 0, p, sizeof p);
        unlink(p);
    }
    if (open_current(b) < 0) {
        vg_log("logs: %s: cannot open a new file in %s: %s; not on the disk any more", b->name, b->disk,
               strerror(errno));
        b->disk[0] = 0;
    }
}

/* Let the disk go after a failure (caller holds io). */
static void disk_drop(logbook_t *b, const char *what)
{
    vg_log("logs: %s: %s %s: %s; kept in memory until the disk is back", b->name, what, b->disk, strerror(errno));
    if (b->fd >= 0) close(b->fd);
    b->fd = -1;
    b->disk[0] = 0;
}

typedef struct {
    logrec_t     *recs;             /* the disk's newest, newest first */
    unsigned long n, cap;
} tail_t;

static int tail_cb(void *user, const logrec_t *r)
{
    tail_t *t = user;
    t->recs[t->n++] = *r;
    return t->n >= t->cap;
}

static size_t lines_of(const logrec_t *recs, unsigned long n, char **out)
{
    size_t cap = (size_t)n * 512 + 1, len = 0;
    char *text = malloc(cap);
    *out = text;
    if (!text) return 0;
    for (unsigned long i = 0; i < n; ++i) {
        int l = logbook_format(&recs[i], text + len, cap - len);
        if (l > 0) len += (size_t)l;
    }
    return len;
}

int logbook_set_disk(logbook_t *b, const char *dir)
{
    if (!b) return -1;
    pthread_mutex_lock(&b->io);
    if (b->fd >= 0) close(b->fd);
    b->fd = -1;
    b->disk[0] = 0;
    if (!dir || !*dir) {
        pthread_mutex_unlock(&b->io);
        return 0;
    }
    if (mkdir(dir, 0700) < 0 && errno != EEXIST) {
        int e = errno;
        pthread_mutex_unlock(&b->io);
        errno = e;
        return -1;
    }
    snprintf(b->disk, sizeof b->disk, "%s", dir);
    if (open_current(b) < 0) {
        int e = errno;
        b->disk[0] = 0;
        pthread_mutex_unlock(&b->io);
        errno = e;
        return -1;
    }
    /* What the disk has (its newest, as many as the ring holds) ... */
    unsigned long ram = (unsigned long)b->conf.ram;
    tail_t t = { calloc(ram, sizeof(logrec_t)), 0, ram };
    if (t.recs) disk_back(b, tail_cb, &t);
    /* ... and what RAM has that it lacks: written there, in order. */
    pthread_mutex_lock(&b->lock);
    unsigned long n = kept(b), mark = b->total;
    logrec_t *mine = calloc(n ? n : 1, sizeof *mine);
    for (unsigned long i = 0; mine && i < n; ++i) mine[i] = *at(b, b->total - n + i);
    pthread_mutex_unlock(&b->lock);
    unsigned long missing = 0;
    for (unsigned long i = 0; mine && t.recs && i < n; ++i) {
        int found = 0;
        for (unsigned long k = 0; k < t.n && !found; ++k) found = !strcmp(t.recs[k].id, mine[i].id);
        if (!found) mine[missing++] = mine[i];
    }
    int rc = 0;
    if (!mine || !t.recs) {
        rc = -1;
        errno = ENOMEM;
    } else if (missing) {
        char *text;
        size_t len = lines_of(mine, missing, &text);
        if (!text || write_all(b->fd, text, len) < 0) {
            rc = -1;
            if (!text) errno = ENOMEM;
        } else {
            b->fsize += (long)len;
        }
        free(text);
    }
    if (rc < 0) {
        int e = errno;
        disk_drop(b, "cannot write to");
        free(mine);
        free(t.recs);
        pthread_mutex_unlock(&b->io);
        errno = e;
        return -1;
    }
    /* The ring: the disk's newest, what was written now, and what came
     * meanwhile (not on the disk yet). */
    pthread_mutex_lock(&b->lock);
    unsigned long since = b->total - mark, all_n = t.n + missing + since;
    if (since > ram) since = ram;
    logrec_t *all = calloc(all_n ? all_n : 1, sizeof *all);
    if (all) {
        unsigned long k = 0;
        for (unsigned long i = t.n; i-- > 0;) all[k++] = t.recs[i];
        for (unsigned long i = 0; i < missing; ++i) all[k++] = mine[i];
        for (unsigned long i = 0; i < since; ++i) all[k++] = *at(b, b->total - since + i);
        ring_set(b, all, k);
        b->disk_upto = b->total - since;
        b->flash_dirty = 1;
    }
    pthread_mutex_unlock(&b->lock);
    free(all);
    free(mine);
    free(t.recs);
    if (b->fsize >= b->conf.file_cap) rotate(b);
    pthread_mutex_unlock(&b->io);
    return 0;
}

void logbook_disk(logbook_t *b, char *out, size_t cap)
{
    pthread_mutex_lock(&b->io);
    snprintf(out, cap, "%s", b->disk);
    pthread_mutex_unlock(&b->io);
}

void logbook_sync(logbook_t *b, time_t now, int final)
{
    if (!b) return;
    pthread_mutex_lock(&b->io);
    if (b->fd >= 0) {
        pthread_mutex_lock(&b->lock);
        unsigned long first = b->disk_upto, last = b->total, n = kept(b);
        if (last - first > n) {
            if (!b->lost_told)
                vg_log("logs: %s: %lu record(s) came faster than they could be written; lost on the disk",
                       b->name, last - first - n);
            b->lost_told = 1;
            first = last - n;
        }
        unsigned long cnt = last - first;
        logrec_t *recs = cnt ? malloc(cnt * sizeof *recs) : NULL;
        for (unsigned long i = 0; recs && i < cnt; ++i) recs[i] = *at(b, first + i);
        pthread_mutex_unlock(&b->lock);
        if (recs) {
            char *text;
            size_t len = lines_of(recs, cnt, &text);
            if (text && write_all(b->fd, text, len) == 0) {
                b->fsize += (long)len;
                pthread_mutex_lock(&b->lock);
                b->disk_upto = last;
                pthread_mutex_unlock(&b->lock);
                if (final) fsync(b->fd);
                if (b->fsize >= b->conf.file_cap) rotate(b);
            } else if (text) {
                disk_drop(b, "cannot write to");
            }
            free(text);
            free(recs);
        } else if (final) {
            fsync(b->fd);
        }
    }
    if (b->flash[0] && b->conf.flash_keep > 0 && !(b->conf.flash_no_disk && b->fd >= 0)) {
        if (!b->flash_at) b->flash_at = now;    /* the first copy one period after the start */
        pthread_mutex_lock(&b->lock);
        int dirty = b->flash_dirty;
        pthread_mutex_unlock(&b->lock);
        if (dirty && (final || now - b->flash_at >= b->conf.flash_secs)) {
            flash_write(b);
            b->flash_at = now;
        }
    }
    pthread_mutex_unlock(&b->io);
}
