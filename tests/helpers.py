import time


def pump(things, seconds, until=None):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        now = time.monotonic()
        for t in things:
            t.step(now)
        if until is not None and until():
            return True
        time.sleep(1 / 30)
    return until() if until is not None else True
