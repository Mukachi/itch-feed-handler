// The data recorder (note 009): every datagram as the receive thread read it
// (D1), copied into lockfree-ipc's SPSC ring and written to a file by a
// thread of its own (D2: the receive thread never waits; a full ring leaves
// the datagram unrecorded, counted, and the next record says how many), in
// 4 MB writes with sync_file_range so dirty pages never wait for the
// kernel's writeback (D3), fdatasync at the end; and the reader, which says
// whether a recording is complete (D4).
//   file:    header, records, trailer
//   record:  length, socket (0 the group, 1 the request socket), flags,
//            datagrams skipped before it, the kernel's receive stamp, CRC-32
//            of all of it with the bytes, then the bytes
// Design: docs/design/009-recording-monitoring.md
#pragma once

#include <itchfh/udp.hpp>

#include <lfipc/spsc.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>

namespace itchfh::rec {

inline constexpr std::uint64_t file_magic = 0x6974636866687263ULL;  // "itchfhrc"
inline constexpr std::uint32_t file_version = 1;
inline constexpr std::uint8_t trailer_socket = 0xFF;
inline constexpr std::uint8_t after_skip = 0x01;  // flags: datagrams were left unrecorded before this one

struct file_header {
    std::uint64_t magic;
    std::uint32_t version, header_size;
    std::uint64_t config_digest;
    std::int64_t start_ns;  // CLOCK_REALTIME
    unsigned char reserved[32];
};
static_assert(sizeof(file_header) == 64);

struct record_header {
    std::uint16_t length;
    std::uint8_t socket, flags;
    std::uint32_t skipped;
    std::int64_t kernel_ns;
    std::uint32_t crc, reserved;
};
static_assert(sizeof(record_header) == 24);

struct trailer_body {
    std::uint64_t recorded, unrecorded, kernel_drops, answer_drops;
    std::int64_t end_ns;
};

// CRC-32 (IEEE), by table: the writer's and the reader's, never the receive thread's.
inline std::uint32_t crc32(std::uint32_t crc, const unsigned char* p, std::size_t n) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
// Over the whole header (its CRC field as zero; `reserved` written as zero, so a change there shows too) and the bytes.
inline std::uint32_t record_crc(record_header h, const unsigned char* bytes) {
    h.crc = 0;
    return crc32(crc32(0, reinterpret_cast<const unsigned char*>(&h), sizeof(h)), bytes, h.length);
}

inline std::int64_t realtime_ns() {
    timespec t;
    ::clock_gettime(CLOCK_REALTIME, &t);
    return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}

// A datagram on its way to the writer: one ring slot.
struct alignas(64) slot {
    std::int64_t kernel_ns;
    std::uint16_t length;
    std::uint8_t socket, pad;
    std::uint32_t skipped;
    unsigned char bytes[1584];
};
static_assert(sizeof(slot) == 1600 && sizeof(slot::bytes) >= udp::datagram_room);

class recorder {
public:
    static constexpr std::size_t ring_slots = 16'384;  // D2: ~0.5 s at the day's busiest millisecond
    static constexpr std::size_t buffer_bytes = 4u << 20;  // D3
    using ring = lfipc::spsc_ring<slot, ring_slots>;

    recorder() = default;
    recorder(const recorder&) = delete;
    recorder& operator=(const recorder&) = delete;
    ~recorder() { close(0, 0); }

    // Creates `path` (never over an existing file), writes the header and
    // starts the writer on `writer_cpu` (a housekeeping CPU; -1 unpinned).
    bool open(const std::string& path, std::uint64_t config_digest, int writer_cpu, std::string& why) {
        fd_ = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0644);
        if (fd_ < 0) return fail(why, path + ": " + std::strerror(errno));
        // The ring in pages touched now, so the receive thread's first lap takes no page faults.
        void* m = ::mmap(nullptr, sizeof(ring), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (m == MAP_FAILED) return fail(why, std::string("mmap: ") + std::strerror(errno));
        ring_ = new (m) ring();
        buf_ = std::make_unique<unsigned char[]>(buffer_bytes + sizeof(record_header) + sizeof(slot::bytes));
        file_header h{};
        h.magic = file_magic;
        h.version = file_version;
        h.header_size = sizeof(h);
        h.config_digest = config_digest;
        h.start_ns = realtime_ns();
        std::memcpy(buf_.get(), &h, sizeof(h));
        used_ = sizeof(h);
        writer_ = std::thread([this, writer_cpu] { run(writer_cpu); });
        return true;
    }

