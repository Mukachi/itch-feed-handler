// The reference decoder (note 003, D1): one TotalView-ITCH 5.0 message's bytes
// into its decoded form (messages.hpp), field by field at the specification's
// offsets, written for clarity, not speed. The tests of notes 003 and later
// read the sample day through it; note 004's decoder is held to it, as the fast
// book is to the reference book.
// Design: docs/design/003-events-directory-status.md
#pragma once

#include <itchfh/messages.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace itchfh {

enum class decode_status : std::uint8_t { ok, unknown_type, wrong_length };

namespace detail {

inline std::uint64_t be(const unsigned char* p, int n) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = v << 8 | p[i];
    return v;
}
inline std::uint32_t be32(const unsigned char* p) noexcept { return static_cast<std::uint32_t>(be(p, 4)); }
inline std::uint64_t be64(const unsigned char* p) noexcept { return be(p, 8); }
template <std::size_t N>
void chars(std::array<char, N>& out, const unsigned char* p) noexcept {
    std::memcpy(out.data(), p, N);
}

}  // namespace detail

inline decode_status decode_reference(const unsigned char* p, std::size_t n, message& m) noexcept {
    using detail::be32;
    using detail::be64;
    using detail::chars;
    if (n == 0) return decode_status::wrong_length;
    const char t = static_cast<char>(p[0]);
    const std::size_t want = message_length(t);
    if (want == 0) return decode_status::unknown_type;
    if (n != want) return decode_status::wrong_length;
    m = {};
    m.type = t;
    m.locate = static_cast<std::uint16_t>(detail::be(p + 1, 2));
    m.tracking = static_cast<std::uint16_t>(detail::be(p + 3, 2));
    m.timestamp_ns = detail::be(p + 5, 6);
    const auto a = [&](std::size_t off) { return static_cast<char>(p[off]); };
    switch (t) {
        case 'S':
            m.S.event_code = a(11);
            break;
        case 'R': {
            stock_directory& r = m.R;
            chars(r.stock, p + 11);
            r.market_category = a(19);
            r.financial_status = a(20);
            r.round_lot_size = be32(p + 21);
            r.round_lots_only = a(25);
            r.issue_classification = a(26);
            chars(r.issue_sub_type, p + 27);
            r.authenticity = a(29);
            r.short_sale_threshold = a(30);
            r.ipo_flag = a(31);
            r.luld_tier = a(32);
            r.etp_flag = a(33);
            r.etp_leverage_factor = be32(p + 34);
            r.inverse_indicator = a(38);
            break;
        }
        case 'H':
            chars(m.H.stock, p + 11);
            m.H.trading_state = a(19);
            m.H.reserved = a(20);
            chars(m.H.reason, p + 21);
            break;
        case 'Y':
            chars(m.Y.stock, p + 11);
            m.Y.action = a(19);
            break;
        case 'L':
            chars(m.L.participant, p + 11);
            chars(m.L.stock, p + 15);
            m.L.primary_market_maker = a(23);
            m.L.market_maker_mode = a(24);
            m.L.participant_state = a(25);
            break;
        case 'V':
            m.V.level1 = be64(p + 11);
            m.V.level2 = be64(p + 19);
            m.V.level3 = be64(p + 27);
            break;
        case 'W':
            m.W.breached_level = a(11);
            break;
        case 'K':
            chars(m.K.stock, p + 11);
            m.K.release_time_s = be32(p + 19);
            m.K.release_qualifier = a(23);
            m.K.ipo_price = be32(p + 24);
            break;
        case 'J':
            chars(m.J.stock, p + 11);
            m.J.reference_price = be32(p + 19);
            m.J.upper_price = be32(p + 23);
            m.J.lower_price = be32(p + 27);
            m.J.extension = be32(p + 31);
            break;
        case 'h':
            chars(m.h.stock, p + 11);
            m.h.market_code = a(19);
            m.h.action = a(20);
            break;
        case 'A':
        case 'F':
            m.A.ref = be64(p + 11);
            m.A.side = a(19);
            m.A.shares = be32(p + 20);
            chars(m.A.stock, p + 24);
            m.A.price = be32(p + 32);
            if (t == 'F') chars(m.A.attribution, p + 36);
            else m.A.attribution = {' ', ' ', ' ', ' '};
            break;
        case 'E':
        case 'C':
            m.E.ref = be64(p + 11);
            m.E.shares = be32(p + 19);
            m.E.match = be64(p + 23);
            if (t == 'C') {
                m.E.printable = a(31);
                m.E.price = be32(p + 32);
            }
            break;
        case 'X':
            m.X.ref = be64(p + 11);
            m.X.shares = be32(p + 19);
            break;
        case 'D':
            m.D.ref = be64(p + 11);
            break;
        case 'U':
            m.U.old_ref = be64(p + 11);
            m.U.new_ref = be64(p + 19);
            m.U.shares = be32(p + 27);
            m.U.price = be32(p + 31);
            break;
        case 'P':
            m.P.ref = be64(p + 11);
            m.P.side = a(19);
            m.P.shares = be32(p + 20);
            chars(m.P.stock, p + 24);
            m.P.price = be32(p + 32);
            m.P.match = be64(p + 36);
            break;
        case 'Q':
            m.Q.shares = be64(p + 11);
            chars(m.Q.stock, p + 19);
            m.Q.price = be32(p + 27);
            m.Q.match = be64(p + 31);
            m.Q.cross_type = a(39);
            break;
        case 'B':
            m.B.match = be64(p + 11);
            break;
        case 'I':
            m.I.paired_shares = be64(p + 11);
            m.I.imbalance_shares = be64(p + 19);
            m.I.direction = a(27);
            chars(m.I.stock, p + 28);
            m.I.far_price = be32(p + 36);
            m.I.near_price = be32(p + 40);
            m.I.reference_price = be32(p + 44);
            m.I.cross_type = a(48);
            m.I.price_variation = a(49);
            break;
        case 'N':
            chars(m.N.stock, p + 11);
            m.N.interest_flag = a(19);
            break;
        case 'O':
            chars(m.O.stock, p + 11);
            m.O.open_eligibility = a(19);
            m.O.min_price = be32(p + 20);
            m.O.max_price = be32(p + 24);
            m.O.near_price = be32(p + 28);
            m.O.near_time = be64(p + 32);
            m.O.lower_collar = be32(p + 40);
            m.O.upper_collar = be32(p + 44);
            break;
    }
    return decode_status::ok;
}

}  // namespace itchfh
