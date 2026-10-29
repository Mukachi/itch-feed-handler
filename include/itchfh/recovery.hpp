// Recovery from a snapshot in the program (note 011), on its one thread:
//   published_on  a book's handler sinking into a publisher it shares with
//                 the other book: what feed_one runs (note 008's published,
//                 with the publisher outside it)
//   recovery      D1: when the sequencer is `lost` (or at start, joining
//                 late), a snapshot asked of the GLIMPSE service; D3: applied
//                 into a standby book and handler built ahead, which are then
//                 swapped in (the feed's handler, the publisher's book and
//                 status), and the old pair rebuilt fresh on a thread of its
//                 own; D4: the spin read and applied a slice a call, the loop
//                 reading its sockets between calls (drain); D7: a spin that
//                 fails (no connection, refused, cut short, malformed, or no
//                 progress for the timeout) leaves the live pair untouched,
//                 the feed `lost`, and is tried again after `retry_ns`.
// While a spin is applied the publisher is muted: nothing to the rings, the
// table untouched; restored() then rebuilds the table and replays every
// consumer (note 008's D5).
// Design: docs/design/011-snapshot-recovery.md
#pragma once

#include <itchfh/book.hpp>
#include <itchfh/book_config.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/mold_sender.hpp>  // monotonic_ns
#include <itchfh/publisher.hpp>
#include <itchfh/snapshot.hpp>
#include <itchfh/snapshot_link.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <pthread.h>
#include <sched.h>

namespace itchfh {

template <class Book = default_book>
struct published_on {
    publisher<Book>& pub;
    handler<Book, publisher<Book>> h;
    published_on(Book& b, publisher<Book>& p, const handler_config& c = {}) : pub(p), h(b, p, c) {}
    void on(std::uint64_t sequence, const message& m) {
        pub.begin(sequence, m.timestamp_ns);
        h.on(m);
    }
    void feed(feed_state s, std::uint64_t next) { pub.feed(s, next); }
    void tick(std::int64_t now) { pub.tick(now); }
};

struct recovery_config {
    std::string ip = "127.0.0.1";
    std::uint16_t port = 26403;
    std::size_t slice = 64u << 10;                // D4
    std::int64_t retry_ns = 1'000'000'000;        // D7
    std::int64_t timeout_ns = 30'000'000'000;     // D7: no progress for this long is a failure
    int rebuild_cpu = -1;                         // D3: the standby's rebuild (a housekeeping CPU on the program's node)
    bool join = false;                            // D1: start from a snapshot
};

struct recovery_counts {
    std::uint64_t snapshots = 0, failures = 0, waits = 0;  // waits: a loss found the standby not yet rebuilt
    // The last recovery (D9): from `lost` to `live` with the hold drained, in
    // three parts: asking until the spin's first byte (the server's build and
    // the login), the spin read and applied, and the held feed delivered.
    std::int64_t last_ns = 0, last_wait_ns = 0, last_apply_ns = 0, last_backlog_ns = 0;
    std::uint64_t last_bytes = 0, last_held = 0, last_messages = 0;
};

template <class Book = default_book>
class recovery {
public:
    using pair = published_on<Book>;
    using handler_type = handler<Book, publisher<Book>>;

    recovery(publisher<Book>& pub, const book_config& bc, const handler_config& hc, const recovery_config& c)
        : pub_(pub), bc_(bc), hc_(hc), cfg_(c), buf_(std::make_unique<unsigned char[]>(c.slice)) {
        for (slot& s : slots_) build(s);
        client_.emplace(slots_[1].p->h);
        pub_.rebind(*slots_[0].book);
        pub_.watch(slots_[0].p->h);
        worker_ = std::thread([this] { rebuild_loop(); });
    }
    recovery(const recovery&) = delete;
    recovery& operator=(const recovery&) = delete;
    ~recovery() {
        stop_.store(true, std::memory_order_release);
        worker_.join();
    }

    pair& active() { return *slots_[active_].p; }
    const Book& book() const { return *slots_[active_].book; }
    const handler_type& active_handler() const { return slots_[active_].p->h; }

    // Before the first pass: with `join`, the feed starts lost, held, and is joined from a snapshot (D1).
    template <class Feed>
    void begin(Feed& f) {
        if (cfg_.join) f.seq().lose();
    }

