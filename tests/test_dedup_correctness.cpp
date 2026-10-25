#include <gtest/gtest.h>

#include <random>

#include "block_store.h"
#include "common.h"
#include "memory_backing_file.h"
#include "naive_store.h"

using namespace lsd;

namespace {

std::array<uint8_t, kLogicalBlockSize> make_block(std::mt19937_64& rng) {
    std::array<uint8_t, kLogicalBlockSize> b{};
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (auto& x : b) x = static_cast<uint8_t>(byte_dist(rng));
    return b;
}

}  // namespace

TEST(DedupCorrectness, SingleWriteReadRoundTrip) {
    MemoryBackingFile backing(64 * kSegmentSize);
    DedupBlockStore store(backing, /*num_logical_blocks=*/1024, GcPolicy::LifetimeGrouped);
    std::mt19937_64 rng(1);
    auto block = make_block(rng);
    store.write_block(5, block.data(), LifetimeClass::Hot);
    std::array<uint8_t, kLogicalBlockSize> out{};
    store.read_block(5, out.data());
    EXPECT_EQ(block, out);
}

TEST(DedupCorrectness, UnwrittenBlockReadsAsZero) {
    MemoryBackingFile backing(64 * kSegmentSize);
    DedupBlockStore store(backing, 1024, GcPolicy::LifetimeGrouped);
    std::array<uint8_t, kLogicalBlockSize> out{};
    out.fill(0xAA);
    store.read_block(42, out.data());
    for (auto b : out) EXPECT_EQ(b, 0);
}

TEST(DedupCorrectness, RepeatedWriteOfIdenticalBlockIsADedupHit) {
    MemoryBackingFile backing(64 * kSegmentSize);
    DedupBlockStore store(backing, 1024, GcPolicy::LifetimeGrouped);
    std::mt19937_64 rng(2);
    auto block = make_block(rng);
    store.write_block(0, block.data(), LifetimeClass::Hot);
    store.write_block(1, block.data(), LifetimeClass::Hot);  // same content, different LBA
    Stats s = store.stats();
    EXPECT_EQ(s.dedup_misses, kChunksPerBlock);  // first write: all 8 chunks new
    EXPECT_EQ(s.dedup_hits, kChunksPerBlock);    // second write: all 8 chunks already exist
    std::array<uint8_t, kLogicalBlockSize> out{};
    store.read_block(1, out.data());
    EXPECT_EQ(block, out);
}

// Randomized fuzz test: every write and read is mirrored against NaiveStore,
// the obviously-correct reference oracle, across a workload with a
// deliberate duplicate ratio (half of all writes reuse one of a small pool
// of canned blocks) so the dedup path is genuinely exercised, not just the
// miss path.
TEST(DedupCorrectness, FuzzAgainstNaiveOracle) {
    constexpr uint32_t kNumBlocks = 512;
    MemoryBackingFile backing(256 * kSegmentSize);
    DedupBlockStore store(backing, kNumBlocks, GcPolicy::LifetimeGrouped, 0.2);
    NaiveStore oracle(kNumBlocks);

    std::mt19937_64 rng(7);
    std::vector<std::array<uint8_t, kLogicalBlockSize>> pool;
    for (int i = 0; i < 16; ++i) pool.push_back(make_block(rng));

    std::uniform_int_distribution<uint32_t> lba_dist(0, kNumBlocks - 1);
    std::uniform_int_distribution<int> pool_dist(0, static_cast<int>(pool.size()) - 1);
    std::uniform_int_distribution<int> coin(0, 1);
    std::uniform_int_distribution<int> lifetime_dist(0, 2);

    for (int iter = 0; iter < 20000; ++iter) {
        uint32_t lba = lba_dist(rng);
        std::array<uint8_t, kLogicalBlockSize> block = coin(rng) ? pool[pool_dist(rng)] : make_block(rng);
        auto cls = static_cast<LifetimeClass>(lifetime_dist(rng));
        store.write_block(lba, block.data(), cls);
        oracle.write_block(lba, block.data());

        if (iter % 5 == 0) {
            uint32_t rlba = lba_dist(rng);
            std::array<uint8_t, kLogicalBlockSize> got{}, want{};
            store.read_block(rlba, got.data());
            oracle.read_block(rlba, want.data());
            ASSERT_EQ(got, want) << "mismatch at lba " << rlba << " iteration " << iter;
        }
        if (iter % 500 == 0) store.run_gc();
    }

    for (uint32_t lba = 0; lba < kNumBlocks; ++lba) {
        std::array<uint8_t, kLogicalBlockSize> got{}, want{};
        store.read_block(lba, got.data());
        oracle.read_block(lba, want.data());
        ASSERT_EQ(got, want) << "final mismatch at lba " << lba;
    }

    Stats s = store.stats();
    EXPECT_GT(s.dedup_hits, 0u);
    // GC migration is not guaranteed here: with this much headroom (256
    // segments for 512 blocks), segments tend to coast to full death via
    // DedupBlockStore::mark_dead's lazy free path on their own, the same
    // way GcLiveness.FreeSegmentsStayAboveReserveUnderChurn does under a
    // uniform-churn workload. See test_gc_liveness.cpp for workloads
    // deliberately shaped (and capacity deliberately tight) to force
    // migration and compare it across GC policies.
    EXPECT_LE(s.segments_free, s.segments_total);
}
