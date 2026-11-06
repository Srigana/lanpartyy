import struct

PROTOCOL_ID = 0x4C414E31
HEADER = struct.Struct("!IHHIB")
HEADER_SIZE = HEADER.size

MSG_JOIN = 1
MSG_ACCEPT = 2
MSG_REJECT = 3
MSG_LEAVE = 4
MSG_INPUT = 5
MSG_SNAPSHOT = 6
MSG_TAG = 7
MSG_SCORE = 8

RELIABLE_TYPES = {MSG_JOIN, MSG_ACCEPT, MSG_LEAVE, MSG_TAG, MSG_SCORE}

BUTTON_UP = 1
BUTTON_DOWN = 2
BUTTON_LEFT = 4
BUTTON_RIGHT = 8

FLAG_IT = 1
FLAG_DISCONNECTED = 2
FLAG_REMOVED = 0x80

SEQ_BUFFER = 256
MSG_SLOTS = 64
MSG_MAX = 64
LOST_AFTER = 64


def seq_greater(a, b):
    return (a > b and a - b <= 32768) or (a < b and b - a > 32768)


class Packet:
    def __init__(self, seq, msg_type, payload, stale, acked):
        self.seq = seq
        self.type = msg_type
        self.payload = payload
        self.stale = stale
        self.acked = acked


class Endpoint:
    def __init__(self):
        self.local_seq = 0
        self.remote_seq = 0
        self.have_remote = False
        self.sent = [None] * SEQ_BUFFER
        self.received = [None] * SEQ_BUFFER
        self.next_send_msg = 0
        self.next_recv_msg = 0
        self.send_queue = {}
        self.recv_queue = {}
        self.rtt = 0.0
        self.packets_sent = 0
        self.packets_recv = 0
        self.packets_acked = 0
        self.packets_lost = 0

    def _ack_fields(self):
        if not self.have_remote:
            return 0, 0
        bits = 0
        for i in range(32):
            seq = (self.remote_seq - 1 - i) & 0xFFFF
            if self.received[seq % SEQ_BUFFER] == seq:
                bits |= 1 << i
        return self.remote_seq, bits

    def write_packet(self, msg_type, payload, now, msg_id=None):
        ack, bits = self._ack_fields()
        seq = self.local_seq
        old = (seq - LOST_AFTER) & 0xFFFF
        entry = self.sent[old % SEQ_BUFFER]
        if entry is not None and entry["seq"] == old and not entry["acked"]:
            self.packets_lost += 1
            self.sent[old % SEQ_BUFFER] = None

        self.sent[seq % SEQ_BUFFER] = {"seq": seq, "time": now, "acked": False, "msg": msg_id}
        self.local_seq = (seq + 1) & 0xFFFF
        self.packets_sent += 1
        return HEADER.pack(PROTOCOL_ID, seq, ack, bits, msg_type) + payload

    def queue_reliable(self, msg_type, data):
        msg_id = self.next_send_msg
        if len(data) > MSG_MAX or msg_id % MSG_SLOTS in self.send_queue:
            return None
        self.send_queue[msg_id % MSG_SLOTS] = {"id": msg_id, "type": msg_type, "data": data, "last_sent": -1e9}
        self.next_send_msg = (msg_id + 1) & 0xFFFF
        return msg_id

    def write_reliable(self, now, resend_after):
        for i in range(MSG_SLOTS, 0, -1):
            msg_id = (self.next_send_msg - i) & 0xFFFF
            slot = self.send_queue.get(msg_id % MSG_SLOTS)
            if slot is None or slot["id"] != msg_id:
                continue
            if now - slot["last_sent"] < resend_after:
                continue
            slot["last_sent"] = now
            payload = struct.pack("!H", msg_id) + slot["data"]
            return self.write_packet(slot["type"], payload, now, msg_id)
        return None

    def reliable_pending(self):
        return len(self.send_queue)

    def _on_acked(self, seq, now, acked):
        entry = self.sent[seq % SEQ_BUFFER]
        if entry is None or entry["seq"] != seq or entry["acked"]:
            return
        entry["acked"] = True
        self.packets_acked += 1
        sample = now - entry["time"]
        self.rtt = sample if self.rtt == 0.0 else self.rtt + (sample - self.rtt) * 0.1
        msg_id = entry["msg"]
        if msg_id is not None:
            slot = self.send_queue.get(msg_id % MSG_SLOTS)
            if slot is not None and slot["id"] == msg_id:
                del self.send_queue[msg_id % MSG_SLOTS]
        acked.append(seq)

    def read_packet(self, data, now):
        if len(data) < HEADER_SIZE:
            return None
        protocol_id, seq, ack, bits, msg_type = HEADER.unpack_from(data)
        if protocol_id != PROTOCOL_ID:
            return None
        payload = data[HEADER_SIZE:]
        reliable = msg_type in RELIABLE_TYPES
        if reliable and (len(payload) < 2 or len(payload) - 2 > MSG_MAX):
            return None

        if self.have_remote:
            if self.received[seq % SEQ_BUFFER] == seq:
                return None
            if not seq_greater(seq, self.remote_seq) and ((self.remote_seq - seq) & 0xFFFF) >= SEQ_BUFFER:
                return None

        stale = False
        if not self.have_remote:
            self.have_remote = True
            self.remote_seq = seq
        elif seq_greater(seq, self.remote_seq):
            gap = (seq - self.remote_seq) & 0xFFFF
            if gap >= SEQ_BUFFER:
                self.received = [None] * SEQ_BUFFER
            else:
                s = (self.remote_seq + 1) & 0xFFFF
                while s != seq:
                    self.received[s % SEQ_BUFFER] = None
                    s = (s + 1) & 0xFFFF
            self.remote_seq = seq
        else:
            stale = True
        self.received[seq % SEQ_BUFFER] = seq
        self.packets_recv += 1

        acked = []
        if ack != 0 or bits != 0:
            self._on_acked(ack, now, acked)
            for i in range(32):
                if bits & (1 << i):
                    self._on_acked((ack - 1 - i) & 0xFFFF, now, acked)

        if reliable:
            msg_id = struct.unpack_from("!H", payload)[0]
            payload = payload[2:]
            diff = (msg_id - self.next_recv_msg) & 0xFFFF
            if diff < MSG_SLOTS and msg_id % MSG_SLOTS not in self.recv_queue:
                self.recv_queue[msg_id % MSG_SLOTS] = (msg_id, msg_type, payload)

        return Packet(seq, msg_type, payload, stale, acked)

    def pop_reliable(self):
        slot = self.recv_queue.get(self.next_recv_msg % MSG_SLOTS)
        if slot is None or slot[0] != self.next_recv_msg:
            return None
        del self.recv_queue[self.next_recv_msg % MSG_SLOTS]
        self.next_recv_msg = (self.next_recv_msg + 1) & 0xFFFF
        return slot[1], slot[2]


def parse_snapshot(payload):
    if len(payload) < 9:
        return None
    tick, baseline, count = struct.unpack_from("!IIB", payload)
    if len(payload) < 9 + count * 6:
        return None
    entries = []
    for i in range(count):
        pid, flags, x, y = struct.unpack_from("!BBHH", payload, 9 + i * 6)
        entries.append((pid, flags, x, y))
    return tick, baseline, entries
