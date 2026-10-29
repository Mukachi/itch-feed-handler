// Snapshots over a socket (note 011): note 006's GLIMPSE 5.0 server and
// client, which are sans-I/O, on TCP.
//   glimpse_service     the server's side (D2): on a thread of its own, it
//                       follows a session store up to the next message the
//                       sender will send, and answers each SoupBinTCP login
//                       with a spin of the state there: caught up to that
//                       point as it stands at the login, so the End of
//                       Snapshot's number is right by construction (note 006,
//                       D2). Blocking; for tests, faults on demand (D7).
//                       reset() starts it again for a new session.
//   snapshot_connection the client's side, for the program's one thread (D4):
//                       a non-blocking connect, the login sent once it is
//                       connected, and reads of at most a slice.
// Design: docs/design/011-snapshot-recovery.md
#pragma once

#include <itchfh/decoder.hpp>
#include <itchfh/session_store.hpp>
#include <itchfh/snapshot.hpp>
#include <itchfh/soup.hpp>
#include <itchfh/udp.hpp>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace itchfh {

class glimpse_service {
public:
    // What a test asks the next logins to meet (D7).
    enum class fault : int { none, reject, cut, corrupt };

    glimpse_service() = default;
    glimpse_service(const glimpse_service&) = delete;
    glimpse_service& operator=(const glimpse_service&) = delete;
    ~glimpse_service() {
        if (fd_ >= 0) ::close(fd_);
    }

    bool open(const std::string& ip, std::uint16_t port, std::string& why) {
        sockaddr_in a;
        if (!udp::address(ip, port, a)) return fail(why, "snapshot service: bad address " + ip);
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        const int one = 1;
        if (fd_ < 0 || ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
            ::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || ::listen(fd_, 4) != 0)
            return fail(why, "snapshot service: cannot listen on " + ip + ":" + std::to_string(port) + ": " + std::strerror(errno));
        return true;
    }
    // A new session (the sender's next run): the state followed so far dropped.
    void reset(const soup::session_id& s) {
        server_ = std::make_unique<snapshot_server>();
        followed_.store(1, std::memory_order_release);
        session_ = s;
    }
    // A new session over the same messages, from a point at or beyond what is followed: the state kept.
    void rename(const soup::session_id& s) { session_ = s; }
    // The next message to follow (any thread).
    std::uint64_t followed() const { return followed_.load(std::memory_order_acquire); }
    // Faults for the logins that follow, until changed; `cut` sends that many bytes of the spin and closes.
    void set_fault(fault f, std::size_t cut_after = 0) {
        cut_after_.store(cut_after, std::memory_order_relaxed);
        fault_.store(static_cast<int>(f), std::memory_order_release);
    }

    // One step on the serving thread: the store followed up to `sent` (at
    // most `most` messages), then one waiting login served, if there is one.
    void step(const session_store& st, const std::atomic<std::uint64_t>& sent, std::uint64_t most = 100'000) {
        follow(st, sent.load(std::memory_order_acquire), most);
        const int c = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
        if (c < 0) return;
        serve(c, st, sent);
        ::close(c);
    }

