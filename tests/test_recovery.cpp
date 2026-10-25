#include <gtest/gtest.h>

#include <memory>
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
}  // namespace

// The reference model this test enforces: after a simulated crash,
// recovery must show exactly the acknowledged prefix. A write is
// "acknowledged" once flush() has been called and returned after it was
// issued. Writes issued after the last completed flush before the crash
// may come back as their old value or their new value, but never a
// torn/corrupted mix of the two within one 512-byte chunk, because the
// CRC check in DedupBlockStore::read_record rejects any record that is
// not byte-for-byte exactly what was written.
TEST(Recovery, AcknowledgedPrefixSurvivesCrash) {
    constexpr uint32_t kNumBlocks = 24;
    constexpr uint64_t kSegs = 32;
    auto backing = std::make_unique<MemoryBackingFile>(kSegs * kSegmentSize);
    std::mt19937_64 rng(99);

    std::vector<std::array<uint8_t, kLogicalBlockSize>> pre_batch_content(kNumBlocks);
    std::vector<std::array<uint8_t, kLogicalBlockSize>> new_content(kNumBlocks);
    {
        DedupBlockStore store(*backing, kNumBlocks, GcPolicy::LifetimeGrouped, 0.2);
        for (uint32_t lba = 0; lba < kNumBlocks; ++lba) {
            pre_batch_content[lba] = make_block(rng);
            store.write_block(lba, pre_batch_content[lba].data(), LifetimeClass::Hot);
        }
        store.flush();  // everything so far is acknowledged

        // A second, unacknowledged batch: no flush() follows it before the
        // simulated crash. Every chunk here is fresh random content (a
        // guaranteed dedup miss), so it is exactly kChunksPerBlock backing
        // writes per block; arm the crash *before* issuing the batch, at
        // the call count the midpoint of the batch will reach, so the
        // fault actually lands inside it rather than merely being
        // recorded after the fact.
        int64_t calls_before_batch = backing->write_calls();
        int64_t expected_calls_in_batch = static_cast<int64_t>(kNumBlocks) * kChunksPerBlock;
        backing->arm_crash(calls_before_batch + expected_calls_in_batch / 2);

        for (uint32_t lba = 0; lba < kNumBlocks; ++lba) {
            new_content[lba] = make_block(rng);
            store.write_block(lba, new_content[lba].data(), LifetimeClass::Hot);
        }

        // One more write_block call, also after the crash point: exercises
        // the fault path for a write that was "in flight" at power loss.
        auto trailing = make_block(rng);
        store.write_block(0, trailing.data(), LifetimeClass::Hot);

        // store goes out of scope here: simulates the process dying, with
        // no further flush and no clean shutdown.
    }

    // "Restart": a fresh store instance over the same (now fault-injected)
    // backing data.
    DedupBlockStore recovered(*backing, kNumBlocks, GcPolicy::LifetimeGrouped, 0.2);
    uint64_t applied = recovered.recover();
    EXPECT_GT(applied, 0u);

    // The unacknowledged batch is not guaranteed to be entirely absent: a
    // write the test issued after the crash point is still permitted to
    // have landed durably before power was actually "lost" (the crash
    // model only promises the acknowledged write is never missing and a
    // torn write never surfaces corrupted, not that nothing unacknowledged
    // ever survives). So every LBA must equal either its pre-batch value
    // or its new, unacknowledged value, and never a third, corrupted one.
    for (uint32_t lba = 0; lba < kNumBlocks; ++lba) {
        std::array<uint8_t, kLogicalBlockSize> out{};
        recovered.read_block(lba, out.data());
        bool is_pre = (out == pre_batch_content[lba]);
        bool is_new = (out == new_content[lba]);
        EXPECT_TRUE(is_pre || is_new) << "lba " << lba << ": recovered content matches neither the acknowledged "
                                       << "write nor the unacknowledged one, i.e. it is torn or corrupted";
    }
}

TEST(Recovery, RecoveringTwiceInARowIsIdempotent) {
    constexpr uint32_t kNumBlocks = 8;
    MemoryBackingFile backing(16 * kSegmentSize);
    std::mt19937_64 rng(5);
    {
        DedupBlockStore store(backing, kNumBlocks, GcPolicy::LifetimeGrouped);
        for (uint32_t lba = 0; lba < kNumBlocks; ++lba) {
            auto b = make_block(rng);
            store.write_block(lba, b.data(), LifetimeClass::Hot);
        }
        store.flush();
    }
    DedupBlockStore r1(backing, kNumBlocks, GcPolicy::LifetimeGrouped);
    uint64_t applied1 = r1.recover();
    DedupBlockStore r2(backing, kNumBlocks, GcPolicy::LifetimeGrouped);
    uint64_t applied2 = r2.recover();
    EXPECT_EQ(applied1, applied2);
    for (uint32_t lba = 0; lba < kNumBlocks; ++lba) {
        std::array<uint8_t, kLogicalBlockSize> a{}, b{};
        r1.read_block(lba, a.data());
        r2.read_block(lba, b.data());
        EXPECT_EQ(a, b);
    }
}
