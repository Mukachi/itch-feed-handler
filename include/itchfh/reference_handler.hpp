// The reference handler (note 003, D7): the event handler's rules written a
// second time for clarity, over standard containers (maps keyed by locate,
// symbol, match number, instrument and MPID), no fixed capacity. The oracle
// the handler is compared against after every message, and the baseline it
// is measured beside. Same sink calls, same accessors.
// Design: docs/design/003-events-directory-status.md
#pragma once

#include <itchfh/book_types.hpp>
#include <itchfh/event.hpp>
#include <itchfh/handler_types.hpp>
#include <itchfh/market_status.hpp>
#include <itchfh/messages.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>

namespace itchfh {

template <class Book, class Sink>
class reference_handler {
public:
    reference_handler(Book& b, Sink& s) : book_(b), sink_(s) {}

    void on(const message& m) {
        const std::uint16_t l = m.locate;
        if (m.type == 'S') {
            static const std::string order = "OSQMEC";
            market_.phase = m.S.event_code;
            if (market_.phases < order.size() && order[market_.phases] == m.S.event_code) ++market_.phases;
            else ++counts_.phase_out_of_order;
            sink_.market(m);
            return;
        }
        if (m.type == 'V') {
            market_.mwcb = m.V;
            sink_.market(m);
            return;
        }
        if (m.type == 'W') {
            market_.mwcb_breached = m.W.breached_level;
            sink_.market(m);
            return;
        }
        if (m.type == 'R') {
            if (entries_.contains(l)) ++counts_.second_directory;
            entries_[l] = m.R;
            symbols_[std::string(m.R.stock.begin(), m.R.stock.end())] = l;
            sink_.directory(l, m.R);
            return;
        }
        if (m.type == 'K') {
            const auto it = symbols_.find(std::string(m.K.stock.begin(), m.K.stock.end()));
            if (it == symbols_.end()) {
                ++counts_.no_directory;
                return;
            }
            status_[it->second].ipo = m.K;
            status_[it->second].has |= 2;
            sink_.status(it->second, status_field::ipo_quoting);
            return;
        }
        if (!entries_.contains(l)) ++counts_.no_directory;
        instrument_status& st = status_[l];
        switch (m.type) {
            case 'H': {
                event e{};
                e.type = event_type::trading_state;
                e.locate = l;
                e.side = m.H.trading_state;
                e.timestamp_ns = m.timestamp_ns;
                book_.apply(e);
                st.trading_state = m.H.trading_state;
                st.trading_reason = m.H.reason;
                sink_.status(l, status_field::trading_state);
                return;
            }
            case 'Y':
                st.reg_sho = m.Y.action;
                sink_.status(l, status_field::reg_sho);
                return;
            case 'L':
                participants_[{l, std::string(m.L.participant.begin(), m.L.participant.end())}] = {
                    m.L.primary_market_maker, m.L.market_maker_mode, m.L.participant_state};
                sink_.status(l, status_field::participant);
                return;
            case 'J':
                st.luld = m.J;
                st.has |= 1;
                sink_.status(l, status_field::luld_collar);
                return;
            case 'h': {
                const std::string codes = "QBX";
                const auto i = codes.find(m.h.market_code);
                if (i == std::string::npos) {
                    ++counts_.bad_field;
                    return;
                }
                st.operational_halt[i] = m.h.action;
                sink_.status(l, status_field::operational_halt);
                return;
            }
            case 'N':
                st.retail_interest = m.N.interest_flag;
                sink_.status(l, status_field::retail_interest);
                return;
            case 'O':
                st.dlcr = m.O;
                st.has |= 4;
                sink_.status(l, status_field::direct_listing);
                return;
            case 'I':
                st.noii = m.I;
                st.has |= 8;
                sink_.imbalance(l, m.I);
                return;
            case 'A':
            case 'F': {
                event e{};
                e.type = event_type::add;
                e.locate = l;
                e.timestamp_ns = m.timestamp_ns;
                e.ref = m.A.ref;
                e.side = m.A.side;
                e.shares = m.A.shares;
                e.price = m.A.price;
                sink_.book(book_.apply(e));
                return;
            }
            case 'E':
            case 'C': {
                const auto o = book_.find(m.E.ref);
                event e{};
                e.type = m.type == 'E' ? event_type::executed : event_type::executed_price;
                e.locate = l;
                e.timestamp_ns = m.timestamp_ns;
                e.ref = m.E.ref;
                e.shares = m.E.shares;
                e.price = m.E.price;
                const applied a = book_.apply(e);
                sink_.book(a);
                if (o && a.bad == bad_event::none)
                    side_of_trade(m.E.match, a.locate, m.E.shares, m.type == 'E' ? o->price : m.E.price,
                                  m.type == 'E' || m.E.printable == 'Y', trade_kind::displayed, m.timestamp_ns);
                return;
            }
            case 'X': {
                event e{};
                e.type = event_type::cancel;
                e.locate = l;
                e.timestamp_ns = m.timestamp_ns;
                e.ref = m.X.ref;
                e.shares = m.X.shares;
                sink_.book(book_.apply(e));
                return;
            }
            case 'D': {
                event e{};
                e.type = event_type::remove;
                e.locate = l;
                e.timestamp_ns = m.timestamp_ns;
                e.ref = m.D.ref;
                sink_.book(book_.apply(e));
                return;
            }
            case 'U': {
                event e{};
                e.type = event_type::replace;
                e.locate = l;
                e.timestamp_ns = m.timestamp_ns;
                e.ref = m.U.old_ref;
                e.new_ref = m.U.new_ref;
                e.shares = m.U.shares;
                e.price = m.U.price;
                sink_.book(book_.apply(e));
                return;
            }
            case 'P':
                side_of_trade(m.P.match, l, m.P.shares, m.P.price, true, trade_kind::non_displayed, m.timestamp_ns);
                return;
            case 'Q':
                side_of_trade(m.Q.match, l, m.Q.shares, m.Q.price, true, trade_kind::cross, m.timestamp_ns);
                return;
            case 'B': {
                const auto it = matches_.find(m.B.match);
                if (it == matches_.end()) {
                    ++counts_.unknown_break;
                    return;
                }
                trade_rec& t = it->second;
                if (t.state != counted) return;
                t.state = broken;
                instrument_trades& f = trades_[t.report.locate];
                f.volume -= t.report.shares;
                --f.trades;
                trade_report r = t.report;
                r.timestamp_ns = m.timestamp_ns;
                sink_.trade_broken(r);
                return;
            }
            default:
                ++counts_.unknown_type;
                return;
        }
    }