    // The receive thread's: never waits, never allocates, makes no system call.
    void record(const udp::datagram& d, std::uint8_t socket) {
        scratch_.kernel_ns = d.kernel_ns;
        scratch_.length = static_cast<std::uint16_t>(d.length);
        scratch_.socket = socket;
        scratch_.skipped = skipped_;
        std::memcpy(scratch_.bytes, d.bytes, d.length);
        if (ring_->try_push(scratch_)) {
            skipped_ = 0;
            pushed_.store(pushed_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        } else {
            ++skipped_;
            unrecorded_.store(unrecorded_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
        }
    }

    // Stops the writer once the ring is empty, writes the trailer, syncs.
    bool close(std::uint64_t kernel_drops, std::uint64_t answer_drops) {
        if (fd_ < 0) return false;
        stop_.store(true, std::memory_order_release);
        hold_.store(false, std::memory_order_release);
        if (writer_.joinable()) writer_.join();
        trailer_body t{written_.load(), unrecorded_.load(), kernel_drops, answer_drops, realtime_ns()};
        record_header h{};
        h.length = sizeof(t);
        h.socket = trailer_socket;
        h.kernel_ns = t.end_ns;
        h.crc = record_crc(h, reinterpret_cast<const unsigned char*>(&t));
        append(h, reinterpret_cast<const unsigned char*>(&t));
        flush();
        const bool ok = ::fdatasync(fd_) == 0 && !write_failed_;
        ::close(fd_);
        fd_ = -1;
        ring_->~ring();
        ::munmap(ring_, sizeof(ring));
        ring_ = nullptr;
        return ok;
    }

    // Holds the writer back (a test of D2: the ring fills, the feed goes on).
    void hold(bool on) { hold_.store(on, std::memory_order_release); }

    std::uint64_t pushed() const { return pushed_.load(std::memory_order_relaxed); }
    std::uint64_t unrecorded() const { return unrecorded_.load(std::memory_order_relaxed); }
    std::uint64_t written() const { return written_.load(std::memory_order_relaxed); }
    std::uint64_t backlog() const { return pushed() - popped_.load(std::memory_order_relaxed); }
    std::uint64_t bytes_written() const { return file_bytes_.load(std::memory_order_relaxed); }
    bool write_failed() const { return write_failed_; }

private:
    static bool fail(std::string& why, std::string w) {
        why = std::move(w);
        return false;
    }
    void run(int cpu) {
        if (cpu >= 0) {
            cpu_set_t s;
            CPU_ZERO(&s);
            CPU_SET(cpu, &s);
            ::sched_setaffinity(0, sizeof(s), &s);
        }
        auto last_flush = std::chrono::steady_clock::now();
        auto* in = static_cast<slot*>(::operator new(sizeof(slot)));
        for (;;) {
            if (hold_.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                continue;
            }
            if (ring_->try_pop(*in)) {
                popped_.store(popped_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                record_header h{};
                h.length = in->length;
                h.socket = in->socket;
                h.flags = in->skipped ? after_skip : 0;
                h.skipped = in->skipped;
                h.kernel_ns = in->kernel_ns;
                h.crc = record_crc(h, in->bytes);
                append(h, in->bytes);
                written_.store(written_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                if (used_ >= buffer_bytes) {
                    flush();
                    last_flush = std::chrono::steady_clock::now();
                }
                continue;
            }
            if (stop_.load(std::memory_order_acquire)) break;
            // Nothing waiting: what is buffered goes out every 100 ms, so a crash loses little.
            if (used_ > 0 && std::chrono::steady_clock::now() - last_flush > std::chrono::milliseconds(100)) {
                flush();
                last_flush = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        ::operator delete(in);
    }
    void append(const record_header& h, const unsigned char* bytes) {
        std::memcpy(buf_.get() + used_, &h, sizeof(h));
        std::memcpy(buf_.get() + used_ + sizeof(h), bytes, h.length);
        used_ += sizeof(h) + h.length;
    }
    // D3: write, start that range's writeback, and wait for the one before it,
    // so at most two buffers are ever dirty.
    void flush() {
        if (used_ == 0) return;
        std::size_t done = 0;
        while (done < used_) {
            const ssize_t n = ::write(fd_, buf_.get() + done, used_ - done);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                write_failed_ = true;
                break;
            }
            done += static_cast<std::size_t>(n);
        }
        ::sync_file_range(fd_, static_cast<off_t>(offset_), static_cast<off_t>(done), SYNC_FILE_RANGE_WRITE);
        if (prev_len_)
            ::sync_file_range(fd_, static_cast<off_t>(prev_off_), static_cast<off_t>(prev_len_),
                              SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER);
        prev_off_ = offset_;
        prev_len_ = done;
        offset_ += done;
        file_bytes_.store(offset_, std::memory_order_relaxed);
        used_ = 0;
    }

    int fd_ = -1;
    ring* ring_ = nullptr;
    slot scratch_;  // the receive thread's, reused
    std::uint32_t skipped_ = 0;
    std::atomic<std::uint64_t> pushed_{0}, unrecorded_{0}, popped_{0}, written_{0}, file_bytes_{0};
    std::atomic<bool> stop_{false}, hold_{false};
    std::thread writer_;
    std::unique_ptr<unsigned char[]> buf_;
    std::size_t used_ = 0, offset_ = 0, prev_off_ = 0, prev_len_ = 0;
    bool write_failed_ = false;
};

// Reads a recording record by record (D4).
class reader {
public:
    enum class result { record, end, truncated, corrupt };
    struct record {
        record_header h;
        unsigned char bytes[2048];
    };

    reader() = default;
    reader(const reader&) = delete;
    reader& operator=(const reader&) = delete;
    ~reader() {
        if (f_) std::fclose(f_);
    }
    bool open(const std::string& path, std::string& why) {
        f_ = std::fopen(path.c_str(), "rb");
        if (!f_) {
            why = path + ": " + std::strerror(errno);
            return false;
        }
        std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);
        if (std::fread(&header_, sizeof(header_), 1, f_) != 1 || header_.magic != file_magic || header_.version != file_version) {
            why = path + ": not a recording of this version";
            return false;
        }
        return true;
    }
    // The next record; `end` after the trailer, `truncated` if the file stops
    // mid-record or without a trailer, `corrupt` if a record's CRC is wrong.
    result next(record& r) {
        if (done_) return result::end;
        if (std::fread(&r.h, sizeof(r.h), 1, f_) != 1) return result::truncated;
        if (r.h.length > sizeof(r.bytes)) return result::corrupt;
        if (std::fread(r.bytes, 1, r.h.length, f_) != r.h.length) return result::truncated;
        if (record_crc(r.h, r.bytes) != r.h.crc) return result::corrupt;
        if (r.h.socket == trailer_socket) {
            if (r.h.length != sizeof(trailer_)) return result::corrupt;
            std::memcpy(&trailer_, r.bytes, sizeof(trailer_));
            has_trailer_ = done_ = true;
            return result::end;
        }
        ++records_;
        skipped_ += r.h.skipped;
        return result::record;
    }
    const file_header& header() const { return header_; }
    bool has_trailer() const { return has_trailer_; }
    const trailer_body& trailer() const { return trailer_; }
    std::uint64_t records() const { return records_; }
    std::uint64_t skipped() const { return skipped_; }

private:
    std::FILE* f_ = nullptr;
    file_header header_{};
    trailer_body trailer_{};
    bool has_trailer_ = false, done_ = false;
    std::uint64_t records_ = 0, skipped_ = 0;
};

}  // namespace itchfh::rec
