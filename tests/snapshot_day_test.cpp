// Snapshots over Nasdaq's sample day (note 006, S1 S2), label `data`: needs
// data/01302020.NASDAQ_ITCH50.gz (tools/fetch_day.py), so CI does not run it.
// The day's messages, each decoded and encoded again (byte for byte: the
// encoder over every message), feed the snapshot server. At every 50 million
// messages, and at the day's peak of live orders (found by a first pass), a
// spin; a fresh client (handler on the default book) rebuilt from it equals
// the server's books in full (S1); from then on it is fed the rest of the day,
// and after every message the levels it changed and both bests equal the
// server's, and in full at the close (S2).
//
// Usage: snapshot_day_test [day.gz]
#include "check.hpp"

#include <itchfh/book.hpp>
#include <itchfh/encoder.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/reference_decoder.hpp>
#include <itchfh/snapshot.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace itchfh;
using itchfh::test::check;

struct last_applied {
    applied a;
    bool got = false;
    void book(const applied& x) {
        a = x;
        got = true;
    }
    void trade(const trade_report&) {}
    void trade_broken(const trade_report&) {}
    void status(std::uint16_t, status_field) {}
    void imbalance(std::uint16_t, const itchfh::imbalance&) {}
    void market(const message&) {}
    void directory(std::uint16_t, const stock_directory&) {}
};
using client_handler = handler<default_book, last_applied>;

struct client {
    std::uint64_t from = 0;
    std::unique_ptr<default_book> book = std::make_unique<default_book>();
    last_applied sink;
    std::unique_ptr<client_handler> h = std::make_unique<client_handler>(*book, sink);
    std::string problem;
};

std::string books_differ(const reference_book& a, const default_book& b) {
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

template <class F>
bool each_message(const std::string& path, F&& f) {
    std::FILE* in = ::popen(("gzip -dc '" + path + "'").c_str(), "r");
    if (!in) return false;
    unsigned char buf[1 << 16];
    std::uint64_t n = 0;
    for (;;) {
        unsigned char lb[2];
        if (std::fread(lb, 1, 2, in) != 2) break;
        const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
        if (len == 0 || std::fread(buf, 1, len, in) != len) break;
        f(++n, buf, len);
    }
    ::pclose(in);
    return n > 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1] : ITCHFH_DATA_DIR "/01302020.NASDAQ_ITCH50.gz";
    const auto t0 = std::chrono::steady_clock::now();
    // The first pass: where the day's live orders peak.
    std::uint64_t peak_at = 0;
    {
        auto b = std::make_unique<default_book>();
        last_applied sink;
        auto h = std::make_unique<client_handler>(*b, sink);
        std::size_t peak = 0;
        const bool read = each_message(path, [&](std::uint64_t n, const unsigned char* p, std::size_t len) {
            message m;
            if (decode_reference(p, len, m) != decode_status::ok) return;
            h->on(m);
            if (b->live_orders() > peak) {
                peak = b->live_orders();
                peak_at = n + 1;  // a snapshot there: after the message that reached it
            }
        });
        if (!read) {
            check(false, path + ": cannot read (tools/fetch_day.py fetches it)");
            return itchfh::test::result();
        }
    }
    const soup::session_id session = {'G', 'L', 'I', 'M', 'P', 'S', 'E', ' ', ' ', '1'};
    auto server = std::make_unique<snapshot_server>();
    std::vector<std::unique_ptr<client>> clients;
    std::uint64_t reencoded_wrong = 0, total = 0;
    std::vector<std::string> spins;
    std::string round_trip;
    each_message(path, [&](std::uint64_t n, const unsigned char* p, std::size_t len) {
        if (n == peak_at || (n > 1 && (n - 1) % 50'000'000 == 0)) {  // a spin of the state after messages 1 .. n-1
            const auto s0 = std::chrono::steady_clock::now();
            const std::vector<unsigned char> spin = server->spin(session, 0);
            auto c = std::make_unique<client>();
            c->from = n;
            snapshot_client<client_handler> sc(*c->h);
            sc.feed(spin.data(), spin.size());
            const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count();
            std::string d = books_differ(server->book(), *c->book);
            if (d.empty() && (sc.resume() != n || sc.undecoded())) d = "End of Snapshot's number wrong, or messages undecoded";
            if (!d.empty() && round_trip.empty()) round_trip = "at message " + std::to_string(n) + ": " + d;
            spins.push_back("at " + std::to_string(n) + (n == peak_at ? " (the peak)" : "") + ": " + std::to_string(sc.messages()) +
                            " messages, " + std::to_string(spin.size() / 1'000'000) + " MB, " + std::to_string(static_cast<int>(s)) + " s");
            clients.push_back(std::move(c));
        }
        message m;
        if (decode_reference(p, len, m) != decode_status::ok) return;
        ++total;
        unsigned char back[64];
        if (encode(m, back) != len || std::memcmp(back, p, len) != 0) ++reencoded_wrong;
        server->on(m);
        for (auto& c : clients) {
            c->sink.got = false;
            c->h->on(m);
            if (!c->problem.empty() || !c->sink.got) continue;
            const applied& a = c->sink.a;
            for (int k = 0; k < a.changed && c->problem.empty(); ++k)
                if (server->book().at(a.locate, a.level[k].s, a.level[k].price) != c->book->at(a.locate, a.level[k].s, a.level[k].price))
                    c->problem = "message " + std::to_string(n) + ": a changed level differs";
            for (const side s : {side::bid, side::ask})
                if (c->problem.empty() && server->book().best(a.locate, s) != c->book->best(a.locate, s))
                    c->problem = "message " + std::to_string(n) + ": a best differs";
        }
    });
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    check(total > 0 && reencoded_wrong == 0, "every one of the day's " + std::to_string(total) + " messages decoded and encoded again, byte for byte");
    std::string spun;
    for (const auto& s : spins) spun += "; " + s;
    check(round_trip.empty() && clients.size() >= 9,
          "S1: " + std::to_string(clients.size()) + " snapshots across the day, each client rebuilt with the server's books, every level and "
          "queue" + spun + (round_trip.empty() ? "" : " (" + round_trip + ")"));
    std::string caught;
    for (const auto& c : clients) {
        if (c->problem.empty())
            if (auto d = books_differ(server->book(), *c->book); !d.empty()) c->problem = "at the close: " + d;
        if (!c->problem.empty() && caught.empty()) caught = "from " + std::to_string(c->from) + ": " + c->problem;
    }
    check(caught.empty(), "S2: each client fed the day from its snapshot's N equals the server after every message (changed levels, "
                          "bests) and in full at the close" + (caught.empty() ? "" : " (" + caught + ")"));
    std::printf("(%.0f s)\n", secs);
    return itchfh::test::result();
}
