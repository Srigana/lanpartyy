import os
import socket
import struct
import time

import netproto as proto

RECONNECT_AFTER = 2.0
SNAPSHOT_KEEP = 64


class GameClient:
    def __init__(self, host, port, token=0, reconnect_after=RECONNECT_AFTER):
        self.server = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", 0))
        self.sock.setblocking(False)
        self.reconnect_after = reconnect_after

        self.state = "idle"
        self.token = token
        self.player_id = None
        self.endpoint = None
        self.nonce = 0

        self.players = {}
        self.scores = {}
        self.latest_tick = 0
        self.snapshots = {}
        self.events = []
        self.buttons = 0

        self.last_recv = 0.0
        self.join_started = 0.0
        self.join_times = []
        self.accept_times = []
        self.reconnects = 0
        self.snapshots_received = 0
        self.snapshots_dropped = 0
        self.bytes_received = 0

    @property
    def connected(self):
        return self.state == "connected"

    def connect(self, now=None):
        now = time.monotonic() if now is None else now
        self.endpoint = proto.Endpoint()
        self.nonce = struct.unpack("!I", os.urandom(4))[0]
        self.snapshots = {}
        self.latest_tick = 0
        self.state = "joining"
        self.join_started = now
        self.last_recv = now
        self.endpoint.queue_reliable(proto.MSG_JOIN, struct.pack("!II", self.nonce, self.token))

    def leave(self):
        if self.endpoint is None or self.state not in ("connected", "joining"):
            return
        now = time.monotonic()
        self.endpoint.queue_reliable(proto.MSG_LEAVE, b"")
        for _ in range(3):
            for packet in self._reliable_packets(now, resend_after=0.0):
                self._send(packet)
        self.state = "left"

    def close(self):
        self.sock.close()

    def _send(self, packet):
        try:
            self.sock.sendto(packet, self.server)
        except (BlockingIOError, ConnectionRefusedError):
            pass

    def _reliable_packets(self, now, resend_after):
        packets = []
        for _ in range(4):
            packet = self.endpoint.write_reliable(now, resend_after)
            if packet is None:
                break
            packets.append(packet)
        return packets

    def update(self, now=None):
        now = time.monotonic() if now is None else now
        while True:
            try:
                data, addr = self.sock.recvfrom(2048)
            except (BlockingIOError, ConnectionRefusedError):
                break
            if addr != self.server:
                continue
            self._handle(data, now)

        if self.state in ("connected", "joining") and self.token and now - self.last_recv > self.reconnect_after:
            self.reconnects += 1
            self.connect(now)

    def send(self, now=None):
        now = time.monotonic() if now is None else now
        if self.endpoint is None or self.state not in ("connected", "joining"):
            return
        resend = max(0.1, self.endpoint.rtt * 1.5)
        for packet in self._reliable_packets(now, resend):
            self._send(packet)
        if self.state == "connected":
            self._send(self.endpoint.write_packet(proto.MSG_INPUT, bytes([self.buttons]), now))

    def step(self, now=None):
        now = time.monotonic() if now is None else now
        self.update(now)
        self.send(now)

    def _handle(self, data, now):
        if len(data) >= proto.HEADER_SIZE and data[proto.HEADER_SIZE - 1] == proto.MSG_REJECT:
            if self.state == "joining":
                self.state = "rejected"
            return
        if self.endpoint is None:
            return

        packet = self.endpoint.read_packet(data, now)
        if packet is None:
            return
        self.last_recv = now
        self.bytes_received += len(data)

        if packet.type == proto.MSG_SNAPSHOT:
            self._apply_snapshot(packet.payload, now)

        while True:
            msg = self.endpoint.pop_reliable()
            if msg is None:
                break
            self._handle_message(msg[0], msg[1], now)

    def _handle_message(self, msg_type, data, now):
        if msg_type == proto.MSG_ACCEPT and len(data) >= 5:
            self.player_id, self.token = struct.unpack_from("!BI", data)
            if self.state == "joining":
                self.join_times.append(now - self.join_started)
                self.accept_times.append(now)
            self.state = "connected"
        elif msg_type == proto.MSG_TAG and len(data) >= 2:
            self.events.append(("tag", data[0], data[1]))
        elif msg_type == proto.MSG_SCORE and len(data) >= 3:
            pid, score = struct.unpack_from("!BH", data)
            self.scores[pid] = score

    def _apply_snapshot(self, payload, now):
        parsed = proto.parse_snapshot(payload)
        if parsed is None:
            return
        tick, baseline, entries = parsed

        if baseline == 0:
            state = {}
        elif baseline in self.snapshots:
            state = dict(self.snapshots[baseline])
        else:
            self.snapshots_dropped += 1
            return

        for pid, flags, x, y in entries:
            if flags & proto.FLAG_REMOVED:
                state.pop(pid, None)
            else:
                state[pid] = (flags, x, y)

        self.snapshots[tick] = state
        self.snapshots_received += 1
        for old in [t for t in self.snapshots if tick - t > SNAPSHOT_KEEP]:
            del self.snapshots[old]

        if tick > self.latest_tick:
            self.latest_tick = tick
            self.players = state

    def it_player(self):
        for pid, (flags, _, _) in self.players.items():
            if flags & proto.FLAG_IT:
                return pid
        return None

    def position(self, pid=None):
        pid = self.player_id if pid is None else pid
        entry = self.players.get(pid)
        if entry is None:
            return None
        return entry[1], entry[2]
