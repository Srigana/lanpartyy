import pytest

from bot import Bot
from drift import load_tick_log, measure_drift, percentile
from helpers import pump


def play(link, count, seconds, record=True):
    bots = [Bot(link.host, link.port, behavior="chase", seed=i, record=record) for i in range(count)]
    for b in bots:
        b.start()
    assert pump(bots, 5.0, until=lambda: all(b.client.connected for b in bots))
    pump(bots, seconds)
    return bots


@pytest.mark.parametrize("loss", [0.01, 0.05, 0.10])
def test_playable_under_loss(start_server, lossy_link, loss):
    server = start_server(tick_log=True)
    link = lossy_link(server, loss=loss, delay=0.02, jitter=0.005)
    bots = play(link, 4, 8.0)

    for b in bots:
        c = b.client
        assert c.reconnects == 0
        assert c.connected
        assert c.snapshots_dropped == 0
        assert len(c.players) == 4

    samples = [s for b in bots for s in b.samples]
    for b in bots:
        b.close()
    server.stop()

    drift = measure_drift(load_tick_log(server.tick_log_path), samples)
    errors = [d[0] for d in drift]
    behind = [d[1] for d in drift]
    assert len(errors) > 1000
    assert percentile(errors, 50) < 15
    assert percentile(errors, 95) < 30
    assert percentile(behind, 95) <= 4


def test_delta_uses_less_bandwidth(start_server):
    results = {}
    for delta in (False, True):
        server = start_server(delta=delta)
        bots = [Bot("127.0.0.1", server.port, behavior="idle", seed=i) for i in range(6)]
        for b in bots:
            b.start()
        pump(bots, 3.0)
        for b in bots:
            b.close()
        results[delta] = server.stop()

    full = results[False]
    delta = results[True]
    assert full["delta_snapshots"] == 0
    assert delta["delta_snapshots"] > 0
    assert delta["bytes_per_sec_per_player"] < full["bytes_per_sec_per_player"] * 0.6
