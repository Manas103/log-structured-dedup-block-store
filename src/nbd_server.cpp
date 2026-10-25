#include "nbd_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <endian.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "nbd_protocol.h"

namespace lsd {
namespace {

bool read_exact(int fd, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::recv(fd, p + got, len - got, 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

bool write_exact(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::send(fd, p + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

// The NBD protocol carries no per-write lifetime hint, so this server
// derives one the way a real deployment would for data it cannot ask the
// host about: by address convention. The lower half of the exported LBA
// space is treated as Hot, the upper half as Cold. A benchmark that wants
// to exercise lifetime-grouped GC drives its churny, frequently-rewritten
// I/O at the lower half and its stable, rarely-touched I/O at the upper
// half, exactly the shape a real journal-vs-reference-data split would
// produce.
LifetimeClass lifetime_for_lba(uint64_t lba, uint64_t total_blocks) {
    return (lba < total_blocks / 2) ? LifetimeClass::Hot : LifetimeClass::Cold;
}

NbdServer::NbdServer(DedupBlockStore& store, uint64_t export_size_bytes, int port)
    : store_(store), export_size_(export_size_bytes), port_(port) {}

void NbdServer::stop() {
    stop_requested_ = true;
    if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
}

void NbdServer::run(bool serve_once) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) throw std::runtime_error("NbdServer: socket() failed");
    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        throw std::runtime_error("NbdServer: bind() failed, errno=" + std::to_string(errno));
    }
    if (::listen(listen_fd_, 1) != 0) throw std::runtime_error("NbdServer: listen() failed");

    while (!stop_requested_) {
        int client = ::accept(listen_fd_, nullptr, nullptr);
        if (client < 0) {
            if (stop_requested_) break;
            continue;
        }
        int one = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        try {
            serve_connection(client);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "nbd_server: connection error: %s\n", e.what());
        }
        ::close(client);
        if (serve_once) break;
    }
    ::close(listen_fd_);
    listen_fd_ = -1;
}

// Newstyle fixed negotiation with a single, unnamed default export. Modern
// nbd-client (3.x) dropped oldstyle support entirely ("the oldstyle
// protocol is no longer supported"), so this is the protocol version that
// actually matters for a real kernel attach, even though it only ever
// needs to handle the one option (NBD_OPT_EXPORT_NAME) any client that
// just wants "the" export sends.
bool NbdServer::negotiate(int fd) {
    uint64_t magic1 = htobe64(nbd::kNegotiationMagic);
    uint64_t magic2 = htobe64(nbd::kIHaveOpt);
    uint16_t handshake_flags = htons(nbd::kHandshakeFlagFixedNewstyle);
    if (!write_exact(fd, &magic1, 8) || !write_exact(fd, &magic2, 8) || !write_exact(fd, &handshake_flags, 2)) {
        return false;
    }

    uint32_t client_flags_wire;
    if (!read_exact(fd, &client_flags_wire, 4)) return false;
    uint32_t client_flags = ntohl(client_flags_wire);
    bool no_zeroes = client_flags & nbd::kClientFlagNoZeroes;

    for (;;) {
        uint64_t opt_magic_wire;
        uint32_t option_wire, len_wire;
        if (!read_exact(fd, &opt_magic_wire, 8)) return false;
        if (be64toh(opt_magic_wire) != nbd::kIHaveOpt) return false;
        if (!read_exact(fd, &option_wire, 4) || !read_exact(fd, &len_wire, 4)) return false;
        uint32_t option = ntohl(option_wire);
        uint32_t len = ntohl(len_wire);
        std::vector<uint8_t> data(len);
        if (len > 0 && !read_exact(fd, data.data(), len)) return false;

        if (option == nbd::kOptExportName) {
            uint64_t size_be = htobe64(export_size_);
            uint16_t tflags = htons(nbd::kTransmissionFlagHasFlags | nbd::kTransmissionFlagSendFlush |
                                     nbd::kTransmissionFlagSendFua);
            if (!write_exact(fd, &size_be, 8) || !write_exact(fd, &tflags, 2)) return false;
            if (!no_zeroes) {
                uint8_t zero[124] = {};
                if (!write_exact(fd, zero, sizeof(zero))) return false;
            }
            return true;  // negotiation done; transmission phase starts immediately
        }

        auto send_option_reply = [&](uint32_t reply_type, const uint8_t* reply_data, uint32_t reply_len) -> bool {
            uint64_t m = htobe64(nbd::kOptionReplyMagic);
            uint32_t opt_be = htonl(option);
            uint32_t type_be = htonl(reply_type);
            uint32_t len_be = htonl(reply_len);
            return write_exact(fd, &m, 8) && write_exact(fd, &opt_be, 4) && write_exact(fd, &type_be, 4) &&
                   write_exact(fd, &len_be, 4) && (reply_len == 0 || write_exact(fd, reply_data, reply_len));
        };

        if (option == nbd::kOptAbort) {
            send_option_reply(nbd::kRepAck, nullptr, 0);
            return false;
        }
        // Everything else (NBD_OPT_LIST, NBD_OPT_STARTTLS, ...) is simply
        // unsupported: this server hosts exactly one export and nothing
        // needs discovering.
        if (!send_option_reply(nbd::kRepErrUnsup, nullptr, 0)) return false;
    }
}

void NbdServer::serve_connection(int fd) {
    if (!negotiate(fd)) return;

    std::vector<uint8_t> iobuf;
    for (;;) {
        nbd::Request req_wire;
        if (!read_exact(fd, &req_wire, sizeof(req_wire))) return;
        uint32_t magic = ntohl(req_wire.magic);
        uint16_t flags = ntohs(req_wire.flags);
        uint16_t type = ntohs(req_wire.type);
        uint64_t offset = be64toh(req_wire.offset);
        uint32_t length = ntohl(req_wire.length);
        uint64_t handle_raw = req_wire.handle;  // opaque: echoed back without reinterpretation
        if (magic != nbd::kRequestMagic) return;

        auto send_reply = [&](uint32_t error, const uint8_t* data, size_t data_len) -> bool {
            nbd::SimpleReply reply;
            reply.magic = htonl(nbd::kSimpleReplyMagic);
            reply.error = htonl(error);
            reply.handle = handle_raw;
            if (!write_exact(fd, &reply, sizeof(reply))) return false;
            if (data_len > 0 && !write_exact(fd, data, data_len)) return false;
            return true;
        };

        if (type == nbd::kCmdDisc) return;

        if (type == nbd::kCmdFlush) {
            store_.flush();
            if (!send_reply(0, nullptr, 0)) return;
            continue;
        }

        if (type == nbd::kCmdWrite || type == nbd::kCmdRead) {
            if (offset % kLogicalBlockSize != 0 || length % kLogicalBlockSize != 0) {
                // Misaligned request: this server only implements whole
                // 4 KiB logical blocks (see README Limitations). Mount
                // with nbd-client's -b 4096 so the kernel never issues
                // anything else.
                if (type == nbd::kCmdWrite) {
                    iobuf.resize(length);
                    if (!read_exact(fd, iobuf.data(), length)) return;
                }
                if (!send_reply(EINVAL, nullptr, 0)) return;
                continue;
            }
        }

        if (type == nbd::kCmdWrite) {
            iobuf.resize(length);
            if (!read_exact(fd, iobuf.data(), length)) return;
            bool fua = flags & nbd::kCmdFlagFua;
            uint32_t num_blocks = length / kLogicalBlockSize;
            uint64_t first_lba = offset / kLogicalBlockSize;
            uint64_t total_blocks = export_size_ / kLogicalBlockSize;
            for (uint32_t i = 0; i < num_blocks; ++i) {
                uint64_t lba = first_lba + i;
                store_.write_block(static_cast<uint32_t>(lba), iobuf.data() + i * kLogicalBlockSize,
                                    lifetime_for_lba(lba, total_blocks), fua && (i == num_blocks - 1));
            }
            if (!send_reply(0, nullptr, 0)) return;
        } else if (type == nbd::kCmdRead) {
            iobuf.resize(length);
            uint32_t num_blocks = length / kLogicalBlockSize;
            uint64_t first_lba = offset / kLogicalBlockSize;
            for (uint32_t i = 0; i < num_blocks; ++i) {
                store_.read_block(static_cast<uint32_t>(first_lba + i), iobuf.data() + i * kLogicalBlockSize);
            }
            if (!send_reply(0, iobuf.data(), length)) return;
        } else {
            // Unsupported command (e.g. NBD_CMD_TRIM): acknowledge as a
            // no-op rather than hanging the client.
            if (!send_reply(0, nullptr, 0)) return;
        }
    }
}

}  // namespace lsd
