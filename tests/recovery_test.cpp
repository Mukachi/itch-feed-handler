// Tests for include/itchfh/recovery.hpp and snapshot_link.hpp (note 011), in
// process over loopback: our sender (a range lost on purpose: its packets not
// sent, requests for it refused) and its GLIMPSE service on threads, the
// program's loop here (feed_one, the recovery's step, the sockets drained
// between slices) with a consumer of the publisher read in the same loop.
//   S1  the feed lost by spent retries, and by a full hold, is recovered from
//       a snapshot; a late start joins from one; afterwards the books equal a
//       book fed every message, the consumer's books equal the program's, and
//       the state table agrees with the book for every instrument
//   S2  each failure (the connection refused, a service that never answers,
//       the login refused, the spin cut short, the spin corrupt) leaves the
//       feed lost, the live book and the table as they were, and is counted;
//       a retry follows and succeeds
//   S3  the multicast socket is read between slices (no kernel drops here)
// Design: docs/design/011-snapshot-recovery.md
#include "check.hpp"
#include "synthetic_stream.hpp"

#include <itchfh/book.hpp>
#include <itchfh/consumer.hpp>
#include <itchfh/encoder.hpp>
#include <itchfh/feed.hpp>
#include <itchfh/mold_sender.hpp>
#include <itchfh/recovery.hpp>
#include <itchfh/session_store.hpp>
#include <itchfh/snapshot.hpp>
#include <itchfh/snapshot_link.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

const mold::session_id session = {'0', '0', '0', '0', '0', '0', '0', '0', '1', '1'};

