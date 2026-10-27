// The event handler (note 003): each decoded TotalView-ITCH 5.0 message to
// the state it changes, the change reported to a sink. Order messages to the
// book (note 002), Stock Directory to the directory (D4), executions and
// trades to the trades (D5, by match number), everything else to the market
// status (D6). Sans-I/O: one thread owns it all; nothing allocates (S5).
//
// The sink (D2) is any type with these calls, made directly:
//   book(const applied&)                    an order message applied (or refused)
//   trade(const trade_report&)              a trade counted, once per match number
//   trade_broken(const trade_report&)       a counted trade broken
//   status(std::uint16_t, status_field)     an instrument's status field set
//   imbalance(std::uint16_t, const imbalance&)
//   market(const message&)                  S, V, W
//   directory(std::uint16_t, const stock_directory&)
// Design: docs/design/003-events-directory-status.md
#pragma once

#include <itchfh/book_types.hpp>
#include <itchfh/directory.hpp>
#include <itchfh/event.hpp>
#include <itchfh/handler_types.hpp>
#include <itchfh/market_status.hpp>
#include <itchfh/messages.hpp>
#include <itchfh/trades.hpp>

#include <cstdint>
#include <optional>

namespace itchfh {

struct handler_config {
    std::size_t matches = 16'000'000;     // the sample day has 10,347,476
    std::size_t participants = 1u << 19;  // the sample day's L messages number 216,802
    bool prefault = true;
};

template <class Book, class Sink>
class handler {
public:
    handler(Book& b, Sink& s, const handler_config& c = {})
        : book_(b), sink_(s), directory_(c.prefault), trades_(c.matches, c.prefault), status_(c.participants, c.prefault) {}

