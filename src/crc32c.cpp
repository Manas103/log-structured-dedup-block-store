#include "crc32c.h"

namespace lsd {
namespace {

constexpr uint32_t kPoly = 0x82F63B78u;  // reversed Castagnoli polynomial

struct Table {
    uint32_t t[256];
    constexpr Table() : t{} {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (kPoly ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
    }
};

constexpr Table kTable{};

}  // namespace

uint32_t crc32c(const uint8_t* data, size_t len, uint32_t seed) {
    uint32_t crc = seed;
    for (size_t i = 0; i < len; ++i) {
        crc = kTable.t[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

}  // namespace lsd
