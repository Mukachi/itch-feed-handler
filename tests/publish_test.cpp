// Tests for include/itchfh/publication.hpp, publisher.hpp and consumer.hpp.
// Spec: docs/design/008-publication.md (S1 to S4 in small), on a synthetic
// stream: S1, a consumer in another process keeps books equal to the
// handler's and sees every trade, status and directory update once, in order,
// and the state table agrees with its books; S2, through the sequencer, a gap
// (what its backlog delivers is marked catching up) and then a loss rebuilt
// from a snapshot (reset, replay, the books equal again); S3, a consumer that
// stops reading is overrun without delaying the publisher or another
// consumer, and rejoins with the books; S4, the state table's seqlock never
// gives a torn entry, a writer and readers on other threads.
#include "check.hpp"
#include "synthetic_stream.hpp"

#include <itchfh/book.hpp>
#include <itchfh/consumer.hpp>
#include <itchfh/encoder.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/monitoring.hpp>
#include <itchfh/publisher.hpp>
#include <itchfh/sequencer.hpp>
#include <itchfh/session_store.hpp>
#include <itchfh/snapshot.hpp>

#include <atomic>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

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

std::string segment(const char* what) { return "/itchfh-pt-" + std::to_string(::getpid()) + "-" + what; }

// What the handler reports, counted by kind: what a consumer should see.
struct counting_sink {
    std::uint64_t levels = 0, trades = 0, broken = 0, statuses = 0, imbalances = 0, markets = 0, directories = 0;
    void book(const applied& a) {
        if (a.bad == bad_event::none) levels += a.changed;
    }
    void trade(const trade_report&) { ++trades; }
    void trade_broken(const trade_report&) { ++broken; }
    void status(std::uint16_t, status_field) { ++statuses; }
    void imbalance(std::uint16_t, const itchfh::imbalance&) { ++imbalances; }
    void market(const message&) { ++markets; }
    void directory(std::uint16_t, const stock_directory&) { ++directories; }
};

struct publishing {
    std::unique_ptr<default_book> book = std::make_unique<default_book>(small_book());
    std::unique_ptr<published<>> p = std::make_unique<published<>>(*book, small_handler());
    bool open(const std::string& name) {
        std::string why;
        const bool ok = p->pub.open(name, why);
        if (!ok) std::fprintf(stderr, "open: %s\n", why.c_str());
        return ok;
    }
    int live_slot() const {
        for (int c = 0; c < pub::max_consumers; ++c)
            if (p->pub.slot(c) == pub::slot_live) return c;
        return -1;
    }
};

// S1: the consumer in a child process; it reports through a pipe.
struct child_report {
    std::uint64_t digest, levels, trades, broken, statuses, imbalances, markets, directories, resets, feed_changes;
    bool in_order, table_agrees;
};

