import argparse
import time

import pygame

import netproto as proto
from game_client import GameClient

WIDTH = 800
HEIGHT = 600
SEND_INTERVAL = 1.0 / 30.0

BACKGROUND = (24, 26, 32)
COLORS = [
    (80, 160, 255), (120, 220, 120), (240, 200, 80), (200, 120, 240),
    (90, 220, 220), (240, 140, 90), (180, 180, 180), (250, 120, 170),
]
IT_COLOR = (230, 60, 60)
OFFLINE_COLOR = (90, 90, 90)


def read_buttons():
    keys = pygame.key.get_pressed()
    buttons = 0
    if keys[pygame.K_UP] or keys[pygame.K_w]:
        buttons |= proto.BUTTON_UP
    if keys[pygame.K_DOWN] or keys[pygame.K_s]:
        buttons |= proto.BUTTON_DOWN
    if keys[pygame.K_LEFT] or keys[pygame.K_a]:
        buttons |= proto.BUTTON_LEFT
    if keys[pygame.K_RIGHT] or keys[pygame.K_d]:
        buttons |= proto.BUTTON_RIGHT
    return buttons


def draw(screen, font, client):
    screen.fill(BACKGROUND)

    for pid, (flags, x, y) in client.players.items():
        if flags & proto.FLAG_DISCONNECTED:
            color = OFFLINE_COLOR
        elif flags & proto.FLAG_IT:
            color = IT_COLOR
        else:
            color = COLORS[pid % len(COLORS)]
        pygame.draw.circle(screen, color, (x, y), 10)
        if pid == client.player_id:
            pygame.draw.circle(screen, (255, 255, 255), (x, y), 13, 2)

    lines = [f"you: {client.player_id}   state: {client.state}"]
    if client.endpoint is not None:
        lines.append(f"rtt: {client.endpoint.rtt * 1000:.0f} ms")
    for pid in sorted(client.players):
        lines.append(f"player {pid}: {client.scores.get(pid, 0)}")
    for i, line in enumerate(lines):
        screen.blit(font.render(line, True, (220, 220, 220)), (10, 10 + i * 18))

    pygame.display.flip()


def main():
    parser = argparse.ArgumentParser(description="tag game client")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=40000)
    parser.add_argument("--token", type=int, default=0, help="token from an earlier session, to rejoin")
    parser.add_argument("--frames", type=int, default=0, help="quit after this many frames (for testing)")
    args = parser.parse_args()

    pygame.init()
    screen = pygame.display.set_mode((WIDTH, HEIGHT))
    pygame.display.set_caption("LAN tag")
    font = pygame.font.SysFont(None, 20)
    clock = pygame.time.Clock()

    client = GameClient(args.host, args.port, token=args.token)
    client.connect()
    next_send = time.monotonic()
    printed_token = False
    frames = 0

    running = True
    while running:
        for event in pygame.event.get():
            if event.type == pygame.QUIT:
                running = False
            elif event.type == pygame.KEYDOWN and event.key == pygame.K_ESCAPE:
                running = False

        now = time.monotonic()
        client.update(now)
        if client.connected and not printed_token:
            print(f"joined as player {client.player_id}, token {client.token}")
            printed_token = True
        if client.state == "rejected":
            print("server is full")
            running = False

        client.buttons = read_buttons()
        if now >= next_send:
            client.send(now)
            next_send += SEND_INTERVAL
            if next_send < now:
                next_send = now + SEND_INTERVAL

        draw(screen, font, client)
        clock.tick(60)
        frames += 1
        if args.frames and frames >= args.frames:
            running = False

    client.leave()
    client.close()
    pygame.quit()


if __name__ == "__main__":
    main()
