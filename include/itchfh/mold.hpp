// MoldUDP64 packets (note 005), as the specification V1.00 lays them out: a
// downstream packet is a 20-byte header (session, 10 bytes; the sequence
// number of its first message, 8; a message count, 2: 0 a heartbeat, 0xFFFF
// the end of the session) and then each message after a 2-byte length; a
// request packet is a session, a first sequence number and a count. All
// numbers big-endian. Reading checks that the blocks fit the packet; building
// writes into a caller's buffer.
// Design: docs/design/005-sequencing-recovery.md
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace itchfh::mold {

using session_id = std::array<char, 10>;

inline constexpr std::size_t header_size = 20;
inline constexpr std::size_t request_size = 20;
inline constexpr std::uint16_t heartbeat = 0;
inline constexpr std::uint16_t end_of_session = 0xFFFF;
// D7: the downstream payload our sender and re-request server fill to, at most.
inline constexpr std::size_t payload = 1400;

namespace detail {
inline std::uint64_t be(const unsigned char* p, int n) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = v << 8 | p[i];
    return v;
}
inline void put(unsigned char* p, std::uint64_t v, int n) noexcept {
    for (int i = n - 1; i >= 0; --i, v >>= 8) p[i] = static_cast<unsigned char>(v);
}
}  // namespace detail

struct header {
    session_id session;
    std::uint64_t sequence;  // of the first message; a heartbeat's and the end's: the next expected
    std::uint16_t count;
};

inline header read_header(const unsigned char* p) noexcept {
    header h;
    std::memcpy(h.session.data(), p, 10);
    h.sequence = detail::be(p + 10, 8);
    h.count = static_cast<std::uint16_t>(detail::be(p + 18, 2));
    return h;
}

// A downstream packet's messages, checked: false if the packet is shorter than
// its header, or its blocks do not fit it exactly. Calls f(sequence, bytes,
// length) for each message, numbered from the header's.
template <class F>
bool each_message(const unsigned char* p, std::size_t n, F&& f) {
    if (n < header_size) return false;
    const header h = read_header(p);
    if (h.count == heartbeat || h.count == end_of_session) return n == header_size;
    std::size_t at = header_size;
    for (std::uint16_t i = 0; i < h.count; ++i) {
        if (n - at < 2) return false;
        const std::size_t len = static_cast<std::size_t>(detail::be(p + at, 2));
        if (n - at - 2 < len) return false;
        f(h.sequence + i, p + at + 2, len);
        at += 2 + len;
    }
    return at == n;
}

// Builds a downstream packet in `out` (at least header_size + payload bytes).
class packet_builder {
public:
    packet_builder(unsigned char* out, const session_id& s, std::uint64_t first) : out_(out) {
        std::memcpy(out_, s.data(), 10);
        detail::put(out_ + 10, first, 8);
        detail::put(out_ + 18, 0, 2);
    }
    // False if the message does not fit within the payload bound.
    bool add(const unsigned char* m, std::size_t len) {
        if (at_ - header_size + 2 + len > payload) return false;
        detail::put(out_ + at_, len, 2);
        std::memcpy(out_ + at_ + 2, m, len);
        at_ += 2 + len;
        detail::put(out_ + 18, ++count_, 2);
        return true;
    }
    std::uint16_t count() const { return count_; }
    std::size_t size() const { return at_; }

private:
    unsigned char* out_;
    std::size_t at_ = header_size;
    std::uint16_t count_ = 0;
};

// A heartbeat or end-of-session packet (20 bytes) in `out`.
inline void build_marker(unsigned char* out, const session_id& s, std::uint64_t next, std::uint16_t count) {
    std::memcpy(out, s.data(), 10);
    detail::put(out + 10, next, 8);
    detail::put(out + 18, count, 2);
}

struct request {
    session_id session;
    std::uint64_t sequence;
    std::uint16_t count;
};

inline void build_request(unsigned char* out, const request& r) {
    std::memcpy(out, r.session.data(), 10);
    detail::put(out + 10, r.sequence, 8);
    detail::put(out + 18, r.count, 2);
}
inline bool read_request(const unsigned char* p, std::size_t n, request& r) {
    if (n != request_size) return false;
    std::memcpy(r.session.data(), p, 10);
    r.sequence = detail::be(p + 10, 8);
    r.count = static_cast<std::uint16_t>(detail::be(p + 18, 2));
    return true;
}

}  // namespace itchfh::mold
