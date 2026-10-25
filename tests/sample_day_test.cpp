// The books over Nasdaq's sample day (note 002, S1 S2), label `data`: needs
// data/01302020.events (tools/fetch_day.py, then tools/extract_events), so CI
// does not run it. The reference book and every fast book (each D1 with each
// D2, default capacities) in lockstep. After every event, for each fast book:
// what was refused and changed against the reference's, the changed levels
// and both bests, and the invariants of the changed levels; for the reference,
// its changed levels' invariants and D6. Every 10 million events and at the
// end: every level and queue of every book against the reference's, and the
// reference's full invariants. Nothing refused, nothing live at the close.
//
// Usage: sample_day_test [events file]
#include "check.hpp"
#include "invariants.hpp"

#include <itchfh/book.hpp>
#include <itchfh/event_file.hpp>
#include <itchfh/reference_book.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <tuple>

namespace {

using namespace itchfh;
using itchfh::test::check;

template <class Book>
std::string full_check(const Book& b) {
    std::size_t seen = 0;
    for (const std::uint16_t loc : b.instruments())
        if (auto p = itchfh::test::book_problem(b, loc, &seen); !p.empty()) return "locate " + std::to_string(loc) + ": " + p;
    if (seen != b.live_orders()) return "live orders not all in levels";
    return {};
}

template <class Book>
std::string full_compare(const reference_book& r, const Book& b) {
    if (r.instruments() != b.instruments()) return "different instruments have orders";
    for (const std::uint16_t loc : r.instruments())
        for (const side s : {side::bid, side::ask}) {
            const auto lv = r.levels(loc, s);
            if (lv != b.levels(loc, s)) return "locate " + std::to_string(loc) + ": levels differ";
            for (const level_view& l : lv)
                if (r.queue(loc, s, l.price) != b.queue(loc, s, l.price)) return "locate " + std::to_string(loc) + ": a queue differs";
        }
    return r.live_orders() == b.live_orders() ? std::string{} : "live orders differ";
}

template <class Book>
struct fast {
    std::unique_ptr<Book> b = std::make_unique<Book>();
    std::string problem;
    std::string name;

    void step(const reference_book& r, const event& e, const applied& x, std::uint64_t i) {
        const applied y = b->apply(e);
        if (!problem.empty()) return;
        std::string p;
        if (x.bad != y.bad || x.locate != y.locate || x.changed != y.changed) p = "applied differs";
        for (int k = 0; p.empty() && k < x.changed; ++k) {
            if (x.level[k].s != y.level[k].s || x.level[k].price != y.level[k].price) p = "a different level changed";
            else if (r.at(x.locate, x.level[k].s, x.level[k].price) != b->at(x.locate, x.level[k].s, x.level[k].price)) p = "a changed level differs";
            else p = itchfh::test::level_problem(*b, x.locate, x.level[k].s, x.level[k].price);
        }
        for (const side s : {side::bid, side::ask})
            if (p.empty() && r.best(x.locate, s) != b->best(x.locate, s)) p = "a best differs";
        if (!p.empty()) problem = "event " + std::to_string(i) + ": " + p;
    }
    void compare(const reference_book& r, const std::string& when) {
        if (!problem.empty()) return;
        if (auto p = full_compare(r, *b); !p.empty()) problem = when + ": " + p;
    }
};

template <class R, class L>
fast<book<R, L>> make() {
    fast<book<R, L>> f;
    f.name = std::string("refs ") + R::name + ", levels " + L::name;
    return f;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : ITCHFH_DATA_DIR "/01302020.events";
    const event_file f(path);
    if (!f.ok()) {
        check(false, path + ": not found or not an events file (tools/fetch_day.py, then tools/extract_events)");
        return itchfh::test::result();
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto ref = std::make_unique<reference_book>();
    auto books = std::make_tuple(make<ref_map, levels_map>(), make<ref_map, levels_sorted>(), make<ref_map, levels_window>(),
                                 make<ref_hash, levels_map>(), make<ref_hash, levels_sorted>(), make<ref_hash, levels_window>(),
                                 make<ref_direct, levels_map>(), make<ref_direct, levels_sorted>(), make<ref_direct, levels_window>());
    itchfh::test::d6_watch d6;
    std::string ref_problem;
    std::uint64_t i = 0, checkpoints = 0;
    const auto checkpoint = [&](const std::string& when) {
        if (ref_problem.empty())
            if (auto p = full_check(*ref); !p.empty()) ref_problem = when + ": " + p;
        std::apply([&](auto&... fb) { (fb.compare(*ref, when), ...); }, books);
    };
    for (const event& e : f.events()) {
        const applied a = ref->apply(e);
        ++i;
        if (ref_problem.empty()) {
            std::string p = a.bad != bad_event::none ? "refused" : "";
            for (int k = 0; p.empty() && k < a.changed; ++k) p = itchfh::test::level_problem(*ref, a.locate, a.level[k].s, a.level[k].price);
            if (p.empty() && e.type != event_type::trading_state) p = d6.after(*ref, a.locate);
            if (!p.empty()) ref_problem = "event " + std::to_string(i) + ": " + p;
        }
        std::apply([&](auto&... fb) { (fb.step(*ref, e, a, i), ...); }, books);
        if (i % 10'000'000 == 0) {
            ++checkpoints;
            checkpoint("checkpoint at event " + std::to_string(i));
        }
    }
    checkpoint("at the end");
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(i == f.header().count && ref_problem.empty(),
          "reference book, " + std::to_string(i) + " events of " + std::string(f.header().source) + ": invariants after every event, " +
              std::to_string(checkpoints) + " full checkpoints and the end, D6, nothing refused" +
              (ref_problem.empty() ? "" : " (" + ref_problem + ")"));
    check(ref->live_orders() == 0, "no order live at the end of the day (" + std::to_string(ref->live_orders()) + ")");
    std::apply([&](auto&... fb) {
        (check(fb.problem.empty(), fb.name + ": equal to the reference book after every event and at every checkpoint" +
                                        (fb.problem.empty() ? "" : " (" + fb.problem + ")") + "; peak " +
                                        std::to_string(fb.b->peak_orders()) + " orders, " + std::to_string(fb.b->peak_levels()) + " levels"),
         ...);
    }, books);
    std::printf("(%.0f s)\n", s);
    return itchfh::test::result();
}
