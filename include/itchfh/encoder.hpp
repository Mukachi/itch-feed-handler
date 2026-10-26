// Encoding a decoded TotalView-ITCH 5.0 message back to its wire bytes (note
// 006): the snapshot server writes its spin from state, in the feed's own
// formats. The decoder's field tables run the other way (decoder.hpp), so one
// table defines both directions; fields the decoder fills with defaults are
// not on the wire and are skipped. Tested by the day's every message decoded
// and encoded again, byte for byte.
// Design: docs/design/006-snapshots-replay.md
#pragma once

#include <itchfh/decoder.hpp>
#include <itchfh/messages.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace itchfh {

namespace detail {

template <layout::field F>
inline void put_one(const unsigned char* m, unsigned char* out) {
    if constexpr (F.k == layout::copy) {
        std::memcpy(out + F.in, m + F.out, F.len);
    } else if constexpr (F.k == layout::integer) {
        std::uint64_t v = 0;
        std::memcpy(&v, m + F.out, F.width);  // little-endian in the struct
        for (int i = F.len - 1; i >= 0; --i, v >>= 8) out[F.in + i] = static_cast<unsigned char>(v);
    }
}

template <const auto& Table, std::size_t... I>
inline void put_all(const unsigned char* m, unsigned char* out, std::index_sequence<I...>) {
    (put_one<Table[I]>(m, out), ...);
}

template <const auto& Table>
inline void put_fields(const message& m, unsigned char* out) {
    put_all<Table>(reinterpret_cast<const unsigned char*>(&m), out, std::make_index_sequence<std::size(Table)>{});
}

}  // namespace detail

// The message's wire bytes into `out` (at least its type's length); the
// length, or 0 for a type not defined.
inline std::size_t encode(const message& m, unsigned char* out) noexcept {
    using namespace layout;
    switch (m.type) {
        case 'S': detail::put_fields<S>(m, out); break;
        case 'R': detail::put_fields<R>(m, out); break;
        case 'H': detail::put_fields<H>(m, out); break;
        case 'Y': detail::put_fields<Y>(m, out); break;
        case 'L': detail::put_fields<L>(m, out); break;
        case 'V': detail::put_fields<V>(m, out); break;
        case 'W': detail::put_fields<W>(m, out); break;
        case 'K': detail::put_fields<K>(m, out); break;
        case 'J': detail::put_fields<J>(m, out); break;
        case 'h': detail::put_fields<h>(m, out); break;
        case 'A': detail::put_fields<A>(m, out); break;
        case 'F': detail::put_fields<F>(m, out); break;
        case 'E': detail::put_fields<E>(m, out); break;
        case 'C': detail::put_fields<Cx>(m, out); break;
        case 'X': detail::put_fields<X>(m, out); break;
        case 'D': detail::put_fields<D>(m, out); break;
        case 'U': detail::put_fields<U>(m, out); break;
        case 'P': detail::put_fields<P>(m, out); break;
        case 'Q': detail::put_fields<Q>(m, out); break;
        case 'B': detail::put_fields<B>(m, out); break;
        case 'I': detail::put_fields<Ix>(m, out); break;
        case 'N': detail::put_fields<N>(m, out); break;
        case 'O': detail::put_fields<O>(m, out); break;
        default: return 0;
    }
    return message_length(m.type);
}

}  // namespace itchfh