void another_process(const std::vector<message>& all) {
    const std::string name = segment("s1");
    publishing w;
    if (!w.open(name)) {
        check(false, "S1: the publisher's segments created");
        return;
    }
    int fds[2];
    if (::pipe(fds) != 0) return;
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(fds[0]);
        pub::consumer c;
        std::string why;
        child_report r{};
        if (c.attach(name, why)) {
            pub::books b;
            pub::update u;
            for (;;) {
                if (!c.poll(u)) continue;
                b.apply(u);
                if (u.k == pub::kind::feed && u.feed.state == feed_state::ended) break;
            }
            r = {b.digest(), b.levels, b.trades, b.broken, b.statuses, b.imbalances, b.markets, b.directories, b.resets, b.feed_changes, b.in_order, true};
            for (const auto& [l, sides] : b.by_instrument) {
                pub::entry_view e;
                c.read(l, e);
                const auto bb = b.best(l, side::bid), ba = b.best(l, side::ask);
                if (e.bid_price != bb.first || e.bid_shares != bb.second || e.ask_price != ba.first || e.ask_shares != ba.second) r.table_agrees = false;
            }
        }
        [[maybe_unused]] const ssize_t n = ::write(fds[1], &r, sizeof(r));
        ::_exit(0);
    }
    ::close(fds[1]);
    // Until the child has joined (its replay of an empty book done).
    int c = -1;
    for (int i = 0; i < 2'000'000 && c < 0; ++i) {
        w.p->tick(0);
        c = w.live_slot();
        if (c < 0) std::this_thread::yield();
    }
    std::uint64_t seq = 0;
    for (const message& m : all) {
        while (w.p->pub.behind(c) > pub::ring_capacity / 2) std::this_thread::yield();  // the test paces; the publisher never waits
        w.p->on(++seq, m);
    }
    w.p->feed(feed_state::ended, seq + 1);
    child_report r{};
    const bool got = ::read(fds[0], &r, sizeof(r)) == static_cast<ssize_t>(sizeof(r));
    ::close(fds[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    // What the handler reported, counted apart.
    auto rb = std::make_unique<default_book>(small_book());
    counting_sink k;
    handler<default_book, counting_sink> rh(*rb, k, small_handler());
    for (const message& m : all) rh.on(m);
    check(got && c >= 0 && r.digest == pub::digest_of(*w.book) && r.levels == k.levels && r.trades == k.trades && r.broken == k.broken &&
              r.statuses == k.statuses && r.imbalances == k.imbalances && r.markets == k.markets && r.directories == k.directories &&
              r.resets == 1 && r.in_order && r.table_agrees && w.p->pub.overruns() == 0,
          "S1 in small: a consumer in another process, " + std::to_string(all.size()) + " messages: its books equal the handler's (" +
              std::to_string(r.levels) + " level updates); every trade (" + std::to_string(r.trades) + "), break, status (" +
              std::to_string(r.statuses) + "), imbalance, market event and directory entry (" + std::to_string(r.directories) +
              ") once, sequence numbers never back; the state table's best bid and ask equal its books'");
}

// S2: through the sequencer: a gap, then a loss recovered from a snapshot.
void gap_and_snapshot(const std::vector<message>& all) {
    const std::string name = segment("s2");
    const mold::session_id session = {'0', '0', '0', '0', '0', '0', '0', '0', '0', '8'};
    auto store = std::make_unique<session_store>(session, all.size(), all.size() * 64, false);
    unsigned char b[64];
    for (const message& m : all) store->append(b, encode(m, b));
    std::vector<std::vector<unsigned char>> packets;
    for (std::uint64_t first = 1; first <= store->count();) {
        std::vector<unsigned char> pk(mold::header_size + mold::payload);
        pk.resize(store->fill(first, ~0ull, pk.data()));
        first += mold::read_header(pk.data()).count;
        packets.push_back(std::move(pk));
    }
    publishing w;
    if (!w.open(name)) return;
    pub::consumer c;
    std::string why;
    if (!c.attach(name, why)) return;
    pub::books cb;
    const auto drain = [&] {
        pub::update u;
        while (c.poll(u)) cb.apply(u);
    };
    // The handler the sequencer delivers to, swapped for a rebuilt one after the snapshot.
    using phandler = handler<default_book, publisher<default_book>>;
    struct out_t {
        phandler* h;
        publisher<default_book>* p;
        std::vector<feed_state> states;
        void deliver(std::uint64_t s, const unsigned char* q, std::size_t n) {
            message m;
            if (decode(q, n, m) != decode_status::ok) return;
            p->begin(s, m.timestamp_ns);
            h->on(m);
        }
        void state(feed_state f, std::uint64_t next) {
            states.push_back(f);
            p->feed(f, next);
        }
        void send_request(const unsigned char*, std::size_t) {}
    } out{&w.p->h, &w.p->pub, {}};
    sequencer_config qc;
    qc.prefault = false;
    qc.huge_pages = false;
    qc.hold_bytes = 16u << 20;
    sequencer<out_t> seq(out, qc);
    std::int64_t now = 0;
    const auto settle = [&] {
        for (int i = 0; i < 1000 && w.live_slot() < 0; ++i) {
            w.p->tick(now);
            drain();
        }
    };
    settle();
    // A gap: packet 20 held back until after packet 30.
    const std::size_t n = packets.size();
    for (std::size_t i = 0; i < 40; ++i) {
        if (i == 20) continue;
        seq.on_packet(packets[i].data(), packets[i].size(), now += 1000);
        if (i == 30) seq.on_packet(packets[20].data(), packets[20].size(), now += 1000);
        drain();
    }
    const std::uint64_t caught_up = cb.catching_up;
    const bool gap_seen = std::find(out.states.begin(), out.states.end(), feed_state::gapped) != out.states.end();
    // A loss: packets 60-79 never come; the retries are spent; lost.
    for (std::size_t i = 40; i < 90; ++i) {
        if (i >= 60 && i < 80) continue;
        seq.on_packet(packets[i].data(), packets[i].size(), now += 1000);
        drain();
    }
    for (int k = 0; k < 10 && seq.state() != feed_state::lost; ++k) seq.on_tick(now += qc.retry_ns + 1);
    const bool lost = seq.state() == feed_state::lost;
    // The snapshot: a server that followed every message up to the end of packet 89.
    std::uint64_t through = 0;
    for (std::size_t i = 0; i < 90; ++i) through += mold::read_header(packets[i].data()).count;
    snapshot_server server;
    for (std::uint64_t s = 0; s < through; ++s) server.on(all[s]);
    const std::vector<unsigned char> spin = server.spin(session, 0);
    auto b2 = std::make_unique<default_book>(small_book());
    auto h2 = std::make_unique<phandler>(*b2, w.p->pub, small_handler());
    w.p->pub.mute(true);
    w.p->pub.rebind(*b2);
    w.p->pub.watch(*h2);
    snapshot_client<phandler> sc(*h2);
    sc.feed(spin.data(), spin.size());
    w.p->pub.restored();
    out.h = h2.get();
    seq.resume(*sc.resume(), now);
    settle();
    const std::uint64_t resets_after = cb.resets, unrestored_before = cb.not_current;
    for (std::size_t i = 90; i < n; ++i) {
        seq.on_packet(packets[i].data(), packets[i].size(), now += 1000);
        drain();
        w.p->tick(now);
    }
    drain();
    // The rebuilt book against one fed every message directly.
    auto rb = std::make_unique<default_book>(small_book());
    counting_sink k;
    handler<default_book, counting_sink> rh(*rb, k, small_handler());
    for (const message& m : all) rh.on(m);
    check(gap_seen && caught_up > 0 && lost && sc.resume() && *sc.resume() == through + 1 && resets_after >= 2 &&
              std::find(out.states.begin(), out.states.end(), feed_state::lost) != out.states.end() && cb.digest() == pub::digest_of(*b2) &&
              pub::digest_of(*b2) == pub::digest_of(*rb) && cb.not_current > unrestored_before && cb.feed_changes >= 4,
          "S2 in small: a gap: " + std::to_string(caught_up) + " updates from its backlog marked catching up; a loss: the feed lost, "
          "its change published; a snapshot applied with the rings muted, then a reset and the rebuilt books replayed; afterwards "
          "the consumer's books equal the handler's, and those a book fed every message has; status not yet restored marked not current");
}

// S3: one consumer stops reading, another keeps up.
void overrun_and_rejoin(const std::vector<message>& all) {
    const std::string name = segment("s3");
    publishing w;
    if (!w.open(name)) return;
    pub::consumer slow, quick;
    std::string why;
    if (!slow.attach(name, why) || !quick.attach(name, why)) return;
    pub::books sb, qb;
    pub::update u;
    for (int i = 0; i < 100; ++i) {
        w.p->tick(0);
        while (quick.poll(u)) qb.apply(u);
        while (slow.poll(u)) sb.apply(u);
    }
    std::uint64_t seq = 0;
    for (const message& m : all) {  // the slow one reads nothing now
        w.p->on(++seq, m);
        while (quick.poll(u)) qb.apply(u);
    }
    const bool overrun = slow.overrun() && w.p->pub.overruns() == 1 && !quick.overrun();
    slow.rejoin();
    for (int i = 0; i < 100'000 && slow.state() != pub::slot_live; ++i) {
        w.p->tick(0);
        while (slow.poll(u)) sb.apply(u);
    }
    while (slow.poll(u)) sb.apply(u);
    while (quick.poll(u)) qb.apply(u);
    const std::uint64_t d = pub::digest_of(*w.book);
    check(overrun && slow.state() == pub::slot_live && sb.digest() == d && qb.digest() == d && qb.resets == 1 && sb.resets == 2,
          "S3 in small: a consumer that stops reading is overrun (its ring of " + std::to_string(pub::ring_capacity) +
              " full) while another keeps up untouched; rejoined, it is reset and replayed, and both have the handler's books");
}

// Note 008's Review: a consumer on another node is counted, and flagged unhealthy.
void remote_consumer() {
    const std::string name = segment("node");
    publishing w;
    if (!w.open(name)) return;
    pub::consumer c;
    std::string why;
    if (!c.attach(name, why)) return;
    if (c.publisher_node() < 0) {  // no NUMA topology to be seen (a container may hide it)
        check(true, "note 008, Review: a consumer on another node: n/a, no NUMA topology visible here");
        return;
    }
    const bool pinned = c.pin_to_publisher_node();
    const bool same = pub::node_of(::sched_getcpu()) == c.publisher_node();
    w.p->tick(2'000'000'000);
    const std::uint64_t before = w.p->pub.remote_consumers();
    // Its slot made to say another node, as a consumer there would report it.
    pub::table_segment t;
    if (!t.attach(name, why)) return;
    t.layout()->slots[c.index()].node.store(c.publisher_node() + 1, std::memory_order_release);
    w.p->tick(4'000'000'000);
    const std::uint64_t after = w.p->pub.remote_consumers();
    mon::values v{};
    v[mon::feed_state_now] = static_cast<std::uint64_t>(feed_state::live);
    v[mon::consumers_remote] = after;
    check(c.publisher_node() >= 0 && pinned && same && before == 0 && after == 1 && (mon::health(v) & mon::consumer_remote) &&
              mon::unhealthy(mon::health(v)),
          "note 008, Review: the publisher's node in the table (" + std::to_string(c.publisher_node()) + "); a consumer pinned to it "
          "reports it and is not counted remote; one reporting another node is, within the second's sweep, and the monitoring "
          "calls the feed unhealthy");
}

// S4: the seqlock, a writer and two readers on other threads.
void seqlock() {
    std::vector<pub::table_entry> e(16);
    std::atomic<bool> done{false};
    std::atomic<std::uint64_t> torn{0}, reads{0};
    std::thread wr([&] {
        for (std::uint64_t k = 1; k <= 2'000'000; ++k) {
            const std::uint64_t v[7] = {k, k, k, k, k, k, k};
            pub::write_entry(e[k % 16], v);
        }
        done = true;
    });
    const auto reader = [&](std::uint64_t seed) {
        std::mt19937_64 rng(seed);
        while (!done.load(std::memory_order_relaxed)) {
            std::uint64_t v[7];
            if (!pub::try_read_entry(e[rng() % 16], v)) continue;
            reads.fetch_add(1, std::memory_order_relaxed);
            for (int i = 1; i < 7; ++i)
                if (v[i] != v[0]) torn.fetch_add(1, std::memory_order_relaxed);
        }
    };
    std::thread r1(reader, 1), r2(reader, 2);
    wr.join();
    r1.join();
    r2.join();
    check(torn == 0 && reads > 0, "S4: the state table's seqlock: 2,000,000 writes over 16 entries, " + std::to_string(reads.load()) +
                                      " reads on two other threads, none torn");
}

}  // namespace

int main() {
    const std::vector<message> all = itchfh::test::stream(80'000);
    another_process(all);
    gap_and_snapshot(all);
    overrun_and_rejoin(all);
    remote_consumer();
    seqlock();
    return itchfh::test::result();
}
