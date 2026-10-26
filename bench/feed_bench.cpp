// The receive path's cost (note 007, S6), against feed_sender (its own
// process; for the card, inside the far port's namespace). The first 50
// million messages of the sample day (about 1.16 million packets) sent at
// each pace in turn, nothing dropped by the sender, into each of:
//   one, recvmmsg   feed_one, D1's batches (D4 a), on CPU 2
//   one, recvmsg    feed_one, a datagram a call
//   two             feed_two (D4 b): receiving on CPU 2, handling on CPU 3
// behind them the default decoder and the handler on the default book. Per
// case: the kernel's drops (SO_RXQ_OVFL, D3; the highest pace without any is
// what S6 publishes; a case with drops is overloaded, recorded, not a
// failure), the pace achieved (the sender's packets over its first to last
// sending), the cost per datagram (each receiving pass that found
// datagrams timed between start() and stop() and divided among them: for
// feed_one everything to the handler, for feed_two the receiving side alone),
// and the time from a datagram's kernel receive stamp (SO_TIMESTAMPNS, D8) to
// its first message handled (CLOCK_REALTIME, the stamp's clock), the first
// 10,000 of each discarded as warm-up. Every case checked: every message
// handled once, the feed ended. The machine's net.core.rmem_max, busy_read
// and busy_poll are recorded (D2: kernel busy polling is set system-wide for
// a run, as the gateway did). Run JSON through lockfree-ipc's harness.
// Design: docs/design/007-receiver-framer.md
//
// Usage: feed_bench <loopback|card> <our ip> <sender ip> [output.json]
//   feed_sender must be serving: group 239.192.0.1 port 26400, requests on
//   26401, control on 26402.
#include <itchfh/book.hpp>
#include <itchfh/feed.hpp>
#include <itchfh/handler.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace lfipc::bench;
namespace fh = itchfh;

constexpr int cpu = 2, handler_cpu = 3;
constexpr std::uint64_t prefix = 50'000'000, warmup = 10'000;
constexpr std::uint16_t port = 26400, request_port = 26401, control_port = 26402;
const char* const group = "239.192.0.1";
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

// The kernel's stamp to the first message of each datagram handled.
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

std::string sysctl(const char* path) {
    std::ifstream f(path);
    std::string v;
    std::getline(f, v);
    return v;
}

