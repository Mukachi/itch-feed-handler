// The state publisher (note 008): the handler's sink (D1), on its thread,
// writing each change as a 64-byte update (D2) to every consumer's ring (D3)
// and the instrument's entry in the state table (D6), each with its
// reliability (D4). It never waits: a consumer whose ring is full is marked
// overrun and gets nothing more until it asks to join again; a consumer
// joining, or every consumer after a snapshot (D5), gets a reset for all
// instruments and then each instrument's book, best first, an instrument at a
// time once its ring has room for all of it (from tick(), a budget a call).
//   published<Book>: the handler with its publisher, as feed_one's handler:
//   on(sequence, message), feed(state, next), tick(now).
// Design: docs/design/008-publication.md
#pragma once

#include <itchfh/book.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/publication.hpp>

#include <lfipc/shm.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace itchfh {

template <class Book>
class publisher {
public:
    using ring = lfipc::shm_spsc<pub::update, pub::ring_capacity>;
    static constexpr std::uint64_t replay_budget = 4096;  // updates a tick, at most

    explicit publisher(const Book& b) : book_(&b), mirror_(std::make_unique<std::uint64_t[][7]>(pub::instruments)) {}

    // Creates the table and every consumer's ring; false, with `why`, if not.
    bool open(const std::string& name, std::string& why) {
        if (!table_.create(name, why)) return false;
        t_ = table_.layout();
        for (int i = 0; i < pub::max_consumers; ++i)
            if (!(rings_[i] = ring::create(pub::ring_name(name, i), pub::ring_type_id, why))) return false;
        return true;
    }
    // A rebuilt book (after a snapshot, note 006) in place of the old.
    void rebind(const Book& b) { book_ = &b; }
    // The handler whose instrument status (note 003's D5 marks) the reliability reads.
    template <class H>
    void watch(const H& h) {
        status_ = &h.statuses();
    }

    // ---- from the feed
    void begin(std::uint64_t sequence, std::uint64_t timestamp_ns) {
        seq_ = sequence;
        ts_ = timestamp_ns;
    }
    void feed(feed_state s, std::uint64_t next) {
        feed_ = s;
        if (t_) t_->header.feed.store(static_cast<std::uint64_t>(s) | next << 8, std::memory_order_release);
        pub::update u = head(pub::kind::feed, 0);
        u.sequence = next;
        u.feed = {s, next};
        push(u, false);
    }
    // Joins, replays, and once a second whether each consumer's process is running.
    void tick(std::int64_t now) {
        if (!t_) return;
        for (int c = 0; c < pub::max_consumers; ++c) {
            pub::consumer_slot& sl = t_->slots[c];
            const std::uint32_t st = sl.state.load(std::memory_order_acquire);
            if (st == pub::slot_join) start_replay(c);
            else if (st == pub::slot_free && local_[c] != pub::slot_free) drop(c);
            if (local_[c] == pub::slot_replaying) replay_step(c);
        }
        if (now - last_sweep_ >= 1'000'000'000) {
            last_sweep_ = now;
            for (int c = 0; c < pub::max_consumers; ++c)
                if (local_[c] != pub::slot_free && !pub::table_segment::alive(t_->slots[c].pid.load(std::memory_order_acquire))) {
                    t_->slots[c].state.store(pub::slot_free, std::memory_order_release);
                    drop(c);
                }
        }
    }
    // While a snapshot is applied (note 006 replays its spin through the
    // handler) nothing goes to the rings; the table still follows.
    void mute(bool on) { muted_ = on; }
    // D5: after a snapshot, every consumer gets the rebuilt books.
    void restored() {
        muted_ = false;
        for (int c = 0; c < pub::max_consumers; ++c)
            if (local_[c] == pub::slot_live || local_[c] == pub::slot_replaying) start_replay(c);
    }

