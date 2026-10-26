// Tests for include/itchfh/mold.hpp, session_store.hpp and sequencer.hpp.
// Spec: docs/design/005-sequencing-recovery.md (S1 in small, S2, S3, S4): the
// MoldUDP64 layouts; then, through the lossy link, every pattern by hand (one
// packet lost, a burst, duplicates, reordering, a lost request, every request
// lost, a full hold, another session, a gap at the tail, silence, a gap at the
// start, a malformed packet), and random streams at loss rates with requests
// and answers lost too: every message delivered once, in order, as stored.
#include "check.hpp"
#include "lossy_link.hpp"

#include <itchfh/mold.hpp>
#include <itchfh/sequencer.hpp>
#include <itchfh/session_store.hpp>

#include <cstring>
#include <memory>
#include <random>
#include <string>

namespace {

using namespace itchfh;
using itchfh::test::check;
using link_t = itchfh::test::lossy_link<itchfh::test::ignore_messages>;

const mold::session_id session = {'0', '0', '0', '0', '0', '0', '0', '1', '2', '3'};

std::unique_ptr<session_store> synthetic(std::size_t n, std::uint64_t seed = 1) {
    auto s = std::make_unique<session_store>(session, n, n * 50, false);
    std::mt19937_64 rng(seed);
    unsigned char m[64];
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t len = 19 + rng() % 32;
        for (std::size_t b = 0; b < len; ++b) m[b] = static_cast<unsigned char>(rng());
        s->append(m, len);
    }
    return s;
}
sequencer_config small_hold() {
    sequencer_config c;
    c.hold_bytes = 4u << 20;
    c.prefault = false;
    return c;
}
bool all_once(const link_t& l, std::size_t n) {
    return l.seen().delivered == n && l.seen().out_of_order == 0 && l.seen().wrong_bytes == 0;
}
bool went(const link_t& l, std::initializer_list<feed_state> order) {
    std::size_t i = 0;
    for (const auto& [s, next] : l.seen().states)
        if (i < order.size() && s == order.begin()[i]) ++i;
    return i == order.size();
}

void layouts() {
    unsigned char buf[mold::header_size + mold::payload];
    mold::packet_builder b(buf, session, 1000);
    const unsigned char m1[3] = {1, 2, 3}, m2[1] = {9};
    b.add(m1, 3);
    b.add(m2, 1);
    const mold::header h = mold::read_header(buf);
    std::vector<std::pair<std::uint64_t, std::size_t>> got;
    const bool ok = mold::each_message(buf, b.size(), [&](std::uint64_t s, const unsigned char*, std::size_t len) { got.push_back({s, len}); });
    check(ok && h.session == session && h.sequence == 1000 && h.count == 2 && b.size() == 20 + 2 + 3 + 2 + 1 && buf[18] == 0 && buf[19] == 2 &&
              buf[20] == 0 && buf[21] == 3 && got == std::vector<std::pair<std::uint64_t, std::size_t>>{{1000, 3}, {1001, 1}},
          "a downstream packet: 20-byte header (session, first number, count), then each message after its 2-byte length, numbered on");
    check(!mold::each_message(buf, b.size() - 1, [](auto...) {}) && !mold::each_message(buf, b.size() + 1, [](auto...) {}) &&
              !mold::each_message(buf, 19, [](auto...) {}),
          "a packet whose blocks do not fit it exactly is refused");
    unsigned char r[20];
    mold::build_request(r, {session, 77, 5});
    mold::request rr;
    check(mold::read_request(r, 20, rr) && rr.session == session && rr.sequence == 77 && rr.count == 5 && r[18] == 0 && r[19] == 5 &&
              !mold::read_request(r, 19, rr),
          "a request packet: session, first number, count");
    unsigned char big[mold::header_size + mold::payload + 100];
    mold::packet_builder full(big, session, 1);
    unsigned char m[100] = {};
    int fitted = 0;
    while (full.add(m, 100)) ++fitted;
    check(fitted == 13 && full.size() <= mold::header_size + mold::payload, "D7: messages fill a packet up to the 1,400-byte payload, no further");
}

