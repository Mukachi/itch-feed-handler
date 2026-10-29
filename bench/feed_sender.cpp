// Our MoldUDP64 sender as its own process (note 007, D6), for feed_bench and
// the card path: started first (for the card, inside the far port's network
// namespace: `sudo ip netns exec itchfh-peer bench-run feed_sender ...`, which
// keeps the namespace), it loads the sample day into a session store and then
// waits. feed_bench drives it over a control port (the request port + 1),
// one datagram a command:
//   R  run: session (10 bytes), packets a second (double), share dropped
//      (double), last message (u64, 0 the whole day): sends messages 1..last
//      onto the group, then the end of session until the next command,
//      answering re-requests throughout (mold_sender.hpp); and, for note 010,
//      a longer form adding the day's own pace sped up by (double, 0: the
//      fixed pace) and the ITCH times from and to (u64 ns since midnight, 0:
//      from 1 and to `last`), answered with the first and last messages the
//      times resolve to (two u64), which the receiver must expect; and, for
//      note 011, a form longer again adding messages lost on purpose, from
//      and to (two u64, 0: none): their packets not sent, requests for them
//      refused
//   S  stop the run; the reply is its counts (sender_counts, as laid out here)
//   Q  quit
// The sending runs on `cpu` (a reserved one); the control thread blocks on
// its socket, pinned to `control cpu` (a housekeeping one). Given a stamps
// name, it writes each message's intended send time there (note 010, D1).
// Given a snapshot CPU, a GLIMPSE service on the request port + 2 (note 011,
// D2): a thread on that CPU following what has been sent, a spin for each
// login. A run that starts behind what it has followed starts it again from
// message 1; a run from the middle of the day waits until it has followed
// that far (minutes, on note 006's reference structures).
// Design: docs/design/007-receiver-framer.md, docs/design/011-snapshot-recovery.md
//
// Usage: feed_sender <interface ip> <group> <port> <request port> <cpu> <control cpu> [day.gz [stamps name|- [snapshot cpu]]]
#include <itchfh/mold_sender.hpp>
#include <itchfh/session_store.hpp>
#include <itchfh/snapshot_link.hpp>
#include <itchfh/udp.hpp>

#include "stamps.hpp"

#include <lfipc/bench/cross_cpu.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace fh = itchfh;

