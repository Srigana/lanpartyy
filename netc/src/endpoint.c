#include "netc.h"

#include <string.h>

#define LOST_AFTER 64

void netc_endpoint_init(netc_endpoint *ep)
{
    memset(ep, 0, sizeof(*ep));
}

static int recv_has(const netc_endpoint *ep, uint16_t seq)
{
    int i = seq % NETC_SEQ_BUFFER;
    return ep->recv_valid[i] && ep->recv_seq[i] == seq;
}

static void build_ack(const netc_endpoint *ep, netc_header *h)
{
    int i;
    h->ack = 0;
    h->ack_bits = 0;
    if (!ep->have_remote)
        return;
    h->ack = ep->remote_seq;
    for (i = 0; i < 32; i++) {
        if (recv_has(ep, (uint16_t)(ep->remote_seq - 1 - i)))
            h->ack_bits |= 1u << i;
    }
}

static void count_lost(netc_endpoint *ep, uint16_t seq)
{
    uint16_t old = (uint16_t)(seq - LOST_AFTER);
    netc_sent_entry *e = &ep->sent[old % NETC_SEQ_BUFFER];
    if (e->valid && e->seq == old && !e->acked) {
        ep->packets_lost++;
        e->valid = 0;
    }
}

static size_t write_packet(netc_endpoint *ep, uint8_t type, const uint8_t *prefix,
                           size_t prefix_len, const uint8_t *payload, size_t len,
                           uint8_t *out, size_t cap, double now, int msg_id)
{
    netc_header h;
    netc_writer w;
    netc_sent_entry *e;

    if (cap < NETC_HEADER_SIZE + prefix_len + len)
        return 0;

    h.protocol_id = NETC_PROTOCOL_ID;
    h.seq = ep->local_seq;
    h.type = type;
    build_ack(ep, &h);

    if (netc_write_header(out, cap, &h) < 0)
        return 0;
    netc_writer_init(&w, out + NETC_HEADER_SIZE, cap - NETC_HEADER_SIZE);
    netc_write_bytes(&w, prefix, prefix_len);
    netc_write_bytes(&w, payload, len);
    if (w.err)
        return 0;

    count_lost(ep, h.seq);

    e = &ep->sent[h.seq % NETC_SEQ_BUFFER];
    e->seq = h.seq;
    e->valid = 1;
    e->acked = 0;
    e->time = now;
    e->has_msg = msg_id >= 0;
    e->msg_id = msg_id >= 0 ? (uint16_t)msg_id : 0;

    ep->local_seq++;
    ep->packets_sent++;
    ep->bytes_sent += NETC_HEADER_SIZE + w.pos;
    return NETC_HEADER_SIZE + w.pos;
}

size_t netc_write_packet(netc_endpoint *ep, uint8_t type, const uint8_t *payload,
                         size_t len, uint8_t *out, size_t cap, double now)
{
    return write_packet(ep, type, NULL, 0, payload, len, out, cap, now, -1);
}

int netc_queue_reliable(netc_endpoint *ep, uint8_t type, const uint8_t *data, size_t len)
{
    uint16_t id = ep->next_send_msg;
    netc_msg_slot *slot = &ep->send_q[id % NETC_MSG_SLOTS];

    if (len > NETC_MSG_MAX || slot->used)
        return -1;

    slot->used = 1;
    slot->type = type;
    slot->id = id;
    slot->len = (uint16_t)len;
    slot->last_sent = -1.0e9;
    if (len > 0)
        memcpy(slot->data, data, len);
    ep->next_send_msg++;
    return id;
}

size_t netc_write_reliable(netc_endpoint *ep, double now, double resend_after,
                           uint8_t *out, size_t cap)
{
    int i;
    for (i = NETC_MSG_SLOTS; i > 0; i--) {
        uint16_t id = (uint16_t)(ep->next_send_msg - i);
        netc_msg_slot *slot = &ep->send_q[id % NETC_MSG_SLOTS];
        uint8_t prefix[2];
        size_t n;

        if (!slot->used || slot->id != id)
            continue;
        if (now - slot->last_sent < resend_after)
            continue;

        prefix[0] = (uint8_t)(id >> 8);
        prefix[1] = (uint8_t)(id & 0xff);
        n = write_packet(ep, slot->type, prefix, 2, slot->data, slot->len, out, cap, now, id);
        if (n > 0)
            slot->last_sent = now;
        return n;
    }
    return 0;
}

int netc_reliable_pending(const netc_endpoint *ep)
{
    int i, count = 0;
    for (i = 0; i < NETC_MSG_SLOTS; i++) {
        if (ep->send_q[i].used)
            count++;
    }
    return count;
}

