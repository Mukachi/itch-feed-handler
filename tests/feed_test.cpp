// Tests for include/itchfh/udp.hpp, mold_sender.hpp and feed.hpp. Spec:
// docs/design/007-receiver-framer.md (S1 to S4 in small): our sender and the
// receiver over real loopback multicast, a synthetic stream (encoded, in a
// session store), in both of D4's arrangements and D1's two ways of reading,
// with no loss and with 2% of packets dropped by the sender (recovered by
// re-request over unicast): every message handled once, the books equal to
// the reference's; malformed datagrams counted, never applied; and the
// datagrams received, recorded and replayed into a fresh sequencer, giving
// the same books (D5).
#include "check.hpp"
#include "handler_checks.hpp"
#include "synthetic_stream.hpp"

#include <itchfh/book.hpp>
#include <itchfh/encoder.hpp>
#include <itchfh/feed.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/mold_sender.hpp>
#include <itchfh/session_store.hpp>
#include <itchfh/snapshot.hpp>
#include <itchfh/udp.hpp>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace itchfh;
using itchfh::test::check;

book_config small_book() {
    book_config c;
    c.orders = 50'000;
    c.levels = 50'000;
    c.ref_slots = 1 << 17;
    c.ref_span = 1 << 20;
    c.level_arena = 1 << 18;
    c.prefault = false;
    return c;
}
handler_config small_handler() {
    handler_config c;
    c.matches = 100'000;
    c.participants = 10'000;
    c.prefault = false;
    return c;
}
sequencer_config small_seq() {
    sequencer_config c;
    c.hold_bytes = 16u << 20;
    c.prefault = false;
    c.huge_pages = false;
    // Re-requests given 20 ms, not the default 1 ms: under a sanitizer on CI's shared runners an answer
    // can take longer than five 1 ms retries (the live feed then lost, the test failed; seen in CI).
    c.retry_ns = 20'000'000;
    return c;
}

using quiet_handler = handler<default_book, itchfh::test::recording_sink>;

struct client {
    std::unique_ptr<default_book> book = std::make_unique<default_book>(small_book());
    itchfh::test::recording_sink sink;
    std::unique_ptr<quiet_handler> h;
    client() {
        sink.keep = false;
        h = std::make_unique<quiet_handler>(*book, sink, small_handler());
    }
};

std::string books_differ(const reference_book& a, const default_book& b) {
    if (a.instruments() != b.instruments()) return "different instruments have orders";
    for (const std::uint16_t l : a.instruments())
        for (const side s : {side::bid, side::ask}) {
            const auto lv = a.levels(l, s);
            if (lv != b.levels(l, s)) return "locate " + std::to_string(l) + ": levels differ";
            for (const level_view& v : lv)
                if (a.queue(l, s, v.price) != b.queue(l, s, v.price)) return "locate " + std::to_string(l) + ": a queue differs";
        }
    return a.live_orders() == b.live_orders() ? std::string{} : "live orders differ";
}

const mold::session_id session = {'0', '0', '0', '0', '0', '0', '0', '0', '0', '7'};

