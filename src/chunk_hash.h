#pragma once

#include <cstddef>
#include <cstdint>

#include "common.h"

namespace lsd {

// A deliberately narrow 32-bit FNV-1a hash, not a cryptographic digest. The
// engine always verifies a dedup candidate byte-for-byte before committing
// metadata (see DedupBlockStore::find_dedup_candidate), so the hash only
// needs to be a candidate filter: collisions cost an extra read-and-compare,
// never correctness. Keeping it small (instead of SHA-256 or even 64 bits)
// makes that verification path exercised by real collisions at a testable
// scale (see tests/test_hash_collision.cpp) rather than effectively dead
// code behind a space too large to collide in practice.
inline uint32_t chunk_hash(const uint8_t* data, size_t len) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 0x01000193u;
    }
    // Never produce the reserved "unwritten" sentinel.
    return h == kUnwrittenHash ? 0x01000193u : h;
}

}  // namespace lsd
