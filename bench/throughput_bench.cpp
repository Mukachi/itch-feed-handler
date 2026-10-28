// The day's throughput (note 010, S3 and D6), on CPU 2: the whole sample day
// from a recording (note 009's replay, at full speed: the sequencer, the
// default decoder and a handler; no network, the kernel's part being note
// 007's) as messages a second:
//   fast              the handler on the default book, a sink that does nothing
//   fast, published   the same with the publisher and one consumer, its own
//                     process on CPU 3 (the publisher's node), reading only
//   reference         the reference handler on the reference book (note 003's
//                     and note 002's oracles; it has no publisher)
// The recording is made once from the day into the build directory (its
// packets as the sender packs them, synthetic kernel stamps 10 us apart) and
// read by every case; the replay's reading of it (from the page cache) is in
// the figure. Every case checked: the recording replayed completely, every
// case handling the same number of messages. Run
// JSON through lockfree-ipc's harness. Design: docs/design/010-measurement.md
//
// Usage: throughput_bench [output.json] [day.gz]
#include <itchfh/book.hpp>
#include <itchfh/consumer.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/publisher.hpp>
#include <itchfh/recording.hpp>
#include <itchfh/reference_book.hpp>
#include <itchfh/reference_handler.hpp>
#include <itchfh/replay.hpp>
#include <itchfh/session_store.hpp>

#include <lfipc/bench/clock_block.hpp>
#include <lfipc/bench/cross_cpu.hpp>
#include <lfipc/bench/noise_block.hpp>
#include <lfipc/bench/run_file.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace lfipc::bench;
namespace fh = itchfh;

constexpr int cpu = 2, consumer_cpu = 3;

struct quiet_sink {
    void book(const fh::applied&) {}
    void trade(const fh::trade_report&) {}
    void trade_broken(const fh::trade_report&) {}
    void status(std::uint16_t, fh::status_field) {}
    void imbalance(std::uint16_t, const fh::imbalance&) {}
    void market(const fh::message&) {}
    void directory(std::uint16_t, const fh::stock_directory&) {}
};

// The day as a recording: made once, then reused.
bool make_recording(const std::string& day, const std::string& path, std::string& why) {
    if (std::filesystem::exists(path)) {
        fh::rec::reader rd;
        if (rd.open(path, why)) return true;
        std::filesystem::remove(path);
    }
    const fh::mold::session_id session = {'T', 'H', 'R', 'O', 'U', 'G', 'H', 'P', 'U', 'T'};
    auto store = std::make_unique<fh::session_store>(session, 450'000'000, std::size_t{14} << 30, false);
    std::FILE* in = ::popen(("gzip -dc '" + day + "'").c_str(), "r");
    unsigned char buf[1 << 16];
    for (; in;) {
        unsigned char lb[2];
        if (std::fread(lb, 1, 2, in) != 2) break;
        const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
        if (len == 0 || std::fread(buf, 1, len, in) != len || !store->append(buf, len)) break;
    }
    if (in) ::pclose(in);
    if (store->count() == 0) {
        why = day + ": cannot read";
        return false;
    }
    fh::rec::recorder r;
    if (!r.open(path, 0, 1, why)) return false;
    fh::udp::datagram d;
    std::int64_t stamp = 0;
    for (std::uint64_t first = 1; first <= store->count();) {
        d.length = static_cast<std::uint32_t>(store->fill(first, ~0ull, d.bytes));
        first += fh::mold::read_header(d.bytes).count;
        d.kernel_ns = stamp += 10'000;
        while (r.backlog() > 8000) std::this_thread::yield();  // never left unrecorded here
        r.record(d, 0);
    }
    unsigned char end[fh::mold::header_size];
    fh::mold::build_marker(end, session, store->count() + 1, fh::mold::end_of_session);
    std::memcpy(d.bytes, end, sizeof(end));
    d.length = sizeof(end);
    d.kernel_ns = stamp += 10'000;
    r.record(d, 0);
    return r.close(0, 0);
}

struct case_result {
    std::string name;
    double seconds = 0;
    std::uint64_t messages = 0;
    bool ok = false;
};