void clean() {
    auto s = synthetic(20'000);
    itchfh::test::ignore_messages ig;
    link_t l(*s, {}, small_hold(), ig);
    l.run(1'000, 2'000'000'000);
    check(all_once(l, 20'000) && l.seen().requests_sent == 0 && l.seq().state() == feed_state::ended && went(l, {feed_state::live, feed_state::ended}),
          "nothing lost: every message once, in order, no request; live, then ended at the end of session");
}

void one_lost() {
    auto s = synthetic(20'000);
    itchfh::test::ignore_messages ig;
    itchfh::test::link_rules r;
    r.drop_first = [](std::uint64_t f) { return f > 5'000 && f < 5'040; };  // one packet
    link_t l(*s, r, small_hold(), ig);
    l.run(1'000, 2'000'000'000);
    check(all_once(l, 20'000) && l.seq().counts().gaps == 1 && l.seen().requests_sent >= 1 && l.seq().counts().held > 0 &&
              went(l, {feed_state::live, feed_state::gapped, feed_state::live, feed_state::ended}),
          "one packet lost: the gap seen at the next packet, asked for, held packets applied after it; every message once, in order");
}

void burst() {
    auto s = synthetic(40'000);
    itchfh::test::ignore_messages ig;
    itchfh::test::link_rules r;
    r.drop_first = [](std::uint64_t f) { return f > 10'000 && f < 12'500; };  // about 50 packets
    link_t l(*s, r, small_hold(), ig);
    l.run(1'000, 2'000'000'000);
    check(all_once(l, 40'000) && l.seen().requests_sent >= 40 && l.seq().counts().retried == 0,
          "a burst of about 50 packets lost: asked for in a window of requests, topped up as answers arrive, no retry needed");
}

// D2 amended: a burst of about 2,000 packets lost at 500,000 packets a second
// (5 us each way). One request in flight recovers a packet a round trip, so
// the hold (2,048 packets) fills and the feed is lost; with the window it is
// recovered, no retry needed. Found by note 007's day over loopback.
void window() {
    auto s = synthetic(400'000);
    bool lost_alone = false, recovered = false;
    std::uint64_t requests = 0;
    for (const int w : {1, 16}) {
        itchfh::test::ignore_messages ig;
        itchfh::test::link_rules r;
        r.drop_first = [](std::uint64_t f) { return f > 100'000 && f < 176'000; };
        sequencer_config c = small_hold();
        c.window = w;
        link_t l(*s, r, c, ig);
        l.run(2'000, 2'000'000'000);
        if (w == 1) lost_alone = l.seq().state() == feed_state::lost && l.seq().counts().hold_full > 0;
        else {
            recovered = all_once(l, 400'000) && l.seq().state() == feed_state::ended && l.seq().counts().retried == 0;
            requests = l.seen().requests_sent;
        }
    }
    check(lost_alone && recovered, "D2 amended: about 2,000 packets lost at 500,000 a second: one request in flight fills the hold (lost); "
                                   "16 in flight, 26 messages each, recover it with no retry (" + std::to_string(requests) + " requests)");
}

void duplicates_and_reordering() {
    auto s = synthetic(30'000);
    itchfh::test::ignore_messages ig;
    itchfh::test::link_rules r;
    r.duplicate = 0.2;
    r.delay = 0.3;
    r.delay_ns = 30'000;
    link_t l(*s, r, small_hold(), ig);
    l.run(1'000, 2'000'000'000);
    check(all_once(l, 30'000) && l.seq().counts().duplicate_messages > 0 && l.seq().counts().held > 0 && l.seq().state() == feed_state::ended,
          "D3: 20% duplicated and 30% delayed (reordered): duplicates dropped and counted, early packets held; every message once, in order");
}

void lost_request() {
    auto s = synthetic(20'000);
    itchfh::test::ignore_messages ig;
    itchfh::test::link_rules r;
    r.drop_first = [](std::uint64_t f) { return f > 5'000 && f < 5'040; };
    r.drop_request = [](std::uint64_t k) { return k == 1; };
    link_t l(*s, r, small_hold(), ig);
    l.run(1'000, 2'000'000'000);
    check(all_once(l, 20'000) && l.seq().counts().retried >= 1, "D2: the first request lost: asked again after the 1 ms timeout");
}

void every_request_lost() {
    auto s = synthetic(20'000);
    itchfh::test::ignore_messages ig;
    itchfh::test::link_rules r;
    std::uint64_t dropped_from = 0, dropped_to = 0;
    r.drop_first = [&](std::uint64_t f) {
        if (f > 5'000 && dropped_from == 0) dropped_from = f;
        return f == dropped_from;
    };
    r.drop_requests = 1.0;
    link_t l(*s, r, small_hold(), ig);
    l.run(1'000, 50'000'000);
    const bool lost = l.seq().state() == feed_state::lost && l.seq().counts().retried == 5;
    const std::uint64_t held_before = l.seq().held_packets();
    // A stand-in snapshot (note 006's): it covers the messages up to the end of the lost packet.
    unsigned char buf[mold::header_size + mold::payload];
    s->fill(dropped_from, ~0ull, buf);
    dropped_to = dropped_from + mold::read_header(buf).count;
    l.seq().resume(dropped_to, l.now());
    l.advance(l.now() + 10'000'000);
    check(lost && held_before > 0 && l.seq().held_packets() == 0 && l.seen().delivered == 20'000 - (dropped_to - dropped_from) &&
              l.seen().wrong_bytes == 0 && went(l, {feed_state::gapped, feed_state::lost}) && l.seq().state() != feed_state::lost,
          "D4: every request lost: after 5 retries, lost; resumed from a stand-in snapshot, the hold applied from its number");
}

void hold_full() {
    auto s = synthetic(200'000);
    itchfh::test::ignore_messages ig;
    itchfh::test::link_rules r;
    r.drop_first = [](std::uint64_t f) { return f > 1'000 && f < 1'040; };
    r.drop_requests = 1.0;
    sequencer_config c = small_hold();
    c.hold_bytes = 64 * 2048;  // 64 packets
    c.retries = 1000;          // so the hold fills before the retries are spent
    link_t l(*s, r, c, ig);
    l.run(1'000, 2'000'000'000);
    check(l.seq().state() == feed_state::lost && l.seq().counts().hold_full > 0 && l.seq().held_packets() == 64,
          "D4: a full hold: lost, the packets beyond it counted, not kept");
}

void other_session_and_malformed() {
    auto s = synthetic(1'000);
    itchfh::test::ignore_messages ig;
    link_t l(*s, {}, small_hold(), ig);
    unsigned char buf[mold::header_size + mold::payload];
    const std::size_t n = s->fill(1, 10, buf);
    l.inject(buf, n, 0, 100'000);
    mold::session_id other = session;
    other[9] = '9';
    unsigned char o[mold::header_size + mold::payload];
    mold::packet_builder b(o, other, 11);
    const unsigned char m[5] = {};
    b.add(m, 5);
    l.inject(o, b.size(), 200'000, 300'000);
    l.inject(buf, n - 1, 400'000, 500'000);
    check(l.seq().counts().other_session == 1 && l.seq().counts().malformed == 1 && l.seq().next_expected() == 11 && l.seq().state() == feed_state::live,
          "D5: a packet of another session refused and counted; a malformed one counted; neither changes the sequence");
}

void tail_and_silence() {
    auto s = synthetic(5'000);
    itchfh::test::ignore_messages ig;
    itchfh::test::link_rules r;
    std::uint64_t last_first = 0;
    {
        unsigned char buf[mold::header_size + mold::payload];
        for (std::uint64_t f = 1; f <= s->count(); f += mold::read_header(buf).count) {
            s->fill(f, ~0ull, buf);
            last_first = f;
        }
    }
    r.drop_first = [&](std::uint64_t f) { return f == last_first; };
    link_t l(*s, r, small_hold(), ig);
    l.run(1'000, 2'000'000'000);
    check(all_once(l, 5'000) && l.seq().counts().gaps == 1 && l.seq().state() == feed_state::ended,
          "D5: the last packet lost: the heartbeat's next number shows the gap at the tail, asked for, filled; ended");

    auto s2 = synthetic(100);
    link_t q(*s2, {}, small_hold(), ig);
    unsigned char buf[mold::header_size + mold::payload];
    const std::size_t n1 = s2->fill(1, 10, buf);
    q.inject(buf, n1, 0, 3'000'000'000);
    const bool stale = q.seq().state() == feed_state::stale;
    const std::size_t n2 = s2->fill(11, 10, buf);
    q.inject(buf, n2, 3'000'000'001, 3'100'000'000);
    check(stale && q.seq().state() == feed_state::live && went(q, {feed_state::live, feed_state::stale, feed_state::live}),
          "D5: 2 s with nothing: stale; the next packet, nothing missed: live");
}

void gap_at_the_start() {
    auto s = synthetic(10'000);
    itchfh::test::ignore_messages ig;
    itchfh::test::link_rules r;
    r.drop_first = [](std::uint64_t f) { return f == 1; };
    link_t l(*s, r, small_hold(), ig);
    l.run(1'000, 2'000'000'000);
    check(all_once(l, 10'000) && l.seq().counts().gaps == 1,
          "the first packet lost: a first packet beyond number 1 is a gap, not a new beginning; asked for and filled");
}

void random_streams() {
    for (const double loss : {0.001, 0.01, 0.05}) {
        bool all = true;
        std::uint64_t gaps = 0, requests = 0;
        for (std::uint64_t seed = 1; seed <= 3; ++seed) {
            auto s = synthetic(100'000, seed);
            itchfh::test::ignore_messages ig;
            itchfh::test::link_rules r;
            r.drop = loss;
            r.duplicate = loss;
            r.delay = 0.05;
            r.drop_requests = 0.1;
            r.seed = seed;
            // The agreed hold (64 MB) and a packet every 10 us (100,000 a second): the
            // hold must cover the retries at the packet rate (D4).
            sequencer_config c;
            c.prefault = false;
            link_t l(*s, r, c, ig);
            l.run(10'000, 30'000'000'000);
            all &= all_once(l, 100'000) && l.seq().state() == feed_state::ended;
            gaps += l.seq().counts().gaps;
            requests += l.seen().requests_sent;
        }
        char pct[16];
        std::snprintf(pct, sizeof(pct), "%g", loss * 100);
        check(all && gaps > 0, "S1 in small: " + std::string(pct) + "% of packets lost and duplicated, 5% delayed, 10% of requests lost, " +
                                   "100,000 packets a second, 3 seeds of 100,000 messages: every message once, in order, as stored; " + std::to_string(gaps) + " gaps, " +
                                   std::to_string(requests) + " requests");
    }
}

}  // namespace

int main() {
    layouts();
    clean();
    one_lost();
    burst();
    window();
    duplicates_and_reordering();
    lost_request();
    every_request_lost();
    hold_full();
    other_session_and_malformed();
    tail_and_silence();
    gap_at_the_start();
    random_streams();
    return itchfh::test::result();
}
