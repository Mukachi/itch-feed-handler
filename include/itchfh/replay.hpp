// Replay of a recording (note 009, D5; note 007's D5): its datagrams into the
// sequencer in the order they were read, each record's kernel stamp as the
// sequencer's clock (so its timers run as they did live), then the default
// decoder and the handler as feed_one gives them (a publishing handler gets
// sequence numbers and the sequencer's changes of state). At full speed, or
// at the recorded pace. Complete (D4): a trailer, nothing left unrecorded,
// every record whole and sound, and the feed ended with no gap open.
// Design: docs/design/009-recording-monitoring.md
#pragma once

#include <itchfh/decoder.hpp>
#include <itchfh/mold_sender.hpp>  // monotonic_ns
#include <itchfh/recording.hpp>
#include <itchfh/sequencer.hpp>

#include <atomic>
#include <cstdint>

namespace itchfh {

struct replay_result {
    std::uint64_t records = 0, messages = 0, undecoded = 0, skipped = 0;
    rec::reader::result stopped = rec::reader::result::end;  // end, truncated or corrupt
    feed_state state = feed_state::waiting;
    std::size_t held = 0;
    bool trailer = false, complete = false;
};

// `stop`, if given, ends the replay early (the program's signal).
template <class Handler>
replay_result replay(rec::reader& rd, Handler& h, const sequencer_config& c = {}, bool paced = false, const std::atomic<bool>* stop = nullptr) {
    struct output {
        Handler& h;
        replay_result& r;
        void deliver(std::uint64_t sequence, const unsigned char* p, std::size_t n) {
            message m;
            if (decode(p, n, m) != decode_status::ok) {
                ++r.undecoded;
                return;
            }
            ++r.messages;
            if constexpr (requires { h.on(sequence, m); }) h.on(sequence, m);
            else h.on(m);
        }
        void state(feed_state s, std::uint64_t next) {
            if constexpr (requires { h.feed(s, next); }) h.feed(s, next);
        }
        void send_request(const unsigned char*, std::size_t) {}  // nobody to ask: the recording has what came
    };
    replay_result r;
    output out{h, r};
    sequencer<output> seq(out, c);
    rec::reader::record x;
    std::int64_t first_stamp = 0, start = 0;
    for (;;) {
        if (stop && stop->load(std::memory_order_relaxed)) {
            r.stopped = rec::reader::result::truncated;  // not read to its end
            break;
        }
        const auto got = rd.next(x);
        if (got != rec::reader::result::record) {
            r.stopped = got;
            break;
        }
        ++r.records;
        r.skipped += x.h.skipped;
        if (paced) {
            if (r.records == 1) {
                first_stamp = x.h.kernel_ns;
                start = monotonic_ns();
            }
            while (monotonic_ns() - start < x.h.kernel_ns - first_stamp && !(stop && stop->load(std::memory_order_relaxed))) {
            }
        }
        seq.on_packet(x.bytes, x.h.length, x.h.kernel_ns);
        seq.on_tick(x.h.kernel_ns);
        if constexpr (requires { h.tick(x.h.kernel_ns); }) h.tick(x.h.kernel_ns);
    }
    r.state = seq.state();
    r.held = seq.held_packets();
    r.trailer = rd.has_trailer();
    r.complete = r.stopped == rec::reader::result::end && r.trailer && rd.trailer().unrecorded == 0 && r.skipped == 0 &&
                 r.state == feed_state::ended && r.held == 0;
    return r;
}

}  // namespace itchfh
