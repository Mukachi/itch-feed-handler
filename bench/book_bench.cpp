// The books' cost on Nasdaq's sample day (note 002, S5): the reference book
// and every fast book (each D1 with each D2, default capacities), one after
// another on CPU 2. For each, two passes over the day's events, each with a
// fresh book:
//   timed: every apply() between start() and stop(), unsubtracted, into a
//          histogram per event type (lockfree-ipc's), the first million
//          events discarded as warm-up; beside an empty window ("none") for
//          the clock's own cost;
//   whole day: every event applied untimed, as events a second.
// And the most memory the book held over the day: resident memory read every
// million events in the timed pass (between calls, outside every window), its
// peak less what was resident before the book was built (the fast books'
// tables are allocated and faulted in at construction, so they count whole).
// Before every pass the heap is handed back to the kernel (malloc_trim), so
// each book starts as in a fresh process: without it, a book on standard
// containers reused the heap an earlier book freed, its growth hidden and its
// page faults never taken (note 002, Review). The events are copied first into memory taken after
// the harness binds memory to CPU 2's node, so their placement is known.
// Run JSON through lockfree-ipc's harness. Design: docs/design/002-order-book.md
//
// Usage: book_bench [output.json] [events file]
#include <itchfh/book.hpp>
#include <itchfh/event_file.hpp>
#include <itchfh/memory.hpp>
#include <itchfh/reference_book.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <malloc.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace lfipc::bench;
using namespace itchfh;

constexpr int cpu = 2;
constexpr std::size_t warmup = 1'000'000;
constexpr int types = 7;  // event_type 1..6, and 0 for "all"
const char* const type_names[types] = {"all", "add", "executed", "executed with price", "cancel", "delete", "replace"};

using hist = histogram<10, 40>;

std::size_t resident_kb() {
    std::ifstream f("/proc/self/status");
    std::string k;
    std::size_t v = 0;
    while (f >> k)
        if (k == "VmRSS:") {
            f >> v;
            return v;
        }
    return 0;
}

struct result {
    std::string name, refs, levels;
    std::vector<hist> h = std::vector<hist>(types);
    double seconds = 0, events_per_s = 0, peak_resident_mb = 0;
    std::size_t peak_orders = 0, peak_levels = 0;
    bool clean = true;  // nothing refused in either pass
};

template <class Book, class Make>
result measure(const std::string& name, const char* refs, const char* levels, std::span<const event> ev, Make make) {
    result r;
    r.name = name;
    r.refs = refs;
    r.levels = levels;
    {
        ::malloc_trim(0);
        const std::size_t before = resident_kb();
        std::size_t peak = before;
        auto b = make();
        for (std::size_t i = 0; i < ev.size(); ++i) {
            if (i % 1'000'000 == 0) peak = std::max(peak, resident_kb());
            const ticks t0 = start();
            const applied a = b->apply(ev[i]);
            const ticks t1 = stop();
            r.clean &= a.bad == bad_event::none;
            if (i < warmup) continue;
            const int t = static_cast<int>(ev[i].type);
            if (t >= 1 && t < types) {
                r.h[t].record(t1 - t0);
                r.h[0].record(t1 - t0);
            }
        }
        peak = std::max(peak, resident_kb());
        r.peak_resident_mb = static_cast<double>(peak - before) / 1024.0;
    }
    ::malloc_trim(0);
    auto b = make();
    const auto w0 = std::chrono::steady_clock::now();
    for (const event& e : ev) r.clean &= b->apply(e).bad == bad_event::none;
    const auto w1 = std::chrono::steady_clock::now();
    r.seconds = std::chrono::duration<double>(w1 - w0).count();
    r.events_per_s = static_cast<double>(ev.size()) / r.seconds;
    if constexpr (requires { b->peak_orders(); }) {
        r.peak_orders = b->peak_orders();
        r.peak_levels = b->peak_levels();
    }
    std::printf("%-45s %8.2f M events/s  %7.1f s  %8.0f MB%s\n", name.c_str(), r.events_per_s / 1e6, r.seconds, r.peak_resident_mb,
                r.clean ? "" : "  REFUSED SOME");
    std::fflush(stdout);
    return r;
}

