/* SPDX-License-Identifier: GPL-3.0-or-later */
/* source_capture.c — `capture:N`: local video input N through
 * hal_capture.h. The platform shows the input in the channel's tile and
 * encodes it; its encoded access units arrive on a platform thread and
 * are queued here for the channel thread (bounded: on overflow the
 * oldest are dropped and the next AU is flagged as a discontinuity). */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hal_capture.h"
#include "source.h"

#define QLEN       32
#define QBYTES_MAX (8u << 20)

typedef struct {
    uint8_t *data;
    size_t   len;
    vg_au_t  au;
} qitem_t;

typedef struct {
    int             input;
    int             started;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    qitem_t         q[QLEN];
    int             head, n;
    size_t          bytes;
    int             dropped;
    uint8_t        *cur;            /* item being emitted */
} cap_t;

static void cap_cb(void *user, const vg_au_t *au)
{
    cap_t *c = user;
    uint8_t *copy = malloc(au->len);
    if (!copy) return;
    memcpy(copy, au->data, au->len);
    pthread_mutex_lock(&c->lock);
    while (c->n && (c->n == QLEN || c->bytes + au->len > QBYTES_MAX)) {
        qitem_t *old = &c->q[c->head];
        c->bytes -= old->len;
        free(old->data);
        c->head = (c->head + 1) % QLEN;
        --c->n;
        c->dropped = 1;
    }
    qitem_t *it = &c->q[(c->head + c->n) % QLEN];
    it->data = copy;
    it->len = au->len;
    it->au = *au;
    it->au.data = copy;
    it->au.discont = au->discont || c->dropped;
    c->dropped = 0;
    c->bytes += au->len;
    ++c->n;
    pthread_cond_signal(&c->cond);
    pthread_mutex_unlock(&c->lock);
}

static int cap_create(source_t *s)
{
    if (strncmp(s->url, "capture:", 8) != 0) return -1;
    char *end;
    long in = strtol(s->url + 8, &end, 10);
    if (end == s->url + 8 || *end || in < 0) return -1;
    cap_t *c = calloc(1, sizeof *c);
    if (!c) return -1;
    c->input = (int)in;
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->cond, NULL);
    snprintf(s->host, sizeof s->host, "input %ld", in);
    s->priv = c;
    return 0;
}

static int cap_open(source_t *s)
{
    cap_t *c = s->priv;
    if (hal_cap_start(c->input, s->index, cap_cb, c) < 0) {
        snprintf(s->err, sizeof s->err, "no capture input %d", c->input);
        return -1;
    }
    c->started = 1;
    return 0;
}

static int cap_poll(source_t *s, au_emit_fn emit, void *user)
{
    cap_t *c = s->priv;
    pthread_mutex_lock(&c->lock);
    if (!c->n) {
        struct timespec t;
        clock_gettime(CLOCK_REALTIME, &t);
        t.tv_sec += 1;
        pthread_cond_timedwait(&c->cond, &c->lock, &t);
    }
    int got = 0;
    while (c->n) {
        qitem_t it = c->q[c->head];
        c->head = (c->head + 1) % QLEN;
        --c->n;
        c->bytes -= it.len;
        pthread_mutex_unlock(&c->lock);
        s->codec = it.au.codec;
        emit(user, &it.au);
        free(it.data);
        got = 1;
        pthread_mutex_lock(&c->lock);
    }
    pthread_mutex_unlock(&c->lock);
    return got;
}

static void cap_close(source_t *s)
{
    cap_t *c = s->priv;
    if (!c) return;
    if (c->started) hal_cap_stop(c->input);
    c->started = 0;
    pthread_mutex_lock(&c->lock);
    while (c->n) {
        free(c->q[c->head].data);
        c->head = (c->head + 1) % QLEN;
        --c->n;
    }
    c->bytes = 0;
    pthread_mutex_unlock(&c->lock);
}

static void cap_destroy(source_t *s)
{
    cap_t *c = s->priv;
    if (!c) return;
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->cond);
    free(c);
    s->priv = NULL;
}

const source_ops_t source_capture_ops = {
    .scheme         = "capture",
    .create         = cap_create,
    .open           = cap_open,
    .poll           = cap_poll,
    .close          = cap_close,
    .destroy        = cap_destroy,
    .self_displayed = 1,
};
