#include "snapshot.h"

#include <cmath>

void SnapshotHistory::reset()
{
    records_.fill(SnapshotRecord{});
    last_acked_ = 0;
}

void SnapshotHistory::store(uint32_t tick, uint16_t seq, const WorldState &players)
{
    SnapshotRecord &r = records_[tick % kSnapshotHistory];
    r.valid = true;
    r.acked = false;
    r.tick = tick;
    r.seq = seq;
    r.players = players;
}

void SnapshotHistory::on_ack(uint16_t seq)
{
    for (SnapshotRecord &r : records_) {
        if (r.valid && !r.acked && r.seq == seq) {
            r.acked = true;
            if (r.tick > last_acked_)
                last_acked_ = r.tick;
            return;
        }
    }
}

const SnapshotRecord *SnapshotHistory::baseline(uint32_t current_tick) const
{
    if (last_acked_ == 0 || current_tick - last_acked_ >= kSnapshotHistory)
        return nullptr;
    const SnapshotRecord &r = records_[last_acked_ % kSnapshotHistory];
    if (!r.valid || !r.acked || r.tick != last_acked_)
        return nullptr;
    return &r;
}

void capture_world(const World &world, WorldState &out)
{
    for (int i = 0; i < kMaxPlayers; i++) {
        const Player &p = world.player(i);
        PlayerState &s = out[i];
        if (!p.active) {
            s = PlayerState{};
            continue;
        }
        s.present = 1;
        s.flags = 0;
        if (world.it_player() == i)
            s.flags |= kFlagIt;
        if (!p.connected)
            s.flags |= kFlagDisconnected;
        s.x = static_cast<uint16_t>(std::lround(p.x));
        s.y = static_cast<uint16_t>(std::lround(p.y));
    }
}

static void write_entry(netc_writer &w, int id, const PlayerState &s, uint8_t flags)
{
    netc_write_u8(&w, static_cast<uint8_t>(id));
    netc_write_u8(&w, flags);
    netc_write_u16(&w, s.x);
    netc_write_u16(&w, s.y);
}

size_t write_snapshot(uint8_t *buf, size_t cap, uint32_t tick, const SnapshotRecord *base,
                      const WorldState &current)
{
    netc_writer w;
    netc_writer_init(&w, buf, cap);
    netc_write_u32(&w, tick);
    netc_write_u32(&w, base ? base->tick : 0);
    size_t count_pos = w.pos;
    netc_write_u8(&w, 0);

    uint8_t count = 0;
    for (int i = 0; i < kMaxPlayers; i++) {
        const PlayerState &now = current[i];
        if (!base) {
            if (now.present) {
                write_entry(w, i, now, now.flags);
                count++;
            }
            continue;
        }

        const PlayerState &old = base->players[i];
        if (now.present && now != old) {
            write_entry(w, i, now, now.flags);
            count++;
        } else if (!now.present && old.present) {
            write_entry(w, i, PlayerState{}, kFlagRemoved);
            count++;
        }
    }

    if (w.err)
        return 0;
    buf[count_pos] = count;
    return w.pos;
}
