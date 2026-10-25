// The book's invariants (note 002), checked through any book's public
// interface, so the same checks hold the reference book and the fast one:
// a level's shares are the sum of its orders' and its count their number;
// every order has shares, sits on its own instrument, side and price, and in
// exactly one level; levels are in price order, best first, none empty; the
// best is the first level; and D6: a book never becomes locked or crossed
// while its instrument is in state T.
#pragma once

#include <itchfh/book_types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace itchfh::test {

// What is wrong with one instrument's book, or empty if nothing.
template <class Book>
std::string book_problem(const Book& b, std::uint16_t locate, std::size_t* orders_seen = nullptr) {
    for (const side s : {side::bid, side::ask}) {
        const std::vector<level_view> lv = b.levels(locate, s);
        for (std::size_t i = 0; i < lv.size(); ++i) {
            const level_view& l = lv[i];
            if (l.orders == 0 || l.shares == 0) return "an empty level";
            if (i > 0 && (s == side::bid ? lv[i - 1].price <= l.price : lv[i - 1].price >= l.price))
                return "levels out of price order";
            const auto q = b.queue(locate, s, l.price);
            if (q.size() != l.orders) return "a level's order count is not its queue's length";
            std::uint64_t sum = 0;
            for (const std::uint64_t ref : q) {
                const auto o = b.find(ref);
                if (!o) return "a queued order that is not live";
                if (o->locate != locate || o->s != s || o->price != l.price) return "an order queued at another level";
                if (o->shares == 0) return "an order with no shares";
                sum += o->shares;
            }
            if (sum != l.shares) return "a level's shares are not its orders' sum";
            if (orders_seen) *orders_seen += q.size();
        }
        const auto best = b.best(locate, s);
        if (lv.empty() != !best || (best && !(*best == lv.front()))) return "the best is not the first level";
    }
    return {};
}

// One level's invariants (cheap enough for after every event): its count is
// its queue's length, its shares its orders' sum, each order on it; and the
// best of its side is the first level.
template <class Book>
std::string level_problem(const Book& b, std::uint16_t locate, side s, std::uint32_t price) {
    const level_view l = b.at(locate, s, price);
    const auto q = b.queue(locate, s, price);
    if (q.size() != l.orders) return "a level's order count is not its queue's length";
    std::uint64_t sum = 0;
    for (const std::uint64_t ref : q) {
        const auto o = b.find(ref);
        if (!o || o->locate != locate || o->s != s || o->price != price || o->shares == 0) return "an order not as queued";
        sum += o->shares;
    }
    if (sum != l.shares) return "a level's shares are not its orders' sum";
    const auto best = b.best(locate, s);
    if (best && l.orders > 0 && (s == side::bid ? best->price < price : best->price > price)) return "a level better than the best";
    return {};
}

// D6, followed event by event: an instrument's book may not go from neither
// locked nor crossed to either while its state is 'T'.
class d6_watch {
public:
    template <class Book>
    std::string after(const Book& b, std::uint16_t locate) {
        if (was_.size() <= locate) was_.resize(locate + 1u, false);
        const auto bid = b.best(locate, side::bid), ask = b.best(locate, side::ask);
        const bool touching = bid && ask && bid->price >= ask->price;
        const bool became = touching && !was_[locate];
        was_[locate] = touching;
        if (became && b.trading_state(locate) == 'T') return "became locked or crossed while trading (D6)";
        return {};
    }

private:
    std::vector<bool> was_;
};

}  // namespace itchfh::test
