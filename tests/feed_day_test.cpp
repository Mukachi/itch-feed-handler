// The feed over Nasdaq's sample day, over loopback multicast (note 007, S1,
// S2 and S4), label `data`: needs data/01302020.NASDAQ_ITCH50.gz
// (tools/fetch_day.py), so CI does not run it. The day's messages kept in a
// session store; our sender (on a thread here; note 007's benchmark runs it
// as its own process, and over the card) sends it onto the group at a fixed
// pace, dropping a share of packets, and answers re-requests. Received by
// each of D4's arrangements, behind them the default decoder and the handler
// on the default book. Checked: every message handled once; the feed ended,
// nothing held; the book equal, every level and queue, to a book fed the same
// messages directly; kernel drops (if any) counted and recovered. And S4: the
// first run's datagrams recorded as received, from both sockets, replayed
// into a fresh sequencer: the same book.
//
// Usage: feed_day_test [day.gz] [packets a second, default 100000]
#include "check.hpp"

#include <itchfh/book.hpp>
#include <itchfh/decoder.hpp>
#include <itchfh/feed.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/mold_sender.hpp>
#include <itchfh/session_store.hpp>
#include <itchfh/udp.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace itchfh;
using itchfh::test::check;

struct quiet_sink {
    void book(const applied&) {}
    void trade(const trade_report&) {}
    void trade_broken(const trade_report&) {}
    void status(std::uint16_t, status_field) {}
    void imbalance(std::uint16_t, const itchfh::imbalance&) {}
    void market(const message&) {}
    void directory(std::uint16_t, const stock_directory&) {}
};
using day_handler = handler<default_book, quiet_sink>;

struct client {
    std::unique_ptr<default_book> book = std::make_unique<default_book>();
    quiet_sink sink;
    std::unique_ptr<day_handler> h = std::make_unique<day_handler>(*book, sink);
};