static void on_acked(netc_endpoint *ep, uint16_t seq, double now, netc_packet *pkt)
{
    netc_sent_entry *e = &ep->sent[seq % NETC_SEQ_BUFFER];
    double sample;

    if (!e->valid || e->seq != seq || e->acked)
        return;

    e->acked = 1;
    ep->packets_acked++;

    sample = now - e->time;
    if (ep->rtt == 0.0)
        ep->rtt = sample;
    else
        ep->rtt += (sample - ep->rtt) * 0.1;

    if (e->has_msg) {
        netc_msg_slot *slot = &ep->send_q[e->msg_id % NETC_MSG_SLOTS];
        if (slot->used && slot->id == e->msg_id)
            slot->used = 0;
    }

    if (pkt->acked_count < NETC_ACKS_PER_PACKET)
        pkt->acked[pkt->acked_count++] = seq;
}

static void store_received(netc_endpoint *ep, uint16_t seq, int *stale)
{
    *stale = 0;
    if (!ep->have_remote) {
        ep->have_remote = 1;
        ep->remote_seq = seq;
    } else if (netc_seq_greater(seq, ep->remote_seq)) {
        uint16_t gap = (uint16_t)(seq - ep->remote_seq);
        if (gap >= NETC_SEQ_BUFFER) {
            memset(ep->recv_valid, 0, sizeof(ep->recv_valid));
        } else {
            uint16_t s;
            for (s = (uint16_t)(ep->remote_seq + 1); s != seq; s++)
                ep->recv_valid[s % NETC_SEQ_BUFFER] = 0;
        }
        ep->remote_seq = seq;
    } else {
        *stale = 1;
    }
    ep->recv_seq[seq % NETC_SEQ_BUFFER] = seq;
    ep->recv_valid[seq % NETC_SEQ_BUFFER] = 1;
}

static void store_message(netc_endpoint *ep, uint16_t id, uint8_t type,
                          const uint8_t *data, size_t len)
{
    uint16_t diff = (uint16_t)(id - ep->next_recv_msg);
    netc_msg_slot *slot;

    if (diff >= NETC_MSG_SLOTS)
        return;

    slot = &ep->recv_q[id % NETC_MSG_SLOTS];
    if (slot->used)
        return;
    slot->used = 1;
    slot->id = id;
    slot->type = type;
    slot->len = (uint16_t)len;
    if (len > 0)
        memcpy(slot->data, data, len);
}

int netc_read_packet(netc_endpoint *ep, const uint8_t *buf, size_t len, double now,
                     netc_packet *pkt)
{
    int i;
    int reliable;
    uint16_t seq;

    memset(pkt, 0, sizeof(*pkt));
    if (netc_read_header(buf, len, &pkt->header) < 0)
        return 0;

    pkt->payload = buf + NETC_HEADER_SIZE;
    pkt->payload_len = len - NETC_HEADER_SIZE;
    seq = pkt->header.seq;

    reliable = netc_is_reliable_type(pkt->header.type);
    if (reliable && (pkt->payload_len < 2 || pkt->payload_len - 2 > NETC_MSG_MAX))
        return 0;

    if (ep->have_remote) {
        if (recv_has(ep, seq))
            return 0;
        if (!netc_seq_greater(seq, ep->remote_seq) &&
            (uint16_t)(ep->remote_seq - seq) >= NETC_SEQ_BUFFER)
            return 0;
    }

    store_received(ep, seq, &pkt->stale);
    ep->packets_recv++;
    ep->bytes_recv += len;

    if (pkt->header.ack != 0 || pkt->header.ack_bits != 0) {
        on_acked(ep, pkt->header.ack, now, pkt);
        for (i = 0; i < 32; i++) {
            if (pkt->header.ack_bits & (1u << i))
                on_acked(ep, (uint16_t)(pkt->header.ack - 1 - i), now, pkt);
        }
    }

    if (reliable) {
        uint16_t id = (uint16_t)((pkt->payload[0] << 8) | pkt->payload[1]);
        pkt->payload += 2;
        pkt->payload_len -= 2;
        store_message(ep, id, pkt->header.type, pkt->payload, pkt->payload_len);
    }
    return 1;
}

int netc_pop_reliable(netc_endpoint *ep, uint8_t *type, uint8_t *data, size_t *len)
{
    netc_msg_slot *slot = &ep->recv_q[ep->next_recv_msg % NETC_MSG_SLOTS];
    if (!slot->used || slot->id != ep->next_recv_msg)
        return 0;

    *type = slot->type;
    *len = slot->len;
    if (slot->len > 0)
        memcpy(data, slot->data, slot->len);
    slot->used = 0;
    ep->next_recv_msg++;
    return 1;
}
