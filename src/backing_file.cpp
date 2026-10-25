#include "backing_file.h"

#include <fcntl.h>
#include <liburing.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace lsd {

UringBackingFile::UringBackingFile(const std::string& path, uint64_t capacity_bytes, bool create)
    : capacity_(capacity_bytes) {
    int flags = O_RDWR | (create ? (O_CREAT | O_TRUNC) : 0);
    fd_ = ::open(path.c_str(), flags, 0644);
    if (fd_ < 0) throw std::runtime_error("UringBackingFile: open failed for " + path);
    if (create) {
        if (::ftruncate(fd_, static_cast<off_t>(capacity_bytes)) != 0) {
            throw std::runtime_error("UringBackingFile: ftruncate failed");
        }
    }
    auto* ring = new io_uring();
    if (io_uring_queue_init(8, ring, 0) < 0) {
        delete ring;
        throw std::runtime_error("UringBackingFile: io_uring_queue_init failed");
    }
    ring_storage_ = ring;
}

UringBackingFile::~UringBackingFile() {
    auto* ring = static_cast<io_uring*>(ring_storage_);
    if (ring) {
        io_uring_queue_exit(ring);
        delete ring;
    }
    if (fd_ >= 0) ::close(fd_);
}

void UringBackingFile::write(uint64_t offset, const uint8_t* data, size_t len) {
    auto* ring = static_cast<io_uring*>(ring_storage_);
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    io_uring_prep_write(sqe, fd_, data, len, offset);
    io_uring_submit(ring);
    io_uring_cqe* cqe = nullptr;
    if (io_uring_wait_cqe(ring, &cqe) < 0) throw std::runtime_error("UringBackingFile: wait_cqe failed (write)");
    int res = cqe->res;
    io_uring_cqe_seen(ring, cqe);
    if (res != static_cast<int>(len)) throw std::runtime_error("UringBackingFile: short/failed write");
}

void UringBackingFile::read(uint64_t offset, uint8_t* data, size_t len) const {
    auto* ring = static_cast<io_uring*>(const_cast<void*>(ring_storage_));
    io_uring_sqe* sqe = io_uring_get_sqe(ring);
    io_uring_prep_read(sqe, fd_, data, len, offset);
    io_uring_submit(ring);
    io_uring_cqe* cqe = nullptr;
    if (io_uring_wait_cqe(ring, &cqe) < 0) throw std::runtime_error("UringBackingFile: wait_cqe failed (read)");
    int res = cqe->res;
    io_uring_cqe_seen(ring, cqe);
    if (res != static_cast<int>(len)) throw std::runtime_error("UringBackingFile: short/failed read");
}

void UringBackingFile::flush() {
    if (::fsync(fd_) != 0) throw std::runtime_error("UringBackingFile: fsync failed");
}

}  // namespace lsd
