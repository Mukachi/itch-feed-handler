// SoupBinTCP 3.00 packets and GLIMPSE 5.0's End of Snapshot (note 006). A
// SoupBinTCP packet is a 2-byte big-endian length (the bytes after it) and a
// type: from the server, Login Accepted `A` (session, 10; next sequence
// number, 20 ASCII digits, left-padded with spaces), Login Rejected `J`,
// Sequenced Data `S` (one message), Server Heartbeat `H`, End of Session `Z`;
// from the client, Login Request `L` (username 6, password 10, requested
// session 10, requested sequence number 20 ASCII), and Debug `+` either way,
// to be ignored. GLIMPSE's End of Snapshot (`G`, 21 bytes) carries the
// TotalView-ITCH sequence number to resume from, also as 20 ASCII digits.
// Building appends to a caller's buffer; reading is a stream parser: bytes in,
// in whatever pieces, whole packets out.
// Design: docs/design/006-snapshots-replay.md
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace itchfh::soup {

using session_id = std::array<char, 10>;

namespace detail {
// A number as `width` ASCII digits, left-padded with spaces.
inline void ascii(unsigned char* out, std::uint64_t v, int width) {
    for (int i = width - 1; i >= 0; --i) {
        out[i] = static_cast<unsigned char>(v || i == width - 1 ? '0' + v % 10 : ' ');
        v /= 10;
    }
}
// A number from ASCII digits, spaces allowed before them; false if anything else.
inline bool ascii(const unsigned char* p, int width, std::uint64_t& v) {
    v = 0;
    int i = 0;
    while (i < width && p[i] == ' ') ++i;
    if (i == width) return false;
    for (; i < width; ++i) {
        if (p[i] < '0' || p[i] > '9') return false;
        v = v * 10 + (p[i] - '0');
    }
    return true;
}
inline void packet(std::vector<unsigned char>& out, char type, const unsigned char* body, std::size_t n) {
    const std::size_t len = n + 1;
    out.push_back(static_cast<unsigned char>(len >> 8));
    out.push_back(static_cast<unsigned char>(len));
    out.push_back(static_cast<unsigned char>(type));
    out.insert(out.end(), body, body + n);
}
}  // namespace detail

inline void login_accepted(std::vector<unsigned char>& out, const session_id& s, std::uint64_t next) {
    unsigned char b[30];
    std::memcpy(b, s.data(), 10);
    detail::ascii(b + 10, next, 20);
    detail::packet(out, 'A', b, sizeof(b));
}
inline void login_rejected(std::vector<unsigned char>& out, char reason) {
    const unsigned char b = static_cast<unsigned char>(reason);
    detail::packet(out, 'J', &b, 1);
}
inline void sequenced(std::vector<unsigned char>& out, const unsigned char* message, std::size_t n) { detail::packet(out, 'S', message, n); }
inline void heartbeat(std::vector<unsigned char>& out) { detail::packet(out, 'H', nullptr, 0); }
inline void end_of_session(std::vector<unsigned char>& out) { detail::packet(out, 'Z', nullptr, 0); }
inline void debug(std::vector<unsigned char>& out, const std::string& text) {
    detail::packet(out, '+', reinterpret_cast<const unsigned char*>(text.data()), text.size());
}
inline void login_request(std::vector<unsigned char>& out, const std::string& user, const std::string& password, const session_id& s,
                          std::uint64_t sequence) {
    unsigned char b[46];
    std::memset(b, ' ', sizeof(b));
    std::memcpy(b, user.data(), std::min<std::size_t>(user.size(), 6));
    std::memcpy(b + 6, password.data(), std::min<std::size_t>(password.size(), 10));
    std::memcpy(b + 16, s.data(), 10);
    detail::ascii(b + 26, sequence, 20);
    detail::packet(out, 'L', b, sizeof(b));
}

// GLIMPSE's End of Snapshot, as a message (21 bytes into `out`).
inline constexpr std::size_t end_of_snapshot_size = 21;
inline void end_of_snapshot(unsigned char* out, std::uint64_t next) {
    out[0] = 'G';
    detail::ascii(out + 1, next, 20);
}
inline bool read_end_of_snapshot(const unsigned char* p, std::size_t n, std::uint64_t& next) {
    return n == end_of_snapshot_size && p[0] == 'G' && detail::ascii(p + 1, 20, next);
}

// The client's side of a stream: bytes in, in any pieces; for each whole
// packet, f(type, body, length). Debug packets are skipped here. Packets are
// read where they lie; only a packet split across pieces is copied, into a
// buffer of the largest packet's size (note 011: nothing allocated).
class reader {
public:
    template <class F>
    void feed(const unsigned char* p, std::size_t n, F&& f) {
        if (held_ > 0) {  // a packet begun in an earlier piece: completed first
            if (held_ < 2) {
                const std::size_t k = std::min<std::size_t>(2 - held_, n);
                std::memcpy(tail_ + held_, p, k);
                held_ += k, p += k, n -= k;
                if (held_ < 2) return;
            }
            const std::size_t len = std::size_t{tail_[0]} << 8 | tail_[1];
            if (len == 0) {
                ++malformed_;
                held_ = 0;
            } else {
                const std::size_t k = std::min(2 + len - held_, n);
                std::memcpy(tail_ + held_, p, k);
                held_ += k, p += k, n -= k;
                if (held_ < 2 + len) return;
                one(tail_, len, f);
                held_ = 0;
            }
        }
        while (n >= 2) {
            const std::size_t len = std::size_t{p[0]} << 8 | p[1];
            if (len == 0) {
                ++malformed_;
                p += 2, n -= 2;
                continue;
            }
            if (n < 2 + len) break;
            one(p, len, f);
            p += 2 + len, n -= 2 + len;
        }
        std::memcpy(tail_, p, n);
        held_ = n;
    }
    std::size_t malformed() const { return malformed_; }
    // A new stream: whatever was held is dropped.
    void reset() {
        held_ = 0;
        malformed_ = 0;
    }

private:
    template <class F>
    static void one(const unsigned char* at, std::size_t len, F& f) {
        const char type = static_cast<char>(at[2]);
        if (type != '+') f(type, at + 3, len - 1);
    }

    unsigned char tail_[2 + 65535];
    std::size_t held_ = 0, malformed_ = 0;
};

}  // namespace itchfh::soup
