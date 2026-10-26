// Subjects for check_hot_path.py (note 002, S4): applying an event, for every
// combination of D1 and D2 that is meant not to allocate (B and C of each; the
// A alternatives are the standard containers and allocate by design).
// Spec: docs/design/002-order-book.md
// And handling a decoded message (note 003, S5): the handler on the default
// book, with a sink that does nothing, so what is checked is the handler's own.
// And decoding a message (note 004, S3), every alternative; and a packet
// through the sequencer and its timer (note 005, S5), with an output that does
// nothing.
#include <itchfh/book.hpp>
#include <itchfh/decoder.hpp>
#include <itchfh/handler.hpp>
#include <itchfh/sequencer.hpp>

#define ITCHFH_SUBJECT(R, L)                                                                                    \
    extern "C" [[gnu::noinline]] itchfh::applied itchfh_apply_##R##_##L(itchfh::book<itchfh::R, itchfh::L>& b, \
                                                                        const itchfh::event& e) {               \
        return b.apply(e);                                                                                      \
    }

ITCHFH_SUBJECT(ref_hash, levels_sorted)
ITCHFH_SUBJECT(ref_hash, levels_window)
ITCHFH_SUBJECT(ref_direct, levels_sorted)
ITCHFH_SUBJECT(ref_direct, levels_window)

struct quiet_sink {
    void book(const itchfh::applied&) {}
    void trade(const itchfh::trade_report&) {}
    void trade_broken(const itchfh::trade_report&) {}
    void status(std::uint16_t, itchfh::status_field) {}
    void imbalance(std::uint16_t, const itchfh::imbalance&) {}
    void market(const itchfh::message&) {}
    void directory(std::uint16_t, const itchfh::stock_directory&) {}
};
extern "C" [[gnu::noinline]] void itchfh_handle_default(itchfh::handler<itchfh::default_book, quiet_sink>& h, const itchfh::message& m) {
    h.on(m);
}

#define ITCHFH_DECODER(f)                                                                                         \
    extern "C" [[gnu::noinline]] itchfh::decode_status itchfh_##f(const unsigned char* p, std::size_t n, itchfh::message& m) { \
        return itchfh::f(p, n, m);                                                                                    \
    }
ITCHFH_DECODER(decode_switch)
ITCHFH_DECODER(decode_table)
ITCHFH_DECODER(decode_simd)
ITCHFH_DECODER(decode_simd_unpadded)

struct quiet_feed {
    void deliver(std::uint64_t, const unsigned char*, std::size_t) {}
    void state(itchfh::feed_state, std::uint64_t) {}
    void send_request(const unsigned char*, std::size_t) {}
};
extern "C" [[gnu::noinline]] void itchfh_sequence_packet(itchfh::sequencer<quiet_feed>& s, const unsigned char* p, std::size_t n,
                                                         std::int64_t now) {
    s.on_packet(p, n, now);
}
extern "C" [[gnu::noinline]] void itchfh_sequence_tick(itchfh::sequencer<quiet_feed>& s, std::int64_t now) { s.on_tick(now); }

int main() { return 0; }
