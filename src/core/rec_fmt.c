/* SPDX-License-Identifier: GPL-3.0-or-later */
/* rec_fmt.c — the recording store's records (rec_fmt.h). */
#include <string.h>

#include "rec_fmt.h"

/* Random, once: vigil's 'uuid' box types. */
const uint8_t REC_UUID_VGCH[16] = { 0x76, 0x67, 0x63, 0x68, 0x9b, 0x1e, 0x4c, 0x0a,
                                    0xa5, 0x2d, 0x61, 0x7f, 0x0e, 0x3c, 0xd2, 0x41 };
const uint8_t REC_UUID_VGIX[16] = { 0x76, 0x67, 0x69, 0x78, 0x3f, 0x82, 0x47, 0x5b,
                                    0x91, 0x0c, 0xe8, 0x24, 0x6a, 0xb7, 0x13, 0x95 };

/* CRC32C (Castagnoli), reflected 0x82F63B78: the one ext4 and iSCSI use. */
static uint32_t k_crc[256];
static int k_crc_ready;

static void crc_init(void)
{
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? (c >> 1) ^ 0x82F63B78u : c >> 1;
        k_crc[i] = c;
    }
    k_crc_ready = 1;
}

uint32_t rec_crc32c(uint32_t crc, const void *data, size_t len)
{
    /* The table is the same whoever fills it: a race only fills it twice. */
    if (!k_crc_ready) crc_init();
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) crc = k_crc[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t rec_crc_seed(uint64_t pool_id)
{
    uint8_t b[8];
    rec_put64(b, pool_id);
    return rec_crc32c(0, b, sizeof b);
}

/* ---- chunk record ---- */

_Static_assert(REC_CHUNK_REC == 64 && REC_JREC == 32 && REC_EVREC == 32 && REC_FRAG == 16, "record sizes");
_Static_assert(REC_VGCH_B >= 512 && REC_VGCH_B + REC_CHUNK_REC <= REC_VGCH_SIZE, "vgch copies in different sectors");

void rec_chunk_encode(const rec_chunk_t *c, uint64_t pool_id, uint8_t o[REC_CHUNK_REC])
{
    memset(o, 0, REC_CHUNK_REC);
    o[0] = c->state;
    o[1] = c->cam;
    o[2] = c->stream;
    o[3] = c->codec;
    rec_put32(o + 4, c->flags);
    rec_put32(o + 8, c->use);
    rec_put32(o + 12, c->nfrag);
    rec_put64(o + 16, c->start_us);
    rec_put64(o + 24, c->end_us);
    rec_put32(o + 32, c->bytes);
    rec_put32(o + 36, c->init_len);
    rec_put16(o + 40, c->width);
    rec_put16(o + 42, c->height);
    rec_put32(o + 44, c->last_off);
    rec_put32(o + 48, c->vgix_len);
    rec_put32(o + 52, c->no);
    rec_put32(o + 60, rec_crc32c(rec_crc_seed(pool_id), o, 60));
}

int rec_chunk_decode(rec_chunk_t *c, uint64_t pool_id, const uint8_t in[REC_CHUNK_REC])
{
    if (rec_get32(in + 60) != rec_crc32c(rec_crc_seed(pool_id), in, 60)) return -1;
    memset(c, 0, sizeof *c);
    c->state = in[0];
    c->cam = in[1];
    c->stream = in[2];
    c->codec = in[3];
    c->flags = rec_get32(in + 4);
    c->use = rec_get32(in + 8);
    c->nfrag = rec_get32(in + 12);
    c->start_us = rec_get64(in + 16);
    c->end_us = rec_get64(in + 24);
    c->bytes = rec_get32(in + 32);
    c->init_len = rec_get32(in + 36);
    c->width = rec_get16(in + 40);
    c->height = rec_get16(in + 42);
    c->last_off = rec_get32(in + 44);
    c->vgix_len = rec_get32(in + 48);
    c->no = rec_get32(in + 52);
    return c->state <= RC_BAD ? 0 : -1;
}

/* ---- vgch ---- */

#define VGCH_MAGIC "VGCH"
#define VGCH_VERSION 1

void rec_vgch_build(uint8_t box[REC_VGCH_SIZE], uint64_t pool_id, const rec_chunk_t *a, const rec_chunk_t *b)
{
    memset(box, 0, REC_VGCH_SIZE);
    box[0] = (uint8_t)(REC_VGCH_SIZE >> 24);
    box[1] = (uint8_t)(REC_VGCH_SIZE >> 16);
    box[2] = (uint8_t)(REC_VGCH_SIZE >> 8);
    box[3] = (uint8_t)REC_VGCH_SIZE;
    memcpy(box + 4, "uuid", 4);
    memcpy(box + 8, REC_UUID_VGCH, 16);
    memcpy(box + 24, VGCH_MAGIC, 4);
    rec_put16(box + 28, VGCH_VERSION);
    rec_put64(box + 32, pool_id);
    rec_put32(box + 40, rec_crc32c(0, box, 40));
    if (a) rec_chunk_encode(a, pool_id, box + REC_VGCH_A);
    if (b) rec_chunk_encode(b, pool_id, box + REC_VGCH_B);
}

uint64_t rec_vgch_pool(const uint8_t box[REC_VGCH_SIZE])
{
    if (rec_be32(box) != REC_VGCH_SIZE || memcmp(box + 4, "uuid", 4) || memcmp(box + 8, REC_UUID_VGCH, 16) ||
        memcmp(box + 24, VGCH_MAGIC, 4) || rec_get32(box + 40) != rec_crc32c(0, box, 40))
        return 0;
    return rec_get64(box + 32);
}

int rec_vgch_parse(const uint8_t box[REC_VGCH_SIZE], uint64_t pool_id, rec_chunk_t *out)
{
    if (!pool_id || rec_vgch_pool(box) != pool_id) return -1;
    rec_chunk_t a, b;
    int ha = rec_chunk_decode(&a, pool_id, box + REC_VGCH_A) == 0;
    int hb = rec_chunk_decode(&b, pool_id, box + REC_VGCH_B) == 0;
    /* B is the close of A's use; a B left from an earlier use (A
     * rewritten at the reuse, B not yet) is not this use's. */
    if (hb && (!ha || b.use == a.use)) {
        *out = b;
        return 0;
    }
    if (ha) {
        *out = a;
        return 0;
    }
    return -1;
}

/* ---- journal ---- */

void rec_jrec_encode(const rec_jrec_t *j, uint64_t pool_id, uint8_t o[REC_JREC])
{
    memset(o, 0, REC_JREC);
    o[0] = j->type;
    o[1] = j->a;
    o[2] = j->b;
    o[3] = j->c;
    rec_put32(o + 4, j->chunk);
    rec_put32(o + 8, j->use);
    if (j->type == RJ_FRAG) {
        rec_put32(o + 12, j->w0);
        rec_put32(o + 16, j->w1);
        rec_put32(o + 20, j->w2);
        rec_put32(o + 24, j->w3);
    } else {
        rec_put64(o + 12, j->u64);
        rec_put32(o + 20, j->w0);
        rec_put32(o + 24, j->w1);
    }
    rec_put32(o + 28, rec_crc32c(rec_crc_seed(pool_id), o, 28));
}

int rec_jrec_decode(rec_jrec_t *j, uint64_t pool_id, const uint8_t in[REC_JREC])
{
    if (rec_get32(in + 28) != rec_crc32c(rec_crc_seed(pool_id), in, 28)) return -1;
    memset(j, 0, sizeof *j);
    j->type = in[0];
    j->a = in[1];
    j->b = in[2];
    j->c = in[3];
    j->chunk = rec_get32(in + 4);
    j->use = rec_get32(in + 8);
    if (j->type == RJ_FRAG) {
        j->w0 = rec_get32(in + 12);
        j->w1 = rec_get32(in + 16);
        j->w2 = rec_get32(in + 20);
        j->w3 = rec_get32(in + 24);
    } else {
        j->u64 = rec_get64(in + 12);
        j->w0 = rec_get32(in + 20);
        j->w1 = rec_get32(in + 24);
    }
    return j->type >= RJ_OPEN && j->type <= RJ_FREE ? 0 : -1;
}

/* ---- fragments ---- */

void rec_frag_encode(const rec_frag_t *f, uint8_t o[REC_FRAG])
{
    rec_put32(o, f->t_ms);
    rec_put32(o + 4, f->dur_ms);
    rec_put32(o + 8, f->off);
    rec_put32(o + 12, f->size);
}

void rec_frag_decode(rec_frag_t *f, const uint8_t in[REC_FRAG])
{
    f->t_ms = rec_get32(in);
    f->dur_ms = rec_get32(in + 4);
    f->off = rec_get32(in + 8);
    f->size = rec_get32(in + 12);
}

/* ---- events ---- */

static const char *const k_ev_names[REC_EV_TYPES] = {
    NULL, "motion", "human", "face", "vehicle", "line", "intrusion", "video_loss", "tamper", "alarm_in", "disk",
};

const char *rec_event_name(int type)
{
    return type > 0 && type < REC_EV_TYPES ? k_ev_names[type] : NULL;
}

int rec_event_type(const char *name)
{
    for (int t = 1; t < REC_EV_TYPES; ++t)
        if (!strcmp(name, k_ev_names[t])) return t;
    return 0;
}

void rec_event_encode(const rec_event_t *e, uint64_t pool_id, uint8_t o[REC_EVREC])
{
    memset(o, 0, REC_EVREC);
    rec_put64(o, e->time_us);
    rec_put32(o + 8, e->dur_ms);
    o[12] = e->cam;
    o[13] = e->type;
    rec_put16(o + 14, e->detail);
    rec_put32(o + 16, e->chunk);
    rec_put32(o + 20, e->frag);
    rec_put32(o + 24, e->seq);
    rec_put32(o + 28, rec_crc32c(rec_crc_seed(pool_id), o, 28));
}

int rec_event_decode(rec_event_t *e, uint64_t pool_id, const uint8_t in[REC_EVREC])
{
    if (rec_get32(in + 28) != rec_crc32c(rec_crc_seed(pool_id), in, 28)) return -1;
    e->time_us = rec_get64(in);
    e->dur_ms = rec_get32(in + 8);
    e->cam = in[12];
    e->type = in[13];
    e->detail = rec_get16(in + 14);
    e->chunk = rec_get32(in + 16);
    e->frag = rec_get32(in + 20);
    e->seq = rec_get32(in + 24);
    return 0;
}

/* ---- headers ---- */

void rec_table_hdr_encode(const rec_table_hdr_t *h, uint8_t o[REC_TABLE_HDR])
{
    memset(o, 0, REC_TABLE_HDR);
    memcpy(o, "VGCT", 4);
    rec_put16(o + 4, 1);
    rec_put16(o + 6, REC_CHUNK_REC);
    rec_put64(o + 8, h->pool_id);
    rec_put64(o + 16, h->generation);
    rec_put32(o + 24, h->chunk_bytes);
    rec_put32(o + 28, h->n);
    rec_put32(o + 32, h->records_crc);
    rec_put32(o + 60, rec_crc32c(0, o, 60));
}

int rec_table_hdr_decode(rec_table_hdr_t *h, const uint8_t in[REC_TABLE_HDR])
{
    if (memcmp(in, "VGCT", 4) || rec_get16(in + 4) != 1 || rec_get16(in + 6) != REC_CHUNK_REC ||
        rec_get32(in + 60) != rec_crc32c(0, in, 60))
        return -1;
    h->pool_id = rec_get64(in + 8);
    h->generation = rec_get64(in + 16);
    h->chunk_bytes = rec_get32(in + 24);
    h->n = rec_get32(in + 28);
    h->records_crc = rec_get32(in + 32);
    return 0;
}

void rec_journal_hdr_encode(const rec_journal_hdr_t *h, uint8_t o[REC_JOURNAL_HDR])
{
    memset(o, 0, REC_JOURNAL_HDR);
    memcpy(o, "VGJN", 4);
    rec_put16(o + 4, 1);
    rec_put64(o + 8, h->pool_id);
    rec_put64(o + 16, h->generation);
    rec_put32(o + 28, rec_crc32c(0, o, 28));
}

int rec_journal_hdr_decode(rec_journal_hdr_t *h, const uint8_t in[REC_JOURNAL_HDR])
{
    if (memcmp(in, "VGJN", 4) || rec_get16(in + 4) != 1 || rec_get32(in + 28) != rec_crc32c(0, in, 28)) return -1;
    h->pool_id = rec_get64(in + 8);
    h->generation = rec_get64(in + 16);
    return 0;
}

void rec_evlog_hdr_encode(const rec_evlog_hdr_t *h, uint8_t o[64])
{
    memset(o, 0, 64);
    memcpy(o, "VGEV", 4);
    rec_put16(o + 4, 1);
    rec_put16(o + 6, REC_EVREC);
    rec_put64(o + 8, h->pool_id);
    rec_put64(o + 16, h->generation);
    rec_put32(o + 24, h->cap);
    rec_put32(o + 28, h->head);
    rec_put32(o + 60, rec_crc32c(0, o, 60));
}

int rec_evlog_hdr_decode(rec_evlog_hdr_t *h, const uint8_t in[64])
{
    if (memcmp(in, "VGEV", 4) || rec_get16(in + 4) != 1 || rec_get16(in + 6) != REC_EVREC ||
        rec_get32(in + 60) != rec_crc32c(0, in, 60))
        return -1;
    h->pool_id = rec_get64(in + 8);
    h->generation = rec_get64(in + 16);
    h->cap = rec_get32(in + 24);
    h->head = rec_get32(in + 28);
    return 0;
}

/* ---- vgix ---- */
/* [size BE]['uuid'][uuid 16]["VGIX"][u16 version][u16 0][u64 pool][u32 use]
 * [u32 nfrag][u32 nev][u32 CRC of the entries][fragments][events] */

_Static_assert(REC_VGIX_HDR == 56, "vgix header");

size_t rec_vgix_size(uint32_t nfrag, uint32_t nev)
{
    return REC_VGIX_HDR + (size_t)nfrag * REC_FRAG + (size_t)nev * REC_EVREC;
}

void rec_vgix_build(uint8_t *o, uint64_t pool_id, uint32_t use, const rec_frag_t *f, uint32_t nfrag,
                    const rec_event_t *ev, uint32_t nev)
{
    size_t size = rec_vgix_size(nfrag, nev);
    memset(o, 0, REC_VGIX_HDR);
    o[0] = (uint8_t)(size >> 24);
    o[1] = (uint8_t)(size >> 16);
    o[2] = (uint8_t)(size >> 8);
    o[3] = (uint8_t)size;
    memcpy(o + 4, "uuid", 4);
    memcpy(o + 8, REC_UUID_VGIX, 16);
    memcpy(o + 24, "VGIX", 4);
    rec_put16(o + 28, 1);
    rec_put64(o + 32, pool_id);
    rec_put32(o + 40, use);
    rec_put32(o + 44, nfrag);
    rec_put32(o + 48, nev);
    uint8_t *p = o + REC_VGIX_HDR;
    for (uint32_t i = 0; i < nfrag; ++i, p += REC_FRAG) rec_frag_encode(&f[i], p);
    for (uint32_t i = 0; i < nev; ++i, p += REC_EVREC) rec_event_encode(&ev[i], pool_id, p);
    uint32_t crc = rec_crc32c(rec_crc_seed(pool_id), o, 52);
    rec_put32(o + 52, rec_crc32c(crc, o + REC_VGIX_HDR, size - REC_VGIX_HDR));
}

int rec_vgix_parse(const uint8_t *box, size_t len, uint64_t pool_id, uint32_t use, uint32_t *nfrag,
                   const uint8_t **frags, uint32_t *nev, const uint8_t **events)
{
    if (len < REC_VGIX_HDR || memcmp(box + 4, "uuid", 4) || memcmp(box + 8, REC_UUID_VGIX, 16) ||
        memcmp(box + 24, "VGIX", 4) || rec_get16(box + 28) != 1 || rec_get64(box + 32) != pool_id ||
        rec_get32(box + 40) != use)
        return -1;
    uint32_t nf = rec_get32(box + 44), ne = rec_get32(box + 48);
    size_t size = rec_vgix_size(nf, ne);
    if (rec_be32(box) != size || size > len) return -1;
    uint32_t crc = rec_crc32c(rec_crc_seed(pool_id), box, 52);
    if (rec_get32(box + 52) != rec_crc32c(crc, box + REC_VGIX_HDR, size - REC_VGIX_HDR)) return -1;
    *nfrag = nf;
    *nev = ne;
    *frags = box + REC_VGIX_HDR;
    *events = box + REC_VGIX_HDR + (size_t)nf * REC_FRAG;
    return 0;
}
