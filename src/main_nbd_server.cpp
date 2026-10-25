#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include "backing_file.h"
#include "block_store.h"
#include "nbd_server.h"

using namespace lsd;

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

void write_stats_json(const std::string& path, const Stats& s) {
    std::ofstream out(path, std::ios::trunc);
    out << "{"
        << "\"host_bytes_written\":" << s.host_bytes_written << ","
        << "\"media_bytes_written\":" << s.media_bytes_written << ","
        << "\"write_amplification\":" << s.write_amplification() << ","
        << "\"utilization\":" << s.utilization() << ","
        << "\"gc_runs\":" << s.gc_runs << ","
        << "\"gc_bytes_migrated\":" << s.gc_bytes_migrated << ","
        << "\"segments_total\":" << s.segments_total << ","
        << "\"segments_free\":" << s.segments_free << ","
        << "\"dedup_hits\":" << s.dedup_hits << ","
        << "\"dedup_misses\":" << s.dedup_misses << "}";
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr,
                      "usage: %s <backing_path> <backing_capacity_bytes> <num_logical_blocks> <port> "
                      "<gc_policy: lifetime|fifo> <mode: create|reuse> [stats_json_path]\n",
                      argv[0]);
        return 2;
    }
    std::string backing_path = argv[1];
    uint64_t capacity = std::strtoull(argv[2], nullptr, 10);
    uint32_t num_blocks = static_cast<uint32_t>(std::strtoull(argv[3], nullptr, 10));
    int port = std::atoi(argv[4]);
    GcPolicy policy = (std::string(argv[5]) == "fifo") ? GcPolicy::Fifo : GcPolicy::LifetimeGrouped;
    bool create = (std::string(argv[6]) == "create");
    std::string stats_path = argc > 7 ? argv[7] : "";

    UringBackingFile backing(backing_path, capacity, create);
    DedupBlockStore store(backing, num_blocks, policy);
    if (!create) {
        uint64_t applied = store.recover();
        std::fprintf(stderr, "recovered %llu records\n", static_cast<unsigned long long>(applied));
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::thread gc_thread([&] {
        while (!g_stop) {
            store.run_gc();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });
    std::thread stats_thread([&] {
        while (!g_stop) {
            if (!stats_path.empty()) write_stats_json(stats_path, store.stats());
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        if (!stats_path.empty()) write_stats_json(stats_path, store.stats());
    });

    uint64_t export_size = static_cast<uint64_t>(num_blocks) * kLogicalBlockSize;
    NbdServer server(store, export_size, port);
    std::thread watchdog([&] {
        while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        server.stop();
    });

    std::fprintf(stderr, "nbd_server listening on 127.0.0.1:%d, export %llu bytes (%u blocks), gc=%s\n", port,
                 static_cast<unsigned long long>(export_size), num_blocks, argv[5]);
    server.run(/*serve_once=*/false);

    g_stop = true;
    gc_thread.join();
    stats_thread.join();
    watchdog.join();
    store.flush();
    std::fprintf(stderr, "nbd_server clean shutdown\n");
    return 0;
}
