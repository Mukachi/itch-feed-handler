// The event handler's cost on Nasdaq's sample day (note 003, S6): the handler
// on the default book (note 002), beside the reference handler on the reference
// book, one after the other on CPU 2. The day is decoded first (the reference
// decoder; decoding is note 004's) into memory taken after the harness binds
// memory to CPU 2's node, so what is timed is handling alone. For each, two
// passes over the day's messages, each fresh, the heap handed back before each:
//   timed: every on() between start() and stop(), unsubtracted, into a
//          histogram per message type (lockfree-ipc's), the first million
//          messages discarded as warm-up; the most resident memory added,
//          read every million messages between calls;
//   whole day: every message handled untimed, as messages a second.
// The sink counts its calls and does nothing else. Run JSON through
// lockfree-ipc's harness. Design: docs/design/003-events-directory-status.md
//
// Usage: handler_bench [output.json] [day.gz]
#include <itchfh/book.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/memory.hpp>
#include <itchfh/reference_book.hpp>
#include <itchfh/reference_decoder.hpp>
#include <itchfh/reference_handler.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <malloc.h>

namespace {

using namespace lfipc::bench;
using namespace itchfh;

constexpr int cpu = 2;
constexpr std::size_t warmup = 1'000'000;
constexpr const char* type_order = "SRHYLVWKJhAFECXDUPQBINO";  // index + 1; 0 is "all"
constexpr int types = 24;

using hist = histogram<10, 40>;

int type_index(char t) {
    const char* p = std::strchr(type_order, t);
    return p && t ? static_cast<int>(p - type_order) + 1 : -1;
}

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

struct counting_sink {
    std::uint64_t calls = 0, refused = 0;
    void book(const applied& a) {
        ++calls;
        refused += a.bad != bad_event::none;
    }
    void trade(const trade_report&) { ++calls; }
    void trade_broken(const trade_report&) { ++calls; }
    void status(std::uint16_t, status_field) { ++calls; }
    void imbalance(std::uint16_t, const itchfh::imbalance&) { ++calls; }
    void market(const itchfh::message&) { ++calls; }
    void directory(std::uint16_t, const stock_directory&) { ++calls; }
};

struct result {
    std::string name;
    std::vector<hist> h = std::vector<hist>(types);
    double seconds = 0, messages_per_s = 0, peak_resident_mb = 0;
    bool clean = true;  // nothing refused by the book, no edge case counted
};

template <class Setup>
result measure(const std::string& name, std::span<const itchfh::message> ms) {
    result r;
    r.name = name;
    {
        ::malloc_trim(0);
        const std::size_t before = resident_kb();
        std::size_t peak = before;
        Setup s;
        for (std::size_t i = 0; i < ms.size(); ++i) {
            if (i % 1'000'000 == 0) peak = std::max(peak, resident_kb());
            const ticks t0 = start();
            s.h->on(ms[i]);
            const ticks t1 = stop();
            if (i < warmup) continue;
            const int t = type_index(ms[i].type);
            if (t > 0) r.h[t].record(t1 - t0);
            r.h[0].record(t1 - t0);
        }
        peak = std::max(peak, resident_kb());
        r.peak_resident_mb = static_cast<double>(peak - before) / 1024.0;
        r.clean &= s.sink.refused == 0 && s.h->counts().unknown_type == 0 && s.h->counts().no_directory == 0 &&
                   s.h->counts().unknown_break == 0 && s.h->counts().table_full == 0;
    }
    ::malloc_trim(0);
    Setup s;
    const auto w0 = std::chrono::steady_clock::now();
    for (const itchfh::message& m : ms) s.h->on(m);
    const auto w1 = std::chrono::steady_clock::now();
    r.seconds = std::chrono::duration<double>(w1 - w0).count();
    r.messages_per_s = static_cast<double>(ms.size()) / r.seconds;
    r.clean &= s.sink.refused == 0;
    std::printf("%-44s %8.2f M messages/s  %7.1f s  %8.0f MB%s\n", name.c_str(), r.messages_per_s / 1e6, r.seconds, r.peak_resident_mb,
                r.clean ? "" : "  REFUSED OR COUNTED SOME");
    std::fflush(stdout);
    return r;
}

struct fast_setup {
    std::unique_ptr<default_book> b = std::make_unique<default_book>();
    counting_sink sink;
    std::unique_ptr<handler<default_book, counting_sink>> h = std::make_unique<handler<default_book, counting_sink>>(*b, sink);
};
struct reference_setup {
    std::unique_ptr<reference_book> b = std::make_unique<reference_book>();
    counting_sink sink;
    std::unique_ptr<reference_handler<reference_book, counting_sink>> h =
        std::make_unique<reference_handler<reference_book, counting_sink>>(*b, sink);
};

}  // namespace

