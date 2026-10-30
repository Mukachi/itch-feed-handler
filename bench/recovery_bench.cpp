// Recovery from a snapshot at the day's worst point (note 011, D9 and S5),
// against feed_sender started with stamps /itchfh-stamps and a snapshot CPU
// (its GLIMPSE service on the request port + 2). On CPU 2, the program's
// components (one thread, the default book, the publisher, recording off,
// the recovery with its standby rebuilt on CPU 1); a consumer, its own
// process on CPU 3, forked first (note 010's copy-on-write finding), reads
// its ring and takes CLOCK_MONOTONIC less each message's intended send time.
// One run: the day at its own pace from 20 s before the peak of live orders
// (message 412,349,206, 15:57:45.69: note 006) to 20 s after; the program
// joins from a snapshot once the feed is flowing (D1's late join, measured
// too: lost on its first datagram; asked earlier, our service is still
// following the day to the start, minutes, and the client times out and
// retries, which is not what is measured); 2,000 messages from
// the peak are lost on purpose (not sent, requests for them refused), so the
// feed is lost and recovered. Each recovery from `lost` (or the start) to
// `live` with the hold drained, in three parts: asking until the spin's first
// byte (the server's build and the login), the spin read and applied in
// slices, the held feed delivered; with the spin's size and the packets held,
// and the kernel's drops over the recovery. Latency (intended send to the
// consumer's read) before the loss (from 5 s after the start: the join's own
// held messages left out), and for the messages of the 2 s of day time from
// the loss (held, then delivered), each message's first update
// counted once: a replay after a snapshot repeats sequence numbers already
// seen and is not a message's latency. Checked: two snapshots, none
// failed, the feed ended, the consumer neither overrun nor on another node.
// Run JSON through lockfree-ipc's harness.
// Design: docs/design/011-snapshot-recovery.md
//
// Usage: recovery_bench <loopback|card-busy-poll> <our ip> <sender ip> [output.json]
//   feed_sender must be serving with stamps /itchfh-stamps and a snapshot CPU:
//   group 239.192.0.1 port 26400, requests on 26401, control on 26402,
//   snapshots on 26403.
#include <itchfh/book.hpp>
#include <itchfh/consumer.hpp>
#include <itchfh/feed.hpp>
#include <itchfh/publisher.hpp>
#include <itchfh/recovery.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include "stamps.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace lfipc::bench;
namespace fh = itchfh;

