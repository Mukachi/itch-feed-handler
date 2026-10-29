// The feed handler (note 009, D7): one feed, from a configuration file,
// received (note 007: feed_one, the default arrangement), sequenced (005),
// decoded (004) and handled (003) onto the default book (002), published
// (008), recorded and monitored (009), until the end of session or SIGINT or
// SIGTERM. With --replay, a recording in place of the sockets (D5). A lost
// feed is recovered from a snapshot, or joined from one (note 011).
// Design: docs/design/009-recording-monitoring.md
//
// Usage: itchfh <config> [--replay <recording> [--paced] [--wait-consumers N]]
#include <itchfh/book.hpp>
#include <itchfh/config.hpp>
#include <itchfh/feed.hpp>
#include <itchfh/monitoring.hpp>
#include <itchfh/publisher.hpp>
#include <itchfh/recording.hpp>
#include <itchfh/recovery.hpp>
#include <itchfh/replay.hpp>
#include <itchfh/udp.hpp>

#include <lfipc/bench/cross_cpu.hpp>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>

namespace {

std::atomic<bool> stop{false};
void on_signal(int) { stop.store(true, std::memory_order_relaxed); }

std::string utc_stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::gmtime_r(&t, &tm);
    char b[32];
    std::strftime(b, sizeof(b), "%Y%m%dT%H%M%SZ", &tm);
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    namespace fh = itchfh;
    if (argc < 2) {
        std::fprintf(stderr, "usage: itchfh <config> [--replay <recording> [--paced] [--wait-consumers N]]\n");
        return 2;
    }
    fh::program_config cfg;
    std::string why;
    if (!fh::load_config(argv[1], cfg, why)) {
        std::fprintf(stderr, "itchfh: %s\n", why.c_str());
        return 2;
    }
    const std::string replay_path = argc > 3 && std::string(argv[2]) == "--replay" ? argv[3] : "";
    bool paced = false;
    int wait_consumers = 0;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--paced") paced = true;
        else if (a == "--wait-consumers" && i + 1 < argc) wait_consumers = std::atoi(argv[++i]);
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    fh::book_config bc;
    bc.orders = cfg.orders;
    bc.levels = cfg.levels;
    fh::sequencer_config qc;
    qc.hold_bytes = static_cast<std::size_t>(cfg.hold_mb) << 20;

    if (!replay_path.empty()) {
        auto book = std::make_unique<fh::default_book>(bc);
        auto p = std::make_unique<fh::published<>>(*book);
        if (!cfg.publish.empty() && !p->pub.open(cfg.publish, why)) {
            std::fprintf(stderr, "itchfh: publish: %s\n", why.c_str());
            return 1;
        }
        fh::rec::reader rd;
        if (!rd.open(replay_path, why)) {
            std::fprintf(stderr, "itchfh: %s\n", why.c_str());
            return 1;
        }
        // Consumers that are to see the replay from its start join first (up to a minute).
        for (int i = 0; i < 60'000 && !stop.load(std::memory_order_relaxed); ++i) {
            p->tick(fh::monotonic_ns());
            int live = 0;
            for (int c = 0; c < fh::pub::max_consumers; ++c) live += p->pub.slot(c) == fh::pub::slot_live;
            if (live >= wait_consumers) break;
            ::usleep(1000);
        }
        const fh::replay_result r = fh::replay(rd, *p, qc, paced, &stop);
        const char* stopped = r.stopped == fh::rec::reader::result::end ? "end" : r.stopped == fh::rec::reader::result::truncated ? "truncated" : "corrupt";
        std::printf("itchfh: replayed %s: %llu records, %llu messages, %llu undecoded; stopped at %s; feed %d, %zu held; %llu datagrams "
                    "left unrecorded; %s\n",
                    replay_path.c_str(), static_cast<unsigned long long>(r.records), static_cast<unsigned long long>(r.messages),
                    static_cast<unsigned long long>(r.undecoded), stopped, static_cast<int>(r.state), r.held,
                    static_cast<unsigned long long>(r.skipped), r.complete ? "complete" : "INCOMPLETE");
        return r.complete ? 0 : 1;
    }

