// The fast book's capacities (note 002, D4), fixed at construction. Defaults
// from the sample day's peaks (1,925,638 orders live at once; Stock Locate up
// to 8,915 of a 16-bit field; reference numbers to 494,466,152), with headroom.
// Design: docs/design/002-order-book.md
#pragma once

#include <cstddef>
#include <cstdint>

namespace itchfh {

struct book_config {
    std::size_t orders = 4'000'000;      // live at once
    std::size_t levels = 2'000'000;      // live at once, all books together
    // D1 B: open-addressing slots (0: twice `orders`, as a power of two).
    std::size_t ref_slots = 0;
    // D1 C: reference numbers [ref_base, ref_base + ref_span) by direct index;
    // others fall back to B. 2^29 covers the sample day's 494 million.
    std::uint64_t ref_base = 0;
    std::uint64_t ref_span = std::uint64_t{1} << 29;
    // D2 B: entries of the sorted-level arena (sides' arrays, doubling).
    std::size_t level_arena = std::size_t{1} << 24;
    // D2 C: each side's window of price keys, and how many windows there are.
    std::size_t price_window = 2048;
    std::size_t windows = 32'768;
    bool prefault = true;
};

}  // namespace itchfh
