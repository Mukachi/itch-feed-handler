// What every book shares (note 002): how a level is seen from outside, the
// bad events of D5, and what applying an event changed. The fast book and the
// reference book both answer in these terms, so tests can compare them.
// Design: docs/design/002-order-book.md
#pragma once

#include <cstdint>

namespace itchfh {

enum class side : std::uint8_t { bid = 0, ask = 1 };

struct level_view {
    std::uint32_t price = 0;   // four implied decimals
    std::uint64_t shares = 0;  // displayed, all orders at the price
    std::uint32_t orders = 0;
    bool operator==(const level_view&) const = default;
};

struct order_view {
    std::uint16_t locate = 0;
    side s = side::bid;
    std::uint32_t shares = 0, price = 0;
    bool operator==(const order_view&) const = default;
};

// D5: an event that cannot be applied as it stands, meaning a message was
// missed or misapplied upstream. Never applied; counted; its book marked
// unreliable. `full` is D4's: capacity reached.
enum class bad_event : std::uint8_t {
    none = 0,
    unknown_ref,     // executed, cancel, delete or replace of no live order
    too_many_shares, // executed or cancel of more shares than remain
    duplicate_ref,   // add or replace to a reference number already live
    malformed,       // a side other than B or S, zero shares on an add, an unknown type
    full,            // no room for the order or its level
};
inline constexpr int bad_event_kinds = 6;

// What one event changed: up to two levels (a replace moves an order), each
// named by side and price. A test compares exactly these after every event.
struct applied {
    bad_event bad = bad_event::none;
    std::uint16_t locate = 0;
    std::uint8_t changed = 0;  // levels in `level`
    struct {
        side s;
        std::uint32_t price;
    } level[2] = {};
};

}  // namespace itchfh
