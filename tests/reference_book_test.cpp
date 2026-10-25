// Tests for include/itchfh/reference_book.hpp. Spec: docs/design/002-order-book.md
// (S2, S3): every event type applied by hand, with its effect on orders,
// levels, bests and queue order (D3); every bad event of D5 refused, counted
// and marking its book unreliable; the invariants after each step; D6's watch.
#include "check.hpp"
#include "invariants.hpp"

#include <itchfh/reference_book.hpp>

#include <string>
#include <vector>

namespace {

using namespace itchfh;
using itchfh::test::check;

event add(std::uint64_t ref, std::uint16_t loc, char sd, std::uint32_t shares, std::uint32_t price) {
    event e{};
    e.type = event_type::add;
    e.ref = ref;
    e.locate = loc;
    e.side = sd;
    e.shares = shares;
    e.price = price;
    return e;
}
event with_shares(event_type t, std::uint64_t ref, std::uint16_t loc, std::uint32_t shares, std::uint32_t price = 0) {
    event e{};
    e.type = t;
    e.ref = ref;
    e.locate = loc;
    e.shares = shares;
    e.price = price;
    return e;
}
event remove(std::uint64_t ref, std::uint16_t loc) {
    event e{};
    e.type = event_type::remove;
    e.ref = ref;
    e.locate = loc;
    return e;
}
event replace(std::uint64_t ref, std::uint64_t new_ref, std::uint16_t loc, std::uint32_t shares, std::uint32_t price) {
    event e{};
    e.type = event_type::replace;
    e.ref = ref;
    e.new_ref = new_ref;
    e.locate = loc;
    e.shares = shares;
    e.price = price;
    return e;
}
event state(std::uint16_t loc, char st) {
    event e{};
    e.type = event_type::trading_state;
    e.locate = loc;
    e.side = st;
    return e;
}

bool clean(const reference_book& b, std::uint16_t loc) { return itchfh::test::book_problem(b, loc).empty(); }

void events() {
    reference_book b;
    b.apply(add(1, 7, 'B', 100, 1000000));  // 100.0000
    b.apply(add(2, 7, 'B', 50, 1000000));
    b.apply(add(3, 7, 'B', 30, 990000));
    b.apply(add(4, 7, 'S', 200, 1010000));
    check(b.best(7, side::bid) == level_view{1000000, 150, 2} && b.best(7, side::ask) == level_view{1010000, 200, 1} &&
              b.levels(7, side::bid).size() == 2 && clean(b, 7),
          "add: orders join their levels; bests are the highest bid and the lowest offer");
    check(b.queue(7, side::bid, 1000000) == std::vector<std::uint64_t>{1, 2}, "D3: a level keeps its orders in time priority");

    const applied ex = b.apply(with_shares(event_type::executed, 1, 7, 40));
    check(ex.bad == bad_event::none && ex.changed == 1 && b.best(7, side::bid) == level_view{1000000, 110, 2} &&
              b.find(1)->shares == 60 && b.queue(7, side::bid, 1000000) == std::vector<std::uint64_t>{1, 2} && clean(b, 7),
          "executed: shares reduced, the order keeps its place");
    b.apply(with_shares(event_type::executed_price, 1, 7, 10, 995000));
    check(b.find(1) == order_view{7, side::bid, 50, 1000000} && clean(b, 7),
          "executed with price: shares reduced, the display price unchanged");
    b.apply(with_shares(event_type::cancel, 2, 7, 20));
    check(b.at(7, side::bid, 1000000) == level_view{1000000, 80, 2} && clean(b, 7), "cancel: part of the shares removed");
    b.apply(with_shares(event_type::executed, 1, 7, 50));
    check(!b.find(1) && b.queue(7, side::bid, 1000000) == std::vector<std::uint64_t>{2} && clean(b, 7),
          "executed to zero: the order leaves the book");
    b.apply(remove(2, 7));
    check(!b.find(2) && b.best(7, side::bid) == level_view{990000, 30, 1} && clean(b, 7),
          "delete: the order and its emptied level leave; the next level is best");

    b.apply(add(5, 7, 'S', 10, 1010000));
    const applied rp = b.apply(replace(4, 6, 7, 120, 1005000));
    check(rp.bad == bad_event::none && rp.changed == 2 && !b.find(4) && b.find(6) == order_view{7, side::ask, 120, 1005000} &&
              b.best(7, side::ask) == level_view{1005000, 120, 1} && b.at(7, side::ask, 1010000) == level_view{1010000, 10, 1} &&
              clean(b, 7),
          "replace: the old order removed, the new one at its new price, same side; both levels named as changed");
    b.apply(add(7, 7, 'S', 5, 1005000));
    b.apply(replace(6, 8, 7, 100, 1005000));
    check(b.queue(7, side::ask, 1005000) == std::vector<std::uint64_t>{7, 8}, "replace at the same price: to the back of the queue");

    b.apply(add(9, 8, 'B', 1, 500));
    check(b.best(8, side::bid) == level_view{500, 1, 1} && b.best(7, side::bid) == level_view{990000, 30, 1},
          "instruments are separate books");
    check(b.live_orders() == 5 && b.bad_count(bad_event::unknown_ref) == 0 && !b.unreliable(7), "nothing refused so far");
}

void bad_events() {
    reference_book b;
    b.apply(add(1, 3, 'B', 100, 1000));
    struct one {
        event e;
        bad_event want;
        const char* what;
    };
    const one cases[] = {
        {with_shares(event_type::executed, 99, 3, 1), bad_event::unknown_ref, "executed of an unknown order"},
        {with_shares(event_type::cancel, 99, 3, 1), bad_event::unknown_ref, "cancel of an unknown order"},
        {remove(99, 3), bad_event::unknown_ref, "delete of an unknown order"},
        {replace(99, 100, 3, 1, 1000), bad_event::unknown_ref, "replace of an unknown order"},
        {with_shares(event_type::executed, 1, 3, 101), bad_event::too_many_shares, "executed beyond the shares"},
        {with_shares(event_type::cancel, 1, 3, 101), bad_event::too_many_shares, "cancel beyond the shares"},
        {add(1, 3, 'S', 5, 1000), bad_event::duplicate_ref, "add reusing a live reference number"},
        {add(2, 3, 'X', 5, 1000), bad_event::malformed, "add with a side other than B or S"},
        {add(2, 3, 'B', 0, 1000), bad_event::malformed, "add of zero shares"},
    };
    for (const one& c : cases) {
        reference_book fresh;
        fresh.apply(add(1, 3, 'B', 100, 1000));
        const applied a = fresh.apply(c.e);
        check(a.bad == c.want && fresh.bad_count(c.want) == 1 && fresh.unreliable(3) && fresh.find(1) == order_view{3, side::bid, 100, 1000} &&
                  clean(fresh, 3),
              std::string("D5: ") + c.what + ": refused, counted, the book unreliable, nothing applied");
    }
    b.apply(add(2, 3, 'B', 5, 1000));
    const applied dup = b.apply(replace(1, 2, 3, 10, 1000));
    check(dup.bad == bad_event::duplicate_ref && b.find(1) && b.find(2), "D5: replace onto a live reference number refused");
    reference_book other;
    other.apply(add(1, 3, 'B', 100, 1000));
    other.apply(add(2, 4, 'B', 100, 1000));
    other.apply(with_shares(event_type::cancel, 2, 4, 500));
    check(other.unreliable(4) && !other.unreliable(3), "D5: only the instrument of the order concerned is marked unreliable");
}

void d6() {
    reference_book b;
    itchfh::test::d6_watch w;
    b.apply(state(1, 'T'));
    b.apply(add(1, 1, 'B', 10, 1000));
    check(w.after(b, 1).empty(), "D6: a normal book while trading passes");
    b.apply(add(2, 1, 'S', 10, 1000));
    check(!w.after(b, 1).empty(), "D6: a book locked while trading is caught");

    reference_book h;
    itchfh::test::d6_watch wh;
    h.apply(state(2, 'H'));
    h.apply(add(1, 2, 'B', 10, 1100));
    h.apply(add(2, 2, 'S', 10, 1000));
    const bool halted_ok = wh.after(h, 2).empty();
    h.apply(state(2, 'T'));
    h.apply(add(3, 2, 'B', 1, 900));
    check(halted_ok && wh.after(h, 2).empty(), "D6: crossed during a halt, and still crossed on resuming, is allowed");
    h.apply(remove(1, 2));
    const bool cleared = wh.after(h, 2).empty();
    h.apply(add(4, 2, 'B', 1, 1200));
    check(cleared && !wh.after(h, 2).empty(), "D6: once cleared while trading, crossing again is caught");
}

}  // namespace

int main() {
    events();
    bad_events();
    d6();
    return itchfh::test::result();
}
