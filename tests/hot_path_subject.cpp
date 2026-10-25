// Subjects for check_hot_path.py (note 002, S4): applying an event, for every
// combination of D1 and D2 that is meant not to allocate (B and C of each; the
// A alternatives are the standard containers and allocate by design).
// Spec: docs/design/002-order-book.md
#include <itchfh/book.hpp>

#define ITCHFH_SUBJECT(R, L)                                                                                    \
    extern "C" [[gnu::noinline]] itchfh::applied itchfh_apply_##R##_##L(itchfh::book<itchfh::R, itchfh::L>& b, \
                                                                        const itchfh::event& e) {               \
        return b.apply(e);                                                                                      \
    }

ITCHFH_SUBJECT(ref_hash, levels_sorted)
ITCHFH_SUBJECT(ref_hash, levels_window)
ITCHFH_SUBJECT(ref_direct, levels_sorted)
ITCHFH_SUBJECT(ref_direct, levels_window)

int main() { return 0; }
