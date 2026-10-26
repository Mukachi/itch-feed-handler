// The ITCH decoder (note 004): one TotalView-ITCH 5.0 message's bytes into
// its decoded form (messages.hpp), three ways (D1), all from one table of each
// type's fields (wire offset and length, place in the struct, how to move it):
//   A  decode_switch  a switch on the type; per field one load of its width
//                     and a byte swap, the table unrolled by the compiler
//   B  decode_table   the same per-type functions, chosen through a table of
//                     256 function pointers indexed by the type byte
//   C  decode_simd    the message's 16-byte chunks shuffled (pshufb) into the
//                     struct's four 16-byte lanes by masks built from the
//                     table, OR-ed with a lane of constants; x86-64-v3, else A
// A and B never read past the message (the 6-byte timestamp is the low part
// of an 8-byte load starting in the header). C reads up to 15 bytes past it:
// decode_simd needs the padded-input contract (D2: 32 readable bytes after
// the message); decode_simd_unpadded copies the message into a local buffer
// first. Checked: the type and the length only (D3), refused as the reference
// decoder refuses. Fields other than the type's are left as they were, except
// C, which writes the whole struct.
// Design: docs/design/004-decoder.md
#pragma once

#include <itchfh/messages.hpp>
#include <itchfh/reference_decoder.hpp>  // decode_status

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#if defined(__SSSE3__)
#include <immintrin.h>
#endif

