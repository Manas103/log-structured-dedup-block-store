#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace lsd {

constexpr size_t kLogicalBlockSize = 4096;
constexpr size_t kChunkSize = 512;
constexpr size_t kChunksPerBlock = kLogicalBlockSize / kChunkSize;  // 8
// Deliberately small (1 MiB, not the 8-32 MiB a real log-structured store
// would use) so that one segment's worth of GC migration, done under a
// single store-wide lock (see DedupBlockStore::gc_migrate_segment), bounds
// the longest a foreground read can be blocked behind reclamation. See
// README Findings for what measuring that tradeoff at 8 MiB looked like.
constexpr size_t kSegmentSize = 1u * 1024 * 1024;  // 1 MiB

// A chunk hash of 0 is reserved to mean "never written" (reads as zeros).
// It is vanishingly unlikely for a real chunk's hash to be 0 and the dedup
// table never stores an entry under key 0, so the sentinel is exact, not
// probabilistic.
constexpr uint32_t kUnwrittenHash = 0;

enum class LifetimeClass : uint8_t { Hot = 0, Warm = 1, Cold = 2 };
constexpr size_t kLifetimeClassCount = 3;

enum class GcPolicy { LifetimeGrouped, Fifo };

struct PhysicalAddr {
    uint32_t segment_id = std::numeric_limits<uint32_t>::max();
    uint32_t slot = std::numeric_limits<uint32_t>::max();

    bool valid() const { return segment_id != std::numeric_limits<uint32_t>::max(); }
    bool operator==(const PhysicalAddr& o) const { return segment_id == o.segment_id && slot == o.slot; }
    bool operator!=(const PhysicalAddr& o) const { return !(*this == o); }
};

using ChunkBuffer = std::array<uint8_t, kChunkSize>;

}  // namespace lsd
