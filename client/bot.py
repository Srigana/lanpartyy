import argparse
import math
import random
import time

import netproto as proto
from game_client import GameClient

TICK = 1.0 / 30.0
DIRECTIONS = [
    0,
    proto.BUTTON_UP,
    proto.BUTTON_DOWN,
    proto.BUTTON_LEFT,
    proto.BUTTON_RIGHT,
    proto.BUTTON_UP | proto.BUTTON_LEFT,
    proto.BUTTON_UP | proto.BUTTON_RIGHT,
    proto.BUTTON_DOWN | proto.BUTTON_LEFT,
    proto.BUTTON_DOWN | proto.BUTTON_RIGHT,
]


def buttons_towards(x, y, tx, ty):
    buttons = 0
    if tx > x + 4:
        buttons |= proto.BUTTON_RIGHT
    elif tx < x - 4:
        buttons |= proto.BUTTON_LEFT
    if ty > y + 4:
        buttons |= proto.BUTTON_DOWN
    elif ty < y - 4:
        buttons |= proto.BUTTON_UP
    return buttons


class Bot:
    def __init__(self, host, port, behavior="wander", seed=None, record=False, token=0):
        self.client = GameClient(host, port, token=token)
        self.behavior = behavior
        self.rng = random.Random(seed)
        self.record = record
        self.samples = []
        self.wander = 0
        self.next_turn = 0.0

    def start(self, now=None):
        self.client.connect(now)

    def step(self, now):
        c = self.client
        c.update(now)
        if c.connected:
            c.buttons = self.decide(now)
        c.send(now)
        if self.record and c.connected and c.players:
            view = {pid: (x, y) for pid, (_, x, y) in c.players.items()}
            self.samples.append((now, c.latest_tick, view))

    def decide(self, now):
        c = self.client
        if self.behavior == "idle":
            return 0

        me = c.position()
        if self.behavior == "chase" and me is not None and c.it_player() == c.player_id:
            target = None
            best = None
            for pid, (flags, x, y) in c.players.items():
                if pid == c.player_id or flags & proto.FLAG_DISCONNECTED:
                    continue
                d = math.hypot(x - me[0], y - me[1])
                if best is None or d < best:
                    best = d
                    target = (x, y)
            if target is not None:
                return buttons_towards(me[0], me[1], target[0], target[1])

        if now >= self.next_turn:
            self.wander = self.rng.choice(DIRECTIONS)
            self.next_turn = now + self.rng.uniform(0.4, 1.5)
        return self.wander

    def leave(self):
        self.client.leave()

    def close(self):
        self.client.close()


def run_bots(bots, duration, until=None, seed=0):
    rng = random.Random(seed)
    start = time.monotonic()
    next_tick = start
    while True:
        now = time.monotonic()
        if now - start >= duration:
            break
        if until is not None and until():
            break
        for bot in bots:
            bot.step(now)
        next_tick += TICK
        sleep = next_tick + rng.uniform(0, TICK * 0.9) - time.monotonic()
        if sleep > 0:
            time.sleep(sleep)
        elif sleep < -TICK:
            next_tick = time.monotonic()


def main():
    parser = argparse.ArgumentParser(description="run some bot players")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=40000)
    parser.add_argument("--count", type=int, default=4)
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--behavior", choices=["wander", "chase", "idle"], default="chase")
    args = parser.parse_args()

    bots = [Bot(args.host, args.port, behavior=args.behavior, seed=i) for i in range(args.count)]
    for bot in bots:
        bot.start()
    try:
        run_bots(bots, args.duration)
    except KeyboardInterrupt:
        pass
    for bot in bots:
        c = bot.client
        print(f"bot player={c.player_id} state={c.state} snapshots={c.snapshots_received} "
              f"reconnects={c.reconnects} score={c.scores.get(c.player_id, 0)}")
        bot.leave()
        bot.close()


if __name__ == "__main__":
    main()
