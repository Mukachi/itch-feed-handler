// The fast book (note 002): every instrument's displayed book from
// TotalView-ITCH 5.0's order events, over one way of finding orders (D1:
// ref_tables.hpp) and one way of keeping levels (D2: levels.hpp). Orders and
// levels live in fixed pools allocated and pre-faulted at construction (D4);
// each level keeps its orders in time priority, a doubly linked list through
// the order records (D3); a bad event is refused, counted and marks its
// instrument unreliable (D5). Same answers as the reference book, through the
// same interface.
// Design: docs/design/002-order-book.md
#pragma once

#include <itchfh/book_config.hpp>
#include <itchfh/book_types.hpp>
#include <itchfh/event.hpp>
#include <itchfh/levels.hpp>
#include <itchfh/memory.hpp>
#include <itchfh/ref_tables.hpp>

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace itchfh {

template <class Refs, class Levels>
class book {
public:
    static constexpr std::size_t instruments_max = 65'536;  // Stock Locate is 16-bit

    explicit book(const book_config& c = {})
        : orders_(zeroed<order_rec>(c.orders, c.prefault)), levels_(zeroed<level_rec>(c.levels, c.prefault)), refs_(c),
          shared_(c), books_(std::make_unique<instrument[]>(instruments_max)) {
        for (std::size_t i = 0; i < c.orders; ++i) orders_[i].next = i + 1 < c.orders ? static_cast<std::uint32_t>(i + 1) : npos;
        for (std::size_t i = 0; i < c.levels; ++i) levels_[i].first = i + 1 < c.levels ? static_cast<std::uint32_t>(i + 1) : npos;
        free_order_ = c.orders ? 0 : npos;
        free_level_ = c.levels ? 0 : npos;
    }

    applied apply(const event& e) {
        applied a;
        a.locate = e.locate;
        switch (e.type) {
            case event_type::trading_state:
                state_[e.locate] = e.side;
                return a;
            case event_type::add: {
                if ((e.side != 'B' && e.side != 'S') || e.shares == 0) return refuse(a, bad_event::malformed);
                if (refs_.find(e.ref) != npos) return refuse(a, bad_event::duplicate_ref);
                const bad_event b = add(e.ref, e.locate, e.side == 'B' ? side::bid : side::ask, e.shares, e.price, a);
                return b == bad_event::none ? a : refuse(a, b);
            }
            case event_type::executed:
            case event_type::executed_price:
            case event_type::cancel: {
                const std::uint32_t o = refs_.find(e.ref);
                if (o == npos) return refuse(a, bad_event::unknown_ref);
                order_rec& r = orders_[o];
                a.locate = r.locate;
                if (e.shares > r.shares) return refuse(a, bad_event::too_many_shares);
                r.shares -= e.shares;
                levels_[r.level].shares -= e.shares;
                note(a, static_cast<side>(r.s), r.price);
                if (r.shares == 0) remove(o);
                return a;
            }
            case event_type::remove: {
                const std::uint32_t o = refs_.find(e.ref);
                if (o == npos) return refuse(a, bad_event::unknown_ref);
                a.locate = orders_[o].locate;
                note(a, static_cast<side>(orders_[o].s), orders_[o].price);
                remove(o);
                return a;
            }
            case event_type::replace: {
                const std::uint32_t o = refs_.find(e.ref);
                if (o == npos) return refuse(a, bad_event::unknown_ref);
                a.locate = orders_[o].locate;
                if (e.shares == 0) return refuse(a, bad_event::malformed);
                if (e.new_ref != e.ref && refs_.find(e.new_ref) != npos) return refuse(a, bad_event::duplicate_ref);
                const std::uint16_t locate = orders_[o].locate;
                const side s = static_cast<side>(orders_[o].s);
                note(a, s, orders_[o].price);
                remove(o);
                // Out of room for the new order: the old one is already gone,
                // so the book is short an order and marked unreliable (D4).
                const bad_event b = add(e.new_ref, locate, s, e.shares, e.price, a);
                return b == bad_event::none ? a : refuse(a, b);
            }
        }
        return refuse(a, bad_event::malformed);
    }

    // ---- what it holds, in the reference book's terms
    std::optional<level_view> best(std::uint16_t locate, side s) const {
        const std::uint32_t l = with(locate, s, [&](const auto& sd) { return sd.best(shared_); });
        if (l == npos) return std::nullopt;
        return view(l);
    }
    level_view at(std::uint16_t locate, side s, std::uint32_t price) const {
        const std::uint32_t l = with(locate, s, [&](const auto& sd) { return sd.find(shared_, price); });
        return l == npos ? level_view{price, 0, 0} : view(l);
    }
    std::vector<level_view> levels(std::uint16_t locate, side s) const {
        std::vector<level_view> v;
        with(locate, s, [&](const auto& sd) {
            sd.each(shared_, [&](std::uint32_t l) { v.push_back(view(l)); });
            return 0;
        });
        return v;
    }
    std::vector<std::uint64_t> queue(std::uint16_t locate, side s, std::uint32_t price) const {
        std::vector<std::uint64_t> q;
        const std::uint32_t l = with(locate, s, [&](const auto& sd) { return sd.find(shared_, price); });
        if (l == npos) return q;
        for (std::uint32_t o = levels_[l].first; o != npos; o = orders_[o].next) q.push_back(orders_[o].ref);
        return q;
    }
    std::optional<order_view> find(std::uint64_t ref) const {
        const std::uint32_t o = refs_.find(ref);
        if (o == npos) return std::nullopt;
        const order_rec& r = orders_[o];
        return order_view{r.locate, static_cast<side>(r.s), r.shares, r.price};
    }
    char trading_state(std::uint16_t locate) const { return state_[locate]; }
    bool unreliable(std::uint16_t locate) const { return unreliable_[locate]; }
    std::uint64_t bad_count(bad_event k) const { return bad_[static_cast<int>(k)]; }
    std::size_t live_orders() const { return live_orders_; }
    std::size_t live_levels() const { return live_levels_; }
    std::size_t peak_orders() const { return peak_orders_; }
    std::size_t peak_levels() const { return peak_levels_; }
    std::vector<std::uint16_t> instruments() const {
        std::vector<std::uint16_t> v;
        for (std::size_t i = 0; i < instruments_max; ++i)
            if (books_[i].bid.best(shared_) != npos || books_[i].ask.best(shared_) != npos) v.push_back(static_cast<std::uint16_t>(i));
        return v;
    }

private:
    struct order_rec {
        std::uint64_t ref;
        std::uint32_t shares, price;
        std::uint32_t level, prev, next;  // next: also the free list's link
        std::uint16_t locate;
        std::uint8_t s;
        std::uint8_t unused;
    };
    static_assert(sizeof(order_rec) == 32);
    struct level_rec {
        std::uint64_t shares;
        std::uint32_t price, count;
        std::uint32_t first, last;  // first: also the free list's link
    };
    struct instrument {
        typename Levels::template side<true> bid;
        typename Levels::template side<false> ask;
    };

    template <class F>
    auto with(std::uint16_t locate, side s, F&& f) const {
        return s == side::bid ? f(books_[locate].bid) : f(books_[locate].ask);
    }
    template <class F>
    auto with(std::uint16_t locate, side s, F&& f) {
        return s == side::bid ? f(books_[locate].bid) : f(books_[locate].ask);
    }
    level_view view(std::uint32_t l) const { return {levels_[l].price, levels_[l].shares, levels_[l].count}; }
    static void note(applied& a, side s, std::uint32_t price) {
        for (int i = 0; i < a.changed; ++i)
            if (a.level[i].s == s && a.level[i].price == price) return;
        a.level[a.changed++] = {s, price};
    }

    // A new order at the back of its level; nothing changed if it is refused.
    bad_event add(std::uint64_t ref, std::uint16_t locate, side s, std::uint32_t shares, std::uint32_t price, applied& a) {
        if (free_order_ == npos) return bad_event::full;
        const std::uint32_t o = free_order_;
        std::uint32_t l = with(locate, s, [&](auto& sd) { return sd.find(shared_, price); });
        const bool new_level = l == npos;
        if (new_level) {
            if (free_level_ == npos) return bad_event::full;
            l = free_level_;
            if (!with(locate, s, [&](auto& sd) { return sd.insert(shared_, price, l); })) return bad_event::full;
            free_level_ = levels_[l].first;
            levels_[l] = {0, price, 0, npos, npos};
        }
        if (!refs_.insert(ref, o)) {
            if (new_level) {
                with(locate, s, [&](auto& sd) {
                    sd.erase(shared_, price);
                    return 0;
                });
                levels_[l].first = free_level_;
                free_level_ = l;
            }
            return bad_event::full;
        }
        free_order_ = orders_[o].next;
        level_rec& lv = levels_[l];
        orders_[o] = {ref, shares, price, l, lv.last, npos, locate, static_cast<std::uint8_t>(s), 0};
        if (lv.last != npos) orders_[lv.last].next = o;
        else lv.first = o;
        lv.last = o;
        ++lv.count;
        lv.shares += shares;
        if (new_level && ++live_levels_ > peak_levels_) peak_levels_ = live_levels_;
        if (++live_orders_ > peak_orders_) peak_orders_ = live_orders_;
        note(a, s, price);
        return bad_event::none;
    }
    // An order out of its level (the level out of its side if emptied), its
    // number and its record freed.
    void remove(std::uint32_t o) {
        order_rec& r = orders_[o];
        level_rec& lv = levels_[r.level];
        if (r.prev != npos) orders_[r.prev].next = r.next;
        else lv.first = r.next;
        if (r.next != npos) orders_[r.next].prev = r.prev;
        else lv.last = r.prev;
        lv.shares -= r.shares;
        if (--lv.count == 0) {
            with(r.locate, static_cast<side>(r.s), [&](auto& sd) {
                sd.erase(shared_, r.price);
                return 0;
            });
            lv.first = free_level_;
            free_level_ = r.level;
            --live_levels_;
        }
        refs_.erase(r.ref);
        r.next = free_order_;
        free_order_ = o;
        --live_orders_;
    }
    applied& refuse(applied& a, bad_event k) {
        a.bad = k;
        ++bad_[static_cast<int>(k)];
        unreliable_[a.locate] = true;
        return a;
    }

    table<order_rec> orders_;
    table<level_rec> levels_;
    std::uint32_t free_order_ = npos, free_level_ = npos;
    Refs refs_;
    typename Levels::shared shared_;
    std::unique_ptr<instrument[]> books_;
    std::array<char, instruments_max> state_{};
    std::bitset<instruments_max> unreliable_;
    std::array<std::uint64_t, bad_event_kinds> bad_{};
    std::size_t live_orders_ = 0, live_levels_ = 0, peak_orders_ = 0, peak_levels_ = 0;
};

// The default (note 002, D1 and D2, chosen from the measured figures): the
// direct array, and sorted arrays of levels.
using default_book = book<ref_direct, levels_sorted>;

}  // namespace itchfh
