// What the event handler reports and keeps (note 003): the trade it reports
// once per match number (D5), the instrument fields a status change names
// (D6), each instrument's trade figures, the market-wide state, and the
// handler's counts of edge cases (S4). Shared by the handler and the
// reference handler, so tests can compare them.
// Design: docs/design/003-events-directory-status.md
#pragma once

#include <itchfh/messages.hpp>

#include <array>
#include <cstdint>

namespace itchfh {

enum class trade_kind : std::uint8_t { displayed = 0, non_displayed = 1, cross = 2 };

// D5: one per match number, when the trade is first counted.
struct trade_report {
    std::uint16_t locate = 0;
    trade_kind kind = trade_kind::displayed;
    std::uint32_t price = 0;
    std::uint64_t shares = 0;
    std::uint64_t match = 0;
    std::uint64_t timestamp_ns = 0;
    bool operator==(const trade_report&) const = default;
};

// D6: which of an instrument's status fields a message set.
enum class status_field : std::uint8_t {
    trading_state, reg_sho, operational_halt, luld_collar, ipo_quoting, retail_interest, direct_listing, participant
};

struct instrument_trades {
    std::uint64_t volume = 0;  // shares, counted once per match number
    std::uint64_t trades = 0;
    std::uint32_t last_price = 0;
    std::uint64_t last_match = 0;
    bool operator==(const instrument_trades&) const = default;
};

// D6: an instrument's status. `has` flags say which have been set today.
struct instrument_status {
    char trading_state = 0;  // 0: no Stock Trading Action yet (treated as halted)
    std::array<char, 4> trading_reason{};
    char reg_sho = 0;
    std::array<char, 3> operational_halt{};  // by market code: Q (Nasdaq), B (BX), X (PSX); 0 none
    luld_collar luld{};
    ipo_quoting ipo{};
    char retail_interest = 0;
    direct_listing dlcr{};
    imbalance noii{};
    std::uint8_t has = 0;  // bits: 1 luld, 2 ipo, 4 dlcr, 8 noii
    // Note 006, D5: after a rebuild from a snapshot, what GLIMPSE could not
    // restore, until a live message sets it again (trades never: the volume
    // before the snapshot is not recovered).
    std::uint8_t not_since_snapshot = 0;  // bits of since_snapshot
};

enum since_snapshot : std::uint8_t {
    snapshot_trades = 1, snapshot_luld = 2, snapshot_ipo = 4, snapshot_dlcr = 8, snapshot_noii = 16, snapshot_participants = 32,
    snapshot_all = 63
};

struct market_state {
    char phase = 0;          // the last System Event code
    std::uint8_t phases = 0; // how many of O S Q M E C have come, in order
    mwcb_decline mwcb{};
    char mwcb_breached = 0;
    bool mwcb_not_since_snapshot = false;  // note 006, D5
};

// S4: edge cases, counted and never fatal.
struct handler_counts {
    std::uint64_t unknown_type = 0;          // a message type the handler does not know
    std::uint64_t second_directory = 0;      // a further Stock Directory for a locate (D4)
    std::uint64_t no_directory = 0;          // a message for a locate with no directory entry (D4)
    std::uint64_t unknown_break = 0;         // a Broken Trade for a match number never seen (D5)
    std::uint64_t phase_out_of_order = 0;    // a System Event not in the specification's order (D6)
    std::uint64_t table_full = 0;            // a fixed table out of room
    std::uint64_t bad_field = 0;             // a field outside the specification's values (an operational halt's market code)
    bool operator==(const handler_counts&) const = default;
};

}  // namespace itchfh
