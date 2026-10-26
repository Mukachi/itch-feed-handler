// Tests for include/itchfh/soup.hpp, encoder.hpp and snapshot.hpp. Spec:
// docs/design/006-snapshots-replay.md (S1, S2 and S5 in small, S3): the
// SoupBinTCP layouts and GLIMPSE's End of Snapshot (20 ASCII digits); the
// encoder against the reference decoder, byte for byte; then a server
// following a random stream, a client rebuilt from its spin at several points:
// the same books (every level and queue, time priority included) and the same
// directory and restorable status as the server's; the spin's F orders with
// their MPIDs; the client fed the rest of the stream equal to the server at the
// end; and D5's marks set, then cleared by live messages. And S4: through the
// lossy link with every request lost, the sequencer gives up; a snapshot from
// the server rebuilds a fresh client, the sequencer resumes from its N, and
// the feed is live again with the books the reference's.
#include "check.hpp"
#include "handler_checks.hpp"
#include "lossy_link.hpp"
#include "synthetic_stream.hpp"

#include <itchfh/book.hpp>
#include <itchfh/encoder.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/reference_decoder.hpp>
#include <itchfh/snapshot.hpp>
#include <itchfh/soup.hpp>

#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace itchfh;
using itchfh::test::check;

const soup::session_id session = {'G', 'L', 'I', 'M', 'P', 'S', 'E', ' ', ' ', '1'};

void layouts() {
    std::vector<unsigned char> s;
    soup::login_accepted(s, session, 12345);
    check(s.size() == 33 && s[0] == 0 && s[1] == 31 && s[2] == 'A' && std::memcmp(s.data() + 3, session.data(), 10) == 0 &&
              std::string(s.begin() + 13, s.end()) == "               12345",
          "Login Accepted: length 31, type A, session, the next number as 20 ASCII digits left-padded with spaces");
    std::vector<unsigned char> l;
    soup::login_request(l, "user", "pw", session, 1);
    check(l.size() == 49 && l[2] == 'L' && std::string(l.begin() + 3, l.begin() + 9) == "user  " &&
              std::string(l.begin() + 9, l.begin() + 19) == "pw        " && std::string(l.begin() + 29, l.end()) == "                   1",
          "Login Request: username 6, password 10, session 10, sequence number 20 ASCII");
    unsigned char g[21];
    soup::end_of_snapshot(g, 987654321);
    std::uint64_t n = 0;
    unsigned char bad[21];
    std::memcpy(bad, g, 21);
    bad[15] = 'x';
    check(g[0] == 'G' && std::string(g + 1, g + 21) == "           987654321" && soup::read_end_of_snapshot(g, 21, n) && n == 987654321 &&
              !soup::read_end_of_snapshot(bad, 21, n) && !soup::read_end_of_snapshot(g, 20, n),
          "End of Snapshot: type G, the sequence number as 20 ASCII digits; anything else refused");
    std::vector<unsigned char> st;
    soup::debug(st, "hello");
    const unsigned char m[3] = {1, 2, 3};
    soup::sequenced(st, m, 3);
    soup::heartbeat(st);
    soup::end_of_session(st);
    soup::reader r;
    std::string seen;
    for (const unsigned char b : st) r.feed(&b, 1, [&](char t, const unsigned char*, std::size_t len) { seen += t + std::to_string(len); });
    check(seen == "S3H0Z0", "the stream read a byte at a time: Debug ignored, Sequenced Data, Heartbeat, End of Session whole");
}

void encoder_round_trip() {
    std::mt19937_64 rng(6);
    const char* types = "SRHYLVWKJhAFECXDUPQBINO";
    std::size_t bad = 0, n = 0;
    unsigned char in[64], out[64];
    for (int i = 0; i < 1'000'000; ++i) {
        const char t = types[rng() % 23];
        const std::size_t len = message_length(t);
        for (std::size_t b = 0; b < len; ++b) in[b] = static_cast<unsigned char>(rng());
        in[0] = static_cast<unsigned char>(t);
        message m;
        if (decode_reference(in, len, m) != decode_status::ok) continue;
        ++n;
        if (encode(m, out) != len || std::memcmp(in, out, len) != 0) ++bad;
    }
    check(bad == 0 && n == 1'000'000, "the encoder: a million random messages of every type, decoded and encoded again, byte for byte");
}

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

