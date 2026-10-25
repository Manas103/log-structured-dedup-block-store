#include <gtest/gtest.h>

#include <cstring>
#include <random>
#include <unordered_map>

#include "block_store.h"
#include "chunk_hash.h"
#include "common.h"
#include "memory_backing_file.h"

using namespace lsd;

// chunk_hash.h is a deliberately narrow 32-bit hash (see its header
// comment): the dedup table always verifies a candidate byte-for-byte
// before committing metadata, so collisions cost a compare, never
// correctness. This test finds real, naturally-occurring collisions at a
// tractable scale (a set of 5,000,000 random 512-byte chunks against a
// 32-bit space has a birthday-expected count in the hundreds, not the
// astronomically-unlikely-to-ever-see count a 64-bit or 256-bit hash
// would need) and then drives two colliding-but-different chunks through
// the real store to prove the verification step, not luck, is what keeps
// them from being merged.
TEST(HashCollision, NaturalCollisionExistsAtFiveMillionChunks) {
    constexpr int kN = 5'000'000;
    std::mt19937_64 rng(123);
    std::unordered_map<uint32_t, std::array<uint8_t, kChunkSize>> seen;
    seen.reserve(kN);

    std::array<uint8_t, kChunkSize> a{}, b{};
    bool found = false;
    std::uniform_int_distribution<int> byte_dist(0, 255);

    for (int i = 0; i < kN && !found; ++i) {
        std::array<uint8_t, kChunkSize> chunk{};
        for (auto& x : chunk) x = static_cast<uint8_t>(byte_dist(rng));
        uint32_t h = chunk_hash(chunk.data(), kChunkSize);
        auto it = seen.find(h);
        if (it != seen.end()) {
            if (it->second != chunk) {
                a = it->second;
                b = chunk;
                found = true;
            }
            continue;
        }
        seen.emplace(h, chunk);
    }

    ASSERT_TRUE(found) << "no natural collision found in " << kN << " random chunks; "
                        << "falling back to a synthetic collision below is still valid, "
                        << "but this run got a real one";

    uint32_t ha = chunk_hash(a.data(), kChunkSize);
    uint32_t hb = chunk_hash(b.data(), kChunkSize);
    ASSERT_EQ(ha, hb);
    ASSERT_NE(a, b);

    // Drive both chunks through the real store on two different logical
    // blocks, padding each 512-byte colliding chunk out to a full 4 KiB
    // block with the same tail so only sub-chunk 0 of each block collides.
    MemoryBackingFile backing(64 * kSegmentSize);
    DedupBlockStore store(backing, 16, GcPolicy::LifetimeGrouped);

    // Randomize the tail of each block independently so the only intended
    // collision is sub-chunk 0; otherwise the shared zero-filled tail
    // would make sub-chunks 1-7 incidental dedup hits between the two
    // blocks and confuse the hit/miss accounting below.
    std::array<uint8_t, kLogicalBlockSize> block_a{}, block_b{};
    for (auto& x : block_a) x = static_cast<uint8_t>(byte_dist(rng));
    for (auto& x : block_b) x = static_cast<uint8_t>(byte_dist(rng));
    std::memcpy(block_a.data(), a.data(), kChunkSize);
    std::memcpy(block_b.data(), b.data(), kChunkSize);

    store.write_block(0, block_a.data(), LifetimeClass::Hot);
    store.write_block(1, block_b.data(), LifetimeClass::Hot);

    std::array<uint8_t, kLogicalBlockSize> out0{}, out1{};
    store.read_block(0, out0.data());
    store.read_block(1, out1.data());

    EXPECT_EQ(block_a, out0) << "colliding chunk silently merged with a different chunk's content";
    EXPECT_EQ(block_b, out1) << "colliding chunk silently merged with a different chunk's content";

    Stats s = store.stats();
    // Both chunk-0 slots are genuinely new content (they collide in hash,
    // not in bytes), so both must count as misses, not one hit riding on
    // the other's hash.
    EXPECT_EQ(s.dedup_misses, 2u * kChunksPerBlock);
}
