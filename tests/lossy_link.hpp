// The lossy link of note 005 (D8), for tests: a server sending a session's
// packets in order (D7's packets from a session_store), with heartbeats while
// it has nothing to send and the end of session after the last; a link to the
// sequencer that drops, duplicates and delays (so reorders) packets, by
// pattern or at random with a fixed seed; a link back that does the same to
// requests; and the re-request server answering them over the first link.
// Time is simulated, in nanoseconds; the sequencer's on_tick is called every
// `tick_ns`. Each delivered message is checked against the store: in
// sequence, exactly once, the same bytes.
#pragma once

#include <itchfh/mold.hpp>
#include <itchfh/sequencer.hpp>
#include <itchfh/session_store.hpp>

#include <cstdint>
#include <cstring>
#include <functional>
#include <queue>
#include <random>
#include <string>
#include <vector>

namespace itchfh::test {

struct link_rules {
    double drop = 0, duplicate = 0, delay = 0;  // probability per packet
    std::int64_t delay_ns = 20'000;             // a delayed packet's extra delay, up to
    std::int64_t latency_ns = 5'000;            // every packet's
    double drop_requests = 0;
    std::uint64_t seed = 2026;
    // Packets of the first sending (by their first sequence number) dropped
    // whatever the probabilities: a pattern.
    std::function<bool(std::uint64_t first)> drop_first = nullptr;
    std::function<bool(std::uint64_t request_number)> drop_request = nullptr;
};

template <class Next>  // Next: what the sequencer delivers to, with deliver(seq, bytes, len)
class lossy_link {
public:
    struct output {
        lossy_link& l;
        void deliver(std::uint64_t s, const unsigned char* m, std::size_t len) { l.got(s, m, len); }
        void state(feed_state f, std::uint64_t next) { l.seen_.states.push_back({f, next}); }
        void send_request(const unsigned char* p, std::size_t n) { l.request(p, n); }
    };
    struct observed {
        std::vector<std::pair<feed_state, std::uint64_t>> states;
        std::uint64_t delivered = 0, out_of_order = 0, wrong_bytes = 0, requests_sent = 0, requests_answered = 0;
    };

    lossy_link(const session_store& store, const link_rules& r, const sequencer_config& c, Next& next)
        : store_(store), rules_(r), rng_(r.seed), next_(next), out_{*this}, seq_(out_, c) {}

    // Sends every packet of the store, one every `gap_ns`, then heartbeats and
    // the end of session; runs until everything is delivered or `until_ns`.
    void run(std::int64_t gap_ns, std::int64_t until_ns, std::int64_t tick_ns = 100'000) {
        std::uint64_t first = 1;
        std::int64_t t = 0;
        unsigned char buf[mold::header_size + mold::payload];
        while (first <= store_.count()) {
            const std::size_t n = store_.fill(first, ~0ull, buf);
            const std::uint64_t count = mold::read_header(buf).count;
            const bool dropped = rules_.drop_first && rules_.drop_first(first);
            if (!dropped) send_down(buf, n, t);
            first += count;
            t += gap_ns;
            pump(t, tick_ns);
        }
        // Heartbeats then the end, every millisecond, until done or the time is up.
        for (int k = 0; t < until_ns; ++k) {
            mold::build_marker(buf, store_.session(), store_.count() + 1, k < 5 ? mold::heartbeat : mold::end_of_session);
            send_down(buf, mold::header_size, t);
            t += 1'000'000;
            pump(t, tick_ns);
            if (seq_.state() == feed_state::ended || seq_.state() == feed_state::lost) break;
        }
        pump(t + 10'000'000, tick_ns);
    }
    // Sends a packet as it is (for hand-built cases), then runs the clock to `until`.
    void inject(const unsigned char* p, std::size_t n, std::int64_t at, std::int64_t until, std::int64_t tick_ns = 100'000) {
        send_down(p, n, at);
        pump(until, tick_ns);
    }
    void advance(std::int64_t until, std::int64_t tick_ns = 100'000) { pump(until, tick_ns); }

    sequencer<lossy_link::output>& seq() { return seq_; }
    const observed& seen() const { return seen_; }
    std::int64_t now() const { return now_; }


private:
    struct event {
        std::int64_t at;
        std::uint64_t order;
        bool to_server;
        std::vector<unsigned char> bytes;
        bool operator>(const event& o) const { return at != o.at ? at > o.at : order > o.order; }
    };

    double u() { return std::uniform_real_distribution<double>(0, 1)(rng_); }
    void send_down(const unsigned char* p, std::size_t n, std::int64_t at) {
        if (u() < rules_.drop) return;
        const int copies = u() < rules_.duplicate ? 2 : 1;
        for (int c = 0; c < copies; ++c) {
            std::int64_t when = at + rules_.latency_ns;
            if (u() < rules_.delay) when += static_cast<std::int64_t>(u() * static_cast<double>(rules_.delay_ns));
            q_.push({when, order_++, false, std::vector<unsigned char>(p, p + n)});
        }
    }
    void request(const unsigned char* p, std::size_t n) {
        ++seen_.requests_sent;
        if ((rules_.drop_request && rules_.drop_request(seen_.requests_sent)) || u() < rules_.drop_requests) return;
        q_.push({now_ + rules_.latency_ns, order_++, true, std::vector<unsigned char>(p, p + n)});
    }
    void got(std::uint64_t s, const unsigned char* m, std::size_t len) {
        ++seen_.delivered;
        if (s != expected_) ++seen_.out_of_order;
        expected_ = s + 1;
        std::size_t want_len;
        const unsigned char* want = store_.message(s, want_len);
        if (want_len != len || std::memcmp(want, m, len) != 0) ++seen_.wrong_bytes;
        next_.deliver(s, m, len);
    }
    void pump(std::int64_t until, std::int64_t tick_ns) {
        while (true) {
            const std::int64_t next_tick = (now_ / tick_ns + 1) * tick_ns;
            const std::int64_t next_event = q_.empty() ? until + 1 : q_.top().at;
            const std::int64_t t = std::min(next_tick, next_event);
            if (t > until) break;
            now_ = t;
            if (next_event <= next_tick) {
                event e = q_.top();
                q_.pop();
                if (e.to_server) {
                    unsigned char buf[mold::header_size + mold::payload];
                    if (const std::size_t n = store_.answer(e.bytes.data(), e.bytes.size(), buf)) {
                        ++seen_.requests_answered;
                        send_down(buf, n, now_);
                    }
                } else {
                    seq_.on_packet(e.bytes.data(), e.bytes.size(), now_);
                }
            } else {
                seq_.on_tick(now_);
            }
        }
        now_ = std::max(now_, until);
    }

    const session_store& store_;
    link_rules rules_;
    std::mt19937_64 rng_;
    Next& next_;
    output out_;
    sequencer<output> seq_;
    std::priority_queue<event, std::vector<event>, std::greater<>> q_;
    std::uint64_t order_ = 0, expected_ = 1;
    std::int64_t now_ = 0;
    observed seen_;
};

struct ignore_messages {
    void deliver(std::uint64_t, const unsigned char*, std::size_t) {}
};

}  // namespace itchfh::test
