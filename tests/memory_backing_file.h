#pragma once

#include <cstring>
#include <vector>

#include "backing_file.h"

namespace lsd {

// An in-memory IBackingFile used only by the test suite, so correctness
// tests run in milliseconds without touching disk. It can also simulate a
// crash: after `crash_after` write() calls land normally, the next call is
// torn (only its first third of bytes actually applied, the rest left
// stale) and every call after that is dropped entirely, as if it never
// reached the drive. This is the same fault model the sibling
// crash-consistency-fault-injection-harness repo drives against the real
// UringBackingFile.
class MemoryBackingFile : public IBackingFile {
public:
    explicit MemoryBackingFile(uint64_t capacity) : buf_(capacity, 0) {}

    void arm_crash(int64_t after_n_calls) { crash_after_ = after_n_calls; }
    int64_t write_calls() const { return write_calls_; }

    void write(uint64_t offset, const uint8_t* data, size_t len) override {
        write_calls_++;
        if (crash_after_ < 0 || write_calls_ <= crash_after_) {
            std::memcpy(buf_.data() + offset, data, len);
            return;
        }
        if (write_calls_ == crash_after_ + 1) {
            size_t torn = len / 3;
            std::memcpy(buf_.data() + offset, data, torn);
            return;
        }
        // dropped: never applied
    }
    void read(uint64_t offset, uint8_t* data, size_t len) const override {
        std::memcpy(data, buf_.data() + offset, len);
    }
    void flush() override {}
    uint64_t size() const override { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
    int64_t write_calls_ = 0;
    int64_t crash_after_ = -1;
};

}  // namespace lsd
