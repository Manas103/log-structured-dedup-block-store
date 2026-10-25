#pragma once

#include <atomic>
#include <cstdint>

#include "block_store.h"

namespace lsd {

// A single-export Linux NBD server. Implements the oldstyle negotiation
// (simpler than newstyle's export-name haggling, and this process only
// ever hosts one export) followed by the standard transmission phase, so
// the real kernel NBD driver, attached via `nbd-client`, can mount this
// store as a genuine block device.
class NbdServer {
public:
    NbdServer(DedupBlockStore& store, uint64_t export_size_bytes, int port);

    // Blocks, accepting and serving one client connection at a time
    // (matching nbd-client's single-connection default) until stop() is
    // called from another thread or the client disconnects and
    // `serve_once` is true.
    void run(bool serve_once);
    void stop();

private:
    bool negotiate(int fd);
    void serve_connection(int fd);

    DedupBlockStore& store_;
    uint64_t export_size_;
    int port_;
    int listen_fd_ = -1;
    std::atomic<bool> stop_requested_{false};
};

}  // namespace lsd
