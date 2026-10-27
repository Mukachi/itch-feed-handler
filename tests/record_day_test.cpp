// Recording over Nasdaq's sample day, through the programs themselves (note
// 009, S1, and note 001's S7), label `data`: needs
// data/01302020.NASDAQ_ITCH50.gz (tools/fetch_day.py), so CI does not run it.
// feed_sender (note 007) sends the whole day over loopback multicast at 50k
// packets a second (publishing, the program's one thread keeps up to about
// 80k: note 008's cost, ~93 ns a message, on note 007's), 1% of packets
// dropped; `itchfh` (D7) receives (on CPU 2), publishes, records to the build
// directory's disk and monitors, from a configuration file; a consumer here
// keeps books from what it publishes and a digest of every update, byte for
// byte, on CPU 3 (the publisher's node, as note 008's benchmark places its
// consumers: one on the other socket slows every push, note 008's Review).
// Then `itchfh --replay --paced` reads the recording, at the pace it was
// recorded, into a fresh publisher, a second consumer doing the same.
// Checked: the live and replayed consumers' books equal a book fed the day
// directly; neither consumer overrun; the two update streams the same, byte
// for byte; the replay reports the recording complete; the monitoring
// block's last counters say every datagram recorded, none unrecorded,
// healthy. On failure the recording is kept.
//
// Usage: record_day_test [day.gz]
#include "check.hpp"

#include <itchfh/book.hpp>
#include <itchfh/consumer.hpp>
#include <itchfh/decoder.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/mold_sender.hpp>
#include <itchfh/monitoring.hpp>

#include <lfipc/bench/cross_cpu.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <algorithm>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace itchfh;
using itchfh::test::check;