namespace itchfh {

namespace layout {

enum kind : std::uint8_t { copy, integer, fill };

// One field: `len` wire bytes at `in` to the struct at `out`; an integer is
// big-endian on the wire, `width` bytes in the struct (a 6-byte one into 8);
// `fill` writes `value` into `len` bytes (defaults the reference decoder sets).
struct field {
    std::uint8_t in, len, out, width;
    kind k;
    std::uint8_t value = 0;
};

#define ITCHFH_AT(member) static_cast<std::uint8_t>(offsetof(message, member))
constexpr field C(std::uint8_t in, std::uint8_t len, std::uint8_t out) { return {in, len, out, len, copy}; }
constexpr field I(std::uint8_t in, std::uint8_t len, std::uint8_t out, std::uint8_t width) { return {in, len, out, width, integer}; }
constexpr field Z(std::uint8_t len, std::uint8_t out, std::uint8_t v) { return {0, len, out, len, fill, v}; }

// The header every message carries: type, Stock Locate, tracking number, timestamp.
#define ITCHFH_HEADER C(0, 1, ITCHFH_AT(type)), I(1, 2, ITCHFH_AT(locate), 2), I(3, 2, ITCHFH_AT(tracking), 2), I(5, 6, ITCHFH_AT(timestamp_ns), 8)

inline constexpr field S[] = {ITCHFH_HEADER, C(11, 1, ITCHFH_AT(S.event_code))};
inline constexpr field R[] = {ITCHFH_HEADER, C(11, 8, ITCHFH_AT(R.stock)), C(19, 1, ITCHFH_AT(R.market_category)),
                              C(20, 1, ITCHFH_AT(R.financial_status)), I(21, 4, ITCHFH_AT(R.round_lot_size), 4),
                              C(25, 1, ITCHFH_AT(R.round_lots_only)), C(26, 1, ITCHFH_AT(R.issue_classification)),
                              C(27, 2, ITCHFH_AT(R.issue_sub_type)), C(29, 1, ITCHFH_AT(R.authenticity)),
                              C(30, 1, ITCHFH_AT(R.short_sale_threshold)), C(31, 1, ITCHFH_AT(R.ipo_flag)),
                              C(32, 1, ITCHFH_AT(R.luld_tier)), C(33, 1, ITCHFH_AT(R.etp_flag)),
                              I(34, 4, ITCHFH_AT(R.etp_leverage_factor), 4), C(38, 1, ITCHFH_AT(R.inverse_indicator))};
inline constexpr field H[] = {ITCHFH_HEADER, C(11, 8, ITCHFH_AT(H.stock)), C(19, 1, ITCHFH_AT(H.trading_state)),
                              C(20, 1, ITCHFH_AT(H.reserved)), C(21, 4, ITCHFH_AT(H.reason))};
inline constexpr field Y[] = {ITCHFH_HEADER, C(11, 8, ITCHFH_AT(Y.stock)), C(19, 1, ITCHFH_AT(Y.action))};
inline constexpr field L[] = {ITCHFH_HEADER, C(11, 4, ITCHFH_AT(L.participant)), C(15, 8, ITCHFH_AT(L.stock)),
                              C(23, 1, ITCHFH_AT(L.primary_market_maker)), C(24, 1, ITCHFH_AT(L.market_maker_mode)),
                              C(25, 1, ITCHFH_AT(L.participant_state))};
inline constexpr field V[] = {ITCHFH_HEADER, I(11, 8, ITCHFH_AT(V.level1), 8), I(19, 8, ITCHFH_AT(V.level2), 8),
                              I(27, 8, ITCHFH_AT(V.level3), 8)};
inline constexpr field W[] = {ITCHFH_HEADER, C(11, 1, ITCHFH_AT(W.breached_level))};
inline constexpr field K[] = {ITCHFH_HEADER, C(11, 8, ITCHFH_AT(K.stock)), I(19, 4, ITCHFH_AT(K.release_time_s), 4),
                              C(23, 1, ITCHFH_AT(K.release_qualifier)), I(24, 4, ITCHFH_AT(K.ipo_price), 4)};
inline constexpr field J[] = {ITCHFH_HEADER, C(11, 8, ITCHFH_AT(J.stock)), I(19, 4, ITCHFH_AT(J.reference_price), 4),
                              I(23, 4, ITCHFH_AT(J.upper_price), 4), I(27, 4, ITCHFH_AT(J.lower_price), 4),
                              I(31, 4, ITCHFH_AT(J.extension), 4)};
inline constexpr field h[] = {ITCHFH_HEADER, C(11, 8, ITCHFH_AT(h.stock)), C(19, 1, ITCHFH_AT(h.market_code)), C(20, 1, ITCHFH_AT(h.action))};
inline constexpr field A[] = {ITCHFH_HEADER, I(11, 8, ITCHFH_AT(A.ref), 8), C(19, 1, ITCHFH_AT(A.side)), I(20, 4, ITCHFH_AT(A.shares), 4),
                              C(24, 8, ITCHFH_AT(A.stock)), I(32, 4, ITCHFH_AT(A.price), 4), Z(4, ITCHFH_AT(A.attribution), ' ')};
inline constexpr field F[] = {ITCHFH_HEADER, I(11, 8, ITCHFH_AT(A.ref), 8), C(19, 1, ITCHFH_AT(A.side)), I(20, 4, ITCHFH_AT(A.shares), 4),
                              C(24, 8, ITCHFH_AT(A.stock)), I(32, 4, ITCHFH_AT(A.price), 4), C(36, 4, ITCHFH_AT(A.attribution))};
inline constexpr field E[] = {ITCHFH_HEADER, I(11, 8, ITCHFH_AT(E.ref), 8), I(19, 4, ITCHFH_AT(E.shares), 4), I(23, 8, ITCHFH_AT(E.match), 8),
                              Z(1, ITCHFH_AT(E.printable), 0), Z(4, ITCHFH_AT(E.price), 0)};
inline constexpr field Cx[] = {ITCHFH_HEADER, I(11, 8, ITCHFH_AT(E.ref), 8), I(19, 4, ITCHFH_AT(E.shares), 4), I(23, 8, ITCHFH_AT(E.match), 8),
                               C(31, 1, ITCHFH_AT(E.printable)), I(32, 4, ITCHFH_AT(E.price), 4)};
inline constexpr field X[] = {ITCHFH_HEADER, I(11, 8, ITCHFH_AT(X.ref), 8), I(19, 4, ITCHFH_AT(X.shares), 4)};
inline constexpr field D[] = {ITCHFH_HEADER, I(11, 8, ITCHFH_AT(D.ref), 8)};
inline constexpr field U[] = {ITCHFH_HEADER, I(11, 8, ITCHFH_AT(U.old_ref), 8), I(19, 8, ITCHFH_AT(U.new_ref), 8),
                              I(27, 4, ITCHFH_AT(U.shares), 4), I(31, 4, ITCHFH_AT(U.price), 4)};
inline constexpr field P[] = {ITCHFH_HEADER, I(11, 8, ITCHFH_AT(P.ref), 8), C(19, 1, ITCHFH_AT(P.side)), I(20, 4, ITCHFH_AT(P.shares), 4),
                              C(24, 8, ITCHFH_AT(P.stock)), I(32, 4, ITCHFH_AT(P.price), 4), I(36, 8, ITCHFH_AT(P.match), 8)};
inline constexpr field Q[] = {ITCHFH_HEADER, I(11, 8, ITCHFH_AT(Q.shares), 8), C(19, 8, ITCHFH_AT(Q.stock)), I(27, 4, ITCHFH_AT(Q.price), 4),
                              I(31, 8, ITCHFH_AT(Q.match), 8), C(39, 1, ITCHFH_AT(Q.cross_type))};
inline constexpr field B[] = {ITCHFH_HEADER, I(11, 8, ITCHFH_AT(B.match), 8)};
inline constexpr field Ix[] = {ITCHFH_HEADER, I(11, 8, ITCHFH_AT(I.paired_shares), 8), I(19, 8, ITCHFH_AT(I.imbalance_shares), 8),
                               C(27, 1, ITCHFH_AT(I.direction)), C(28, 8, ITCHFH_AT(I.stock)), I(36, 4, ITCHFH_AT(I.far_price), 4),
                               I(40, 4, ITCHFH_AT(I.near_price), 4), I(44, 4, ITCHFH_AT(I.reference_price), 4),
                               C(48, 1, ITCHFH_AT(I.cross_type)), C(49, 1, ITCHFH_AT(I.price_variation))};
inline constexpr field N[] = {ITCHFH_HEADER, C(11, 8, ITCHFH_AT(N.stock)), C(19, 1, ITCHFH_AT(N.interest_flag))};
inline constexpr field O[] = {ITCHFH_HEADER, C(11, 8, ITCHFH_AT(O.stock)), C(19, 1, ITCHFH_AT(O.open_eligibility)),
                              I(20, 4, ITCHFH_AT(O.min_price), 4), I(24, 4, ITCHFH_AT(O.max_price), 4), I(28, 4, ITCHFH_AT(O.near_price), 4),
                              I(32, 8, ITCHFH_AT(O.near_time), 8), I(40, 4, ITCHFH_AT(O.lower_collar), 4), I(44, 4, ITCHFH_AT(O.upper_collar), 4)};
#undef ITCHFH_HEADER
#undef ITCHFH_AT

// Every type the specification defines, its table.
template <class Fn>
constexpr void for_each_type(Fn&& f) {
    f('S', S), f('R', R), f('H', H), f('Y', Y), f('L', L), f('V', V), f('W', W), f('K', K), f('J', J), f('h', h), f('A', A), f('F', F),
        f('E', E), f('C', Cx), f('X', X), f('D', D), f('U', U), f('P', P), f('Q', Q), f('B', B), f('I', Ix), f('N', N), f('O', O);
}

// The wire length of each type byte; 0 for a type not defined.
inline constexpr auto lengths = [] {
    std::array<std::uint8_t, 256> t{};
    for (int c = 0; c < 256; ++c) t[c] = static_cast<std::uint8_t>(message_length(static_cast<char>(c)));
    return t;
}();

}  // namespace layout

namespace detail {

inline std::uint16_t swap(std::uint16_t v) { return __builtin_bswap16(v); }
inline std::uint32_t swap(std::uint32_t v) { return __builtin_bswap32(v); }
inline std::uint64_t swap(std::uint64_t v) { return __builtin_bswap64(v); }

template <class T>
inline T load(const unsigned char* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

// One field, the table entry a constant: the compiler emits its load, swap and store.
template <layout::field F>
inline void one(const unsigned char* p, unsigned char* out) {
    if constexpr (F.k == layout::copy) {
        std::memcpy(out + F.out, p + F.in, F.len);
    } else if constexpr (F.k == layout::fill) {
        std::memset(out + F.out, F.value, F.len);
    } else if constexpr (F.len == 2) {
        const std::uint16_t v = swap(load<std::uint16_t>(p + F.in));
        std::memcpy(out + F.out, &v, 2);
    } else if constexpr (F.len == 4) {
        const std::uint32_t v = swap(load<std::uint32_t>(p + F.in));
        std::memcpy(out + F.out, &v, 4);
    } else if constexpr (F.len == 8) {
        const std::uint64_t v = swap(load<std::uint64_t>(p + F.in));
        std::memcpy(out + F.out, &v, 8);
    } else {
        static_assert(F.len == 6 && F.in >= 2, "a 6-byte integer is read as the low part of an 8-byte load ending at its end");
        const std::uint64_t v = swap(load<std::uint64_t>(p + F.in - 2)) & 0xFFFF'FFFF'FFFFull;
        std::memcpy(out + F.out, &v, 8);
    }
}

template <const auto& Table, std::size_t... I>
inline void all(const unsigned char* p, unsigned char* out, std::index_sequence<I...>) {
    (one<Table[I]>(p, out), ...);
}

template <const auto& Table>
inline void fields(const unsigned char* p, message& m) {
    all<Table>(p, reinterpret_cast<unsigned char*>(&m), std::make_index_sequence<std::size(Table)>{});
}

}  // namespace detail

// A: a switch on the type.
inline decode_status decode_switch(const unsigned char* p, std::size_t n, message& m) noexcept {
    if (n == 0) return decode_status::wrong_length;
    const std::uint8_t want = layout::lengths[p[0]];
    if (want == 0) return decode_status::unknown_type;
    if (n != want) return decode_status::wrong_length;
    using namespace layout;
    switch (p[0]) {
        case 'S': detail::fields<S>(p, m); break;
        case 'R': detail::fields<R>(p, m); break;
        case 'H': detail::fields<H>(p, m); break;
        case 'Y': detail::fields<Y>(p, m); break;
        case 'L': detail::fields<L>(p, m); break;
        case 'V': detail::fields<V>(p, m); break;
        case 'W': detail::fields<W>(p, m); break;
        case 'K': detail::fields<K>(p, m); break;
        case 'J': detail::fields<J>(p, m); break;
        case 'h': detail::fields<h>(p, m); break;
        case 'A': detail::fields<A>(p, m); break;
        case 'F': detail::fields<F>(p, m); break;
        case 'E': detail::fields<E>(p, m); break;
        case 'C': detail::fields<Cx>(p, m); break;
        case 'X': detail::fields<X>(p, m); break;
        case 'D': detail::fields<D>(p, m); break;
        case 'U': detail::fields<U>(p, m); break;
        case 'P': detail::fields<P>(p, m); break;
        case 'Q': detail::fields<Q>(p, m); break;
        case 'B': detail::fields<B>(p, m); break;
        case 'I': detail::fields<Ix>(p, m); break;
        case 'N': detail::fields<N>(p, m); break;
        case 'O': detail::fields<O>(p, m); break;
    }
    return decode_status::ok;
}

// B: a table of 256 function pointers, indexed by the type byte.
namespace detail {
using decode_fn = void (*)(const unsigned char*, message&);
inline constexpr auto functions = [] {
    std::array<decode_fn, 256> t{};
    using namespace layout;
    t['S'] = &fields<S>, t['R'] = &fields<R>, t['H'] = &fields<H>, t['Y'] = &fields<Y>, t['L'] = &fields<L>, t['V'] = &fields<V>,
    t['W'] = &fields<W>, t['K'] = &fields<K>, t['J'] = &fields<J>, t['h'] = &fields<h>, t['A'] = &fields<A>, t['F'] = &fields<F>,
    t['E'] = &fields<E>, t['C'] = &fields<Cx>, t['X'] = &fields<X>, t['D'] = &fields<D>, t['U'] = &fields<U>, t['P'] = &fields<P>,
    t['Q'] = &fields<Q>, t['B'] = &fields<B>, t['I'] = &fields<Ix>, t['N'] = &fields<N>, t['O'] = &fields<O>;
    return t;
}();
}  // namespace detail

inline decode_status decode_table(const unsigned char* p, std::size_t n, message& m) noexcept {
    if (n == 0) return decode_status::wrong_length;
    const std::uint8_t want = layout::lengths[p[0]];
    if (want == 0) return decode_status::unknown_type;
    if (n != want) return decode_status::wrong_length;
    detail::functions[p[0]](p, m);
    return decode_status::ok;
}

// C: shuffles. For each type, from its table: which input byte (or constant)
// lands in each of the struct's 64 bytes, then, per output lane and input
// chunk that feed it, one pshufb mask.
namespace detail {

struct shuffle_plan {
    std::uint8_t chunks = 0;  // 16-byte input chunks the message spans
    std::uint8_t steps = 0;
    struct step {
        std::uint8_t lane, chunk;
        std::array<std::uint8_t, 16> mask;
    } step[16]{};
    std::array<std::array<std::uint8_t, 16>, 4> constant{};
};

constexpr shuffle_plan plan_of(const layout::field* t, std::size_t n, std::size_t length) {
    shuffle_plan pl;
    std::array<int, 64> src{};  // input byte + 1; 0: constant (zero unless filled)
    for (std::size_t i = 0; i < n; ++i) {
        const layout::field& f = t[i];
        for (int b = 0; b < f.len; ++b) {
            if (f.k == layout::fill) {
                pl.constant[(f.out + b) / 16][(f.out + b) % 16] = f.value;
            } else if (f.k == layout::copy) {
                src[f.out + b] = f.in + b + 1;
            } else {  // big-endian on the wire, little-endian in the struct: byte b of the value is wire byte in+len-1-b
                src[f.out + b] = f.in + f.len - 1 - b + 1;
            }
        }
    }
    pl.chunks = static_cast<std::uint8_t>((length + 15) / 16);
    for (int lane = 0; lane < 4; ++lane)
        for (int chunk = 0; chunk < pl.chunks; ++chunk) {
            std::array<std::uint8_t, 16> mask{};
            bool used = false;
            for (int b = 0; b < 16; ++b) {
                const int s = src[lane * 16 + b] - 1;
                if (s >= chunk * 16 && s < chunk * 16 + 16) {
                    mask[b] = static_cast<std::uint8_t>(s - chunk * 16);
                    used = true;
                } else {
                    mask[b] = 0x80;  // zero
                }
            }
            if (used) pl.step[pl.steps++] = {static_cast<std::uint8_t>(lane), static_cast<std::uint8_t>(chunk), mask};
        }
    return pl;
}

inline constexpr auto plans = [] {
    std::array<shuffle_plan, 256> t{};
    layout::for_each_type([&](char c, const auto& table) {
        t[static_cast<unsigned char>(c)] = plan_of(table, std::size(table), message_length(c));
    });
    return t;
}();

}  // namespace detail

#if defined(__SSSE3__)
// Padded: up to 15 bytes past the message's end are read (D2's contract: 32).
inline decode_status decode_simd(const unsigned char* p, std::size_t n, message& m) noexcept {
    if (n == 0) return decode_status::wrong_length;
    const std::uint8_t want = layout::lengths[p[0]];
    if (want == 0) return decode_status::unknown_type;
    if (n != want) return decode_status::wrong_length;
    const detail::shuffle_plan& pl = detail::plans[p[0]];
    __m128i in[4];
    for (int c = 0; c < pl.chunks; ++c) in[c] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p + 16 * c));
    __m128i out[4];
    for (int l = 0; l < 4; ++l) out[l] = _mm_loadu_si128(reinterpret_cast<const __m128i*>(pl.constant[l].data()));
    for (int s = 0; s < pl.steps; ++s) {
        const auto& st = pl.step[s];
        out[st.lane] = _mm_or_si128(out[st.lane], _mm_shuffle_epi8(in[st.chunk], _mm_loadu_si128(reinterpret_cast<const __m128i*>(st.mask.data()))));
    }
    std::memcpy(&m, out, sizeof(message));
    return decode_status::ok;
}
#else
inline decode_status decode_simd(const unsigned char* p, std::size_t n, message& m) noexcept { return decode_switch(p, n, m); }
#endif

// Unpadded: the message copied first into a buffer it can be read past in.
inline decode_status decode_simd_unpadded(const unsigned char* p, std::size_t n, message& m) noexcept {
    if (n == 0 || n > 64) return n == 0 ? decode_status::wrong_length : (layout::lengths[p[0]] ? decode_status::wrong_length : decode_status::unknown_type);
    alignas(16) unsigned char buf[64 + 16] = {};
    std::memcpy(buf, p, n);
    return decode_simd(buf, n, m);
}

static_assert(sizeof(message) == 64, "decode_simd writes the struct as four 16-byte lanes");

// The default (note 004, D1, chosen from the measured figures): B.
inline decode_status decode(const unsigned char* p, std::size_t n, message& m) noexcept { return decode_table(p, n, m); }

}  // namespace itchfh