    const snapshot_server& server() const { return *server_; }
    std::uint64_t served() const { return served_.load(std::memory_order_acquire); }
    std::uint64_t refused() const { return refused_.load(std::memory_order_acquire); }

private:
    static bool fail(std::string& why, std::string w) {
        why = std::move(w);
        return false;
    }
    void follow(const session_store& st, std::uint64_t upto, std::uint64_t most) {
        for (std::uint64_t n = 0; server_->next() < upto && server_->next() <= st.count() && n < most; ++n) {
            std::size_t len;
            const unsigned char* p = st.message(server_->next(), len);
            message m;
            if (decode(p, len, m) == decode_status::ok) server_->on(m);
            else server_->skip();
        }
        followed_.store(server_->next(), std::memory_order_release);
    }
    void serve(int c, const session_store& st, const std::atomic<std::uint64_t>& sent) {
        const timeval t{2, 0};
        ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof(t));
        const timeval ts{30, 0};
        ::setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &ts, sizeof(ts));
        // The Login Request: 2 bytes of length, then `L` and 46 bytes.
        unsigned char req[49];
        std::size_t got = 0;
        while (got < sizeof(req)) {
            const ssize_t n = ::recv(c, req + got, sizeof(req) - got, 0);
            if (n <= 0) return;
            got += static_cast<std::size_t>(n);
        }
        if (req[2] != 'L') return;
        const auto f = static_cast<fault>(fault_.load(std::memory_order_acquire));
        std::vector<unsigned char> out;
        if (f == fault::reject) {
            soup::login_rejected(out, 'A');
            send_all(c, out.data(), out.size());
            refused_.fetch_add(1, std::memory_order_release);
            return;
        }
        // Caught up to the sender as it stands now, whatever that takes.
        const std::uint64_t upto = sent.load(std::memory_order_acquire);
        while (server_->next() < upto && server_->next() <= st.count()) follow(st, upto, ~0ull);
        out = server_->spin(session_, 0);
        std::size_t n = out.size();
        if (f == fault::corrupt) {  // a Sequenced Data packet past the middle gets a message type that does not exist
            for (std::size_t at = 0; at + 3 < out.size();) {
                const std::size_t len = std::size_t{out[at]} << 8 | out[at + 1];
                if (at >= out.size() / 2 && out[at + 2] == 'S' && len > 1) {
                    out[at + 3] = '?';
                    break;
                }
                at += 2 + len;
            }
        }
        if (f == fault::cut) n = cut_after_.load(std::memory_order_relaxed) < n ? cut_after_.load(std::memory_order_relaxed) : n;
        send_all(c, out.data(), n);
        served_.fetch_add(1, std::memory_order_release);
    }
    static void send_all(int c, const unsigned char* p, std::size_t n) {
        while (n > 0) {
            const ssize_t k = ::send(c, p, n, MSG_NOSIGNAL);
            if (k <= 0) return;
            p += k;
            n -= static_cast<std::size_t>(k);
        }
    }

    int fd_ = -1;
    std::unique_ptr<snapshot_server> server_ = std::make_unique<snapshot_server>();
    soup::session_id session_{};
    std::atomic<int> fault_{0};
    std::atomic<std::size_t> cut_after_{0};
    std::atomic<std::uint64_t> served_{0}, refused_{0}, followed_{1};
};

class snapshot_connection {
public:
    enum class state { closed, connecting, open, failed };

    // The Login Request is built once, here: a recovery allocates nothing (S6).
    // Our server ignores the session asked for; it is left blank.
    snapshot_connection() {
        soup::session_id blank;
        blank.fill(' ');
        soup::login_request(login_, "itchfh", "", blank, 1);
    }
    snapshot_connection(const snapshot_connection&) = delete;
    snapshot_connection& operator=(const snapshot_connection&) = delete;
    ~snapshot_connection() { close(); }

    // Starts connecting; false (and failed) if it cannot even begin.
    bool start(const std::string& ip, std::uint16_t port) {
        close();
        sockaddr_in a;
        if (!udp::address(ip, port, a)) return failed();
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return failed();
        const int one = 1;
        ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 && errno != EINPROGRESS) return failed();
        st_ = state::connecting;
        return true;
    }
    // A connecting socket moved on: once connected, the Login Request goes.
    state progress() {
        if (st_ != state::connecting) return st_;
        pollfd p{fd_, POLLOUT, 0};
        if (::poll(&p, 1, 0) <= 0) return st_;
        int err = 0;
        socklen_t len = sizeof(err);
        if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) return failed(), st_;
        if (::send(fd_, login_.data(), login_.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(login_.size())) return failed(), st_;
        st_ = state::open;
        return st_;
    }
    // At most `most` bytes waiting: their number; 0 if none yet; -1 if the
    // server has closed the connection or it failed.
    long read(unsigned char* buf, std::size_t most) {
        if (st_ != state::open) return st_ == state::connecting ? 0 : -1;
        const ssize_t n = ::recv(fd_, buf, most, MSG_DONTWAIT);
        if (n > 0) return static_cast<long>(n);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        failed();
        return -1;
    }
    void close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        st_ = state::closed;
    }
    state now() const { return st_; }

private:
    bool failed() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        st_ = state::failed;
        return false;
    }

    int fd_ = -1;
    state st_ = state::closed;
    std::vector<unsigned char> login_;
};

}  // namespace itchfh