    // ---- the handler's sink (note 003)
    void book(const applied& a) {
        const std::uint8_t r = reliability(a.locate);
        if (a.bad != bad_event::none) {
            if (r != last_rel_[a.locate]) {
                last_rel_[a.locate] = r;
                pub::update u = head(pub::kind::instrument, a.locate);
                push(u, true);
                write_table(a.locate);
            }
            return;
        }
        last_rel_[a.locate] = r;
        const auto bb = book_->best(a.locate, side::bid), ba = book_->best(a.locate, side::ask);
        for (int i = 0; i < a.changed; ++i) {
            pub::update u = head(pub::kind::level, a.locate);
            u.s = a.level[i].s;
            const level_view v = book_->at(a.locate, a.level[i].s, a.level[i].price);
            u.level = {v.price, v.orders, v.shares, bb ? bb->price : 0, ba ? ba->price : 0, bb ? bb->shares : 0, ba ? ba->shares : 0};
            push(u, true);
        }
        std::uint64_t* m = mirror_[a.locate];
        m[0] = (bb ? bb->price : 0) | static_cast<std::uint64_t>(bb ? bb->orders : 0) << 32;
        m[1] = bb ? bb->shares : 0;
        m[2] = (ba ? ba->price : 0) | static_cast<std::uint64_t>(ba ? ba->orders : 0) << 32;
        m[3] = ba ? ba->shares : 0;
        write_table(a.locate);
    }
    void trade(const trade_report& t) {
        pub::update u = head(pub::kind::trade, t.locate);
        u.trade = {t.price, t.kind, t.shares, t.match};
        push(u, true);
        std::uint64_t* m = mirror_[t.locate];
        m[4] = (m[4] & ~0xFFFF'FFFFull) | t.price;
        m[5] = t.shares;
        write_table(t.locate);
    }
    void trade_broken(const trade_report& t) {
        pub::update u = head(pub::kind::trade_broken, t.locate);
        u.trade = {t.price, t.kind, t.shares, t.match};
        push(u, true);
    }
    void status(std::uint16_t l, status_field f) {
        pub::update u = head(pub::kind::status, l);
        u.field = f;
        const instrument_status& st = status_->of(l);
        switch (f) {
            case status_field::trading_state:
                u.raw[0] = static_cast<unsigned char>(st.trading_state);
                std::memcpy(u.raw + 1, st.trading_reason.data(), 4);
                mirror_[l][4] = (mirror_[l][4] & ~(0xFFull << 32)) | static_cast<std::uint64_t>(static_cast<unsigned char>(st.trading_state)) << 32;
                break;
            case status_field::reg_sho: u.raw[0] = static_cast<unsigned char>(st.reg_sho); break;
            case status_field::operational_halt: std::memcpy(u.raw, st.operational_halt.data(), 3); break;
            case status_field::luld_collar: std::memcpy(u.raw, &st.luld, sizeof(st.luld)); break;
            case status_field::ipo_quoting: std::memcpy(u.raw, &st.ipo, sizeof(st.ipo)); break;
            case status_field::retail_interest: u.raw[0] = static_cast<unsigned char>(st.retail_interest); break;
            case status_field::direct_listing: std::memcpy(u.raw, &st.dlcr, sizeof(st.dlcr)); break;
            case status_field::participant: break;
        }
        push(u, true);
        last_rel_[l] = u.reliability;
        write_table(l);
    }
    void imbalance(std::uint16_t l, const itchfh::imbalance& x) {
        pub::update u = head(pub::kind::imbalance, l);
        u.noii = {x.paired_shares, x.imbalance_shares, x.far_price, x.near_price, x.reference_price, x.direction, x.cross_type, x.price_variation};
        push(u, true);
    }
    void market(const message& msg) {
        pub::update u = head(pub::kind::market, msg.locate);
        switch (msg.type) {
            case 'S': std::memcpy(u.raw, &msg.S, sizeof(msg.S)); break;
            case 'V': std::memcpy(u.raw, &msg.V, sizeof(msg.V)); break;
            case 'W': std::memcpy(u.raw, &msg.W, sizeof(msg.W)); break;
            default: break;
        }
        u.field = static_cast<status_field>(msg.type);  // the message type, for a market update
        push(u, true);
    }
    void directory(std::uint16_t l, const stock_directory& r) {
        pub::update u = head(pub::kind::directory, l);
        std::memcpy(u.raw, &r, sizeof(r));
        push(u, true);
    }

    std::uint64_t overruns() const { return overruns_; }
    // Updates sent to consumer `c` not yet read (for a test or benchmark to pace by; the publisher never waits).
    std::uint64_t behind(int c) const { return t_ ? pushed_[c] - t_->slots[c].consumed.load(std::memory_order_acquire) : 0; }
    std::uint32_t slot(int c) const { return local_[c]; }

private:
    std::uint8_t reliability(std::uint16_t l) const {
        return pub::reliability(feed_, book_->unreliable(l), status_ && status_->of(l).not_since_snapshot != 0);
    }
    pub::update head(pub::kind k, std::uint16_t l) const {
        pub::update u;
        std::memset(&u, 0, sizeof(u));
        u.k = k;
        u.locate = l;
        u.reliability = reliability(l);
        u.sequence = seq_;
        u.timestamp_ns = ts_;
        return u;
    }
    void write_table(std::uint16_t l) {
        if (!t_) return;
        std::uint64_t(&m)[7] = mirror_[l];
        m[4] = (m[4] & ~(0xFFull << 40)) | static_cast<std::uint64_t>(reliability(l)) << 40;
        m[6] = seq_;
        pub::write_entry(t_->entries[l], m);
    }
    // To every consumer that takes it; `message`: from an ITCH message (flags it
    // as continuing one already published).
    void push(pub::update& u, bool message) {
        if (muted_) return;
        if (message) {
            if (u.sequence == last_seq_ && have_last_) u.flags |= pub::continues;
            last_seq_ = u.sequence;
            have_last_ = true;
        }
        for (int i = 0; i < n_active_; ++i) {
            const int c = active_[i];
            if (local_[c] == pub::slot_replaying && u.k == pub::kind::level && u.locate >= cursor_[c]) continue;  // replayed later, as it then stands
            if (rings_[c]->ring().try_push(u)) ++pushed_[c];
            else overrun(c), --i;
        }
    }
    void overrun(int c) {
        ++overruns_;
        local_[c] = pub::slot_overrun;
        t_->slots[c].state.store(pub::slot_overrun, std::memory_order_release);
        deactivate(c);
    }
    void drop(int c) {
        local_[c] = pub::slot_free;
        deactivate(c);
    }
    void deactivate(int c) {
        for (int i = 0; i < n_active_; ++i)
            if (active_[i] == c) {
                active_[i] = active_[--n_active_];
                return;
            }
    }
    void start_replay(int c) {
        if (local_[c] != pub::slot_live && local_[c] != pub::slot_replaying) {
            // A join: the consumer has drained its ring; count from what it has read.
            pushed_[c] = t_->slots[c].consumed.load(std::memory_order_acquire);
            active_[n_active_++] = c;
        }
        local_[c] = pub::slot_replaying;
        t_->slots[c].state.store(pub::slot_replaying, std::memory_order_release);
        cursor_[c] = 0;
        pub::update u = head(pub::kind::reset, 0);
        u.flags = pub::all_instruments;
        if (rings_[c]->ring().try_push(u)) ++pushed_[c];
        else overrun(c);
    }
    void replay_step(int c) {
        std::uint64_t budget = replay_budget;
        const std::uint64_t consumed = t_->slots[c].consumed.load(std::memory_order_acquire);
        while (cursor_[c] < pub::instruments && budget > 0) {
            const auto l = static_cast<std::uint16_t>(cursor_[c]);
            std::uint64_t n = 0;
            for (const side s : {side::bid, side::ask}) book_->each_level(l, s, [&](const level_view&) { ++n; });
            if (n == 0) {
                ++cursor_[c];
                continue;
            }
            if (pub::ring_capacity - (pushed_[c] - consumed) < n + 64) return;  // room for the whole instrument, or wait
            const auto bb = book_->best(l, side::bid), ba = book_->best(l, side::ask);
            bool ok = true;
            for (const side s : {side::bid, side::ask})
                book_->each_level(l, s, [&](const level_view& v) {
                    pub::update u = head(pub::kind::level, l);
                    u.s = s;
                    u.level = {v.price, v.orders, v.shares, bb ? bb->price : 0, ba ? ba->price : 0, bb ? bb->shares : 0, ba ? ba->shares : 0};
                    if (ok && rings_[c]->ring().try_push(u)) ++pushed_[c];
                    else ok = false;
                });
            if (!ok) {
                overrun(c);
                return;
            }
            ++cursor_[c];
            budget -= n < budget ? n : budget;
        }
        if (cursor_[c] >= pub::instruments) {
            local_[c] = pub::slot_live;
            t_->slots[c].state.store(pub::slot_live, std::memory_order_release);
        }
    }

    const Book* book_;
    const market_status* status_ = nullptr;
    pub::table_segment table_;
    pub::table_layout* t_ = nullptr;
    std::unique_ptr<ring> rings_[pub::max_consumers];
    std::uint32_t local_[pub::max_consumers] = {};
    std::uint64_t pushed_[pub::max_consumers] = {}, cursor_[pub::max_consumers] = {};
    int active_[pub::max_consumers] = {}, n_active_ = 0;
    std::unique_ptr<std::uint64_t[][7]> mirror_;
    std::uint8_t last_rel_[pub::instruments] = {};
    feed_state feed_ = feed_state::waiting;
    std::uint64_t seq_ = 0, ts_ = 0, last_seq_ = 0, overruns_ = 0;
    bool have_last_ = false, muted_ = false;
    std::int64_t last_sweep_ = 0;
};

// The handler with its publisher: what feed_one runs when publishing.
template <class Book = default_book>
struct published {
    publisher<Book> pub;
    handler<Book, publisher<Book>> h;
    explicit published(Book& b, const handler_config& c = {}) : pub(b), h(b, pub, c) { pub.watch(h); }
    void on(std::uint64_t sequence, const message& m) {
        pub.begin(sequence, m.timestamp_ns);
        h.on(m);
    }
    void feed(feed_state s, std::uint64_t next) { pub.feed(s, next); }
    void tick(std::int64_t now) { pub.tick(now); }
};

}  // namespace itchfh
