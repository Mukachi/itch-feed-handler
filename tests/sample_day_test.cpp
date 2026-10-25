// The books over Nasdaq's sample day (note 002, S1 S2), label `data`: needs
// data/01302020.events (tools/fetch_day.py, then tools/extract_events), so CI
// does not run it. After every event: the levels it changed and D6; every
// 10 million events and at the end, every level of every book, and the live
// orders counted against the levels. Nothing refused.
//
// Usage: sample_day_test [events file]
#include "check.hpp"
#include "invariants.hpp"

#include <itchfh/event_file.hpp>
#include <itchfh/reference_book.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

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
    itchfh::test::d6_watch d6;
    std::string first_problem;
    std::uint64_t i = 0, checkpoints = 0;
    for (const event& e : f.events()) {
        const applied a = ref->apply(e);
        ++i;
        if (first_problem.empty()) {
            std::string p = a.bad != bad_event::none ? "refused" : "";
            for (int k = 0; p.empty() && k < a.changed; ++k) p = itchfh::test::level_problem(*ref, a.locate, a.level[k].s, a.level[k].price);
            if (p.empty() && e.type != event_type::trading_state) p = d6.after(*ref, a.locate);
            if (!p.empty()) first_problem = "event " + std::to_string(i) + ": " + p;
        }
        if (i % 10'000'000 == 0 && first_problem.empty()) {
            ++checkpoints;
            if (auto p = full_check(*ref); !p.empty()) first_problem = "checkpoint at event " + std::to_string(i) + ": " + p;
        }
    }
    if (first_problem.empty())
        if (auto p = full_check(*ref); !p.empty()) first_problem = "at the end: " + p;
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(i == f.header().count && first_problem.empty(),
          "reference book, " + std::to_string(i) + " events of " + std::string(f.header().source) + ": invariants after every event, " +
              std::to_string(checkpoints) + " full checkpoints and the end, D6, nothing refused" +
              (first_problem.empty() ? "" : " (" + first_problem + ")"));
    check(ref->live_orders() == 0, "no order live at the end of the day (" + std::to_string(ref->live_orders()) + ")");
    std::printf("(%.0f s)\n", s);
    return itchfh::test::result();
}
