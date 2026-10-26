// The sequencer's cost (note 005, S6), on CPU 2, the sample day packed by D7's
// packetizer into packets held in memory taken after the harness binds memory
// to CPU 2's node. The output counts messages and does nothing else.
//   in order:   every packet of the day, in order, through on_packet, each call
//               timed between start() and stop(), unsubtracted, the first
//               10,000 packets discarded as warm-up; and the whole day untimed,
//               as packets and messages a second;
//   held:       a packet arriving after a gap, copied into the hold, each timed;
//   backlog:    a gap of one packet with H packets held behind it (H = 10, 100,
//               1,000, 10,000), then the missing packet: that call, which
//               delivers the whole backlog, timed; 200 trials at each H, at
//               positions spread over the day.
// The amendment to D9 (Review): the lossy link's times are simulated, so what
// is measured is the sequencer's own work on a gap.
// Run JSON through lockfree-ipc's harness. Design: docs/design/005-sequencing-recovery.md
//
// Usage: sequencer_bench [output.json] [day.gz]
#include <itchfh/memory.hpp>
#include <itchfh/mold.hpp>
#include <itchfh/sequencer.hpp>
#include <itchfh/session_store.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace lfipc::bench;
namespace fh = itchfh;

constexpr int cpu = 2;
constexpr std::size_t warmup = 10'000;
using hist = histogram<10, 40>;

struct counting {
    std::uint64_t messages = 0, bytes = 0, requests = 0;
    void deliver(std::uint64_t, const unsigned char*, std::size_t n) {
        ++messages;
        bytes += n;
    }
    void state(fh::feed_state, std::uint64_t) {}
    void send_request(const unsigned char*, std::size_t) { ++requests; }
};

// The day's packets, one after another, and where each begins.
struct packets {
    fh::table<unsigned char> bytes;
    std::vector<std::uint64_t> at;  // at[i] .. at[i+1]
    const unsigned char* p(std::size_t i) const { return bytes.get() + at[i]; }
    std::size_t n(std::size_t i) const { return at[i + 1] - at[i]; }
    std::size_t size() const { return at.size() - 1; }
};

