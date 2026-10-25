// Tests for include/itchfh/book.hpp. Spec: docs/design/002-order-book.md
// (S1 in small, S2, S3): every combination of D1 (ref_tables.hpp) and D2
// (levels.hpp) against the reference book, in lockstep over random event
// streams with bad events in them; after every event, what was refused and
// changed, the changed levels and both bests, and the invariants; every 500
// events, every level and queue of every book. Then D4: capacity reached is
// refused as `full` and marks the book, and a book that ran out recovers as
// orders leave.
#include "check.hpp"
#include "invariants.hpp"

#include <itchfh/book.hpp>
#include <itchfh/reference_book.hpp>

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace itchfh;
using itchfh::test::check;

// Small capacities, faulted in as used: the window and arena logic exercised
// at edges (few windows, short arrays), not a big machine's defaults.
book_config small() {
    book_config c;
    c.orders = 20'000;
    c.levels = 20'000;
    c.ref_slots = 65'536;
    c.ref_base = 0;
    c.ref_span = 1 << 16;  // C: half the numbers direct, half through B
    c.level_arena = 1 << 18;
    c.price_window = 128;
    c.windows = 6;  // fewer than the instruments: some sides have no window
    c.prefault = false;
    return c;
}

// A stream with prices near a drifting middle per instrument (sub-dollar, at
// and off the cent grid, and far away), numbers reused and unknown on purpose.
struct stream {
    std::mt19937_64 rng{2026};
    std::vector<std::uint64_t> live;
    std::uint64_t next_ref = 1;
    std::uint32_t mid[8] = {500, 9'800, 10'000, 250'000, 1'000'000, 5'000'000, 120'000, 64'000'000};

