#pragma once

#include <cstddef>
#include <cstdint>

namespace lsd {

// Castagnoli CRC-32C, table-based. Used to detect torn writes: a record
// truncated or corrupted by a simulated power loss fails this check and is
// never applied during recovery.
uint32_t crc32c(const uint8_t* data, size_t len, uint32_t seed = 0xFFFFFFFFu);

}  // namespace lsd
