// A fixed-capacity map from a 64-bit key to a value, for the day's tables of
// note 003 (symbols to locates, match numbers to trades, market participant
// positions): open addressing, linear probing, at most half full, allocated
// and pre-faulted at construction; entries are never removed within a day.
// Design: docs/design/003-events-directory-status.md
#pragma once

#include <itchfh/memory.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace itchfh {

template <class V>
class fixed_map {
    static_assert(std::is_trivially_copyable_v<V>);

public:
    // Room for `entries` (twice as many slots).
    fixed_map(std::size_t entries, bool prefault)
        : mask_(pow2_at_least(2 * entries) - 1), shift_(64 - bits(mask_ + 1)), slots_(zeroed<slot>(mask_ + 1, prefault)) {}

    // The value for `key`, inserted zeroed if absent; nullptr if full.
    // `fresh` says whether it was inserted.
    V* get(std::uint64_t key, bool& fresh) {
        const std::uint64_t k = key + 1;
        std::size_t s = home(key);
        while (slots_[s].key != 0 && slots_[s].key != k) s = (s + 1) & mask_;
        fresh = slots_[s].key == 0;
        if (fresh) {
            if ((count_ + 1) * 2 > mask_ + 1) return nullptr;
            slots_[s].key = k;
            ++count_;
        }
        return &slots_[s].value;
    }
    V* find(std::uint64_t key) { return const_cast<V*>(static_cast<const fixed_map&>(*this).find(key)); }
    const V* find(std::uint64_t key) const {
        for (std::size_t s = home(key);; s = (s + 1) & mask_) {
            if (slots_[s].key == 0) return nullptr;
            if (slots_[s].key == key + 1) return &slots_[s].value;
        }
    }
    std::size_t size() const { return count_; }

private:
    struct slot {
        std::uint64_t key;  // the key + 1; 0 is empty (a key of all ones is not supported)
        V value;
    };
    static unsigned bits(std::size_t n) {
        unsigned b = 0;
        while ((std::size_t{1} << b) < n) ++b;
        return b;
    }
    std::size_t home(std::uint64_t key) const { return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_); }

    std::size_t mask_;
    unsigned shift_;
    table<slot> slots_;
    std::size_t count_ = 0;
};

}  // namespace itchfh