using client_handler = handler<default_book, itchfh::test::recording_sink>;

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
// The restorable part of an instrument's status.
std::string restorable(const instrument_status& s) {
    return std::string(1, s.trading_state ? s.trading_state : '_') + itchfh::test::text(s.trading_reason) + (s.reg_sho ? s.reg_sho : '_') +
           (s.retail_interest ? s.retail_interest : '_') + (s.operational_halt[0] ? s.operational_halt[0] : '_') +
           (s.operational_halt[1] ? s.operational_halt[1] : '_') + (s.operational_halt[2] ? s.operational_halt[2] : '_');
}

void round_trip_and_catch_up() {
    const std::vector<message> all = itchfh::test::stream(60'000);
    bool all_same = true, all_caught_up = true, marks = true, attributed = true;
    std::string problem;
    for (const std::size_t at : {std::size_t{20}, std::size_t{5'000}, std::size_t{30'000}, std::size_t{59'999}}) {
        snapshot_server server;
        for (std::size_t i = 0; i < at; ++i) server.on(all[i]);
        const std::vector<unsigned char> spin = server.spin(session, 999);
        // The spin's F orders: each with the MPID of the order's own F.
        soup::reader rd;
        std::size_t f_orders = 0;
        rd.feed(spin.data(), spin.size(), [&](char t, const unsigned char* body, std::size_t len) {
            message m;
            if (t != 'S' || decode_reference(body, len, m) != decode_status::ok || m.type != 'F') return;
            ++f_orders;
            for (const message& o : all)
                if ((o.type == 'F' && o.A.ref == m.A.ref) && o.A.attribution != m.A.attribution) attributed = false;
        });
        auto b = std::make_unique<default_book>(small_book());
        itchfh::test::recording_sink sink;
        sink.keep = false;
        auto h = std::make_unique<client_handler>(*b, sink, small_handler());
        snapshot_client<client_handler> c(*h);
        for (std::size_t i = 0; i < spin.size(); i += 777) c.feed(spin.data() + i, std::min<std::size_t>(777, spin.size() - i));  // in pieces
        std::string p = books_differ(server.book(), *b);
        for (std::uint16_t l = 1; l <= 8 && p.empty(); ++l) {
            if (!h->entry(l) || !server.handler().entry(l) || itchfh::test::text(h->entry(l)->stock) != itchfh::test::text(server.handler().entry(l)->stock))
                p = "directory differs";
            else if (restorable(h->status_of(l)) != restorable(server.handler().status_of(l)))
                p = "status differs for " + std::to_string(l);
        }
        const bool resumed = c.logged_in() && c.ended() && c.resume() == at + 1 && c.undecoded() == 0;
        marks &= h->status_of(1).not_since_snapshot == snapshot_all;
        all_same &= p.empty() && resumed;
        if (!p.empty() && problem.empty()) problem = "at " + std::to_string(at) + ": " + p;
        // Catch up from N: the rest of the stream into both.
        for (std::size_t i = at; i < all.size(); ++i) {
            server.on(all[i]);
            h->on(all[i]);
        }
        const std::string q = books_differ(server.book(), *b);
        all_caught_up &= q.empty();
        if (!q.empty() && problem.empty()) problem = "caught up from " + std::to_string(at) + ": " + q;
        // D5: live messages clear the marks they set; trades stay marked.
        bool noii_cleared = false;
        for (std::uint16_t l = 1; l <= 8; ++l) noii_cleared |= !(h->status_of(l).not_since_snapshot & snapshot_noii);
        const bool followed = all.size() - at > 1'000;  // an imbalance can only clear its mark if messages follow
        marks &= (!followed || noii_cleared) && (h->status_of(1).not_since_snapshot & snapshot_trades);
        (void)f_orders;
    }
    check(all_same, "S1 in small: a client rebuilt from the spin at 4 points has the server's books (every level and queue), directory and "
                    "restorable status; logged in, End of Snapshot's N, End of Session" + (problem.empty() ? "" : " (" + problem + ")"));
    check(attributed, "D1: every F in the spin carries the MPID of that order's own attributed Add");
    check(all_caught_up, "S2 in small: each rebuilt client fed the rest of the stream from N equals the server at the end");
    check(marks, "S5: after a rebuild, the unrestorable fields marked; an imbalance clears its mark; trades stay marked");
}

// What the sequencer delivers to: the current client handler, decoding first;
// switched to the rebuilt one after a snapshot.
struct current_client {
    client_handler* h = nullptr;
    void deliver(std::uint64_t, const unsigned char* p, std::size_t n) {
        message m;
        if (decode_simd_unpadded(p, n, m) == decode_status::ok) h->on(m);
    }
};

void through_the_lossy_link() {
    const std::vector<message> all = itchfh::test::stream(60'000);
    const mold::session_id mold_session = {'0', '0', '0', '0', '0', '0', '0', '0', '0', '6'};
    session_store store(mold_session, all.size(), all.size() * 64, false);
    unsigned char buf[64];
    for (const message& m : all) store.append(buf, encode(m, buf));
    // The packet lost: the one holding message 20,000; every request lost.
    std::uint64_t lost_from = 0, lost_to = 0;
    {
        unsigned char pk[mold::header_size + mold::payload];
        for (std::uint64_t f = 1; f <= store.count();) {
            store.fill(f, ~0ull, pk);
            const std::uint64_t c = mold::read_header(pk).count;
            if (f <= 20'000 && 20'000 < f + c) lost_from = f, lost_to = f + c;
            f += c;
        }
    }
    itchfh::test::link_rules rules;
    rules.drop_first = [&](std::uint64_t f) { return f == lost_from; };
    rules.drop_requests = 1.0;
    auto b1 = std::make_unique<default_book>(small_book());
    itchfh::test::recording_sink sink;
    sink.keep = false;
    auto h1 = std::make_unique<client_handler>(*b1, sink, small_handler());
    current_client cur{h1.get()};
    sequencer_config sc;
    sc.prefault = false;
    itchfh::test::lossy_link<current_client> link(store, rules, sc, cur);
    link.run(10'000, 200'000'000);
    const bool gave_up = link.seq().state() == feed_state::lost;
    // The snapshot, at the end of the lost packet: the server has followed the feed to there.
    snapshot_server server;
    for (std::uint64_t i = 0; i + 1 < lost_to; ++i) server.on(all[i]);
    const std::vector<unsigned char> spin = server.spin(session, 0);
    auto b2 = std::make_unique<default_book>(small_book());
    auto h2 = std::make_unique<client_handler>(*b2, sink, small_handler());
    snapshot_client<client_handler> c(*h2);
    c.feed(spin.data(), spin.size());
    cur.h = h2.get();
    link.seq().resume(*c.resume(), link.now());
    link.advance(link.now() + 10'000'000);
    snapshot_server everything;  // the reference: every message, in order
    for (const message& m : all) everything.on(m);
    const std::string diff = books_differ(everything.book(), *b2);
    check(gave_up && c.resume() == lost_to && link.seq().state() != feed_state::lost && link.seq().state() != feed_state::gapped &&
              link.seq().held_packets() == 0 && diff.empty(),
          "S4: one packet and every request lost: the sequencer gives up; a snapshot at the lost packet's end rebuilds a fresh client, the "
          "sequencer resumes from its N with what it held, and the books equal the reference's" + (diff.empty() ? "" : " (" + diff + ")"));
}

}  // namespace

int main() {
    layouts();
    encoder_round_trip();
    round_trip_and_catch_up();
    through_the_lossy_link();
    return itchfh::test::result();
}
