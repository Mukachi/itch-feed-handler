// The sequencer (note 005): MoldUDP64 downstream packets in, in any order and
// with any losses or repeats; out, every message once and in sequence, the
// re-request packets to send, and each change of reliability state. Sans-I/O:
// time is given (nanoseconds), nothing is read or sent here.
//   D1  what follows a gap is held (fixed slots, ordered by sequence number)
//       and applied once the gap is filled
//   D2  a gap is asked for exactly, in steps of `request_step` messages, up
//       to `window` requests in flight (one answer is one packet, so one
//       request at a time recovered too slowly: amended, note 005 Review);
//       more asked as answers move it on; the window asked again from the
//       front after the timeout without progress, up to the retries
//   D3  messages below the next expected are dropped and counted
//   D4  spent retries or a full hold: `lost`, until resume() from a snapshot
//   D5  nothing within the staleness bound: `stale`; a heartbeat beyond the
//       next expected is a gap at the tail; another session is refused
// The output (a template parameter) has three calls, made directly:
//   deliver(std::uint64_t sequence, const unsigned char* message, std::size_t length)
//   state(feed_state now, std::uint64_t next_expected)
//   send_request(const unsigned char* packet, std::size_t length)
// Design: docs/design/005-sequencing-recovery.md
#pragma once

#include <itchfh/memory.hpp>
#include <itchfh/mold.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace itchfh {

enum class feed_state : std::uint8_t { waiting, live, gapped, stale, lost, ended };

struct sequencer_config {
    std::size_t hold_bytes = 64u << 20;     // D4
    std::size_t slot_bytes = 2048;          // a held packet's room (ours are at most 1,420 bytes)
    std::int64_t retry_ns = 1'000'000;      // D2
    int retries = 5;
    // D2 amended: messages a request asks for, the fewest a full packet can
    // carry (1,400-byte payloads of the longest message, 50 bytes and its
    // 2-byte length), so the answers overlap rather than leave holes; and the
    // requests in flight at once.
    std::uint32_t request_step = 26;
    int window = 16;
    std::int64_t stale_ns = 2'000'000'000;  // D5
    // The first sequence number expected: 1 for a session joined at its start;
    // a first packet beyond it is a gap, not a new beginning.
    std::uint64_t first_expected = 1;
    bool prefault = true;
    // The hold in huge pages (note 005's amendment to D4).
    bool huge_pages = true;
};

struct sequencer_counts {
    std::uint64_t packets = 0, malformed = 0, other_session = 0, too_big = 0;
    std::uint64_t duplicate_messages = 0, duplicate_packets = 0;
    std::uint64_t held = 0, hold_full = 0, requests = 0, retried = 0, gaps = 0;
};

template <class Out>
class sequencer {
public:
    sequencer(Out& out, const sequencer_config& c = {})
        : out_(out), cfg_(c), slots_(c.hold_bytes / c.slot_bytes),
          mem_(c.huge_pages ? zeroed_huge<unsigned char>(slots_ * c.slot_bytes, c.prefault) : zeroed<unsigned char>(slots_ * c.slot_bytes, c.prefault)),
          slot_len_(zeroed<std::uint32_t>(slots_, c.prefault)), heap_(zeroed<entry>(slots_, c.prefault)),
          free_(zeroed<std::uint32_t>(slots_, c.prefault)) {
        for (std::size_t i = 0; i < slots_; ++i) free_[i] = static_cast<std::uint32_t>(slots_ - 1 - i);
        free_n_ = slots_;
        next_ = c.first_expected;
    }

