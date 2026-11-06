import json
import os
import signal
import socket
import subprocess
import sys
import threading
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "client"))
sys.path.insert(0, str(ROOT / "tools"))

from proxy import LossyProxy  # noqa: E402

SERVER_BIN = Path(os.environ.get("LANSERVER", ROOT / "build" / "lanserver"))
USE_NETEM = os.environ.get("USE_NETEM") == "1"


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class ServerProcess:
    def __init__(self, tmp_path, max_players=8, delta=True, tick_log=False):
        self.port = free_port()
        self.stats_path = tmp_path / f"stats_{self.port}.json"
        self.tick_log_path = tmp_path / f"ticks_{self.port}.csv" if tick_log else None
        self.lines = []
        self.stats = None

        cmd = [str(SERVER_BIN), "--port", str(self.port), "--max-players", str(max_players),
               "--stats", str(self.stats_path)]
        if not delta:
            cmd.append("--no-delta")
        if self.tick_log_path:
            cmd += ["--tick-log", str(self.tick_log_path)]

        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        first = self.proc.stdout.readline()
        if "listening" not in first:
            self.proc.kill()
            raise RuntimeError(f"server did not start: {first}")
        self.lines.append(first.strip())
        self.reader = threading.Thread(target=self._read_output, daemon=True)
        self.reader.start()

    def _read_output(self):
        for line in self.proc.stdout:
            self.lines.append(line.strip())

    def alive(self):
        return self.proc.poll() is None

    def stop(self):
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGINT)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.reader.join(timeout=2)
        if self.stats_path.exists():
            self.stats = json.loads(self.stats_path.read_text())
        return self.stats


@pytest.fixture
def start_server(tmp_path):
    if not SERVER_BIN.exists():
        pytest.skip(f"server binary not found at {SERVER_BIN}, build it first")
    servers = []

    def factory(**kwargs):
        server = ServerProcess(tmp_path, **kwargs)
        servers.append(server)
        return server

    yield factory
    for server in servers:
        server.stop()
        assert server.proc.returncode == 0, "\n".join(server.lines)


def netem(args):
    subprocess.run(["tc", "qdisc", "replace", "dev", "lo", "root", "netem"] + args, check=True)


def netem_clear():
    subprocess.run(["tc", "qdisc", "del", "dev", "lo", "root"], check=False,
                   stderr=subprocess.DEVNULL)


class Link:
    def __init__(self, host, port, proxy=None):
        self.host = host
        self.port = port
        self.proxy = proxy


@pytest.fixture
def lossy_link():
    proxies = []
    used_netem = []

    def factory(server, loss=0.0, delay=0.0, jitter=0.0, force_proxy=False):
        if USE_NETEM and not force_proxy:
            args = ["loss", f"{loss * 100:.1f}%"]
            if delay or jitter:
                args += ["delay", f"{delay * 1000:.0f}ms", f"{jitter * 1000:.0f}ms"]
            netem(args)
            used_netem.append(True)
            return Link("127.0.0.1", server.port)
        proxy = LossyProxy(server.port, loss=loss, delay=delay, jitter=jitter).start()
        proxies.append(proxy)
        return Link("127.0.0.1", proxy.port, proxy)

    yield factory
    for proxy in proxies:
        proxy.stop()
    if used_netem:
        netem_clear()
