// Packet to published update (note 010, S1, S2 and S4), against feed_sender
// started with a stamps name (bench/stamps.hpp): it writes each message's
// intended send time before sending its packet (D1, D2: open loop, so a late
// sender or a backed-up receiver shows in the latency). On CPU 2, the program's
// components (note 009's D7: one thread reading a datagram a call, the default
// book and handler, the publisher), recording off and on (D5); a consumer, its
// own process on CPU 3 (the publisher's node: note 008's amendment), reads its
// ring and, for the first update of each message, takes CLOCK_MONOTONIC less
// that message's stamp; the first 10,000 discarded. Cases (D3): the first 50
// million messages at 10k, 25k and 50k packets a second, recording off and
// on; and five minutes of the open (from 09:29:30) at the day's own pace,
// recording on. Every case checked: every message handled once, the feed
// ended, the consumer neither overrun nor on another node. D7: the stamp
// write's cost and the clock read's, each timed alone. Run JSON through
// lockfree-ipc's harness. Design: docs/design/010-measurement.md
//
// Usage: e2e_bench <loopback|card-busy-poll> <our ip> <sender ip> [output.json]
//   feed_sender must be serving with stamps /itchfh-stamps: group 239.192.0.1
//   port 26400, requests on 26401, control on 26402.
#include <itchfh/book.hpp>
#include <itchfh/consumer.hpp>
#include <itchfh/feed.hpp>
#include <itchfh/publisher.hpp>
#include <itchfh/recording.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include "stamps.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace lfipc::bench;
namespace fh = itchfh;

constexpr int cpu = 2, consumer_cpu = 3, writer_cpu = 1;
constexpr std::uint64_t prefix = 50'000'000, warmup = 10'000;
constexpr std::uint64_t open_from_ns = (9ull * 3600 + 29 * 60 + 30) * 1'000'000'000, open_to_ns = open_from_ns + 300ull * 1'000'000'000;
constexpr std::uint16_t port = 26400, request_port = 26401, control_port = 26402;
const char* const stamps_name = "/itchfh-stamps";
using hist = histogram<10, 40>;

