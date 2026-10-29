// Recovery from a snapshot over Nasdaq's sample day, through the programs
// themselves (note 011, S1 and D8), label `data`: needs
// data/01302020.NASDAQ_ITCH50.gz (tools/fetch_day.py), so CI does not run it.
// feed_sender sends the whole day over loopback multicast at 15k packets a
// second, with its GLIMPSE service on CPU 5 (D2), and loses 2,000 messages on
// purpose at the day's peak of live orders (message 412,349,206: note 006):
// their packets not sent, requests for them refused. `itchfh` (on CPU 2) is
// lost there and recovers from a snapshot; a consumer here (CPU 3, the
// publisher's node) keeps books from what it publishes. The pace: our
// service follows the feed on note 006's reference structures, at about 1.2
// million messages a second, so the day is sent at a pace it keeps near
// (about 0.65 million), not at full speed (note 011, Review).
// Checked: the consumer's books at the close equal a book fed the day
// directly, never overrun; the program ends healthy, one snapshot applied,
// none failed; the sender lost and refused what it was told to.
//
// Usage: recovery_day_test [day.gz]
#include "check.hpp"

#include <itchfh/book.hpp>
#include <itchfh/consumer.hpp>
#include <itchfh/decoder.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/mold_sender.hpp>
#include <itchfh/monitoring.hpp>

#include <lfipc/bench/cross_cpu.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
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

