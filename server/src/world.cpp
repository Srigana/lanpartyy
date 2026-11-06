#include "world.h"

#include <algorithm>

void World::add_player(int id, uint32_t seed)
{
    Player &p = players_[id];
    p = Player{};
    p.active = true;
    p.connected = true;
    p.x = 50.0f + static_cast<float>(seed % 700);
    p.y = 50.0f + static_cast<float>((seed / 700) % 500);

    if (it_ < 0)
        it_ = id;
}

void World::remove_player(int id, double now)
{
    players_[id] = Player{};
    if (it_ == id)
        pick_new_it(id, now);
}

void World::set_connected(int id, bool connected, double now)
{
    Player &p = players_[id];
    if (!p.active)
        return;
    p.connected = connected;
    p.buttons = 0;
    if (!connected && it_ == id)
        pick_new_it(id, now);
    if (connected && it_ < 0) {
        it_ = id;
        it_since_ = now;
    }
}

void World::set_buttons(int id, uint8_t buttons)
{
    players_[id].buttons = buttons;
}

int World::active_count() const
{
    int n = 0;
    for (const Player &p : players_) {
        if (p.active)
            n++;
    }
    return n;
}

void World::pick_new_it(int except, double now)
{
    it_ = -1;
    for (int i = 0; i < kMaxPlayers; i++) {
        if (i != except && players_[i].active && players_[i].connected) {
            it_ = i;
            it_since_ = now;
            return;
        }
    }
}

int World::step(float dt, double now, TagEvent *events, int max_events)
{
    for (Player &p : players_) {
        if (!p.active || !p.connected)
            continue;

        float dx = 0.0f;
        float dy = 0.0f;
        if (p.buttons & kButtonUp)
            dy -= 1.0f;
        if (p.buttons & kButtonDown)
            dy += 1.0f;
        if (p.buttons & kButtonLeft)
            dx -= 1.0f;
        if (p.buttons & kButtonRight)
            dx += 1.0f;
        if (dx != 0.0f && dy != 0.0f) {
            dx *= 0.7071f;
            dy *= 0.7071f;
        }

        p.x = std::clamp(p.x + dx * kPlayerSpeed * dt, kPlayerRadius, kArenaWidth - kPlayerRadius);
        p.y = std::clamp(p.y + dy * kPlayerSpeed * dt, kPlayerRadius, kArenaHeight - kPlayerRadius);
    }

    int count = 0;
    if (it_ < 0 || now - it_since_ < kTagCooldown || max_events <= 0)
        return count;

    const Player &it = players_[it_];
    for (int i = 0; i < kMaxPlayers; i++) {
        const Player &other = players_[i];
        if (i == it_ || !other.active || !other.connected)
            continue;
        float dx = other.x - it.x;
        float dy = other.y - it.y;
        if (dx * dx + dy * dy <= kTagDistance * kTagDistance) {
            players_[it_].score++;
            events[count++] = TagEvent{it_, i};
            it_ = i;
            it_since_ = now;
            break;
        }
    }
    return count;
}
