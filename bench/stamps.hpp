// Note 010's stamps (D1): for each message's sequence number, the time the
// sender meant to send its packet (CLOCK_MONOTONIC ns, one clock for every
// process on the machine; a network namespace does not separate /dev/shm, so
// the card path shares it too). Written by feed_sender before each packet,
// read by a consumer when an update caused by that message arrives. Indexed
// by sequence number modulo its size: 4 million, far more than are ever in
// flight. Design: docs/design/010-measurement.md
#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace itchfh::bench {

struct stamp_array {
    static constexpr std::size_t n = 1u << 22;
    std::atomic<std::int64_t> v[n];
    void put(std::uint64_t sequence, std::int64_t due_ns) { v[sequence & (n - 1)].store(due_ns, std::memory_order_relaxed); }
    std::int64_t get(std::uint64_t sequence) const { return v[sequence & (n - 1)].load(std::memory_order_relaxed); }
};

// Creates (`create`) or attaches to the named array; nullptr, with `why`, if not.
inline stamp_array* open_stamps(const std::string& name, bool create, std::string& why) {
    const int fd = ::shm_open(name.c_str(), create ? O_CREAT | O_RDWR : O_RDWR, 0600);
    if (fd < 0) {
        why = name + ": shm_open: " + std::strerror(errno);
        return nullptr;
    }
    if (create && ::ftruncate(fd, sizeof(stamp_array)) != 0) {
        why = name + ": ftruncate: " + std::strerror(errno);
        ::close(fd);
        return nullptr;
    }
    void* m = ::mmap(nullptr, sizeof(stamp_array), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) {
        why = name + ": mmap: " + std::strerror(errno);
        return nullptr;
    }
    return static_cast<stamp_array*>(m);
}

}  // namespace itchfh::bench
