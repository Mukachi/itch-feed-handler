// Snapshots (note 006): the snapshot server, which follows the feed and writes
// a GLIMPSE 5.0 spin over SoupBinTCP at any point between two messages, and the
// client's rebuild from one.
//   D1  the server keeps the reference handler and reference book, and each
//       order's attribution (the MPID of its F, carried across a Replace)
//   D2  a spin is of the state after exactly the messages before N, N the
//       server's next expected sequence number, written in one go
//   D3  System Event; every Stock Directory; each instrument's Trading Action,
//       Reg SHO, retail interest, operational halts; every displayed order,
//       instrument by instrument, each side best first, each level in time
//       priority; End of Snapshot (N, 20 ASCII digits); End of Session
//   D4  the client feeds the spin through the same decoder and handler as the
//       live feed (B, `decode`: note 011's D6; it was C), into a fresh book
//       and handler, and learns N
//   D5  the rebuilt handler marks what GLIMPSE could not restore
// Design: docs/design/006-snapshots-replay.md
#pragma once

#include <itchfh/decoder.hpp>
#include <itchfh/encoder.hpp>
#include <itchfh/handler_types.hpp>
#include <itchfh/messages.hpp>
#include <itchfh/reference_book.hpp>
#include <itchfh/reference_handler.hpp>
#include <itchfh/soup.hpp>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace itchfh {

class snapshot_server {
public:
    struct quiet {
        void book(const applied&) {}
        void trade(const trade_report&) {}
        void trade_broken(const trade_report&) {}
        void status(std::uint16_t, status_field) {}
        void imbalance(std::uint16_t, const itchfh::imbalance&) {}
        void market(const message&) {}
        void directory(std::uint16_t, const stock_directory&) {}
    };

    // The feed, in sequence (the server sits behind a sequencer of its own).
    void on(const message& m) {
        if (m.type == 'F') attribution_[m.A.ref] = m.A.attribution;
        if (m.type == 'U') {
            const auto it = attribution_.find(m.U.old_ref);
            if (it != attribution_.end()) attribution_[m.U.new_ref] = it->second;
        }
        handler_.on(m);
        ++next_;
    }
    // A message that could not be decoded: counted past (note 011's service follows a store).
    void skip() { ++next_; }
    std::uint64_t next() const { return next_; }
    const reference_book& book() const { return book_; }
    const reference_handler<reference_book, quiet>& handler() const { return handler_; }

