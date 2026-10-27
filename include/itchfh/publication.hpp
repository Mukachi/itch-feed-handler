// What the publisher (note 008) and its consumers share: the update record
// (D2), the reliability byte (D4), and the state table's segment (D6), which
// also holds each consumer's slot (D3: its state and how far it has read).
// The consumers' rings are lockfree-ipc's shm_spsc, one segment each, named
// after the table: "<name>-ring-<i>".
// Design: docs/design/008-publication.md
#pragma once

#include <itchfh/book_types.hpp>
#include <itchfh/handler_types.hpp>
#include <itchfh/messages.hpp>
#include <itchfh/sequencer.hpp>  // feed_state

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <type_traits>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace itchfh::pub {

// `instrument`: its reliability changed with no level changed (an event refused, note 002's D5).
enum class kind : std::uint8_t { level = 1, trade, trade_broken, status, imbalance, market, directory, feed, reset, instrument };

// D4: the feed's state in the low three bits, the instrument's in two flags.
inline constexpr std::uint8_t feed_mask = 0x07, book_unsound = 0x10, status_unrestored = 0x20;
inline std::uint8_t reliability(feed_state f, bool unsound, bool unrestored) {
    return static_cast<std::uint8_t>(static_cast<std::uint8_t>(f) | (unsound ? book_unsound : 0) | (unrestored ? status_unrestored : 0));
}
inline feed_state feed_of(std::uint8_t r) { return static_cast<feed_state>(r & feed_mask); }
// Current: the feed live and the instrument sound.
inline bool current(std::uint8_t r) { return feed_of(r) == feed_state::live && (r & (book_unsound | status_unrestored)) == 0; }
// Catching up: delivered from the sequencer's backlog, the gap just filled.
inline bool catching_up(std::uint8_t r) { return feed_of(r) == feed_state::gapped; }

inline constexpr std::uint8_t continues = 0x01;  // flags: the same ITCH message as the update before it
inline constexpr std::uint8_t all_instruments = 0x02;  // flags, on a reset: every instrument

struct level_body {
    std::uint32_t price, orders;  // the level after the change (0 orders: gone)
    std::uint64_t shares;
    std::uint32_t bid_price, ask_price;  // the instrument's best after it (0 shares: none)
    std::uint64_t bid_shares, ask_shares;
};
struct trade_body {
    std::uint32_t price;
    trade_kind tkind;
    std::uint64_t shares, match;
};
struct imbalance_body {  // the decoded Net Order Imbalance Indicator, packed
    std::uint64_t paired_shares, imbalance_shares;
    std::uint32_t far_price, near_price, reference_price;
    char direction, cross_type, price_variation;
};
struct feed_body {
    feed_state state;
    std::uint64_t next_expected;
};

// D2: one cache line.
struct alignas(64) update {
    kind k;
    std::uint8_t reliability;
    std::uint16_t locate;
    std::uint8_t flags;
    side s;               // a level's
    status_field field;   // a status update's
    std::uint8_t pad_;
    std::uint64_t sequence;      // of the ITCH message that caused it
    std::uint64_t timestamp_ns;  // its timestamp, nanoseconds since midnight
    union {
        level_body level;
        trade_body trade;
        imbalance_body noii;
        feed_body feed;
        // status: the field's value as the handler now holds it (trading
        // state and reason, Reg SHO, halts, LULD collar, IPO quoting, retail
        // interest, DLCR; nothing for a participant position); market (S, V,
        // W) and directory (R): the decoded message body.
        unsigned char raw[40];
    };
};
static_assert(sizeof(update) == 64 && std::is_trivially_copyable_v<update>);
static_assert(sizeof(stock_directory) <= 40 && sizeof(direct_listing) <= 40 && sizeof(luld_collar) <= 40 && sizeof(mwcb_decline) <= 40);

inline constexpr std::size_t ring_capacity = 1u << 16;  // updates (4 MB) a consumer
inline constexpr std::uint64_t ring_type_id = 0x6974636866687075ULL;  // "itchfhpu"
inline constexpr int max_consumers = 8;

// D3: a consumer's slot. The consumer claims it, drains its ring and asks to
// join; the publisher replays the books to it (D5), then sends it everything.
enum slot_state : std::uint32_t { slot_free = 0, slot_claimed, slot_join, slot_replaying, slot_live, slot_overrun };
struct alignas(128) consumer_slot {
    std::atomic<std::uint32_t> state{slot_free};
    std::atomic<std::int32_t> pid{0};
    alignas(64) std::atomic<std::uint64_t> consumed{0};  // updates read, written by the consumer
};

// D6: an instrument's latest state under a seqlock, one cache line. Words:
// 0 bid price | bid orders << 32, 1 bid shares, 2 ask price | ask orders << 32,
// 3 ask shares, 4 last trade price | trading state << 32 | reliability << 40,
// 5 last trade shares, 6 the sequence number of the last change.
struct alignas(64) table_entry {
    std::atomic<std::uint64_t> seq{0};
    std::atomic<std::uint64_t> w[7]{};
};
struct entry_view {
    std::uint32_t bid_price = 0, bid_orders = 0, ask_price = 0, ask_orders = 0, last_price = 0;
    std::uint64_t bid_shares = 0, ask_shares = 0, last_shares = 0, sequence = 0;
    char trading_state = 0;
    std::uint8_t reliability = 0;
    bool operator==(const entry_view&) const = default;
};

inline constexpr std::uint64_t table_magic = 0x6974636866687462ULL;  // "itchfhtb"
inline constexpr std::uint32_t table_version = 1;
inline constexpr std::size_t instruments = 65'536;

struct alignas(128) table_header {
    std::uint64_t magic;
    std::uint32_t version;
    std::atomic<std::uint32_t> ready;
    std::int32_t creator_pid;
    std::uint32_t consumers;
    std::uint64_t ring_capacity;
    std::atomic<std::uint64_t> feed;  // feed_state | next expected << 8
};
struct table_layout {
    table_header header;
    consumer_slot slots[max_consumers];
    table_entry entries[instruments];
};
static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

// The table's segment: created by the publisher, attached by consumers.
class table_segment {
public:
    table_segment() = default;
    table_segment(const table_segment&) = delete;
    table_segment& operator=(const table_segment&) = delete;
    ~table_segment() {
        if (base_) ::munmap(base_, sizeof(table_layout));
        if (creator_) ::shm_unlink(name_.c_str());
    }
    // Replaces a segment of that name whose creator has gone; refuses a live one.
    bool create(const std::string& name, std::string& why) {
        name_ = name;
        int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0 && errno == EEXIST) {
            if (attach(name, why) && alive(layout()->header.creator_pid)) return fail(why, name + ": in use by a running publisher");
            if (base_) ::munmap(base_, sizeof(table_layout));
            base_ = nullptr;
            ::shm_unlink(name.c_str());
            fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
        }
        if (fd < 0) return fail(why, name + ": shm_open: " + std::strerror(errno));
        if (::ftruncate(fd, sizeof(table_layout)) != 0) {
            ::close(fd);
            return fail(why, name + ": ftruncate: " + std::strerror(errno));
        }
        base_ = ::mmap(nullptr, sizeof(table_layout), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
        ::close(fd);
        if (base_ == MAP_FAILED) {
            base_ = nullptr;
            return fail(why, name + ": mmap: " + std::strerror(errno));
        }
        creator_ = true;
        auto* t = new (base_) table_layout{};
        t->header.magic = table_magic;
        t->header.version = table_version;
        t->header.creator_pid = static_cast<std::int32_t>(::getpid());
        t->header.consumers = max_consumers;
        t->header.ring_capacity = ring_capacity;
        t->header.ready.store(1, std::memory_order_release);
        return true;
    }
    bool attach(const std::string& name, std::string& why) {
        name_ = name;
        const int fd = ::shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0) return fail(why, name + ": shm_open: " + std::strerror(errno));
        struct stat st {};
        if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) != sizeof(table_layout)) {
            ::close(fd);
            return fail(why, name + ": not a table of this layout");
        }
        base_ = ::mmap(nullptr, sizeof(table_layout), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (base_ == MAP_FAILED) {
            base_ = nullptr;
            return fail(why, name + ": mmap: " + std::strerror(errno));
        }
        const table_header& h = layout()->header;
        if (h.ready.load(std::memory_order_acquire) != 1 || h.magic != table_magic || h.version != table_version ||
            h.consumers != max_consumers || h.ring_capacity != ring_capacity)
            return fail(why, name + ": not a ready table of this version and layout");
        return true;
    }
    table_layout* layout() const { return std::launder(static_cast<table_layout*>(base_)); }
    static bool alive(std::int32_t pid) { return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM); }

