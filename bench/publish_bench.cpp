// The publisher's cost (note 008, S6 and D8), on CPU 2, its consumers each a
// process of their own on CPUs 3, 4, 5 and 6 (separate cores of the same
// node, forked after the harness binds memory there). The first 50 million
// messages of the sample day, decoded into memory first, through a fresh
// default book and handler in each case:
//   no publisher     the handler with a sink that does nothing (note 003's)
//   table only       the handler and its publisher, no consumer attached
//   1, 2, 4          consumers attached, each reading its ring
// Each message's handling timed between start() and stop(), unsubtracted, the
// first 10,000 discarded; the publisher's own cost is the difference from
// "no publisher". Write to read: before each message the publisher's thread
// stores the cycle counter for its sequence number in a shared array; a
// consumer reading an update takes after_load() and subtracts the stamp of
// its sequence number (so it includes the handling up to the push). At full
// speed (the cost pass: queueing, if a consumer falls behind) and paced, the
// first 2 million messages at a million a second. A consumer overrun is
// counted (it voids that case's write-to-read). Run JSON through
// lockfree-ipc's harness. Design: docs/design/008-publication.md
//
// Usage: publish_bench [output.json] [day.gz]
#include <itchfh/book.hpp>
#include <itchfh/consumer.hpp>
#include <itchfh/decoder.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/publisher.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/histogram.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace lfipc::bench;
namespace fh = itchfh;

constexpr int cpu = 2;
constexpr int consumer_cpus[] = {3, 4, 5, 6};
constexpr std::size_t prefix = 50'000'000, paced = 2'000'000, warmup = 10'000;
constexpr double paced_rate = 1'000'000;
constexpr std::size_t stamps_n = 1u << 20;
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

struct summary {
    std::uint64_t count = 0, updates = 0;
    double p50 = -1, p99 = -1, p999 = -1, max = -1;  // ticks
    bool overrun = false;
};
summary summarise(const hist& h) {
    summary s;
    s.count = h.count();
    const auto t = [](std::optional<ticks> v) { return v ? static_cast<double>(*v) : -1.0; };
    s.p50 = t(h.percentile(50));
    s.p99 = t(h.percentile(99));
    s.p999 = t(h.percentile(99.9));
    s.max = static_cast<double>(h.max());
    return s;
}

