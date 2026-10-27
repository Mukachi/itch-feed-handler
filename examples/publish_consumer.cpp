// An example consumer of the publisher (note 008, D7), its own process:
// attaches to the state table and a ring, keeps books from the updates
// (pub::books), rejoins after an overrun, and once a second prints what it has
// seen and checks the state table's best bid and ask against its books. Ends
// at the feed's end, or after the seconds given.
// Design: docs/design/008-publication.md
//
// Usage: publish_consumer <table name, e.g. /itchfh-pub> [seconds]
#include <itchfh/consumer.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    namespace pub = itchfh::pub;
    if (argc < 2) {
        std::fprintf(stderr, "usage: publish_consumer <table name> [seconds]\n");
        return 2;
    }
    const double limit = argc > 2 ? std::atof(argv[2]) : 0;
    pub::consumer c;
    std::string why;
    if (!c.attach(argv[1], why)) {
        std::fprintf(stderr, "publish_consumer: %s\n", why.c_str());
        return 1;
    }
    std::printf("publish_consumer: slot %d of %s\n", c.index(), argv[1]);
    pub::books b;
    pub::update u;
    std::uint64_t rejoins = 0;
    const auto t0 = std::chrono::steady_clock::now();
    auto last = t0;
    for (bool ended = false; !ended;) {
        if (c.poll(u)) {
            b.apply(u);
            ended = u.k == pub::kind::feed && u.feed.state == itchfh::feed_state::ended;
            continue;
        }
        if (c.overrun()) {
            ++rejoins;
            c.rejoin();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::seconds(1)) continue;
        last = now;
        std::uint64_t checked = 0, differ = 0;
        for (const auto& [l, sides] : b.by_instrument) {
            pub::entry_view e;
            c.read(l, e);
            const auto bb = b.best(l, itchfh::side::bid), ba = b.best(l, itchfh::side::ask);
            ++checked;
            // The table may be ahead of the ring: a difference here is a change not yet read.
            if (e.bid_price != bb.first || e.bid_shares != bb.second || e.ask_price != ba.first || e.ask_shares != ba.second) ++differ;
        }
        std::uint64_t next;
        const auto f = c.feed(next);
        std::printf("%6.0f s: %llu updates (%llu levels, %llu trades, %llu status), %llu not current, %llu rejoins; feed %d, next %llu; "
                    "table: %llu instruments, %llu ahead of the ring\n",
                    std::chrono::duration<double>(now - t0).count(), static_cast<unsigned long long>(b.updates),
                    static_cast<unsigned long long>(b.levels), static_cast<unsigned long long>(b.trades),
                    static_cast<unsigned long long>(b.statuses), static_cast<unsigned long long>(b.not_current),
                    static_cast<unsigned long long>(rejoins), static_cast<int>(f), static_cast<unsigned long long>(next),
                    static_cast<unsigned long long>(checked), static_cast<unsigned long long>(differ));
        std::fflush(stdout);
        if (limit > 0 && std::chrono::duration<double>(now - t0).count() >= limit) break;
    }
    std::printf("publish_consumer: done, %llu updates, books digest %016llx\n", static_cast<unsigned long long>(b.updates),
                static_cast<unsigned long long>(b.digest()));
    return 0;
}
