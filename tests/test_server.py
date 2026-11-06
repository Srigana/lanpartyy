import os
import socket
import struct
import time

import netproto as proto
from bot import Bot
from game_client import GameClient
from helpers import pump


def connect_all(clients, timeout=3.0):
    for c in clients:
        c.connect()
    return pump(clients, timeout, until=lambda: all(c.connected for c in clients))


def test_join_and_receive_snapshots(start_server):
    server = start_server()
    c = GameClient("127.0.0.1", server.port)
    assert connect_all([c])
    assert c.player_id == 0
    assert c.token != 0

    pump([c], 1.0)
    assert c.snapshots_received >= 25
    assert c.player_id in c.players
    assert c.it_player() == c.player_id
    c.close()


def test_players_see_each_other(start_server):
    server = start_server()
    clients = [GameClient("127.0.0.1", server.port) for _ in range(3)]
    assert connect_all(clients)
    pump(clients, 0.5)

    ids = {c.player_id for c in clients}
    assert len(ids) == 3
    for c in clients:
        assert set(c.players) == ids
        assert set(c.scores) == ids
    for c in clients:
        c.close()


def test_input_moves_player(start_server):
    server = start_server()
    c = GameClient("127.0.0.1", server.port)
    assert connect_all([c])
    pump([c], 0.3)
    x0, y0 = c.position()

    c.buttons = proto.BUTTON_RIGHT if x0 < 400 else proto.BUTTON_LEFT
    pump([c], 1.0)
    x1, y1 = c.position()
    assert abs(x1 - x0) > 100
    assert y1 == y0
    c.close()


def test_server_full_rejects_extra_player(start_server):
    server = start_server(max_players=2)
    clients = [GameClient("127.0.0.1", server.port) for _ in range(2)]
    assert connect_all(clients)

    extra = GameClient("127.0.0.1", server.port)
    extra.connect()
    assert pump(clients + [extra], 2.0, until=lambda: extra.state == "rejected")
    for c in clients + [extra]:
        c.close()


def test_leave_removes_player(start_server):
    server = start_server()
    a = GameClient("127.0.0.1", server.port)
    b = GameClient("127.0.0.1", server.port)
    assert connect_all([a, b])
    pump([a, b], 0.3)
    assert b.player_id in a.players

    b.leave()
    b.close()
    assert pump([a], 2.0, until=lambda: b.player_id not in a.players)
    a.close()


def test_garbage_does_not_break_server(start_server):
    server = start_server()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    addr = ("127.0.0.1", server.port)

    junk = [
        b"",
        b"\x00",
        os.urandom(5),
        os.urandom(13),
        os.urandom(1200),
        struct.pack("!IHHIB", proto.PROTOCOL_ID, 0, 0, 0, proto.MSG_JOIN),
        struct.pack("!IHHIB", proto.PROTOCOL_ID, 0, 0, 0, proto.MSG_JOIN) + b"\x00",
        struct.pack("!IHHIB", proto.PROTOCOL_ID, 0, 0, 0, proto.MSG_INPUT) + b"\x0f",
        struct.pack("!IHHIB", proto.PROTOCOL_ID, 0, 0, 0, proto.MSG_SNAPSHOT) + os.urandom(40),
        struct.pack("!IHHIB", proto.PROTOCOL_ID, 0, 0, 0, 200) + os.urandom(10),
        struct.pack("!IHHIBH", proto.PROTOCOL_ID, 0, 0, 0, proto.MSG_SCORE, 0) + os.urandom(80),
    ]
    for _ in range(20):
        for packet in junk:
            s.sendto(packet, addr)
    s.close()

    time.sleep(0.2)
    assert server.alive()
    c = GameClient("127.0.0.1", server.port)
    assert connect_all([c])
    assert c.player_id == 0
    c.close()


def test_tag_and_score_are_delivered(start_server):
    server = start_server()
    chaser = Bot("127.0.0.1", server.port, behavior="chase", seed=1)
    runner = Bot("127.0.0.1", server.port, behavior="idle", seed=2)
    chaser.start()
    pump([chaser], 2.0, until=lambda: chaser.client.connected)
    runner.start()

    def tagged():
        return any(e[0] == "tag" for e in runner.client.events)

    assert pump([chaser, runner], 15.0, until=tagged)
    tag = next(e for e in runner.client.events if e[0] == "tag")
    assert tag[1] == chaser.client.player_id
    assert tag[2] == runner.client.player_id
    pump([chaser, runner], 0.5)
    assert runner.client.scores[chaser.client.player_id] >= 1
    assert chaser.client.scores[chaser.client.player_id] >= 1
    chaser.close()
    runner.close()