namespace {

struct command {
    char what = 0;
    fh::mold::session_id session{};
    double pace = 0, drop = 0, speed = 0;
    std::uint64_t last = 0, from_ns = 0, to_ns = 0, first = 1, lose_from = 0, lose_to = 0;
};

// A command's layout: the letter, then for R the fields in order, unpadded.
bool read_command(const unsigned char* p, std::size_t n, command& c) {
    if (n < 1) return false;
    c.what = static_cast<char>(p[0]);
    if (c.what != 'R') return c.what == 'S' || c.what == 'Q';
    if (n != 35 && n != 59 && n != 75) return false;
    std::memcpy(c.session.data(), p + 1, 10);
    std::memcpy(&c.pace, p + 11, 8);
    std::memcpy(&c.drop, p + 19, 8);
    std::memcpy(&c.last, p + 27, 8);
    if (n > 35) {
        std::memcpy(&c.speed, p + 35, 8);
        std::memcpy(&c.from_ns, p + 43, 8);
        std::memcpy(&c.to_ns, p + 51, 8);
    }
    if (n > 59) {
        std::memcpy(&c.lose_from, p + 59, 8);
        std::memcpy(&c.lose_to, p + 67, 8);
    }
    return c.pace > 0 || c.speed > 0;
}

// The first message whose timestamp is at or after `t` (timestamps never go back within a day).
std::uint64_t first_at(const fh::session_store& st, std::uint64_t t) {
    std::uint64_t lo = 1, hi = st.count() + 1;
    while (lo < hi) {
        const std::uint64_t mid = lo + (hi - lo) / 2;
        std::size_t len;
        const unsigned char* m = st.message(mid, len);
        if ((len >= 11 ? fh::mold::detail::be(m + 5, 6) : 0) < t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 7) {
        std::fprintf(stderr, "usage: feed_sender <interface ip> <group> <port> <request port> <cpu> <control cpu> [day.gz]\n");
        return 2;
    }
    const std::string ip = argv[1], group = argv[2];
    const auto port = static_cast<std::uint16_t>(std::atoi(argv[3])), request_port = static_cast<std::uint16_t>(std::atoi(argv[4]));
    const int cpu = std::atoi(argv[5]), control_cpu = std::atoi(argv[6]);
    const std::string path = argc > 7 ? argv[7] : ITCHFH_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    if (!lfipc::bench::detail::pin_to(cpu)) {
        std::fprintf(stderr, "feed_sender: cannot pin to CPU %d\n", cpu);
        return 1;
    }
    auto store = std::make_unique<fh::session_store>(fh::mold::session_id{}, 450'000'000, std::size_t{14} << 30, true);
    {
        std::FILE* in = ::popen(("gzip -dc '" + path + "'").c_str(), "r");
        unsigned char buf[1 << 16];
        for (; in;) {
            unsigned char lb[2];
            if (std::fread(lb, 1, 2, in) != 2) break;
            const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
            if (len == 0 || std::fread(buf, 1, len, in) != len || !store->append(buf, len)) break;
        }
        if (in) ::pclose(in);
    }
    if (store->count() == 0) {
        std::fprintf(stderr, "feed_sender: cannot read %s\n", path.c_str());
        return 1;
    }
    std::string why;
    fh::bench::stamp_array* stamps = nullptr;
    if (argc > 8 && std::string(argv[8]) != "-" && !(stamps = fh::bench::open_stamps(argv[8], true, why))) {
        std::fprintf(stderr, "feed_sender: %s\n", why.c_str());
        return 1;
    }
    fh::udp::sender tx;
    if (!tx.open(group, port, ip, request_port, why)) {
        std::fprintf(stderr, "feed_sender: %s\n", why.c_str());
        return 1;
    }
    const int ctl = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in a;
    fh::udp::address(ip, static_cast<std::uint16_t>(request_port + 1), a);
    if (ctl < 0 || ::bind(ctl, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
        std::fprintf(stderr, "feed_sender: cannot bind the control port %u\n", request_port + 1);
        return 1;
    }

    // Note 011: the snapshot service, on its own thread, following the run in progress.
    const int snapshot_cpu = argc > 9 ? std::atoi(argv[9]) : -1;
    std::atomic<std::uint64_t> sent{1}, generation{0}, run_first{1}, taken{0};  // taken: the run the service has started on
    fh::mold::session_id run_session{};
    std::atomic<bool> quit{false};
    std::thread snapshots;
    fh::glimpse_service glimpse;
    if (snapshot_cpu >= 0) {
        if (!glimpse.open(ip, static_cast<std::uint16_t>(request_port + 2), why)) {
            std::fprintf(stderr, "feed_sender: %s\n", why.c_str());
            return 1;
        }
        snapshots = std::thread([&] {
            lfipc::bench::detail::pin_to(snapshot_cpu);
            std::uint64_t seen = 0;
            while (!quit.load(std::memory_order_relaxed)) {
                if (const std::uint64_t g = generation.load(std::memory_order_acquire); g != seen) {
                    seen = g;
                    if (run_first.load(std::memory_order_relaxed) < glimpse.followed()) glimpse.reset(run_session);
                    else glimpse.rename(run_session);
                    taken.store(g, std::memory_order_release);
                }
                if (seen == 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                glimpse.step(*store, sent);
            }
        });
    }

    // The control thread hands commands over; the sending thread (this one) runs them.
    std::mutex mu;
    std::condition_variable cv;
    command pending;
    bool has_pending = false;
    std::atomic<bool> stop{false};
    fh::sender_counts last_counts{};
    bool counts_ready = false;
    std::thread control([&] {
        lfipc::bench::detail::pin_to(control_cpu);
        for (;;) {
            unsigned char b[128];
            sockaddr_in from;
            socklen_t fl = sizeof(from);
            const ssize_t n = ::recvfrom(ctl, b, sizeof(b), 0, reinterpret_cast<sockaddr*>(&from), &fl);
            command c;
            if (n <= 0 || !read_command(b, static_cast<std::size_t>(n), c)) continue;
            stop = true;  // any command ends the run in progress
            if (c.what == 'R' && n > 35) {
                // The times resolved to messages, answered at once.
                if (c.from_ns > 0) {
                    c.first = first_at(*store, c.from_ns);
                    c.last = c.to_ns > 0 ? first_at(*store, c.to_ns) - 1 : c.last;
                }
                const std::uint64_t span[2] = {c.first, c.last ? c.last : store->count()};
                ::sendto(ctl, span, sizeof(span), 0, reinterpret_cast<sockaddr*>(&from), fl);
            }
            std::unique_lock<std::mutex> l(mu);
            if (c.what == 'S') {
                cv.wait(l, [&] { return counts_ready; });
                ::sendto(ctl, &last_counts, sizeof(last_counts), 0, reinterpret_cast<sockaddr*>(&from), fl);
                continue;
            }
            pending = c;
            has_pending = true;
            cv.notify_all();
            if (c.what == 'Q') return;
        }
    });
    std::fprintf(stderr, "feed_sender: %llu messages loaded; sending to %s:%u from %s, requests on %u, control on %u, CPU %d\n",
                 static_cast<unsigned long long>(store->count()), group.c_str(), port, ip.c_str(), request_port, request_port + 1, cpu);
    if (snapshot_cpu >= 0) std::fprintf(stderr, "feed_sender: snapshots on %u, CPU %d\n", request_port + 2, snapshot_cpu);
    for (;;) {
        command c;
        {
            std::unique_lock<std::mutex> l(mu);
            counts_ready = true;
            cv.notify_all();
            cv.wait(l, [&] { return has_pending; });
            c = pending;
            has_pending = false;
            counts_ready = false;
            stop = false;
        }
        if (c.what == 'Q') break;
        store->set_session(c.session);
        run_session = c.session;
        run_first.store(c.first, std::memory_order_relaxed);
        sent.store(c.first, std::memory_order_release);
        const std::uint64_t g = generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (snapshot_cpu >= 0) {  // the service on this run and caught up to where it starts, first
            while ((taken.load(std::memory_order_acquire) != g || glimpse.followed() < c.first) && !stop.load(std::memory_order_relaxed))
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::fprintf(stderr, "feed_sender: snapshots followed to message %llu\n", static_cast<unsigned long long>(glimpse.followed()));
        }
        fh::sender_config sc;
        sc.packets_per_s = c.speed > 0 ? 0 : c.pace;
        sc.speed = c.speed;
        sc.drop = c.drop;
        sc.first = c.first;
        sc.last = c.last;
        if (stamps) {
            sc.stamp = [](void* ctx, std::uint64_t first, std::uint64_t count, std::int64_t due) {
                auto* a = static_cast<fh::bench::stamp_array*>(ctx);
                for (std::uint64_t q = first; q < first + count; ++q) a->put(q, due);
            };
            sc.stamp_ctx = stamps;
        }
        sc.heartbeat_ns = 100'000'000;
        sc.sent = &sent;
        sc.lose_from = c.lose_from;
        sc.lose_to = c.lose_to;
        fh::mold_sender s(*store, tx, sc);
        s.run(stop);
        std::lock_guard<std::mutex> l(mu);
        last_counts = s.counts();
    }
    control.join();
    quit = true;
    if (snapshots.joinable()) snapshots.join();
    return 0;
}
