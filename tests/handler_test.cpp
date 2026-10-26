// Tests for include/itchfh/handler.hpp and reference_handler.hpp. Spec:
// docs/design/003-events-directory-status.md (S1, S3, S4): every message type
// by hand, with what it changes and what the sink is told, in both handlers;
// trades once per match number, non-printable executions left out, broken
// trades taken back out; the directory's edge cases counted. Then the handler
// (on the fast book) and the reference handler (on the reference book) in
// lockstep over random streams of every type: the same sink calls, and the
// same state for the instrument each message touched.
#include "check.hpp"
#include "handler_checks.hpp"

#include <itchfh/book.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/reference_book.hpp>
#include <itchfh/reference_handler.hpp>

#include <cstring>
#include <memory>
#include <random>
#include <string>

namespace {

using namespace itchfh;
using itchfh::test::check;
using itchfh::test::recording_sink;

book_config small_book() {
    book_config c;
    c.orders = 20'000;
    c.levels = 20'000;
    c.ref_slots = 65'536;
    c.ref_span = 1 << 16;
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

symbol sym(const char* s) {
    symbol x;
    x.fill(' ');
    std::memcpy(x.data(), s, std::min<std::size_t>(std::strlen(s), 8));
    return x;
}
message msg(char type, std::uint16_t locate, std::uint64_t ts = 1000) {
    message m{};
    m.type = type;
    m.locate = locate;
    m.timestamp_ns = ts;
    return m;
}
message directory_entry(std::uint16_t l, const char* stock) {
    message m = msg('R', l);
    m.R.stock = sym(stock);
    m.R.market_category = 'Q';
    m.R.round_lot_size = 100;
    return m;
}
message add(std::uint16_t l, std::uint64_t ref, char side, std::uint32_t shares, std::uint32_t price) {
    message m = msg('A', l);
    m.A.ref = ref;
    m.A.side = side;
    m.A.shares = shares;
    m.A.price = price;
    m.A.stock = sym("X");
    return m;
}
message executed(std::uint16_t l, std::uint64_t ref, std::uint32_t shares, std::uint64_t match) {
    message m = msg('E', l);
    m.E.ref = ref;
    m.E.shares = shares;
    m.E.match = match;
    return m;
}
message executed_price(std::uint16_t l, std::uint64_t ref, std::uint32_t shares, std::uint64_t match, char printable, std::uint32_t price) {
    message m = executed(l, ref, shares, match);
    m.type = 'C';
    m.E.printable = printable;
    m.E.price = price;
    return m;
}

// Both handlers, fed alike.
struct pair_of_handlers {
    std::unique_ptr<default_book> fast_book = std::make_unique<default_book>(small_book());
    reference_book ref_book;
    recording_sink fast_sink, ref_sink;
    handler<default_book, recording_sink> fast{*fast_book, fast_sink, small_handler()};
    reference_handler<reference_book, recording_sink> ref{ref_book, ref_sink};

    void on(const message& m) {
        fast.on(m);
        ref.on(m);
    }
    bool agree(std::uint16_t l) const {
        return fast_sink.calls == ref_sink.calls && itchfh::test::instrument(fast, l) == itchfh::test::instrument(ref, l) &&
               fast.counts() == ref.counts() && itchfh::test::describe(fast.market()) == itchfh::test::describe(ref.market());
    }
};

void market_wide() {
    pair_of_handlers h;
    for (const char c : std::string("OSQMEC")) {
        message m = msg('S', 0);
        m.S.event_code = c;
        h.on(m);
    }
    const bool in_order = h.fast.market().phases == 6 && h.fast.counts().phase_out_of_order == 0;
    message again = msg('S', 0);
    again.S.event_code = 'Q';
    h.on(again);
    check(in_order && h.fast.counts().phase_out_of_order == 1 && h.agree(0),
          "S: the System Events O S Q M E C in order; one out of order counted; the sink told of each");
    message v = msg('V', 0);
    v.V = {100, 200, 300};
    message w = msg('W', 0);
    w.W.breached_level = '1';
    h.on(v);
    h.on(w);
    check(h.fast.market().mwcb.level2 == 200 && h.fast.market().mwcb_breached == '1' && h.agree(0), "V, W: circuit-breaker levels and breach");
}

void directory_cases() {
    pair_of_handlers h;
    h.on(directory_entry(5, "ELAT"));
    h.on(add(7, 1, 'B', 100, 10000));
    const bool counted_missing = h.fast.counts().no_directory == 1;
    message second = directory_entry(5, "ELAT");
    second.R.round_lot_size = 10;
    h.on(second);
    check(h.fast.counts().second_directory == 1 && h.fast.entry(5)->round_lot_size == 10 && h.fast.locate_of(sym("ELAT")) == std::uint16_t{5} &&
              h.agree(5),
          "R: a second entry for a locate replaces the first and is counted; the symbol finds the locate");
    check(counted_missing && h.fast_book->find(1) && h.agree(7), "a message for a locate with no entry is applied, and counted (S4)");
    message k = msg('K', 0);
    k.K.stock = sym("ELAT");
    k.K.release_time_s = 36000;
    k.K.release_qualifier = 'A';
    k.K.ipo_price = 150000;
    h.on(k);
    message k2 = k;
    k2.K.stock = sym("NOSUCH");
    h.on(k2);
    check(h.fast.status_of(5).ipo.ipo_price == 150000 && h.fast.counts().no_directory == 2 && h.agree(5),
          "K: its locate is 0, so it is found by symbol; an unknown symbol is counted");
}

void status_cases() {
    pair_of_handlers h;
    h.on(directory_entry(3, "ABC"));
    message ha = msg('H', 3);
    ha.H.trading_state = 'H';
    std::memcpy(ha.H.reason.data(), "LUDP", 4);
    h.on(ha);
    check(h.fast.status_of(3).trading_state == 'H' && h.fast_book->trading_state(3) == 'H' && h.ref_book.trading_state(3) == 'H' && h.agree(3),
          "H: the trading state and reason, and the book's state for D6");
    message y = msg('Y', 3);
    y.Y.action = '1';
    message l = msg('L', 3);
    std::memcpy(l.L.participant.data(), "GSCO", 4);
    l.L.primary_market_maker = 'Y';
    l.L.market_maker_mode = 'N';
    l.L.participant_state = 'A';
    message j = msg('J', 3);
    j.J = {sym("ABC"), 1000, 1100, 900, 1};
    message oh = msg('h', 3);
    oh.h.market_code = 'B';
    oh.h.action = 'H';
    message bad = oh;
    bad.h.market_code = 'Z';
    message n = msg('N', 3);
    n.N.interest_flag = 'A';
    message o = msg('O', 3);
    o.O.open_eligibility = 'Y';
    o.O.near_price = 5000;
    message i = msg('I', 3);
    i.I.paired_shares = 1000;
    i.I.imbalance_shares = 50;
    i.I.direction = 'B';
    for (const message& x : {y, l, j, oh, bad, n, o, i}) h.on(x);
    const instrument_status s = h.fast.status_of(3);
    const participant_view* p = h.fast.participant_of(3, l.L.participant);
    check(s.reg_sho == '1' && s.luld.upper_price == 1100 && s.operational_halt[1] == 'H' && s.operational_halt[0] == 0 &&
              s.retail_interest == 'A' && s.dlcr.near_price == 5000 && s.noii.imbalance_shares == 50 && p && p->primary_market_maker == 'Y' &&
              h.fast.counts().bad_field == 1 && h.agree(3),
          "Y, L, J, h, N, O, I: each sets its field; an operational halt for an unknown market code is counted");
    h.on(msg('Z', 3));
    check(h.fast.counts().unknown_type == 1 && h.agree(3), "an unknown message type is counted, never fatal (S1)");
}

void trade_cases() {
    pair_of_handlers h;
    h.on(directory_entry(9, "TRD"));
    h.on(add(9, 1, 'B', 1000, 50000));
    h.on(add(9, 2, 'S', 1000, 50100));
    h.on(executed(9, 1, 100, 11));                     // printable, at the order's price
    h.on(executed_price(9, 1, 50, 12, 'Y', 49900));     // printable, at its own price
    h.on(executed_price(9, 2, 70, 13, 'N', 50050));     // not printable: left out
    h.on(executed_price(9, 2, 30, 14, 'Y', 50000));     // one trade, both sides reported:
    h.on(executed(9, 1, 30, 14));                       //   the same match number, counted once
    message p = msg('P', 9);
    p.P = {0, 'B', 200, sym("TRD"), 50020, 15};
    message q = msg('Q', 9);
    q.Q = {5000, sym("TRD"), 50010, 16, 'O'};
    message q0 = msg('Q', 9);
    q0.Q = {0, sym("TRD"), 0, 17, 'C'};
    h.on(p);
    h.on(q);
    h.on(q0);
    const instrument_trades t = h.fast.trades_of(9);
    check(t.volume == 100 + 50 + 30 + 200 + 5000 && t.trades == 5 && t.last_price == 50010 && t.last_match == 16 && h.agree(9),
          "S3: E at the order's price, C at its own, P and Q counted; a non-printable C and a zero-share Q left out; a match number "
          "seen twice counted once");
    std::size_t trade_calls = 0;
    for (const auto& c : h.fast_sink.calls) trade_calls += c.starts_with("trade ");
    check(trade_calls == 5 && h.fast_book->find(1)->shares == 1000 - 100 - 50 - 30 && h.agree(9),
          "the sink told of each trade once; the book applied every execution, both sides of match 14 included");
    message b = msg('B', 9);
    b.B.match = 15;
    h.on(b);
    const instrument_trades after = h.fast.trades_of(9);
    h.on(b);
    message bu = b;
    bu.B.match = 999;
    h.on(bu);
    message bn = b;
    bn.B.match = 13;  // the non-printable execution: never counted, nothing to break
    h.on(bn);
    check(after.volume == t.volume - 200 && after.trades == 4 && h.fast.trades_of(9) == after && h.fast.counts().unknown_break == 1 && h.agree(9),
          "S3: a Broken Trade takes its trade back out, once; an unknown match number is counted; an uncounted one changes nothing");
}

// Random streams of every type, both handlers in lockstep.
void lockstep() {
    pair_of_handlers h;
    std::mt19937_64 rng(2026);
    std::vector<std::uint64_t> live;
    std::uint64_t next_ref = 1, next_match = 1;
    const char* types = "SRHYLVWKJhAFECXDUPQBINOZ";
    std::string problem;
    constexpr int n = 50'000;
    for (int i = 0; i < n && problem.empty(); ++i) {
        const std::uint16_t l = static_cast<std::uint16_t>(rng() % 12);
        const char t = i < 12 ? 'R' : types[rng() % 24];
        message m = msg(t, l, static_cast<std::uint64_t>(i));
        const std::uint64_t r = rng();
        const auto ref = [&] { return !live.empty() && r % 20 ? live[r % live.size()] : 9'000'000 + r % 100; };
        const auto match = [&] { return r % 4 == 0 && next_match > 1 ? 1 + r % next_match : next_match++; };
        switch (t) {
            case 'S': m.S.event_code = "OSQMEC"[r % 6]; m.locate = 0; break;
            case 'R': m = directory_entry(l, ("S" + std::to_string(r % 15)).c_str()); m.timestamp_ns = static_cast<std::uint64_t>(i); break;
            case 'H': m.H.trading_state = "HPQT"[r % 4]; break;
            case 'Y': m.Y.action = "012"[r % 3]; break;
            case 'L': std::memcpy(m.L.participant.data(), r % 2 ? "GSCO" : "MSCO", 4); m.L.participant_state = "AEWSD"[r % 5]; break;
            case 'V': m.V = {r % 1000, r % 900, r % 800}; m.locate = 0; break;
            case 'W': m.W.breached_level = "123"[r % 3]; m.locate = 0; break;
            case 'K': m.K.stock = sym(("S" + std::to_string(r % 20)).c_str()); m.K.ipo_price = static_cast<std::uint32_t>(r % 99999); m.locate = 0; break;
            case 'J': m.J.upper_price = static_cast<std::uint32_t>(r % 5000); break;
            case 'h': m.h.market_code = "QBXZ"[r % 4]; m.h.action = r % 2 ? 'H' : 'T'; break;
            case 'A': case 'F':
                m.A.ref = r % 30 == 0 && !live.empty() ? live[r % live.size()] : next_ref++;
                m.A.side = r % 2 ? 'B' : 'S';
                m.A.shares = static_cast<std::uint32_t>(1 + r % 500);
                m.A.price = static_cast<std::uint32_t>(10000 + (r >> 8) % 50 * 100);
                live.push_back(m.A.ref);
                break;
            case 'E': case 'C':
                m.E.ref = ref();
                m.E.shares = static_cast<std::uint32_t>(1 + r % 120);
                m.E.match = match();
                m.E.printable = r % 5 ? 'Y' : 'N';
                m.E.price = static_cast<std::uint32_t>(10000 + (r >> 8) % 50 * 100);
                break;
            case 'X': m.X.ref = ref(); m.X.shares = static_cast<std::uint32_t>(1 + r % 100); break;
            case 'D': m.D.ref = ref(); break;
            case 'U':
                m.U.old_ref = ref();
                m.U.new_ref = next_ref++;
                m.U.shares = static_cast<std::uint32_t>(1 + r % 500);
                m.U.price = static_cast<std::uint32_t>(10000 + (r >> 8) % 50 * 100);
                live.push_back(m.U.new_ref);
                break;
            case 'P': m.P = {0, 'B', static_cast<std::uint32_t>(1 + r % 300), sym("P"), static_cast<std::uint32_t>(r % 20000), match()}; break;
            case 'Q': m.Q = {r % 3 ? 1 + r % 10000 : 0, sym("Q"), static_cast<std::uint32_t>(r % 20000), match(), 'O'}; break;
            case 'B': m.B.match = r % 3 ? 1 + r % next_match : 777'777'777; break;
            case 'I': m.I.paired_shares = r % 10000; m.I.direction = "BSNOP"[r % 5]; break;
            case 'N': m.N.interest_flag = "BSAN"[r % 4]; break;
            case 'O': m.O.near_price = static_cast<std::uint32_t>(r % 50000); break;
            default: break;
        }
        if (live.size() > 3000) live.erase(live.begin(), live.begin() + 1000);
        h.on(m);
        if (!h.agree(m.locate)) problem = "message " + std::to_string(i) + " (" + t + ")";
        h.fast_sink.calls.clear();
        h.ref_sink.calls.clear();
    }
    bool every = problem.empty();
    for (std::uint16_t l = 0; l < 12 && every; ++l) every = h.agree(l);
    check(every && h.fast.counts().unknown_type > 0 && h.fast.counts().unknown_break > 0 && h.fast.counts().no_directory > 0,
          "50,000 random messages of every type, the handler (fast book) and the reference handler (reference book) in lockstep: "
          "the same sink calls, counts and state after each, every instrument at the end" + (problem.empty() ? "" : " (" + problem + ")"));
}

}  // namespace

int main() {
    market_wide();
    directory_cases();
    status_cases();
    trade_cases();
    lockstep();
    return itchfh::test::result();
}
