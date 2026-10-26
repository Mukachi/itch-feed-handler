// The decoders' cost on Nasdaq's sample day (note 004, S5): the reference
// decoder and every alternative (A switch, B table, C SIMD padded and
// unpadded), one after another on CPU 2. The day is held uncompressed in
// memory taken after the harness binds memory to CPU 2's node, as the file
// has it: each message after its 2-byte length, so the next message's bytes
// follow each one (the padded contract, D2; 32 zero bytes after the last).
// Every alternative reads it in place, in the day's order (the real mix and
// sequence of types, for the branch predictor). For each, three passes:
//   timed:      each decode between start() and stop(), unsubtracted, into a
//               histogram per type (lockfree-ipc's), the first million
//               messages discarded as warm-up; branch mispredictions per
//               message counted over the pass;
//   whole day:  every message decoded untimed, as messages a second;
//   end to end: every message decoded and handled (note 003's handler on the
//               default book, a sink that counts), untimed, as messages a
//               second, and each decode-and-handle timed in a further pass.
// Run JSON through lockfree-ipc's harness. Design: docs/design/004-decoder.md
//
// Usage: decoder_bench [output.json] [day.gz]
#include "branch_misses.hpp"

#include <itchfh/book.hpp>
#include <itchfh/decoder.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/memory.hpp>
#include <itchfh/reference_decoder.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <malloc.h>

namespace {

using namespace lfipc::bench;
namespace fh = itchfh;

constexpr int cpu = 2;
constexpr std::size_t warmup = 1'000'000;
constexpr const char* type_order = "SRHYLVWKJhAFECXDUPQBINO";
constexpr int types = 24;  // index + 1; 0 is "all"

using hist = histogram<10, 40>;
using decode_fn = fh::decode_status (*)(const unsigned char*, std::size_t, fh::message&);

int type_index(unsigned char t) {
    const char* p = t ? std::strchr(type_order, t) : nullptr;
    return p ? static_cast<int>(p - type_order) + 1 : -1;
}

struct counting_sink {
    std::uint64_t calls = 0;
    void book(const fh::applied&) { ++calls; }
    void trade(const fh::trade_report&) { ++calls; }
    void trade_broken(const fh::trade_report&) { ++calls; }
    void status(std::uint16_t, fh::status_field) { ++calls; }
    void imbalance(std::uint16_t, const fh::imbalance&) { ++calls; }
    void market(const fh::message&) { ++calls; }
    void directory(std::uint16_t, const fh::stock_directory&) { ++calls; }
};

struct day {
    const unsigned char* bytes;
    std::size_t size, messages;
};

struct result {
    std::string name;
    std::vector<hist> h = std::vector<hist>(types), e2e = std::vector<hist>(1);
    double mispredictions_per_message = -1, messages_per_s = 0, e2e_messages_per_s = 0;
    std::uint64_t refused = 0;
};

// Calls f(p, n) for every message of the day, in order.
template <class F>
inline void each(const day& d, F&& f) {
    for (std::size_t at = 0; at + 2 <= d.size;) {
        const std::size_t n = std::size_t{d.bytes[at]} << 8 | d.bytes[at + 1];
        f(d.bytes + at + 2, n);
        at += 2 + n;
    }
}

result measure(const char* name, decode_fn decode, const day& d, const branch_misses& bm) {
    result r;
    r.name = name;
    fh::message m;
    {  // timed, mispredictions counted
        std::size_t i = 0;
        const std::uint64_t m0 = bm.ok() ? bm.read() : 0;
        each(d, [&](const unsigned char* p, std::size_t n) {
            const ticks t0 = start();
            const fh::decode_status s = decode(p, n, m);
            const ticks t1 = stop();
            r.refused += s != fh::decode_status::ok;
            if (i++ < warmup) return;
            if (const int t = type_index(p[0]); t > 0) r.h[t].record(t1 - t0);
            r.h[0].record(t1 - t0);
        });
        if (bm.ok()) r.mispredictions_per_message = static_cast<double>(bm.read() - m0) / static_cast<double>(d.messages);
    }
    {  // whole day, untimed
        std::uint64_t sum = 0;
        const auto w0 = std::chrono::steady_clock::now();
        each(d, [&](const unsigned char* p, std::size_t n) {
            decode(p, n, m);
            sum += m.timestamp_ns;  // the result used, so the work is kept
        });
        const auto w1 = std::chrono::steady_clock::now();
        asm volatile("" : : "r"(sum));
        r.messages_per_s = static_cast<double>(d.messages) / std::chrono::duration<double>(w1 - w0).count();
    }
    {  // end to end, untimed, then timed
        for (int pass = 0; pass < 2; ++pass) {
            ::malloc_trim(0);
            auto b = std::make_unique<fh::default_book>();
            counting_sink sink;
            auto h = std::make_unique<fh::handler<fh::default_book, counting_sink>>(*b, sink);
            std::size_t i = 0;
            const auto w0 = std::chrono::steady_clock::now();
            if (pass == 0) {
                each(d, [&](const unsigned char* p, std::size_t n) {
                    if (decode(p, n, m) == fh::decode_status::ok) h->on(m);
                });
                const auto w1 = std::chrono::steady_clock::now();
                r.e2e_messages_per_s = static_cast<double>(d.messages) / std::chrono::duration<double>(w1 - w0).count();
            } else {
                each(d, [&](const unsigned char* p, std::size_t n) {
                    const ticks t0 = start();
                    if (decode(p, n, m) == fh::decode_status::ok) h->on(m);
                    const ticks t1 = stop();
                    if (i++ >= warmup) r.e2e[0].record(t1 - t0);
                });
            }
        }
    }
    std::printf("%-22s %7.1f M/s decode  %6.2f M/s with the handler  %5.2f mispredictions/msg%s\n", name, r.messages_per_s / 1e6,
                r.e2e_messages_per_s / 1e6, r.mispredictions_per_message, r.refused ? "  REFUSED SOME" : "");
    std::fflush(stdout);
    return r;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 2 ? argv[2] : ITCHFH_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "decoder");
    if (run.refused) return run.finish(false, "decoder_bench");
    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !lfipc::bench::detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "decoder_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "decoder_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;

