// Tests for include/itchfh/reference_decoder.hpp. Spec:
// docs/design/003-events-directory-status.md (S1): every TotalView-ITCH 5.0
// message type built byte by byte from a table of the specification's
// offsets and lengths, transcribed here apart from the decoder, decoded, and
// every field compared. Each table must also cover its type's length with no
// gap or overlap. Then a wrong length and an unknown type are refused.
#include "check.hpp"

#include <itchfh/reference_decoder.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace itchfh;
using itchfh::test::check;

// One field: offset, length, and the value it is given (a number, written
// big-endian, or characters).
struct field {
    std::size_t offset, length;
    std::uint64_t number = 0;
    const char* text = nullptr;
};

// The header every message carries: type, Stock Locate 0x1234, tracking
// number 0x0506, timestamp 0x0A0B0C0D0E0F.
std::vector<field> header(char t) {
    static char type[2];
    type[0] = t;
    return {{0, 1, 0, type}, {1, 2, 0x1234}, {3, 2, 0x0506}, {5, 6, 0x0A0B0C0D0E0Full}};
}

struct built {
    std::vector<unsigned char> bytes;
    bool covers;  // the fields tile the message exactly
};

built build(char t, std::vector<field> body) {
    std::vector<field> f = header(t);
    f.insert(f.end(), body.begin(), body.end());
    const std::size_t n = message_length(t);
    built b{std::vector<unsigned char>(n, 0xEE), true};
    std::size_t at = 0;
    for (const field& x : f) {
        b.covers &= x.offset == at;
        at = x.offset + x.length;
        for (std::size_t i = 0; i < x.length && x.offset + i < n; ++i)
            b.bytes[x.offset + i] = x.text ? static_cast<unsigned char>(x.text[i]) : static_cast<unsigned char>(x.number >> (8 * (x.length - 1 - i)));
    }
    b.covers &= at == n;
    return b;
}

template <std::size_t N>
bool is(const std::array<char, N>& a, const char* s) {
    return std::memcmp(a.data(), s, N) == 0;
}

bool decoded(const built& b, message& m) {
    return b.covers && decode_reference(b.bytes.data(), b.bytes.size(), m) == decode_status::ok && m.locate == 0x1234 &&
           m.tracking == 0x0506 && m.timestamp_ns == 0x0A0B0C0D0E0Full;
}

