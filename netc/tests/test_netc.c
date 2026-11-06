#include "netc.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            printf("%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                    \
        }                                                                  \
    } while (0)

static void test_header_roundtrip(void)
{
    uint8_t buf[NETC_HEADER_SIZE];
    netc_header in = {NETC_PROTOCOL_ID, 0x1234, 0xabcd, 0x80000001u, NETC_MSG_INPUT};
    netc_header out;

    CHECK(netc_write_header(buf, sizeof(buf), &in) == NETC_HEADER_SIZE);
    CHECK(buf[0] == 0x4C && buf[1] == 0x41 && buf[2] == 0x4E && buf[3] == 0x31);
    CHECK(buf[4] == 0x12 && buf[5] == 0x34);
    CHECK(netc_read_header(buf, sizeof(buf), &out) == NETC_HEADER_SIZE);
    CHECK(out.seq == 0x1234 && out.ack == 0xabcd);
    CHECK(out.ack_bits == 0x80000001u && out.type == NETC_MSG_INPUT);

    CHECK(netc_read_header(buf, sizeof(buf) - 1, &out) < 0);
    buf[0] = 0;
    CHECK(netc_read_header(buf, sizeof(buf), &out) < 0);
    CHECK(netc_write_header(buf, 5, &in) < 0);
}

static void test_reader_bounds(void)
{
    uint8_t buf[3] = {1, 2, 3};
    netc_reader r;
    netc_reader_init(&r, buf, sizeof(buf));
    CHECK(netc_read_u16(&r) == 0x0102);
    CHECK(netc_read_u16(&r) == 0);
    CHECK(r.err == 1);
}

static void test_seq_greater(void)
{
    CHECK(netc_seq_greater(1, 0));
    CHECK(!netc_seq_greater(0, 1));
    CHECK(netc_seq_greater(0, 65535));
    CHECK(netc_seq_greater(10, 65000));
    CHECK(!netc_seq_greater(65000, 10));
    CHECK(!netc_seq_greater(5, 5));
}

static int deliver(netc_endpoint *to, const uint8_t *buf, size_t len, netc_packet *pkt)
{
    return netc_read_packet(to, buf, len, 0.0, pkt);
}

static void test_acks_and_loss(void)
{
    static netc_endpoint a, b;
    uint8_t buf[NETC_MAX_PACKET];
    uint8_t payload[4] = {9, 9, 9, 9};
    netc_packet pkt;
    int i;
    size_t n;

    netc_endpoint_init(&a);
    netc_endpoint_init(&b);

    for (i = 0; i < 10; i++) {
        n = netc_write_packet(&a, NETC_MSG_INPUT, payload, sizeof(payload), buf, sizeof(buf), 0.0);
        CHECK(n == NETC_HEADER_SIZE + sizeof(payload));
        if (i == 3 || i == 7)
            continue;
        CHECK(deliver(&b, buf, n, &pkt) == 1);
    }

    n = netc_write_packet(&b, NETC_MSG_SNAPSHOT, NULL, 0, buf, sizeof(buf), 0.0);
    CHECK(deliver(&a, buf, n, &pkt) == 1);
    CHECK(pkt.header.ack == 9);
    CHECK((pkt.header.ack_bits & (1u << 1)) == 0);
    CHECK((pkt.header.ack_bits & (1u << 5)) == 0);
    CHECK((pkt.header.ack_bits & (1u << 0)) != 0);
    CHECK(pkt.acked_count == 8);
    CHECK(a.packets_acked == 8);
}

static void test_duplicate_and_stale(void)
{
    static netc_endpoint a, b;
    uint8_t first[64], second[64];
    size_t n1, n2;
    netc_packet pkt;

    netc_endpoint_init(&a);
    netc_endpoint_init(&b);

    n1 = netc_write_packet(&a, NETC_MSG_INPUT, (const uint8_t *)"a", 1, first, sizeof(first), 0.0);
    n2 = netc_write_packet(&a, NETC_MSG_INPUT, (const uint8_t *)"b", 1, second, sizeof(second), 0.0);

    CHECK(deliver(&b, second, n2, &pkt) == 1);
    CHECK(pkt.stale == 0);
    CHECK(deliver(&b, first, n1, &pkt) == 1);
    CHECK(pkt.stale == 1);
    CHECK(deliver(&b, first, n1, &pkt) == 0);
    CHECK(deliver(&b, second, n2, &pkt) == 0);
}

static void test_no_false_ack_before_receive(void)
{
    static netc_endpoint a, b;
    uint8_t buf[64];
    size_t n;
    netc_packet pkt;

    netc_endpoint_init(&a);
    netc_endpoint_init(&b);

    netc_write_packet(&a, NETC_MSG_INPUT, NULL, 0, buf, sizeof(buf), 0.0);
    n = netc_write_packet(&b, NETC_MSG_SNAPSHOT, NULL, 0, buf, sizeof(buf), 0.0);
    CHECK(deliver(&a, buf, n, &pkt) == 1);
    CHECK(pkt.acked_count == 0);
}

