import heapq
import itertools
import random
import select
import socket
import threading
import time


class LossyProxy:
    """UDP relay that drops and delays packets, for when tc netem isn't available."""

    def __init__(self, server_port, loss=0.0, delay=0.0, jitter=0.0, seed=1):
        self.server = ("127.0.0.1", server_port)
        self.loss = loss
        self.delay = delay
        self.jitter = jitter
        self.rng = random.Random(seed)

        self.listen = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.listen.bind(("127.0.0.1", 0))
        self.port = self.listen.getsockname()[1]

        self.upstream = {}
        self.client_for = {}
        self.queue = []
        self.counter = itertools.count()
        self.blocked_until = 0.0
        self.lock = threading.Lock()
        self.running = False
        self.thread = None

    def start(self):
        self.running = True
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()
        return self

    def stop(self):
        self.running = False
        if self.thread:
            self.thread.join(timeout=2)
        for s in self.upstream.values():
            s.close()
        self.listen.close()

    def block(self, seconds):
        with self.lock:
            self.blocked_until = time.monotonic() + seconds

    def _schedule(self, sock, data, addr):
        now = time.monotonic()
        with self.lock:
            if now < self.blocked_until:
                return
        if self.rng.random() < self.loss:
            return
        when = now + self.delay + self.rng.uniform(0, self.jitter)
        heapq.heappush(self.queue, (when, next(self.counter), sock, data, addr))

    def _loop(self):
        while self.running:
            sockets = [self.listen] + list(self.upstream.values())
            timeout = 0.005
            if self.queue:
                timeout = max(0.0, min(timeout, self.queue[0][0] - time.monotonic()))
            readable, _, _ = select.select(sockets, [], [], timeout)

            for s in readable:
                try:
                    data, addr = s.recvfrom(4096)
                except OSError:
                    continue
                if s is self.listen:
                    up = self.upstream.get(addr)
                    if up is None:
                        up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                        up.bind(("127.0.0.1", 0))
                        self.upstream[addr] = up
                        self.client_for[up] = addr
                    self._schedule(up, data, self.server)
                else:
                    self._schedule(self.listen, data, self.client_for[s])

            now = time.monotonic()
            while self.queue and self.queue[0][0] <= now:
                _, _, sock, data, addr = heapq.heappop(self.queue)
                try:
                    sock.sendto(data, addr)
                except OSError:
                    pass
