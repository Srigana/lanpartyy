import bisect
import csv
import math


def load_tick_log(path):
    times = []
    ticks = []
    positions = {}
    with open(path) as f:
        for row in csv.DictReader(f):
            tick = int(row["tick"])
            if not ticks or ticks[-1] != tick:
                times.append(float(row["time"]))
                ticks.append(tick)
                positions[tick] = {}
            positions[tick][int(row["id"])] = (int(row["x"]), int(row["y"]))
    return times, ticks, positions


def measure_drift(tick_log, samples):
    """Compare what a client was showing with what the server had at the same moment.

    Returns (position error in px, ticks behind, age of the shown tick in seconds) for every
    player in every sample. Server and bots must run on the same machine so they use the
    same monotonic clock.
    """
    times, ticks, positions = tick_log
    tick_time = dict(zip(ticks, times))
    results = []
    for t, client_tick, view in samples:
        i = bisect.bisect_right(times, t) - 1
        if i < 0:
            continue
        server_tick = ticks[i]
        truth = positions[server_tick]
        age = t - tick_time[client_tick] if client_tick in tick_time else None
        for pid, (x, y) in view.items():
            if pid not in truth:
                continue
            tx, ty = truth[pid]
            results.append((math.hypot(x - tx, y - ty), server_tick - client_tick, age))
    return results


def percentile(values, p):
    if not values:
        return 0.0
    values = sorted(values)
    k = (len(values) - 1) * p / 100.0
    lo = math.floor(k)
    hi = math.ceil(k)
    if lo == hi:
        return values[lo]
    return values[lo] + (values[hi] - values[lo]) * (k - lo)