    const stock_directory* entry(std::uint16_t l) const {
        const auto it = entries_.find(l);
        return it == entries_.end() ? nullptr : &it->second;
    }
    std::optional<std::uint16_t> locate_of(const symbol& s) const {
        const auto it = symbols_.find(std::string(s.begin(), s.end()));
        return it == symbols_.end() ? std::nullopt : std::optional<std::uint16_t>(it->second);
    }
    instrument_trades trades_of(std::uint16_t l) const {
        const auto it = trades_.find(l);
        return it == trades_.end() ? instrument_trades{} : it->second;
    }
    instrument_status status_of(std::uint16_t l) const {
        const auto it = status_.find(l);
        return it == status_.end() ? instrument_status{} : it->second;
    }
    const participant_view* participant_of(std::uint16_t l, const mpid& p) const {
        const auto it = participants_.find({l, std::string(p.begin(), p.end())});
        return it == participants_.end() ? nullptr : &it->second;
    }
    const market_state& market() const { return market_; }
    const handler_counts& counts() const { return counts_; }

private:
    enum trade_state : std::uint8_t { seen_only, counted, broken };
    struct trade_rec {
        trade_state state = seen_only;
        trade_report report;
    };
    void side_of_trade(std::uint64_t match, std::uint16_t l, std::uint64_t shares, std::uint32_t price, bool printable, trade_kind k,
                       std::uint64_t ts) {
        trade_rec& t = matches_[match];
        if (t.state != seen_only || !printable || shares == 0) return;
        t.state = counted;
        t.report = {l, k, price, shares, match, ts};
        instrument_trades& f = trades_[l];
        f.volume += shares;
        ++f.trades;
        f.last_price = price;
        f.last_match = match;
        sink_.trade(t.report);
    }

    Book& book_;
    Sink& sink_;
    std::map<std::uint16_t, stock_directory> entries_;
    std::unordered_map<std::string, std::uint16_t> symbols_;
    std::unordered_map<std::uint64_t, trade_rec> matches_;
    std::map<std::uint16_t, instrument_trades> trades_;
    std::map<std::uint16_t, instrument_status> status_;
    std::map<std::pair<std::uint16_t, std::string>, participant_view> participants_;
    market_state market_;
    handler_counts counts_;
};

}  // namespace itchfh
