// Market status (note 003, D6): each instrument's trading state and reason,
// Reg SHO, operational halt per Nasdaq market, LULD collar, IPO quoting
// period, retail interest, direct-listing price discovery and latest
// imbalance; market participant positions by instrument and MPID; and the
// market-wide state: the System Event phase (checked to come in the
// specification's order: O S Q M E C) and the circuit breakers. Kept apart
// from the displayed book. Fixed, pre-faulted.
// Design: docs/design/003-events-directory-status.md
#pragma once

#include <itchfh/fixed_map.hpp>
#include <itchfh/handler_types.hpp>
#include <itchfh/memory.hpp>
#include <itchfh/messages.hpp>

#include <cstdint>
#include <cstring>

namespace itchfh {

struct participant_view {
    char primary_market_maker, market_maker_mode, participant_state;
    bool operator==(const participant_view&) const = default;
};

class market_status {
public:
    market_status(std::size_t participants, bool prefault)
        : of_(zeroed<instrument_status>(65'536, prefault)), participants_(participants, prefault) {}

    // Each sets what its message says; false only where noted.
    void trading_action(std::uint16_t l, const trading_action& m) {
        of_[l].trading_state = m.trading_state;
        of_[l].trading_reason = m.reason;
    }
    void reg_sho(std::uint16_t l, const itchfh::reg_sho& m) { of_[l].reg_sho = m.action; }
    // False if the market code is not one of Nasdaq's three.
    bool operational_halt(std::uint16_t l, const itchfh::operational_halt& m) {
        const int i = m.market_code == 'Q' ? 0 : m.market_code == 'B' ? 1 : m.market_code == 'X' ? 2 : -1;
        if (i < 0) return false;
        of_[l].operational_halt[i] = m.action;
        return true;
    }
    void luld(std::uint16_t l, const luld_collar& m) {
        of_[l].luld = m;
        of_[l].has |= 1;
        of_[l].not_since_snapshot &= ~snapshot_luld;
    }
    void ipo(std::uint16_t l, const ipo_quoting& m) {
        of_[l].ipo = m;
        of_[l].has |= 2;
        of_[l].not_since_snapshot &= ~snapshot_ipo;
    }
    void retail(std::uint16_t l, const retail_interest& m) { of_[l].retail_interest = m.interest_flag; }
    void dlcr(std::uint16_t l, const direct_listing& m) {
        of_[l].dlcr = m;
        of_[l].has |= 4;
        of_[l].not_since_snapshot &= ~snapshot_dlcr;
    }
    void noii(std::uint16_t l, const imbalance& m) {
        of_[l].noii = m;
        of_[l].has |= 8;
        of_[l].not_since_snapshot &= ~snapshot_noii;
    }
    // False if the table had no room.
    bool participant(std::uint16_t l, const participant_position& m) {
        bool fresh;
        participant_view* p = participants_.get(participant_key(l, m.participant), fresh);
        if (!p) return false;
        *p = {m.primary_market_maker, m.market_maker_mode, m.participant_state};
        of_[l].not_since_snapshot &= ~snapshot_participants;
        return true;
    }
    // False if the code is not the next of O S Q M E C.
    bool system_event(char code) {
        static constexpr char order[] = "OSQMEC";
        market_.phase = code;
        if (market_.phases < 6 && order[market_.phases] == code) {
            ++market_.phases;
            return true;
        }
        return false;
    }
    void mwcb_decline(const itchfh::mwcb_decline& m) {
        market_.mwcb = m;
        market_.mwcb_not_since_snapshot = false;
    }
    // Note 006, D5: rebuilt from a snapshot; every instrument's unrestorable fields marked.
    void mark_restored() {
        for (std::size_t l = 0; l < 65'536; ++l) of_[l].not_since_snapshot = snapshot_all;
        market_.mwcb_not_since_snapshot = true;
    }
    void mwcb_status(char level) { market_.mwcb_breached = level; }

    const instrument_status& of(std::uint16_t l) const { return of_[l]; }
    const participant_view* participant_of(std::uint16_t l, const mpid& p) const { return participants_.find(participant_key(l, p)); }
    const market_state& market() const { return market_; }
    std::size_t participants() const { return participants_.size(); }

    static std::uint64_t participant_key(std::uint16_t l, const mpid& p) {
        std::uint32_t m;
        std::memcpy(&m, p.data(), sizeof(m));
        return std::uint64_t{l} << 32 | m;
    }

private:
    table<instrument_status> of_;
    fixed_map<participant_view> participants_;
    market_state market_;
};

}  // namespace itchfh
