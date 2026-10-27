// Our MoldUDP64 sender (note 007, D6): a recorded day from a session store
// onto a multicast group, packed as note 005's D7 says; at a fixed number of
// packets a second, or at the day's own pace (each packet sent when its first
// message's timestamp comes round), sped up by a factor; a chosen share of
// packets dropped before sending (so recovery runs over a real socket);
// heartbeats while it has nothing to send, then the end of session in their
// place (the specification: "for a short while in place of Heartbeats";
// here until stopped, so a receiver that lost the tail still learns of it); and the re-request server (note 005, D6) answering on unicast from
// the same store, between packets. Busy-polls its CPU.
// Design: docs/design/007-receiver-framer.md
#pragma once

#include <itchfh/mold.hpp>
#include <itchfh/session_store.hpp>
#include <itchfh/udp.hpp>

#include <atomic>
#include <cstdint>
#include <ctime>
#include <random>

namespace itchfh {

struct sender_config {
    double packets_per_s = 0;  // > 0: a fixed pace
    double speed = 0;          // > 0: the day's own pace, this many times faster
    double drop = 0;           // share of packets not sent (their first sending only)
    std::uint64_t seed = 7;
    std::int64_t heartbeat_ns = 1'000'000'000;
    std::uint64_t first = 1;   // the first message sent (note 010: the open, mid-day); the receiver must expect it
    std::uint64_t last = 0;    // > 0: only messages first..last, the session ending there
    // Note 010, D1: told of each packet's messages and its intended send time
    // (monotonic ns), before it is sent; nullptr: nothing.
    void (*stamp)(void* ctx, std::uint64_t first, std::uint64_t count, std::int64_t due_ns) = nullptr;
    void* stamp_ctx = nullptr;
    int end_markers = 5;       // end-of-session packets after the last, a millisecond apart, then one a heartbeat
};

struct sender_counts {
    std::uint64_t packets = 0, dropped = 0, heartbeats = 0, requests = 0, answered = 0, behind = 0;
    std::int64_t first_ns = 0, last_ns = 0;  // the first and last packets' sending (monotonic)
    // Note 010, D2: how late each packet went, against its intended time.
    std::int64_t late_max_ns = 0;
    std::uint64_t late_over_10us = 0, late_over_100us = 0;
};

inline std::int64_t monotonic_ns() {
    timespec t;
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}

class mold_sender {
public:
    mold_sender(const session_store& store, udp::sender& out, const sender_config& c) : store_(store), out_(out), cfg_(c), rng_(c.seed) {}

    // Sends the whole session, then the end, answering re-requests throughout,
    // then keeps sending the end and answering until `stop` (the receiver may
    // still be recovering).
    void run(const std::atomic<bool>& stop) {
        unsigned char pk[mold::header_size + mold::payload];
        const std::int64_t t0 = monotonic_ns();
        std::int64_t last_sent = t0;
        std::uint64_t first = cfg_.first > 0 ? cfg_.first : 1, sent = 0;
        const std::uint64_t day_start = timestamp_of(first);
        const std::uint64_t end = cfg_.last > 0 && cfg_.last < store_.count() ? cfg_.last : store_.count();
        while (first <= end && !stop.load(std::memory_order_relaxed)) {
            const std::int64_t due = cfg_.packets_per_s > 0
                                         ? t0 + static_cast<std::int64_t>(static_cast<double>(sent) * 1e9 / cfg_.packets_per_s)
                                         : t0 + static_cast<std::int64_t>(static_cast<double>(timestamp_of(first) - day_start) / cfg_.speed);
            std::int64_t now = monotonic_ns();
            if (now - due > 1'000'000) ++counts_.behind;  // a millisecond late: counted, not skipped
            while (now < due) {
                serve();
                if (now - last_sent >= cfg_.heartbeat_ns) {
                    mold::build_marker(pk, store_.session(), first, mold::heartbeat);
                    out_.send(pk, mold::header_size);
                    ++counts_.heartbeats;
                    last_sent = now;
                }
                now = monotonic_ns();
            }
            const std::size_t n = store_.fill(first, end - first + 1, pk);
            const std::uint64_t count = mold::read_header(pk).count;
            if (cfg_.stamp) cfg_.stamp(cfg_.stamp_ctx, first, count, due);
            if (const std::int64_t late = monotonic_ns() - due; late > 0) {
                if (late > counts_.late_max_ns) counts_.late_max_ns = late;
                counts_.late_over_10us += late > 10'000;
                counts_.late_over_100us += late > 100'000;
            }
            if (cfg_.drop > 0 && std::uniform_real_distribution<double>(0, 1)(rng_) < cfg_.drop) ++counts_.dropped;
            else out_.send(pk, n);
            ++counts_.packets;
            ++sent;
            counts_.last_ns = monotonic_ns();
            if (counts_.first_ns == 0) counts_.first_ns = counts_.last_ns;
            first += count;
            last_sent = counts_.last_ns;
            serve();
        }
        for (int k = 0; k < cfg_.end_markers && !stop.load(std::memory_order_relaxed); ++k) {
            mold::build_marker(pk, store_.session(), end + 1, mold::end_of_session);
            out_.send(pk, mold::header_size);
            const std::int64_t until = monotonic_ns() + 1'000'000;
            while (monotonic_ns() < until) serve();
        }
        std::int64_t last_end = monotonic_ns();
        while (!stop.load(std::memory_order_relaxed)) {
            serve();
            if (const std::int64_t now = monotonic_ns(); now - last_end >= cfg_.heartbeat_ns) {
                mold::build_marker(pk, store_.session(), end + 1, mold::end_of_session);
                out_.send(pk, mold::header_size);
                last_end = now;
            }
        }
    }
    const sender_counts& counts() const { return counts_; }

private:
    std::uint64_t timestamp_of(std::uint64_t s) const {
        std::size_t len;
        const unsigned char* m = store_.message(s, len);
        return len >= 11 ? mold::detail::be(m + 5, 6) : 0;
    }
    void serve() {
        unsigned char req[64], pk[mold::header_size + mold::payload];
        std::size_t n;
        sockaddr_in from;
        while (out_.request(req, sizeof(req), n, from)) {
            ++counts_.requests;
            if (const std::size_t a = store_.answer(req, n, pk)) {
                out_.answer(pk, a, from);
                ++counts_.answered;
            }
        }
    }

    const session_store& store_;
    udp::sender& out_;
    sender_config cfg_;
    std::mt19937_64 rng_;
    sender_counts counts_;
};

}  // namespace itchfh