struct control {
    int fd = -1;
    sockaddr_in to{};
    bool open(const std::string& ip) {
        fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        timeval tv{5, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        return fd >= 0 && fh::udp::address(ip, control_port, to);
    }
    // The long form: answered with the first and last messages it resolves to.
    bool run(const fh::mold::session_id& s, double pace, double speed, std::uint64_t last, std::uint64_t from, std::uint64_t to_ns,
             std::uint64_t (&span)[2]) {
        unsigned char b[59];
        const double drop = 0;
        b[0] = 'R';
        std::memcpy(b + 1, s.data(), 10);
        std::memcpy(b + 11, &pace, 8);
        std::memcpy(b + 19, &drop, 8);
        std::memcpy(b + 27, &last, 8);
        std::memcpy(b + 35, &speed, 8);
        std::memcpy(b + 43, &from, 8);
        std::memcpy(b + 51, &to_ns, 8);
        ::sendto(fd, b, sizeof(b), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        return ::recv(fd, span, sizeof(span), 0) == static_cast<ssize_t>(sizeof(span));
    }
    bool stop(fh::sender_counts& c) {
        const unsigned char s = 'S';
        ::sendto(fd, &s, 1, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        return ::recv(fd, &c, sizeof(c), 0) == static_cast<ssize_t>(sizeof(c));
    }
};

struct consumer_report {
    std::uint64_t count = 0, updates = 0;
    double p50 = -1, p99 = -1, p999 = -1, max = -1;  // ns
    bool attached = false, overrun = false, remote = false;
};

// The consumer process: on CPU 3, its ring read until the feed ends.
void consumer_main(const std::string& name, int out) {
    lfipc::bench::detail::pin_to(consumer_cpu);
    consumer_report r;
    std::string why;
    fh::bench::stamp_array* st = fh::bench::open_stamps(stamps_name, false, why);
    fh::pub::consumer c;
    for (int i = 0; i < 2000 && !c.attach(name, why); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    r.attached = st && c.index() >= 0;
    if (r.attached) {
        c.report_node();
        r.remote = fh::pub::node_of(::sched_getcpu()) != c.publisher_node();
        auto h = std::make_unique<hist>();
        std::uint64_t seen = 0;
        fh::pub::update u;
        for (;;) {
            if (!c.poll(u)) {
                if (c.overrun()) {
                    r.overrun = true;
                    break;
                }
                continue;
            }
            const std::int64_t now = fh::monotonic_ns();
            ++r.updates;
            if (u.k == fh::pub::kind::feed) {
                if (u.feed.state == fh::feed_state::ended) break;
                continue;
            }
            if (u.k == fh::pub::kind::reset || u.sequence == 0 || (u.flags & fh::pub::continues)) continue;
            if (++seen > warmup) h->record(static_cast<std::uint64_t>(now - st->get(u.sequence)));
        }
        r.count = h->count();
        const auto v = [](std::optional<std::uint64_t> x) { return x ? static_cast<double>(*x) : -1.0; };
        r.p50 = v(h->percentile(50));
        r.p99 = v(h->percentile(99));
        r.p999 = v(h->percentile(99.9));
        r.max = static_cast<double>(h->max());
    }
    [[maybe_unused]] const ssize_t n = ::write(out, &r, sizeof(r));
}

struct case_result {
    std::string name;
    double pace = 0, speed = 0;
    bool recording = false, ok = false;
    std::uint64_t first = 0, last = 0, messages = 0, datagrams = 0, kernel_drops = 0, requests = 0, unrecorded = 0;
    fh::sender_counts sent{};
    consumer_report c;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: e2e_bench <loopback|card-busy-poll> <our ip> <sender ip> [output.json]\n");
        return 2;
    }
    const std::string path_name = argv[1], our_ip = argv[2], sender_ip = argv[3], dir = ITCHFH_RECORD_DIR;
    char* run_argv[] = {argv[0], argc > 4 ? argv[4] : nullptr, nullptr};
    run_file run(argc > 4 ? 2 : 1, run_argv, LFIPC_RESULTS_DIR, "e2e");
    if (run.refused) return run.finish(false, "e2e_bench");
    const auto noise = sample_noise({cpu, consumer_cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !lfipc::bench::detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "e2e_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "e2e_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;
    std::string why;
    fh::bench::stamp_array* st = fh::bench::open_stamps(stamps_name, false, why);
    control ctl;
    fh::sender_counts probe;
    if (!st || !ctl.open(sender_ip) || !ctl.stop(probe)) {
        std::fprintf(stderr, "e2e_bench: feed_sender with stamps %s does not answer on %s:%u (%s)\n", stamps_name, sender_ip.c_str(), control_port, why.c_str());
        return run.finish(false, "e2e_bench");
    }

    // D7: the stamp write and the clock read, each timed alone.
    // The stamp writes on an array of our own, in sequence order as the sender writes them.
    hist put_cost, clock_cost;
    auto own = std::make_unique<fh::bench::stamp_array>();
    for (std::uint64_t i = 0; i < 1'000'000; ++i) {
        const ticks a = start();
        own->put(i, static_cast<std::int64_t>(i));
        const ticks b = stop();
        put_cost.record(b - a);
    }
    for (int i = 0; i < 1'000'000; ++i) {
        const ticks a = start();
        [[maybe_unused]] volatile std::int64_t t = fh::monotonic_ns();
        const ticks b = stop();
        clock_cost.record(b - a);
    }
    hist none;  // the start()/stop() pair alone, to read the two against
    for (int i = 0; i < 1'000'000; ++i) {
        const ticks a = start();
        const ticks b = stop();
        none.record(b - a);
    }

    struct plan {
        const char* name;
        double pace, speed;
        bool recording;
    };
    const plan plans[] = {{"10k/s", 10'000, 0, false}, {"10k/s", 10'000, 0, true},  {"25k/s", 25'000, 0, false},
                          {"25k/s", 25'000, 0, true},  {"50k/s", 50'000, 0, false}, {"50k/s", 50'000, 0, true},
                          {"the open, its own pace", 0, 1, true}};
    std::vector<std::unique_ptr<case_result>> results;
    bool ok = true;
    char id = 'A';
    int k = 0;
    for (const plan& pl : plans) {
        auto r = std::make_unique<case_result>();
        r->name = pl.name;
        r->pace = pl.pace;
        r->speed = pl.speed;
        r->recording = pl.recording;
        const std::string name = "/itchfh-e2e-" + std::to_string(::getpid()) + "-" + std::to_string(k++);
        fh::udp::receiver rx;
        fh::udp::receiver::options ro;
        ro.interface_ip = our_ip;
        ro.port = port;
        if (!rx.open(ro, why)) {
            std::fprintf(stderr, "e2e_bench: %s\n", why.c_str());
            return run.finish(false, "e2e_bench");
        }
        rx.set_server(sender_ip, request_port);
        auto book = std::make_unique<fh::default_book>();
        auto p = std::make_unique<fh::published<>>(*book);
        if (!p->pub.open(name, why)) {
            std::fprintf(stderr, "e2e_bench: %s\n", why.c_str());
            return run.finish(false, "e2e_bench");
        }
        fh::rec::recorder rec;
        const std::string rec_path = dir + "/e2e-bench.rec";
        std::filesystem::remove(rec_path);
        if (pl.recording && !rec.open(rec_path, 0, writer_cpu, why)) {
            std::fprintf(stderr, "e2e_bench: %s\n", why.c_str());
            return run.finish(false, "e2e_bench");
        }
        int fds[2];
        if (::pipe(fds) != 0) return run.finish(false, "e2e_bench");
        const pid_t kid = ::fork();
        if (kid == 0) {
            ::close(fds[0]);
            consumer_main(name, fds[1]);
            ::_exit(0);
        }
        ::close(fds[1]);
        // The consumer joined (its replay of the empty book done) before anything is sent.
        for (int i = 0; i < 20'000'000; ++i) {
            p->tick(fh::monotonic_ns());
            int live = 0;
            for (int c = 0; c < fh::pub::max_consumers; ++c) live += p->pub.slot(c) == fh::pub::slot_live;
            if (live) break;
        }
        const fh::mold::session_id session = {'E', '2', 'E', 'B', 'E', 'N', 'C', 'H', ' ', id++};
        std::uint64_t span[2] = {0, 0};
        const bool open = pl.speed > 0;
        if (!ctl.run(session, pl.pace, pl.speed, open ? 0 : prefix, open ? open_from_ns : 0, open ? open_to_ns : 0, span)) {
            std::fprintf(stderr, "e2e_bench: feed_sender did not answer the run\n");
            return run.finish(false, "e2e_bench");
        }
        r->first = span[0];
        r->last = span[1];
        fh::sequencer_config qc;
        qc.first_expected = span[0];
        fh::feed_one<fh::published<>, fh::no_probe, fh::rec::recorder> f(rx, *p, qc);
        if (pl.recording) f.set_recorder(rec);
        const double packets = static_cast<double>(span[1] - span[0] + 1) / 43.0;
        const std::int64_t give_up = fh::monotonic_ns() + (open ? 400'000'000'000 : static_cast<std::int64_t>(3e9 * packets / pl.pace) + 60'000'000'000);
        while (f.seq().state() != fh::feed_state::ended && f.seq().state() != fh::feed_state::lost && fh::monotonic_ns() < give_up) f.poll();
        ctl.stop(r->sent);
        r->messages = f.counts().messages;
        r->datagrams = f.counts().datagrams;
        r->kernel_drops = f.counts().kernel_drops;
        r->requests = f.seq().counts().requests;
        if (pl.recording) {
            rec.close(f.counts().kernel_drops, f.counts().answer_drops);
            r->unrecorded = rec.unrecorded();
            std::filesystem::remove(rec_path);
        }
        if (f.seq().state() != fh::feed_state::ended) p->feed(fh::feed_state::ended, 0);  // so the consumer stops
        const bool got = ::read(fds[0], &r->c, sizeof(r->c)) == static_cast<ssize_t>(sizeof(r->c));
        ::close(fds[0]);
        ::waitpid(kid, nullptr, 0);
        r->ok = got && r->c.attached && !r->c.overrun && !r->c.remote && f.seq().state() == fh::feed_state::ended &&
                r->messages == span[1] - span[0] + 1 && r->unrecorded == 0;
        if (!r->ok) ok = false;
        std::printf("%-22s recording %-3s: %llu messages (%llu..%llu); packet to published update p50 %.1f us p99 %.1f us p99.9 %.1f us max "
                    "%.1f us; sender late max %.1f us; %llu kernel drops%s\n",
                    r->name.c_str(), r->recording ? "on" : "off", static_cast<unsigned long long>(r->messages),
                    static_cast<unsigned long long>(span[0]), static_cast<unsigned long long>(span[1]), r->c.p50 / 1e3, r->c.p99 / 1e3,
                    r->c.p999 / 1e3, r->c.max / 1e3, static_cast<double>(r->sent.late_max_ns) / 1e3,
                    static_cast<unsigned long long>(r->kernel_drops), r->ok ? "" : "  NOT OK");
        std::fflush(stdout);
        results.push_back(std::move(r));
    }

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto tns = [&](std::optional<ticks> t) { return t ? static_cast<double>(*t) / tpn : -1.0; };
    run.w.key("e2e");
    run.w.begin_object();
    run.w.member("path", path_name);
    run.w.member("our_ip", our_ip);
    run.w.member("sender_ip", sender_ip);
    run.w.member("cpu", cpu);
    run.w.member("consumer_cpu", consumer_cpu);
    run.w.member("writer_cpu", writer_cpu);
    run.w.member("warmup_discarded", warmup);
    run.w.member("lockfree_ipc_commit", ITCHFH_LFIPC_COMMIT);
    run.w.key("own_cost");
    run.w.begin_object();
    run.w.member("clock_pair_p50_ns", tns(none.percentile(50)));
    run.w.member("stamp_write_p50_ns", tns(put_cost.percentile(50)));
    run.w.member("clock_read_p50_ns", tns(clock_cost.percentile(50)));
    run.w.end_object();
    run.w.key("cases");
    run.w.begin_array();
    for (const auto& r : results) {
        run.w.begin_object();
        run.w.member("name", r->name);
        run.w.member("pace", r->pace);
        run.w.member("speed", r->speed);
        run.w.member("recording", r->recording);
        run.w.member("ok", r->ok);
        run.w.member("first", r->first);
        run.w.member("last", r->last);
        run.w.member("messages", r->messages);
        run.w.member("datagrams", r->datagrams);
        run.w.member("kernel_drops", r->kernel_drops);
        run.w.member("requests", r->requests);
        run.w.member("unrecorded", r->unrecorded);
        run.w.member("sender_packets", r->sent.packets);
        run.w.member("sender_late_max_ns", r->sent.late_max_ns);
        run.w.member("sender_late_over_10us", r->sent.late_over_10us);
        run.w.member("sender_late_over_100us", r->sent.late_over_100us);
        run.w.key("packet_to_update");
        run.w.begin_object();
        run.w.member("count", r->c.count);
        run.w.member("overrun", r->c.overrun);
        run.w.member("remote", r->c.remote);
        run.w.member("p50_ns", r->c.p50);
        run.w.member("p99_ns", r->c.p99);
        run.w.member("p999_ns", r->c.p999);
        run.w.member("max_ns", r->c.max);
        run.w.end_object();
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "e2e_bench");
}
