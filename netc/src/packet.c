#include "netc.h"

#include <string.h>

void netc_writer_init(netc_writer *w, uint8_t *buf, size_t cap)
{
    w->data = buf;
    w->cap = cap;
    w->pos = 0;
    w->err = 0;
}

static int writer_has_room(netc_writer *w, size_t n)
{
    if (w->err || w->cap - w->pos < n) {
        w->err = 1;
        return 0;
    }
    return 1;
}

void netc_write_u8(netc_writer *w, uint8_t v)
{
    if (!writer_has_room(w, 1))
        return;
    w->data[w->pos++] = v;
}

void netc_write_u16(netc_writer *w, uint16_t v)
{
    if (!writer_has_room(w, 2))
        return;
    w->data[w->pos++] = (uint8_t)(v >> 8);
    w->data[w->pos++] = (uint8_t)(v & 0xff);
}

void netc_write_u32(netc_writer *w, uint32_t v)
{
    if (!writer_has_room(w, 4))
        return;
    w->data[w->pos++] = (uint8_t)(v >> 24);
    w->data[w->pos++] = (uint8_t)(v >> 16);
    w->data[w->pos++] = (uint8_t)(v >> 8);
    w->data[w->pos++] = (uint8_t)(v & 0xff);
}

void netc_write_bytes(netc_writer *w, const uint8_t *src, size_t n)
{
    if (n == 0 || !writer_has_room(w, n))
        return;
    memcpy(w->data + w->pos, src, n);
    w->pos += n;
}

void netc_reader_init(netc_reader *r, const uint8_t *buf, size_t len)
{
    r->data = buf;
    r->len = len;
    r->pos = 0;
    r->err = 0;
}

static int reader_has(netc_reader *r, size_t n)
{
    if (r->err || r->len - r->pos < n) {
        r->err = 1;
        return 0;
    }
    return 1;
}

uint8_t netc_read_u8(netc_reader *r)
{
    if (!reader_has(r, 1))
        return 0;
    return r->data[r->pos++];
}

uint16_t netc_read_u16(netc_reader *r)
{
    uint16_t v;
    if (!reader_has(r, 2))
        return 0;
    v = (uint16_t)((r->data[r->pos] << 8) | r->data[r->pos + 1]);
    r->pos += 2;
    return v;
}

uint32_t netc_read_u32(netc_reader *r)
{
    uint32_t v;
    if (!reader_has(r, 4))
        return 0;
    v = ((uint32_t)r->data[r->pos] << 24) | ((uint32_t)r->data[r->pos + 1] << 16) |
        ((uint32_t)r->data[r->pos + 2] << 8) | (uint32_t)r->data[r->pos + 3];
    r->pos += 4;
    return v;
}

void netc_read_bytes(netc_reader *r, uint8_t *dst, size_t n)
{
    if (n == 0 || !reader_has(r, n))
        return;
    memcpy(dst, r->data + r->pos, n);
    r->pos += n;
}

int netc_write_header(uint8_t *buf, size_t cap, const netc_header *h)
{
    netc_writer w;
    netc_writer_init(&w, buf, cap);
    netc_write_u32(&w, h->protocol_id);
    netc_write_u16(&w, h->seq);
    netc_write_u16(&w, h->ack);
    netc_write_u32(&w, h->ack_bits);
    netc_write_u8(&w, h->type);
    return w.err ? -1 : (int)w.pos;
}

int netc_read_header(const uint8_t *buf, size_t len, netc_header *h)
{
    netc_reader r;
    netc_reader_init(&r, buf, len);
    h->protocol_id = netc_read_u32(&r);
    h->seq = netc_read_u16(&r);
    h->ack = netc_read_u16(&r);
    h->ack_bits = netc_read_u32(&r);
    h->type = netc_read_u8(&r);
    if (r.err || h->protocol_id != NETC_PROTOCOL_ID)
        return -1;
    return (int)r.pos;
}

int netc_seq_greater(uint16_t a, uint16_t b)
{
    return ((a > b) && (a - b <= 32768)) || ((a < b) && (b - a > 32768));
}

int netc_is_reliable_type(uint8_t type)
{
    switch (type) {
    case NETC_MSG_JOIN:
    case NETC_MSG_ACCEPT:
    case NETC_MSG_LEAVE:
    case NETC_MSG_TAG:
    case NETC_MSG_SCORE:
        return 1;
    default:
        return 0;
    }
}
