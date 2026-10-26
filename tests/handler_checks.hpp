// Test helpers for note 003: a sink that records every call as a line, and
// descriptions of an instrument's state in one string, so the handler and the
// reference handler can be compared exactly (field by field, no padding).
#pragma once

#include <itchfh/book_types.hpp>
#include <itchfh/handler_types.hpp>
#include <itchfh/market_status.hpp>
#include <itchfh/messages.hpp>

#include <cstdio>
#include <string>
#include <vector>

namespace itchfh::test {

template <std::size_t N>
std::string text(const std::array<char, N>& a) {
    return std::string(a.begin(), a.end());
}

struct recording_sink {
    std::vector<std::string> calls;
    bool keep = true;  // false: count only
    std::size_t count = 0;

    void add(std::string s) {
        ++count;
        if (keep) calls.push_back(std::move(s));
    }
    void book(const applied& a) {
        std::string s = "book " + std::to_string(a.locate) + " bad " + std::to_string(static_cast<int>(a.bad));
        for (int i = 0; i < a.changed; ++i)
            s += " " + std::to_string(static_cast<int>(a.level[i].s)) + "@" + std::to_string(a.level[i].price);
        add(s);
    }
    void trade(const trade_report& r) { add("trade " + describe(r)); }
    void trade_broken(const trade_report& r) { add("broken " + describe(r)); }
    void status(std::uint16_t l, status_field f) { add("status " + std::to_string(l) + " " + std::to_string(static_cast<int>(f))); }
    void imbalance(std::uint16_t l, const itchfh::imbalance& m) {
        add("noii " + std::to_string(l) + " " + std::to_string(m.paired_shares) + " " + std::to_string(m.imbalance_shares) + " " + m.direction);
    }
    void market(const message& m) { add(std::string("market ") + m.type); }
    void directory(std::uint16_t l, const stock_directory& r) { add("directory " + std::to_string(l) + " " + text(r.stock)); }

    static std::string describe(const trade_report& r) {
        return std::to_string(r.locate) + " " + std::to_string(static_cast<int>(r.kind)) + " " + std::to_string(r.price) + " " +
               std::to_string(r.shares) + " " + std::to_string(r.match) + " " + std::to_string(r.timestamp_ns);
    }
};

inline std::string describe(const instrument_trades& t) {
    return std::to_string(t.volume) + "/" + std::to_string(t.trades) + "/" + std::to_string(t.last_price) + "/" + std::to_string(t.last_match);
}

inline std::string describe(const instrument_status& s) {
    std::string d;
    const auto c = [&](char x) { d += x ? x : '_'; };
    const auto n = [&](std::uint64_t v) { d += std::to_string(v) + ","; };
    c(s.trading_state);
    d += text(s.trading_reason);
    c(s.reg_sho);
    for (char x : s.operational_halt) c(x);
    c(s.retail_interest);
    n(s.has);
    n(s.not_since_snapshot);
    if (s.has & 1) n(s.luld.reference_price), n(s.luld.upper_price), n(s.luld.lower_price), n(s.luld.extension);
    if (s.has & 2) n(s.ipo.release_time_s), c(s.ipo.release_qualifier), n(s.ipo.ipo_price);
    if (s.has & 4) c(s.dlcr.open_eligibility), n(s.dlcr.min_price), n(s.dlcr.max_price), n(s.dlcr.near_price), n(s.dlcr.near_time),
        n(s.dlcr.lower_collar), n(s.dlcr.upper_collar);
    if (s.has & 8) n(s.noii.paired_shares), n(s.noii.imbalance_shares), c(s.noii.direction), n(s.noii.far_price), n(s.noii.near_price),
        n(s.noii.reference_price), c(s.noii.cross_type), c(s.noii.price_variation);
    return d;
}

inline std::string describe(const market_state& m) {
    return std::string(1, m.phase ? m.phase : '_') + std::to_string(m.phases) + "/" + std::to_string(m.mwcb.level1) + "/" +
           std::to_string(m.mwcb.level2) + "/" + std::to_string(m.mwcb.level3) + "/" + std::string(1, m.mwcb_breached ? m.mwcb_breached : '_') +
           (m.mwcb_not_since_snapshot ? "/not since snapshot" : "");
}

// Everything the handler holds about one instrument, in one string.
template <class H>
std::string instrument(const H& h, std::uint16_t l) {
    const stock_directory* e = h.entry(l);
    return (e ? text(e->stock) + e->market_category + std::to_string(e->round_lot_size) : std::string("none")) + "|" +
           describe(h.trades_of(l)) + "|" + describe(h.status_of(l));
}

}  // namespace itchfh::test
