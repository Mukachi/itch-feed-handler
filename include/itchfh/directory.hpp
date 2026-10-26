// The instrument directory (note 003, D4): the day's Stock Directory entries,
// by Stock Locate (an array: the field is 16-bit, meant as an index), and an
// index from symbol to locate for consumers that ask by symbol. A further
// entry for a locate replaces the first. Fixed, pre-faulted.
// Design: docs/design/003-events-directory-status.md
#pragma once

#include <itchfh/fixed_map.hpp>
#include <itchfh/memory.hpp>
#include <itchfh/messages.hpp>

#include <cstdint>
#include <cstring>
#include <optional>

namespace itchfh {

class directory {
public:
    static constexpr std::size_t locates = 65'536;

    explicit directory(bool prefault = true) : entries_(zeroed<entry>(locates, prefault)), symbols_(locates, prefault) {}

    // Sets a locate's entry. False if it had one already (it is replaced).
    // `full`: the symbol index had no room (the entry is still set).
    bool set(std::uint16_t locate, const stock_directory& r, bool& full) {
        entry& e = entries_[locate];
        const bool first = !e.present;
        e.r = r;
        e.present = 1;
        bool fresh;
        std::uint16_t* l = symbols_.get(key(r.stock), fresh);
        full = l == nullptr;
        if (l) *l = locate;
        return first;
    }
    const stock_directory* find(std::uint16_t locate) const { return entries_[locate].present ? &entries_[locate].r : nullptr; }
    std::optional<std::uint16_t> locate_of(const symbol& s) const {
        const std::uint16_t* l = symbols_.find(key(s));
        return l ? std::optional<std::uint16_t>(*l) : std::nullopt;
    }
    std::size_t size() const { return symbols_.size(); }

private:
    struct entry {
        stock_directory r;
        std::uint8_t present;
    };
    static std::uint64_t key(const symbol& s) {
        std::uint64_t k;
        std::memcpy(&k, s.data(), sizeof(k));
        return k;
    }
    table<entry> entries_;
    fixed_map<std::uint16_t> symbols_;
};

}  // namespace itchfh