    // A spin of the state now, as the server's side of a SoupBinTCP stream:
    // Login Accepted (session, next number 1), each message as Sequenced
    // Data, End of Snapshot, End of Session. `ts` stamps every message.
    std::vector<unsigned char> spin(const soup::session_id& session, std::uint64_t ts) const {
        std::vector<unsigned char> out;
        out.reserve(64u << 20);
        soup::login_accepted(out, session, 1);
        unsigned char buf[64];
        const auto send = [&](const message& m) { soup::sequenced(out, buf, encode(m, buf)); };
        const auto base = [&](char type, std::uint16_t locate) {
            message m{};
            m.type = type;
            m.locate = locate;
            m.timestamp_ns = ts;
            return m;
        };
        if (const char phase = handler_.market().phase) {
            message m = base('S', 0);
            m.S.event_code = phase;
            send(m);
        }
        std::vector<std::uint16_t> listed;
        for (std::uint32_t l = 0; l < 65'536; ++l)
            if (const stock_directory* e = handler_.entry(static_cast<std::uint16_t>(l))) {
                message m = base('R', static_cast<std::uint16_t>(l));
                m.R = *e;
                send(m);
                listed.push_back(static_cast<std::uint16_t>(l));
            }
        for (const std::uint16_t l : listed) {
            const symbol& stock = handler_.entry(l)->stock;
            const instrument_status st = handler_.status_of(l);
            if (st.trading_state) {
                message m = base('H', l);
                m.H = {stock, st.trading_state, ' ', st.trading_reason};
                send(m);
            }
            if (st.reg_sho) {
                message m = base('Y', l);
                m.Y = {stock, st.reg_sho};
                send(m);
            }
            if (st.retail_interest) {
                message m = base('N', l);
                m.N = {stock, st.retail_interest};
                send(m);
            }
            const char codes[3] = {'Q', 'B', 'X'};
            for (int i = 0; i < 3; ++i)
                if (st.operational_halt[i]) {
                    message m = base('h', l);
                    m.h = {stock, codes[i], st.operational_halt[i]};
                    send(m);
                }
        }
        for (const std::uint16_t l : book_.instruments()) {
            const stock_directory* e = handler_.entry(l);
            const symbol stock = e ? e->stock : symbol{' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
            for (const side s : {side::bid, side::ask})
                for (const level_view& lv : book_.levels(l, s))
                    for (const std::uint64_t ref : book_.queue(l, s, lv.price)) {
                        const auto o = book_.find(ref);
                        const auto a = attribution_.find(ref);
                        message m = base(a == attribution_.end() ? 'A' : 'F', l);
                        m.A = {ref, s == side::bid ? 'B' : 'S', o->shares, stock, o->price,
                               a == attribution_.end() ? mpid{' ', ' ', ' ', ' '} : a->second};
                        send(m);
                    }
        }
        soup::end_of_snapshot(buf, next_);
        soup::sequenced(out, buf, soup::end_of_snapshot_size);
        soup::end_of_session(out);
        return out;
    }

private:
    reference_book book_;
    quiet sink_;
    reference_handler<reference_book, quiet> handler_{book_, sink_};
    std::unordered_map<std::uint64_t, mpid> attribution_;
    std::uint64_t next_ = 1;
};

// The client's rebuild (D4): the spin's bytes, in any pieces, into `h` (a
// fresh handler on a fresh book), through the default decoder. Once End of
// Snapshot has come, `resume()` gives the number to resume the feed from and
// the handler is marked restored (D5). A Login Rejected is `rejected()`.
// restart() begins another spin, into another handler, keeping the reader's
// room (note 011: the program keeps one client across recoveries).
template <class Handler>
class snapshot_client {
public:
    explicit snapshot_client(Handler& h) : h_(&h) {}

    void restart(Handler& h) {
        h_ = &h;
        reader_.reset();
        logged_in_ = rejected_ = ended_ = false;
        resume_.reset();
        messages_ = undecoded_ = 0;
    }

    void feed(const unsigned char* p, std::size_t n) {
        reader_.feed(p, n, [&](char type, const unsigned char* body, std::size_t len) {
            if (type == 'A') {
                logged_in_ = len == 30;
            } else if (type == 'J') {
                rejected_ = true;
            } else if (type == 'S') {
                std::uint64_t next;
                if (soup::read_end_of_snapshot(body, len, next)) {
                    resume_ = next;
                    h_->mark_restored();
                    return;
                }
                message m;
                if (decode(body, len, m) == decode_status::ok) {
                    h_->on(m);
                    ++messages_;
                } else {
                    ++undecoded_;
                }
            } else if (type == 'Z') {
                ended_ = true;
            }
        });
    }
    bool logged_in() const { return logged_in_; }
    bool rejected() const { return rejected_; }
    // Undecodable messages or SoupBinTCP packets: the spin is not to be trusted.
    bool malformed() const { return undecoded_ > 0 || reader_.malformed() > 0; }
    std::optional<std::uint64_t> resume() const { return resume_; }
    bool ended() const { return ended_; }
    std::uint64_t messages() const { return messages_; }
    std::uint64_t undecoded() const { return undecoded_; }

private:
    Handler* h_;
    soup::reader reader_;
    bool logged_in_ = false, rejected_ = false, ended_ = false;
    std::optional<std::uint64_t> resume_;
    std::uint64_t messages_ = 0, undecoded_ = 0;
};

}  // namespace itchfh
