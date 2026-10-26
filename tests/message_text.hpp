// Every field of a decoded message, by its type, as text (note 004's tests):
// written from messages.hpp by hand, member by member, so comparing two
// decoders' results does not go through the decoder's own field tables.
#pragma once

#include <itchfh/messages.hpp>

#include <string>

namespace itchfh::test {

inline std::string message_text(const message& m) {
    std::string s;
    const auto n = [&](std::uint64_t v) { s += std::to_string(v) + ","; };
    const auto c = [&](char v) { s += std::to_string(static_cast<unsigned char>(v)) + ","; };
    const auto a = [&](const auto& arr) {
        for (char v : arr) c(v);
    };
    c(m.type), n(m.locate), n(m.tracking), n(m.timestamp_ns);
    switch (m.type) {
        case 'S': c(m.S.event_code); break;
        case 'R':
            a(m.R.stock), c(m.R.market_category), c(m.R.financial_status), n(m.R.round_lot_size), c(m.R.round_lots_only),
                c(m.R.issue_classification), a(m.R.issue_sub_type), c(m.R.authenticity), c(m.R.short_sale_threshold), c(m.R.ipo_flag),
                c(m.R.luld_tier), c(m.R.etp_flag), n(m.R.etp_leverage_factor), c(m.R.inverse_indicator);
            break;
        case 'H': a(m.H.stock), c(m.H.trading_state), c(m.H.reserved), a(m.H.reason); break;
        case 'Y': a(m.Y.stock), c(m.Y.action); break;
        case 'L': a(m.L.participant), a(m.L.stock), c(m.L.primary_market_maker), c(m.L.market_maker_mode), c(m.L.participant_state); break;
        case 'V': n(m.V.level1), n(m.V.level2), n(m.V.level3); break;
        case 'W': c(m.W.breached_level); break;
        case 'K': a(m.K.stock), n(m.K.release_time_s), c(m.K.release_qualifier), n(m.K.ipo_price); break;
        case 'J': a(m.J.stock), n(m.J.reference_price), n(m.J.upper_price), n(m.J.lower_price), n(m.J.extension); break;
        case 'h': a(m.h.stock), c(m.h.market_code), c(m.h.action); break;
        case 'A':
        case 'F': n(m.A.ref), c(m.A.side), n(m.A.shares), a(m.A.stock), n(m.A.price), a(m.A.attribution); break;
        case 'E':
        case 'C': n(m.E.ref), n(m.E.shares), n(m.E.match), c(m.E.printable), n(m.E.price); break;
        case 'X': n(m.X.ref), n(m.X.shares); break;
        case 'D': n(m.D.ref); break;
        case 'U': n(m.U.old_ref), n(m.U.new_ref), n(m.U.shares), n(m.U.price); break;
        case 'P': n(m.P.ref), c(m.P.side), n(m.P.shares), a(m.P.stock), n(m.P.price), n(m.P.match); break;
        case 'Q': n(m.Q.shares), a(m.Q.stock), n(m.Q.price), n(m.Q.match), c(m.Q.cross_type); break;
        case 'B': n(m.B.match); break;
        case 'I':
            n(m.I.paired_shares), n(m.I.imbalance_shares), c(m.I.direction), a(m.I.stock), n(m.I.far_price), n(m.I.near_price),
                n(m.I.reference_price), c(m.I.cross_type), c(m.I.price_variation);
            break;
        case 'N': a(m.N.stock), c(m.N.interest_flag); break;
        case 'O':
            a(m.O.stock), c(m.O.open_eligibility), n(m.O.min_price), n(m.O.max_price), n(m.O.near_price), n(m.O.near_time),
                n(m.O.lower_collar), n(m.O.upper_collar);
            break;
        default: s += "?"; break;
    }
    return s;
}

}  // namespace itchfh::test