template <class R, class L>
result fast_book(std::span<const event> ev) {
    return measure<book<R, L>>(std::string("fast: refs ") + R::name + ", levels " + L::name, R::name, L::name, ev,
                               [] { return std::make_unique<book<R, L>>(); });
}

}  // namespace

int main(int argc, char** argv) {
    const std::string events_path = argc > 2 ? argv[2] : ITCHFH_DATA_DIR "/01302020.events";
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "book");
    if (run.refused) return run.finish(false, "book_bench");
    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "book_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "book_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    const event_file f(events_path);
    if (!f.ok()) {
        std::fprintf(stderr, "book_bench: %s: not an events file (tools/fetch_day.py, then tools/extract_events)\n", events_path.c_str());
        write_clock_block(run.w, clk);
        return run.finish(false, "book_bench");
    }
    auto copy = zeroed<event>(f.events().size(), true);  // on CPU 2's node: the harness bound memory there
    std::memcpy(copy.get(), f.events().data(), f.events().size_bytes());
    const std::span<const event> ev(copy.get(), f.events().size());

    hist none;
    for (std::size_t i = 0; i < 10'000'000; ++i) {
        const ticks t0 = start();
        const ticks t1 = stop();
        if (i >= warmup) none.record(t1 - t0);
    }

    std::vector<result> results;
    results.push_back(measure<reference_book>("reference book", "reference", "reference", ev,
                                              [] { return std::make_unique<reference_book>(); }));
    results.push_back(fast_book<ref_map, levels_map>(ev));
    results.push_back(fast_book<ref_map, levels_sorted>(ev));
    results.push_back(fast_book<ref_map, levels_window>(ev));
    results.push_back(fast_book<ref_hash, levels_map>(ev));
    results.push_back(fast_book<ref_hash, levels_sorted>(ev));
    results.push_back(fast_book<ref_hash, levels_window>(ev));
    results.push_back(fast_book<ref_direct, levels_map>(ev));
    results.push_back(fast_book<ref_direct, levels_sorted>(ev));
    results.push_back(fast_book<ref_direct, levels_window>(ev));

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto ns = [&](std::optional<ticks> t) { return t ? static_cast<double>(*t) / tpn : -1.0; };
    const auto write_hist = [&](const hist& h) {
        run.w.member("count", h.count());
        run.w.member("p50_ns", ns(h.percentile(50)));
        run.w.member("p99_ns", ns(h.percentile(99)));
        run.w.member("p999_ns", ns(h.percentile(99.9)));
        run.w.member("max_ns", static_cast<double>(h.max()) / tpn);
    };
    bool ok = true;
    run.w.key("book");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("source", std::string(f.header().source));
    run.w.member("events", static_cast<std::uint64_t>(ev.size()));
    run.w.member("warmup_discarded", static_cast<std::uint64_t>(warmup));
    run.w.member("lockfree_ipc_commit", ITCHFH_LFIPC_COMMIT);
    run.w.key("none");
    run.w.begin_object();
    write_hist(none);
    run.w.end_object();
    run.w.key("books");
    run.w.begin_array();
    for (const result& r : results) {
        ok &= r.clean;
        run.w.begin_object();
        run.w.member("name", r.name);
        run.w.member("refs", r.refs);
        run.w.member("levels", r.levels);
        run.w.member("nothing_refused", r.clean);
        run.w.member("events_per_s", r.events_per_s);
        run.w.member("seconds", r.seconds);
        run.w.member("peak_resident_mb", r.peak_resident_mb);
        run.w.member("peak_orders", static_cast<std::uint64_t>(r.peak_orders));
        run.w.member("peak_levels", static_cast<std::uint64_t>(r.peak_levels));
        run.w.key("types");
        run.w.begin_array();
        for (int t = 0; t < types; ++t) {
            run.w.begin_object();
            run.w.member("type", std::string(type_names[t]));
            write_hist(r.h[t]);
            run.w.end_object();
        }
        run.w.end_array();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "book_bench");
}