// A consumer process: reads its ring until the feed's end, twice (the full
// speed pass, then the paced one), the write-to-read of each update recorded.
void consumer_main(const std::string& name, int my_cpu, const volatile ticks* stamps, int out) {
    lfipc::bench::detail::pin_to(my_cpu);
    fh::pub::consumer c;
    std::string why;
    summary res[2];
    bool attached = false;
    for (int i = 0; i < 12'000 && !(attached = c.attach(name, why)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));  // the producer builds first
    if (attached) {
        for (int pass = 0; pass < 2; ++pass) {
            auto h = std::make_unique<hist>();
            std::uint64_t updates = 0, seen = 0;
            fh::pub::update u;
            for (;;) {
                if (!c.poll(u)) {
                    if (c.overrun()) res[pass].overrun = true;
                    continue;
                }
                const ticks now = lfipc::bench::detail::after_load();
                ++updates;
                if (u.k == fh::pub::kind::feed) {
                    if (u.feed.state == fh::feed_state::ended) break;
                    continue;
                }
                if (u.k == fh::pub::kind::reset || u.sequence == 0) continue;
                if (++seen > warmup) h->record(now - stamps[u.sequence & (stamps_n - 1)]);
            }
            res[pass] = {summarise(*h).count, updates, summarise(*h).p50, summarise(*h).p99, summarise(*h).p999, summarise(*h).max, res[pass].overrun};
        }
    }
    [[maybe_unused]] const ssize_t n = ::write(out, res, sizeof(res));
}

struct case_result {
    std::string name;
    int consumers = 0;
    double messages_per_s = 0;
    hist cost;
    std::uint64_t overruns = 0;
    std::vector<summary> full, slow;
};

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 2 ? argv[2] : ITCHFH_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "publish");
    if (run.refused) return run.finish(false, "publish_bench");
    const auto noise = sample_noise({cpu, consumer_cpus[0], consumer_cpus[1], consumer_cpus[2], consumer_cpus[3]}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !lfipc::bench::detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "publish_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "publish_bench");
    }
    const double tpn = clk.cal.ticks_per_ns;

    // The day's first messages, decoded, on CPU 2's node.
    std::vector<fh::message> msgs;
    msgs.reserve(prefix);
    {
        std::FILE* in = ::popen(("gzip -dc '" + path + "'").c_str(), "r");
        unsigned char buf[1 << 16];
        while (in && msgs.size() < prefix) {
            unsigned char lb[2];
            if (std::fread(lb, 1, 2, in) != 2) break;
            const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
            if (len == 0 || std::fread(buf, 1, len, in) != len) break;
            fh::message m;
            if (fh::decode(buf, len, m) == fh::decode_status::ok) msgs.push_back(m);
        }
        if (in) ::pclose(in);
    }
    if (msgs.size() < prefix) {
        std::fprintf(stderr, "publish_bench: %s: %zu messages read\n", path.c_str(), msgs.size());
        return run.finish(false, "publish_bench");
    }
    auto* stamps = static_cast<volatile ticks*>(::mmap(nullptr, stamps_n * sizeof(ticks), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE, -1, 0));

    std::vector<std::unique_ptr<case_result>> results;
    bool ok = true;
    const std::string name = "/itchfh-pb-" + std::to_string(::getpid());
    for (const int consumers : {-1, 0, 1, 2, 4}) {
        auto r = std::make_unique<case_result>();
        r->consumers = consumers < 0 ? 0 : consumers;
        r->name = consumers < 0 ? "no publisher" : consumers == 0 ? "table only" : std::to_string(consumers) + (consumers == 1 ? " consumer" : " consumers");
        // Consumers forked first, before the book and publisher exist: forked after, the producer's first
        // write to each of their pages is a copy-on-write fault, and each further fork shares them again
        // (note 008, Review: found by note 010).
        std::vector<pid_t> kids;
        std::vector<int> pipes;
        for (int k = 0; k < consumers; ++k) {
            int fds[2];
            if (::pipe(fds) != 0) return run.finish(false, "publish_bench");
            const pid_t kid = ::fork();
            if (kid == 0) {
                ::close(fds[0]);
                consumer_main(name, consumer_cpus[k], stamps, fds[1]);
                ::_exit(0);
            }
            ::close(fds[1]);
            kids.push_back(kid);
            pipes.push_back(fds[0]);
        }
        auto book = std::make_unique<fh::default_book>();
        if (consumers < 0) {
            quiet_sink sink;
            auto h = std::make_unique<fh::handler<fh::default_book, quiet_sink>>(*book, sink);
            const auto t0 = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < msgs.size(); ++i) {
                const ticks a = start();
                h->on(msgs[i]);
                const ticks b = stop();
                if (i >= warmup) r->cost.record(b - a);
            }
            r->messages_per_s = static_cast<double>(msgs.size()) / std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        } else {
            auto p = std::make_unique<fh::published<>>(*book);
            std::string why;
            if (!p->pub.open(name, why)) {
                std::fprintf(stderr, "publish_bench: %s\n", why.c_str());
                return run.finish(false, "publish_bench");
            }
            // Every consumer joined (replay of the empty book done) before the clock starts.
            for (int live = 0; live < consumers;) {
                p->tick(0);
                live = 0;
                for (int k = 0; k < fh::pub::max_consumers; ++k) live += p->pub.slot(k) == fh::pub::slot_live;
            }
            const auto t0 = std::chrono::steady_clock::now();
            std::uint64_t seq = 0;
            for (std::size_t i = 0; i < msgs.size(); ++i) {
                stamps[(seq + 1) & (stamps_n - 1)] = stamp();
                const ticks a = start();
                p->on(++seq, msgs[i]);
                const ticks b = stop();
                if (i >= warmup) r->cost.record(b - a);
            }
            r->messages_per_s = static_cast<double>(msgs.size()) / std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            p->feed(fh::feed_state::ended, seq + 1);
            // The paced pass: the same book goes on; a fresh one would replay (D5). Sequence numbers continue.
            if (consumers > 0) {
                const double gap = 1e9 * tpn / paced_rate;
                const ticks begin = stamp();
                for (std::size_t i = 0; i < paced; ++i) {
                    const auto due = begin + static_cast<ticks>(gap * static_cast<double>(i));
                    while (stamp() < due) {
                    }
                    stamps[(seq + 1) & (stamps_n - 1)] = stamp();
                    p->on(++seq, msgs[i]);
                }
                p->feed(fh::feed_state::ended, seq + 1);
            }
            for (int k = 0; k < consumers; ++k) {
                summary s[2];
                const bool got = ::read(pipes[k], s, sizeof(s)) == static_cast<ssize_t>(sizeof(s));
                ::close(pipes[k]);
                ::waitpid(kids[k], nullptr, 0);
                if (!got) ok = false;
                r->full.push_back(s[0]);
                r->slow.push_back(s[1]);
            }
            r->overruns = p->pub.overruns();
        }
        const auto ns = [&](std::optional<ticks> t) { return t ? static_cast<double>(*t) / tpn : -1.0; };
        std::printf("%-13s %.2f M messages/s, p50 %.0f ns, p99 %.0f ns a message; overruns %llu", r->name.c_str(), r->messages_per_s / 1e6,
                    ns(r->cost.percentile(50)), ns(r->cost.percentile(99)), static_cast<unsigned long long>(r->overruns));
        for (const summary& s : r->slow) std::printf("; paced write to read p50 %.0f ns p99 %.0f", s.p50 / tpn, s.p99 / tpn);
        std::printf("\n");
        std::fflush(stdout);
        results.push_back(std::move(r));
    }

    write_noise_block(run.w, noise, tpn);
    write_clock_block(run.w, clk);
    const auto ns = [&](std::optional<ticks> t) { return t ? static_cast<double>(*t) / tpn : -1.0; };
    const auto write_summary = [&](const summary& s) {
        run.w.begin_object();
        run.w.member("count", s.count);
        run.w.member("updates", s.updates);
        run.w.member("overrun", s.overrun);
        run.w.member("p50_ns", s.p50 < 0 ? -1.0 : s.p50 / tpn);
        run.w.member("p99_ns", s.p99 < 0 ? -1.0 : s.p99 / tpn);
        run.w.member("p999_ns", s.p999 < 0 ? -1.0 : s.p999 / tpn);
        run.w.member("max_ns", s.max < 0 ? -1.0 : s.max / tpn);
        run.w.end_object();
    };
    run.w.key("publish");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.key("consumer_cpus");
    run.w.begin_inline_array();
    for (const int c : consumer_cpus) run.w.value(c);
    run.w.end_array();
    run.w.member("source", path.substr(path.find_last_of('/') + 1));
    run.w.member("messages", static_cast<std::uint64_t>(prefix));
    run.w.member("paced_messages", static_cast<std::uint64_t>(paced));
    run.w.member("paced_rate", paced_rate);
    run.w.member("warmup_discarded", static_cast<std::uint64_t>(warmup));
    run.w.member("lockfree_ipc_commit", ITCHFH_LFIPC_COMMIT);
    run.w.key("cases");
    run.w.begin_array();
    for (const auto& r : results) {
        run.w.begin_object();
        run.w.member("name", r->name);
        run.w.member("consumers", r->consumers);
        run.w.member("messages_per_s", r->messages_per_s);
        run.w.member("overruns", r->overruns);
        run.w.key("cost");
        run.w.begin_object();
        run.w.member("count", r->cost.count());
        run.w.member("p50_ns", ns(r->cost.percentile(50)));
        run.w.member("p99_ns", ns(r->cost.percentile(99)));
        run.w.member("p999_ns", ns(r->cost.percentile(99.9)));
        run.w.member("max_ns", static_cast<double>(r->cost.max()) / tpn);
        run.w.end_object();
        run.w.key("write_to_read_full_speed");
        run.w.begin_array();
        for (const summary& s : r->full) write_summary(s);
        run.w.end_array();
        run.w.key("write_to_read_paced");
        run.w.begin_array();
        for (const summary& s : r->slow) write_summary(s);
        run.w.end_array();
        run.w.end_object();
        if (r->overruns) ok = false;
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "publish_bench");
}