static void test_reliable_with_loss(void)
{
    static netc_endpoint a, b;
    uint8_t buf[NETC_MAX_PACKET];
    uint8_t data[NETC_MSG_MAX];
    uint8_t type;
    size_t len, n;
    netc_packet pkt;
    int round, i, got = 0;
    double now = 0.0;

    netc_endpoint_init(&a);
    netc_endpoint_init(&b);

    for (i = 0; i < 5; i++) {
        uint8_t v = (uint8_t)i;
        CHECK(netc_queue_reliable(&a, NETC_MSG_SCORE, &v, 1) == i);
    }

    for (round = 0; round < 20 && got < 5; round++) {
        int sent = 0;
        while ((n = netc_write_reliable(&a, now, 0.1, buf, sizeof(buf))) > 0) {
            if (((round + sent) % 3) != 0)
                deliver(&b, buf, n, &pkt);
            sent++;
        }
        while (netc_pop_reliable(&b, &type, data, &len)) {
            CHECK(type == NETC_MSG_SCORE);
            CHECK(len == 1 && data[0] == got);
            got++;
        }
        n = netc_write_packet(&b, NETC_MSG_SNAPSHOT, NULL, 0, buf, sizeof(buf), now);
        deliver(&a, buf, n, &pkt);
        now += 0.2;
    }

    CHECK(got == 5);
    CHECK(netc_reliable_pending(&a) == 0);
}

static void test_reliable_out_of_order(void)
{
    static netc_endpoint a, b;
    uint8_t p0[64], p1[64], p0_again[64];
    uint8_t data[NETC_MSG_MAX];
    uint8_t v, type;
    size_t n0, n1, n2, len;
    netc_packet pkt;

    netc_endpoint_init(&a);
    netc_endpoint_init(&b);

    v = 10;
    netc_queue_reliable(&a, NETC_MSG_TAG, &v, 1);
    v = 11;
    netc_queue_reliable(&a, NETC_MSG_TAG, &v, 1);
    n0 = netc_write_reliable(&a, 0.0, 0.1, p0, sizeof(p0));
    n1 = netc_write_reliable(&a, 0.0, 0.1, p1, sizeof(p1));
    CHECK(netc_write_reliable(&a, 0.05, 0.1, p0_again, sizeof(p0_again)) == 0);
    n2 = netc_write_reliable(&a, 0.2, 0.1, p0_again, sizeof(p0_again));
    CHECK(n2 > 0);

    deliver(&b, p1, n1, &pkt);
    CHECK(netc_pop_reliable(&b, &type, data, &len) == 0);
    deliver(&b, p0, n0, &pkt);
    CHECK(netc_pop_reliable(&b, &type, data, &len) == 1 && data[0] == 10);
    CHECK(netc_pop_reliable(&b, &type, data, &len) == 1 && data[0] == 11);
    deliver(&b, p0_again, n2, &pkt);
    CHECK(netc_pop_reliable(&b, &type, data, &len) == 0);
}

static void test_reliable_queue_full(void)
{
    static netc_endpoint a;
    uint8_t big[NETC_MSG_MAX + 1] = {0};
    int i;

    netc_endpoint_init(&a);
    CHECK(netc_queue_reliable(&a, NETC_MSG_TAG, big, sizeof(big)) == -1);
    for (i = 0; i < NETC_MSG_SLOTS; i++)
        CHECK(netc_queue_reliable(&a, NETC_MSG_TAG, big, 1) >= 0);
    CHECK(netc_queue_reliable(&a, NETC_MSG_TAG, big, 1) == -1);
}

static void test_seq_wraparound(void)
{
    static netc_endpoint a, b;
    uint8_t buf[64];
    netc_packet pkt;
    size_t n;
    int i;

    netc_endpoint_init(&a);
    netc_endpoint_init(&b);
    a.local_seq = 65530;

    for (i = 0; i < 12; i++) {
        n = netc_write_packet(&a, NETC_MSG_INPUT, NULL, 0, buf, sizeof(buf), 0.0);
        CHECK(deliver(&b, buf, n, &pkt) == 1);
        CHECK(pkt.stale == 0);
    }
    CHECK(b.remote_seq == 5);

    n = netc_write_packet(&b, NETC_MSG_SNAPSHOT, NULL, 0, buf, sizeof(buf), 0.0);
    CHECK(deliver(&a, buf, n, &pkt) == 1);
    CHECK(pkt.acked_count == 12);
}

int main(void)
{
    test_header_roundtrip();
    test_reader_bounds();
    test_seq_greater();
    test_acks_and_loss();
    test_duplicate_and_stale();
    test_no_false_ack_before_receive();
    test_reliable_with_loss();
    test_reliable_out_of_order();
    test_reliable_queue_full();
    test_seq_wraparound();

    if (failures) {
        printf("%d checks failed\n", failures);
        return 1;
    }
    printf("all netc tests passed\n");
    return 0;
}