    std::uint32_t price(std::uint16_t loc) {
        const std::uint64_t r = rng() % 100;
        std::uint32_t m = mid[loc % 8];
        if (rng() % 50 == 0) mid[loc % 8] = m = m + static_cast<std::uint32_t>(rng() % 2000) - std::min<std::uint32_t>(m - 1, 1000);
        const std::uint32_t tick = m < 10'000 ? 1 : 100;
        std::int64_t p = std::int64_t{m} + (static_cast<std::int64_t>(rng() % 41) - 20) * tick;
        if (r < 3) p += static_cast<std::int64_t>(rng() % 3) * 37;            // off the cent grid
        if (r >= 3 && r < 6) p += (static_cast<std::int64_t>(rng() % 2) * 2 - 1) * 400 * tick;  // far from the middle
        return static_cast<std::uint32_t>(std::max<std::int64_t>(p, 1));
    }
    event next() {
        event e{};
        e.locate = static_cast<std::uint16_t>(1 + rng() % 7);
        const std::uint64_t r = rng() % 100;
        const auto pick = [&]() -> std::uint64_t {
            if (live.empty() || rng() % 40 == 0) return 900'000'000 + rng() % 1000;  // unknown
            return live[rng() % live.size()];
        };
        if (r < 45 || live.size() < 50) {
            e.type = event_type::add;
            e.ref = rng() % 60 == 0 && !live.empty() ? live[rng() % live.size()] : (next_ref += 1 + rng() % 3) + (rng() % 2 ? 70'000 : 0);
            e.side = rng() % 200 == 0 ? 'X' : (rng() % 2 ? 'B' : 'S');
            e.shares = rng() % 150 == 0 ? 0 : static_cast<std::uint32_t>(1 + rng() % 500);
            e.price = price(e.locate);
            live.push_back(e.ref);
        } else if (r < 60) {
            e.type = rng() % 3 ? event_type::executed : (rng() % 2 ? event_type::executed_price : event_type::cancel);
            e.ref = pick();
            e.shares = static_cast<std::uint32_t>(1 + rng() % (rng() % 30 == 0 ? 2000 : 200));
            e.price = price(e.locate);
        } else if (r < 85) {
            e.type = event_type::remove;
            e.ref = pick();
        } else if (r < 98) {
            e.type = event_type::replace;
            e.ref = pick();
            e.new_ref = rng() % 60 == 0 && !live.empty() ? live[rng() % live.size()] : (next_ref += 1);
            e.shares = static_cast<std::uint32_t>(rng() % 120 == 0 ? 0 : 1 + rng() % 500);
            e.price = price(e.locate);
            live.push_back(e.new_ref);
        } else {
            e.type = event_type::trading_state;
            e.side = "HPQT"[rng() % 4];
        }
        if (live.size() > 4000) live.erase(live.begin(), live.begin() + 1000);
        return e;
    }
};

template <class Book>
std::string same(const reference_book& r, const Book& b, std::uint16_t loc) {
    for (const side s : {side::bid, side::ask}) {
        if (r.levels(loc, s) != b.levels(loc, s)) return "levels differ";
        for (const level_view& l : r.levels(loc, s))
            if (r.queue(loc, s, l.price) != b.queue(loc, s, l.price)) return "a queue differs";
    }
    return {};
}

template <class Refs, class Levels>
void against_reference(const char* name) {
    reference_book r;
    auto b = std::make_unique<book<Refs, Levels>>(small());
    stream g;
    std::string problem;
    std::uint64_t refused = 0;
    constexpr int n = 60'000;
    for (int i = 1; i <= n && problem.empty(); ++i) {
        const event e = g.next();
        const applied x = r.apply(e), y = b->apply(e);
        if (x.bad != y.bad || x.locate != y.locate || x.changed != y.changed) problem = "applied differs";
        refused += x.bad != bad_event::none;
        for (int k = 0; problem.empty() && k < x.changed; ++k) {
            if (x.level[k].s != y.level[k].s || x.level[k].price != y.level[k].price) problem = "a different level changed";
            else if (r.at(x.locate, x.level[k].s, x.level[k].price) != b->at(x.locate, x.level[k].s, x.level[k].price)) problem = "a changed level differs";
            else problem = itchfh::test::level_problem(*b, x.locate, x.level[k].s, x.level[k].price);
        }
        for (const side s : {side::bid, side::ask})
            if (problem.empty() && r.best(e.locate, s) != b->best(e.locate, s)) problem = "a best differs";
        if (problem.empty() && i % 500 == 0)
            for (std::uint16_t loc = 1; loc <= 7 && problem.empty(); ++loc) {
                problem = same(r, *b, loc);
                if (problem.empty()) problem = itchfh::test::book_problem(*b, loc);
            }
        if (!problem.empty()) problem = "event " + std::to_string(i) + ": " + problem;
    }
    bool counts = r.live_orders() == b->live_orders();
    for (int k = 0; k < bad_event_kinds; ++k) counts = counts && r.bad_count(static_cast<bad_event>(k)) == b->bad_count(static_cast<bad_event>(k));
    check(problem.empty() && counts && refused > 0,
          std::string(name) + ": 60,000 random events in lockstep with the reference book, " + std::to_string(refused) +
              " of them refused alike; changed levels, bests, every level and queue every 500, invariants" +
              (problem.empty() ? "" : " (" + problem + ")"));
}

template <class Refs, class Levels>
void capacity(const char* name) {
    book_config c = small();
    c.orders = 3;
    c.levels = 2;
    book<Refs, Levels> b(c);
    const auto add = [&](std::uint64_t ref, std::uint32_t price) {
        event e{};
        e.type = event_type::add;
        e.ref = ref;
        e.locate = 1;
        e.side = 'B';
        e.shares = 10;
        e.price = price;
        return b.apply(e).bad;
    };
    const bool levels_full = add(1, 100) == bad_event::none && add(2, 200) == bad_event::none && add(3, 300) == bad_event::full;
    const bool orders_full = add(4, 200) == bad_event::none && add(5, 100) == bad_event::full;
    event d{};
    d.type = event_type::remove;
    d.ref = 1;
    d.locate = 1;
    b.apply(d);
    const bool after = add(6, 300) == bad_event::none && !b.find(3) && b.find(6) && b.live_orders() == 3;
    check(levels_full && orders_full && b.bad_count(bad_event::full) == 2 && b.unreliable(1) && after &&
              itchfh::test::book_problem(b, 1).empty(),
          std::string(name) + ": D4: out of levels or orders refused as full, the book marked; room again once an order leaves");
}

template <class Refs>
void each_levels(const char* r) {
    against_reference<Refs, levels_map>((std::string(r) + " + A") .c_str());
    against_reference<Refs, levels_sorted>((std::string(r) + " + B").c_str());
    against_reference<Refs, levels_window>((std::string(r) + " + C").c_str());
    capacity<Refs, levels_window>((std::string(r) + " + C").c_str());
}

}  // namespace

int main() {
    each_levels<ref_map>("refs A");
    each_levels<ref_hash>("refs B");
    each_levels<ref_direct>("refs C");
    capacity<ref_hash, levels_sorted>("refs B + B");
    capacity<ref_map, levels_map>("refs A + A");
    return itchfh::test::result();
}