struct control {
    int fd = -1;
    sockaddr_in to{};
    bool open(const std::string& sender_ip) {
        fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        timeval tv{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return fd >= 0 && fh::udp::address(sender_ip, control_port, to);
    }
    void run(const fh::mold::session_id& s, double pace, double drop, std::uint64_t last) {
        unsigned char b[35];
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

struct result {
    const char* arrangement;
    double pace = 0, achieved = 0;
    std::uint64_t packets = 0, messages = 0, datagrams = 0, kernel_drops = 0, requests = 0, empty_calls = 0, calls = 0;
    fh::sender_counts sent{};
    bool ok = false, sustained = false;
    hist cost, latency;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: feed_bench <loopback|card> <our ip> <sender ip> [output.json]\n");
        return 2;
    }
    const std::string path_name = argv[1], our_ip = argv[2], sender_ip = argv[3];
    // The harness reads argv[1] as the output path: give it ours.
    char* run_argv[] = {argv[0], argc > 4 ? argv[4] : nullptr, nullptr};
    run_file run(argc > 4 ? 2 : 1, run_argv, LFIPC_RESULTS_DIR, "feed");
    if (run.refused) return run.finish(false, "feed_bench");
    const auto noise = sample_noise({cpu, handler_cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !lfipc::bench::detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "feed_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "feed_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    control ctl;
    if (!ctl.open(sender_ip)) {
        std::fprintf(stderr, "feed_bench: cannot reach feed_sender's control port\n");
        return run.finish(false, "feed_bench");
    }
    fh::sender_counts probe_counts;
    if (!ctl.stop(probe_counts)) {
        std::fprintf(stderr, "feed_bench: feed_sender does not answer on %s:%u\n", sender_ip.c_str(), control_port);
        return run.finish(false, "feed_bench");
    }

    const double paces[] = {50'000, 75'000, 100'000, 125'000, 150'000, 200'000, 300'000, 400'000};
    const char* arrangements[] = {"one, recvmmsg", "one, recvmsg", "two"};
    std::vector<std::unique_ptr<result>> results;
    int granted = 0;
    char next_session = 'A';
    for (int a = 0; a < 3; ++a)
        for (const double pace : paces) {
            auto r = std::make_unique<result>();
            r->arrangement = arrangements[a];
            r->pace = pace;
            std::string why;
            fh::udp::receiver rx;
            fh::udp::receiver::options ro;
            ro.group = group;
            ro.interface_ip = our_ip;
            ro.port = port;
            if (!rx.open(ro, why)) {
                std::fprintf(stderr, "feed_bench: %s\n", why.c_str());
                return run.finish(false, "feed_bench");
            }
            granted = rx.granted_buffer();
            rx.set_server(sender_ip, request_port);
            auto book = std::make_unique<fh::default_book>();
            quiet_sink sink;
            auto h = std::make_unique<day_handler>(*book, sink);
            fh::sequencer_config qc;
            const fh::mold::session_id session = {'B', 'E', 'N', 'C', 'H', ' ', ' ', ' ', ' ', next_session++};
            const std::int64_t give_up = fh::monotonic_ns() + static_cast<std::int64_t>(4e9 * 1.2e6 / pace) + 30'000'000'000;
            fh::feed_counts fc;
            fh::feed_state state;
            ctl.run(session, pace, 0, prefix);
            if (a < 2) {
                fh::feed_one<day_handler, latency_probe> f(rx, *h, qc, a == 0, latency_probe{&r->latency});
                std::uint64_t seen = 0;
                while (f.seq().state() != fh::feed_state::ended && f.seq().state() != fh::feed_state::lost && fh::monotonic_ns() < give_up) {
                    const std::uint64_t before = f.counts().datagrams;
                    const ticks t0 = start();
                    f.poll();
                    const ticks t1 = stop();
                    const std::uint64_t n = f.counts().datagrams - before;
                    if (n == 0) continue;
                    seen += n;
                    if (seen > warmup)
                        for (std::uint64_t i = 0; i < n; ++i) r->cost.record((t1 - t0) / n);
                }
                fc = f.counts();
                state = f.seq().state();
                r->requests = f.seq().counts().requests;
            } else {
                fh::feed_two<day_handler, latency_probe> f(rx, *h, qc, latency_probe{&r->latency});
                std::atomic<bool> receiving{true};
                std::thread t([&] {
                    lfipc::bench::detail::pin_to(handler_cpu);
                    for (;;)
                        if (!f.handle_one() && !receiving.load(std::memory_order_acquire) && !f.handle_one()) break;
                });
                std::uint64_t seen = 0;
                while (f.seq().state() != fh::feed_state::ended && f.seq().state() != fh::feed_state::lost && fh::monotonic_ns() < give_up) {
                    const std::uint64_t before = f.counts().datagrams;
                    const ticks t0 = start();
                    f.receive_pass();
                    const ticks t1 = stop();
                    const std::uint64_t n = f.counts().datagrams - before;
                    if (n == 0) continue;
                    seen += n;
                    if (seen > warmup)
                        for (std::uint64_t i = 0; i < n; ++i) r->cost.record((t1 - t0) / n);
                }
                receiving.store(false, std::memory_order_release);
                t.join();
                fc = f.counts();
                fc.messages = f.handled();
                state = f.seq().state();
                r->requests = f.seq().counts().requests;
            }
            ctl.stop(r->sent);
            r->messages = fc.messages;
            r->datagrams = fc.datagrams;
            r->kernel_drops = fc.kernel_drops;
            r->calls = fc.calls;
            r->empty_calls = fc.empty_calls;
            r->packets = r->sent.packets;
            r->achieved = r->sent.last_ns > r->sent.first_ns
                              ? static_cast<double>(r->sent.packets - 1) * 1e9 / static_cast<double>(r->sent.last_ns - r->sent.first_ns)
                              : 0;
            r->sustained = r->kernel_drops == 0;
            r->ok = state == fh::feed_state::ended && fc.messages == prefix;
            std::printf("%-14s %7.0f/s: achieved %7.0f/s, %llu kernel drops, %llu requests; cost p50 %.0f ns a datagram; kernel to handled p50 %.1f us p99 %.1f us%s\n",
                        r->arrangement, pace, r->achieved, static_cast<unsigned long long>(r->kernel_drops),
                        static_cast<unsigned long long>(r->requests), r->cost.percentile(50) ? static_cast<double>(*r->cost.percentile(50)) / tpn : -1.0,
                        r->latency.percentile(50) ? static_cast<double>(*r->latency.percentile(50)) / 1e3 : -1.0,
                        r->latency.percentile(99) ? static_cast<double>(*r->latency.percentile(99)) / 1e3 : -1.0, !r->sustained ? "  overloaded (kernel drops)" : r->ok ? "" : "  NOT OK");
            std::fflush(stdout);
            results.push_back(std::move(r));
        }

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto ticks_ns = [&](std::optional<ticks> t) { return t ? static_cast<double>(*t) / tpn : -1.0; };
    const auto plain_ns = [&](std::optional<std::uint64_t> t) { return t ? static_cast<double>(*t) : -1.0; };
    bool ok = true;
    run.w.key("feed");
    run.w.begin_object();
    run.w.member("path", path_name);
    run.w.member("our_ip", our_ip);
    run.w.member("sender_ip", sender_ip);
    run.w.member("cpu", cpu);
    run.w.member("handler_cpu", handler_cpu);
    run.w.member("messages", prefix);
    run.w.member("warmup_discarded", warmup);
    run.w.member("buffer_granted", granted);
    run.w.member("rmem_max", sysctl("/proc/sys/net/core/rmem_max"));
    run.w.member("busy_read", sysctl("/proc/sys/net/core/busy_read"));
    run.w.member("busy_poll", sysctl("/proc/sys/net/core/busy_poll"));
    run.w.member("lockfree_ipc_commit", ITCHFH_LFIPC_COMMIT);
    run.w.key("cases");
    run.w.begin_array();
    for (const auto& r : results) {
        ok = ok && (r->ok || !r->sustained);  // overloaded cases are what the sweep finds, not failures
        run.w.begin_object();
        run.w.member("arrangement", std::string(r->arrangement));
        run.w.member("pace", r->pace);
        run.w.member("achieved", r->achieved);
        run.w.member("ok", r->ok);
        run.w.member("sustained", r->sustained);
        run.w.member("packets_sent", r->packets);
        run.w.member("sent_late", r->sent.behind);
        run.w.member("datagrams", r->datagrams);
        run.w.member("messages", r->messages);
        run.w.member("kernel_drops", r->kernel_drops);
        run.w.member("requests", r->requests);
        run.w.member("calls", r->calls);
        run.w.member("empty_calls", r->empty_calls);
        run.w.key("cost");
        run.w.begin_object();
        run.w.member("count", r->cost.count());
        run.w.member("p50_ns", ticks_ns(r->cost.percentile(50)));
        run.w.member("p99_ns", ticks_ns(r->cost.percentile(99)));
        run.w.member("p999_ns", ticks_ns(r->cost.percentile(99.9)));
        run.w.member("max_ns", static_cast<double>(r->cost.max()) / tpn);
        run.w.end_object();
        run.w.key("kernel_to_handled");
        run.w.begin_object();
        run.w.member("count", r->latency.count());
        run.w.member("p50_ns", plain_ns(r->latency.percentile(50)));
        run.w.member("p99_ns", plain_ns(r->latency.percentile(99)));
        run.w.member("p999_ns", plain_ns(r->latency.percentile(99.9)));
        run.w.member("max_ns", static_cast<double>(r->latency.max()));
        run.w.end_object();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "feed_bench");
}
