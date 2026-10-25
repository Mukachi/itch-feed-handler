// Fixed tables for the fast book (note 002, D4): anonymous mappings, zeroed by
// the kernel, faulted in at construction (MAP_POPULATE) so no event takes a
// page fault, as the gateway's order book does (its note 008). `prefault`
// off: pages faulted in as used, for tests and small machines.
// Design: docs/design/002-order-book.md
#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>

#include <sys/mman.h>

namespace itchfh {

struct unmap {
    std::size_t bytes = 0;
    void operator()(void* p) const noexcept { ::munmap(p, bytes); }
};

template <class T>
using table = std::unique_ptr<T[], unmap>;

template <class T>
table<T> zeroed(std::size_t n, bool prefault) {
    static_assert(std::is_trivially_destructible_v<T> && std::is_trivially_copyable_v<T>);
    const std::size_t bytes = (n ? n : 1) * sizeof(T);
    void* p = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | (prefault ? MAP_POPULATE : 0), -1, 0);
    if (p == MAP_FAILED) throw std::bad_alloc();
    return table<T>(static_cast<T*>(p), unmap{bytes});
}

// The smallest power of two at least n (and at least 16).
inline std::size_t pow2_at_least(std::size_t n) {
    std::size_t s = 16;
    while (s < n) s <<= 1;
    return s;
}

}  // namespace itchfh