std::string books_differ(const default_book& a, const default_book& b) {
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

// Datagrams as received, end to end: a 2-byte length before each.
struct recording {
    std::vector<unsigned char> bytes;
    std::uint64_t datagrams = 0;
    void add(const unsigned char* p, std::size_t n) {
        bytes.push_back(static_cast<unsigned char>(n >> 8));
        bytes.push_back(static_cast<unsigned char>(n));
        bytes.insert(bytes.end(), p, p + n);
        ++datagrams;
    }
};

struct deliver_to_handler {
    client& c;
    udp::receiver* rx = nullptr;
    std::uint64_t messages = 0, undecoded = 0;
    void deliver(std::uint64_t, const unsigned char* p, std::size_t n) {
        message m;
        if (decode(p, n, m) != decode_status::ok) {
            ++undecoded;
            return;
        }
        ++messages;
        c.h->on(m);
    }
    void state(feed_state, std::uint64_t) {}
    void send_request(const unsigned char* p, std::size_t n) {
        if (rx) rx->send_request(p, n);
    }
};

struct result {
    std::string books;
    std::uint64_t messages = 0, undecoded = 0, kernel_drops = 0, requests = 0, held = 0;
    sender_counts sent;
    feed_state state = feed_state::waiting;
    double secs = 0;
};

const mold::session_id session = {'0', '0', '0', '0', '0', '0', '0', '0', '0', '1'};

// One run over the socket. arrangement: 0 feed_one, 1 feed_two, 2 the
// recording loop (feed_one's work, with every datagram kept).
result run(const session_store& store, const default_book& direct, double pace, double drop, int arrangement, std::uint16_t port,
           recording* rec) {
    result r;
    const auto t0 = std::chrono::steady_clock::now();
    std::string why;
    udp::receiver rx;
    udp::receiver::options ro;
    ro.port = port;
    udp::sender tx;
    if (!rx.open(ro, why) || !tx.open(ro.group, port, "127.0.0.1", static_cast<std::uint16_t>(port + 1), why)) {
        r.books = why;
        return r;
    }
    rx.set_server("127.0.0.1", static_cast<std::uint16_t>(port + 1));
    sender_config sc;
    sc.packets_per_s = pace;
    sc.drop = drop;
    client c;  // built (its tables faulted in, seconds) before anything is sent
    mold_sender s(store, tx, sc);
    std::atomic<bool> stop{false};
    std::thread st([&] { s.run(stop); });
    sequencer_config qc;
    qc.prefault = false;
    // Generous: the day at this pace, twice over.
    const std::int64_t give_up = monotonic_ns() + static_cast<std::int64_t>(2e9 * static_cast<double>(store.count()) / 45.0 / pace) + 60'000'000'000;
    if (arrangement == 0) {
        feed_one<day_handler> f(rx, *c.h, qc);
        while (f.seq().state() != feed_state::ended && f.seq().state() != feed_state::lost && monotonic_ns() < give_up) f.poll();
        r.messages = f.counts().messages;
        r.undecoded = f.counts().undecoded;
        r.kernel_drops = f.counts().kernel_drops;
        r.requests = f.seq().counts().requests;
        r.held = f.seq().held_packets();
        r.state = f.seq().state();
    } else if (arrangement == 1) {
        feed_two<day_handler> f(rx, *c.h, qc);
        std::atomic<bool> giving_up{false}, done{false};
        std::thread guard([&] {
            while (!done && monotonic_ns() < give_up) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            giving_up = true;
        });
        f.run(giving_up, [] {});
        done = true;
        guard.join();
        r.messages = f.counts().messages;
        r.kernel_drops = f.counts().kernel_drops;
        r.requests = f.seq().counts().requests;
        r.held = f.seq().held_packets();
        r.state = f.seq().state();
    } else {
        deliver_to_handler out{c, &rx};
        sequencer<deliver_to_handler> seq(out, qc);
        auto d = std::make_unique<udp::datagram[]>(udp::receiver::max_batch);
        while (seq.state() != feed_state::ended && seq.state() != feed_state::lost && monotonic_ns() < give_up) {
            const std::int64_t now = monotonic_ns();
            const int a = seq.state() == feed_state::gapped ? rx.receive_answers(d.get(), udp::receiver::max_batch) : 0;
            for (int i = 0; i < a; ++i) {
                rec->add(d[i].bytes, d[i].length);
                seq.on_packet(d[i].bytes, d[i].length, now);
            }
            const int n = rx.receive_batch(d.get(), udp::receiver::max_batch);
            for (int i = 0; i < n; ++i) {
                if (d[i].drops > r.kernel_drops) r.kernel_drops = d[i].drops;
                rec->add(d[i].bytes, d[i].length);
                seq.on_packet(d[i].bytes, d[i].length, now);
            }
            seq.on_tick(now);
        }
        r.messages = out.messages;
        r.undecoded = out.undecoded;
        r.requests = seq.counts().requests;
        r.held = seq.held_packets();
        r.state = seq.state();
    }
    stop = true;
    st.join();
    r.sent = s.counts();
    r.books = books_differ(direct, *c.book);
    r.secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : ITCHFH_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    const double pace = argc > 2 ? std::atof(argv[2]) : 100'000;
    auto store = std::make_unique<session_store>(session, 450'000'000, std::size_t{14} << 30, false);
    {
        std::FILE* in = ::popen(("gzip -dc '" + path + "'").c_str(), "r");
        unsigned char buf[1 << 16];
        for (; in;) {
            unsigned char lb[2];
            if (std::fread(lb, 1, 2, in) != 2) break;
            const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
            if (len == 0 || std::fread(buf, 1, len, in) != len || !store->append(buf, len)) break;
        }
        if (in) ::pclose(in);
    }
    if (store->count() == 0) {
        check(false, path + ": cannot read (tools/fetch_day.py fetches it)");
        return itchfh::test::result();
    }
    client direct;
    for (std::uint64_t s = 1; s <= store->count(); ++s) {
        std::size_t len;
        const unsigned char* m = store->message(s, len);
        message msg;
        if (decode(m, len, msg) == decode_status::ok) direct.h->on(msg);
    }
    recording rec;
    rec.bytes.reserve(std::size_t{15} << 30);
    struct case_ {
        const char* name;
        int arrangement;
        double drop;
    } cases[] = {{"one thread (recorded)", 2, 0.001}, {"one thread", 0, 0.01}, {"two threads", 1, 0.01}};
    std::uint16_t port = 26500;
    for (const case_& k : cases) {
        const result r = run(*store, *direct.book, pace, k.drop, k.arrangement, port, k.arrangement == 2 ? &rec : nullptr);
        port = static_cast<std::uint16_t>(port + 2);
        check(r.books.empty() && r.state == feed_state::ended && r.held == 0 && r.messages == store->count() && r.undecoded == 0,
              std::string("S1, S2, the day over loopback multicast, ") + k.name + ", " + std::to_string(static_cast<int>(pace)) +
                  " packets a second, " + std::to_string(k.drop * 100).substr(0, 4) + "% dropped by the sender: " + std::to_string(r.messages) +
                  " messages handled once; ended, nothing held; the book equal to the one fed directly" +
                  (r.books.empty() ? "" : " (" + r.books + ")") + "; " + std::to_string(r.sent.packets) + " packets, " +
                  std::to_string(r.sent.dropped) + " dropped by the sender, " + std::to_string(r.kernel_drops) + " by the kernel, " +
                  std::to_string(r.requests) + " re-requests, " + std::to_string(r.sent.behind) + " sent late; state " +
                  std::to_string(static_cast<int>(r.state)) + " (" + std::to_string(static_cast<int>(r.secs)) + " s)");
    }
    // S4: the recording replayed into a fresh sequencer, as if from the socket.
    {
        client c;
        deliver_to_handler out{c, nullptr};
        sequencer_config qc;
        qc.prefault = false;
        sequencer<deliver_to_handler> seq(out, qc);
        std::uint64_t n = 0;
        for (std::size_t at = 0; at + 2 <= rec.bytes.size(); ++n) {
            const std::size_t len = std::size_t{rec.bytes[at]} << 8 | rec.bytes[at + 1];
            seq.on_packet(rec.bytes.data() + at + 2, len, 0);
            at += 2 + len;
        }
        const std::string diff = books_differ(*direct.book, *c.book);
        check(n == rec.datagrams && seq.state() == feed_state::ended && seq.held_packets() == 0 && out.messages == store->count() && diff.empty(),
              "S4: the first run's " + std::to_string(n) + " datagrams (" + std::to_string(rec.bytes.size() >> 20) +
                  " MB) replayed into a fresh sequencer: ended, nothing held, every message once, the book equal to the one fed directly" +
                  (diff.empty() ? "" : " (" + diff + ")"));
    }
    return itchfh::test::result();
}