    void on(const message& m) {
        const std::uint16_t l = m.locate;
        switch (m.type) {
            case 'S':
                if (!status_.system_event(m.S.event_code)) ++counts_.phase_out_of_order;
                sink_.market(m);
                return;
            case 'V':
                status_.mwcb_decline(m.V);
                sink_.market(m);
                return;
            case 'W':
                status_.mwcb_status(m.W.breached_level);
                sink_.market(m);
                return;
            case 'R': {
                bool full;
                if (!directory_.set(l, m.R, full)) ++counts_.second_directory;
                if (full) ++counts_.table_full;
                sink_.directory(l, m.R);
                return;
            }
            case 'K': {  // its Stock Locate is always 0: the instrument is found by symbol
                const auto found = directory_.locate_of(m.K.stock);
                if (!found) {
                    ++counts_.no_directory;
                    return;
                }
                status_.ipo(*found, m.K);
                sink_.status(*found, status_field::ipo_quoting);
                return;
            }
            default:
                break;
        }
        if (!directory_.find(l)) ++counts_.no_directory;
        switch (m.type) {
            case 'H': {
                event e{};
                e.type = event_type::trading_state;
                e.locate = l;
                e.side = m.H.trading_state;
                e.timestamp_ns = m.timestamp_ns;
                book_.apply(e);
                status_.trading_action(l, m.H);
                sink_.status(l, status_field::trading_state);
                return;
            }
            case 'Y':
                status_.reg_sho(l, m.Y);
                sink_.status(l, status_field::reg_sho);
                return;
            case 'L':
                if (!status_.participant(l, m.L)) ++counts_.table_full;
                sink_.status(l, status_field::participant);
                return;
            case 'J':
                status_.luld(l, m.J);
                sink_.status(l, status_field::luld_collar);
                return;
            case 'h':
                if (status_.operational_halt(l, m.h)) sink_.status(l, status_field::operational_halt);
                else ++counts_.bad_field;
                return;
            case 'N':
                status_.retail(l, m.N);
                sink_.status(l, status_field::retail_interest);
                return;
            case 'O':
                status_.dlcr(l, m.O);
                sink_.status(l, status_field::direct_listing);
                return;
            case 'I':
                status_.noii(l, m.I);
                sink_.imbalance(l, m.I);
                return;
            case 'A':
            case 'F': {
                event e = base(m, event_type::add);
                e.ref = m.A.ref;
                e.side = m.A.side;
                e.shares = m.A.shares;
                e.price = m.A.price;
                sink_.book(book_.apply(e));
                return;
            }
            case 'E':
            case 'C': {
                // D3: the order's price (and that it exists) before it is applied
                const auto o = book_.find(m.E.ref);
                event e = base(m, m.type == 'E' ? event_type::executed : event_type::executed_price);
                e.ref = m.E.ref;
                e.shares = m.E.shares;
                e.price = m.E.price;
                const applied a = book_.apply(e);
                sink_.book(a);
                if (!o || a.bad != bad_event::none) return;
                const bool printable = m.type == 'E' || m.E.printable == 'Y';
                count(m.E.match, a.locate, m.E.shares, m.type == 'E' ? o->price : m.E.price, printable, trade_kind::displayed, m.timestamp_ns);
                return;
            }
            case 'X': {
                event e = base(m, event_type::cancel);
                e.ref = m.X.ref;
                e.shares = m.X.shares;
                sink_.book(book_.apply(e));
                return;
            }
            case 'D': {
                event e = base(m, event_type::remove);
                e.ref = m.D.ref;
                sink_.book(book_.apply(e));
                return;
            }
            case 'U': {
                event e = base(m, event_type::replace);
                e.ref = m.U.old_ref;
                e.new_ref = m.U.new_ref;
                e.shares = m.U.shares;
                e.price = m.U.price;
                sink_.book(book_.apply(e));
                return;
            }
            case 'P':
                count(m.P.match, l, m.P.shares, m.P.price, true, trade_kind::non_displayed, m.timestamp_ns);
                return;
            case 'Q':
                count(m.Q.match, l, m.Q.shares, m.Q.price, true, trade_kind::cross, m.timestamp_ns);
                return;
            case 'B': {
                trade_report r;
                switch (trades_.broken(m.B.match, m.timestamp_ns, r)) {
                    case trades::break_result::broken: sink_.trade_broken(r); break;
                    case trades::break_result::unknown: ++counts_.unknown_break; break;
                    case trades::break_result::not_counted: break;
                }
                return;
            }
            default:
                ++counts_.unknown_type;
                return;
        }
    }

    // Note 006, D5: the state was rebuilt from a snapshot; what it could not
    // restore is marked until set again.
    void mark_restored() { status_.mark_restored(); }

    // What it holds, in the same terms as the reference handler.
    const stock_directory* entry(std::uint16_t l) const { return directory_.find(l); }
    std::optional<std::uint16_t> locate_of(const symbol& s) const { return directory_.locate_of(s); }
    const instrument_trades& trades_of(std::uint16_t l) const { return trades_.of(l); }
    const instrument_status& status_of(std::uint16_t l) const { return status_.of(l); }
    const market_status& statuses() const { return status_; }
    const participant_view* participant_of(std::uint16_t l, const mpid& p) const { return status_.participant_of(l, p); }
    const market_state& market() const { return status_.market(); }
    const handler_counts& counts() const { return counts_; }

private:
    static event base(const message& m, event_type t) {
        event e{};
        e.type = t;
        e.locate = m.locate;
        e.timestamp_ns = m.timestamp_ns;
        return e;
    }
    void count(std::uint64_t match, std::uint16_t l, std::uint64_t shares, std::uint32_t price, bool printable, trade_kind k,
               std::uint64_t ts) {
        trade_report r;
        switch (trades_.seen(match, l, shares, price, printable, k, ts, r)) {
            case trades::seen_result::counted: sink_.trade(r); break;
            case trades::seen_result::full: ++counts_.table_full; break;
            case trades::seen_result::not_counted: break;
        }
    }

    Book& book_;
    Sink& sink_;
    directory directory_;
    itchfh::trades trades_;
    market_status status_;
    handler_counts counts_;
};

}  // namespace itchfh
