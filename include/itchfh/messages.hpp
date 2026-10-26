// The decoded forms of every TotalView-ITCH 5.0 message (note 003, D1): the
// specification's fields, in its order and units, host byte order. Prices are
// integers with the decimals the specification states (Price(4), or Price(8)
// for the circuit-breaker levels); alpha fields keep their bytes, symbols and
// MPIDs space-padded as sent; timestamps are nanoseconds since midnight. The
// header every message carries (type, Stock Locate, tracking number,
// timestamp) is in `message`; each type's own fields in its struct.
// Section numbers are the specification's (revised 28 April 2023).
// Design: docs/design/003-events-directory-status.md
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace itchfh {

using symbol = std::array<char, 8>;  // right-padded with spaces
using mpid = std::array<char, 4>;

// 1.1
struct system_event { char event_code; };
// 1.2.1
struct stock_directory {
    symbol stock;
    char market_category, financial_status;
    std::uint32_t round_lot_size;
    char round_lots_only, issue_classification;
    std::array<char, 2> issue_sub_type;
    char authenticity, short_sale_threshold, ipo_flag, luld_tier, etp_flag;
    std::uint32_t etp_leverage_factor;
    char inverse_indicator;
};
// 1.2.2
struct trading_action {
    symbol stock;
    char trading_state, reserved;
    std::array<char, 4> reason;
};
// 1.2.3
struct reg_sho { symbol stock; char action; };
// 1.2.4
struct participant_position {
    mpid participant;
    symbol stock;
    char primary_market_maker, market_maker_mode, participant_state;
};
// 1.2.5.1, 1.2.5.2
struct mwcb_decline { std::uint64_t level1, level2, level3; };  // Price(8)
struct mwcb_status { char breached_level; };
// 1.2.6
struct ipo_quoting {
    symbol stock;
    std::uint32_t release_time_s;  // seconds since midnight
    char release_qualifier;
    std::uint32_t ipo_price;
};
// 1.2.7
struct luld_collar {
    symbol stock;
    std::uint32_t reference_price, upper_price, lower_price, extension;
};
// 1.2.8
struct operational_halt { symbol stock; char market_code, action; };
// 1.3.1, 1.3.2 (`attribution` blank for A)
struct add_order {
    std::uint64_t ref;
    char side;
    std::uint32_t shares;
    symbol stock;
    std::uint32_t price;
    mpid attribution;
};
// 1.4.1, 1.4.2 (`printable` and `price` only for C)
struct order_executed {
    std::uint64_t ref;
    std::uint32_t shares;
    std::uint64_t match;
    char printable;
    std::uint32_t price;
};
// 1.4.3, 1.4.4
struct order_cancel { std::uint64_t ref; std::uint32_t shares; };
struct order_delete { std::uint64_t ref; };
// 1.4.5
struct order_replace { std::uint64_t old_ref, new_ref; std::uint32_t shares, price; };
// 1.5.1
struct trade {
    std::uint64_t ref;  // always zero (since 2010)
    char side;          // always 'B' (since 2014)
    std::uint32_t shares;
    symbol stock;
    std::uint32_t price;
    std::uint64_t match;
};
// 1.5.2
struct cross_trade {
    std::uint64_t shares;
    symbol stock;
    std::uint32_t price;
    std::uint64_t match;
    char cross_type;
};
// 1.5.3
struct broken_trade { std::uint64_t match; };
// 1.6
struct imbalance {
    std::uint64_t paired_shares, imbalance_shares;
    char direction;
    symbol stock;
    std::uint32_t far_price, near_price, reference_price;
    char cross_type, price_variation;
};
// 1.7
struct retail_interest { symbol stock; char interest_flag; };
// 1.8
struct direct_listing {
    symbol stock;
    char open_eligibility;
    std::uint32_t min_price, max_price, near_price;
    std::uint64_t near_time;
    std::uint32_t lower_collar, upper_collar;
};

struct message {
    char type;  // the specification's message type
    std::uint16_t locate, tracking;
    std::uint64_t timestamp_ns;
    union {
        system_event S;
        stock_directory R;
        trading_action H;
        reg_sho Y;
        participant_position L;
        mwcb_decline V;
        mwcb_status W;
        ipo_quoting K;
        luld_collar J;
        operational_halt h;
        add_order A;  // and F
        order_executed E;  // and C
        order_cancel X;
        order_delete D;
        order_replace U;
        trade P;
        cross_trade Q;
        broken_trade B;
        imbalance I;
        retail_interest N;
        direct_listing O;
    };
};
static_assert(std::is_trivially_copyable_v<message>);

// Each type's length on the wire, from the specification's layouts; 0 for a
// type it does not define.
constexpr std::size_t message_length(char type) noexcept {
    switch (type) {
        case 'S': return 12;
        case 'R': return 39;
        case 'H': return 25;
        case 'Y': return 20;
        case 'L': return 26;
        case 'V': return 35;
        case 'W': return 12;
        case 'K': return 28;
        case 'J': return 35;
        case 'h': return 21;
        case 'A': return 36;
        case 'F': return 40;
        case 'E': return 31;
        case 'C': return 36;
        case 'X': return 23;
        case 'D': return 19;
        case 'U': return 35;
        case 'P': return 44;
        case 'Q': return 40;
        case 'B': return 19;
        case 'I': return 50;
        case 'N': return 20;
        case 'O': return 48;
        default: return 0;
    }
}

}  // namespace itchfh
