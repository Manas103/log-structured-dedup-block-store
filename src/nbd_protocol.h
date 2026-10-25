#pragma once

#include <cstdint>

// Constants from the Linux NBD protocol (newstyle, fixed negotiation),
// defined locally rather than pulled from <linux/nbd.h> so this repository
// builds without kernel headers present. Values match
// https://github.com/NetworkBlockDevice/nbd/blob/master/doc/proto.md.
namespace lsd::nbd {

constexpr uint64_t kNegotiationMagic = 0x4e42444d41474943ULL;  // "NBDMAGIC"
constexpr uint64_t kIHaveOpt = 0x49484156454f5054ULL;          // "IHAVEOPT"
constexpr uint64_t kOptionReplyMagic = 0x3e889045565a9ULL;
constexpr uint16_t kHandshakeFlagFixedNewstyle = 1 << 0;
constexpr uint32_t kClientFlagFixedNewstyle = 1 << 0;

constexpr uint32_t kOptExportName = 1;
constexpr uint32_t kOptAbort = 2;
constexpr uint32_t kOptList = 3;
constexpr uint32_t kOptStartTls = 5;

constexpr uint64_t kOptionReplyMagicV2 = 0x3e889045565a9ULL;  // same as kOptionReplyMagic, kept distinct for clarity
constexpr uint32_t kRepAck = 1;
constexpr uint32_t kRepServer = 2;
constexpr uint32_t kRepErrUnsup = 0x80000001u;
constexpr uint32_t kClientFlagNoZeroes = 1u << 1;

constexpr uint16_t kTransmissionFlagHasFlags = 1 << 0;
constexpr uint16_t kTransmissionFlagSendFlush = 1 << 2;
constexpr uint16_t kTransmissionFlagSendFua = 1 << 3;

constexpr uint32_t kRequestMagic = 0x25609513;
constexpr uint32_t kSimpleReplyMagic = 0x67446698;

constexpr uint16_t kCmdRead = 0;
constexpr uint16_t kCmdWrite = 1;
constexpr uint16_t kCmdDisc = 2;
constexpr uint16_t kCmdFlush = 3;

constexpr uint16_t kCmdFlagFua = 1 << 0;

#pragma pack(push, 1)
struct Request {
    uint32_t magic;
    uint16_t flags;
    uint16_t type;
    uint64_t handle;
    uint64_t offset;
    uint32_t length;
};
struct SimpleReply {
    uint32_t magic;
    uint32_t error;
    uint64_t handle;
};
#pragma pack(pop)
static_assert(sizeof(Request) == 28, "NBD request must be the wire-exact size");
static_assert(sizeof(SimpleReply) == 16, "NBD simple reply must be the wire-exact size");

}  // namespace lsd::nbd
