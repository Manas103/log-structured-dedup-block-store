#pragma once

#include <algorithm>
#include <array>
#include <vector>

#include "common.h"

namespace lsd {

// The reference oracle: a flat array of logical blocks with plain
// in-place overwrite. No dedup, no log structure, no GC, nothing to get
// wrong. Every randomized write/read sequence run against DedupBlockStore
// is also run against this and diffed byte-for-byte (see
// tests/test_dedup_correctness.cpp).
class NaiveStore {
public:
    explicit NaiveStore(uint32_t num_logical_blocks) : blocks_(num_logical_blocks) {}

    void write_block(uint32_t lba, const uint8_t* data) {
        std::copy(data, data + kLogicalBlockSize, blocks_.at(lba).begin());
    }
    void read_block(uint32_t lba, uint8_t* out) const {
        const auto& b = blocks_.at(lba);
        std::copy(b.begin(), b.end(), out);
    }

private:
    std::vector<std::array<uint8_t, kLogicalBlockSize>> blocks_;
};

}  // namespace lsd
