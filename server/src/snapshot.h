#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "netc.h"
#include "world.h"

constexpr int kSnapshotHistory = 32;

enum SnapshotFlags : uint8_t {
    kFlagIt = 1,
    kFlagDisconnected = 2,
    kFlagRemoved = 0x80,
};

struct PlayerState {
    uint8_t present = 0;
    uint8_t flags = 0;
    uint16_t x = 0;
    uint16_t y = 0;

    bool operator==(const PlayerState &o) const
    {
        return present == o.present && flags == o.flags && x == o.x && y == o.y;
    }
    bool operator!=(const PlayerState &o) const { return !(*this == o); }
};

using WorldState = std::array<PlayerState, kMaxPlayers>;

struct SnapshotRecord {
    bool valid = false;
    bool acked = false;
    uint32_t tick = 0;
    uint16_t seq = 0;
    WorldState players{};
};

class SnapshotHistory {
public:
    void reset();
    void store(uint32_t tick, uint16_t seq, const WorldState &players);
    void on_ack(uint16_t seq);
    const SnapshotRecord *baseline(uint32_t current_tick) const;

private:
    std::array<SnapshotRecord, kSnapshotHistory> records_{};
    uint32_t last_acked_ = 0;
};

void capture_world(const World &world, WorldState &out);

size_t write_snapshot(uint8_t *buf, size_t cap, uint32_t tick, const SnapshotRecord *base,
                      const WorldState &current);