    // Every pass of the loop. True while a spin is under way: the loop then
    // reads its sockets (drain) before the next call.
    template <class Feed>
    bool step(Feed& f, std::int64_t now) {
        switch (phase_) {
            case phase::idle: {
                if (f.seq().state() != feed_state::lost) return false;
                if (lost_at_ == 0) lost_at_ = now;
                if (now < next_try_) return false;
                if (standby_.load(std::memory_order_acquire) != ready) {
                    if (!waiting_) ++counts_.waits;
                    waiting_ = true;
                    return false;
                }
                waiting_ = false;
                client_->restart(slots_[1 - active_].p->h);
                pub_.mute(true);
                asked_at_ = progress_at_ = now;
                first_byte_at_ = 0;
                bytes_ = 0;
                if (!conn_.start(cfg_.ip, cfg_.port)) return fail(now);
                phase_ = phase::connecting;
                return true;
            }
            case phase::connecting: {
                const auto s = conn_.progress();
                if (s == snapshot_connection::state::failed || now - asked_at_ > cfg_.timeout_ns) return fail(now);
                if (s == snapshot_connection::state::open) phase_ = phase::reading;
                return true;
            }
            case phase::reading: return slice(f, now);
        }
        return false;
    }
    // A slice of the spin (D4): at most `slice` bytes read and applied. Public
    // for the hot-path check (S6); step() is what the loop calls.
    template <class Feed>
    bool slice(Feed& f, std::int64_t now) {
        const long n = conn_.read(buf_.get(), cfg_.slice);
        if (n > 0) {
            if (first_byte_at_ == 0) first_byte_at_ = now;
            bytes_ += static_cast<std::uint64_t>(n);
            progress_at_ = now;
            client_->feed(buf_.get(), static_cast<std::size_t>(n));
        }
        if (client_->rejected() || client_->malformed()) return fail(now);
        if (client_->resume()) return finish(f, now);
        if (n < 0 || now - progress_at_ > cfg_.timeout_ns) return fail(now);
        return true;
    }
    // The sockets read until empty, between slices (D4).
    template <class Feed>
    static void drain(Feed& f) {
        for (int k = 0; k < 100'000; ++k) {
            const std::uint64_t empty = f.counts().empty_calls;
            f.poll();
            if (f.counts().empty_calls != empty) return;
        }
    }

    bool recovering() const { return phase_ != phase::idle; }
    bool standby_ready() const { return standby_.load(std::memory_order_acquire) == ready; }
    const recovery_counts& counts() const { return counts_; }

private:
    enum class phase { idle, connecting, reading };
    static constexpr int ready = 0, dirty = 1;
    struct slot {
        std::unique_ptr<Book> book;
        std::unique_ptr<pair> p;
    };

    void build(slot& s) {
        s.p.reset();
        s.book.reset();
        s.book = std::make_unique<Book>(bc_);
        s.p = std::make_unique<pair>(*s.book, pub_, hc_);
    }
    template <class Feed>
    bool finish(Feed& f, std::int64_t now) {
        const std::uint64_t next = *client_->resume();
        conn_.close();
        const int old = active_;
        active_ = 1 - active_;
        f.replace_handler(*slots_[active_].p);
        pub_.rebind(*slots_[active_].book);
        pub_.watch(slots_[active_].p->h);
        pub_.restored();
        counts_.last_held = f.seq().held_packets();
        f.seq().resume(next, now);
        const std::int64_t done = monotonic_ns();
        ++counts_.snapshots;
        counts_.last_ns = done - lost_at_;
        counts_.last_wait_ns = (first_byte_at_ ? first_byte_at_ : now) - asked_at_;
        counts_.last_apply_ns = now - (first_byte_at_ ? first_byte_at_ : now);
        counts_.last_backlog_ns = done - now;
        counts_.last_bytes = bytes_;
        counts_.last_messages = client_->messages();
        lost_at_ = 0;
        phase_ = phase::idle;
        rebuild(old);
        return false;
    }
    bool fail(std::int64_t now) {
        conn_.close();
        pub_.mute(false);
        ++counts_.failures;
        next_try_ = now + cfg_.retry_ns;
        phase_ = phase::idle;
        if (client_->messages() > 0 || client_->resume()) rebuild(1 - active_);  // part of a spin went into the standby
        return false;
    }
    void rebuild(int which) {
        rebuild_which_.store(which, std::memory_order_relaxed);
        standby_.store(dirty, std::memory_order_release);
    }
    void rebuild_loop() {
        if (cfg_.rebuild_cpu >= 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cfg_.rebuild_cpu, &set);
            ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
        }
        while (!stop_.load(std::memory_order_acquire)) {
            if (standby_.load(std::memory_order_acquire) != dirty) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            build(slots_[rebuild_which_.load(std::memory_order_relaxed)]);
            standby_.store(ready, std::memory_order_release);
        }
    }

    publisher<Book>& pub_;
    book_config bc_;
    handler_config hc_;
    recovery_config cfg_;
    slot slots_[2];
    int active_ = 0;
    std::optional<snapshot_client<handler_type>> client_;
    snapshot_connection conn_;
    std::unique_ptr<unsigned char[]> buf_;
    phase phase_ = phase::idle;
    std::int64_t lost_at_ = 0, next_try_ = 0, asked_at_ = 0, progress_at_ = 0, first_byte_at_ = 0;
    std::uint64_t bytes_ = 0;
    bool waiting_ = false;
    recovery_counts counts_;
    std::atomic<int> standby_{ready}, rebuild_which_{1};
    std::atomic<bool> stop_{false};
    std::thread worker_;
};

}  // namespace itchfh
