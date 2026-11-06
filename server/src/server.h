#pragma once

#include <array>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

#include <netinet/in.h>

#include "netc.h"
#include "snapshot.h"
#include "world.h"

constexpr double kTickRate = 30.0;
constexpr double kTimeoutSeconds = 5.0;
constexpr double kRejoinSeconds = 30.0;
constexpr int kMaxReliablePerTick = 4;

struct ServerConfig {
    uint16_t port = 40000;
    int max_players = 8;
    bool delta = true;
    double duration = 0.0;
    std::string stats_path;
    std::string tick_log_path;
};

enum class SessionState { Free, Connected, Disconnected };

struct Session {
    SessionState state = SessionState::Free;
    bool has_addr = false;
    sockaddr_in addr{};
    uint32_t token = 0;
    uint32_t nonce = 0;
    double last_heard = 0.0;
    double disconnected_at = 0.0;
    uint8_t buttons = 0;
    netc_endpoint ep{};
    SnapshotHistory history;

    double connected_time = 0.0;
    uint64_t bytes_sent = 0;
    uint64_t bytes_recv = 0;
};

struct ServerStats {
    double run_time = 0.0;
    uint64_t ticks = 0;
    uint64_t overruns = 0;
    double tick_time_total = 0.0;
    double tick_time_max = 0.0;
    int peak_players = 0;
    uint64_t full_snapshots = 0;
    uint64_t delta_snapshots = 0;
    uint64_t full_bytes = 0;
    uint64_t delta_bytes = 0;
    uint64_t total_bytes_sent = 0;
    uint64_t total_bytes_recv = 0;
    double player_seconds = 0.0;
    uint64_t joins = 0;
    uint64_t rejoins = 0;
    uint64_t timeouts = 0;
    uint64_t packets_lost = 0;
    uint64_t packets_sent = 0;
};

class Server {
public:
    explicit Server(const ServerConfig &config);
    ~Server();

    bool start();
    void run(volatile std::sig_atomic_t &running);
    void write_stats() const;

private:
    void receive_packets(double now);
    void handle_datagram(const sockaddr_in &from, const uint8_t *buf, size_t len, double now);
    void handle_join_packet(const sockaddr_in &from, const uint8_t *buf, size_t len, double now);
    void process_packet(int id, const uint8_t *buf, size_t len, double now);
    void handle_message(int id, uint8_t type, const uint8_t *data, size_t len, double now);

    void tick(double now);
    void check_timeouts(double now);
    void send_snapshots(double now);
    void send_reliable(double now);

    int find_by_addr(const sockaddr_in &addr) const;
    int find_by_token(uint32_t token) const;
    int find_free_slot() const;
    int used_slots() const;

    void bind_session(int id, const sockaddr_in &from, uint32_t nonce);
    void accept_player(int id, double now);
    void drop_player(int id, double now);
    void broadcast_reliable(uint8_t type, const uint8_t *data, size_t len);
    void send_scores_to(int id);
    void send_raw(const sockaddr_in &to, const uint8_t *buf, size_t len);

    ServerConfig config_;
    int fd_ = -1;
    uint32_t tick_ = 0;
    World world_;
    std::array<Session, kMaxPlayers> sessions_{};
    WorldState current_{};
    netc_endpoint scratch_{};
    std::mt19937 rng_;
    ServerStats stats_;
    FILE *tick_log_ = nullptr;
    uint8_t packet_[NETC_MAX_PACKET];
    uint8_t payload_[NETC_MAX_PAYLOAD];
};

double monotonic_seconds();
