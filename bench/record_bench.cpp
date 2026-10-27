// The recorder's cost (note 009, S6 and D8), on CPU 2, its writer on CPU 1 (a
// housekeeping CPU), the recording on the build directory's disk:
//   writer flat out  1,400-byte datagrams recorded in a tight loop for 10 s:
//                    record()'s own cost (each call timed between start() and
//                    stop(), the first 10,000 discarded), apart for the calls
//                    that recorded and those that found the ring full (the
//                    check run's single figure was mostly the second: 98.7% of
//                    the calls), the rate written to disk, and the share left
//                    unrecorded once the writer falls behind (D2)
//   feed, off / on   against feed_sender (as feed_bench): the first 50 million
//                    messages at 50k and 100k packets a second, one thread
//                    reading a datagram a call (note 007's default), with the
//                    recorder off and on: the cost a datagram (each pass that
//                    found datagrams timed and divided among them), kernel to
//                    handled (as feed_bench), datagrams unrecorded
// Every recording is deleted after its case. Run JSON through lockfree-ipc's
// harness. Design: docs/design/009-recording-monitoring.md
//
// Usage: record_bench <our ip> <sender ip> [output.json]
//   feed_sender must be serving: group 239.192.0.1 port 26400, requests on
//   26401, control on 26402.
#include <itchfh/book.hpp>
#include <itchfh/feed.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/recording.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

using namespace lfipc::bench;
namespace fh = itchfh;

constexpr int cpu = 2, writer_cpu = 1;
constexpr std::uint64_t prefix = 50'000'000, warmup = 10'000;
constexpr double flat_out_s = 10;
constexpr std::uint16_t port = 26400, request_port = 26401, control_port = 26402;
using hist = histogram<10, 40>;

struct quiet_sink {
    void book(const fh::applied&) {}
    void trade(const fh::trade_report&) {}
    void trade_broken(const fh::trade_report&) {}
    void status(std::uint16_t, fh::status_field) {}
    void imbalance(std::uint16_t, const fh::imbalance&) {}
    void market(const fh::message&) {}
    void directory(std::uint16_t, const fh::stock_directory&) {}
};
using day_handler = fh::handler<fh::default_book, quiet_sink>;

std::int64_t realtime_ns() {
    timespec t;
    ::clock_gettime(CLOCK_REALTIME, &t);
    return static_cast<std::int64_t>(t.tv_sec) * 1'000'000'000 + t.tv_nsec;
}
struct latency_probe {
    hist* h = nullptr;
    std::int64_t last = 0;
    std::uint64_t seen = 0;
    void handled(std::int64_t kernel_ns) {
        if (kernel_ns == last) return;
        last = kernel_ns;
        if (++seen > warmup) h->record(static_cast<std::uint64_t>(realtime_ns() - kernel_ns));
    }
};

