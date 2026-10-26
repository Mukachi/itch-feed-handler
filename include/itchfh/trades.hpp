// Trades (note 003, D5): every execution, non-displayed trade and cross print
// by its match number, so a trade is counted once although both its sides may
// be reported (the sample day repeats 5,170 match numbers, each the same
// shares on the same instrument). Counted: printable executions (E, and C
// marked printable), non-displayed trades (P) and cross prints (Q) with
// shares; a non-printable C is recorded, not counted (it is in a later bulk
// print; the specification, 1.4.2). A Broken Trade takes its trade back out.
// Per instrument: volume, trade count, last price and match. Fixed, pre-faulted.
// Design: docs/design/003-events-directory-status.md
#pragma once

#include <itchfh/fixed_map.hpp>
#include <itchfh/handler_types.hpp>
#include <itchfh/memory.hpp>

#include <cstdint>

namespace itchfh {

class trades {
public:
    trades(std::size_t matches, bool prefault) : matches_(matches, prefault), of_(zeroed<instrument_trades>(65'536, prefault)) {}

    enum class seen_result : std::uint8_t { counted, not_counted, full };

    // A side of a trade seen. `counted` if this is the first time its match
    // number is counted (`out` filled), `not_counted` if already counted or not
    // printable, `full` if the table had no room.
    seen_result seen(std::uint64_t match, std::uint16_t locate, std::uint64_t shares, std::uint32_t price, bool printable, trade_kind k,
                     std::uint64_t timestamp_ns, trade_report& out) {
        bool fresh;
        rec* r = matches_.get(match, fresh);
        if (!r) return seen_result::full;
        if (fresh) *r = {0, 0, 0, 0, 0};
        if (r->counted || !printable || shares == 0) return seen_result::not_counted;
        *r = {shares, price, locate, static_cast<std::uint8_t>(k), 1};
        instrument_trades& t = of_[locate];
        t.volume += shares;
        ++t.trades;
        t.last_price = price;
        t.last_match = match;
        out = {locate, k, price, shares, match, timestamp_ns};
        return seen_result::counted;
    }

    enum class break_result : std::uint8_t { broken, not_counted, unknown };

    // A Broken Trade: its trade taken back out of volume (`out` filled), if it
    // was counted and not broken already. The last price is left as it was.
    break_result broken(std::uint64_t match, std::uint64_t timestamp_ns, trade_report& out) {
        rec* r = matches_.find(match);
        if (!r) return break_result::unknown;
        if (r->counted != 1) return break_result::not_counted;
        r->counted = 2;
        instrument_trades& t = of_[r->locate];
        t.volume -= r->shares;
        --t.trades;
        out = {r->locate, static_cast<trade_kind>(r->kind), r->price, r->shares, match, timestamp_ns};
        return break_result::broken;
    }

    const instrument_trades& of(std::uint16_t locate) const { return of_[locate]; }
    std::size_t matches() const { return matches_.size(); }

private:
    struct rec {
        std::uint64_t shares;
        std::uint32_t price;
        std::uint16_t locate;
        std::uint8_t kind;
        std::uint8_t counted;  // 0 not yet, 1 counted, 2 broken
    };
    static_assert(sizeof(rec) == 16);
    fixed_map<rec> matches_;
    table<instrument_trades> of_;
};

}  // namespace itchfh
