// A file of events (note 002, D7): a 64-byte header, then the events as their
// 40-byte records, in feed order. Written by tools/extract_events from a
// recorded day, read here by mapping the file (tests and benchmarks; never the
// hot path). Little-endian, as written on this machine.
// Design: docs/design/002-order-book.md
#pragma once

#include <itchfh/event.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace itchfh {

struct event_file_header {
    char magic[8];             // "ITCHFHEV"
    std::uint32_t version;     // 1
    std::uint32_t record_size; // sizeof(event)
    std::uint64_t count;       // events that follow
    char source[40];           // the recorded day's file name, for the run's record
};
static_assert(sizeof(event_file_header) == 64);

inline constexpr char event_file_magic[8] = {'I', 'T', 'C', 'H', 'F', 'H', 'E', 'V'};

inline event_file_header make_event_file_header(std::uint64_t count, const std::string& source) {
    event_file_header h{};
    std::memcpy(h.magic, event_file_magic, sizeof(h.magic));
    h.version = 1;
    h.record_size = sizeof(event);
    h.count = count;
    std::strncpy(h.source, source.c_str(), sizeof(h.source) - 1);
    return h;
}

// The events of a file, mapped read-only; empty if the file is missing or not one.
class event_file {
public:
    explicit event_file(const std::string& path) {
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        struct stat st {};
        if (::fstat(fd, &st) == 0 && static_cast<std::size_t>(st.st_size) >= sizeof(event_file_header)) {
            void* p = ::mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
            if (p != MAP_FAILED) {
                base_ = p;
                bytes_ = static_cast<std::size_t>(st.st_size);
            }
        }
        ::close(fd);
        if (!base_) return;
        std::memcpy(&header_, base_, sizeof(header_));
        const bool valid = std::memcmp(header_.magic, event_file_magic, sizeof(header_.magic)) == 0 && header_.version == 1 &&
                           header_.record_size == sizeof(event) &&
                           bytes_ == sizeof(event_file_header) + header_.count * sizeof(event);
        if (!valid) {
            ::munmap(base_, bytes_);
            base_ = nullptr;
        }
    }
    ~event_file() {
        if (base_) ::munmap(base_, bytes_);
    }
    event_file(const event_file&) = delete;
    event_file& operator=(const event_file&) = delete;

    bool ok() const noexcept { return base_ != nullptr; }
    const event_file_header& header() const noexcept { return header_; }
    std::span<const event> events() const noexcept {
        if (!base_) return {};
        return {reinterpret_cast<const event*>(static_cast<const char*>(base_) + sizeof(event_file_header)),
                static_cast<std::size_t>(header_.count)};
    }

private:
    void* base_ = nullptr;
    std::size_t bytes_ = 0;
    event_file_header header_{};
};

}  // namespace itchfh
