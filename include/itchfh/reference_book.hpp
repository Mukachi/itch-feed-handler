// The reference book (note 002, and note 001's D3): every instrument's
// displayed book from TotalView-ITCH 5.0's order events, written for clarity,
// not speed: standard maps and lists, no pools, no fixed capacity. It is the
// oracle the fast book is compared against after every event, and the
// baseline it is measured beside. It keeps each level's orders in time
// priority (D3) and refuses the same bad events (D5).
// Design: docs/design/002-order-book.md
#pragma once

#include <itchfh/book_types.hpp>
#include <itchfh/event.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace itchfh {

class reference_book {
public:
    applied apply(const event& e) {
        applied a;
        a.locate = e.locate;
        switch (e.type) {
            case event_type::trading_state:
                state_[e.locate] = e.side;
                return a;
            case event_type::add:
                if ((e.side != 'B' && e.side != 'S') || e.shares == 0) return refuse(a, bad_event::malformed);
                if (orders_.contains(e.ref)) return refuse(a, bad_event::duplicate_ref);
                add(e.ref, e.locate, e.side == 'B' ? side::bid : side::ask, e.shares, e.price, a);
                return a;
            case event_type::executed:
            case event_type::executed_price:
            case event_type::cancel: {
                const auto it = orders_.find(e.ref);
                if (it == orders_.end()) return refuse(a, bad_event::unknown_ref);
                order& o = it->second;
                a.locate = o.locate;
                if (e.shares > o.shares) return refuse(a, bad_event::too_many_shares);
                level& l = book_of(o.locate, o.s).at(o.price);
                o.shares -= e.shares;
                l.shares -= e.shares;
                note(a, o.s, o.price);
                if (o.shares == 0) erase(it);
                return a;
            }
            case event_type::remove: {
                const auto it = orders_.find(e.ref);
                if (it == orders_.end()) return refuse(a, bad_event::unknown_ref);
                a.locate = it->second.locate;
                note(a, it->second.s, it->second.price);
                erase(it);
                return a;
            }
            case event_type::replace: {
                const auto it = orders_.find(e.ref);
                if (it == orders_.end()) return refuse(a, bad_event::unknown_ref);
                a.locate = it->second.locate;
                if (e.shares == 0) return refuse(a, bad_event::malformed);
                if (e.new_ref != e.ref && orders_.contains(e.new_ref)) return refuse(a, bad_event::duplicate_ref);
                const order old = it->second;
                note(a, old.s, old.price);
                erase(it);
                add(e.new_ref, old.locate, old.s, e.shares, e.price, a);
                return a;
            }
        }
        return refuse(a, bad_event::malformed);
    }

    std::optional<level_view> best(std::uint16_t locate, side s) const {
        const auto* m = find_book(locate, s);
        if (!m || m->empty()) return std::nullopt;
        const auto& [price, l] = *m->begin();
        return view(price, l);
    }
    // The level at a price; zero shares and orders if there is none.
    level_view at(std::uint16_t locate, side s, std::uint32_t price) const {
        const auto* m = find_book(locate, s);
        if (!m) return {price, 0, 0};
        const auto it = m->find(price);
        return it == m->end() ? level_view{price, 0, 0} : view(price, it->second);
    }
    // Every level of a side, best first.
    std::vector<level_view> levels(std::uint16_t locate, side s) const {
        std::vector<level_view> v;
        if (const auto* m = find_book(locate, s))
            for (const auto& [price, l] : *m) v.push_back(view(price, l));
        return v;
    }
    // A level's orders in time priority (D3): their reference numbers.
    std::vector<std::uint64_t> queue(std::uint16_t locate, side s, std::uint32_t price) const {
        const auto* m = find_book(locate, s);
        if (!m) return {};
        const auto it = m->find(price);
        if (it == m->end()) return {};
        return {it->second.queue.begin(), it->second.queue.end()};
    }

    std::optional<order_view> find(std::uint64_t ref) const {
        const auto it = orders_.find(ref);
        if (it == orders_.end()) return std::nullopt;
        return order_view{it->second.locate, it->second.s, it->second.shares, it->second.price};
    }

    // The instrument's trading state ('H', 'P', 'Q', 'T'); 0 before any
    // Stock Trading Action (treated as halted: TotalView-ITCH 5.0, 1.2.2).
    char trading_state(std::uint16_t locate) const {
        const auto it = state_.find(locate);
        return it == state_.end() ? 0 : it->second;
    }
    bool unreliable(std::uint16_t locate) const { return unreliable_.contains(locate); }
    std::uint64_t bad_count(bad_event k) const { return bad_[static_cast<int>(k)]; }
    std::size_t live_orders() const { return orders_.size(); }
    // Every instrument with an order or a level, for full comparisons.
    std::vector<std::uint16_t> instruments() const {
        std::vector<std::uint16_t> v;
        for (const auto& [locate, b] : books_)
            if (!b[0].empty() || !b[1].empty()) v.push_back(locate);
        return v;
    }

private:
    struct level {
        std::uint64_t shares = 0;
        std::list<std::uint64_t> queue;  // reference numbers, oldest first
    };
    // Bids best first (highest price), asks best first (lowest).
    using side_book = std::map<std::uint32_t, level, std::function<bool(std::uint32_t, std::uint32_t)>>;
    struct order {
        std::uint16_t locate;
        side s;
        std::uint32_t shares, price;
        std::list<std::uint64_t>::iterator place;
    };

    static level_view view(std::uint32_t price, const level& l) {
        return {price, l.shares, static_cast<std::uint32_t>(l.queue.size())};
    }
    side_book& book_of(std::uint16_t locate, side s) {
        auto it = books_.find(locate);
        if (it == books_.end())
            it = books_.emplace(locate, std::array<side_book, 2>{side_book(std::greater<std::uint32_t>()),
                                                                 side_book(std::less<std::uint32_t>())}).first;
        return it->second[static_cast<int>(s)];
    }
    const side_book* find_book(std::uint16_t locate, side s) const {
        const auto it = books_.find(locate);
        return it == books_.end() ? nullptr : &it->second[static_cast<int>(s)];
    }
    static void note(applied& a, side s, std::uint32_t price) {
        for (int i = 0; i < a.changed; ++i)
            if (a.level[i].s == s && a.level[i].price == price) return;
        a.level[a.changed++] = {s, price};
    }
    void add(std::uint64_t ref, std::uint16_t locate, side s, std::uint32_t shares, std::uint32_t price, applied& a) {
        level& l = book_of(locate, s)[price];
        l.queue.push_back(ref);
        l.shares += shares;
        orders_.emplace(ref, order{locate, s, shares, price, std::prev(l.queue.end())});
        note(a, s, price);
    }
    void erase(std::unordered_map<std::uint64_t, order>::iterator it) {
        const order& o = it->second;
        side_book& b = book_of(o.locate, o.s);
        const auto lv = b.find(o.price);
        lv->second.shares -= o.shares;
        lv->second.queue.erase(o.place);
        if (lv->second.queue.empty()) b.erase(lv);
        orders_.erase(it);
    }
    applied& refuse(applied& a, bad_event k) {
        a.bad = k;
        ++bad_[static_cast<int>(k)];
        unreliable_.insert(a.locate);
        return a;
    }

    std::unordered_map<std::uint64_t, order> orders_;
    std::map<std::uint16_t, std::array<side_book, 2>> books_;
    std::unordered_map<std::uint16_t, char> state_;
    std::unordered_set<std::uint16_t> unreliable_;
    std::array<std::uint64_t, bad_event_kinds> bad_{};
};

}  // namespace itchfh
