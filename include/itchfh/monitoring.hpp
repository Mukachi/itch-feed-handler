// Monitoring (note 009, D6): every component's counters and the feed's
// state, copied by the feed loop every 100 ms into a block of shared memory
// under note 008's seqlock (release stores and acquire loads, word by word),
// with no allocation or system call; read by itchfh-monitor, which judges
// health from them.
// Design: docs/design/009-recording-monitoring.md
#pragma once

#include <itchfh/book_types.hpp>
#include <itchfh/sequencer.hpp>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace itchfh::mon {

enum counter : std::uint32_t {
    feed_state_now,     // the sequencer's state (feed_state)
    since_datagram_ms,  // since the last datagram from either socket
    datagrams, answers, messages, undecoded, kernel_drops, answer_drops,
    packets, malformed, other_session, gaps, requests, retried, hold_full, held_now, duplicate_messages,
    book_bad,           // events refused by the book (note 002's D5), every kind
    consumers_live, publisher_overruns, consumers_remote,
    recorded, unrecorded, recorder_backlog, recorded_bytes,
    snapshots, snapshot_failures, snapshot_waits, last_recovery_ns, recovering_now,  // note 011, D7
    refreshes,
    n_counters
};
inline constexpr const char* names[n_counters] = {
    "feed_state", "since_datagram_ms", "datagrams", "answers", "messages", "undecoded", "kernel_drops", "answer_drops",
    "packets", "malformed", "other_session", "gaps", "requests", "retried", "hold_full", "held_now", "duplicate_messages",
    "book_bad", "consumers_live", "publisher_overruns", "consumers_remote", "recorded", "unrecorded", "recorder_backlog", "recorded_bytes",
    "snapshots", "snapshot_failures", "snapshot_waits", "last_recovery_ns", "recovering", "refreshes"};

using values = std::uint64_t[n_counters];

// Health: what is wrong now. Kernel drops and refused events are shown but
// do not by themselves make the feed unhealthy (they are recovered, or marked).
enum health_bit : std::uint32_t {
    feed_stale = 1, feed_lost = 2, feed_gapped = 4, silent = 8, recorder_unrecorded = 16, publisher_overrun = 32,
    consumer_remote = 64,  // a consumer on another NUMA node than the publisher's (note 008, Review)
    recovering = 128       // a snapshot being applied (note 011, D7)
};
inline std::uint32_t health(const values& v) {
    const auto s = static_cast<feed_state>(v[feed_state_now]);
    std::uint32_t h = 0;
    if (s == feed_state::stale) h |= feed_stale;
    if (s == feed_state::lost) h |= feed_lost;
    if (s == feed_state::gapped) h |= feed_gapped;
    if (s != feed_state::ended && v[datagrams] > 0 && v[since_datagram_ms] > 2000) h |= silent;
    if (v[unrecorded] > 0) h |= recorder_unrecorded;
    if (v[publisher_overruns] > 0) h |= publisher_overrun;
    if (v[consumers_remote] > 0) h |= consumer_remote;
    if (v[recovering_now] > 0) h |= recovering;
    return h;
}
// Unhealthy: stale, lost, silent, a datagram unrecorded, a consumer overrun or
// one on another node (gapped alone is recovery under way).
inline bool unhealthy(std::uint32_t h) { return (h & ~static_cast<std::uint32_t>(feed_gapped)) != 0; }

inline constexpr std::uint64_t block_magic = 0x6974636866686D6EULL;  // "itchfhmn"
inline constexpr std::uint32_t block_version = 3;  // 2: consumers_remote; 3: snapshots (note 011)

struct alignas(64) block_layout {
    std::uint64_t magic;
    std::uint32_t version, counters;
    std::atomic<std::uint32_t> ready;
    std::int32_t pid;
    alignas(64) std::atomic<std::uint64_t> seq{0};
    std::atomic<std::uint64_t> v[n_counters]{};
};

