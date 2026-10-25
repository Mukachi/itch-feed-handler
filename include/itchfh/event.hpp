// What the order book takes (note 002): one decoded TotalView-ITCH 5.0 event
// that can change a displayed book or the state it is judged in. Fixed size
// and trivially copyable, so a day of them can be kept in a file of records
// (note 002, D7) and read back without parsing.
// Design: docs/design/002-order-book.md
#pragma once

#include <cstdint>
#include <type_traits>

namespace itchfh {

enum class event_type : std::uint8_t {
    add = 1,              // A, F: a new displayed order
    executed = 2,         // E
    executed_price = 3,   // C: executed at another price; the display price stands
    cancel = 4,           // X: part of the shares
    remove = 5,           // D: the whole order
    replace = 6,          // U: a new reference number, shares and price; same side and instrument
    trading_state = 7,    // H: the instrument's trading state (note 002, D6)
};

struct event {
    std::uint64_t timestamp_ns;  // since midnight
    std::uint64_t ref;           // the order's reference number (replace: the old one)
    std::uint64_t new_ref;       // replace: the new reference number; otherwise 0
    std::uint32_t shares;        // add, replace: displayed; executed, cancel: removed
    std::uint32_t price;         // add, replace, executed_price: four implied decimals
    std::uint16_t locate;        // Stock Locate code
    event_type type;
    char side;                   // add: 'B' or 'S'; trading_state: 'H', 'P', 'Q' or 'T'
    std::uint32_t reserved = 0;
};
static_assert(sizeof(event) == 40 && std::is_trivially_copyable_v<event>);

}  // namespace itchfh
