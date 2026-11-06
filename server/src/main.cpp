#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "server.h"

static volatile std::sig_atomic_t g_running = 1;

static void on_signal(int)
{
    g_running = 0;
}

static void usage(const char *prog)
{
    printf("usage: %s [options]\n", prog);
    printf("  --port N           udp port (default 40000)\n");
    printf("  --max-players N    player limit, up to %d (default 8)\n", kMaxPlayers);
    printf("  --no-delta         always send full snapshots\n");
    printf("  --duration SEC     stop after SEC seconds\n");
    printf("  --stats FILE       write stats as json on exit\n");
    printf("  --tick-log FILE    write every player position per tick (csv)\n");
}

int main(int argc, char **argv)
{
    ServerConfig config;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        bool has_value = i + 1 < argc;
        if (strcmp(arg, "--port") == 0 && has_value) {
            config.port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(arg, "--max-players") == 0 && has_value) {
            config.max_players = atoi(argv[++i]);
        } else if (strcmp(arg, "--no-delta") == 0) {
            config.delta = false;
        } else if (strcmp(arg, "--duration") == 0 && has_value) {
            config.duration = atof(argv[++i]);
        } else if (strcmp(arg, "--stats") == 0 && has_value) {
            config.stats_path = argv[++i];
        } else if (strcmp(arg, "--tick-log") == 0 && has_value) {
            config.tick_log_path = argv[++i];
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option %s\n", arg);
            usage(argv[0]);
            return 1;
        }
    }

    setvbuf(stdout, nullptr, _IOLBF, 0);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    static Server server(config);
    if (!server.start())
        return 1;
    server.run(g_running);
    server.write_stats();
    return 0;
}
