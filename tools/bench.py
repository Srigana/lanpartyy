import argparse
import json
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "client"))
sys.path.insert(0, str(ROOT / "tools"))

from bot import Bot, run_bots  # noqa: E402
from drift import load_tick_log, measure_drift, percentile  # noqa: E402
from game_client import GameClient  # noqa: E402
from proxy import LossyProxy  # noqa: E402


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Server:
    def __init__(self, binary, workdir, max_players=8, delta=True, tick_log=False):
        self.port = free_port()
        self.stats_path = Path(workdir) / f"stats_{self.port}.json"
        self.tick_log = Path(workdir) / f"ticks_{self.port}.csv" if tick_log else None
        cmd = [binary, "--port", str(self.port), "--max-players", str(max_players),
               "--stats", str(self.stats_path)]
        if not delta:
            cmd.append("--no-delta")
        if self.tick_log:
            cmd += ["--tick-log", str(self.tick_log)]
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL)
        time.sleep(0.2)

    def stop(self):
        self.proc.send_signal(signal.SIGINT)
        self.proc.wait(timeout=5)
        return json.loads(self.stats_path.read_text())


def start_bots(host, port, count, behavior, record=False, seed=0):
    bots = [Bot(host, port, behavior=behavior, seed=seed + i, record=record) for i in range(count)]
    for b in bots:
        b.start()
    run_bots(bots, 5.0, until=lambda: all(b.client.connected for b in bots))
    return bots


def bandwidth(binary, workdir, seconds):
    rows = []
    for label, moving in (("8 players, all moving", 8), ("8 players, 2 moving", 2)):
        result = {}
        for delta in (False, True):
            server = Server(binary, workdir, delta=delta)
            bots = start_bots("127.0.0.1", server.port, moving, "wander")
            bots += start_bots("127.0.0.1", server.port, 8 - moving, "idle", seed=100)
            run_bots(bots, seconds)
            for b in bots:
                b.close()
            result[delta] = server.stop()
        rows.append((label, result[False], result[True]))

    print("\n### Bandwidth (server to client, UDP payload incl. 13 byte header)\n")
    print("| scenario | full B/s/player | delta B/s/player | saved | avg full snapshot | avg delta snapshot |")
    print("|---|---|---|---|---|---|")
    for label, full, delta in rows:
        f = full["bytes_per_sec_per_player"]
        d = delta["bytes_per_sec_per_player"]
        print(f"| {label} | {f:.0f} | {d:.0f} | {100 * (1 - d / f):.0f}% | "
              f"{full['full_snapshot_avg_bytes']:.1f} B | {delta['delta_snapshot_avg_bytes']:.1f} B |")


def loss_levels(binary, workdir, seconds, delay):
    print(f"\n### Drift under loss (4 bots, {delay * 1000:.0f} ms one-way delay, {seconds:.0f} s each)\n")
    print("| loss | drift p50 | drift p95 | drift max | view age p50 | view age p95 | snapshots/s per client | reconnects |")
    print("|---|---|---|---|---|---|---|---|")
    for loss in (0.0, 0.01, 0.05, 0.10):
        server = Server(binary, workdir, tick_log=True)
        proxy = LossyProxy(server.port, loss=loss, delay=delay, jitter=0.005).start()
        bots = start_bots("127.0.0.1", proxy.port, 4, "chase", record=True)
        before = [b.client.snapshots_received for b in bots]
        run_bots(bots, seconds)
        rate = sum(b.client.snapshots_received - n for b, n in zip(bots, before)) / len(bots) / seconds
        reconnects = sum(b.client.reconnects for b in bots)
        samples = [s for b in bots for s in b.samples]
        for b in bots:
            b.close()
        proxy.stop()
        server.stop()

        drift = measure_drift(load_tick_log(server.tick_log), samples)
        errors = [d[0] for d in drift]
        ages = [d[2] * 1000 for d in drift if d[2] is not None]
        print(f"| {loss * 100:.0f}% | {percentile(errors, 50):.1f} px | {percentile(errors, 95):.1f} px | "
              f"{max(errors):.1f} px | {percentile(ages, 50):.0f} ms | {percentile(ages, 95):.0f} ms | "
              f"{rate:.1f} | {reconnects} |")


def reconnects(binary, workdir, trials):
    outage = []
    for trial in range(trials):
        server = Server(binary, workdir)
        proxy = LossyProxy(server.port).start()
        c = GameClient("127.0.0.1", proxy.port)
        c.connect()
        run_bots([c], 3.0, until=lambda: c.connected)
        run_bots([c], 0.5)
        proxy.block(6.0)
        back = proxy.blocked_until
        run_bots([c], 8.0, until=lambda: time.monotonic() > back and c.connected, seed=trial)
        outage.append(c.accept_times[-1] - back)
        c.close()
        proxy.stop()
        server.stop()

    restart = []
    for trial in range(trials):
        server = Server(binary, workdir)
        c = GameClient("127.0.0.1", server.port)
        c.connect()
        run_bots([c], 3.0, until=lambda: c.connected)
        token = c.token
        c.close()
        c2 = GameClient("127.0.0.1", server.port, token=token)
        c2.connect()
        run_bots([c2], 3.0, until=lambda: c2.connected, seed=trial)
        restart.append(c2.join_times[-1])
        c2.close()
        server.stop()

    print(f"\n### Reconnect time ({trials} trials each)\n")
    print("| case | avg | max |")
    print("|---|---|---|")
    print(f"| after 6 s outage (from link back up to ACCEPT) | {1000 * sum(outage) / len(outage):.0f} ms | "
          f"{1000 * max(outage):.0f} ms |")
    print(f"| new process with saved token (JOIN to ACCEPT) | {1000 * sum(restart) / len(restart):.1f} ms | "
          f"{1000 * max(restart):.1f} ms |")


def player_count(binary, workdir, seconds):
    print(f"\n### Tick cost by player count ({seconds:.0f} s each)\n")
    print("| players | avg tick | max tick | overruns | ticks/s |")
    print("|---|---|---|---|---|")
    for count in (2, 8, 16, 32):
        server = Server(binary, workdir, max_players=count)
        bots = start_bots("127.0.0.1", server.port, count, "chase")
        run_bots(bots, seconds)
        for b in bots:
            b.close()
        stats = server.stop()
        print(f"| {stats['peak_players']} | {stats['avg_tick_ms']:.3f} ms | {stats['max_tick_ms']:.3f} ms | "
              f"{stats['overruns']} | {stats['ticks'] / stats['run_time']:.1f} |")


def main():
    parser = argparse.ArgumentParser(description="measure the server")
    parser.add_argument("--server", default=str(ROOT / "build" / "lanserver"))
    parser.add_argument("--seconds", type=float, default=10.0)
    parser.add_argument("--delay", type=float, default=0.02)
    parser.add_argument("--trials", type=int, default=5)
    parser.add_argument("--only", choices=["bandwidth", "loss", "reconnect", "players"])
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as workdir:
        if args.only in (None, "bandwidth"):
            bandwidth(args.server, workdir, args.seconds)
        if args.only in (None, "loss"):
            loss_levels(args.server, workdir, args.seconds, args.delay)
        if args.only in (None, "reconnect"):
            reconnects(args.server, workdir, args.trials)
        if args.only in (None, "players"):
            player_count(args.server, workdir, args.seconds)


if __name__ == "__main__":
    main()
