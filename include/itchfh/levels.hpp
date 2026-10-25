// Keeping one side of a book's price levels (note 002, D2), three ways, each
// mapping a price to the level's index in the book's pool and giving the best:
//   A  std::map, as the reference book uses (allocates per level)
//   B  a sorted array, best at the end (most changes are near the best), in
//      chunks of an arena that double as a side grows; fixed, pre-faulted
//   C  an array indexed by price key in a window around where the side
//      started, with a bitmap to find the best; prices outside the window, or
//      off the tick grid, in a B array beside it
// Bid sides keep higher prices better, ask sides lower.
// Design: docs/design/002-order-book.md
#pragma once

#include <itchfh/book_config.hpp>
#include <itchfh/memory.hpp>
#include <itchfh/ref_tables.hpp>

#include <bit>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <type_traits>

namespace itchfh {

// ------------------------------------------------------------------- A
struct levels_map {
    static constexpr const char* name = "A: std::map";
    struct shared {
        explicit shared(const book_config&) {}
    };
    template <bool Bid>
    class side {
    public:
        std::uint32_t find(const shared&, std::uint32_t price) const {
            const auto it = m_.find(price);
            return it == m_.end() ? npos : it->second;
        }
        bool insert(shared&, std::uint32_t price, std::uint32_t i) {
            m_.emplace(price, i);
            return true;
        }
        void erase(shared&, std::uint32_t price) { m_.erase(price); }
        std::uint32_t best(const shared&) const { return m_.empty() ? npos : m_.begin()->second; }
        template <class F>
        void each(const shared&, F&& f) const {
            for (const auto& [p, i] : m_) f(i);
        }

    private:
        std::map<std::uint32_t, std::uint32_t, std::conditional_t<Bid, std::greater<>, std::less<>>> m_;
    };
};

// ------------------------------------------------------------------- B
// Chunks of 8 << k entries (k = 0..11: 8 to 16,384 levels a side); a freed
// chunk goes on its size's list. An entry is the price in the high half and
// the level's index in the low.
class level_arena {
public:
    static constexpr int classes = 12;
    level_arena(std::size_t entries, bool prefault) : size_(entries), mem_(zeroed<std::uint64_t>(entries, prefault)) {
        for (auto& h : free_) h = npos;
    }
    static std::uint32_t capacity(int k) { return 8u << k; }
    std::uint32_t alloc(int k) {
        if (k >= classes) return npos;
        if (free_[k] != npos) {
            const std::uint32_t off = free_[k];
            free_[k] = static_cast<std::uint32_t>(mem_[off]);
            return off;
        }
        if (used_ + capacity(k) > size_) return npos;
        const auto off = static_cast<std::uint32_t>(used_);
        used_ += capacity(k);
        return off;
    }
    void release(std::uint32_t off, int k) {
        mem_[off] = free_[k];
        free_[k] = off;
    }
    std::uint64_t* at(std::uint32_t off) { return &mem_[off]; }
    const std::uint64_t* at(std::uint32_t off) const { return &mem_[off]; }

private:
    std::size_t size_, used_ = 0;
    table<std::uint64_t> mem_;
    std::uint32_t free_[classes];
};

template <bool Bid>
class sorted_side {
public:
    std::uint32_t find(const level_arena& a, std::uint32_t price) const {
        const std::uint32_t pos = lower(a, price);
        return pos < n_ && price_of(a.at(off_)[pos]) == price ? index_of(a.at(off_)[pos]) : npos;
    }
    bool insert(level_arena& a, std::uint32_t price, std::uint32_t i) {
        if (off_ == npos || n_ == level_arena::capacity(k_)) {
            const int k = off_ == npos ? 0 : k_ + 1;
            const std::uint32_t off = a.alloc(k);
            if (off == npos) return false;
            if (off_ != npos) {
                std::memcpy(a.at(off), a.at(off_), n_ * sizeof(std::uint64_t));
                a.release(off_, k_);
            }
            off_ = off;
            k_ = k;
        }
        const std::uint32_t pos = lower(a, price);
        std::uint64_t* e = a.at(off_);
        std::memmove(e + pos + 1, e + pos, (n_ - pos) * sizeof(std::uint64_t));
        e[pos] = std::uint64_t{price} << 32 | i;
        ++n_;
        return true;
    }
    void erase(level_arena& a, std::uint32_t price) {
        const std::uint32_t pos = lower(a, price);
        if (pos >= n_ || price_of(a.at(off_)[pos]) != price) return;
        std::uint64_t* e = a.at(off_);
        std::memmove(e + pos, e + pos + 1, (n_ - pos - 1) * sizeof(std::uint64_t));
        if (--n_ == 0) {
            a.release(off_, k_);
            off_ = npos;
        }
    }
    std::uint32_t best(const level_arena& a) const { return n_ ? index_of(a.at(off_)[n_ - 1]) : npos; }
    std::uint32_t best_price(const level_arena& a) const { return price_of(a.at(off_)[n_ - 1]); }
    bool empty() const { return n_ == 0; }
    template <class F>
    void each(const level_arena& a, F&& f) const {
        for (std::uint32_t j = n_; j-- > 0;) f(index_of(a.at(off_)[j]), price_of(a.at(off_)[j]));
    }

private:
    static std::uint32_t price_of(std::uint64_t e) { return static_cast<std::uint32_t>(e >> 32); }
    static std::uint32_t index_of(std::uint64_t e) { return static_cast<std::uint32_t>(e); }
    // Kept worst first: bids ascending, asks descending. The first position
    // whose price is not worse than `price`.
    std::uint32_t lower(const level_arena& a, std::uint32_t price) const {
        if (n_ == 0) return 0;
        const std::uint64_t* e = a.at(off_);
        std::uint32_t lo = 0, hi = n_;
        while (lo < hi) {
            const std::uint32_t mid = (lo + hi) / 2;
            const std::uint32_t p = price_of(e[mid]);
            if (Bid ? p < price : p > price) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }
    std::uint32_t off_ = npos, n_ = 0;
    int k_ = 0;
};

struct levels_sorted {
    static constexpr const char* name = "B: sorted array";
    struct shared {
        explicit shared(const book_config& c) : arena(c.level_arena, c.prefault) {}
        level_arena arena;
    };
    template <bool Bid>
    struct side {
        std::uint32_t find(const shared& s, std::uint32_t price) const { return a_.find(s.arena, price); }
        bool insert(shared& s, std::uint32_t price, std::uint32_t i) { return a_.insert(s.arena, price, i); }
        void erase(shared& s, std::uint32_t price) { a_.erase(s.arena, price); }
        std::uint32_t best(const shared& s) const { return a_.best(s.arena); }
        template <class F>
        void each(const shared& s, F&& f) const {
            a_.each(s.arena, [&](std::uint32_t i, std::uint32_t) { f(i); });
        }