struct world {
    std::vector<message> all = itchfh::test::stream(80'000);
    std::unique_ptr<session_store> store;
    snapshot_server reference;  // fed everything
    world() {
        store = std::make_unique<session_store>(session, all.size(), all.size() * 64, false);
        unsigned char b[64];
        for (const message& m : all) {
            store->append(b, encode(m, b));
            reference.on(m);
        }
    }
};

// A hash of every state-table entry: unchanged means the table was left alone.
std::uint64_t table_hash(pub::consumer& c) {
    std::uint64_t h = 1469598103934665603ULL;
    for (std::uint32_t l = 0; l < pub::instruments; ++l) {
        pub::entry_view e;
        c.read(static_cast<std::uint16_t>(l), e);
        for (const std::uint64_t x : {std::uint64_t{e.bid_price}, std::uint64_t{e.bid_shares}, std::uint64_t{e.ask_price}, std::uint64_t{e.ask_shares},
                                      std::uint64_t{e.reliability}})
            h = (h ^ x) * 1099511628211ULL;
    }
    return h;
}
// Every instrument's table entry against the book's best.
bool table_agrees(pub::consumer& c, const default_book& b) {
    for (std::uint32_t i = 0; i < pub::instruments; ++i) {
        const auto l = static_cast<std::uint16_t>(i);
        pub::entry_view e;
        c.read(l, e);
        const auto bb = b.best(l, side::bid), ba = b.best(l, side::ask);
        if (e.bid_price != (bb ? bb->price : 0) || e.bid_shares != (bb ? bb->shares : 0) || e.ask_price != (ba ? ba->price : 0) ||
            e.ask_shares != (ba ? ba->shares : 0))
            return false;
    }
    return true;
}

struct options {
    std::uint16_t port;
    std::uint64_t lose_from = 0, lose_to = 0;
    std::size_t hold_bytes = 16u << 20;
    bool join = false;
    std::int64_t start_late_ns = 0;     // the receiver starts this long after the sender
    bool faults = false;                // S2: refused, silent, rejected, cut, corrupt, then a good spin
    bool lose_when_flowing = false;     // lost by hand on the first datagram, the feed in order (a late join, the bench's way)
};

struct outcome {
    std::string books;
    bool consumer_agrees = false, table_ok = false, untouched = true, ended = false;
    recovery_counts rc;
    feed_counts fc;
    sender_counts sent;
    std::uint64_t requests = 0;
    int faults_met = 0;
};

outcome run(const world& w, const options& o) {
    outcome r;
    std::string why;
    udp::receiver rx;
    udp::receiver::options ro;
    ro.port = o.port;
    udp::sender tx;
    // A late start opens its receiver only after the sender has begun (what was sent before, missed).
    if ((o.start_late_ns == 0 && !rx.open(ro, why)) || !tx.open(ro.group, o.port, "127.0.0.1", static_cast<std::uint16_t>(o.port + 1), why)) {
        r.books = "sockets: " + why;
        return r;
    }

    const std::string name = "/itchfh-rcv-" + std::to_string(::getpid()) + "-" + std::to_string(o.port);
    publisher<default_book> pub;
    if (!pub.open(name, why)) {
        r.books = "publisher: " + why;
        return r;
    }
    recovery_config rcfg;
    rcfg.port = static_cast<std::uint16_t>(o.port + 2);
    rcfg.slice = 4096;  // many slices for a small spin (D4)
    rcfg.retry_ns = 50'000'000;
    rcfg.timeout_ns = o.faults ? 1'000'000'000 : 5'000'000'000;  // S2's silent service: a timeout
    rcfg.join = o.join;
    recovery<default_book> rcv(pub, small_book(), small_handler(), rcfg);
    sequencer_config qc;
    qc.hold_bytes = o.hold_bytes;
    qc.prefault = false;
    qc.huge_pages = false;
    qc.retry_ns = 20'000'000;  // as feed_test, for sanitized runs on CI

    pub::consumer c;
    if (!c.attach(name, why)) {
        r.books = "consumer: " + why;
        return r;
    }
    pub::books cb;

    // The sender and its GLIMPSE service.
    std::atomic<std::uint64_t> sent{1};
    std::atomic<bool> stop{false}, serve{!o.faults};
    glimpse_service svc;
    svc.reset(session);
    const auto open_service = [&] { return svc.open("127.0.0.1", static_cast<std::uint16_t>(o.port + 2), why); };
    if (!o.faults && !open_service()) {  // S2's first fault: nothing listening, the connection refused
        r.books = "service: " + why;
        return r;
    }
    std::thread st([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            if (serve.load(std::memory_order_acquire)) svc.step(*w.store, sent);
            else std::this_thread::sleep_for(std::chrono::milliseconds(1));  // S2: not listening, then listening but never answering
        }
    });
    sender_config sc;
    sc.packets_per_s = 2'000;
    sc.heartbeat_ns = 100'000'000;
    sc.sent = &sent;
    sc.lose_from = o.lose_from;
    sc.lose_to = o.lose_to;
    mold_sender s(*w.store, tx, sc);
    std::thread sender([&] { s.run(stop); });

