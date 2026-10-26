// The handlers over Nasdaq's sample day (note 003, S2 S3), label `data`: needs
// data/01302020.NASDAQ_ITCH50.gz (tools/fetch_day.py), so CI does not run it.
// Every message of the day through the reference decoder, then into the handler
// (on the default fast book) and the reference handler (on the reference book)
// in lockstep. After every message: the same sink calls. Every 10 million
// messages and at the end: every instrument's directory entry, trade figures
// and status, the market state and the counts, and both books in full.
// Also, counted here on its own: how many executions or trades arrive with a
// match number already counted (S3: each such is one trade, not two).
//
// Usage: handler_day_test [day.gz]
#include "check.hpp"
#include "handler_checks.hpp"

#include <itchfh/book.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/reference_book.hpp>
#include <itchfh/reference_decoder.hpp>
#include <itchfh/reference_handler.hpp>

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

using namespace itchfh;
using itchfh::test::check;
using itchfh::test::recording_sink;

template <class A, class B>
std::string books_differ(const A& a, const B& b) {
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
    std::FILE* in = ::popen(("gzip -dc '" + path + "'").c_str(), "r");
    if (!in) {
        check(false, path + ": cannot read (tools/fetch_day.py fetches it)");
        return itchfh::test::result();
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto fast_book = std::make_unique<default_book>();
    auto ref_book = std::make_unique<reference_book>();
    recording_sink fast_sink, ref_sink;
    auto fast = std::make_unique<handler<default_book, recording_sink>>(*fast_book, fast_sink);
    auto ref = std::make_unique<reference_handler<reference_book, recording_sink>>(*ref_book, ref_sink);

    std::vector<unsigned char> buf(1 << 16);
    std::uint64_t n = 0, undecoded = 0, checkpoints = 0, sides_again = 0;
    std::unordered_set<std::uint64_t> counted;  // match numbers already counted, kept here on its own
    counted.reserve(1 << 24);
    std::string problem;
    std::uint16_t max_locate = 0;
    const auto checkpoint = [&](const std::string& when) {
        if (!problem.empty()) return;
        for (std::uint32_t l = 0; l <= max_locate && problem.empty(); ++l)
            if (itchfh::test::instrument(*fast, static_cast<std::uint16_t>(l)) != itchfh::test::instrument(*ref, static_cast<std::uint16_t>(l)))
                problem = when + ": locate " + std::to_string(l) + " differs";
        if (problem.empty() && (!(fast->counts() == ref->counts()) ||
                                itchfh::test::describe(fast->market()) != itchfh::test::describe(ref->market())))
            problem = when + ": counts or market state differ";
        if (problem.empty())
            if (auto p = books_differ(*ref_book, *fast_book); !p.empty()) problem = when + ": " + p;
    };
    for (;;) {
        unsigned char lb[2];
        if (std::fread(lb, 1, 2, in) != 2) break;
        const std::size_t len = std::size_t{lb[0]} << 8 | lb[1];
        if (len == 0 || std::fread(buf.data(), 1, len, in) != len) break;
        message m;
        if (decode_reference(buf.data(), len, m) != decode_status::ok) {
            ++undecoded;
            continue;
        }
        ++n;
        if (m.locate > max_locate) max_locate = m.locate;
        // S3, independently: an execution, trade or cross whose match number was already counted
        std::uint64_t match = 0;
        bool printable = false;
        if (m.type == 'E' || m.type == 'C') {
            match = m.E.match;
            printable = m.type == 'E' || m.E.printable == 'Y';
        } else if (m.type == 'P') {
            match = m.P.match;
            printable = true;
        } else if (m.type == 'Q') {
            match = m.Q.match;
            printable = m.Q.shares > 0;
        }
        if (match) {
            if (counted.contains(match)) ++sides_again;
            else if (printable) counted.insert(match);
        }
        fast->on(m);
        ref->on(m);
        if (problem.empty() && fast_sink.calls != ref_sink.calls) problem = "message " + std::to_string(n) + " (" + m.type + "): sink calls differ";
        fast_sink.calls.clear();
        ref_sink.calls.clear();
        if (n % 10'000'000 == 0) {
            ++checkpoints;
            checkpoint("checkpoint at message " + std::to_string(n));
        }
    }
    ::pclose(in);
    checkpoint("at the end");
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::uint64_t volume = 0, trades = 0;
    for (std::uint32_t l = 0; l <= max_locate; ++l) {
        volume += fast->trades_of(static_cast<std::uint16_t>(l)).volume;
        trades += fast->trades_of(static_cast<std::uint16_t>(l)).trades;
    }
    const handler_counts& c = fast->counts();
    check(n > 0 && undecoded == 0 && problem.empty(),
          std::to_string(n) + " messages, every one decoded; the handler and the reference handler alike after every message, at " +
              std::to_string(checkpoints) + " checkpoints and at the end (every instrument, the counts, the market state, both books)" +
              (problem.empty() ? "" : " (" + problem + ")"));
    check(trades == counted.size() && sides_again > 0,
          "S3: " + std::to_string(trades) + " trades, one per match number counted (" + std::to_string(counted.size()) +
              " here on their own); " + std::to_string(sides_again) + " executions or trades arrived with a match number already counted, "
              "not counted again; volume " + std::to_string(volume) + " shares");
    check(c.unknown_type == 0 && c.no_directory == 0 && c.unknown_break == 0 && c.phase_out_of_order == 0 && c.table_full == 0 && c.bad_field == 0,
          "S4 on the day: second directory entries " + std::to_string(c.second_directory) +
              "; no unknown types, messages without a directory entry, unknown breaks, out-of-order phases or full tables");
    check(fast->market().phases == 6 && fast_book->live_orders() == 0, "the day's System Events all in order; no order live at the close");
    std::printf("(%.0f s)\n", s);
    return itchfh::test::result();
}