constexpr int cpu = 2, consumer_cpu = 3, rebuild_cpu = 1;
constexpr std::uint64_t peak = 412'349'206, lost_messages = 2'000;
constexpr std::uint64_t peak_ns = 57'465'685'679'090;  // the peak message's ITCH time (15:57:45.685679)
constexpr std::uint64_t from_ns = peak_ns - 20'000'000'000, to_ns = peak_ns + 20'000'000'000, window_ns = 2'000'000'000;
constexpr std::uint64_t settle_ns = 5'000'000'000;  // "before the loss" from 5 s of day time after the start: the join and its backlog past
constexpr std::uint16_t port = 26400, request_port = 26401, control_port = 26402, snapshot_port = 26403;
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
    // The longest form: the day's own pace between two times, a range lost on purpose; answered with the span.
    bool run(const fh::mold::session_id& s, std::uint64_t lose_from, std::uint64_t lose_to, std::uint64_t (&span)[2]) {
        unsigned char b[75];
        const double pace = 0, drop = 0, speed = 1;
        const std::uint64_t last = 0, from = from_ns, to_t = to_ns;
        b[0] = 'R';
        std::memcpy(b + 1, s.data(), 10);
        std::memcpy(b + 11, &pace, 8);
        std::memcpy(b + 19, &drop, 8);
        std::memcpy(b + 27, &last, 8);
        std::memcpy(b + 35, &speed, 8);
        std::memcpy(b + 43, &from, 8);
        std::memcpy(b + 51, &to_t, 8);
        std::memcpy(b + 59, &lose_from, 8);
        std::memcpy(b + 67, &lose_to, 8);
        ::sendto(fd, b, sizeof(b), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        return ::recv(fd, span, sizeof(span), 0) == static_cast<ssize_t>(sizeof(span));
    }
    bool stop(fh::sender_counts& c) {
        const unsigned char s = 'S';
        ::sendto(fd, &s, 1, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        return ::recv(fd, &c, sizeof(c), 0) == static_cast<ssize_t>(sizeof(c));
    }
};

struct latency {
    std::uint64_t count = 0;
    double p50 = -1, p99 = -1, p999 = -1, max = -1;  // ns
};
struct consumer_report {
    latency before, after;  // from 5 s after the start to the loss; the 2 s of day time from it
    bool attached = false, overrun = false, remote = false;
};

latency summary(const hist& h) {
    const auto v = [](std::optional<std::uint64_t> x) { return x ? static_cast<double>(*x) : -1.0; };
    return {h.count(), v(h.percentile(50)), v(h.percentile(99)), v(h.percentile(99.9)), static_cast<double>(h.max())};
}

// The consumer process: on CPU 3, its ring read until the feed ends; each
// message's first update, by a sequence number newer than any seen.
void consumer_main(const std::string& name, int out) {
    lfipc::bench::detail::pin_to(consumer_cpu);
    consumer_report r;
    std::string why;
    fh::bench::stamp_array* st = fh::bench::open_stamps(stamps_name, false, why);
    fh::pub::consumer c;
    for (int i = 0; i < 12'000 && !c.attach(name, why); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    r.attached = st && c.index() >= 0;
    if (r.attached) {
        c.report_node();
        r.remote = fh::pub::node_of(::sched_getcpu()) != c.publisher_node();
        auto before = std::make_unique<hist>(), after = std::make_unique<hist>();
        std::uint64_t newest = 0;
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
            if (u.k == fh::pub::kind::feed) {
                if (u.feed.state == fh::feed_state::ended) break;
                continue;
            }
            if (u.k == fh::pub::kind::reset || u.sequence <= newest) continue;
            newest = u.sequence;
            const auto d = static_cast<std::uint64_t>(now - st->get(u.sequence));
            if (u.timestamp_ns >= from_ns + settle_ns && u.timestamp_ns < peak_ns) before->record(d);
            else if (u.timestamp_ns < peak_ns + window_ns) after->record(d);
        }
        r.before = summary(*before);
        r.after = summary(*after);
    }
    [[maybe_unused]] const ssize_t n = ::write(out, &r, sizeof(r));
}

void write_recovery(json_writer& w, const char* key, const fh::recovery_counts& c, std::uint64_t drops) {
    w.key(key);
    w.begin_object();
    w.member("ns", c.last_ns);
    w.member("wait_ns", c.last_wait_ns);
    w.member("apply_ns", c.last_apply_ns);
    w.member("backlog_ns", c.last_backlog_ns);
    w.member("spin_bytes", c.last_bytes);
    w.member("spin_messages", c.last_messages);
    w.member("packets_held", c.last_held);
    w.member("kernel_drops", drops);
    w.end_object();
}
void write_latency(json_writer& w, const char* key, const latency& l) {
    w.key(key);
    w.begin_object();
    w.member("count", l.count);
    w.member("p50_ns", l.p50);
    w.member("p99_ns", l.p99);
    w.member("p999_ns", l.p999);
    w.member("max_ns", l.max);
    w.end_object();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: recovery_bench <loopback|card-busy-poll> <our ip> <sender ip> [output.json]\n");
        return 2;
    }
    const std::string path_name = argv[1], our_ip = argv[2], sender_ip = argv[3];
    char* run_argv[] = {argv[0], argc > 4 ? argv[4] : nullptr, nullptr};
    run_file run(argc > 4 ? 2 : 1, run_argv, LFIPC_RESULTS_DIR, "recovery");
    if (run.refused) return run.finish(false, "recovery_bench");
    const auto noise = sample_noise({cpu, consumer_cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !lfipc::bench::detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "recovery_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "recovery_bench");
    }
    std::string why;
    fh::bench::stamp_array* st = fh::bench::open_stamps(stamps_name, false, why);
    control ctl;
    fh::sender_counts probe;
    if (!st || !ctl.open(sender_ip) || !ctl.stop(probe)) {
        std::fprintf(stderr, "recovery_bench: feed_sender with stamps %s does not answer on %s:%u (%s)\n", stamps_name, sender_ip.c_str(), control_port,
                     why.c_str());
        return run.finish(false, "recovery_bench");
    }
    const std::string name = "/itchfh-rcb-" + std::to_string(::getpid());
    int fds[2];
    if (::pipe(fds) != 0) return run.finish(false, "recovery_bench");
    const pid_t kid = ::fork();  // first, before anything it would share is built (note 010, Review)
    if (kid == 0) {
        ::close(fds[0]);
        consumer_main(name, fds[1]);
        ::_exit(0);
    }
    ::close(fds[1]);
    fh::udp::receiver rx;
    fh::udp::receiver::options ro;
    ro.interface_ip = our_ip;
    ro.port = port;
    if (!rx.open(ro, why)) {
        std::fprintf(stderr, "recovery_bench: %s\n", why.c_str());
        return run.finish(false, "recovery_bench");
    }
    rx.set_server(sender_ip, request_port);
    auto pub = std::make_unique<fh::publisher<fh::default_book>>();
    if (!pub->open(name, why)) {
        std::fprintf(stderr, "recovery_bench: %s\n", why.c_str());
        return run.finish(false, "recovery_bench");
    }
    fh::recovery_config rc;
    rc.ip = sender_ip;
    rc.port = snapshot_port;
    rc.rebuild_cpu = rebuild_cpu;
    rc.join = false;  // lost by hand on the first datagram, below
    auto rcv = std::make_unique<fh::recovery<>>(*pub, fh::book_config{}, fh::handler_config{}, rc);
    fh::sequencer_config qc;
    qc.hold_bytes = std::size_t{256} << 20;  // D5's default
    // The consumer joined (its replay of the empty book done) before anything is sent.
    for (int i = 0; i < 20'000'000; ++i) {
        pub->tick(fh::monotonic_ns());
        int live = 0;
        for (int c = 0; c < fh::pub::max_consumers; ++c) live += pub->slot(c) == fh::pub::slot_live;
        if (live) break;
    }
    const fh::mold::session_id session = {'R', 'E', 'C', 'O', 'V', 'B', 'E', 'N', 'C', 'H'};
    std::uint64_t span[2] = {0, 0};
    if (!ctl.run(session, peak, peak + lost_messages - 1, span)) {
        std::fprintf(stderr, "recovery_bench: feed_sender did not answer the run\n");
        return run.finish(false, "recovery_bench");
    }
    std::printf("recovery_bench: messages %llu..%llu at the day's own pace (the sender first follows the day to the start: minutes)\n",
                static_cast<unsigned long long>(span[0]), static_cast<unsigned long long>(span[1]));
    std::fflush(stdout);
    qc.first_expected = span[0];
    fh::feed_one<fh::published_on<>> f(rx, rcv->active(), qc);
    fh::recovery_counts join{}, loss{};
    bool joined = false, lost = false, recovered = false;
    std::uint64_t drops_at_start = 0, drops_at_loss = 0, join_drops = 0, loss_drops = 0;
    const std::int64_t give_up = fh::monotonic_ns() + 1'800'000'000'000;  // the sender's catching up, then 40 s
    bool flowing = false;
    std::int64_t flowing_at = 0, join_done_at = 0, loss_done_at = 0, join_rebuild_from = 0, join_rebuild_to = 0;
    while (f.seq().state() != fh::feed_state::ended && fh::monotonic_ns() < give_up) {
        f.poll();
        const std::int64_t now = fh::monotonic_ns();
        if (!flowing && f.counts().datagrams > 0) {  // the late join: what the first packet began is dropped with the rest below N
            flowing = true;
            flowing_at = now;
            drops_at_start = f.counts().kernel_drops;
            f.seq().lose();
        }
        if (rcv->step(f, now)) fh::recovery<>::drain(f);
        if (!joined && rcv->counts().snapshots == 1) {
            joined = true;
            join = rcv->counts();
            join_drops = f.counts().kernel_drops - drops_at_start;
            join_done_at = now;
        }
        if (joined && !lost && f.seq().state() == fh::feed_state::lost) {
            lost = true;
            drops_at_loss = f.counts().kernel_drops;
            join_rebuild_from = rcv->rebuild_started_ns();  // the join's rebuild, long done by the loss
            join_rebuild_to = rcv->rebuild_ended_ns();
        }
        if (lost && !recovered && rcv->counts().snapshots == 2) {
            recovered = true;
            loss = rcv->counts();
            loss_drops = f.counts().kernel_drops - drops_at_loss;
            loss_done_at = now;
        }
    }
    fh::sender_counts sent{};
    ctl.stop(sent);
    if (f.seq().state() != fh::feed_state::ended) pub->feed(fh::feed_state::ended, 0);  // so the consumer stops
    consumer_report cr;
    const bool got = ::read(fds[0], &cr, sizeof(cr)) == static_cast<ssize_t>(sizeof(cr));
    ::close(fds[0]);
    ::waitpid(kid, nullptr, 0);
    const auto& rcn = rcv->counts();
    const bool ok = got && cr.attached && !cr.overrun && !cr.remote && f.seq().state() == fh::feed_state::ended && joined && recovered &&
                    rcn.snapshots == 2 && rcn.failures == 0 && sent.lost_on_purpose > 0;
    const auto ms = [](std::int64_t ns) { return static_cast<double>(ns) / 1e6; };
    std::printf("join: %.1f ms (wait %.1f, apply %.1f, backlog %.1f), %llu bytes, %llu packets held, %llu kernel drops\n", ms(join.last_ns),
                ms(join.last_wait_ns), ms(join.last_apply_ns), ms(join.last_backlog_ns), static_cast<unsigned long long>(join.last_bytes),
                static_cast<unsigned long long>(join.last_held), static_cast<unsigned long long>(join_drops));
    std::printf("loss at the peak: %.1f ms (wait %.1f, apply %.1f, backlog %.1f), %llu bytes, %llu packets held, %llu kernel drops\n", ms(loss.last_ns),
                ms(loss.last_wait_ns), ms(loss.last_apply_ns), ms(loss.last_backlog_ns), static_cast<unsigned long long>(loss.last_bytes),
                static_cast<unsigned long long>(loss.last_held), static_cast<unsigned long long>(loss_drops));
    std::printf("latency, intended send to read: before the loss p50 %.1f us p99 %.1f us max %.1f us; 2 s from the loss p50 %.1f ms p99 %.1f ms max %.1f ms%s\n",
                cr.before.p50 / 1e3, cr.before.p99 / 1e3, cr.before.max / 1e3, cr.after.p50 / 1e6, cr.after.p99 / 1e6, cr.after.max / 1e6,
                ok ? "" : "  NOT OK");
    write_noise_block(run.w, noise, clk.cal.ticks_per_ns);
    write_clock_block(run.w, clk);
    run.w.key("recovery");
    run.w.begin_object();
    run.w.member("path", path_name);
    run.w.member("our_ip", our_ip);
    run.w.member("sender_ip", sender_ip);
    run.w.member("cpu", cpu);
    run.w.member("consumer_cpu", consumer_cpu);
    run.w.member("rebuild_cpu", rebuild_cpu);
    run.w.member("lockfree_ipc_commit", ITCHFH_LFIPC_COMMIT);
    run.w.member("ok", ok);
    run.w.member("first", span[0]);
    run.w.member("last", span[1]);
    run.w.member("lost_from", peak);
    run.w.member("lost_to", peak + lost_messages - 1);
    run.w.member("hold_mb", static_cast<std::uint64_t>(256));
    run.w.member("snapshots", rcn.snapshots);
    run.w.member("failures", rcn.failures);
    run.w.member("waits", rcn.waits);
    run.w.member("sender_lost_on_purpose", sent.lost_on_purpose);
    run.w.member("sender_refused", sent.refused);
    run.w.member("sender_late_max_ns", sent.late_max_ns);
    run.w.member("consumer_overrun", cr.overrun);
    run.w.member("consumer_remote", cr.remote);
    // The standby's rebuild after the join, and after the loss (by the end), in ms from the feed's first datagram.
    for (;; std::this_thread::sleep_for(std::chrono::milliseconds(10)))
        if (rcv->standby_ready()) break;
    const auto rel = [&](std::int64_t t) { return t > 0 ? static_cast<double>(t - flowing_at) / 1e6 : -1.0; };
    run.w.member("join_done_ms", rel(join_done_at));
    run.w.member("join_rebuild_from_ms", rel(join_rebuild_from));
    run.w.member("join_rebuild_to_ms", rel(join_rebuild_to));
    run.w.member("loss_done_ms", rel(loss_done_at));
    run.w.member("loss_rebuild_from_ms", rel(rcv->rebuild_started_ns()));
    run.w.member("loss_rebuild_to_ms", rel(rcv->rebuild_ended_ns()));
    write_recovery(run.w, "join", join, join_drops);
    write_recovery(run.w, "loss", loss, loss_drops);
    write_latency(run.w, "latency_before", cr.before);
    write_latency(run.w, "latency_after_loss", cr.after);
    run.w.end_object();
    return run.finish(ok, "recovery_bench");
}