    if (o.start_late_ns > 0) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(o.start_late_ns));
        if (!rx.open(ro, why)) r.books = "late receiver: " + why;
    }
    rx.set_server("127.0.0.1", static_cast<std::uint16_t>(o.port + 1));
    feed_one<published_on<default_book>> f(rx, rcv.active(), qc);
    rcv.begin(f);
    // S2: the faults in turn, each met once; the book and table at the loss, to compare after each.
    std::uint64_t at_loss_digest = 0, at_loss_table = 0, failures_seen = 0;
    bool lost_seen = false;
    const std::int64_t give_up = monotonic_ns() + 60'000'000'000;
    bool flowing = false;
    while (f.seq().state() != feed_state::ended && monotonic_ns() < give_up) {
        f.poll();
        const std::int64_t now = monotonic_ns();
        if (o.lose_when_flowing && !flowing && f.counts().datagrams > 0) {
            flowing = true;
            f.seq().lose();
        }
        // Lost in order: the recovery waits for packets to be held, or a fast spin (CI's) holds none and proves nothing.
        const bool hold_first = o.lose_when_flowing && f.seq().state() == feed_state::lost && f.seq().held_packets() < 8;
        if (!hold_first && rcv.step(f, now)) recovery<default_book>::drain(f);
        pub::update u;
        while (c.poll(u)) cb.apply(u);
        if (c.overrun()) c.rejoin();
        if (o.faults) {
            if (!lost_seen && f.seq().state() == feed_state::lost && !rcv.recovering()) {
                lost_seen = true;
                at_loss_digest = pub::digest_of(rcv.book());
                at_loss_table = table_hash(c);
            }
            if (rcv.counts().failures != failures_seen && !rcv.recovering()) {
                failures_seen = rcv.counts().failures;
                r.untouched = r.untouched && f.seq().state() == feed_state::lost && pub::digest_of(rcv.book()) == at_loss_digest &&
                              table_hash(c) == at_loss_table;
                ++r.faults_met;
                // The next: listening but silent, the login rejected, the spin cut short, corrupt, then none.
                if (failures_seen == 1) {
                    if (!open_service()) r.books = "service: " + why;
                } else if (failures_seen == 2) {
                    svc.set_fault(glimpse_service::fault::reject);
                    serve.store(true, std::memory_order_release);
                } else if (failures_seen == 3) {
                    svc.set_fault(glimpse_service::fault::cut, 20'000);
                } else if (failures_seen == 4) {
                    svc.set_fault(glimpse_service::fault::corrupt);
                } else {
                    svc.set_fault(glimpse_service::fault::none);
                }
            }
        }
    }
    // The consumer reads to the end.
    for (int i = 0; i < 200'000; ++i) {
        f.poll();
        pub::update u;
        while (c.poll(u)) cb.apply(u);
        if (cb.feed_changes > 0 && c.state() == pub::slot_live && i > 1000) break;
    }
    stop = true;
    sender.join();
    st.join();
    r.ended = f.seq().state() == feed_state::ended;
    r.books = books_differ(w.reference.book(), rcv.book());
    r.consumer_agrees = cb.digest() == pub::digest_of(rcv.book());
    r.table_ok = table_agrees(c, rcv.book());
    r.rc = rcv.counts();
    r.fc = f.counts();
    r.sent = s.counts();
    r.requests = f.seq().counts().requests;
    return r;
}

std::string about(const outcome& o) {
    return std::to_string(o.rc.snapshots) + " snapshots, " + std::to_string(o.rc.failures) + " failed, " + std::to_string(o.sent.lost_on_purpose) +
           " packets lost on purpose, " + std::to_string(o.sent.refused) + " requests refused, " + std::to_string(o.fc.kernel_drops) +
           " kernel drops; last recovery " + std::to_string(o.rc.last_ns / 1000) + " us (" + std::to_string(o.rc.last_bytes) + " bytes, " +
           std::to_string(o.rc.last_held) + " packets held)" + (o.books.empty() ? "" : "; books: " + o.books);
}

void retries_spent(const world& w) {
    options o{26810};
    o.lose_from = 20'000;
    o.lose_to = 20'500;
    const outcome r = run(w, o);
    check(r.ended && r.books.empty() && r.consumer_agrees && r.table_ok && r.rc.snapshots == 1 && r.rc.failures == 0 && r.sent.refused > 0 &&
              r.fc.kernel_drops == 0,
          "S1: retries spent on a range never answered: lost, recovered from a snapshot, the feed to its end; the books equal a book "
          "fed every message, the consumer's equal the program's, the table agrees with the book for every instrument; S3 no kernel drops: " +
              about(r));
}

void hold_full(const world& w) {
    options o{26820};
    o.lose_from = 20'000;
    o.lose_to = 20'500;
    o.hold_bytes = 64 * 2048;  // 64 packets: full well before the retries are spent
    const outcome r = run(w, o);
    check(r.ended && r.books.empty() && r.consumer_agrees && r.table_ok && r.rc.snapshots >= 1,
          "S1: the hold full behind a range never answered: lost, recovered from a snapshot, what came after the hold filled "
          "asked for again; the books equal, the consumer's and the table agree: " +
              about(r));
}

