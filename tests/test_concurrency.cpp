#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <thread>

#include "block_store.h"
#include "common.h"
#include "memory_backing_file.h"

using namespace lsd;

namespace {
std::array<uint8_t, kLogicalBlockSize> make_block(std::mt19937_64& rng) {
    std::array<uint8_t, kLogicalBlockSize> b{};
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& x : b) x = static_cast<uint8_t>(byte_dist(rng));
    return b;
}
}  // namespace

// The real concurrency this engine has: a background GC thread (the NBD
// server's gc_thread in main_nbd_server.cpp) runs run_gc() in a loop while
// foreground write_block/read_block calls keep arriving on other threads.
// Every public method takes `mutex_`, so this is a lock-correctness test
// (no two threads touch segments_/entries_/lba_map_ unsynchronized) rather
// than a lock-free design; ThreadSanitizer is what actually checks that
// claim instead of just asserting it.
TEST(Concurrency, WriterReaderAndGcThreadsRaceCleanly) {
    constexpr uint32_t kNumBlocks = 64;
    constexpr uint64_t kSegs = 10;
    MemoryBackingFile backing(kSegs * kSegmentSize);
    DedupBlockStore store(backing, kNumBlocks, GcPolicy::LifetimeGrouped, 0.2);

    std::atomic<bool> stop{false};
    std::atomic<int> reads_ok{0};

    std::thread writer([&] {
        std::mt19937_64 rng(42);
        for (int i = 0; i < 4000; ++i) {
            uint32_t lba = static_cast<uint32_t>(i % kNumBlocks);
            auto block = make_block(rng);
            store.write_block(lba, block.data(), LifetimeClass::Hot);
        }
    });

    std::thread gc_thread([&] {
        while (!stop.load()) {
            store.run_gc();
            std::this_thread::yield();
        }
    });

    std::thread reader([&] {
        std::array<uint8_t, kLogicalBlockSize> out{};
        while (!stop.load()) {
            store.read_block(0, out.data());
            reads_ok++;
            std::this_thread::yield();
        }
    });

    writer.join();
    stop = true;
    gc_thread.join();
    reader.join();

    EXPECT_GT(reads_ok.load(), 0);
    Stats s = store.stats();
    EXPECT_LE(s.segments_free, s.segments_total);
}