struct world {
    std::vector<message> all = itchfh::test::stream(80'000);
    std::unique_ptr<session_store> store;
    snapshot_server reference;  // the reference handler and book, fed everything
    world() {
        store = std::make_unique<session_store>(session, all.size(), all.size() * 64, false);
        unsigned char b[64];
        for (const message& m : all) {
            store->append(b, encode(m, b));
            reference.on(m);
        }
    }
};

// One run: the sender on a thread, the receiver here; `two` picks D4 (b).
struct outcome {
    std::string books;
    feed_counts counts;
    sender_counts sent;
    std::uint64_t malformed = 0, requests = 0;
    feed_state state = feed_state::waiting;
    int granted = 0;
};

outcome run(const world& w, std::uint16_t port, double drop, bool two, bool batch, bool malformed = false, bool overflow = false, bool slow = false) {
    outcome o;
    std::string why;
    udp::receiver rx;
    udp::receiver::options ro;
    ro.port = port;
    if (overflow) ro.buffer_bytes = 1 << 16;  // the kernel's to fill while the receiver is away
    if (!rx.open(ro, why)) {
        o.books = "receiver: " + why;
        return o;
    }
    o.granted = rx.granted_buffer();
    rx.set_server("127.0.0.1", static_cast<std::uint16_t>(port + 1));
    udp::sender tx;
    if (!tx.open(ro.group, port, "127.0.0.1", static_cast<std::uint16_t>(port + 1), why)) {
        o.books = "sender: " + why;
        return o;
    }
    sender_config sc;
    // Slow enough for a sanitized receiver (TSan ~10x slower) not to overflow CI's small buffer; the
    // slow receiver needs a sender faster than it, and the kernel-drop case one far faster.
    sc.packets_per_s = overflow ? 1'000'000 : slow ? 50'000 : 5'000;
    sc.drop = drop;
    sc.heartbeat_ns = 100'000'000;  // within the sequencer's 2 s staleness, and quick
    client c;  // built before anything is sent (under TSan it takes long enough for the buffer to overflow)
    mold_sender s(*w.store, tx, sc);
    std::atomic<bool> stop{false};
    std::thread st([&] {
        if (malformed) {  // two bad datagrams first: too short, and blocks overrunning
            unsigned char bad[30] = {};
            std::memcpy(bad, session.data(), 10);
            bad[19] = 1;
            bad[20] = 0;
            bad[21] = 200;
            tx.send(bad, 10);
            tx.send(bad, 30);
        }
        s.run(stop);
    });
    if (overflow) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (two) {
        feed_two<quiet_handler> f(rx, *c.h, small_seq());
        std::atomic<bool> giving_up{false}, done{false};
        std::thread guard([&] {  // a run that never ends is stopped after 60 s
            for (int i = 0; i < 6000 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            giving_up = true;
        });
        f.run(giving_up, [] {});
        done = true;
        guard.join();
        o.counts = f.counts();
        o.malformed = f.seq().counts().malformed;
        o.requests = f.seq().counts().requests;
        o.state = f.seq().state();
    } else {
        feed_one<quiet_handler> f(rx, *c.h, small_seq(), batch);
        const std::int64_t give_up = monotonic_ns() + 60'000'000'000;
        while (f.seq().state() != feed_state::ended && f.seq().state() != feed_state::lost && monotonic_ns() < give_up) {
            f.poll();
            if (slow) std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        o.counts = f.counts();
        o.malformed = f.seq().counts().malformed;
        o.requests = f.seq().counts().requests;
        o.state = f.seq().state();
    }
    stop = true;
    st.join();
    o.sent = s.counts();
    o.books = books_differ(w.reference.book(), *c.book);
    return o;
}

void over_loopback(const world& w) {
    struct arrangement {
        const char* name;
        bool two, batch;
    } as[] = {{"one thread, recvmmsg", false, true}, {"one thread, recvmsg", false, false}, {"two threads", true, true}};
    std::uint16_t port = 26410;
    for (const arrangement& a : as)
        for (const double drop : {0.0, 0.02}) {
            const outcome o = run(w, port, drop, a.two, a.batch);
            port = static_cast<std::uint16_t>(port + 2);
            check(o.books.empty() && o.state == feed_state::ended && o.counts.messages == w.all.size() && o.counts.undecoded == 0 &&
                      (drop == 0 || (o.sent.dropped > 0 && o.requests > 0 && o.sent.answered > 0)),
                  std::string("S1 in small, ") + a.name + (drop ? ", 2% of packets dropped by the sender" : ", nothing dropped") + ": " +
                      std::to_string(o.counts.messages) + " messages handled once over loopback multicast, the books the reference's; " +
                      std::to_string(o.sent.dropped) + " dropped, " + std::to_string(o.requests) + " re-requests, " +
                      std::to_string(o.sent.answered) + " answered over unicast; kernel drops " + std::to_string(o.counts.kernel_drops) + " (buffer " + std::to_string(o.granted / 1024) + " KB)" +
                      (o.books.empty() ? "" : " (" + o.books + ")") + "; state " + std::to_string(static_cast<int>(o.state)));
        }
}

// S2: drops by the kernel itself: a small buffer, a fast sender, and the
// receiver away for the first 5 ms.
void kernel_drops(const world& w) {
    const outcome o = run(w, 26460, 0, false, true, false, true);
    check(o.books.empty() && o.state == feed_state::ended && o.counts.messages == w.all.size() && o.counts.kernel_drops > 0 && o.requests > 0,
          "S2 in small: a 64 KB buffer, 1M packets/s, the receiver 5 ms late: " + std::to_string(o.counts.kernel_drops) +
              " datagrams dropped by the kernel, counted (SO_RXQ_OVFL), recovered by " + std::to_string(o.requests) +
              " re-requests; every message handled once, the books the reference's" + (o.books.empty() ? "" : " (" + o.books + ")"));
}

// D1 amended: a receiver slower than the sender (one datagram a call, 50 us
// apart: ~20k/s against 50k/s), 2% lost. With answers on the multicast socket
// they queued behind the backlog and the sequencer gave up (lost, at 1,297
// messages); on the request socket they come straight back.
void slow_receiver(const world& w) {
    const outcome o = run(w, 26470, 0.02, false, false, false, false, true);
    check(o.books.empty() && o.state == feed_state::ended && o.counts.messages == w.all.size() && o.requests > 0 && o.counts.answers > 0,
          "D1 amended: a receiver slower than the sender, 2% lost: " + std::to_string(o.requests) + " re-requests, " +
              std::to_string(o.counts.answers) + " answers read from the request socket ahead of the backlog; ended, every message "
              "handled once, the books the reference's" + (o.books.empty() ? "" : " (" + o.books + ")") +
              "; state " + std::to_string(static_cast<int>(o.state)));
}

void malformed_datagrams(const world& w) {
    const outcome o = run(w, 26440, 0, false, true, true);
    check(o.malformed == 2 && o.books.empty() && o.state == feed_state::ended,
          "S3: a datagram shorter than a MoldUDP64 header and one whose blocks overrun it: both counted as malformed, never applied (" +
              std::to_string(o.malformed) + " malformed; state " + std::to_string(static_cast<int>(o.state)) + "; " +
              std::to_string(o.counts.kernel_drops) + " kernel drops" + (o.books.empty() ? "" : "; " + o.books) + ")");
}

// D5: the datagrams as received, replayed into a fresh sequencer: the same books.
void recorded_replay(const world& w) {
    std::string why;
    udp::receiver rx;
    udp::receiver::options ro;
    ro.port = 26450;
    rx.open(ro, why);
    rx.set_server("127.0.0.1", 26451);
    udp::sender tx;
    tx.open(ro.group, 26450, "127.0.0.1", 26451, why);
    sender_config sc;
    sc.packets_per_s = 5'000;
    sc.drop = 0.02;
    sc.heartbeat_ns = 100'000'000;
    std::vector<std::vector<unsigned char>> recorded;
    struct recording {
        std::unique_ptr<client> c = std::make_unique<client>();
        udp::receiver* rx = nullptr;
        void deliver(std::uint64_t, const unsigned char* p, std::size_t n) {
            message m;
            if (decode(p, n, m) == decode_status::ok) c->h->on(m);
        }
        void state(feed_state, std::uint64_t) {}
        void send_request(const unsigned char* p, std::size_t n) { rx->send_request(p, n); }
    } live;
    live.rx = &rx;
    sequencer<recording> seq(live, small_seq());
    mold_sender s(*w.store, tx, sc);  // started once the client is built
    std::atomic<bool> stop{false};
    std::thread st([&] { s.run(stop); });
    auto d = std::make_unique<udp::datagram[]>(64);
    const std::int64_t give_up = monotonic_ns() + 60'000'000'000;
    while (seq.state() != feed_state::ended && seq.state() != feed_state::lost && monotonic_ns() < give_up) {
        const std::int64_t now = monotonic_ns();
        const int a = seq.state() == feed_state::gapped ? rx.receive_answers(d.get(), 64) : 0;
        for (int i = 0; i < a; ++i) {
            recorded.emplace_back(d[i].bytes, d[i].bytes + d[i].length);
            seq.on_packet(d[i].bytes, d[i].length, now);
        }
        const int n = rx.receive_batch(d.get(), 64);
        for (int i = 0; i < n; ++i) {
            recorded.emplace_back(d[i].bytes, d[i].bytes + d[i].length);
            seq.on_packet(d[i].bytes, d[i].length, now);
        }
        seq.on_tick(now);
    }
    stop = true;
    st.join();
    // The replay: the same datagrams, in the order received, into a fresh one.
    struct replaying {
        std::unique_ptr<client> c = std::make_unique<client>();
        std::uint64_t requests = 0;
        void deliver(std::uint64_t, const unsigned char* p, std::size_t n) {
            message m;
            if (decode(p, n, m) == decode_status::ok) c->h->on(m);
        }
        void state(feed_state, std::uint64_t) {}
        void send_request(const unsigned char*, std::size_t) { ++requests; }
    } again;
    sequencer<replaying> seq2(again, small_seq());
    for (const auto& r : recorded) seq2.on_packet(r.data(), r.size(), 0);
    const std::string live_books = books_differ(w.reference.book(), *live.c->book), replay_books = books_differ(w.reference.book(), *again.c->book);
    check(live_books.empty() && replay_books.empty() && seq2.state() == feed_state::ended && seq2.held_packets() == 0,
          "S4 in small, D5: " + std::to_string(recorded.size()) + " datagrams recorded as received, from both sockets, in the order read (losses and answers among them), "
          "replayed into a fresh sequencer: ended, nothing held, the same books as live, both the reference's");
}

}  // namespace

int main() {
    const world w;
    over_loopback(w);
    kernel_drops(w);
    slow_receiver(w);
    malformed_datagrams(w);
    recorded_replay(w);
    return itchfh::test::result();
}
