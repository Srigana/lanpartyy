import socket
import time

import pytest

import netproto as proto
from bot import Bot
from game_client import GameClient
from helpers import pump


def wait_connected(clients, timeout=3.0):
    for c in clients:
        if c.state == "idle":
            c.connect()
    return pump(clients, timeout, until=lambda: all(c.connected for c in clients))


def test_rejoin_after_timeout_keeps_state(start_server, lossy_link):
    server = start_server()
    link = lossy_link(server, force_proxy=True)
    player = GameClient(link.host, link.port)
    assert wait_connected([player])
    observer = GameClient("127.0.0.1", server.port)
    assert wait_connected([player, observer])

    player.buttons = proto.BUTTON_DOWN
    pump([player, observer], 0.5)
    player.buttons = 0
    pump([player, observer], 0.5)

    pid = player.player_id
    token = player.token
    before = observer.position(pid)
    assert before == player.position()

    link.proxy.block(7.0)

    def shown_disconnected():
        entry = observer.players.get(pid)
        return entry is not None and entry[0] & proto.FLAG_DISCONNECTED

    assert pump([player, observer], 6.5, until=shown_disconnected)
    assert observer.position(pid) == before

    back_at = link.proxy.blocked_until
    pump([player, observer], 2.0, until=lambda: time.monotonic() > back_at)
    assert player.state == "joining"
    assert pump([player, observer], 3.0, until=lambda: player.connected)

    assert player.player_id == pid
    assert player.token == token
    assert player.reconnects >= 1
    assert player.accept_times[-1] - back_at < 1.0

    pump([player, observer], 0.5)
    assert player.position() == before
    assert not observer.players[pid][0] & proto.FLAG_DISCONNECTED
    assert len(observer.players) == 2

    player.close()
    observer.close()
    stats = server.stop()
    assert stats["joins"] == 2
    assert stats["rejoins"] == 1
    assert stats["timeouts"] == 1


def test_rejoin_from_new_process_with_token(start_server):
    server = start_server()
    first = GameClient("127.0.0.1", server.port)
    observer = GameClient("127.0.0.1", server.port)
    assert wait_connected([first, observer])
    pump([first, observer], 0.3)
    pid, token = first.player_id, first.token
    first.close()

    second = GameClient("127.0.0.1", server.port, token=token)
    assert wait_connected([second, observer])
    assert second.player_id == pid
    pump([second, observer], 0.5)
    assert len(observer.players) == 2

    second.close()
    observer.close()
    stats = server.stop()
    assert stats["joins"] == 2
    assert stats["rejoins"] == 1


def test_restart_on_same_port_with_token(start_server):
    server = start_server()
    first = GameClient("127.0.0.1", server.port)
    assert wait_connected([first])
    pump([first], 0.3)
    pid, token = first.player_id, first.token
    port = first.sock.getsockname()[1]
    first.close()

    second = GameClient("127.0.0.1", server.port, token=token)
    second.sock.close()
    second.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    second.sock.bind(("0.0.0.0", port))
    second.sock.setblocking(False)

    assert wait_connected([second])
    assert second.player_id == pid
    pump([second], 0.5)
    assert len(second.players) == 1
    second.close()


def test_duplicate_join_packets_make_one_player(start_server):
    server = start_server()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    addr = ("127.0.0.1", server.port)

    ep = proto.Endpoint()
    ep.queue_reliable(proto.MSG_JOIN, b"\x00\x00\x00\x07\x00\x00\x00\x00")
    join = ep.write_reliable(0.0, 0.1)
    for _ in range(5):
        s.sendto(join, addr)
    for t in (0.2, 0.4, 0.6):
        s.sendto(ep.write_reliable(t, 0.1), addr)

    observer = GameClient("127.0.0.1", server.port)
    assert wait_connected([observer])
    pump([observer], 0.5)
    assert len(observer.players) == 2
    s.close()
    observer.close()
    assert server.stop()["joins"] == 2


def test_lossy_joins_make_no_duplicates(start_server, lossy_link):
    server = start_server()
    link = lossy_link(server, loss=0.3, force_proxy=True)
    bots = [Bot(link.host, link.port, seed=i) for i in range(6)]
    for b in bots:
        b.start()
    assert pump(bots, 10.0, until=lambda: all(b.client.connected for b in bots))
    pump(bots, 1.0)

    ids = {b.client.player_id for b in bots}
    assert len(ids) == 6
    for b in bots:
        assert set(b.client.players) == ids
        b.close()
    assert server.stop()["joins"] == 6


@pytest.mark.slow
def test_expired_token_gets_new_player(start_server):
    server = start_server()
    first = GameClient("127.0.0.1", server.port)
    observer = GameClient("127.0.0.1", server.port)
    assert wait_connected([first, observer])
    old_pid, old_token = first.player_id, first.token
    first.close()

    assert pump([observer], 40.0, until=lambda: old_pid not in observer.players)

    late = GameClient("127.0.0.1", server.port, token=old_token)
    assert wait_connected([late, observer])
    assert late.token != old_token
    pump([late, observer], 0.5)
    assert len(observer.players) == 2
    late.close()
    observer.close()
    stats = server.stop()
    assert stats["joins"] == 3
    assert stats["rejoins"] == 0
