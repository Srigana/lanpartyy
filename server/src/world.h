#pragma once

#include <array>
#include <cstdint>

constexpr int kMaxPlayers = 32;
constexpr float kArenaWidth = 800.0f;
constexpr float kArenaHeight = 600.0f;
constexpr float kPlayerRadius = 10.0f;
constexpr float kPlayerSpeed = 160.0f;
constexpr float kTagDistance = 24.0f;
constexpr double kTagCooldown = 1.0;

enum InputButtons : uint8_t {
    kButtonUp = 1,
    kButtonDown = 2,
    kButtonLeft = 4,
    kButtonRight = 8,
};

struct Player {
    bool active = false;
    bool connected = false;
    float x = 0.0f;
    float y = 0.0f;
    uint8_t buttons = 0;
    uint16_t score = 0;
};

struct TagEvent {
    int tagger;
    int tagged;
};

class World {
public:
    void add_player(int id, uint32_t seed);
    void remove_player(int id, double now);
    void set_connected(int id, bool connected, double now);
    void set_buttons(int id, uint8_t buttons);

    int step(float dt, double now, TagEvent *events, int max_events);

    const Player &player(int id) const { return players_[id]; }
    int it_player() const { return it_; }
    int active_count() const;

private:
    void pick_new_it(int except, double now);

    std::array<Player, kMaxPlayers> players_{};
    int it_ = -1;
    double it_since_ = 0.0;
};