constexpr std::uint64_t peak = 412'349'206, lost_messages = 2'000;
constexpr double pace = 15'000;

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
    explicit control(std::uint16_t port) {
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
    // The whole day at a fixed pace, messages from..to lost on purpose (the longest form of R).
    void run(double packets_per_s, std::uint64_t lose_from, std::uint64_t lose_to) {
        unsigned char b[75] = {};
        const mold::session_id s = {'R', 'E', 'C', 'O', 'V', 'E', 'R', 'D', 'A', 'Y'};
        const double zero = 0;
        b[0] = 'R';
        std::memcpy(b + 1, s.data(), 10);
        std::memcpy(b + 11, &packets_per_s, 8);
        std::memcpy(b + 19, &zero, 8);  // drop
        // last 0, speed 0, from and to 0: the whole day at the fixed pace
        std::memcpy(b + 59, &lose_from, 8);
        std::memcpy(b + 67, &lose_to, 8);
        ::sendto(fd, b, sizeof(b), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        std::uint64_t span[2];
        [[maybe_unused]] const ssize_t n = ::recv(fd, span, sizeof(span), 0);  // the long form's answer, set aside
    }
    void quit() {
        const unsigned char q = 'Q';
        ::sendto(fd, &q, 1, 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
    }
};

// Books as a consumer keeps them, fast enough for the live day (as record_day_test's).
struct fast_books {
    struct level {
        std::uint64_t shares;
        std::uint32_t orders;
    };
    std::unordered_map<std::uint64_t, level> m;
    std::uint64_t updates = 0, resets = 0;
    fast_books() { m.reserve(1u << 22); }
    void apply(const pub::update& u) {
        ++updates;
        if (u.k == pub::kind::reset) {
            ++resets;
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

struct watcher {
    fast_books b;
    bool attached = false, ended = false, overrun = false;
    std::atomic<bool> give_up{false};
    std::thread t;
    void start(const std::string& name) {
        t = std::thread([this, name] {
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
                if (u.k == pub::kind::feed && u.feed.state == feed_state::ended) {
                    ended = true;
                    return;
                }
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
    const std::string tag = std::to_string(::getpid());
    const std::string pub_name = "/itchfh-rcd-" + tag + "-pub", mon_name = "/itchfh-rcd-" + tag + "-mon";
    const std::string dir = std::string(ITCHFH_RECORD_DIR) + "/recovery-day-" + tag;
    std::filesystem::create_directories(dir);
    const std::string cfg = dir + "/itchfh.conf";
    std::ofstream(cfg) << "# recovery_day_test\nport = 26720\nserver_port = 26721\nsnapshot_port = 26723\ncpu = 2\nrebuild_cpu = 1\npublish = "
                       << pub_name << "\nrecord_dir = none\nmonitor = " << mon_name << "\n";
    const auto t0 = std::chrono::steady_clock::now();

    const pid_t sender = spawn({ITCHFH_SENDER, "127.0.0.1", "239.192.0.1", "26720", "26721", "4", "0", day, "-", "5"});
    control ctl(26722);
    sender_counts sent{};
    bool sender_up = false;
    for (int i = 0; i < 600 && !(sender_up = ctl.stop(sent)); ++i) {
    }
    // The reference: a book fed the day directly.
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
    const pid_t program = spawn({ITCHFH_PROGRAM, cfg});
    watcher w;
    w.start(pub_name);
    mon::block mb;
    std::string why;
    bool mon_attached = false;
    for (int i = 0; i < 600 && !(mon_attached = mb.attach(mon_name, why)); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    while (!w.attached && std::chrono::steady_clock::now() - t0 < std::chrono::minutes(10)) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));  // the consumer's join replayed
    ctl.run(pace, peak, peak + lost_messages - 1);
    // The day at 15k is ~625 s; not ended in 25 minutes is a failure, reported, not a hang.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(25);
    mon::values v{};
    while (!w.ended && !w.overrun && std::chrono::steady_clock::now() < deadline) {
        if (mon_attached) mb.read(v);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!w.ended) {
        std::fprintf(stderr, "recovery_day_test: the session did not end for the consumer (overrun %d): feed state %llu, %llu datagrams, %llu gaps, "
                     "%llu snapshots, %llu failed, recovering %llu\n", static_cast<int>(w.overrun), static_cast<unsigned long long>(v[mon::feed_state_now]),
                     static_cast<unsigned long long>(v[mon::datagrams]), static_cast<unsigned long long>(v[mon::gaps]),
                     static_cast<unsigned long long>(v[mon::snapshots]), static_cast<unsigned long long>(v[mon::snapshot_failures]),
                     static_cast<unsigned long long>(v[mon::recovering_now]));
        w.give_up = true;
        ::kill(program, SIGTERM);
    }
    w.t.join();
    const int program_exit = wait_exit(program);
    if (mon_attached) mb.read(v);  // the program's last write
    ctl.stop(sent);
    ctl.quit();
    wait_exit(sender);
    const std::uint64_t ref = pub::digest_of(*rbook);
    const std::uint32_t h = mon::health(v);
    const double minutes = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() / 60;
    check(sender_up && w.attached && w.ended && !w.overrun && program_exit == 0 && w.b.digest() == ref && v[mon::snapshots] == 1 &&
              v[mon::snapshot_failures] == 0 && static_cast<feed_state>(v[mon::feed_state_now]) == feed_state::ended && !mon::unhealthy(h) &&
              sent.lost_on_purpose > 0 && sent.refused > 0,
          "S1 (note 011): the day through the programs at " + std::to_string(static_cast<int>(pace)) + " packets a second, " +
              std::to_string(lost_messages) + " messages from the peak of live orders lost on purpose (" + std::to_string(sent.lost_on_purpose) +
              " packets not sent, " + std::to_string(sent.refused) + " requests refused): itchfh recovered from " + std::to_string(v[mon::snapshots]) +
              " snapshot (" + std::to_string(v[mon::snapshot_failures]) + " failed) in " + std::to_string(v[mon::last_recovery_ns] / 1000) +
              " us, ended healthy; the consumer's books at the close equal a book fed the day directly (" + std::to_string(w.b.resets) + " resets, " +
              std::to_string(w.b.updates) + " updates), never overrun; " + std::to_string(minutes).substr(0, 4) + " min");
    std::filesystem::remove_all(dir);
    return itchfh::test::result();
}
