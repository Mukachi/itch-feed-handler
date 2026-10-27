// A consumer of the publisher (note 008, D7), in any process: it claims a slot
// in the state table, drains its ring and asks to join; the publisher then
// sends a reset for every instrument and each instrument's book, best first,
// then everything as it happens. If it falls a ring behind it is marked
// overrun (overrun()), and rejoin() starts it again. read() gives an
// instrument's entry in the state table, never torn (D6's seqlock).
// Design: docs/design/008-publication.md
#pragma once

#include <itchfh/publication.hpp>

#include <lfipc/shm.hpp>

#include <array>
#include <map>
#include <memory>
#include <string>

#include <unistd.h>

namespace itchfh::pub {

class consumer {
public:
    using ring = lfipc::shm_spsc<update, ring_capacity>;

    consumer() = default;
    consumer(const consumer&) = delete;
    consumer& operator=(const consumer&) = delete;
    ~consumer() {
        if (slot_) slot_->state.store(slot_free, std::memory_order_release);
    }

    // False, with `why`, if there is no table or no free slot.
    bool attach(const std::string& name, std::string& why) {
        if (!table_.attach(name, why)) return false;
        table_layout* t = table_.layout();
        for (int i = 0; i < max_consumers; ++i) {
            std::uint32_t expected = slot_free;
            if (!t->slots[i].state.compare_exchange_strong(expected, slot_claimed, std::memory_order_acq_rel)) continue;
            if (!(ring_ = ring::attach(ring_name(name, i), ring_type_id, why))) {
                t->slots[i].state.store(slot_free, std::memory_order_release);
                return false;
            }
            slot_ = &t->slots[i];
            index_ = i;
            consumed_ = slot_->consumed.load(std::memory_order_relaxed);
            slot_->pid.store(static_cast<std::int32_t>(::getpid()), std::memory_order_release);
            join();
            return true;
        }
        why = name + ": every consumer slot is taken";
        return false;
    }
    // The next update, if one is waiting.
    bool poll(update& u) {
        if (!ring_->ring().try_pop(u)) return false;
        slot_->consumed.store(++consumed_, std::memory_order_release);
        return true;
    }
    slot_state state() const { return static_cast<slot_state>(slot_->state.load(std::memory_order_acquire)); }
    bool overrun() const { return state() == slot_overrun; }
    // After an overrun: what is left in the ring is dropped, and the books come again.
    void rejoin() { join(); }
    bool read(std::uint16_t locate, entry_view& v) const {
        std::uint64_t w[7];
        for (;;)
            if (try_read_entry(table_.layout()->entries[locate], w)) {
                v = unpack(w);
                return true;
            }
    }
    feed_state feed(std::uint64_t& next_expected) const {
        const std::uint64_t f = table_.layout()->header.feed.load(std::memory_order_acquire);
        next_expected = f >> 8;
        return static_cast<feed_state>(f & 0xFF);
    }
    int index() const { return index_; }

private:
    void join() {
        update u;
        while (poll(u)) {
        }
        slot_->state.store(slot_join, std::memory_order_release);
    }

    table_segment table_;
    std::unique_ptr<ring> ring_;
    consumer_slot* slot_ = nullptr;
    int index_ = -1;
    std::uint64_t consumed_ = 0;
};

// A consumer's books, kept from the updates (D7's example; it allocates: the
// consumer's side, not the publisher's), with counts of what else came and a
// check that sequence numbers never go back.
struct books {
    struct level {
        std::uint64_t shares;
        std::uint32_t orders;
    };
    std::map<std::uint16_t, std::array<std::map<std::uint32_t, level>, 2>> by_instrument;
    std::uint64_t updates = 0, levels = 0, trades = 0, broken = 0, statuses = 0, imbalances = 0, markets = 0, directories = 0,
                  feed_changes = 0, resets = 0, instrument_changes = 0, not_current = 0, catching_up = 0, last_sequence = 0;
    bool in_order = true;

    void apply(const update& u) {
        ++updates;
        if (u.k != kind::feed && u.k != kind::reset) {
            if (u.sequence < last_sequence) in_order = false;
            last_sequence = u.sequence;
            if (!pub::current(u.reliability)) ++not_current;
            if (pub::catching_up(u.reliability)) ++catching_up;
        }
        switch (u.k) {
            case kind::level: {
                ++levels;
                auto& m = by_instrument[u.locate][static_cast<int>(u.s)];
                if (u.level.orders == 0) m.erase(u.level.price);
                else m[u.level.price] = {u.level.shares, u.level.orders};
                break;
            }
            case kind::reset:
                ++resets;
                if (u.flags & all_instruments) by_instrument.clear();
                else by_instrument.erase(u.locate);
                break;
            case kind::trade: ++trades; break;
            case kind::trade_broken: ++broken; break;
            case kind::status: ++statuses; break;
            case kind::imbalance: ++imbalances; break;
            case kind::market: ++markets; break;
            case kind::directory: ++directories; break;
            case kind::feed: ++feed_changes; break;
            case kind::instrument: ++instrument_changes; break;
        }
    }
    // Every level, instrument by instrument, bids best (highest) first then
    // asks best (lowest) first: (locate, side, price, shares, orders), hashed.
    std::uint64_t digest() const {
        std::uint64_t h = 1469598103934665603ULL;
        const auto mix = [&](std::uint64_t v) {
            for (int i = 0; i < 8; ++i) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 1099511628211ULL;
        };
        for (const auto& [l, sides] : by_instrument)
            for (int s = 0; s < 2; ++s) {
                const auto row = [&](std::uint32_t price, const level& v) {
                    mix(l);
                    mix(static_cast<std::uint64_t>(s));
                    mix(price);
                    mix(v.shares);
                    mix(v.orders);
                };
                if (s == 0)
                    for (auto it = sides[0].rbegin(); it != sides[0].rend(); ++it) row(it->first, it->second);
                else
                    for (const auto& [price, v] : sides[1]) row(price, v);
            }
        return h;
    }
    // The best of a side, as kept: price and shares (0 and 0 if none).
    std::pair<std::uint32_t, std::uint64_t> best(std::uint16_t l, side s) const {
        const auto it = by_instrument.find(l);
        if (it == by_instrument.end() || it->second[static_cast<int>(s)].empty()) return {0, 0};
        const auto& m = it->second[static_cast<int>(s)];
        const auto& e = s == side::bid ? *m.rbegin() : *m.begin();
        return {e.first, e.second.shares};
    }
};

// The same digest from a book (the publisher's side of the comparison).
template <class Book>
std::uint64_t digest_of(const Book& b) {
    std::uint64_t h = 1469598103934665603ULL;
    const auto mix = [&](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) h = (h ^ ((v >> (8 * i)) & 0xFF)) * 1099511628211ULL;
    };
    for (const std::uint16_t l : b.instruments())
        for (const side s : {side::bid, side::ask})
            b.each_level(l, s, [&](const level_view& v) {
                mix(l);
                mix(static_cast<std::uint64_t>(s));
                mix(v.price);
                mix(v.shares);
                mix(v.orders);
            });
    return h;
}

}  // namespace itchfh::pub
