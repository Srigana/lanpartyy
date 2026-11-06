import struct

import netproto as proto


def test_header_is_13_bytes_big_endian():
    ep = proto.Endpoint()
    packet = ep.write_packet(proto.MSG_INPUT, b"\x05", 0.0)
    assert len(packet) == 14
    assert packet[:4] == b"LAN1"
    assert packet[4:6] == b"\x00\x00"
    assert packet[12] == proto.MSG_INPUT


def test_seq_greater_wraps():
    assert proto.seq_greater(1, 0)
    assert proto.seq_greater(0, 65535)
    assert not proto.seq_greater(65535, 0)
    assert not proto.seq_greater(7, 7)


def test_ack_bitfield_marks_lost_packets():
    a = proto.Endpoint()
    b = proto.Endpoint()
    for i in range(40):
        packet = a.write_packet(proto.MSG_INPUT, b"", 0.0)
        if i not in (35, 38):
            assert b.read_packet(packet, 0.0) is not None

    reply = b.write_packet(proto.MSG_SNAPSHOT, b"", 0.0)
    _, _, ack, bits, _ = proto.HEADER.unpack_from(reply)
    assert ack == 39
    assert not bits & (1 << 0)
    assert not bits & (1 << 3)
    assert bits & (1 << 1)

    got = a.read_packet(reply, 0.1)
    assert 38 not in got.acked and 35 not in got.acked
    assert len(got.acked) == 31


def test_duplicate_packet_dropped():
    a = proto.Endpoint()
    b = proto.Endpoint()
    packet = a.write_packet(proto.MSG_INPUT, b"\x01", 0.0)
    assert b.read_packet(packet, 0.0) is not None
    assert b.read_packet(packet, 0.0) is None


def test_reliable_messages_arrive_once_and_in_order():
    a = proto.Endpoint()
    b = proto.Endpoint()
    for i in range(3):
        a.queue_reliable(proto.MSG_TAG, bytes([i, i]))

    first = [a.write_reliable(0.0, 0.1) for _ in range(3)]
    assert a.write_reliable(0.0, 0.1) is None
    b.read_packet(first[2], 0.0)
    b.read_packet(first[1], 0.0)
    assert b.pop_reliable() is None

    resent = a.write_reliable(0.2, 0.1)
    b.read_packet(resent, 0.2)
    b.read_packet(first[0], 0.2)
    got = []
    while True:
        msg = b.pop_reliable()
        if msg is None:
            break
        got.append(msg)
    assert got == [(proto.MSG_TAG, bytes([i, i])) for i in range(3)]

    a.read_packet(b.write_packet(proto.MSG_SNAPSHOT, b"", 0.3), 0.3)
    assert a.reliable_pending() == 0


def test_parse_snapshot():
    payload = struct.pack("!IIB", 100, 97, 2)
    payload += struct.pack("!BBHH", 0, proto.FLAG_IT, 10, 20)
    payload += struct.pack("!BBHH", 3, proto.FLAG_REMOVED, 0, 0)
    tick, base, entries = proto.parse_snapshot(payload)
    assert (tick, base) == (100, 97)
    assert entries == [(0, proto.FLAG_IT, 10, 20), (3, proto.FLAG_REMOVED, 0, 0)]
    assert proto.parse_snapshot(payload[:-1]) is None
