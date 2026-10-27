// Publication over Nasdaq's sample day (note 008, S1), label `data`: needs
// data/01302020.NASDAQ_ITCH50.gz (tools/fetch_day.py), so CI does not run it.
// Every message of the day through the handler and its publisher, sequence
// numbers from 1; a consumer in another process keeps books from its ring
// (pub::books) and, at the feed's end, reports them as a digest with what
// else it saw. The test paces itself by how far behind the consumer is (the
// publisher never waits). Checked: the consumer's books equal the handler's,
// every level update, trade, break, status, imbalance, market event and
// directory entry arrived once, sequence numbers never went back, no overrun;
// and the state table's best bid and ask equal the consumer's books.
//
// Usage: publish_day_test [day.gz]
#include "check.hpp"

#include <itchfh/book.hpp>
#include <itchfh/consumer.hpp>
#include <itchfh/decoder.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/publisher.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace itchfh;
using itchfh::test::check;

struct counting_sink {
    std::uint64_t levels = 0, trades = 0, broken = 0, statuses = 0, imbalances = 0, markets = 0, directories = 0;
    void book(const applied& a) {
        if (a.bad == bad_event::none) levels += a.changed;
    }
    void trade(const trade_report&) { ++trades; }
    void trade_broken(const trade_report&) { ++broken; }
    void status(std::uint16_t, status_field) { ++statuses; }
    void imbalance(std::uint16_t, const itchfh::imbalance&) { ++imbalances; }
    void market(const message&) { ++markets; }
    void directory(std::uint16_t, const stock_directory&) { ++directories; }
};

struct report {
    std::uint64_t digest, levels, trades, broken, statuses, imbalances, markets, directories, resets;
    bool in_order, table_agrees;
};

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : ITCHFH_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    const std::string name = "/itchfh-pdt-" + std::to_string(::getpid());
    const auto t0 = std::chrono::steady_clock::now();
    auto book = std::make_unique<default_book>();
    auto p = std::make_unique<published<>>(*book);
    std::string why;
    if (!p->pub.open(name, why)) {
        check(false, "the publisher's segments: " + why);
        return itchfh::test::result();
    }
    int fds[2];
    if (::pipe(fds) != 0) return 1;
    const pid_t child = ::fork();
    if (child == 0) {
        ::close(fds[0]);
        pub::consumer c;
        report r{};
        if (c.attach(name, why)) {
            pub::books b;
            pub::update u;
            for (;;) {
                if (!c.poll(u)) continue;
                b.apply(u);
                if (u.k == pub::kind::feed && u.feed.state == feed_state::ended) break;
            }
            r = {b.digest(), b.levels, b.trades, b.broken, b.statuses, b.imbalances, b.markets, b.directories, b.resets, b.in_order, true};
            for (const auto& [l, sides] : b.by_instrument) {
                pub::entry_view e;
                c.read(l, e);
                const auto bb = b.best(l, side::bid), ba = b.best(l, side::ask);
                if (e.bid_price != bb.first || e.bid_shares != bb.second || e.ask_price != ba.first || e.ask_shares != ba.second) r.table_agrees = false;
            }
        }
        [[maybe_unused]] const ssize_t n = ::write(fds[1], &r, sizeof(r));
        ::_exit(0);
    }
    ::close(fds[1]);
    int c = -1;
    for (int i = 0; i < 20'000'000 && c < 0; ++i) {
        p->tick(0);
        for (int k = 0; k < pub::max_consumers; ++k)
            if (p->pub.slot(k) == pub::slot_live) c = k;
    }
    auto rbook = std::make_unique<default_book>();
    counting_sink k;
    handler<default_book, counting_sink> rh(*rbook, k);
    std::uint64_t seq = 0, waits = 0;
    std::FILE* in = ::popen(("gzip -dc '" + path + "'").c_str(), "r");
    unsigned char buf[1 << 16];
    for (; in && c >= 0;) {
        unsigned char lb[2];
        if (std::fread(lb, 1, 2, in) != 2) break;
        const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
        if (len == 0 || std::fread(buf, 1, len, in) != len) break;
        message m;
        if (decode(buf, len, m) != decode_status::ok) continue;
        if (p->pub.behind(c) > pub::ring_capacity / 2) {
            ++waits;
            while (p->pub.behind(c) > pub::ring_capacity / 4) std::this_thread::yield();
        }
        p->on(++seq, m);
        rh.on(m);
    }
    if (in) ::pclose(in);
    p->feed(feed_state::ended, seq + 1);
    report r{};
    const bool got = ::read(fds[0], &r, sizeof(r)) == static_cast<ssize_t>(sizeof(r));
    ::waitpid(child, nullptr, 0);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(got && seq > 0 && r.digest == pub::digest_of(*book) && pub::digest_of(*book) == pub::digest_of(*rbook) && r.levels == k.levels &&
              r.trades == k.trades && r.broken == k.broken && r.statuses == k.statuses && r.imbalances == k.imbalances &&
              r.markets == k.markets && r.directories == k.directories && r.resets == 1 && r.in_order && r.table_agrees &&
              p->pub.overruns() == 0,
          "S1: the day's " + std::to_string(seq) + " messages published to a consumer in another process: its books equal the "
          "handler's; " + std::to_string(r.levels) + " level updates, " + std::to_string(r.trades) + " trades, " + std::to_string(r.broken) +
          " breaks, " + std::to_string(r.statuses) + " status changes, " + std::to_string(r.imbalances) + " imbalances, " +
          std::to_string(r.markets) + " market events, " + std::to_string(r.directories) + " directory entries, each once, in order; "
          "no overrun; the state table agrees (" + std::to_string(waits) + " pauses for the consumer, " + std::to_string(static_cast<int>(secs)) + " s)");
    return itchfh::test::result();
}