// The block's segment: created by the program, attached by readers.
class block {
public:
    block() = default;
    block(const block&) = delete;
    block& operator=(const block&) = delete;
    ~block() {
        if (b_) ::munmap(b_, sizeof(block_layout));
        if (creator_) ::shm_unlink(name_.c_str());
    }
    // Replaces a block whose creator has gone; refuses a live one.
    bool create(const std::string& name, std::string& why) {
        name_ = name;
        int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0 && errno == EEXIST) {
            block old;
            if (old.attach(name, why) && old.b_->pid > 0 && (::kill(old.b_->pid, 0) == 0 || errno == EPERM))
                return fail(why, name + ": in use by a running program");
            ::shm_unlink(name.c_str());
            fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        }
        if (fd < 0) return fail(why, name + ": shm_open: " + std::strerror(errno));
        if (::ftruncate(fd, sizeof(block_layout)) != 0) {
            ::close(fd);
            return fail(why, name + ": ftruncate: " + std::strerror(errno));
        }
        void* m = ::mmap(nullptr, sizeof(block_layout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (m == MAP_FAILED) return fail(why, name + ": mmap: " + std::strerror(errno));
        creator_ = true;
        b_ = new (m) block_layout{};
        b_->magic = block_magic;
        b_->version = block_version;
        b_->counters = n_counters;
        b_->pid = static_cast<std::int32_t>(::getpid());
        b_->ready.store(1, std::memory_order_release);
        return true;
    }
    bool attach(const std::string& name, std::string& why) {
        name_ = name;
        const int fd = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0) return fail(why, name + ": shm_open: " + std::strerror(errno));
        struct stat st {};
        if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) != sizeof(block_layout)) {
            ::close(fd);
            return fail(why, name + ": not a monitoring block of this layout");
        }
        void* m = ::mmap(nullptr, sizeof(block_layout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (m == MAP_FAILED) return fail(why, name + ": mmap: " + std::strerror(errno));
        b_ = std::launder(static_cast<block_layout*>(m));
        if (b_->ready.load(std::memory_order_acquire) != 1 || b_->magic != block_magic || b_->version != block_version ||
            b_->counters != n_counters)
            return fail(why, name + ": not a ready block of this version");
        return true;
    }
    // The writer's (one: the feed loop). No allocation, no system call.
    void write(const values& v) {
        const std::uint64_t s = b_->seq.load(std::memory_order_relaxed);
        b_->seq.store(s + 1, std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < n_counters; ++i) b_->v[i].store(v[i], std::memory_order_release);
        b_->seq.store(s + 2, std::memory_order_release);
    }
    // A reader's: retries until it has a consistent copy.
    void read(values& v) const {
        for (;;) {
            const std::uint64_t s1 = b_->seq.load(std::memory_order_acquire);
            if (s1 & 1) continue;
            for (std::uint32_t i = 0; i < n_counters; ++i) v[i] = b_->v[i].load(std::memory_order_acquire);
            if (b_->seq.load(std::memory_order_relaxed) == s1) return;
        }
    }
    std::int32_t pid() const { return b_->pid; }

private:
    static bool fail(std::string& why, std::string w) {
        why = std::move(w);
        return false;
    }
    std::string name_;
    block_layout* b_ = nullptr;
    bool creator_ = false;
};

// The counters of a feed (note 007's feed_one), its sequencer and book, and
// optionally a publisher (note 008) and a recorder, into `v`.
template <class Feed, class Book, class Publisher, class Recorder>
void gather(values& v, Feed& f, const Book& book, const Publisher* pub, const Recorder* rec, std::int64_t now) {
    const auto& fc = f.counts();
    const auto& sc = f.seq().counts();
    v[feed_state_now] = static_cast<std::uint64_t>(f.seq().state());
    v[since_datagram_ms] = fc.last_ns > 0 ? static_cast<std::uint64_t>((now - fc.last_ns) / 1'000'000) : 0;
    v[datagrams] = fc.datagrams;
    v[answers] = fc.answers;
    v[messages] = fc.messages;
    v[undecoded] = fc.undecoded;
    v[kernel_drops] = fc.kernel_drops;
    v[answer_drops] = fc.answer_drops;
    v[packets] = sc.packets;
    v[malformed] = sc.malformed;
    v[other_session] = sc.other_session;
    v[gaps] = sc.gaps;
    v[requests] = sc.requests;
    v[retried] = sc.retried;
    v[hold_full] = sc.hold_full;
    v[held_now] = f.seq().held_packets();
    v[duplicate_messages] = sc.duplicate_messages;
    std::uint64_t bad = 0;
    for (int k = 1; k < bad_event_kinds; ++k) bad += book.bad_count(static_cast<bad_event>(k));
    v[book_bad] = bad;
    if (pub) {
        std::uint64_t live = 0;
        for (int c = 0; c < 8; ++c) live += pub->slot(c) == 4;  // pub::slot_live
        v[consumers_live] = live;
        v[publisher_overruns] = pub->overruns();
        v[consumers_remote] = pub->remote_consumers();
    }
    if (rec) {
        v[recorded] = rec->written();
        v[unrecorded] = rec->unrecorded();
        v[recorder_backlog] = rec->backlog();
        v[recorded_bytes] = rec->bytes_written();
    }
    ++v[refreshes];
}

// A recovery's counters (note 011), into `v`; after gather().
template <class Recovery>
void gather_recovery(values& v, const Recovery& r) {
    const auto& c = r.counts();
    v[snapshots] = c.snapshots;
    v[snapshot_failures] = c.failures;
    v[snapshot_waits] = c.waits;
    v[last_recovery_ns] = static_cast<std::uint64_t>(c.last_ns);
    v[recovering_now] = r.recovering();
}

}  // namespace itchfh::mon
