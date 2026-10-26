// The feed's way in (note 007), in D4's two arrangements, from the receiver's
// socket through the sequencer (note 005), the default decoder (note 004) and
// a handler (note 003):
//   feed_one  one thread does it all: receive a batch (or one datagram, D1),
//             sequence each packet, decode and handle each message
//   feed_two  a receiving thread (socket and sequencer) hands each message to
//             a handling thread through lockfree-ipc's SPSC ring; if the ring
//             is full the receiver waits, and the socket's buffer takes up
//             the slack (or the kernel drops, counted, D3)
// Both busy-poll (D2). While the sequencer is gapped, the request socket's
// answers are read first (D1, amended); otherwise it is left alone. The
// kernel's drop counts (each socket's own) and receive stamps are kept.
// Design: docs/design/007-receiver-framer.md
#pragma once

#include <itchfh/decoder.hpp>
#include <itchfh/mold_sender.hpp>  // monotonic_ns
#include <itchfh/sequencer.hpp>
#include <itchfh/udp.hpp>

#include <lfipc/spsc.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>

namespace itchfh {

struct feed_counts {
    std::uint64_t datagrams = 0, answers = 0, messages = 0, undecoded = 0, kernel_drops = 0, answer_drops = 0, calls = 0, empty_calls = 0;
};

// The answers waiting on the request socket, into the sequencer.
template <class Seq>
void take_answers(udp::receiver& rx, Seq& seq, udp::datagram* d, feed_counts& c, std::int64_t now) {
    const int n = rx.receive_answers(d, udp::receiver::max_batch);
    for (int i = 0; i < n; ++i) {
        ++c.answers;
        if (d[i].drops > c.answer_drops) c.answer_drops = d[i].drops;
        seq.on_packet(d[i].bytes, d[i].length, now);
    }
}

template <class Handler>
class feed_one {
public:
    feed_one(udp::receiver& rx, Handler& h, const sequencer_config& c = {}, bool batch = true)
        : rx_(rx), h_(&h), out_{*this}, seq_(out_, c), batch_(batch), d_(std::make_unique<udp::datagram[]>(udp::receiver::max_batch)) {}

    // One pass: whatever datagrams are waiting, then the sequencer's timers.
    void poll() {
        const std::int64_t now = monotonic_ns();
        if (seq_.state() == feed_state::gapped) take_answers(rx_, seq_, d_.get(), counts_, now);
        int n = batch_ ? rx_.receive_batch(d_.get(), udp::receiver::max_batch) : (rx_.receive_one(d_[0]) ? 1 : 0);
        ++counts_.calls;
        if (n == 0) ++counts_.empty_calls;
        for (int i = 0; i < n; ++i) {
            ++counts_.datagrams;
            if (d_[i].drops > counts_.kernel_drops) counts_.kernel_drops = d_[i].drops;
            seq_.on_packet(d_[i].bytes, d_[i].length, now);
        }
        seq_.on_tick(now);
    }
    // Until the session has ended (or `stop`).
    void run(const std::atomic<bool>& stop) {
        while (!stop.load(std::memory_order_relaxed) && seq_.state() != feed_state::ended) poll();
    }
    auto& seq() { return seq_; }
    const feed_counts& counts() const { return counts_; }

    struct output {
        feed_one& f;
        void deliver(std::uint64_t, const unsigned char* p, std::size_t n) {
            message m;
            if (decode(p, n, m) != decode_status::ok) {
                ++f.counts_.undecoded;
                return;
            }
            ++f.counts_.messages;
            f.h_->on(m);
        }
        void state(feed_state, std::uint64_t) {}
        void send_request(const unsigned char* p, std::size_t n) { f.rx_.send_request(p, n); }
    };

private:
    udp::receiver& rx_;
    Handler* h_ = nullptr;
    output out_;
    sequencer<output> seq_;
    bool batch_;
    std::unique_ptr<udp::datagram[]> d_;
    feed_counts counts_;

public:
    // A rebuilt handler (after a snapshot, note 006) in place of the old.
    void replace_handler(Handler& h) { h_ = &h; }
};

// A message on the ring: its bytes (TotalView-ITCH's longest is 50).
struct ring_message {
    std::uint8_t length;
    unsigned char bytes[63];
};
static_assert(sizeof(ring_message) == 64);

template <class Handler>
class feed_two {
public:
    using ring = lfipc::spsc_ring<ring_message, 1u << 16>;

    feed_two(udp::receiver& rx, Handler& h, const sequencer_config& c = {})
        : rx_(rx), h_(h), ring_(std::make_unique<ring>()), out_{*this}, seq_(out_, c), d_(std::make_unique<udp::datagram[]>(udp::receiver::max_batch)) {}

    // The receiving side on this thread, the handling side on `handler_cpu`'s
    // thread (pinned by `pin`), until the session has ended and everything
    // received is handled (or `stop`).
    template <class Pin>
    void run(const std::atomic<bool>& stop, Pin&& pin_handler) {
        std::atomic<bool> receiving{true};
        std::thread t([&] {
            pin_handler();
            ring_message r;
            for (;;) {
                if (ring_->try_pop(r)) {
                    message m;
                    if (decode(r.bytes, r.length, m) == decode_status::ok) {
                        h_.on(m);
                        handled_.fetch_add(1, std::memory_order_relaxed);
                    }
                } else if (!receiving.load(std::memory_order_acquire)) {
                    if (!ring_->try_pop(r)) break;
                    message m;
                    if (decode(r.bytes, r.length, m) == decode_status::ok) {
                        h_.on(m);
                        handled_.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
        });
        while (!stop.load(std::memory_order_relaxed) && seq_.state() != feed_state::ended) {
            const std::int64_t now = monotonic_ns();
            if (seq_.state() == feed_state::gapped) take_answers(rx_, seq_, d_.get(), counts_, now);
            const int n = rx_.receive_batch(d_.get(), udp::receiver::max_batch);
            ++counts_.calls;
            if (n == 0) ++counts_.empty_calls;
            for (int i = 0; i < n; ++i) {
                ++counts_.datagrams;
                if (d_[i].drops > counts_.kernel_drops) counts_.kernel_drops = d_[i].drops;
                seq_.on_packet(d_[i].bytes, d_[i].length, now);
            }
            seq_.on_tick(now);
        }
        receiving.store(false, std::memory_order_release);
        t.join();
        counts_.messages = handled_.load();
    }
    auto& seq() { return seq_; }
    const feed_counts& counts() const { return counts_; }

    struct output {
        feed_two& f;
        void deliver(std::uint64_t, const unsigned char* p, std::size_t n) {
            ring_message r;
            r.length = static_cast<std::uint8_t>(n);
            std::memcpy(r.bytes, p, n);
            while (!f.ring_->try_push(r)) {
            }
        }
        void state(feed_state, std::uint64_t) {}
        void send_request(const unsigned char* p, std::size_t n) { f.rx_.send_request(p, n); }
    };

private:
    udp::receiver& rx_;
    Handler& h_;
    std::unique_ptr<ring> ring_;
    output out_;
    sequencer<output> seq_;
    std::unique_ptr<udp::datagram[]> d_;
    std::atomic<std::uint64_t> handled_{0};
    feed_counts counts_;
};

}  // namespace itchfh
