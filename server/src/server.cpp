#include "server.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstring>
#include <ctime>

#include <arpa/inet.h>
#include <poll.h>

double monotonic_seconds()
{
    timespec ts;
#ifdef __APPLE__
    clock_gettime(CLOCK_UPTIME_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

static const char *addr_str(const sockaddr_in &addr, char *buf, size_t cap)
{
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
    snprintf(buf, cap, "%s:%u", ip, ntohs(addr.sin_port));
    return buf;
}

Server::Server(const ServerConfig &config) : config_(config), rng_(std::random_device{}())
{
    config_.max_players = std::clamp(config_.max_players, 1, kMaxPlayers);
}

Server::~Server()
{
    netc_socket_close(fd_);
    if (tick_log_)
        fclose(tick_log_);
}

bool Server::start()
{
    fd_ = netc_socket_open(config_.port);
    if (fd_ < 0) {
        fprintf(stderr, "could not open udp port %u: %s\n", config_.port, strerror(errno));
        return false;
    }
    if (!config_.tick_log_path.empty()) {
        tick_log_ = fopen(config_.tick_log_path.c_str(), "w");
        if (!tick_log_) {
            fprintf(stderr, "could not open %s\n", config_.tick_log_path.c_str());
            return false;
        }
        fprintf(tick_log_, "time,tick,id,x,y\n");
    }
    printf("listening on udp port %u, max %d players, %s snapshots\n", config_.port,
           config_.max_players, config_.delta ? "delta" : "full");
    return true;
}

void Server::run(volatile std::sig_atomic_t &running)
{
    const double step = 1.0 / kTickRate;
    const double start = monotonic_seconds();
    double next = start;

    while (running) {
        double now = monotonic_seconds();
        stats_.run_time = now - start;
        if (config_.duration > 0.0 && stats_.run_time >= config_.duration)
            break;

        if (now >= next) {
            receive_packets(now);
            double t0 = monotonic_seconds();
            tick(t0);
            double spent = monotonic_seconds() - t0;

            stats_.ticks++;
            stats_.tick_time_total += spent;
            stats_.tick_time_max = std::max(stats_.tick_time_max, spent);
            if (spent > step)
                stats_.overruns++;

            next += step;
            if (monotonic_seconds() - next > step * 5) {
                stats_.overruns++;
                next = monotonic_seconds() + step;
            }
            continue;
        }

        int wait_ms = static_cast<int>(std::ceil((next - now) * 1000.0));
        pollfd pfd{fd_, POLLIN, 0};
        if (poll(&pfd, 1, wait_ms) > 0)
            receive_packets(monotonic_seconds());
    }
}

void Server::receive_packets(double now)
{
    sockaddr_in from{};
    for (int i = 0; i < 1024; i++) {
        int n = netc_recv(fd_, &from, packet_, sizeof(packet_));
        if (n <= 0)
            break;
        stats_.total_bytes_recv += static_cast<uint64_t>(n);
        handle_datagram(from, packet_, static_cast<size_t>(n), now);
    }
}

void Server::handle_datagram(const sockaddr_in &from, const uint8_t *buf, size_t len, double now)
{
    netc_header h;
    if (netc_read_header(buf, len, &h) < 0)
        return;

    if (h.type == NETC_MSG_JOIN) {
        handle_join_packet(from, buf, len, now);
        return;
    }

    int id = find_by_addr(from);
    if (id >= 0)
        process_packet(id, buf, len, now);
}

void Server::handle_join_packet(const sockaddr_in &from, const uint8_t *buf, size_t len, double now)
{
    netc_reader r;
    netc_reader_init(&r, buf + NETC_HEADER_SIZE, len - NETC_HEADER_SIZE);
    uint16_t msg_id = netc_read_u16(&r);
    uint32_t nonce = netc_read_u32(&r);
    uint32_t token = netc_read_u32(&r);
    if (r.err)
        return;

    int id = find_by_addr(from);
    if (id >= 0 && sessions_[id].nonce == nonce) {
        process_packet(id, buf, len, now);
        return;
    }

    if (msg_id != 0)
        return;

    if (id >= 0 && sessions_[id].token != token) {
        Session &old = sessions_[id];
        old.has_addr = false;
        if (old.state == SessionState::Connected) {
            old.state = SessionState::Disconnected;
            old.disconnected_at = now;
            world_.set_connected(id, false, now);
        }
    }

    int target = token != 0 ? find_by_token(token) : -1;
    if (target < 0) {
        if (used_slots() >= config_.max_players) {
            uint8_t reason = 1;
            netc_endpoint_init(&scratch_);
            size_t n = netc_write_packet(&scratch_, NETC_MSG_REJECT, &reason, 1, packet_,
                                         sizeof(packet_), now);
            send_raw(from, packet_, n);
            return;
        }
        target = find_free_slot();
        if (target < 0)
            return;
        sessions_[target] = Session{};
    }

    bind_session(target, from, nonce);
    process_packet(target, buf, len, now);

    if (sessions_[target].state == SessionState::Free)
        sessions_[target] = Session{};
}

void Server::bind_session(int id, const sockaddr_in &from, uint32_t nonce)
{
    Session &s = sessions_[id];
    for (Session &other : sessions_) {
        if (&other != &s && other.has_addr && other.addr.sin_addr.s_addr == from.sin_addr.s_addr &&
            other.addr.sin_port == from.sin_port)
            other.has_addr = false;
    }
    s.has_addr = true;
    s.addr = from;
    s.nonce = nonce;
    s.buttons = 0;
    netc_endpoint_init(&s.ep);
    s.history.reset();
}

void Server::process_packet(int id, const uint8_t *buf, size_t len, double now)
{
    Session &s = sessions_[id];
    netc_packet pkt;
    if (!netc_read_packet(&s.ep, buf, len, now, &pkt))
        return;

    s.last_heard = now;
    s.bytes_recv += len;
    for (int i = 0; i < pkt.acked_count; i++)
        s.history.on_ack(pkt.acked[i]);

    if (pkt.header.type == NETC_MSG_INPUT && !pkt.stale && pkt.payload_len >= 1 &&
        s.state == SessionState::Connected)
        s.buttons = pkt.payload[0];

    uint8_t type;
    uint8_t data[NETC_MSG_MAX];
    size_t mlen;
    while (netc_pop_reliable(&s.ep, &type, data, &mlen)) {
        handle_message(id, type, data, mlen, now);
        if (type == NETC_MSG_LEAVE)
            break;
    }
}

void Server::handle_message(int id, uint8_t type, const uint8_t *data, size_t len, double now)
{
    (void)data;
    (void)len;
    switch (type) {
    case NETC_MSG_JOIN:
        accept_player(id, now);
        break;
    case NETC_MSG_LEAVE:
        if (sessions_[id].state != SessionState::Free) {
            printf("player %d left\n", id);
            drop_player(id, now);
        }
        break;
    default:
        break;
    }
}

void Server::accept_player(int id, double now)
{
    Session &s = sessions_[id];
    char addr[64];
    bool rejoin = s.state != SessionState::Free;

    if (rejoin) {
        world_.set_connected(id, true, now);
        stats_.rejoins++;
        printf("player %d rejoined from %s\n", id, addr_str(s.addr, addr, sizeof(addr)));
    } else {
        uint32_t token;
        do {
            token = static_cast<uint32_t>(rng_());
        } while (token == 0 || find_by_token(token) >= 0);
        s.token = token;
        world_.add_player(id, static_cast<uint32_t>(rng_()));
        stats_.joins++;
        printf("player %d joined from %s\n", id, addr_str(s.addr, addr, sizeof(addr)));
    }

    s.state = SessionState::Connected;
    s.last_heard = now;

    netc_writer w;
    uint8_t msg[8];
    netc_writer_init(&w, msg, sizeof(msg));
    netc_write_u8(&w, static_cast<uint8_t>(id));
    netc_write_u32(&w, s.token);
    netc_queue_reliable(&s.ep, NETC_MSG_ACCEPT, msg, w.pos);
    send_scores_to(id);

    if (!rejoin) {
        uint8_t score[3] = {static_cast<uint8_t>(id), 0, 0};
        for (int i = 0; i < kMaxPlayers; i++) {
            if (i != id && sessions_[i].state == SessionState::Connected)
                netc_queue_reliable(&sessions_[i].ep, NETC_MSG_SCORE, score, sizeof(score));
        }
    }
}

void Server::drop_player(int id, double now)
{
    world_.remove_player(id, now);
    sessions_[id] = Session{};
}

void Server::send_scores_to(int id)
{
    for (int i = 0; i < kMaxPlayers; i++) {
        const Player &p = world_.player(i);
        if (!p.active)
            continue;
        uint8_t msg[3];
        netc_writer w;
        netc_writer_init(&w, msg, sizeof(msg));
        netc_write_u8(&w, static_cast<uint8_t>(i));
        netc_write_u16(&w, p.score);
        netc_queue_reliable(&sessions_[id].ep, NETC_MSG_SCORE, msg, w.pos);
    }
}

void Server::broadcast_reliable(uint8_t type, const uint8_t *data, size_t len)
{
    for (Session &s : sessions_) {
        if (s.state == SessionState::Connected)
            netc_queue_reliable(&s.ep, type, data, len);
    }
}

void Server::tick(double now)
{
    const float dt = static_cast<float>(1.0 / kTickRate);
    tick_++;

    check_timeouts(now);

    int players = 0;
    for (int i = 0; i < kMaxPlayers; i++) {
        Session &s = sessions_[i];
        if (s.state != SessionState::Connected)
            continue;
        world_.set_buttons(i, s.buttons);
        s.connected_time += dt;
        stats_.player_seconds += dt;
        players++;
    }
    stats_.peak_players = std::max(stats_.peak_players, players);

    TagEvent events[4];
    int n = world_.step(dt, now, events, 4);
    for (int i = 0; i < n; i++) {
        uint8_t tag[2] = {static_cast<uint8_t>(events[i].tagger),
                          static_cast<uint8_t>(events[i].tagged)};
        broadcast_reliable(NETC_MSG_TAG, tag, sizeof(tag));

        uint8_t score[3];
        netc_writer w;
        netc_writer_init(&w, score, sizeof(score));
        netc_write_u8(&w, tag[0]);
        netc_write_u16(&w, world_.player(events[i].tagger).score);
        broadcast_reliable(NETC_MSG_SCORE, score, w.pos);
    }

    capture_world(world_, current_);

    if (tick_log_) {
        for (int i = 0; i < kMaxPlayers; i++) {
            if (current_[i].present)
                fprintf(tick_log_, "%.6f,%u,%d,%u,%u\n", now, tick_, i, current_[i].x,
                        current_[i].y);
        }
    }

    send_reliable(now);
    send_snapshots(now);
}

void Server::check_timeouts(double now)
{
    for (int i = 0; i < kMaxPlayers; i++) {
        Session &s = sessions_[i];
        if (s.state == SessionState::Connected && now - s.last_heard > kTimeoutSeconds) {
            s.state = SessionState::Disconnected;
            s.disconnected_at = now;
            s.has_addr = false;
            world_.set_connected(i, false, now);
            stats_.timeouts++;
            printf("player %d timed out\n", i);
        } else if (s.state == SessionState::Disconnected &&
                   now - s.disconnected_at > kRejoinSeconds) {
            printf("player %d removed\n", i);
            drop_player(i, now);
        }
    }
}

void Server::send_reliable(double now)
{
    for (Session &s : sessions_) {
        if (s.state != SessionState::Connected || !s.has_addr)
            continue;
        double resend = std::max(0.1, s.ep.rtt * 1.5);
        for (int k = 0; k < kMaxReliablePerTick; k++) {
            uint64_t lost_before = s.ep.packets_lost;
            size_t n = netc_write_reliable(&s.ep, now, resend, packet_, sizeof(packet_));
            if (n == 0)
                break;
            send_raw(s.addr, packet_, n);
            s.bytes_sent += n;
            stats_.packets_lost += s.ep.packets_lost - lost_before;
        }
    }
}

void Server::send_snapshots(double now)
{
    for (Session &s : sessions_) {
        if (s.state != SessionState::Connected || !s.has_addr)
            continue;

        const SnapshotRecord *base = config_.delta ? s.history.baseline(tick_) : nullptr;
        size_t plen = write_snapshot(payload_, sizeof(payload_), tick_, base, current_);
        if (plen == 0)
            continue;

        uint16_t seq = s.ep.local_seq;
        uint64_t lost_before = s.ep.packets_lost;
        size_t n = netc_write_packet(&s.ep, NETC_MSG_SNAPSHOT, payload_, plen, packet_,
                                     sizeof(packet_), now);
        if (n == 0)
            continue;

        s.history.store(tick_, seq, current_);
        send_raw(s.addr, packet_, n);
        s.bytes_sent += n;
        stats_.packets_lost += s.ep.packets_lost - lost_before;
        if (base) {
            stats_.delta_snapshots++;
            stats_.delta_bytes += n;
        } else {
            stats_.full_snapshots++;
            stats_.full_bytes += n;
        }
    }
}

void Server::send_raw(const sockaddr_in &to, const uint8_t *buf, size_t len)
{
    if (len == 0)
        return;
    if (netc_send(fd_, &to, buf, len) > 0) {
        stats_.total_bytes_sent += len;
        stats_.packets_sent++;
    }
}

int Server::find_by_addr(const sockaddr_in &addr) const
{
    for (int i = 0; i < kMaxPlayers; i++) {
        const Session &s = sessions_[i];
        if (s.has_addr && s.addr.sin_addr.s_addr == addr.sin_addr.s_addr &&
            s.addr.sin_port == addr.sin_port)
            return i;
    }
    return -1;
}

int Server::find_by_token(uint32_t token) const
{
    for (int i = 0; i < kMaxPlayers; i++) {
        if (sessions_[i].state != SessionState::Free && sessions_[i].token == token)
            return i;
    }
    return -1;
}

int Server::find_free_slot() const
{
    for (int i = 0; i < kMaxPlayers; i++) {
        if (sessions_[i].state == SessionState::Free && !sessions_[i].has_addr)
            return i;
    }
    return -1;
}

int Server::used_slots() const
{
    int n = 0;
    for (const Session &s : sessions_) {
        if (s.state != SessionState::Free)
            n++;
    }
    return n;
}

void Server::write_stats() const
{
    double avg_tick = stats_.ticks ? stats_.tick_time_total / static_cast<double>(stats_.ticks) : 0.0;
    double per_player = stats_.player_seconds > 0.0
                            ? static_cast<double>(stats_.total_bytes_sent) / stats_.player_seconds
                            : 0.0;
    double full_avg = stats_.full_snapshots
                          ? static_cast<double>(stats_.full_bytes) / static_cast<double>(stats_.full_snapshots)
                          : 0.0;
    double delta_avg = stats_.delta_snapshots
                           ? static_cast<double>(stats_.delta_bytes) / static_cast<double>(stats_.delta_snapshots)
                           : 0.0;

    printf("ticks %llu, avg tick %.3f ms, max tick %.3f ms, overruns %llu\n",
           static_cast<unsigned long long>(stats_.ticks), avg_tick * 1000.0,
           stats_.tick_time_max * 1000.0, static_cast<unsigned long long>(stats_.overruns));
    printf("sent %.1f bytes/s per player (full snapshot avg %.1f B, delta avg %.1f B)\n",
           per_player, full_avg, delta_avg);

    if (config_.stats_path.empty())
        return;
    FILE *f = fopen(config_.stats_path.c_str(), "w");
    if (!f) {
        fprintf(stderr, "could not write %s\n", config_.stats_path.c_str());
        return;
    }
    fprintf(f, "{\n");
    fprintf(f, "  \"run_time\": %.3f,\n", stats_.run_time);
    fprintf(f, "  \"ticks\": %llu,\n", static_cast<unsigned long long>(stats_.ticks));
    fprintf(f, "  \"overruns\": %llu,\n", static_cast<unsigned long long>(stats_.overruns));
    fprintf(f, "  \"avg_tick_ms\": %.4f,\n", avg_tick * 1000.0);
    fprintf(f, "  \"max_tick_ms\": %.4f,\n", stats_.tick_time_max * 1000.0);
    fprintf(f, "  \"peak_players\": %d,\n", stats_.peak_players);
    fprintf(f, "  \"bytes_per_sec_per_player\": %.2f,\n", per_player);
    fprintf(f, "  \"full_snapshots\": %llu,\n", static_cast<unsigned long long>(stats_.full_snapshots));
    fprintf(f, "  \"delta_snapshots\": %llu,\n", static_cast<unsigned long long>(stats_.delta_snapshots));
    fprintf(f, "  \"full_snapshot_avg_bytes\": %.2f,\n", full_avg);
    fprintf(f, "  \"delta_snapshot_avg_bytes\": %.2f,\n", delta_avg);
    fprintf(f, "  \"bytes_sent\": %llu,\n", static_cast<unsigned long long>(stats_.total_bytes_sent));
    fprintf(f, "  \"bytes_recv\": %llu,\n", static_cast<unsigned long long>(stats_.total_bytes_recv));
    fprintf(f, "  \"packets_sent\": %llu,\n", static_cast<unsigned long long>(stats_.packets_sent));
    fprintf(f, "  \"packets_lost\": %llu,\n", static_cast<unsigned long long>(stats_.packets_lost));
    fprintf(f, "  \"joins\": %llu,\n", static_cast<unsigned long long>(stats_.joins));
    fprintf(f, "  \"rejoins\": %llu,\n", static_cast<unsigned long long>(stats_.rejoins));
    fprintf(f, "  \"timeouts\": %llu\n", static_cast<unsigned long long>(stats_.timeouts));
    fprintf(f, "}\n");
    fclose(f);
}