fh::sequencer_config config() {
    fh::sequencer_config c;
    c.hold_bytes = std::size_t{64} << 20;
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 2 ? argv[2] : ITCHFH_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "sequencer");
    if (run.refused) return run.finish(false, "sequencer_bench");
    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !lfipc::bench::detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "sequencer_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "sequencer_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;

    // The day into a store, then packed (D7), on CPU 2's node.
    const fh::mold::session_id session = {'0', '0', '0', '0', '0', '0', '0', '0', '0', '1'};
    auto store = std::make_unique<fh::session_store>(session, 450'000'000, std::size_t{14} << 30, true);
    if (std::FILE* in = ::popen(("gzip -dc '" + path + "'").c_str(), "r")) {
        unsigned char buf[1 << 16];
        for (;;) {
            unsigned char lb[2];
            if (std::fread(lb, 1, 2, in) != 2) break;
            const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
            if (len == 0 || std::fread(buf, 1, len, in) != len || !store->append(buf, len)) break;
        }
        ::pclose(in);
    }
    if (store->count() == 0) {
        std::fprintf(stderr, "sequencer_bench: %s: nothing read (tools/fetch_day.py fetches it)\n", path.c_str());
        write_clock_block(run.w, clk);
        return run.finish(false, "sequencer_bench");
    }
    packets pk{fh::zeroed<unsigned char>(std::size_t{16} << 30, true), {}};
    pk.at.reserve(20'000'000);
    {
        std::uint64_t at = 0;
        for (std::uint64_t first = 1; first <= store->count();) {
            const std::size_t n = store->fill(first, ~0ull, pk.bytes.get() + at);
            pk.at.push_back(at);
            at += n;
            first += fh::mold::read_header(pk.bytes.get() + pk.at.back()).count;
        }
        pk.at.push_back(at);
    }
    store.reset();

    hist none;
    for (std::size_t i = 0; i < 10'000'000; ++i) {
        const ticks t0 = start();
        const ticks t1 = stop();
        if (i >= 1'000'000) none.record(t1 - t0);
    }

    // In order, timed then untimed.
    hist in_order;
    std::uint64_t messages = 0;
    {
        counting out;
        fh::sequencer<counting> s(out, config());
        for (std::size_t i = 0; i < pk.size(); ++i) {
            const ticks t0 = start();
            s.on_packet(pk.p(i), pk.n(i), 0);
            const ticks t1 = stop();
            if (i >= warmup) in_order.record(t1 - t0);
        }
        messages = out.messages;
    }
    double packets_per_s = 0, messages_per_s = 0;
    {
        counting out;
        fh::sequencer<counting> s(out, config());
        const auto w0 = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < pk.size(); ++i) s.on_packet(pk.p(i), pk.n(i), 0);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
        packets_per_s = static_cast<double>(pk.size()) / secs;
        messages_per_s = static_cast<double>(out.messages) / secs;
    }

    // Held, and the backlog delivered when a gap fills.
    const std::size_t backlogs[] = {10, 100, 1'000, 10'000};
    hist held;
    std::vector<hist> backlog(4);
    std::vector<double> per_held_ns(4, 0);
    for (int b = 0; b < 4; ++b) {
        const std::size_t H = backlogs[b];
        std::vector<ticks> per;
        for (int trial = 0; trial < 200; ++trial) {
            const std::size_t gap = 1 + static_cast<std::size_t>(trial) * ((pk.size() - H - 2) / 200);
            counting out;
            fh::sequencer_config c = config();
            c.first_expected = fh::mold::read_header(pk.p(gap - 1)).sequence;
            fh::sequencer<counting> s(out, c);
            s.on_packet(pk.p(gap - 1), pk.n(gap - 1), 0);  // in sequence
            for (std::size_t i = gap + 1; i <= gap + H; ++i) {
                const ticks t0 = start();
                s.on_packet(pk.p(i), pk.n(i), 0);  // after the gap: held
                const ticks t1 = stop();
                if (b == 3) held.record(t1 - t0);
            }
            const ticks t0 = start();
            s.on_packet(pk.p(gap), pk.n(gap), 0);  // the missing packet: the backlog delivered
            const ticks t1 = stop();
            backlog[b].record(t1 - t0);
            per.push_back((t1 - t0) / static_cast<ticks>(H + 1));
        }
        std::sort(per.begin(), per.end());
        per_held_ns[b] = static_cast<double>(per[per.size() / 2]) / tpn;
    }

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
    run.w.key("sequencer");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("source", path.substr(path.find_last_of('/') + 1));
    run.w.member("packets", static_cast<std::uint64_t>(pk.size()));
    run.w.member("messages", messages);
    run.w.member("warmup_discarded", static_cast<std::uint64_t>(warmup));
    run.w.member("lockfree_ipc_commit", ITCHFH_LFIPC_COMMIT);
    run.w.key("none");
    run.w.begin_object();
    write_hist(none);
    run.w.end_object();
    run.w.member("packets_per_s", packets_per_s);
    run.w.member("messages_per_s", messages_per_s);
    run.w.key("in_order");
    run.w.begin_object();
    write_hist(in_order);
    run.w.end_object();
    run.w.key("held");
    run.w.begin_object();
    write_hist(held);
    run.w.end_object();
    run.w.key("backlogs");
    run.w.begin_array();
    for (int b = 0; b < 4; ++b) {
        run.w.begin_object();
        run.w.member("held_packets", static_cast<std::uint64_t>(backlogs[b]));
        run.w.member("median_ns_per_packet", per_held_ns[b]);
        write_hist(backlog[b]);
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    std::printf("in order: %.2f M packets/s, %.1f M messages/s, p50 %.0f ns a packet; held p50 %.0f ns; backlog of 10,000: p50 %.0f us (%.0f ns a packet)\n",
                packets_per_s / 1e6, messages_per_s / 1e6, ns(in_order.percentile(50)), ns(held.percentile(50)),
                ns(backlog[3].percentile(50)) / 1e3, per_held_ns[3]);
    return run.finish(messages == 0 ? false : true, "sequencer_bench");
}
