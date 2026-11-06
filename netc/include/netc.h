#ifndef NETC_H
#define NETC_H

#include <stddef.h>
#include <stdint.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NETC_PROTOCOL_ID 0x4C414E31u
#define NETC_HEADER_SIZE 13
#define NETC_MAX_PACKET 1200
#define NETC_MAX_PAYLOAD (NETC_MAX_PACKET - NETC_HEADER_SIZE)

#define NETC_SEQ_BUFFER 256
#define NETC_MSG_SLOTS 64
#define NETC_MSG_MAX 64
#define NETC_ACKS_PER_PACKET 33

enum netc_msg_type {
    NETC_MSG_JOIN = 1,
    NETC_MSG_ACCEPT = 2,
    NETC_MSG_REJECT = 3,
    NETC_MSG_LEAVE = 4,
    NETC_MSG_INPUT = 5,
    NETC_MSG_SNAPSHOT = 6,
    NETC_MSG_TAG = 7,
    NETC_MSG_SCORE = 8
};

typedef struct {
    uint32_t protocol_id;
    uint16_t seq;
    uint16_t ack;
    uint32_t ack_bits;
    uint8_t type;
} netc_header;

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t pos;
    int err;
} netc_writer;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos;
    int err;
} netc_reader;

void netc_writer_init(netc_writer *w, uint8_t *buf, size_t cap);
void netc_write_u8(netc_writer *w, uint8_t v);
void netc_write_u16(netc_writer *w, uint16_t v);
void netc_write_u32(netc_writer *w, uint32_t v);
void netc_write_bytes(netc_writer *w, const uint8_t *src, size_t n);

void netc_reader_init(netc_reader *r, const uint8_t *buf, size_t len);
uint8_t netc_read_u8(netc_reader *r);
uint16_t netc_read_u16(netc_reader *r);
uint32_t netc_read_u32(netc_reader *r);
void netc_read_bytes(netc_reader *r, uint8_t *dst, size_t n);

int netc_write_header(uint8_t *buf, size_t cap, const netc_header *h);
int netc_read_header(const uint8_t *buf, size_t len, netc_header *h);

int netc_seq_greater(uint16_t a, uint16_t b);
int netc_is_reliable_type(uint8_t type);

int netc_socket_open(uint16_t port);
void netc_socket_close(int fd);
int netc_send(int fd, const struct sockaddr_in *to, const uint8_t *buf, size_t len);
int netc_recv(int fd, struct sockaddr_in *from, uint8_t *buf, size_t cap);

typedef struct {
    uint16_t seq;
    uint8_t valid;
    uint8_t acked;
    uint8_t has_msg;
    uint16_t msg_id;
    double time;
} netc_sent_entry;

typedef struct {
    uint8_t used;
    uint8_t type;
    uint16_t id;
    uint16_t len;
    double last_sent;
    uint8_t data[NETC_MSG_MAX];
} netc_msg_slot;

typedef struct {
    uint16_t local_seq;
    uint16_t remote_seq;
    int have_remote;

    netc_sent_entry sent[NETC_SEQ_BUFFER];
    uint16_t recv_seq[NETC_SEQ_BUFFER];
    uint8_t recv_valid[NETC_SEQ_BUFFER];

    uint16_t next_send_msg;
    uint16_t next_recv_msg;
    netc_msg_slot send_q[NETC_MSG_SLOTS];
    netc_msg_slot recv_q[NETC_MSG_SLOTS];

    double rtt;
    uint64_t packets_sent;
    uint64_t packets_recv;
    uint64_t packets_acked;
    uint64_t packets_lost;
    uint64_t bytes_sent;
    uint64_t bytes_recv;
} netc_endpoint;

typedef struct {
    netc_header header;
    const uint8_t *payload;
    size_t payload_len;
    int stale;
    uint16_t acked[NETC_ACKS_PER_PACKET];
    int acked_count;
} netc_packet;

void netc_endpoint_init(netc_endpoint *ep);

size_t netc_write_packet(netc_endpoint *ep, uint8_t type, const uint8_t *payload,
                         size_t len, uint8_t *out, size_t cap, double now);

int netc_queue_reliable(netc_endpoint *ep, uint8_t type, const uint8_t *data, size_t len);
size_t netc_write_reliable(netc_endpoint *ep, double now, double resend_after,
                           uint8_t *out, size_t cap);
int netc_reliable_pending(const netc_endpoint *ep);

int netc_read_packet(netc_endpoint *ep, const uint8_t *buf, size_t len, double now,
                     netc_packet *pkt);
int netc_pop_reliable(netc_endpoint *ep, uint8_t *type, uint8_t *data, size_t *len);

#ifdef __cplusplus
}
#endif

#endif
