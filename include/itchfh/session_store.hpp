// A MoldUDP64 session's messages, kept in order (note 005, D6, D7): every
// message's bytes one after another, and where each begins, by sequence
// number (from 1). It fills downstream packets for the sender (in feed order,
// as many messages as fit a payload, D7) and answers re-requests with the same
// packets from any sequence number (the re-request server, D6).
// Design: docs/design/005-sequencing-recovery.md
#pragma once

#include <itchfh/memory.hpp>
#include <itchfh/mold.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace itchfh {

class session_store {
public:
    // Room for `messages` messages of `bytes` bytes in all.
    session_store(const mold::session_id& s, std::size_t messages, std::size_t bytes, bool prefault)
        : session_(s), capacity_(messages), bytes_cap_(bytes), offsets_(zeroed<std::uint64_t>(messages + 1, prefault)),
          bytes_(zeroed<unsigned char>(bytes, prefault)) {}

    // False if full. The message's sequence number is count() after.
    bool append(const unsigned char* m, std::size_t len) {
        if (count_ == capacity_ || offsets_[count_] + len > bytes_cap_) return false;
        std::memcpy(bytes_.get() + offsets_[count_], m, len);
        offsets_[count_ + 1] = offsets_[count_] + len;
        ++count_;
        return true;
    }
    std::uint64_t count() const { return count_; }
    const mold::session_id& session() const { return session_; }
    // Another session over the same messages (note 007's sender: a fresh one each run).
    void set_session(const mold::session_id& s) { session_ = s; }

    // A downstream packet in `out` (header_size + payload bytes) of the
    // messages from `first`, at most `most`, as many as fit; its size, or 0 if
    // `first` is past the last message.
    std::size_t fill(std::uint64_t first, std::uint64_t most, unsigned char* out) const {
        if (first == 0 || first > count_) return 0;
        mold::packet_builder b(out, session_, first);
        for (std::uint64_t s = first; s <= count_ && s - first < most; ++s) {
            const std::uint64_t at = offsets_[s - 1];
            if (!b.add(bytes_.get() + at, offsets_[s] - at)) break;
        }
        return b.count() ? b.size() : 0;
    }
    // The re-request server's answer to a request packet (one downstream
    // packet, the requested messages that fit), or 0 bytes if it is of
    // another session or asks for nothing stored.
    std::size_t answer(const unsigned char* p, std::size_t n, unsigned char* out) const {
        mold::request r;
        if (!mold::read_request(p, n, r) || r.session != session_ || r.count == 0) return 0;
        return fill(r.sequence, r.count, out);
    }
    // A message's bytes, for tests.
    const unsigned char* message(std::uint64_t s, std::size_t& len) const {
        len = offsets_[s] - offsets_[s - 1];
        return bytes_.get() + offsets_[s - 1];
    }

private:
    mold::session_id session_;
    std::size_t capacity_, bytes_cap_;
    table<std::uint64_t> offsets_;
    table<unsigned char> bytes_;
    std::uint64_t count_ = 0;
};

}  // namespace itchfh