template <class Handler>
case_result timed(const std::string& name, const std::string& path, Handler& h, std::uint64_t expect) {
    case_result c;
    c.name = name;
    fh::rec::reader rd;
    std::string why;
    if (!rd.open(path, why)) return c;
    const auto t0 = std::chrono::steady_clock::now();
    const fh::replay_result r = fh::replay(rd, h);
    c.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    c.messages = r.messages;
    c.ok = r.complete && (expect == 0 || r.messages == expect);
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string day = argc > 2 ? argv[2] : ITCHFH_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    const std::string path = std::string(ITCHFH_RECORD_DIR) + "/throughput-day.rec";
    run_file run(argc > 1 ? 2 : 1, argv, LFIPC_RESULTS_DIR, "throughput");
    if (run.refused) return run.finish(false, "throughput_bench");
    const auto noise = sample_noise({cpu}, 2.0);
    const auto clk = run_clock();
    if (!clk.ok || !lfipc::bench::detail::pin_to(cpu)) {
        write_clock_block(run.w, clk);
        std::fprintf(stderr, "throughput_bench: refused: %s\n", clk.ok ? "cannot pin to CPU 2" : clk.reason.c_str());
        return run.finish(false, "throughput_bench");
    }
    std::string why;
    if (!make_recording(day, path, why)) {
        std::fprintf(stderr, "throughput_bench: %s\n", why.c_str());
        return run.finish(false, "throughput_bench");
    }
    // Every case must replay the recording completely and handle as many messages as the first.
    const std::uint64_t expect = 0;
    std::vector<case_result> results;
    {
        auto b = std::make_unique<fh::default_book>();
        quiet_sink s;
        auto h = std::make_unique<fh::handler<fh::default_book, quiet_sink>>(*b, s);
        results.push_back(timed("fast", path, *h, expect));
    }
    {
        // The consumer forked first, before the book and publisher exist: forked after, the producer's
        // first write to each of their pages is a copy-on-write fault (note 010, Review).
        const std::string name = "/itchfh-tp-" + std::to_string(::getpid());
        int fds[2];
        if (::pipe(fds) != 0) return run.finish(false, "throughput_bench");
        const pid_t kid = ::fork();
        if (kid == 0) {
            ::close(fds[0]);
            lfipc::bench::detail::pin_to(consumer_cpu);
            fh::pub::consumer c;
            std::string w;
            for (int i = 0; i < 12'000 && !c.attach(name, w); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            c.report_node();
            bool overrun = false;
            fh::pub::update u;
            for (;;) {
                if (!c.poll(u)) {
                    if (c.overrun()) {
                        overrun = true;
                        break;
                    }
                    continue;
                }
                if (u.k == fh::pub::kind::feed && u.feed.state == fh::feed_state::ended) break;
            }
            [[maybe_unused]] const ssize_t n = ::write(fds[1], &overrun, sizeof(overrun));
            ::_exit(0);
        }
        ::close(fds[1]);
        auto b = std::make_unique<fh::default_book>();
        auto p = std::make_unique<fh::published<>>(*b);
        if (!p->pub.open(name, why)) {
            std::fprintf(stderr, "throughput_bench: %s\n", why.c_str());
            return run.finish(false, "throughput_bench");
        }
        for (int i = 0; i < 20'000'000; ++i) {
            p->tick(0);
            int live = 0;
            for (int c = 0; c < fh::pub::max_consumers; ++c) live += p->pub.slot(c) == fh::pub::slot_live;
            if (live) break;
        }
        case_result c = timed("fast, published to one consumer", path, *p, expect);
        bool overrun = true;
        [[maybe_unused]] const ssize_t n = ::read(fds[0], &overrun, sizeof(overrun));
        ::close(fds[0]);
        ::waitpid(kid, nullptr, 0);
        c.ok = c.ok && !overrun && p->pub.overruns() == 0;
        results.push_back(c);
    }
    {
        auto b = std::make_unique<fh::reference_book>();
        quiet_sink s;
        auto h = std::make_unique<fh::reference_handler<fh::reference_book, quiet_sink>>(*b, s);
        results.push_back(timed("reference", path, *h, expect));
    }
    bool ok = true;
    for (auto& c : results)
        if (c.messages != results[0].messages) c.ok = false;
    for (const auto& c : results) {
        ok = ok && c.ok;
        std::printf("%-32s %.2f M messages/s (%.1f s)%s\n", c.name.c_str(), static_cast<double>(c.messages) / c.seconds / 1e6, c.seconds,
                    c.ok ? "" : "  NOT OK");
    }
    write_noise_block(run.w, noise, clk.cal.ticks_per_ns);
    write_clock_block(run.w, clk);
    run.w.key("throughput");
    run.w.begin_object();
    run.w.member("cpu", cpu);
    run.w.member("consumer_cpu", consumer_cpu);
    run.w.member("source", day.substr(day.find_last_of('/') + 1));
    run.w.member("messages", results[0].messages);
    run.w.member("recording_bytes", static_cast<std::uint64_t>(std::filesystem::file_size(path)));
    run.w.member("lockfree_ipc_commit", ITCHFH_LFIPC_COMMIT);
    run.w.key("cases");
    run.w.begin_array();
    for (const auto& c : results) {
        run.w.begin_object();
        run.w.member("name", c.name);
        run.w.member("ok", c.ok);
        run.w.member("messages", c.messages);
        run.w.member("seconds", c.seconds);
        run.w.member("messages_per_s", c.seconds > 0 ? static_cast<double>(c.messages) / c.seconds : 0.0);
        run.w.end_object();
    }
    run.w.end_array();
    run.w.end_object();
    return run.finish(ok, "throughput_bench");
}