struct control {
    int fd = -1;
    sockaddr_in to{};
    bool open(const std::string& ip) {
        fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        timeval tv{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return fd >= 0 && fh::udp::address(ip, control_port, to);
    }
    void run(const fh::mold::session_id& s, double pace) {
        unsigned char b[35];
        const double drop = 0;
        const std::uint64_t last = prefix;
        b[0] = 'R';
        std::memcpy(b + 1, s.data(), 10);
        std::memcpy(b + 11, &pace, 8);
        std::memcpy(b + 19, &drop, 8);
        std::memcpy(b + 27, &last, 8);
        ::sendto(fd, b, sizeof(b), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }
    bool stop(fh::sender_counts& c) {
        const unsigned char s = 'S';
        ::sendto(fd, &s, 1, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        return ::recv(fd, &c, sizeof(c), 0) == static_cast<ssize_t>(sizeof(c));
    }
};

struct feed_case {
    double pace = 0;
    bool recording = false, ok = false;
    std::uint64_t datagrams = 0, kernel_drops = 0, recorded = 0, unrecorded = 0, bytes = 0;
    hist cost, latency;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: record_bench <our ip> <sender ip> [output.json]\n");
        return 2;
    }
    const std::string our_ip = argv[1], sender_ip = argv[2], dir = ITCHFH_RECORD_DIR;
    char* run_argv[] = {argv[0], argc > 3 ? argv[3] : nullptr, nullptr};
    run_file run(argc > 3 ? 2 : 1, run_argv, LFIPC_RESULTS_DIR, "record");
    if (run.refused) return run.finish(false, "record_bench");
    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !lfipc::bench::detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "record_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "record_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    std::string why;
    bool ok = true;

    // The writer flat out.
    hist flat, flat_refused;
    std::uint64_t flat_calls = 0, flat_unrecorded = 0, flat_written = 0, flat_bytes = 0;
    double flat_secs = 0;
    {
        const std::string path = dir + "/record-bench-flat.rec";
        std::filesystem::remove(path);
        fh::rec::recorder r;
        if (!r.open(path, 0, writer_cpu, why)) {
            std::fprintf(stderr, "record_bench: %s\n", why.c_str());
            return run.finish(false, "record_bench");
        }
        fh::udp::datagram d;
        d.length = 1400;
        std::mt19937_64 rng(5);
        for (std::uint32_t i = 0; i < d.length; ++i) d.bytes[i] = static_cast<unsigned char>(rng());
        const auto t0 = std::chrono::steady_clock::now();
        const auto until = t0 + std::chrono::duration<double>(flat_out_s);
        while (std::chrono::steady_clock::now() < until)
            for (int k = 0; k < 1000; ++k) {
                d.kernel_ns = static_cast<std::int64_t>(flat_calls);
                const std::uint64_t refused_before = r.unrecorded();
                const ticks a = start();
                r.record(d, 0);
                const ticks b = stop();
                if (++flat_calls > warmup) (r.unrecorded() == refused_before ? flat : flat_refused).record(b - a);
            }
        flat_unrecorded = r.unrecorded();
        r.close(0, 0);
        flat_secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        flat_written = r.written();
        flat_bytes = r.bytes_written();
        std::filesystem::remove(path);
        std::printf("writer flat out: %llu calls, record() recording p50 %.0f ns p99 %.0f (refused p50 %.0f); written %.0f MB/s, %.1f%% unrecorded\n",
                    static_cast<unsigned long long>(flat_calls), flat.percentile(50) ? static_cast<double>(*flat.percentile(50)) / tpn : -1.0,
                    flat.percentile(99) ? static_cast<double>(*flat.percentile(99)) / tpn : -1.0,
                    flat_refused.percentile(50) ? static_cast<double>(*flat_refused.percentile(50)) / tpn : -1.0, static_cast<double>(flat_bytes) / 1e6 / flat_secs,
                    100.0 * static_cast<double>(flat_unrecorded) / static_cast<double>(flat_calls));
        std::fflush(stdout);
    }

    // The feed, recording off and on.
    control ctl;
    fh::sender_counts probe;
    if (!ctl.open(sender_ip) || !ctl.stop(probe)) {
        std::fprintf(stderr, "record_bench: feed_sender does not answer on %s:%u\n", sender_ip.c_str(), control_port);
        return run.finish(false, "record_bench");
    }
    std::vector<std::unique_ptr<feed_case>> cases;
    char id = 'A';
    for (const double pace : {50'000.0, 100'000.0})
        for (const bool recording : {false, true}) {
            auto c = std::make_unique<feed_case>();
            c->pace = pace;
            c->recording = recording;
            fh::udp::receiver rx;
            fh::udp::receiver::options ro;
            ro.interface_ip = our_ip;
            ro.port = port;
            if (!rx.open(ro, why)) {
                std::fprintf(stderr, "record_bench: %s\n", why.c_str());
                return run.finish(false, "record_bench");
            }
            rx.set_server(sender_ip, request_port);
            auto book = std::make_unique<fh::default_book>();
            quiet_sink sink;
            auto h = std::make_unique<day_handler>(*book, sink);
            fh::rec::recorder r;
            const std::string path = dir + "/record-bench-feed.rec";
            std::filesystem::remove(path);
            if (recording && !r.open(path, 0, writer_cpu, why)) {
                std::fprintf(stderr, "record_bench: %s\n", why.c_str());
                return run.finish(false, "record_bench");
            }
            fh::feed_one<day_handler, latency_probe, fh::rec::recorder> f(rx, *h, {}, false, latency_probe{&c->latency});
            if (recording) f.set_recorder(r);
            const fh::mold::session_id session = {'R', 'E', 'C', 'B', 'E', 'N', 'C', 'H', ' ', id++};
            ctl.run(session, pace);
            const std::int64_t give_up = fh::monotonic_ns() + static_cast<std::int64_t>(4e9 * 1.2e6 / pace) + 30'000'000'000;
            std::uint64_t seen = 0;
            while (f.seq().state() != fh::feed_state::ended && f.seq().state() != fh::feed_state::lost && fh::monotonic_ns() < give_up) {
                const std::uint64_t before = f.counts().datagrams;
                const ticks a = start();
                f.poll();
                const ticks b = stop();
                const std::uint64_t n = f.counts().datagrams - before;
                if (n == 0) continue;
                seen += n;
                if (seen > warmup)
                    for (std::uint64_t i = 0; i < n; ++i) c->cost.record((b - a) / n);
            }
            fh::sender_counts sent;
            ctl.stop(sent);
            c->datagrams = f.counts().datagrams;
            c->kernel_drops = f.counts().kernel_drops;
            c->ok = f.seq().state() == fh::feed_state::ended && f.counts().messages == prefix;
            if (recording) {
                r.close(f.counts().kernel_drops, f.counts().answer_drops);
                c->recorded = r.written();
                c->unrecorded = r.unrecorded();
                c->bytes = r.bytes_written();
                std::filesystem::remove(path);
            }
            if (!c->ok || c->unrecorded) ok = false;
            std::printf("feed %6.0f/s, recording %-3s: cost p50 %.0f ns a datagram, kernel to handled p50 %.1f us p99 %.1f us; %llu kernel drops, "
                        "%llu recorded, %llu unrecorded%s\n",
                        pace, recording ? "on" : "off", c->cost.percentile(50) ? static_cast<double>(*c->cost.percentile(50)) / tpn : -1.0,
                        c->latency.percentile(50) ? static_cast<double>(*c->latency.percentile(50)) / 1e3 : -1.0,
                        c->latency.percentile(99) ? static_cast<double>(*c->latency.percentile(99)) / 1e3 : -1.0,
                        static_cast<unsigned long long>(c->kernel_drops), static_cast<unsigned long long>(c->recorded),
                        static_cast<unsigned long long>(c->unrecorded), c->ok ? "" : "  NOT OK");
            std::fflush(stdout);
            cases.push_back(std::move(c));
        }

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto tns = [&](std::optional<ticks> t) { return t ? static_cast<double>(*t) / tpn : -1.0; };
    const auto pns = [&](std::optional<std::uint64_t> t) { return t ? static_cast<double>(*t) : -1.0; };
    run.w.key("record");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("writer_cpu", writer_cpu);
    run.w.member("our_ip", our_ip);
    run.w.member("sender_ip", sender_ip);
    run.w.member("messages", prefix);
    run.w.member("warmup_discarded", warmup);
    run.w.member("lockfree_ipc_commit", ITCHFH_LFIPC_COMMIT);
    run.w.key("flat_out");
    run.w.begin_object();
    run.w.member("seconds", flat_secs);
    run.w.member("datagram_bytes", 1400);
    run.w.member("calls", flat_calls);
    run.w.member("written", flat_written);
    run.w.member("unrecorded", flat_unrecorded);
    run.w.member("mb_per_s", static_cast<double>(flat_bytes) / 1e6 / flat_secs);
    run.w.member("recorded_calls", flat.count());
    run.w.member("p50_ns", tns(flat.percentile(50)));
    run.w.member("p99_ns", tns(flat.percentile(99)));
    run.w.member("p999_ns", tns(flat.percentile(99.9)));
    run.w.member("max_ns", static_cast<double>(flat.max()) / tpn);
    run.w.key("refused");
    run.w.begin_object();
    run.w.member("count", flat_refused.count());
    run.w.member("p50_ns", tns(flat_refused.percentile(50)));
    run.w.member("p99_ns", tns(flat_refused.percentile(99)));
    run.w.end_object();
    run.w.end_object();
    run.w.key("feed");
    run.w.begin_array();
    for (const auto& c : cases) {
        run.w.begin_object();
        run.w.member("pace", c->pace);
        run.w.member("recording", c->recording);
        run.w.member("ok", c->ok);
        run.w.member("datagrams", c->datagrams);
        run.w.member("kernel_drops", c->kernel_drops);
        run.w.member("recorded", c->recorded);
        run.w.member("unrecorded", c->unrecorded);
        run.w.member("bytes", c->bytes);
        run.w.key("cost");
        run.w.begin_object();
        run.w.member("p50_ns", tns(c->cost.percentile(50)));
        run.w.member("p99_ns", tns(c->cost.percentile(99)));
        run.w.member("p999_ns", tns(c->cost.percentile(99.9)));
        run.w.end_object();
        run.w.key("kernel_to_handled");
        run.w.begin_object();
        run.w.member("count", c->latency.count());
        run.w.member("p50_ns", pns(c->latency.percentile(50)));
        run.w.member("p99_ns", pns(c->latency.percentile(99)));
        run.w.member("p999_ns", pns(c->latency.percentile(99.9)));
        run.w.member("max_ns", static_cast<double>(c->latency.max()));
        run.w.end_object();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "record_bench");
}