void every_type() {
    message m{};
    {
        const auto b = build('S', {{11, 1, 0, "Q"}});
        check(decoded(b, m) && m.type == 'S' && m.S.event_code == 'Q', "1.1 System Event: the header and the event code");
    }
    {
        const auto b = build('R', {{11, 8, 0, "AAPL    "}, {19, 1, 0, "Q"}, {20, 1, 0, "N"}, {21, 4, 100}, {25, 1, 0, "N"},
                                   {26, 1, 0, "C"}, {27, 2, 0, "Z "}, {29, 1, 0, "P"}, {30, 1, 0, "N"}, {31, 1, 0, "N"},
                                   {32, 1, 0, "1"}, {33, 1, 0, "N"}, {34, 4, 3}, {38, 1, 0, "Y"}});
        const auto& r = m.R;
        check(decoded(b, m) && is(r.stock, "AAPL    ") && r.market_category == 'Q' && r.financial_status == 'N' && r.round_lot_size == 100 &&
                  r.round_lots_only == 'N' && r.issue_classification == 'C' && is(r.issue_sub_type, "Z ") && r.authenticity == 'P' &&
                  r.short_sale_threshold == 'N' && r.ipo_flag == 'N' && r.luld_tier == '1' && r.etp_flag == 'N' &&
                  r.etp_leverage_factor == 3 && r.inverse_indicator == 'Y',
              "1.2.1 Stock Directory: every field");
    }
    {
        const auto b = build('H', {{11, 8, 0, "MSFT    "}, {19, 1, 0, "P"}, {20, 1, 0, " "}, {21, 4, 0, "LUDP"}});
        check(decoded(b, m) && is(m.H.stock, "MSFT    ") && m.H.trading_state == 'P' && is(m.H.reason, "LUDP"),
              "1.2.2 Stock Trading Action");
    }
    {
        const auto b = build('Y', {{11, 8, 0, "MSFT    "}, {19, 1, 0, "1"}});
        check(decoded(b, m) && m.Y.action == '1' && is(m.Y.stock, "MSFT    "), "1.2.3 Reg SHO");
    }
    {
        const auto b = build('L', {{11, 4, 0, "GSCO"}, {15, 8, 0, "MSFT    "}, {23, 1, 0, "Y"}, {24, 1, 0, "N"}, {25, 1, 0, "A"}});
        check(decoded(b, m) && is(m.L.participant, "GSCO") && is(m.L.stock, "MSFT    ") && m.L.primary_market_maker == 'Y' &&
                  m.L.market_maker_mode == 'N' && m.L.participant_state == 'A',
              "1.2.4 Market Participant Position");
    }
    {
        const auto b = build('V', {{11, 8, 300000000000ull}, {19, 8, 280000000000ull}, {27, 8, 0x0102030405060708ull}});
        check(decoded(b, m) && m.V.level1 == 300000000000ull && m.V.level2 == 280000000000ull && m.V.level3 == 0x0102030405060708ull,
              "1.2.5.1 MWCB Decline Level: three Price(8) fields");
    }
    {
        const auto b = build('W', {{11, 1, 0, "2"}});
        check(decoded(b, m) && m.W.breached_level == '2', "1.2.5.2 MWCB Status");
    }
    {
        const auto b = build('K', {{11, 8, 0, "NEWCO   "}, {19, 4, 36000}, {23, 1, 0, "A"}, {24, 4, 150000}});
        check(decoded(b, m) && m.K.release_time_s == 36000 && m.K.release_qualifier == 'A' && m.K.ipo_price == 150000,
              "1.2.6 IPO Quoting Period Update");
    }
    {
        const auto b = build('J', {{11, 8, 0, "XYZ     "}, {19, 4, 1000000}, {23, 4, 1050000}, {27, 4, 950000}, {31, 4, 2}});
        check(decoded(b, m) && m.J.reference_price == 1000000 && m.J.upper_price == 1050000 && m.J.lower_price == 950000 &&
                  m.J.extension == 2,
              "1.2.7 LULD Auction Collar");
    }
    {
        const auto b = build('h', {{11, 8, 0, "XYZ     "}, {19, 1, 0, "B"}, {20, 1, 0, "H"}});
        check(decoded(b, m) && m.type == 'h' && m.h.market_code == 'B' && m.h.action == 'H', "1.2.8 Operational Halt");
    }
    {
        const auto b = build('A', {{11, 8, 0x1122334455667788ull}, {19, 1, 0, "S"}, {20, 4, 300}, {24, 8, 0, "AAPL    "}, {32, 4, 3201500}});
        check(decoded(b, m) && m.A.ref == 0x1122334455667788ull && m.A.side == 'S' && m.A.shares == 300 && is(m.A.stock, "AAPL    ") &&
                  m.A.price == 3201500 && is(m.A.attribution, "    "),
              "1.3.1 Add Order");
    }
    {
        const auto b = build('F', {{11, 8, 42}, {19, 1, 0, "B"}, {20, 4, 100}, {24, 8, 0, "AAPL    "}, {32, 4, 3200000}, {36, 4, 0, "NSDQ"}});
        check(decoded(b, m) && m.type == 'F' && m.A.ref == 42 && is(m.A.attribution, "NSDQ"), "1.3.2 Add Order with MPID attribution");
    }
    {
        const auto b = build('E', {{11, 8, 42}, {19, 4, 75}, {23, 8, 0x0F0E0D0C0B0A0908ull}});
        check(decoded(b, m) && m.E.ref == 42 && m.E.shares == 75 && m.E.match == 0x0F0E0D0C0B0A0908ull, "1.4.1 Order Executed");
    }
    {
        const auto b = build('C', {{11, 8, 42}, {19, 4, 25}, {23, 8, 9}, {31, 1, 0, "N"}, {32, 4, 3199900}});
        check(decoded(b, m) && m.E.shares == 25 && m.E.match == 9 && m.E.printable == 'N' && m.E.price == 3199900,
              "1.4.2 Order Executed with Price");
    }
    {
        const auto b = build('X', {{11, 8, 42}, {19, 4, 10}});
        check(decoded(b, m) && m.X.ref == 42 && m.X.shares == 10, "1.4.3 Order Cancel");
    }
    {
        const auto b = build('D', {{11, 8, 42}});
        check(decoded(b, m) && m.D.ref == 42, "1.4.4 Order Delete");
    }
    {
        const auto b = build('U', {{11, 8, 42}, {19, 8, 43}, {27, 4, 500}, {31, 4, 3100000}});
        check(decoded(b, m) && m.U.old_ref == 42 && m.U.new_ref == 43 && m.U.shares == 500 && m.U.price == 3100000, "1.4.5 Order Replace");
    }
    {
        const auto b = build('P', {{11, 8, 0}, {19, 1, 0, "B"}, {20, 4, 200}, {24, 8, 0, "AAPL    "}, {32, 4, 3201000}, {36, 8, 77}});
        check(decoded(b, m) && m.P.side == 'B' && m.P.shares == 200 && m.P.price == 3201000 && m.P.match == 77,
              "1.5.1 Trade (non-cross)");
    }
    {
        const auto b = build('Q', {{11, 8, 5000000000ull}, {19, 8, 0, "AAPL    "}, {27, 4, 3200000}, {31, 8, 78}, {39, 1, 0, "O"}});
        check(decoded(b, m) && m.Q.shares == 5000000000ull && m.Q.price == 3200000 && m.Q.match == 78 && m.Q.cross_type == 'O',
              "1.5.2 Cross Trade: 8-byte shares");
    }
    {
        const auto b = build('B', {{11, 8, 77}});
        check(decoded(b, m) && m.B.match == 77, "1.5.3 Broken Trade");
    }
    {
        const auto b = build('I', {{11, 8, 1000}, {19, 8, 250}, {27, 1, 0, "S"}, {28, 8, 0, "AAPL    "}, {36, 4, 3150000},
                                   {40, 4, 3160000}, {44, 4, 3170000}, {48, 1, 0, "C"}, {49, 1, 0, "L"}});
        check(decoded(b, m) && m.I.paired_shares == 1000 && m.I.imbalance_shares == 250 && m.I.direction == 'S' && m.I.far_price == 3150000 &&
                  m.I.near_price == 3160000 && m.I.reference_price == 3170000 && m.I.cross_type == 'C' && m.I.price_variation == 'L',
              "1.6 NOII");
    }
    {
        const auto b = build('N', {{11, 8, 0, "AAPL    "}, {19, 1, 0, "A"}});
        check(decoded(b, m) && m.N.interest_flag == 'A', "1.7 Retail Price Improvement Indicator");
    }
    {
        const auto b = build('O', {{11, 8, 0, "DLCR    "}, {19, 1, 0, "Y"}, {20, 4, 80000}, {24, 4, 180000}, {28, 4, 100000},
                                   {32, 8, 36000000000000ull}, {40, 4, 90000}, {44, 4, 110000}});
        check(decoded(b, m) && m.O.open_eligibility == 'Y' && m.O.min_price == 80000 && m.O.max_price == 180000 && m.O.near_price == 100000 &&
                  m.O.near_time == 36000000000000ull && m.O.lower_collar == 90000 && m.O.upper_collar == 110000,
              "1.8 Direct Listing with Capital Raise");
    }
}

void refused() {
    message m{};
    const unsigned char d[19] = {'D'};
    check(decode_reference(d, 18, m) == decode_status::wrong_length && decode_reference(d, 20, m) == decode_status::wrong_length,
          "a message shorter or longer than its type's length is refused");
    const unsigned char z[19] = {'Z'};
    check(decode_reference(z, 19, m) == decode_status::unknown_type, "a type the specification does not define is refused");
    check(decode_reference(d, 0, m) == decode_status::wrong_length, "an empty message is refused");
}

}  // namespace

int main() {
    every_type();
    refused();
    return itchfh::test::result();
}
