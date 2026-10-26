// Our MoldUDP64 sender as its own process (note 007, D6), for feed_bench and
// the card path: started first (for the card, inside the far port's network
// namespace: `sudo ip netns exec itchfh-peer bench-run feed_sender ...`, which
// keeps the namespace), it loads the sample day into a session store and then
// waits. feed_bench drives it over a control port (the request port + 1),
// one datagram a command:
//   R  run: session (10 bytes), packets a second (double), share dropped
//      (double), last message (u64, 0 the whole day): sends messages 1..last
//      onto the group, then the end of session until the next command,
//      answering re-requests throughout (mold_sender.hpp)
//   S  stop the run; the reply is its counts (sender_counts, as laid out here)
//   Q  quit
// The sending runs on `cpu` (a reserved one); the control thread blocks on
// its socket, pinned to `control cpu` (a housekeeping one).
// Design: docs/design/007-receiver-framer.md
//
// Usage: feed_sender <interface ip> <group> <port> <request port> <cpu> <control cpu> [day.gz]
#include <itchfh/mold_sender.hpp>
#include <itchfh/session_store.hpp>
#include <itchfh/udp.hpp>

#include <lfipc/bench/cross_cpu.hpp>

#include <atomic>
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
    double pace = 0, drop = 0;
    std::uint64_t last = 0;
};

// A command's layout: the letter, then for R the fields in order, unpadded.
bool read_command(const unsigned char* p, std::size_t n, command& c) {
    if (n < 1) return false;
    c.what = static_cast<char>(p[0]);
    if (c.what != 'R') return c.what == 'S' || c.what == 'Q';
    if (n != 1 + 10 + 8 + 8 + 8) return false;
    std::memcpy(c.session.data(), p + 1, 10);
    std::memcpy(&c.pace, p + 11, 8);
    std::memcpy(&c.drop, p + 19, 8);
    std::memcpy(&c.last, p + 27, 8);
    return c.pace > 0;
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
            unsigned char b[64];
            sockaddr_in from;
            socklen_t fl = sizeof(from);
            const ssize_t n = ::recvfrom(ctl, b, sizeof(b), 0, reinterpret_cast<sockaddr*>(&from), &fl);
            command c;
            if (n <= 0 || !read_command(b, static_cast<std::size_t>(n), c)) continue;
            stop = true;  // any command ends the run in progress
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
        fh::sender_config sc;
        sc.packets_per_s = c.pace;
        sc.drop = c.drop;
        sc.last = c.last;
        sc.heartbeat_ns = 100'000'000;
        fh::mold_sender s(*store, tx, sc);
        s.run(stop);
        std::lock_guard<std::mutex> l(mu);
        last_counts = s.counts();
    }
    control.join();
    return 0;
}
