// Subjects for check_hot_path.py (note 002, S4): applying an event, for every
// combination of D1 and D2 that is meant not to allocate (B and C of each; the
// A alternatives are the standard containers and allocate by design).
// Spec: docs/design/002-order-book.md
// And handling a decoded message (note 003, S5): the handler on the default
// book, with a sink that does nothing, so what is checked is the handler's own.
#include <itchfh/book.hpp>
#include <itchfh/handler.hpp>

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

int main() { return 0; }