pid_t spawn(const std::vector<std::string>& args) {
    const pid_t p = ::fork();
    if (p == 0) {
        std::vector<char*> a;
        for (const auto& s : args) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        ::execv(a[0], a.data());
        ::_exit(127);
    }
    return p;
}
int wait_exit(pid_t p) {
    int st = 0;
    ::waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

// feed_sender's control port (bench/feed_sender.cpp).
struct control {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_in to{};
    control(std::uint16_t port) {
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
        timeval tv{1, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    bool stop(sender_counts& c) {
        const unsigned char s = 'S';
        ::sendto(fd, &s, 1, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        return ::recv(fd, &c, sizeof(c), 0) == static_cast<ssize_t>(sizeof(c));
    }
    void run(double pace, double drop) {
        unsigned char b[35];
        const mold::session_id s = {'R', 'E', 'C', 'D', 'A', 'Y', ' ', ' ', ' ', '1'};
        const std::uint64_t last = 0;
        b[0] = 'R';
        std::memcpy(b + 1, s.data(), 10);
        std::memcpy(b + 11, &pace, 8);
        std::memcpy(b + 19, &drop, 8);
        std::memcpy(b + 27, &last, 8);
        ::sendto(fd, b, sizeof(b), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }
    void quit() {
        const unsigned char q = 'Q';
        ::sendto(fd, &q, 1, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }
};

// Books as a consumer keeps them, fast enough for the live day (pub::books's
// ordered maps are not: overrun at 50k packets a second): a hash table, sorted
// once at the end into pub::books's digest.
struct fast_books {
    struct level {
        std::uint64_t shares;
        std::uint32_t orders;
    };
    std::unordered_map<std::uint64_t, level> m;
    std::uint64_t updates = 0;
    fast_books() { m.reserve(1u << 22); }
    void apply(const pub::update& u) {
        ++updates;
        if (u.k == pub::kind::reset) {
            m.clear();
            return;
        }
        if (u.k != pub::kind::level) return;
        const std::uint64_t key = static_cast<std::uint64_t>(u.locate) << 33 | static_cast<std::uint64_t>(u.s) << 32 | u.level.price;
        if (u.level.orders == 0) m.erase(key);
        else m[key] = {u.level.shares, u.level.orders};
    }
    std::uint64_t digest() const {
        std::vector<std::pair<std::uint64_t, level>> v(m.begin(), m.end());
        // pub::books's order: instrument, bids then asks, bids highest price first, asks lowest first.
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
            const std::uint64_t ia = a.first >> 32, ib = b.first >> 32;
            if (ia != ib) return ia < ib;
            const auto pa = static_cast<std::uint32_t>(a.first), pb = static_cast<std::uint32_t>(b.first);
            return (ia & 1) == 0 ? pa > pb : pa < pb;
        });
        std::uint64_t h = 1469598103934665603ULL;
        const auto mix = [&](std::uint64_t x) {
            for (int i = 0; i < 8; ++i) h = (h ^ ((x >> (8 * i)) & 0xFF)) * 1099511628211ULL;
        };
        for (const auto& [k, lv] : v) {
            mix(k >> 33);
            mix((k >> 32) & 1);
            mix(static_cast<std::uint32_t>(k));
            mix(lv.shares);
            mix(lv.orders);
        }
        return h;
    }
};

// A consumer of a program's publisher, on a thread: books, and every update's bytes hashed.
struct watcher {
    fast_books b;
    std::uint64_t h = 1469598103934665603ULL, feed_updates = 0;
    bool attached = false, ended = false, overrun = false;
    // For a failure's diagnosis: each update's reliability, and a hash of the rest of it.
    std::vector<std::uint8_t> rel;
    std::vector<std::uint64_t> rest;
    std::size_t kept = 0;
    std::atomic<bool> give_up{false};
    std::thread t;
    void start(const std::string& name) {
        // Sized (and so touched) before attaching: growing them would stall the consumer into an overrun.
        rel.assign(520'000'000, 0);
        rest.assign(520'000'000, 0);
        t = std::thread([this, name] {
            // On the publisher's node (its CPU 2's): a consumer on the other socket slows every push (note 008, Review).
            lfipc::bench::detail::pin_to(3);
            pub::consumer c;
            std::string why;
            for (int i = 0; i < 6000 && !c.attach(name, why); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            attached = c.index() >= 0;
            if (!attached) return;
            pub::update u;
            while (!give_up.load(std::memory_order_relaxed)) {
                if (!c.poll(u)) {
                    if (c.overrun()) {
                        overrun = true;
                        return;
                    }
                    continue;
                }
                b.apply(u);
                if (u.k == pub::kind::feed) {
                    ++feed_updates;
                    if (u.feed.state == feed_state::ended) {
                        ended = true;
                        return;
                    }
                    continue;
                }
                // The update's eight words, hashed cheaply (a byte-at-a-time hash kept the consumer from keeping up).
                std::uint64_t w[8];
                std::memcpy(w, &u, sizeof(w));
                const auto mix = [](std::uint64_t acc, std::uint64_t x) {
                    acc ^= x * 0x9E3779B97F4A7C15ULL;
                    return (acc << 27 | acc >> 37) * 0xC2B2AE3D27D4EB4FULL;
                };
                for (const std::uint64_t x : w) h = mix(h, x);
                const std::uint8_t reliability = u.reliability;
                if (kept < rel.size()) rel[kept] = reliability;
                w[0] &= ~(0xFFull << 8);  // the reliability byte, apart
                std::uint64_t r = 0;
                for (const std::uint64_t x : w) r = mix(r, x);
                if (kept < rest.size()) rest[kept] = r;
                ++kept;
            }
        });
    }
};

struct quiet_sink {
    void book(const applied&) {}
    void trade(const trade_report&) {}
    void trade_broken(const trade_report&) {}
    void status(std::uint16_t, status_field) {}
    void imbalance(std::uint16_t, const itchfh::imbalance&) {}
    void market(const message&) {}
    void directory(std::uint16_t, const stock_directory&) {}
};

}  // namespace

int main(int argc, char** argv) {
    const std::string day = argc > 1 ? argv[1] : ITCHFH_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    const std::string dir = ITCHFH_RECORD_DIR, tag = std::to_string(::getpid());
    const std::string pub_live = "/itchfh-rdt-" + tag + "-pub", pub_replay = "/itchfh-rdt-" + tag + "-pub2", mon_name = "/itchfh-rdt-" + tag + "-mon";
    const std::string rec_dir = dir + "/record-day-" + tag;
    std::filesystem::create_directories(rec_dir);
    const std::string live_cfg = rec_dir + "/live.conf", replay_cfg = rec_dir + "/replay.conf";
    std::ofstream(live_cfg) << "# record_day_test, live\nport = 26700\nserver_port = 26701\ncpu = 2\nwriter_cpu = 1\npublish = " << pub_live
                            << "\nrecord_dir = " << rec_dir << "\nmonitor = " << mon_name << "\n";
    std::ofstream(replay_cfg) << "# record_day_test, replay\npublish = " << pub_replay << "\nmonitor = none\n";
    const auto t0 = std::chrono::steady_clock::now();

    // The sender, loading the day.
    const pid_t sender = spawn({ITCHFH_SENDER, "127.0.0.1", "239.192.0.1", "26700", "26701", "4", "0", day});
    control ctl(26702);
    sender_counts sent{};
    bool sender_up = false;
    for (int i = 0; i < 600 && !(sender_up = ctl.stop(sent)); ++i) {
    }
    // Meanwhile nothing; then the reference book, fed the day directly.
    auto rbook = std::make_unique<default_book>();
    quiet_sink qs;
    auto rh = std::make_unique<handler<default_book, quiet_sink>>(*rbook, qs);
    {
        std::FILE* in = ::popen(("gzip -dc '" + day + "'").c_str(), "r");
        unsigned char buf[1 << 16];
        for (; in;) {
            unsigned char lb[2];
            if (std::fread(lb, 1, 2, in) != 2) break;
            const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
            if (len == 0 || std::fread(buf, 1, len, in) != len) break;
            message m;
            if (decode(buf, len, m) == decode_status::ok) rh->on(m);
        }
        if (in) ::pclose(in);
    }
    // Live: the program, its consumer, its monitoring block.
    const pid_t live = spawn({ITCHFH_PROGRAM, live_cfg});
    watcher w1;
    w1.start(pub_live);
    mon::block mb;
    std::string why;
    bool mon_attached = false;
    for (int i = 0; i < 600 && !(mon_attached = mb.attach(mon_name, why)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    while (!w1.attached && w1.t.joinable() && std::chrono::steady_clock::now() - t0 < std::chrono::minutes(10)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // the consumer's join replayed
    ctl.run(50'000, 0.01);
    // The day at 50k is ~190 s; a session not ended in 15 minutes is a failure, reported, not a hang.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(15);
    mon::values v{};
    while (!w1.ended && !w1.overrun && std::chrono::steady_clock::now() < deadline) {
        if (mon_attached) mb.read(v);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!w1.ended) {
        std::fprintf(stderr, "record_day_test: the live session did not end for the consumer (overrun %d): feed state %llu, %llu datagrams, %llu kernel drops, %llu gaps, "
                     "%llu hold refusals, %llu unrecorded\n", static_cast<int>(w1.overrun), static_cast<unsigned long long>(v[mon::feed_state_now]),
                     static_cast<unsigned long long>(v[mon::datagrams]), static_cast<unsigned long long>(v[mon::kernel_drops]),
                     static_cast<unsigned long long>(v[mon::gaps]), static_cast<unsigned long long>(v[mon::hold_full]),
                     static_cast<unsigned long long>(v[mon::unrecorded]));
        w1.give_up = true;
        ::kill(live, SIGTERM);
    }
    w1.t.join();
    const int live_exit = wait_exit(live);
    if (mon_attached) mb.read(v);  // the program's last write, at its end (the mapping outlives the name)
    ctl.stop(sent);
    ctl.quit();
    wait_exit(sender);
    std::string recording;
    for (const auto& e : std::filesystem::directory_iterator(rec_dir))
        if (e.path().extension() == ".rec") recording = e.path().string();
    // Replay: the program on the recording, a second consumer.
    watcher w2;
    w2.start(pub_replay);
    // At the recorded pace, once the consumer has joined: it then sees what the live one did, from the start.
    const pid_t again = recording.empty() || !w1.ended ? -1 : spawn({ITCHFH_PROGRAM, replay_cfg, "--replay", recording, "--paced", "--wait-consumers", "1"});
    if (again < 0) w2.give_up = true;
    w2.t.join();
    const int replay_exit = again < 0 ? -1 : wait_exit(again);
    const std::uint64_t ref = pub::digest_of(*rbook);
    std::fprintf(stderr, "record_day_test: sender %d attached %d/%d exits %d/%d recording %d; books live %d replay %d; overrun %d/%d; "
                 "updates %llu/%llu, bytes same %d, feed updates %llu/%llu; monitor attached %d, unrecorded %llu, recorded %llu = %llu + %llu, "
                 "state %llu, health %u\n",
                 sender_up, w1.attached, w2.attached, live_exit, replay_exit, !recording.empty(), w1.b.digest() == ref, w2.b.digest() == ref,
                 w1.overrun, w2.overrun, static_cast<unsigned long long>(w1.b.updates), static_cast<unsigned long long>(w2.b.updates), w1.h == w2.h,
                 static_cast<unsigned long long>(w1.feed_updates), static_cast<unsigned long long>(w2.feed_updates), mon_attached,
                 static_cast<unsigned long long>(v[mon::unrecorded]), static_cast<unsigned long long>(v[mon::recorded]),
                 static_cast<unsigned long long>(v[mon::datagrams]), static_cast<unsigned long long>(v[mon::answers]),
                 static_cast<unsigned long long>(v[mon::feed_state_now]), mon::health(v));
    for (std::size_t i = 0; i < w1.kept && i < w2.kept && i < w1.rel.size(); ++i)
        if (w1.rel[i] != w2.rel[i] || w1.rest[i] != w2.rest[i]) {
            std::size_t n_rel = 0, n_rest = 0;
            for (std::size_t j = i; j < w1.kept && j < w2.kept && j < w1.rel.size(); ++j) {
                n_rel += w1.rel[j] != w2.rel[j];
                n_rest += w1.rest[j] != w2.rest[j];
            }
            std::fprintf(stderr, "record_day_test: first difference at update %zu: reliability %u/%u, rest %s; from there %zu reliabilities "
                         "and %zu rests differ\n", i, w1.rel[i], w2.rel[i], w1.rest[i] == w2.rest[i] ? "same" : "differs", n_rel, n_rest);
            break;
        }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(sender_up && w1.attached && w2.attached && live_exit == 0 && replay_exit == 0 && !recording.empty() && w1.b.digest() == ref &&
              w2.b.digest() == ref && !w1.overrun && !w2.overrun && w1.h == w2.h && w1.b.updates == w2.b.updates && w1.feed_updates == w2.feed_updates && sent.dropped > 0 &&
              mon_attached && v[mon::unrecorded] == 0 && v[mon::recorded] == v[mon::datagrams] + v[mon::answers] &&
              v[mon::feed_state_now] == static_cast<std::uint64_t>(feed_state::ended) && !mon::unhealthy(mon::health(v)),
          "S1 (note 001's S7): the day through the programs: feed_sender over loopback multicast, " + std::to_string(sent.dropped) +
              " packets dropped; itchfh received, published, recorded " + std::to_string(v[mon::recorded]) + " datagrams (" +
              std::to_string(v[mon::recorded_bytes] >> 20) + " MB, " + std::to_string(v[mon::kernel_drops]) + " kernel drops recovered), "
              "none unrecorded, healthy at the end; itchfh --replay: complete; both consumers' books equal a book fed the day directly, "
              "the " + std::to_string(w1.b.updates) + " published updates the same byte for byte (" + std::to_string(static_cast<int>(secs)) + " s)");
    if (itchfh::test::failures == 0) std::filesystem::remove_all(rec_dir);
    else std::fprintf(stderr, "record_day_test: kept %s\n", rec_dir.c_str());
    return itchfh::test::result();
}