    if (!lfipc::bench::detail::pin_to(cfg.cpu)) {
        std::fprintf(stderr, "itchfh: cannot pin to CPU %d\n", cfg.cpu);
        return 1;
    }
    fh::udp::receiver rx;
    fh::udp::receiver::options ro;
    ro.group = cfg.group;
    ro.interface_ip = cfg.interface_ip;
    ro.port = cfg.port;
    ro.buffer_bytes = cfg.buffer_bytes;
    ro.busy_poll_us = cfg.busy_poll_us;
    if (!rx.open(ro, why)) {
        std::fprintf(stderr, "itchfh: %s\n", why.c_str());
        return 1;
    }
    rx.set_server(cfg.server_ip, cfg.server_port);
    // Note 011: both books (the live one and the standby) and the publisher
    // they share, built on this CPU's node once pinned.
    auto pub = std::make_unique<fh::publisher<fh::default_book>>();
    if (!cfg.publish.empty() && !pub->open(cfg.publish, why)) {
        std::fprintf(stderr, "itchfh: publish: %s\n", why.c_str());
        return 1;
    }
    fh::recovery_config rc;
    rc.ip = cfg.snapshot_ip;
    rc.port = cfg.snapshot_port;
    rc.rebuild_cpu = cfg.rebuild_cpu;
    rc.join = cfg.join_snapshot && cfg.snapshot_port != 0;
    auto rcv = std::make_unique<fh::recovery<>>(*pub, bc, fh::handler_config{}, rc);
    std::unique_ptr<fh::rec::recorder> rec;
    std::string rec_path;
    if (!cfg.record_dir.empty()) {
        rec = std::make_unique<fh::rec::recorder>();
        rec_path = cfg.record_dir + "/itchfh-" + utc_stamp() + ".rec";
        if (!rec->open(rec_path, cfg.digest, cfg.writer_cpu, why)) {
            std::fprintf(stderr, "itchfh: record: %s\n", why.c_str());
            return 1;
        }
    }
    fh::mon::block mon;
    if (!cfg.monitor.empty() && !mon.create(cfg.monitor, why)) {
        std::fprintf(stderr, "itchfh: monitor: %s\n", why.c_str());
        return 1;
    }
    fh::feed_one<fh::published_on<>, fh::no_probe, fh::rec::recorder> f(rx, rcv->active(), qc, cfg.batch);
    if (rec) f.set_recorder(*rec);
    rcv->begin(f);
    std::fprintf(stderr, "itchfh: %s:%u on %s, CPU %d%s%s%s%s\n", cfg.group.c_str(), cfg.port, cfg.interface_ip.c_str(), cfg.cpu,
                 cfg.publish.empty() ? "" : ", publishing to ", cfg.publish.c_str(), rec ? ", recording to " : "", rec_path.c_str());
    if (cfg.snapshot_port != 0)
        std::fprintf(stderr, "itchfh: snapshots from %s:%u%s\n", cfg.snapshot_ip.c_str(), cfg.snapshot_port, rc.join ? ", joining from one" : "");
    fh::mon::values v{};
    std::int64_t last_mon = 0;
    while (!stop.load(std::memory_order_relaxed) && f.seq().state() != fh::feed_state::ended) {
        f.poll();
        const std::int64_t now = fh::monotonic_ns();
        if (cfg.snapshot_port != 0 && rcv->step(f, now)) fh::recovery<>::drain(f);  // D4: the sockets read between slices
        if (!cfg.monitor.empty() && now - last_mon >= 100'000'000) {
            fh::mon::gather(v, f, rcv->book(), pub.get(), rec.get(), now);
            fh::mon::gather_recovery(v, *rcv);
            mon.write(v);
            last_mon = now;
        }
    }
    bool ok = true;
    if (rec) ok = rec->close(f.counts().kernel_drops, f.counts().answer_drops);
    if (!cfg.monitor.empty()) {
        fh::mon::gather(v, f, rcv->book(), pub.get(), rec.get(), fh::monotonic_ns());
        fh::mon::gather_recovery(v, *rcv);
        mon.write(v);
    }
    std::printf("itchfh: %s; %llu datagrams, %llu messages, %llu kernel drops, %llu re-requests, %llu gaps%s%llu recorded, %llu unrecorded\n",
                f.seq().state() == fh::feed_state::ended ? "session ended" : "stopped", static_cast<unsigned long long>(f.counts().datagrams),
                static_cast<unsigned long long>(f.counts().messages), static_cast<unsigned long long>(f.counts().kernel_drops),
                static_cast<unsigned long long>(f.seq().counts().requests), static_cast<unsigned long long>(f.seq().counts().gaps), "; ",
                static_cast<unsigned long long>(rec ? rec->written() : 0), static_cast<unsigned long long>(rec ? rec->unrecorded() : 0));
    if (cfg.snapshot_port != 0)
        std::printf("itchfh: %llu snapshots applied, %llu failed; last recovery %.1f ms\n", static_cast<unsigned long long>(rcv->counts().snapshots),
                    static_cast<unsigned long long>(rcv->counts().failures), static_cast<double>(rcv->counts().last_ns) / 1e6);
    return ok ? 0 : 1;
}