    // The day, uncompressed, on CPU 2's node (the harness bound memory there).
    constexpr std::size_t capacity = std::size_t{14} << 30;  // the sample day is 12.95 GB
    auto bytes = fh::zeroed<unsigned char>(capacity + 32, true);
    std::size_t size = 0, messages = 0;
    if (std::FILE* in = ::popen(("gzip -dc '" + path + "'").c_str(), "r")) {
        for (std::size_t got; size < capacity && (got = std::fread(bytes.get() + size, 1, std::min<std::size_t>(1 << 24, capacity - size), in)) > 0;)
            size += got;
        ::pclose(in);
    }
    for (std::size_t at = 0; at + 2 <= size; ++messages) at += 2 + (std::size_t{bytes[at]} << 8 | bytes[at + 1]);
    if (messages == 0) {
        std::fprintf(stderr, "decoder_bench: %s: nothing read (tools/fetch_day.py fetches it)\n", path.c_str());
        write_clock_block(run.w, clk);
        return run.finish(false, "decoder_bench");
    }
    const day d{bytes.get(), size, messages};
    std::vector<std::uint64_t> counts(types, 0);
    each(d, [&](const unsigned char* p, std::size_t) {
        if (const int t = type_index(p[0]); t > 0) ++counts[t];
    });

    hist none;
    for (std::size_t i = 0; i < 10'000'000; ++i) {
        const ticks t0 = start();
        const ticks t1 = stop();
        if (i >= warmup) none.record(t1 - t0);
    }
    const branch_misses bm;
    if (!bm.ok()) std::fprintf(stderr, "decoder_bench: branch mispredictions not counted (perf_event_paranoid above 2?)\n");

    std::vector<result> results;
    results.push_back(measure("reference", fh::decode_reference, d, bm));
    results.push_back(measure("A: switch", fh::decode_switch, d, bm));
    results.push_back(measure("B: table", fh::decode_table, d, bm));
    results.push_back(measure("C: SIMD, padded", fh::decode_simd, d, bm));
    results.push_back(measure("C: SIMD, unpadded", fh::decode_simd_unpadded, d, bm));

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
    run.w.key("decoder");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("source", path.substr(path.find_last_of('/') + 1));
    run.w.member("messages", static_cast<std::uint64_t>(messages));
    run.w.member("warmup_discarded", static_cast<std::uint64_t>(warmup));
    run.w.member("counter", std::string("branch mispredictions, user space, this thread"));
    run.w.member("lockfree_ipc_commit", ITCHFH_LFIPC_COMMIT);
    run.w.key("none");
    run.w.begin_object();
    write_hist(none);
    run.w.end_object();
    run.w.key("decoders");
    run.w.begin_array();
    for (const result& r : results) {
        ok &= r.refused == 0;
        run.w.begin_object();
        run.w.member("name", r.name);
        run.w.member("refused", r.refused);
        run.w.member("mispredictions_per_message", r.mispredictions_per_message);
        run.w.member("messages_per_s", r.messages_per_s);
        run.w.member("end_to_end_messages_per_s", r.e2e_messages_per_s);
        run.w.key("end_to_end");
        run.w.begin_object();
        write_hist(r.e2e[0]);
        run.w.end_object();
        run.w.key("types");
        run.w.begin_array();
        for (int t = 0; t < types; ++t) {
            if (t > 0 && counts[t] == 0) continue;
            run.w.begin_object();
            run.w.member("type", t == 0 ? std::string("all") : std::string(1, type_order[t - 1]));
            run.w.member("in_day", t == 0 ? static_cast<std::uint64_t>(messages) : counts[t]);
            write_hist(r.h[t]);
            run.w.end_object();
        }
        run.w.end_array();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "decoder_bench");
}