private:
    static bool fail(std::string& why, std::string w) {
        why = std::move(w);
        return false;
    }
    std::string name_;
    void* base_ = nullptr;
    bool creator_ = false;
};

inline std::string ring_name(const std::string& table, int i) { return table + "-ring-" + std::to_string(i); }

// D6's seqlock, without standalone fences (which ThreadSanitizer does not
// model): the writer marks the entry odd, stores each word with release (so
// the mark is seen first) and marks it even with release; the reader loads the
// counter with acquire, each word with acquire (so its second load of the
// counter cannot move before them) and the counter again. A word from a write
// begun after the first load carries the odd mark with it, so the second load
// differs (H. Boehm, "Can seqlocks get along with programming language memory
// models?", 2012). On x86 every one of these is a plain move.
inline void write_entry(table_entry& e, const std::uint64_t (&v)[7]) {
    const std::uint64_t s = e.seq.load(std::memory_order_relaxed);
    e.seq.store(s + 1, std::memory_order_relaxed);
    for (int i = 0; i < 7; ++i) e.w[i].store(v[i], std::memory_order_release);
    e.seq.store(s + 2, std::memory_order_release);
}
// The reader's: false if a write was in progress or happened meanwhile.
inline bool try_read_entry(const table_entry& e, std::uint64_t (&v)[7]) {
    const std::uint64_t s1 = e.seq.load(std::memory_order_acquire);
    if (s1 & 1) return false;
    for (int i = 0; i < 7; ++i) v[i] = e.w[i].load(std::memory_order_acquire);
    return e.seq.load(std::memory_order_relaxed) == s1;
}
inline entry_view unpack(const std::uint64_t (&v)[7]) {
    entry_view x;
    x.bid_price = static_cast<std::uint32_t>(v[0]);
    x.bid_orders = static_cast<std::uint32_t>(v[0] >> 32);
    x.bid_shares = v[1];
    x.ask_price = static_cast<std::uint32_t>(v[2]);
    x.ask_orders = static_cast<std::uint32_t>(v[2] >> 32);
    x.ask_shares = v[3];
    x.last_price = static_cast<std::uint32_t>(v[4]);
    x.trading_state = static_cast<char>(v[4] >> 32);
    x.reliability = static_cast<std::uint8_t>(v[4] >> 40);
    x.last_shares = v[5];
    x.sequence = v[6];
    return x;
}

}  // namespace itchfh::pub
