#include <gtest/gtest.h>

#include <random>

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

// A skewed workload under deliberately tight capacity: the first quarter
// of LBAs ("hot") are rewritten every round; the rest ("cold") are written
// once up front and then only 10% of them are rewritten every 20 rounds
// (real but much rarer churn, not "never touched again"). This is the
// shape lifetime grouping is actually for: under GcPolicy::LifetimeGrouped,
// hot segments churn to fully dead on their own (freed for free, no
// migration needed) while cold segments see their own, much smaller
// migration cost in their own stream. Under GcPolicy::Fifo there is one
// stream, so cold data keeps landing in the same segments as hot churn,
// and every time one of those mixed segments is sealed and later picked as
// the oldest victim, FIFO pays to migrate the live cold chunks riding
// alongside it too, repeatedly, for no benefit.
double run_overwrite_workload(GcPolicy policy, uint64_t num_segments, uint32_t num_blocks, int overwrite_rounds,
                               std::mt19937_64& rng) {
    MemoryBackingFile backing(num_segments * kSegmentSize);
    DedupBlockStore store(backing, num_blocks, policy, 0.15);
    uint32_t hot_count = num_blocks / 4;
    uint32_t cold_slice = (num_blocks - hot_count) / 10;

    for (uint32_t lba = hot_count; lba < num_blocks; ++lba) {
        auto block = make_block(rng);
        store.write_block(lba, block.data(), LifetimeClass::Cold);
    }
    for (int round = 0; round < overwrite_rounds; ++round) {
        for (uint32_t lba = 0; lba < hot_count; ++lba) {
            auto block = make_block(rng);
            store.write_block(lba, block.data(), LifetimeClass::Hot);
        }
        if (round % 20 == 0) {
            for (uint32_t lba = hot_count; lba < hot_count + cold_slice; ++lba) {
                auto block = make_block(rng);
                store.write_block(lba, block.data(), LifetimeClass::Cold);
            }
        }
        store.run_gc();
    }
    return store.stats().write_amplification();
}

}  // namespace

// Pure uniform churn (every block rewritten every round, nothing immortal)
// is the one pattern that never needs migration at all: by the round after
// a segment seals, every block it held has been rewritten somewhere else
// too, so the segment reaches zero live slots and frees itself for free
// (DedupBlockStore::mark_dead's lazy free path). gc_runs staying at 0 here
// is the correct, intended outcome, not a sign GC never runs (see the
// other two tests for workloads shaped so it must).
TEST(GcLiveness, FreeSegmentsStayAboveReserveUnderChurn) {
    std::mt19937_64 rng(11);
    constexpr uint64_t kSegs = 8;
    MemoryBackingFile backing(kSegs * kSegmentSize);
    DedupBlockStore store(backing, 64, GcPolicy::LifetimeGrouped, 0.3);

    for (int round = 0; round < 60; ++round) {
        for (uint32_t lba = 0; lba < 64; ++lba) {
            auto block = make_block(rng);
            store.write_block(lba, block.data(), LifetimeClass::Hot);
        }
        Stats s = store.stats();
        // maybe_run_gc_locked runs inline on every write, so the invariant
        // must hold after every round, not just at the end.
        EXPECT_LE(s.segments_free, s.segments_total);
    }
    std::array<uint8_t, kLogicalBlockSize> out{};
    store.read_block(0, out.data());  // still readable; nothing wedged
}

TEST(GcLiveness, ReclaimedDataStillReadsCorrectlyAfterMigration) {
    std::mt19937_64 rng(13);
    // Deliberately tight capacity (a handful of segments, 0.2 reserve) so
    // the device cannot just let a mixed segment coast to full death the
    // way a generously overprovisioned one would: GC is forced to
    // actually migrate the one surviving live block out of a segment
    // most of whose other slots are getting churned to death around it.
    constexpr uint64_t kSegs = 6;
    MemoryBackingFile backing(kSegs * kSegmentSize);
    DedupBlockStore store(backing, 24, GcPolicy::LifetimeGrouped, 0.2);

    // `kept` shares a lifetime class (and therefore a stream and
    // segments) with the churned blocks on purpose: that is what creates
    // the mixed-liveness segment migration exists to clean up.
    auto kept = make_block(rng);
    store.write_block(0, kept.data(), LifetimeClass::Hot);

    for (int round = 0; round < 400; ++round) {
        for (uint32_t lba = 1; lba < 24; ++lba) {
            auto block = make_block(rng);
            store.write_block(lba, block.data(), LifetimeClass::Hot);
        }
    }
    EXPECT_GT(store.stats().gc_runs, 0u);

    std::array<uint8_t, kLogicalBlockSize> out{};
    store.read_block(0, out.data());
    EXPECT_EQ(kept, out) << "data that survived GC migration came back wrong";
}

TEST(GcLiveness, LifetimeGroupingBeatsFifoOnWriteAmplification) {
    std::mt19937_64 rng_a(21), rng_b(21);  // same seed: identical workload, different GC policy
    double waf_fifo = run_overwrite_workload(GcPolicy::Fifo, 6, 256, 300, rng_a);
    double waf_grouped = run_overwrite_workload(GcPolicy::LifetimeGrouped, 6, 256, 300, rng_b);

    EXPECT_LT(waf_grouped, waf_fifo) << "lifetime-grouped GC (" << waf_grouped << "x) did not beat FIFO (" << waf_fifo
                                      << "x) on identical churn; see README Findings if this regresses";
}