void late_join(const world& w) {
    options o{26830};
    o.join = true;
    o.start_late_ns = 300'000'000;
    const outcome r = run(w, o);
    check(r.ended && r.books.empty() && r.consumer_agrees && r.table_ok && r.rc.snapshots == 1,
          "S1: a late start with join = snapshot: joined from a snapshot, not re-requested from message 1 (" + std::to_string(r.requests) +
              " requests); the books equal, the consumer's and the table agree: " + about(r));
}

// Lost while the feed arrives in order: every later packet held, none applied
// to the books being replaced, and nothing asked for again after the resume.
void lost_in_order(const world& w) {
    options o{26850};
    o.lose_when_flowing = true;
    const outcome r = run(w, o);
    check(r.ended && r.books.empty() && r.consumer_agrees && r.table_ok && r.rc.snapshots == 1 && r.requests == 0 && r.rc.last_held > 0,
          "S1: lost while the feed arrives in order: held, not applied (" + std::to_string(r.rc.last_held) + " packets held), recovered, "
          "nothing re-requested after the resume (" + std::to_string(r.requests) + " requests); the books equal, the consumer's and the "
          "table agree: " + about(r));
}

void failures(const world& w) {
    options o{26840};
    o.lose_from = 20'000;
    o.lose_to = 20'500;
    o.faults = true;
    const outcome r = run(w, o);
    check(r.faults_met == 5 && r.untouched && r.rc.failures == 5 && r.rc.snapshots == 1 && r.ended && r.books.empty() && r.consumer_agrees &&
              r.table_ok,
          "S2: the connection refused, a service that never answers (the timeout), the login refused, the spin cut short, the spin "
          "corrupt: each a failure (" + std::to_string(r.faults_met) +
              " met), the feed left lost, the live book and the state table as they were at the loss; then a good spin, recovered, "
              "the books equal: " +
              about(r));
}

// S1's table, directly: an instrument with orders in the old book and none in
// the rebuilt one must not keep its old entry (live messages after a recovery
// would hide it in the runs above, every instrument there trading again).
void table_rebuilt() {
    const std::string name = "/itchfh-rcv-" + std::to_string(::getpid()) + "-table";
    auto old_book = std::make_unique<default_book>(small_book()), new_book = std::make_unique<default_book>(small_book());
    publisher<default_book> pub(*old_book);
    std::string why;
    pub::consumer c;
    if (!pub.open(name, why) || !c.attach(name, why)) {
        check(false, "S1: the table rebuilt: segments: " + why);
        return;
    }
    handler<default_book, publisher<default_book>> h(*old_book, pub, small_handler());
    pub.watch(h);
    message r{};
    r.type = 'R';
    r.locate = 5;
    r.R.stock = {'T', 'B', 'L', ' ', ' ', ' ', ' ', ' '};
    r.R.round_lot_size = 100;
    h.on(r);
    message a{};
    a.type = 'A';
    a.locate = 5;
    a.A = {77, 'B', 300, r.R.stock, 1'000'000, {' ', ' ', ' ', ' '}};
    h.on(a);
    pub::entry_view before, after;
    c.read(5, before);
    handler<default_book, publisher<default_book>> h2(*new_book, pub, small_handler());
    pub.mute(true);
    pub.rebind(*new_book);
    pub.watch(h2);
    pub.restored();
    c.read(5, after);
    check(before.bid_price == 1'000'000 && before.bid_shares == 300 && after.bid_price == 0 && after.bid_shares == 0,
          "S1: the table rebuilt after a snapshot: an instrument with a bid in the old book and none in the rebuilt one has an empty "
          "entry (was " + std::to_string(before.bid_shares) + " shares at " + std::to_string(before.bid_price) + ")");
}

}  // namespace

int main() {
    table_rebuilt();
    const world w;
    retries_spent(w);
    hold_full(w);
    late_join(w);
    lost_in_order(w);
    failures(w);
    return itchfh::test::result();
}
