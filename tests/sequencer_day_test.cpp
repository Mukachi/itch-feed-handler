// The sequencer over Nasdaq's sample day (note 005, S1), label `data`: needs
// data/01302020.NASDAQ_ITCH50.gz (tools/fetch_day.py), so CI does not run it.
// The day's messages kept in a session store, packed by D7's packetizer and
// sent through the lossy link (100,000 packets a second, simulated) at each
// loss rate, with requests and answers lost too; behind the sequencer, the
// decoder (note 004's default) and the handler on the default book. Checked:
// every message delivered exactly once, in order, the same bytes; the feed
// ended, nothing held; and the book fed through the link equal, every level
// and queue, to a book fed the same messages directly.
//
// Usage: sequencer_day_test [day.gz]
#include "check.hpp"
#include "lossy_link.hpp"

#include <itchfh/book.hpp>
#include <itchfh/decoder.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/session_store.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>

namespace {

using namespace itchfh;
using itchfh::test::check;

struct quiet_sink {
    void book(const applied&) {}
    void trade(const trade_report&) {}
    void trade_broken(const trade_report&) {}
    void status(std::uint16_t, status_field) {}
    void imbalance(std::uint16_t, const itchfh::imbalance&) {}
    void market(const message&) {}
    void directory(std::uint16_t, const stock_directory&) {}
};

struct handled {
    std::unique_ptr<default_book> book = std::make_unique<default_book>();
    quiet_sink sink;
    std::unique_ptr<handler<default_book, quiet_sink>> h = std::make_unique<handler<default_book, quiet_sink>>(*book, sink);
    std::uint64_t undecoded = 0;
    void deliver(std::uint64_t, const unsigned char* p, std::size_t n) {
        message m;
        if (decode_simd_unpadded(p, n, m) != decode_status::ok) ++undecoded;  // no padding promised by the hold's slots
        else h->on(m);
    }
};

std::string books_differ(const default_book& a, const default_book& b) {
    if (a.instruments() != b.instruments()) return "different instruments have orders";
    for (const std::uint16_t l : a.instruments())
        for (const side s : {side::bid, side::ask}) {
            const auto lv = a.levels(l, s);
            if (lv != b.levels(l, s)) return "locate " + std::to_string(l) + ": levels differ";
            for (const level_view& v : lv)
                if (a.queue(l, s, v.price) != b.queue(l, s, v.price)) return "locate " + std::to_string(l) + ": a queue differs";
        }
    return a.live_orders() == b.live_orders() ? std::string{} : "live orders differ";
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : ITCHFH_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    const mold::session_id session = {'0', '0', '0', '0', '0', '0', '0', '0', '0', '1'};
    auto store = std::make_unique<session_store>(session, 450'000'000, std::size_t{14} << 30, false);
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
        check(false, path + ": cannot read (tools/fetch_day.py fetches it)");
        return itchfh::test::result();
    }
    // The book fed directly, once, for every rate to be compared against.
    handled direct;
    for (std::uint64_t s = 1; s <= store->count(); ++s) {
        std::size_t len;
        const unsigned char* m = store->message(s, len);
        direct.deliver(s, m, len);
    }
    struct rate {
        const char* name;
        double loss, request_loss;
    } rates[] = {{"none", 0, 0}, {"0.01%", 0.0001, 0.01}, {"0.1%", 0.001, 0.01}, {"1%", 0.01, 0.01}};
    for (const rate& r : rates) {
        const auto t0 = std::chrono::steady_clock::now();
        handled via;
        itchfh::test::link_rules rules;
        rules.drop = r.loss;
        rules.duplicate = r.loss;
        rules.delay = r.loss ? 0.01 : 0;
        rules.drop_requests = r.request_loss;
        sequencer_config c;
        c.prefault = false;
        auto l = std::make_unique<itchfh::test::lossy_link<handled>>(*store, rules, c, via);
        l->run(10'000, 200'000'000'000);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto& seen = l->seen();
        const auto& k = l->seq().counts();
        const std::string diff = books_differ(*direct.book, *via.book);
        check(seen.delivered == store->count() && seen.out_of_order == 0 && seen.wrong_bytes == 0 && via.undecoded == 0 &&
                  l->seq().state() == feed_state::ended && l->seq().held_packets() == 0 && diff.empty(),
              std::string("S1, ") + r.name + " of packets lost and duplicated: " + std::to_string(seen.delivered) +
                  " messages delivered once, in order, as stored; ended, nothing held; the book equal to the one fed directly" +
                  (diff.empty() ? "" : " (" + diff + ")") + "; " + std::to_string(k.gaps) + " gaps, " + std::to_string(k.requests) +
                  " requests (" + std::to_string(k.retried) + " retried), " + std::to_string(k.held) + " packets held, " +
                  std::to_string(k.duplicate_messages) + " duplicate messages dropped (" + std::to_string(static_cast<int>(secs)) + " s)");
    }
    return itchfh::test::result();
}
