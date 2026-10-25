// Finding an order by its reference number (note 002, D1), three ways, each
// mapping a reference number to the order's index in the book's pool:
//   A  std::unordered_map, as the reference book uses (allocates as it grows)
//   B  open addressing, linear probing, fixed capacity, pre-faulted
//   C  a direct array over [ref_base, ref_base + ref_span), B for the rest
// The book checks a number is not live before inserting it.
// Design: docs/design/002-order-book.md
#pragma once

#include <itchfh/book_config.hpp>
#include <itchfh/memory.hpp>

#include <cstdint>
#include <unordered_map>

namespace itchfh {

inline constexpr std::uint32_t npos = 0xFFFFFFFFu;

struct ref_map {
    static constexpr const char* name = "A: unordered_map";
    explicit ref_map(const book_config& c) { m_.reserve(c.orders); }
    bool insert(std::uint64_t ref, std::uint32_t i) {
        m_.emplace(ref, i);
        return true;
    }
    std::uint32_t find(std::uint64_t ref) const {
        const auto it = m_.find(ref);
        return it == m_.end() ? npos : it->second;
    }
    void erase(std::uint64_t ref) { m_.erase(ref); }

private:
    std::unordered_map<std::uint64_t, std::uint32_t> m_;
};

// At most half full; a deletion shifts the following entries of its run back
// (no tombstones, so lookups never slow down over the day).
class ref_hash {
public:
    static constexpr const char* name = "B: open addressing";
    explicit ref_hash(const book_config& c) : ref_hash(c.ref_slots ? c.ref_slots : 2 * c.orders, c.prefault) {}
    ref_hash(std::size_t slots, bool prefault)
        : mask_(pow2_at_least(slots) - 1), shift_(64 - log2(mask_ + 1)), slots_(zeroed<entry>(mask_ + 1, prefault)) {}

    bool insert(std::uint64_t ref, std::uint32_t i) {
        if ((count_ + 1) * 2 > mask_ + 1) return false;
        std::size_t s = home(ref);
        while (slots_[s].value != 0) s = (s + 1) & mask_;
        slots_[s] = {ref, i + 1, 0};
        ++count_;
        return true;
    }
    std::uint32_t find(std::uint64_t ref) const {
        for (std::size_t s = home(ref);; s = (s + 1) & mask_) {
            const entry& e = slots_[s];
            if (e.value == 0) return npos;
            if (e.key == ref) return e.value - 1;
        }
    }
    void erase(std::uint64_t ref) {
        std::size_t s = home(ref);
        while (slots_[s].value != 0 && slots_[s].key != ref) s = (s + 1) & mask_;
        if (slots_[s].value == 0) return;
        // Backward shift: move each later entry of the run into the hole if its
        // home is not between the hole and its slot.
        std::size_t hole = s;
        for (std::size_t j = (s + 1) & mask_; slots_[j].value != 0; j = (j + 1) & mask_) {
            const std::size_t h = home(slots_[j].key);
            const bool stays = hole <= j ? (hole < h && h <= j) : (hole < h || h <= j);
            if (!stays) {
                slots_[hole] = slots_[j];
                hole = j;
            }
        }
        slots_[hole] = {};
        --count_;
    }

private:
    struct entry {
        std::uint64_t key;
        std::uint32_t value;  // index + 1; 0 is empty
        std::uint32_t unused;
    };
    static unsigned log2(std::size_t n) {
        unsigned b = 0;
        while ((std::size_t{1} << b) < n) ++b;
        return b;
    }
    std::size_t home(std::uint64_t ref) const { return static_cast<std::size_t>((ref * 0x9E3779B97F4A7C15ull) >> shift_); }

    std::size_t mask_;
    unsigned shift_;
    table<entry> slots_;
    std::size_t count_ = 0;
};

class ref_direct {
public:
    static constexpr const char* name = "C: direct array";
    explicit ref_direct(const book_config& c)
        : base_(c.ref_base), span_(c.ref_span), slots_(zeroed<std::uint32_t>(c.ref_span, c.prefault)),
          rest_(c.ref_slots ? c.ref_slots : 2 * c.orders, c.prefault) {}

    bool insert(std::uint64_t ref, std::uint32_t i) {
        if (ref - base_ < span_) {
            slots_[ref - base_] = i + 1;
            return true;
        }
        return rest_.insert(ref, i);
    }
    std::uint32_t find(std::uint64_t ref) const { return ref - base_ < span_ ? slots_[ref - base_] - 1 : rest_.find(ref); }
    void erase(std::uint64_t ref) {
        if (ref - base_ < span_) slots_[ref - base_] = 0;
        else rest_.erase(ref);
    }

private:
    std::uint64_t base_, span_;
    table<std::uint32_t> slots_;  // index + 1; 0 is empty
    ref_hash rest_;
};

}  // namespace itchfh