    void on_packet(const unsigned char* p, std::size_t n, std::int64_t now) {
        ++counts_.packets;
        if (!mold::each_message(p, n, [](std::uint64_t, const unsigned char*, std::size_t) {})) {
            ++counts_.malformed;
            return;
        }
        const mold::header h = mold::read_header(p);
        if (!have_session_) {
            session_ = h.session;
            have_session_ = true;
        } else if (h.session != session_) {
            ++counts_.other_session;
            return;
        }
        last_rx_ = now;
        if (h.count == mold::heartbeat || h.count == mold::end_of_session) {
            if (h.sequence > known_end_) known_end_ = h.sequence;
            ended_ = ended_ || h.count == mold::end_of_session;
        } else {
            const std::uint64_t first = h.sequence, last = h.sequence + h.count;
            if (last > known_end_) known_end_ = last;
            if (last <= next_) {
                ++counts_.duplicate_packets;
                counts_.duplicate_messages += h.count;
            } else if (first <= next_) {
                apply(p, n);
                drain();
            } else {
                hold(p, n, first);
            }
        }
        settle(now);
    }

    // Timeouts: a re-request unanswered, or silence.
    void on_tick(std::int64_t now) {
        if (state_ == feed_state::gapped && now - asked_at_ >= cfg_.retry_ns) {
            if (++tries_ > cfg_.retries) {
                change(feed_state::lost);
            } else {
                ++counts_.retried;
                asked_upto_ = next_;  // the whole window again, from the front
                ask(now);
            }
        }
        if ((state_ == feed_state::live || state_ == feed_state::waiting) && have_session_ && now - last_rx_ >= cfg_.stale_ns)
            change(feed_state::stale);
    }

    // D4: after a snapshot taken at `next` (the first message it does not
    // cover): what is held below it is dropped, the rest applied.
    void resume(std::uint64_t next, std::int64_t now) {
        next_ = next;
        if (next_ > known_end_) known_end_ = next_;
        while (heap_n_ > 0 && last_of(heap_[0]) <= next_) release(pop());
        drain();
        tries_ = 0;
        asked_ = false;
        if (state_ == feed_state::lost) state_ = feed_state::gapped;  // re-judged below
        settle(now);
    }

    // Lost on purpose (note 011, D1: a late join from a snapshot): nothing is
    // asked for; what arrives is held until resume().
    void lose() { change(feed_state::lost); }

    feed_state state() const { return state_; }
    std::uint64_t next_expected() const { return next_; }
    std::size_t held_packets() const { return heap_n_; }
    const sequencer_counts& counts() const { return counts_; }

private:
    struct entry {
        std::uint64_t first;
        std::uint32_t slot, count;
    };
    static std::uint64_t last_of(const entry& e) { return e.first + e.count; }