int main(int argc, char** argv) {
    const std::string day = argc > 2 ? argv[2] : ITCHFH_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "handler");
    if (run.refused) return run.finish(false, "handler_bench");
    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !lfipc::bench::detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "handler_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "handler_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;

    // The day, decoded, in memory on CPU 2's node (the harness bound memory there).
    constexpr std::size_t capacity = 450'000'000;  // the sample day has 423,285,709
    auto decoded = zeroed<itchfh::message>(capacity, true);
    std::size_t n = 0, undecoded = 0;
    {
        std::FILE* in = ::popen(("gzip -dc '" + day + "'").c_str(), "r");
        std::vector<unsigned char> buf(1 << 16);
        for (; in && n < capacity;) {
            unsigned char lb[2];
            if (std::fread(lb, 1, 2, in) != 2) break;
            const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
            if (len == 0 || std::fread(buf.data(), 1, len, in) != len) break;
            if (decode_reference(buf.data(), len, decoded[n]) == decode_status::ok) ++n;
            else ++undecoded;
        }
        if (in) ::pclose(in);
    }
    if (n == 0) {
        std::fprintf(stderr, "handler_bench: %s: nothing decoded (tools/fetch_day.py fetches it)\n", day.c_str());
        write_clock_block(run.w, clk);
        return run.finish(false, "handler_bench");
    }
    const std::span<const itchfh::message> ms(decoded.get(), n);

    hist none;
    for (std::size_t i = 0; i < 10'000'000; ++i) {
        const ticks t0 = start();
        const ticks t1 = stop();
        if (i >= warmup) none.record(t1 - t0);
    }
    std::vector<std::uint64_t> counts(types, 0);
    for (const itchfh::message& m : ms)
        if (const int t = type_index(m.type); t > 0) ++counts[t];

    std::vector<result> results;
    results.push_back(measure<reference_setup>("reference handler, reference book", ms));
    results.push_back(measure<fast_setup>("handler, default book", ms));

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
    bool ok = undecoded == 0;
    run.w.key("handler");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("source", day.substr(day.find_last_of('/') + 1));
    run.w.member("messages", static_cast<std::uint64_t>(n));
    run.w.member("undecoded", static_cast<std::uint64_t>(undecoded));
    run.w.member("warmup_discarded", static_cast<std::uint64_t>(warmup));
    run.w.member("lockfree_ipc_commit", ITCHFH_LFIPC_COMMIT);
    run.w.key("none");
    run.w.begin_object();
    write_hist(none);
    run.w.end_object();
    run.w.key("setups");
    run.w.begin_array();
    for (const result& r : results) {
        ok &= r.clean;
        run.w.begin_object();
        run.w.member("name", r.name);
        run.w.member("clean", r.clean);
        run.w.member("messages_per_s", r.messages_per_s);
        run.w.member("seconds", r.seconds);
        run.w.member("peak_resident_mb", r.peak_resident_mb);
        run.w.key("types");
        run.w.begin_array();
        for (int t = 0; t < types; ++t) {
            if (t > 0 && counts[t] == 0) continue;
            run.w.begin_object();
            run.w.member("type", t == 0 ? std::string("all") : std::string(1, type_order[t - 1]));
            run.w.member("in_day", t == 0 ? static_cast<std::uint64_t>(n) : counts[t]);
            write_hist(r.h[t]);
            run.w.end_object();
        }
        run.w.end_array();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "handler_bench");
}
