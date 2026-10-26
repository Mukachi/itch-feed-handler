// Snapshots on Nasdaq's sample day (note 006, S6), on CPU 2: the snapshot
// server follows the whole day (the reference handler and book, D1); at every
// 50 million messages and at the day's peak of live orders (found by a first
// pass), the spin built (timed: the server's cost), then applied by a client
// (timed: the SoupBinTCP stream read, every message decoded and handled into a
// fresh default book; building that book, allocated and faulted in, is not
// counted), with the spin's size in messages and bytes. The time to apply is
// what a recovery costs before the held feed is caught up (note 005's backlog
// figures). Run JSON through lockfree-ipc's harness.
// Design: docs/design/006-snapshots-replay.md
//
// Usage: snapshot_bench [output.json] [day.gz]
#include <itchfh/book.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/reference_decoder.hpp>
#include <itchfh/snapshot.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <malloc.h>

namespace {

using namespace lfipc::bench;
namespace fh = itchfh;

constexpr int cpu = 2;

struct quiet {
    void book(const fh::applied&) {}
    void trade(const fh::trade_report&) {}
    void trade_broken(const fh::trade_report&) {}
    void status(std::uint16_t, fh::status_field) {}
    void imbalance(std::uint16_t, const fh::imbalance&) {}
    void market(const fh::message&) {}
    void directory(std::uint16_t, const fh::stock_directory&) {}
};
using client_handler = fh::handler<fh::default_book, quiet>;

struct point {
    std::uint64_t at = 0, messages = 0, bytes = 0, live_orders = 0;
    bool peak = false, ok = false;
    double build_ms = 0, apply_ms = 0;
};

template <class F>
bool each_message(const std::string& path, F&& f) {
    std::FILE* in = ::popen(("gzip -dc '" + path + "'").c_str(), "r");
    if (!in) return false;
    unsigned char buf[1 << 16];
    std::uint64_t n = 0;
    for (;;) {
        unsigned char lb[2];
        if (std::fread(lb, 1, 2, in) != 2) break;
        const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
        if (len == 0 || std::fread(buf, 1, len, in) != len) break;
        fh::message m;
        if (fh::decode_reference(buf, len, m) == fh::decode_status::ok) f(++n, m);
    }
    ::pclose(in);
    return n > 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 2 ? argv[2] : ITCHFH_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "snapshot");
    if (run.refused) return run.finish(false, "snapshot_bench");
    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !lfipc::bench::detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "snapshot_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "snapshot_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;

    std::uint64_t peak_at = 0;
    {
        auto b = std::make_unique<fh::default_book>();
        quiet q;
        auto h = std::make_unique<client_handler>(*b, q);
        std::size_t peak = 0;
        if (!each_message(path, [&](std::uint64_t n, const fh::message& m) {
                h->on(m);
                if (b->live_orders() > peak) {
                    peak = b->live_orders();
                    peak_at = n + 1;
                }
            })) {
            std::fprintf(stderr, "snapshot_bench: %s: nothing read (tools/fetch_day.py fetches it)\n", path.c_str());
            write_clock_block(run.w, clk);
            return run.finish(false, "snapshot_bench");
        }
    }
    const fh::soup::session_id session = {'G', 'L', 'I', 'M', 'P', 'S', 'E', ' ', ' ', '1'};
    auto server = std::make_unique<fh::snapshot_server>();
    std::vector<point> points;
    each_message(path, [&](std::uint64_t n, const fh::message& m) {
        if (n == peak_at || (n > 1 && (n - 1) % 50'000'000 == 0)) {
            point pt;
            pt.at = n;
            pt.peak = n == peak_at;
            pt.live_orders = server->book().live_orders();
            const auto b0 = std::chrono::steady_clock::now();
            const std::vector<unsigned char> spin = server->spin(session, 0);
            const auto b1 = std::chrono::steady_clock::now();
            ::malloc_trim(0);
            auto b = std::make_unique<fh::default_book>();
            quiet q;
            auto h = std::make_unique<client_handler>(*b, q);
            fh::snapshot_client<client_handler> c(*h);
            const auto a0 = std::chrono::steady_clock::now();
            c.feed(spin.data(), spin.size());
            const auto a1 = std::chrono::steady_clock::now();
            pt.messages = c.messages();
            pt.bytes = spin.size();
            pt.ok = c.resume() == n && c.undecoded() == 0 && b->live_orders() == pt.live_orders;
            pt.build_ms = std::chrono::duration<double, std::milli>(b1 - b0).count();
            pt.apply_ms = std::chrono::duration<double, std::milli>(a1 - a0).count();
            std::printf("at %11llu%s: %9llu messages, %5.0f MB, %8llu live orders: build %7.1f ms, apply %7.1f ms%s\n",
                        (unsigned long long)n, pt.peak ? " (peak)" : "       ", (unsigned long long)pt.messages, pt.bytes / 1e6,
                        (unsigned long long)pt.live_orders, pt.build_ms, pt.apply_ms, pt.ok ? "" : "  NOT AS THE SERVER'S");
            std::fflush(stdout);
            points.push_back(pt);
        }
        server->on(m);
    });

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    bool ok = !points.empty();
    run.w.key("snapshot");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("source", path.substr(path.find_last_of('/') + 1));
    run.w.member("lockfree_ipc_commit", ITCHFH_LFIPC_COMMIT);
    run.w.key("points");
    run.w.begin_array();
    for (const point& p : points) {
        ok &= p.ok;
        run.w.begin_object();
        run.w.member("at", p.at);
        run.w.member("peak", p.peak);
        run.w.member("live_orders", p.live_orders);
        run.w.member("messages", p.messages);
        run.w.member("bytes", p.bytes);
        run.w.member("build_ms", p.build_ms);
        run.w.member("apply_ms", p.apply_ms);
        run.w.member("ok", p.ok);
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "snapshot_bench");
}