    // Each message of a valid packet at or beyond the next expected, in order.
    void apply(const unsigned char* p, std::size_t n) {
        mold::each_message(p, n, [&](std::uint64_t s, const unsigned char* m, std::size_t len) {
            if (s < next_) {
                ++counts_.duplicate_messages;
                return;
            }
            out_.deliver(s, m, len);
            next_ = s + 1;
        });
    }
    // A packet beyond a gap, kept. A duplicate of one already held is kept too:
    // the heap brings it out beside its twin, where it delivers nothing and is
    // counted (scanning the hold for it made holding quadratic in the backlog:
    // 2.9 us a packet behind 10,000, found by the benchmark).
    void hold(const unsigned char* p, std::size_t n, std::uint64_t first) {
        if (n > cfg_.slot_bytes) {
            ++counts_.too_big;
            return;
        }
        if (free_n_ == 0) {
            ++counts_.hold_full;
            change(feed_state::lost);
            return;
        }
        const std::uint32_t slot = free_[--free_n_];
        std::memcpy(mem_.get() + std::size_t{slot} * cfg_.slot_bytes, p, n);
        slot_len_[slot] = static_cast<std::uint32_t>(n);
        push({first, slot, mold::read_header(p).count});
        ++counts_.held;
    }
    // Held packets that now join the sequence, in order.
    void drain() {
        while (heap_n_ > 0 && heap_[0].first <= next_) {
            const entry e = pop();
            if (last_of(e) > next_) apply(mem_.get() + std::size_t{e.slot} * cfg_.slot_bytes, slot_len_[e.slot]);
            else counts_.duplicate_messages += e.count;
            release(e);
        }
    }
    // The state the sequence now gives, and a request if a gap is open.
    void settle(std::int64_t now) {
        if (state_ == feed_state::lost) return;
        if (next_ < known_end_ || heap_n_ > 0) {
            if (state_ != feed_state::gapped) {
                ++counts_.gaps;
                change(feed_state::gapped);
                tries_ = 0;
                asked_ = false;
            }
            // When the gap has moved on since (or is new), the window is
            // topped up beyond what is in flight; otherwise asked again only
            // after the timeout (on_tick). The retries count timeouts without
            // progress: they start again when the gap moves on (found by the
            // random streams: counted over a whole gapped stretch, five
            // timeouts anywhere in it gave up).
            if (!asked_ || asked_from_ != next_) {
                tries_ = 0;
                if (!asked_ || asked_upto_ < next_) asked_upto_ = next_;
                ask(now);
            }
            return;
        }
        if (ended_) change(feed_state::ended);
        else change(feed_state::live);
    }
    // Requests from `asked_upto_` (what is in flight ends there) to the gap's
    // end or the window's, a step each.
    void ask(std::int64_t now) {
        const std::uint64_t to = heap_n_ > 0 ? heap_[0].first : known_end_;
        if (to <= next_) return;
        const std::uint64_t step = cfg_.request_step;
        const std::uint64_t limit = to < next_ + step * static_cast<std::uint64_t>(cfg_.window) ? to : next_ + step * static_cast<std::uint64_t>(cfg_.window);
        for (std::uint64_t s = asked_upto_; s < limit;) {
            const std::uint64_t want = limit - s < step ? limit - s : step;
            unsigned char req[mold::request_size];
            mold::build_request(req, {session_, s, static_cast<std::uint16_t>(want)});
            out_.send_request(req, sizeof(req));
            ++counts_.requests;
            s += want;
            asked_upto_ = s;
        }
        asked_ = true;
        asked_from_ = next_;
        asked_at_ = now;
    }
    void change(feed_state s) {
        if (s == state_) return;
        state_ = s;
        out_.state(s, next_);
    }

    // The hold's order: a binary min-heap by first sequence number.
    void push(entry e) {
        std::size_t i = heap_n_++;
        heap_[i] = e;
        while (i > 0 && heap_[(i - 1) / 2].first > heap_[i].first) {
            std::swap(heap_[(i - 1) / 2], heap_[i]);
            i = (i - 1) / 2;
        }
    }
    entry pop() {
        const entry top = heap_[0];
        heap_[0] = heap_[--heap_n_];
        for (std::size_t i = 0;;) {
            const std::size_t l = 2 * i + 1, r = l + 1;
            std::size_t m = i;
            if (l < heap_n_ && heap_[l].first < heap_[m].first) m = l;
            if (r < heap_n_ && heap_[r].first < heap_[m].first) m = r;
            if (m == i) break;
            std::swap(heap_[i], heap_[m]);
            i = m;
        }
        return top;
    }
    void release(const entry& e) { free_[free_n_++] = e.slot; }

    Out& out_;
    sequencer_config cfg_;
    std::size_t slots_;
    table<unsigned char> mem_;
    table<std::uint32_t> slot_len_;
    table<entry> heap_;
    table<std::uint32_t> free_;
    std::size_t heap_n_ = 0, free_n_ = 0;
    mold::session_id session_{};
    bool have_session_ = false, ended_ = false, asked_ = false;
    std::uint64_t next_ = 1, known_end_ = 0, asked_from_ = 0, asked_upto_ = 0;
    std::int64_t last_rx_ = 0, asked_at_ = 0;
    int tries_ = 0;
    feed_state state_ = feed_state::waiting;
    sequencer_counts counts_;
};

}  // namespace itchfh