    private:
        sorted_side<Bid> a_;
    };
};

// ------------------------------------------------------------------- C
// A price's key: below $1.00 (10,000 in four implied decimals) the price itself,
// from $1.00 a cent a key; a price at or above $1.00 off the cent grid has none.
inline constexpr std::uint32_t no_key = 0xFFFFFFFFu;
inline std::uint32_t price_key(std::uint32_t price) {
    if (price < 10'000) return price;
    return price % 100 == 0 ? 10'000 + (price - 10'000) / 100 : no_key;
}
inline std::uint32_t key_price(std::uint32_t key) { return key < 10'000 ? key : 10'000 + (key - 10'000) * 100; }

struct levels_window {
    static constexpr const char* name = "C: price window";
    struct shared {
        explicit shared(const book_config& c)
            : width(static_cast<std::uint32_t>((c.price_window + 63) / 64 * 64)), windows(c.windows),
              slots(zeroed<std::uint32_t>(windows * width, c.prefault)), bits(zeroed<std::uint64_t>(windows * width / 64, c.prefault)),
              arena(c.level_arena, c.prefault) {}
        std::uint32_t width;
        std::size_t windows, next = 0;
        table<std::uint32_t> slots;  // level index + 1; 0 is empty
        table<std::uint64_t> bits;   // a bit per slot in use
        level_arena arena;           // the overflow arrays
    };
    template <bool Bid>
    class side {
    public:
        std::uint32_t find(const shared& s, std::uint32_t price) const {
            const std::uint32_t k = price_key(price);
            if (in_window(s, k)) return s.slots[slot0(s) + (k - base_)] - 1;
            return over_.find(s.arena, price);
        }
        bool insert(shared& s, std::uint32_t price, std::uint32_t i) {
            const std::uint32_t k = price_key(price);
            if (k != no_key) {
                if (win_ == npos && s.next < s.windows) win_ = static_cast<std::uint32_t>(s.next++);
                if (win_ != npos && count_ == 0 && over_.empty()) {  // an empty side: centre the window here
                    base_ = k > s.width / 2 ? k - s.width / 2 : 0;
                }
            }
            if (!in_window(s, k)) return over_.insert(s.arena, price, i);
            const std::uint32_t slot = k - base_;
            s.slots[slot0(s) + slot] = i + 1;
            s.bits[(slot0(s) + slot) / 64] |= std::uint64_t{1} << (slot % 64);
            if (count_++ == 0 || better(slot, best_slot_)) best_slot_ = slot;
            return true;
        }
        void erase(shared& s, std::uint32_t price) {
            const std::uint32_t k = price_key(price);
            if (!in_window(s, k)) return over_.erase(s.arena, price);
            const std::uint32_t slot = k - base_;
            if (s.slots[slot0(s) + slot] == 0) return;
            s.slots[slot0(s) + slot] = 0;
            s.bits[(slot0(s) + slot) / 64] &= ~(std::uint64_t{1} << (slot % 64));
            if (--count_ > 0 && slot == best_slot_) best_slot_ = next_from(s, slot);
        }
        std::uint32_t best(const shared& s) const {
            if (count_ == 0) return over_.best(s.arena);
            if (over_.empty()) return s.slots[slot0(s) + best_slot_] - 1;
            const std::uint32_t wp = key_price(base_ + best_slot_), op = over_.best_price(s.arena);
            return (Bid ? op > wp : op < wp) ? over_.best(s.arena) : s.slots[slot0(s) + best_slot_] - 1;
        }
        template <class F>
        void each(const shared& s, F&& f) const {
            // the window's levels and the overflow's, merged best first
            std::uint32_t idx[2] = {}, price[2] = {};
            std::uint32_t slot = count_ ? best_slot_ : npos;
            std::uint32_t remaining = count_;
            struct item {
                std::uint32_t i, p;
            };
            item over[16384];  // a side's overflow is at most the arena's largest chunk
            std::uint32_t n_over = 0;
            over_.each(s.arena, [&](std::uint32_t i, std::uint32_t p) { over[n_over++] = {i, p}; });
            std::uint32_t o = 0;
            while (remaining > 0 || o < n_over) {
                bool have_w = remaining > 0, have_o = o < n_over;
                if (have_w) {
                    idx[0] = s.slots[slot0(s) + slot] - 1;
                    price[0] = key_price(base_ + slot);
                }
                if (have_o) {
                    idx[1] = over[o].i;
                    price[1] = over[o].p;
                }
                const bool take_w = have_w && (!have_o || (Bid ? price[0] > price[1] : price[0] < price[1]));
                if (take_w) {
                    f(idx[0]);
                    if (--remaining > 0) slot = next_from(s, slot);
                } else {
                    f(idx[1]);
                    ++o;
                }
            }
        }

    private:
        std::size_t slot0(const shared& s) const { return std::size_t{win_} * s.width; }
        bool in_window(const shared& s, std::uint32_t k) const { return k != no_key && win_ != npos && k >= base_ && k - base_ < s.width; }
        bool better(std::uint32_t a, std::uint32_t b) const { return Bid ? a > b : a < b; }
        // The next slot in use after `slot`, going worse (bids down, asks up).
        std::uint32_t next_from(const shared& s, std::uint32_t slot) const {
            const std::uint64_t* w = &s.bits[slot0(s) / 64];
            const std::uint32_t words = s.width / 64;
            std::uint32_t wi = slot / 64;
            const std::uint32_t bit = slot % 64;
            if (Bid) {  // the highest slot in use below `slot`
                std::uint64_t m = w[wi] & ((std::uint64_t{1} << bit) - 1);
                while (m == 0 && wi > 0) m = w[--wi];
                return m == 0 ? npos : wi * 64 + 63 - static_cast<std::uint32_t>(std::countl_zero(m));
            }
            // the lowest slot in use above `slot`
            std::uint64_t m = bit == 63 ? 0 : w[wi] & ~((std::uint64_t{2} << bit) - 1);
            while (m == 0 && wi + 1 < words) m = w[++wi];
            return m == 0 ? npos : wi * 64 + static_cast<std::uint32_t>(std::countr_zero(m));
        }

        std::uint32_t win_ = npos, base_ = 0, best_slot_ = 0, count_ = 0;
        sorted_side<Bid> over_;
    };
};

}  // namespace itchfh
