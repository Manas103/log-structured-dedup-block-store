#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lsd {

// Abstracts the physical log file so the engine's write/flush path can be
// driven through something other than a real file: the crash-consistency
// harness in the sibling repo (crash-consistency-fault-injection-harness)
// links against this interface and substitutes a shim that records every
// write and flush and can truncate or corrupt the tail to simulate power
// loss, without needing to intercept io_uring syscalls directly.
class IBackingFile {
public:
    virtual ~IBackingFile() = default;
    virtual void write(uint64_t offset, const uint8_t* data, size_t len) = 0;
    virtual void read(uint64_t offset, uint8_t* data, size_t len) const = 0;
    // A durability barrier: once flush() returns, every write() issued
    // before it is durable. The engine treats a write as "acknowledged"
    // to its caller only after the flush that covers it returns.
    virtual void flush() = 0;
    virtual uint64_t size() const = 0;
};

// Real implementation: a single on-disk file, sized up front, written and
// read through io_uring.
//
// This issues one SQE and waits for its matching CQE per call: io_uring is
// used here for the lower per-operation syscall overhead of SQ/CQ
// submission over read(2)/write(2) (or pread/pwrite), not for multi-depth
// asynchronous batching. A production engine would keep many chunk appends
// in flight at once on one ring; that is a real, disclosed scope cut, not
// an oversight (see README Limitations).
class UringBackingFile : public IBackingFile {
public:
    UringBackingFile(const std::string& path, uint64_t capacity_bytes, bool create);
    ~UringBackingFile() override;

    UringBackingFile(const UringBackingFile&) = delete;
    UringBackingFile& operator=(const UringBackingFile&) = delete;

    void write(uint64_t offset, const uint8_t* data, size_t len) override;
    void read(uint64_t offset, uint8_t* data, size_t len) const override;
    void flush() override;
    uint64_t size() const override { return capacity_; }

private:
    int fd_ = -1;
    uint64_t capacity_ = 0;
    void* ring_storage_;  // opaque struct io_uring*, hides <liburing.h> from callers
};

}  // namespace lsd
